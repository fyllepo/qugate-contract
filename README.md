# QuGate — A Payment Automation Primitive for Qubic

QuGate is a **network primitive** — shared, permissionless payment routing infrastructure for the Qubic network. One shared contract that the entire ecosystem can use, instead of every project building its own payment logic. Creation, dust, and chain-hop fees are burned. Idle gates maintain a reserve-backed inactivity budget, with upkeep split between burn and dividends.

**Status**: Testnet verified. 128/128 integration scenarios passing. Preparing for mainnet proposal.
**Author**: fyllepo (Discord: phileepphilop)
**Repository**: [github.com/fyllepo/qugate-contract](https://github.com/fyllepo/qugate-contract)

---

## Table of Contents

1. [Overview](#overview)
2. [Gate Modes](#gate-modes)
3. [Chain Gates](#chain-gates)
4. [Gate-as-Recipient](#gate-as-recipient-internal-routing)
5. [Gate ID Format](#gate-id-format)
5. [Contract Interface](#contract-interface)
6. [Wire Format](#wire-format)
7. [State Architecture](#state-architecture)
8. [Anti-Spam Mechanisms](#anti-spam-mechanisms)
9. [Fee Economics](#fee-economics)
10. [Shareholder Governance](#shareholder-governance)
11. [Design Decisions](#design-decisions)
12. [Security Model](#security-model)
13. [Edge Cases and Safety](#edge-cases-and-safety)
14. [Status Codes](#status-codes)
15. [Log Types](#log-types)
16. [Building and Testing](#building-and-testing)
17. [Known Limitations](#known-limitations)
18. [File Listing](#file-listing)

---

## Overview

QuGate introduces **gates** — configurable routing nodes that automatically forward QU payments according to predefined rules. Each gate supports one of eight active modes (SPLIT, ROUND_ROBIN, THRESHOLD, RANDOM, CONDITIONAL, HEARTBEAT, MULTISIG, TIME_LOCK — slot 5 is reserved). Gates are composable: the output of one gate can be forwarded into another, enabling multi-stage payment pipelines without writing custom contracts.

Gate-to-gate forwarding can happen in two ways:
1. **Manual forwarding**: An external actor (client app, bot) calls sendToGate on the next gate.
2. **Automatic chain forwarding**: Gates configured with chainNextGateId auto-forward after each payout — no external transaction required (max 3 hops).

### Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `QUGATE_MAX_GATES` | 2048 x X_MULTIPLIER | Max concurrent gates (scales with network) |
| `QUGATE_MAX_RECIPIENTS` | 8 | Max recipients per gate |
| `QUGATE_MAX_RATIO` | 10,000 | Max ratio value per recipient |
| `QUGATE_DEFAULT_CREATION_FEE` | 100,000 QU | Initial base creation fee |
| `QUGATE_DEFAULT_MAINTENANCE_FEE` | 25,000 QU | Inactivity maintenance fee charged to idle gates |
| `QUGATE_DEFAULT_MAINTENANCE_INTERVAL_EPOCHS` | 4 | Idle window before reserve-backed inactivity billing |
| `QUGATE_DEFAULT_MAINTENANCE_GRACE_EPOCHS` | 4 | Grace epochs before delinquent gates expire |
| `QUGATE_DEFAULT_MIN_SEND` | 1,000 QU | Initial minimum send amount |
| `QUGATE_FEE_ESCALATION_STEP` | 1,024 | Active gates per fee multiplier step |
| `QUGATE_DEFAULT_EXPIRY_EPOCHS` | 50 | Epochs of inactivity before auto-close |

---

## Gate Modes

| Mode | Constant | Value | Description |
|------|----------|-------|-------------|
| SPLIT | `QUGATE_MODE_SPLIT` | 0 | Proportional distribution to N recipients |
| ROUND_ROBIN | `QUGATE_MODE_ROUND_ROBIN` | 1 | Rotating distribution, one recipient per payment |
| THRESHOLD | `QUGATE_MODE_THRESHOLD` | 2 | Accumulate until threshold reached, then forward |
| RANDOM | `QUGATE_MODE_RANDOM` | 3 | Probabilistic selection per payment |
| CONDITIONAL | `QUGATE_MODE_CONDITIONAL` | 4 | Sender-restricted forwarding (whitelist) |
| _(reserved)_ | `QUGATE_MODE_ORACLE` | 5 | **Reserved** — `createGate` returns `QUGATE_UNSUPPORTED_MODE` |
| HEARTBEAT | `QUGATE_MODE_HEARTBEAT` | 6 | Dead-man's switch: distribute if owner goes silent |
| MULTISIG | `QUGATE_MODE_MULTISIG` | 7 | M-of-N guardian approval before funds release |
| TIME_LOCK | `QUGATE_MODE_TIME_LOCK` | 8 | Holds funds until a target epoch, then releases to recipient |

### QUGATE_MODE_SPLIT (0) — Proportional Distribution

Distributes incoming payments to N recipients according to fixed ratios.

Each recipient's share is calculated as `(amount / totalRatio) * ratio`, with the last recipient receiving the remainder to avoid rounding loss. The overflow-safe formula used:

```
share = (amount / totalRatio) * ratio + ((amount % totalRatio) * ratio) / totalRatio
```

Ratios are relative, not percentages. Ratios of [3, 7] and [30, 70] produce identical splits.

**Validation**: Each ratio must be <= 10,000. Total ratio must be > 0. Recipient count 1-8.

### QUGATE_MODE_ROUND_ROBIN (1) — Rotating Distribution

Forwards each payment to the next recipient in sequence. An internal `roundRobinIndex` tracks position and wraps using `mod(index + 1, recipientCount)`.

Payment 1 goes to recipient 0, payment 2 to recipient 1, etc. After reaching the last recipient, it cycles back to recipient 0.

### QUGATE_MODE_THRESHOLD (2) — Accumulate and Forward

Holds incoming payments in `currentBalance` until the configured threshold is reached. When `currentBalance >= threshold`, the entire balance is transferred to recipient 0 and the balance resets to zero. The gate then begins accumulating again.

**Validation**: Threshold must be > 0.

If the gate is closed or expires while holding a balance, the held funds are refunded to the gate owner.

### QUGATE_MODE_RANDOM (3) — Probabilistic Distribution

Selects one recipient per payment using tick-based entropy:

```
recipientIdx = mod(totalReceived + tick(), recipientCount)
```

Where `totalReceived` is the gate's cumulative received amount (already incremented by the current payment) and `tick()` is the current Qubic tick number. The tick number is not controllable by the sender, providing sufficient unpredictability for payment routing, though this is not cryptographic randomness.

> **Warning:** RANDOM mode uses on-chain tick + totalReceived as entropy. Both values are publicly
> observable before a transaction is submitted, making recipient selection predictable to
> a determined observer. Suitable for non-adversarial fair distribution. Not suitable for
> use cases requiring cryptographic randomness.

### QUGATE_MODE_CONDITIONAL (4) — Sender-Restricted Forwarding

Only forwards payments from addresses in the gate's `allowedSenders` list. Payments from unauthorized senders are bounced back (transferred back to the sender) with status `QUGATE_CONDITIONAL_REJECTED`.

When the sender is authorized, the full amount is forwarded to recipient 0.

**Validation**: `allowedSenderCount` must be <= 8.

### Mode Slot 5 — Reserved

Mode slot 5 is reserved for future use. `createGate` with `mode=5` returns `QUGATE_INVALID_MODE`.

---

## HEARTBEAT Gate (Mode 6)

A heartbeat gate holds funds and distributes them to beneficiaries if the owner stops sending periodic `heartbeat()` signals. Ideal for dead man's switch, inheritance, or automated recurring distributions.

### Setup
1. Create a gate with `mode=6`
2. Call `configureHeartbeat()` with threshold, payout percent, and beneficiaries
3. Call `heartbeat()` periodically to keep the gate dormant
4. If `thresholdEpochs` pass without a heartbeat, the gate triggers

### Parameters
| Field | Description |
|-------|-------------|
| thresholdEpochs | Epochs of inactivity before trigger (min 1, ~1 epoch/week) |
| payoutPercentPerEpoch | % of balance distributed each epoch after trigger (1-100) |
| minimumBalance | Gate auto-closes when balance drops below this amount |
| beneficiaries | Up to 8 addresses with sharePercent summing to 100 |

### Procedures
- `configureHeartbeat(gateId, thresholdEpochs, payoutPercent, minimumBalance, beneficiaries[])` — owner only
- `heartbeat(gateId)` — owner only, resets epoch counter. Rejected after trigger.
- `getHeartbeat(gateId)` — read-only query

### Example use cases
- **Inheritance**: distribute crypto estate to family after inactivity
- **Recurring payments**: automated salary/allowance that continues until cancelled
- **Deadlines**: release funds to counterparty if action not taken within N epochs

### Worked Example: Crypto inheritance

**Scenario:** You want your QU to go to family if you stop checking in for 8 weeks.

**Step 1: Create the gate**
```
createGate(mode=6, recipients=[], ratios=[])
→ Returns gateId (e.g. 4294967296)
```

**Step 2: Fund it**
```
Send QU to the gate address: YAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAMSME
```

**Step 3: Configure**
```
configureHeartbeat(
  gateId: 4294967296,
  thresholdEpochs: 8,          // trigger after 8 missed epochs (~8 weeks)
  payoutPercentPerEpoch: 25,   // pay 25% of remaining balance each epoch
  minimumBalance: 1000000,     // stop when < 1M QU remains
  beneficiaries: [
    { address: WALLET_A, sharePercent: 60 },
    { address: WALLET_B, sharePercent: 40 }
  ]
)
```

**Step 4: Keep alive (automate this)**
```
heartbeat(gateId: 4294967296)
→ Call monthly. Miss 8 epochs → gate triggers automatically.
```

**What happens when triggered:**
- Epoch 1 post-trigger: 25% of balance → 60/40 split to WALLET_A / WALLET_B
- Epoch 2: another 25% of remaining
- Continues until balance < 1,000,000 QU
- Gate auto-closes

**Chained with MULTISIG (guardians can update beneficiaries):**
```
createGate(mode=7)  → MULTISIG gate (gateId: 4294967297)
configureMultisig(
  gateId: 4294967297,
  guardians: [GUARDIAN_1, GUARDIAN_2, GUARDIAN_3],
  required: 2,
  proposalExpiryEpochs: 4
)
// 2-of-3 guardians vote → can update the HEARTBEAT gate's beneficiaries
```

### Error codes
| Code | Meaning |
|------|---------|
| QUGATE_HEARTBEAT_TRIGGERED (-16) | heartbeat() called after gate already triggered |
| QUGATE_HEARTBEAT_NOT_ACTIVE (-17) | Gate not configured for heartbeat mode |
| QUGATE_HEARTBEAT_INVALID (-18) | Invalid config (shares don't sum to 100, bad percent, etc) |

---

## MULTISIG Gate (Mode 7)

A multisig gate holds funds until M-of-N designated guardians send approval transactions. When the threshold is reached, funds release to the target address. Proposals expire after a configurable number of epochs.

### Setup
1. Create a gate with `mode=7`
2. Call `configureMultisig()` with guardian addresses, required count, and expiry
3. Anyone can fund the gate by sending QU
4. Guardians vote by sending any amount to the gate address
5. When M guardians have voted, funds release to `recipients[0]`

### Parameters
| Field | Description |
|-------|-------------|
| guardians | Up to 8 wallet addresses authorised to vote |
| required | Minimum approvals needed (M of N) |
| proposalExpiryEpochs | Epochs before an incomplete proposal resets |

### Procedures
- `configureMultisig(gateId, guardians[], required, expiryEpochs)` — owner only
- `getMultisigState(gateId)` — read-only: returns approvalBitmap, count, proposalEpoch

### Voting mechanics
- Any address can send QU to fund the gate
- Only guardians' transactions count as votes
- Bitmap tracks which guardians have voted (prevents double-voting)
- Incomplete proposals reset after `proposalExpiryEpochs`
- Once threshold met: full balance transfers to target, votes reset

### Example use cases
- **Joint accounts**: 1-of-2 partners can authorise a payment
- **DAO treasury**: 3-of-5 committee members approve disbursements
- **Escrow**: buyer + arbitrator both must sign off
- **Inheritance config**: 2-of-3 guardians can update beneficiaries

### Worked Example: DAO treasury payment

**Scenario:** 3 committee members must approve any payment from the treasury.

**Step 1: Create and configure**
```
createGate(mode=7, recipients=[TREASURY_TARGET], ratios=[100])
configureMultisig(
  gateId: ...,
  guardians: [MEMBER_1, MEMBER_2, MEMBER_3],
  required: 2,                    // 2-of-3 required
  proposalExpiryEpochs: 8         // proposal lapses after 8 epochs if not completed
)
```

**Step 2: Fund it**
```
Send 500,000 QU to the gate address
```

**Step 3: Vote**
```
// Member 1 sends 1 QU to the gate → registers vote
// Member 2 sends 1 QU to the gate → 2nd vote, threshold met
// → Gate releases 500,000 QU to TREASURY_TARGET
// → Vote bitmap resets, ready for next proposal
```

**What happens with an expired proposal:**
```
// Member 1 votes at epoch 100
// No second vote by epoch 108 (proposalExpiryEpochs=8)
// → Proposal resets automatically at epoch boundary
// → Funds remain in gate, ready for a new proposal
```

### Error codes
| Code | Meaning |
|------|---------|
| QUGATE_MULTISIG_NOT_GUARDIAN (-19) | Sender is not in guardian list |
| QUGATE_MULTISIG_ALREADY_VOTED (-20) | Guardian already voted this proposal |
| QUGATE_MULTISIG_INVALID_CONFIG (-21) | Invalid config (0 guardians, required > count, etc) |
| QUGATE_MULTISIG_NO_ACTIVE_PROP (-22) | No active proposal to query |

---

## Admin Gate (adminGateId) — MULTISIG Governance for Any Gate

Any gate can be placed under MULTISIG governance by assigning an **admin gate**. When an admin gate is set, configuration changes (closeGate, updateGate, configureHeartbeat, configureMultisig, configureTimeLock, cancelTimeLock, setChain) require either the owner's signature **or** approval from the admin gate's MULTISIG quorum.

Admin gates should be chosen with the same care as any other signing authority. Guardians cannot transfer gate ownership, but they can control most meaningful configuration changes, including replacing recipients, changing chain routing, replacing the admin gate itself, or closing the gate once approval is active.

Governance-only admin multisigs are still subject to the inactivity model. To keep them alive during quiet periods, fund their `reserve` via `fundGate(gateId)`. If an admin gate expires or becomes unusable, the governed gate owner can still clear governance and recover control.

The `heartbeat()` procedure is intentionally excluded — it is a keep-alive signal that should always remain owner-only.

### How it works

1. Create a MULTISIG gate (mode 7) with guardians and a required threshold
2. Call `setAdminGate(gateId, adminGateId)` on the gate you want to govern
3. From that point, config changes on the governed gate require either:
   - The owner calling the procedure directly, **or**
   - The admin gate's MULTISIG quorum reaching approval in the current epoch
4. To remove governance, call `setAdminGate(gateId, -1)`:
   - If the current admin gate is active, clearing it requires that admin gate's approval window
   - If the current admin gate has expired, been closed, or is otherwise no longer a valid active MULTISIG gate, the owner may clear it directly

### Procedures
- `setAdminGate(gateId, adminGateId)` — owner-only if no admin gate set; requires admin gate approval if already set
- `getAdminGate(gateId)` — read-only: returns hasAdminGate, adminGateId, adminGateMode, guardianCount, required, guardians

### Worked Example: HEARTBEAT + MULTISIG admin governance

**Scenario:** You set up a HEARTBEAT inheritance gate and want 2-of-3 guardians to approve any configuration changes.

**Step 1: Create a MULTISIG admin gate**
```
createGate(mode=7, recipients=[ANY_ADDRESS], ratios=[100])
→ Returns adminGateId (e.g. 5368709121)
configureMultisig(
  gateId: 5368709121,
  guardians: [GUARDIAN_1, GUARDIAN_2, GUARDIAN_3],
  required: 2,
  proposalExpiryEpochs: 4
)
```

**Step 2: Create and configure the HEARTBEAT gate**
```
createGate(mode=6, recipients=[], ratios=[])
→ Returns gateId (e.g. 4294967296)
configureHeartbeat(
  gateId: 4294967296,
  thresholdEpochs: 8,
  payoutPercentPerEpoch: 25,
  minimumBalance: 1000000,
  beneficiaries: [
    { address: WALLET_A, sharePercent: 60 },
    { address: WALLET_B, sharePercent: 40 }
  ]
)
```

**Step 3: Assign the admin gate**
```
setAdminGate(gateId: 4294967296, adminGateId: 5368709121)
```

**Step 4: Owner can still heartbeat() normally**
```
heartbeat(gateId: 4294967296)
→ Works — heartbeat stays owner-only
```

**Step 5: Config changes now require guardian approval**
```
// Owner tries to updateGate() alone → QUGATE_UNAUTHORIZED (unless they are the owner)
// Guardians vote on the MULTISIG gate:
//   GUARDIAN_1 sends 1 QU to gate 5368709121 → vote registered
//   GUARDIAN_2 sends 1 QU to gate 5368709121 → 2-of-3 met
// Now updateGate(4294967296, ...) succeeds in this epoch
```

### Error codes
| Code | Meaning |
|------|---------|
| QUGATE_ADMIN_GATE_REQUIRED (-26) | Config change needs admin gate approval |
| QUGATE_INVALID_ADMIN_GATE (-27) | adminGateId doesn't exist or isn't MULTISIG mode |

---

## TIME_LOCK Gate (Mode 8)

A TIME_LOCK gate holds incoming QU and releases the full balance to a designated target address when a specified future epoch is reached. Nothing releases before that epoch. Supports two lock modes: **absolute** (unlock at a specific epoch) and **relative** (unlock N epochs after configuration).

### Setup
1. Create a gate with `mode=8` and `recipients[0]` = target address
2. Call `configureTimeLock()` with lock mode, timing parameters, and `cancellable`
3. Send QU to the gate — funds accumulate in `currentBalance`
4. At the start of the unlock epoch, `END_EPOCH` automatically releases the full balance to the target

### Lock Modes
| Mode | Constant | Value | Description |
|------|----------|-------|-------------|
| Absolute | `QUGATE_TIME_LOCK_ABSOLUTE_EPOCH` | 0 | Unlock at a specific epoch number (`unlockEpoch` must be > current epoch) |
| Relative | `QUGATE_TIME_LOCK_RELATIVE_EPOCHS` | 1 | Unlock `delayEpochs` epochs after configuration (`delayEpochs` must be > 0) |

In relative mode, if the gate already holds funds at configuration time, the unlock epoch is anchored immediately: `unlockEpoch = currentEpoch + delayEpochs`. If the gate has no balance yet, `unlockEpoch` is set to 0 and anchored when funds first arrive. After anchoring, behaviour is identical to absolute mode.

### Parameters
| Field | Description |
|-------|-------------|
| recipients[0] | Target address that receives funds on unlock |
| lockMode | 0 = absolute epoch, 1 = relative epochs |
| unlockEpoch | The epoch number when funds are released (absolute mode) |
| delayEpochs | Number of epochs from now until unlock (relative mode) |
| cancellable | 1 = owner can cancel and recover funds before unlock |

### Procedures
| Procedure | Description |
|-----------|-------------|
| `configureTimeLock(gateId, unlockEpoch, delayEpochs, lockMode, cancellable)` | Owner only — sets unlock parameters |
| `cancelTimeLock(gateId)` | Owner only — cancels and refunds balance (requires cancellable=1) |
| `getTimeLockState(gateId)` | Read-only — returns config, balance, current epoch, epochs remaining |

### Error codes
| Code | Meaning |
|------|---------|
| QUGATE_TIME_LOCK_ALREADY_FIRED (-23) | Gate has already unlocked and closed |
| QUGATE_TIME_LOCK_NOT_CANCELLABLE (-24) | cancelTimeLock() called but cancellable=0 |
| QUGATE_TIME_LOCK_EPOCH_PAST (-25) | unlockEpoch is in the past at configuration time |
| QUGATE_INVALID_PARAMS (-31) | Invalid lockMode or zero delayEpochs in relative mode |

### Worked Example: Lock 500K QU for 8 epochs (vesting)

**Scenario:** Alice wants to lock 500,000 QU for Bob, releasing 8 epochs from now.

**Step 1: Create the gate**
```
createGate(
  mode = 8,                        // TIME_LOCK
  recipientCount = 1,
  recipients[0] = BOB_ADDRESS,     // target
  ratios[0] = 10000
)
→ gateId: 1234
```

**Step 2: Configure the time lock (relative mode)**
```
configureTimeLock(
  gateId: 1234,
  unlockEpoch: 0,                   // ignored in relative mode
  delayEpochs: 8,                   // unlock 8 epochs from now
  lockMode: 1,                      // RELATIVE
  cancellable: 1                    // Alice can cancel if plans change
)
```

**Step 3: Fund the gate**
```
sendToGate(gateId: 1234, amount: 500000 QU)
// Funds held in gate.currentBalance
// Since gate had no balance at configure time, unlockEpoch is anchored now:
//   unlockEpoch = currentEpoch + 8 (e.g. epoch 218 if current is 210)
// getTimeLockState shows: epochsRemaining = 8
```

> **Note:** If the gate already held funds when `configureTimeLock` was called, the
> unlock epoch would have been anchored at configuration time instead of first funding.

**Step 4: Wait (or cancel)**
```
// Option A: Wait — at epoch 218, END_EPOCH automatically runs:
//   qpi.transfer(BOB_ADDRESS, 500000)
//   gate closes, QUGATE_LOG_TIME_LOCK_FIRED emitted

// Option B: Cancel before epoch 218:
//   cancelTimeLock(gateId: 1234)
//   → 500000 QU refunded to Alice, gate closes
```

---

## Chain Gates

Gates can be linked into chains (max 3 hops) for multi-stage payment pipelines. When a gate completes its payout, funds are automatically forwarded to the next gate in the chain — no external transaction required.

### Flow

1. Payment arrives at Gate A via `sendToGate`
2. Gate A processes the payment according to its mode (SPLIT, ROUND_ROBIN, etc.)
3. If Gate A has `chainNextGateId` set, the forwarded amount is routed to Gate B
4. Gate B processes the forwarded amount, then forwards to Gate C if chained
5. Each hop continues until the chain ends or `QUGATE_MAX_CHAIN_DEPTH` (3) is reached

### Hop Fees and Reserves

Each chain hop burns a `QUGATE_CHAIN_HOP_FEE` (1,000 QU). If the forwarded amount exceeds the hop fee, the fee is deducted from the amount. If the forwarded amount is too small to cover the hop fee, the gate's `reserve` pays instead. If neither can cover the fee, the funds are stranded in `currentBalance`.

Fund a gate's reserve via `fundGate(gateId)`. The reserve covers both chain hop fees and inactivity maintenance, and is refunded to the owner on gate close or expiry. Excess QU paid above the creation fee when calling `createGate` is automatically deposited into the new gate's reserve — overpay to pre-fund.

Inactivity maintenance is deducted from this reserve, not from the gate's operational `currentBalance`. This avoids punishing pass-through gates and governance-only admin multisigs that may not naturally hold funds.

### Setting Up Chains

- At creation: set `chainNextGateId` in `createGate_input`
- After creation: use `setChain` (costs one hop fee, burned)
- Clear a chain: call `setChain` with `nextGateId = -1`

**Note:** CONDITIONAL mode as a chain target requires the contract's own address in `allowedSenders`, because the invocator during chain routing is the contract itself, not the original external sender.

---

## Gate-as-Recipient (Internal Routing)

Recipients can be other gates, not just wallets. When a gate distributes funds (SPLIT, ROUND_ROBIN, RANDOM, CONDITIONAL, THRESHOLD), each share can route internally to another gate via `routeToGate()` instead of `qpi.transfer()`.

### How it works

Each gate has a parallel array `recipientGateIds[8]` alongside `recipients[8]`:
- `recipientGateIds[i] == -1` → wallet recipient (use `recipients[i]` address, existing behaviour)
- `recipientGateIds[i] >= 0` → gate recipient (route share to that versioned gate ID)

### Example: Lottery with house rake

```
Jackpot Pool (THRESHOLD 10M QU)
  → chains to → House Rake (SPLIT: 5% wallet, 95% gate)
                  → 5% → House wallet (qpi.transfer)
                  → 95% → Winner Selection (RANDOM: 6 players, via routeToGate)
```

### Validation

- Gate recipient IDs are validated at create/update time (must be active, correct generation)
- Invalid/closed gate recipients at send time silently retain funds in the contract
- Returns `QUGATE_INVALID_GATE_RECIPIENT` (-28) on invalid IDs at creation

### Deferred routing pattern

Mode processors (processSplit, etc.) cannot call `routeToGate` directly (circular struct dependency). Instead, they populate deferred output fields. The caller (`sendToGate`, `sendToGateVerified`) dispatches the deferred routing after the mode processor returns.

### Interaction with chain forwarding

Gate-as-recipient and chain forwarding are independent:
- **Chain forwarding** (`chainNextGateId`): forwards *remaining balance* after distribution
- **Gate-as-recipient** (`recipientGateIds`): routes *individual shares* during distribution

Both can be used on the same gate. A SPLIT gate can send 5% to a wallet, 95% to a gate-recipient, and chain its remaining balance to another gate.

---

## Gate ID Format

Gate IDs use a versioned encoding to prevent slot-reuse squatting attacks:

```
gateId = ((generation + 1) << 20) | slotIndex
```

- **Lower 20 bits**: slot index in the `_gates` array (supports up to 1,048,575 slots)
- **Upper bits**: generation counter + 1 (incremented each time a slot is recycled)

When a gate is closed or expires, its slot's generation counter increments. Any stored gate ID referencing the old generation becomes invalid, preventing an attacker from creating a new gate in a recycled slot and intercepting payments meant for the previous occupant.

Decoding: `slotIndex = gateId & 0xFFFFF`, `generation = (gateId >> 20) - 1`.

---

## Contract Interface

### Procedure Index

| # | Name | Type | Description |
|---|------|------|-------------|
| 1 | createGate | PUBLIC_PROCEDURE | Create a new gate (fee required) |
| 2 | sendToGate | PUBLIC_PROCEDURE | Send QU through a gate |
| 3 | closeGate | PUBLIC_PROCEDURE | Close a gate (owner only) |
| 4 | updateGate | PUBLIC_PROCEDURE | Update gate recipients/ratios/config (owner only) |
| 5 | getGate | PUBLIC_FUNCTION | Query full gate state and statistics |
| 6 | getGateCount | PUBLIC_FUNCTION | Query total/active gate counts and total burned |
| 7 | getGatesByOwner | PUBLIC_FUNCTION | List gate IDs owned by an address |
| 8 | getGateBatch | PUBLIC_FUNCTION | Batch query up to 32 gates by ID |
| 9 | getFees | PUBLIC_FUNCTION | Query current fee parameters |
| 10 | fundGate | PUBLIC_PROCEDURE | Add QU to a gate's unified reserve |
| 11 | setChain | PUBLIC_PROCEDURE | Set or clear a chain link (owner only) |
| 12 | sendToGateVerified | PUBLIC_PROCEDURE | Like sendToGate, but verifies gate owner before routing |
| 13 | configureHeartbeat | PUBLIC_PROCEDURE | Configure HEARTBEAT gate params and beneficiaries |
| 14 | heartbeat | PUBLIC_PROCEDURE | Reset epoch counter (keep-alive signal) |
| 15 | getHeartbeat | PUBLIC_FUNCTION | Query HEARTBEAT gate state |
| 16 | configureMultisig | PUBLIC_PROCEDURE | Configure MULTISIG gate guardians and threshold |
| 17 | getMultisigState | PUBLIC_FUNCTION | Query current approval state |
| 18 | configureTimeLock | PUBLIC_PROCEDURE | Configure TIME_LOCK gate unlock epoch and cancellability |
| 19 | cancelTimeLock | PUBLIC_PROCEDURE | Cancel a TIME_LOCK gate and refund held balance (owner only) |
| 20 | getTimeLockState | PUBLIC_FUNCTION | Query TIME_LOCK gate state and epochs remaining |
| 21 | setAdminGate | PUBLIC_PROCEDURE | Set or clear admin gate (MULTISIG governance) on any gate |
| 22 | getAdminGate | PUBLIC_FUNCTION | Query admin gate configuration for a gate |
| 23 | withdrawReserve | PUBLIC_PROCEDURE | Withdraw from a gate's unified reserve without closing (owner only) |
| 24 | getGatesByMode | PUBLIC_FUNCTION | Query up to 16 active gates matching a given mode |
| 25 | getGateBySlot | PUBLIC_FUNCTION | Query a gate by raw slot index (returns versioned gate ID + full state) |
| 26 | getLatestExecution | PUBLIC_FUNCTION | Query the latest execution metadata for a gate (outcome, recipient, tick) |

**Totals**: 14 procedures + 12 functions = 26 registered entry points.

### Procedures (State-Changing)

All procedures return a `status` field (sint64). Status 0 = success; negative values indicate errors (see [Status Codes](#status-codes)). Failed procedures refund any attached QU to the caller.

#### createGate (Input Type 1)

Creates a new gate. Requires payment of the current escalated creation fee.

**Input**:

| Field | Type | Size | Description |
|-------|------|------|-------------|
| mode | uint8 | 1 | Gate mode (0-8) |
| recipientCount | uint8 | 1 | Number of recipients (1-8) |
| recipients | Array\<id, 8\> | 256 | Recipient public keys (32 bytes each) |
| ratios | Array\<uint64, 8\> | 64 | Ratio per recipient (SPLIT mode) |
| threshold | uint64 | 8 | Threshold amount (THRESHOLD mode) |
| allowedSenders | Array\<id, 8\> | 256 | Allowed sender pubkeys (CONDITIONAL mode) |
| allowedSenderCount | uint8 | 1 | Number of allowed senders (0-8) |
| chainNextGateId | sint64 | 8 | Chain target gate ID (-1 = no chain) |
| recipientGateIds | Array\<sint64, 8\> | 64 | Gate-as-recipient IDs (-1 = wallet) |

**Output**:

| Field | Type | Description |
|-------|------|-------------|
| status | sint64 | 0 on success, negative on error |
| gateId | uint64 | Assigned gate ID (1-indexed) |
| feePaid | uint64 | Actual fee burned (after escalation) |

Wire size: see [createGate_input Layout](#creategate_input-layout) for exact byte offsets. 24 bytes (output).

**Behaviour**: Validates all parameters. Burns the escalated creation fee. Any excess QU above the fee is automatically deposited into the new gate's `reserve` (not refunded). Allocates a slot from the free-list if available, otherwise from the end of the array. Mode is immutable after creation.

#### sendToGate (Input Type 2)

Sends QU through a gate. Attach QU as the invocation reward.

**Input**:

| Field | Type | Size | Description |
|-------|------|------|-------------|
| gateId | uint64 | 8 | Target gate ID |

**Output**:

| Field | Type | Description |
|-------|------|-------------|
| status | sint64 | 0 on success, negative on error |

**Behaviour**: Validates the gate exists and is active. Burns dust amounts below `_minSendAmount`. Routes the payment according to the gate's mode. Updates `lastActivityEpoch`.

#### closeGate (Input Type 3)

Closes a gate. Owner only.

**Input**:

| Field | Type | Size | Description |
|-------|------|------|-------------|
| gateId | uint64 | 8 | Gate ID to close |

**Output**:

| Field | Type | Description |
|-------|------|-------------|
| status | sint64 | 0 on success, negative on error |

**Behaviour**: Refunds any held balance (THRESHOLD mode) and `reserve` to the owner. Marks the gate inactive. Pushes the slot onto the free-list for reuse. Refunds any attached invocation reward.

#### updateGate (Input Type 4)

Modifies gate configuration. Owner only. Mode cannot be changed.

**Input**:

| Field | Type | Size | Description |
|-------|------|------|-------------|
| gateId | uint64 | 8 | Gate ID to update |
| recipientCount | uint8 | 1 | New recipient count (1-8) |
| recipients | Array\<id, 8\> | 256 | New recipients |
| ratios | Array\<uint64, 8\> | 64 | New ratios |
| threshold | uint64 | 8 | New threshold |
| allowedSenders | Array\<id, 8\> | 256 | New allowed senders |
| allowedSenderCount | uint8 | 1 | New allowed sender count (0-8) |
| recipientGateIds | Array\<sint64, 8\> | 64 | Gate-as-recipient IDs (-1 = wallet) |

**Output**:

| Field | Type | Description |
|-------|------|-------------|
| status | sint64 | 0 on success, negative on error |

Wire size: see [updateGate_input Layout](#updategate_input-layout) for exact byte offsets.

**Behaviour**: Validates parameters (same rules as createGate for the relevant mode). Zeros stale slots when recipient/sender count shrinks. Updates `lastActivityEpoch`. Refunds any attached invocation reward.

### Functions (Read-Only)

#### getGate (Input Type 5)

Returns full gate configuration and statistics.

**Input**: `gateId` (uint64)

**Output**:

| Field | Type | Description |
|-------|------|-------------|
| mode | uint8 | Gate mode |
| recipientCount | uint8 | Number of recipients |
| active | uint8 | 1 = active, 0 = inactive |
| owner | id | Gate owner public key |
| totalReceived | uint64 | Cumulative QU received |
| totalForwarded | uint64 | Cumulative QU forwarded |
| currentBalance | uint64 | Held balance (THRESHOLD mode) |
| threshold | uint64 | Threshold amount |
| createdEpoch | uint16 | Epoch when gate was created |
| lastActivityEpoch | uint16 | Epoch of last send/update |
| recipients | Array\<id, 8\> | Recipient addresses |
| ratios | Array\<uint64, 8\> | Recipient ratios |
| allowedSenders | Array\<id, 8\> | Allowed sender addresses (CONDITIONAL mode) |
| allowedSenderCount | uint8 | Number of allowed senders |
| chainNextGateId | sint64 | Versioned gate ID of next gate in chain (-1 if no chain) |
| chainDepth | uint8 | This gate's position in its chain (0 = root) |
| reserve | sint64 | Unified reserve (covers chain hop fees + inactivity maintenance) |
| nextIdleChargeEpoch | uint16 | Next epoch when idle maintenance will be charged |
| adminGateId | sint64 | Versioned gate ID of admin gate (-1 if no admin gate) |
| hasAdminGate | uint8 | 1 if governed by an admin gate |
| idleDelinquent | uint8 | 1 if gate has missed idle-period funding |
| idleGraceRemainingEpochs | uint16 | Epochs remaining before delinquent gate expires |
| idleExpiryOverdue | uint8 | 1 if idle grace has already been exhausted |
| recipientGateIds | Array\<sint64, 8\> | Gate-as-recipient IDs (-1 = wallet, >= 0 = gate ID) |

Returns `active=0` for invalid gate IDs.

#### getGateCount (Input Type 6)

**Input**: (none)

**Output**:

| Field | Type | Description |
|-------|------|-------------|
| totalGates | uint64 | Total gates ever allocated (high-water mark) |
| activeGates | uint64 | Currently active gates |
| totalBurned | uint64 | Cumulative QU burned by contract |

#### getGatesByOwner (Input Type 7)

**Input**: `owner` (id, 32 bytes)

**Output**:

| Field | Type | Description |
|-------|------|-------------|
| gateIds | Array\<uint64, 16\> | Gate IDs owned by address |
| count | uint64 | Number of results (max 16) |

Performs a linear scan of all gate slots. Returns up to 16 gates.

#### getGateBatch (Input Type 8)

**Input**: `gateIds` (Array\<uint64, 32\>) — up to 32 gate IDs to query.

**Output**: `gates` (Array\<getGate_output, 32\>) — corresponding gate data. Invalid IDs return zeroed entries.

#### getFees (Input Type 9)

**Input**: (none)

**Output**:

| Field | Type | Description |
|-------|------|-------------|
| creationFee | uint64 | Base creation fee |
| currentCreationFee | uint64 | Actual fee after escalation |
| minSendAmount | uint64 | Minimum send amount |
| expiryEpochs | uint64 | Epochs of inactivity before expiry |

#### fundGate (Input Type 10)

Adds invocationReward to a gate's unified reserve. Anyone can fund a gate.

**Input**:

| Field | Type | Size | Description |
|-------|------|------|-------------|
| gateId | sint64 | 8 | Target gate ID |

**Output**:

| Field | Type | Description |
|-------|------|-------------|
| result | sint64 | 0 on success, negative on error |

**Behaviour**: Validates the gate exists and is active. Attached QU is deposited into the gate's `reserve`.

#### setChain (Input Type 11)

Sets or clears the chain link on an existing gate. Owner only. Burns one hop fee.

**Input**:

| Field | Type | Size | Description |
|-------|------|------|-------------|
| gateId | sint64 | 8 | Source gate ID |
| nextGateId | sint64 | 8 | Target gate ID to chain to (-1 to clear chain) |

**Output**:

| Field | Type | Description |
|-------|------|-------------|
| result | sint64 | 0 on success, negative on error |

**Behaviour**: Validates ownership and active status. Requires `QUGATE_CHAIN_HOP_FEE` as invocation reward (burned). Validates the target gate exists, is active, and the resulting chain depth does not exceed `QUGATE_MAX_CHAIN_DEPTH`. Performs cycle detection by walking forward from the target. Setting `nextGateId=-1` clears the chain link.

#### sendToGateVerified (Input Type 12)

Like `sendToGate`, but additionally asserts that `gate.owner == expectedOwner` before routing. Full refund if mismatch. Prevents payments to a recycled slot whose new gate has a different owner.

**Input**:

| Field | Type | Size | Description |
|-------|------|------|-------------|
| gateId | uint64 | 8 | Target gate ID |
| expectedOwner | id | 32 | Expected gate owner public key |

**Output**:

| Field | Type | Description |
|-------|------|-------------|
| status | sint64 | 0 on success, negative on error |

**Behaviour**: Identical to `sendToGate` except for the owner check. Returns `QUGATE_OWNER_MISMATCH` (-15) with full refund if `gate.owner != expectedOwner`. All other validation, dust burn, and routing logic is the same as `sendToGate`.

#### configureHeartbeat (Input Type 13)

Configures heartbeat mode on a HEARTBEAT gate. Owner only. Sets the inactivity threshold, payout rate, minimum balance, and beneficiary list. Cannot be called after the gate has triggered.

**Input**:

| Field | Type | Size | Description |
|-------|------|------|-------------|
| gateId | uint64 | 8 | Target HEARTBEAT gate ID |
| thresholdEpochs | uint32 | 4 | Epochs of inactivity before trigger (>= 1) |
| payoutPercentPerEpoch | uint8 | 1 | % of balance to pay per epoch after trigger (1-100) |
| minimumBalance | sint64 | 8 | Gate auto-closes when balance falls to or below this |
| beneficiaryAddresses | Array\<id, 8\> | 256 | Beneficiary public keys |
| beneficiaryShares | Array\<uint8, 8\> | 8 | Share per beneficiary (must sum to 100) |
| beneficiaryCount | uint8 | 1 | Number of beneficiaries (1-8) |

**Output**: `status` (sint64)

#### heartbeat (Input Type 14)

Resets the heartbeat epoch counter. Owner only. Rejected after the gate has triggered. Attach any QU — it is refunded.

**Input**: `gateId` (uint64)

**Output**: `status` (sint64), `epochRecorded` (uint32)

#### getHeartbeat (Input Type 15)

Read-only query for heartbeat configuration and state.

**Input**: `gateId` (uint64)

**Output**:

| Field | Type | Description |
|-------|------|-------------|
| active | uint8 | 1 if heartbeat is configured |
| triggered | uint8 | 1 if the gate has triggered |
| thresholdEpochs | uint32 | Inactivity threshold |
| lastHeartbeatEpoch | uint32 | Epoch of last heartbeat() call |
| triggerEpoch | uint32 | Epoch when gate triggered (0 if not yet triggered) |
| payoutPercentPerEpoch | uint8 | Payout rate per epoch |
| minimumBalance | sint64 | Auto-close threshold |
| beneficiaryCount | uint8 | Number of beneficiaries |
| beneficiaryAddresses | Array\<id, 8\> | Beneficiary addresses |
| beneficiaryShares | Array\<uint8, 8\> | Beneficiary share percentages |

#### configureMultisig (Input Type 16)

Configures M-of-N guardian approval on a MULTISIG gate. Owner only. Resets any active proposal.

**Input**:

| Field | Type | Size | Description |
|-------|------|------|-------------|
| gateId | uint32 | 4 | Target MULTISIG gate ID |
| guardians | Array\<id, 8\> | 256 | Guardian public keys |
| guardianCount | uint8 | 1 | Number of guardians (1-8) |
| required | uint8 | 1 | Minimum approvals needed (1-guardianCount) |
| proposalExpiryEpochs | uint32 | 4 | Epochs before incomplete proposal resets (>= 1) |

**Output**: `status` (sint64)

**Validation**: No duplicate guardians. `required <= guardianCount`. `proposalExpiryEpochs >= 1`.

#### getMultisigState (Input Type 17)

Read-only query for current multisig proposal state.

**Input**: `gateId` (uint32)

**Output**:

| Field | Type | Description |
|-------|------|-------------|
| status | sint64 | 0 on success, negative on error |
| approvalBitmap | uint8 | Bitmask of guardians who have voted (bit i = guardian i) |
| approvalCount | uint8 | Current vote count |
| required | uint8 | Threshold needed for execution |
| guardianCount | uint8 | Total number of guardians |
| proposalEpoch | uint32 | Epoch when current proposal started |
| proposalActive | uint8 | 1 if a proposal is in progress |
| guardians | Array\<id, 8\> | Guardian public keys from MultisigConfig |

#### configureTimeLock (Input Type 18)

Configures a TIME_LOCK gate with an unlock epoch and cancellability flag. Owner only. The unlock epoch must be in the future.

**Input**:

| Field | Type | Size | Description |
|-------|------|------|-------------|
| gateId | uint64 | 8 | Target TIME_LOCK gate ID |
| unlockEpoch | uint32 | 4 | Epoch when funds release (absolute mode, must be > current epoch) |
| delayEpochs | uint32 | 4 | Epochs from now until unlock (relative mode, must be > 0) |
| lockMode | uint8 | 1 | 0 = absolute epoch, 1 = relative epochs |
| cancellable | uint8 | 1 | 1 = owner can cancel before unlock |

**Output**: `status` (sint64)

**Validation**: Gate must be mode 8 (TIME_LOCK) and active. `lockMode` must be 0 or 1. In absolute mode, `unlockEpoch` must be > current epoch. In relative mode, `delayEpochs` must be > 0. Owner only. Returns `QUGATE_INVALID_PARAMS` (-31) for invalid lockMode or zero delayEpochs.

#### cancelTimeLock (Input Type 19)

Cancels a TIME_LOCK gate before the unlock epoch. Owner only. Requires `cancellable=1`. Refunds all held balance to the gate owner and closes the gate.

**Input**:

| Field | Type | Size | Description |
|-------|------|------|-------------|
| gateId | uint32 | 4 | Target TIME_LOCK gate ID |

**Output**: `status` (sint64)

**Validation**: Gate must be TIME_LOCK, active, not yet fired, and `cancellable=1`. Returns `QUGATE_TIME_LOCK_NOT_CANCELLABLE` (-24) if cancellable=0, `QUGATE_TIME_LOCK_ALREADY_FIRED` (-23) if already unlocked.

#### getTimeLockState (Input Type 20)

Read-only query for TIME_LOCK gate state.

**Input**: `gateId` (uint64)

**Output**:

| Field | Type | Description |
|-------|------|-------------|
| status | sint64 | 0 on success, negative on error |
| unlockEpoch | uint32 | Target epoch for fund release |
| delayEpochs | uint32 | Original delay (relative mode) |
| lockMode | uint8 | 0 = absolute, 1 = relative |
| cancellable | uint8 | 1 if owner can cancel |
| fired | uint8 | 1 if funds have been released |
| cancelled | uint8 | 1 if cancelled by owner |
| active | uint8 | 1 if time lock is configured |
| currentBalance | sint64 | Held balance |
| currentEpoch | uint32 | Current network epoch |
| epochsRemaining | uint32 | Epochs until unlock (0 if fired or past) |

#### setAdminGate (Input Type 21)

Sets or clears the admin gate on a gate. When setting, the admin gate must exist and be MULTISIG mode. Owner-only if no admin gate is currently set; requires admin gate approval if one is already set, except that the owner may directly clear an expired, closed, stale, or otherwise invalid current admin gate.

**Input**:

| Field | Type | Size | Description |
|-------|------|------|-------------|
| gateId | uint64 | 8 | Target gate ID |
| adminGateId | sint64 | 8 | MULTISIG gate ID to assign (-1 to clear) |

**Output**: `status` (sint64)

**Validation**: Target gate must be active. Admin gate (if not -1) must exist, be active, and be MULTISIG mode. Returns `QUGATE_INVALID_ADMIN_GATE` (-27) on invalid admin gate. Returns `QUGATE_ADMIN_GATE_REQUIRED` (-26) if an active admin gate is set and caller lacks approval. If the current admin gate has expired, been closed, become stale, or is no longer an active MULTISIG gate, the owner may clear it with `adminGateId=-1`.

#### getAdminGate (Input Type 22)

Read-only query for admin gate configuration on a gate.

**Input**: `gateId` (uint64)

**Output**:

| Field | Type | Description |
|-------|------|-------------|
| hasAdminGate | uint8 | 1 if an admin gate is assigned |
| adminGateId | sint64 | Versioned gate ID of the admin gate (-1 if none) |
| adminGateMode | uint8 | Mode of the admin gate (should be MULTISIG=7) |
| guardianCount | uint8 | Number of guardians on the admin gate |
| required | uint8 | M-of-N threshold on the admin gate |
| guardians | Array\<id, 8\> | Guardian public keys from the admin gate's MultisigConfig |

#### getGateBySlot (Input Type 25)

Queries a gate by its raw slot index (0-based) rather than versioned gate ID. Useful for iterating all slots.

**Input**: `slotIndex` (uint64)

**Output**:

| Field | Type | Description |
|-------|------|-------------|
| valid | uint8 | 1 if slot is within gateCount range |
| gateId | uint64 | Current versioned gate ID for this slot |
| generation | uint16 | Current generation counter |
| *(remaining fields)* | | Same as `getGate_output` (mode, recipientCount, active, owner, etc.) |

#### getLatestExecution (Input Type 26)

Returns the most recent execution metadata for a gate — what happened the last time a payment was routed through it. Useful for debugging and observability without parsing logs.

**Input**: `gateId` (uint64)

**Output**:

| Field | Type | Description |
|-------|------|-------------|
| valid | uint8 | 1 if an execution has been recorded for this gate |
| mode | uint8 | Gate mode at execution time |
| outcomeType | uint8 | Execution outcome (`QUGATE_EXEC_*`: 0=NONE, 1=FORWARDED, 2=HELD, 3=REFUNDED, 4=BURNED, 5=REJECTED) |
| selectedRecipientIndex | uint8 | Recipient chosen (255 = none/not applicable) |
| selectedDownstreamGateId | sint64 | Gate-as-recipient target (-1 if wallet) |
| forwardedAmount | uint64 | Amount forwarded in this execution |
| observedTick | uint64 | Tick number at execution time |

---

## Wire Format

All data is little-endian. Public keys (`id`) are 32 bytes. `Array<T, N>` is serialized as N contiguous elements with no length prefix.

### createGate_input Layout

```
Offset  Size   Field
------  -----  -----
0       1      mode (uint8)
1       1      recipientCount (uint8)
8       256    recipients[8] (8 x 32-byte pubkeys)
264     64     ratios[8] (8 x uint64)
328     8      threshold (uint64)
336     256    allowedSenders[8] (8 x 32-byte pubkeys)
592     1      allowedSenderCount (uint8)
600     8      chainNextGateId (sint64, -1 = no chain)
608     64     recipientGateIds[8] (8 x sint64, -1 = wallet)
```

Oracle fields were removed in v2.5 when reserves were unified. Exact struct size depends on compiler alignment — check `encoding.service.ts` in the demo app for current byte offsets.

### updateGate_input Layout

```
Offset  Size   Field
------  -----  -----
0       8      gateId (uint64)
8       1      recipientCount (uint8)
+pad    ...    (compiler alignment padding)
        256    recipients[8]
        64     ratios[8]
        8      threshold (uint64)
        256    allowedSenders[8]
        1      allowedSenderCount (uint8)
        64     recipientGateIds[8] (8 x sint64, -1 = wallet)
+pad    ...    (trailing padding)
```

Note: Exact byte offsets depend on the Qubic core compiler's struct alignment rules. The sizes above are the logical field sizes. Unused recipient/ratio/sender slots beyond the declared counts should be zeroed.

---

## State Architecture

### Contract State

```
_gateCount      uint64                          High-water mark of allocated gate slots
_activeGates    uint64                          Currently active gates
_totalBurned    uint64                          Cumulative QU burned
_gates          Array<GateConfig, MAX_GATES>    Gate storage
_freeSlots      Array<uint64, MAX_GATES>        Free-list stack (slot indices)
_freeCount      uint64                          Number of entries in free-list
_creationFee    uint64                          Base creation fee (adjustable)
_minSendAmount  uint64                          Minimum send amount (adjustable)
_expiryEpochs   uint64                          Inactivity epochs before expiry (adjustable)
```

### GateConfig Structure (~280 bytes per gate)

```
owner               id (32 bytes)       Gate creator
mode                uint8               Gate mode (0-8, immutable)
recipientCount      uint8               Active recipients (1-8)
active              uint8               1 = active, 0 = closed/expired
allowedSenderCount  uint8               Allowed senders for CONDITIONAL
createdEpoch        uint16              Creation epoch
lastActivityEpoch   uint16              Last send/update/create epoch
totalReceived       uint64              Cumulative QU received
totalForwarded      uint64              Cumulative QU forwarded
currentBalance      uint64              Held balance (THRESHOLD mode)
threshold           uint64              Threshold amount
roundRobinIndex     uint64              Next recipient index (ROUND_ROBIN)
recipients          Array<id, 8>        Recipient addresses (256 bytes)
ratios              Array<uint64, 8>    Ratios (64 bytes)
allowedSenders      Array<id, 8>        Allowed senders (256 bytes)
chainNextGateId     sint64              Next gate in chain; -1 if terminal
reserve             sint64              Unified reserve (chain hop fees + maintenance)
chainDepth          uint8               Position in chain (0 = root)
adminGateId         sint64              MULTISIG gate ID for governance; -1 if none
hasAdminGate        uint8               1 if adminGateId is set
```

### Free-List

Gate slots are reused via a stack-based free-list. When a gate is closed or expires, its slot index is pushed onto `_freeSlots`. When a new gate is created, the contract first pops from the free-list; only if empty does it allocate from the end of the array (incrementing `_gateCount`).

This prevents a permanent denial-of-service where an attacker creates and closes gates to exhaust the 256-slot array (at X_MULTIPLIER=1).

Total state size at X_MULTIPLIER=1: approximately 75 KB (256 gates x ~280 bytes + free-list overhead + metadata).

---

## Anti-Spam Mechanisms

### Escalating Creation Fees

The creation fee increases as capacity fills:

```
fee = baseFee * (1 + activeGates / FEE_ESCALATION_STEP)
```

Where `FEE_ESCALATION_STEP = 1024` (integer division).

| Active Gates | Multiplier | Fee (base=100,000 QU) |
|--------------|------------|----------------------|
| 0 - 255 (X=1) | 1x | 100,000 |
| 0 - 1,023 (X=4+) | 1x | 100,000 |
| 1,024 - 2,047 | 2x | 200,000 |
| 2,048 - 3,071 | 3x | 300,000 |

At X_MULTIPLIER=10 with 2,560 slots, the maximum multiplier would be 3x (300,000 QU). At X_MULTIPLIER=100 with 25,600 slots, the maximum would be 26x (2,600,000 QU). This makes bulk slot squatting progressively expensive.

Use `getFees()` to query the current escalated fee before creating a gate.

### Gate Expiry

Gates can expire for two reasons:

1. inactivity for `_expiryEpochs` epochs
2. unpaid maintenance after the delinquency grace window

The contract still uses two complementary mechanisms:

1. **Lazy expiry on interaction** (primary): When any procedure (`sendToGate`, `sendToGateVerified`, `updateGate`, `closeGate`, `fundGate`, `setChain`) touches a gate, it checks whether the gate has exceeded its inactivity window. If so, the gate is expired inline — balances (currentBalance and reserve) are refunded to the owner, the slot is freed, and the caller is refunded. The `getGate` function also reports expired gates as `active=0` without mutating state.

2. **END_EPOCH sweep** (safety net): The `END_EPOCH` handler still scans all gates each epoch. This catches gates that nobody interacts with. At scale, most expiries happen lazily, reducing `END_EPOCH` processing load.

The inactivity expiry check uses:

```
if (epoch() - lastActivityEpoch >= expiryEpochs)  →  expire
```

Expired gates:
- Have any held balance (currentBalance and reserve) refunded to the owner
- Are marked inactive
- Have their slot pushed onto the free-list
- Have their generation counter incremented (invalidates old gate IDs)

Default inactivity expiry: 50 epochs (approximately 1 year at current epoch length). Set to 0 to disable inactivity expiry entirely. Shareholder-adjustable.

### Maintenance delinquency

Idle gates are expected to maintain a funded `reserve`. If a gate has no qualifying activity for `4` epochs and is not in an active hold state, the contract charges `25,000 QU` from the reserve. If the reserve cannot cover the fee:

- the gate is marked delinquent
- a `4` epoch grace window begins
- if the reserve is funded before grace ends, the gate cures and continues normally
- if not, the gate expires and reserves/balances are refunded to the owner

This makes maintenance predictable and reserve-backed instead of silently draining operational balances.

### Dust Burn

Sends below `_minSendAmount` (default: 1,000 QU) are burned via `qpi.burn()` and never forwarded. The sender receives status `QUGATE_DUST_AMOUNT`. Zero-amount sends return immediately with no burn.

---

## Fee Economics

QuGate uses a hybrid fee model:

- **Creation fees**: burned on successful gate creation
- **Dust amounts**: burned when sends are below minimum
- **Chain hop fees**: burned on each routed hop
- **Inactivity maintenance**: charged from `reserve` and split:
  - `80%` burned
  - `20%` routed through the standard dividend/shareholder distribution path

The contract tracks cumulative:

- burned QU
- maintenance charged
- maintenance burned
- maintenance dividends earned and distributed

These values are queryable via `getGateCount()`.

---

## Shareholder Governance

Three parameters are stored as state variables and designed to be adjustable via shareholder vote:

| Parameter | State Variable | Default |
|-----------|----------------|---------|
| Base creation fee | `_creationFee` | 100,000 QU |
| Minimum send amount | `_minSendAmount` | 1,000 QU |
| Expiry period | `_expiryEpochs` | 50 epochs |

**Current status**: The `END_EPOCH` handler contains a stub for processing shareholder proposals. Full wiring requires `DEFINE_SHAREHOLDER_PROPOSAL_STORAGE` with a valid `QUGATE_CONTRACT_ASSET_NAME`, which needs `assetNameFromString("QUGATE")` at contract registration. The mechanism is the same pattern used by QUtil and MsVault.

Until the shareholder infrastructure is wired, fees are set during `INITIALIZE` and cannot be changed without a contract upgrade.

---

## Design Decisions

### Why 9 modes?

Eight active modes cover the most common payment routing and custody patterns observed in blockchain applications: proportional splitting (revenue share), rotation (load balancing/fairness), accumulation (crowdfunding/escrow), randomization (lottery/raffle), access control (whitelisting), heartbeat-based inheritance (dead-man's switch), M-of-N multisig approval (DAO treasury, joint accounts), and epoch-based time locks (vesting, escrow). Mode slot 5 is reserved. Together they can be composed to handle most real-world payment flows without custom contracts.

### Why 8 max recipients?

8 recipients keeps per-gate state under 300 bytes while covering the vast majority of practical use cases (team payrolls, multi-way splits, worker pools). Larger recipient sets can be achieved by chaining SPLIT gates via intermediary forwarding (each hop requires a separate transaction).

### Why ratio cap at 10,000?

Without a cap, `amount * ratio` could overflow uint64 for large payments combined with large ratios. Capping individual ratios at 10,000 ensures `amount * ratio` stays within uint64 range for any realistic payment amount. The overflow-safe division formula provides additional protection.

### Why immutable mode?

A gate's mode is set at creation and cannot be changed via `updateGate`. This prevents a class of attacks where a gate owner changes the mode after senders have configured their systems to expect specific routing behaviour. Recipients and ratios can still be updated, but the fundamental routing logic is fixed.

### Why a free-list?

Without slot reuse, an attacker could create and close all gate slots to permanently exhaust capacity. The free-list ensures closed gate slots are recycled. Combined with escalating fees, this makes the contract resilient to slot exhaustion attacks.

### Why maintenance reserve plus dividends?

Creation, dust, and chain-hop fees still follow the original burn-first philosophy. The maintenance model adds reserve-backed upkeep so long-lived infrastructure pays its way without silently draining operational balances.

Using a unified `reserve` keeps the behavior predictable:

- pass-through gates do not lose working funds unexpectedly
- governance-only admin multisigs can be explicitly provisioned
- delinquency is visible and recoverable
- expiry remains a cleanup backstop rather than the only lifecycle pressure

### Why tick entropy for RANDOM?

`qpi.tick()` provides the only source of non-deterministic data available within the QPI. While not cryptographically random, the tick number is not controllable by the sender at the time of transaction submission, providing sufficient unpredictability for payment routing. The formula `mod(totalReceived + tick(), recipientCount)` ensures each payment produces a different selection even within the same tick.

---

## Security Model

### Burn-First Design

Creation fees, dust burns, and chain hop fees are destroyed via `qpi.burn()`. Inactivity maintenance is reserve-backed and uses an `80% burn / 20% dividends` split, keeping the contract mostly deflationary while supporting sustainable long-lived infrastructure.

### No Rug-Pull

THRESHOLD gates with a non-zero `currentBalance` cannot have their recipients changed via `updateGate`. This prevents an owner from redirecting accumulated funds to a different address after senders have deposited.

### Versioned Gate IDs

Gate IDs encode a generation counter to prevent slot-reuse attacks. See [Gate ID Format](#gate-id-format).

### Chain Cycle Prevention

Chain links are validated at creation and update time. The contract walks forward from the target gate (up to `QUGATE_MAX_CHAIN_DEPTH` steps) to detect cycles. Depth is capped at 3 hops.

### RANDOM Entropy Warning

RANDOM mode uses `mod(totalReceived + tick(), recipientCount)` as entropy. Both `totalReceived` and `tick()` are publicly observable before transaction submission, making recipient selection predictable to a determined observer. Not suitable for use cases requiring cryptographic randomness.

### activeGates Underflow Guard

The `active == 1` check before decrementing `_activeGates` in `closeGate` and `END_EPOCH` prevents underflow from double-close scenarios.

### Transfer-First State Updates [QG-01..QG-17]

All `qpi.transfer()` calls that move funds (to recipients, owners, beneficiaries) check the return value (`>= 0`) before updating state. If a transfer fails, state remains unchanged — no funds are lost or double-counted. Error-path refunds (returning `invocationReward` on validation failure) don't need this pattern since no state mutation precedes them.

### Single invocationReward Capture

Every public procedure captures `qpi.invocationReward()` into `locals.invReward` at entry. All subsequent references use the cached value. This prevents inconsistencies if the value were to change between calls and simplifies audit of refund paths.

---

## Edge Cases and Safety

### Dead Chain Links

When a gate is chained to another gate that has been closed, expired, or recycled, the chain forwarding detects the dead link and reverts undeliverable funds to the source gate's currentBalance. The owner can then either fix the chain via setChain or close the gate to recover funds. No QU is lost.

### Chain-Only Gates (0 Recipients)

Gates with recipientCount=0 are valid only when chainNextGateId is set. All funds flow through the chain. If the chain link dies, funds revert to currentBalance. The owner can always closeGate to recover.

### Admin Gate Expiry

If a gate's admin MULTISIG gate expires or is closed, the gate owner can still clear the admin gate (setAdminGate with adminGateId=-1) and regain full control. This prevents permanently locked gates.

### Lazy Expiry on Interaction

When a procedure (`sendToGate`, `updateGate`, `closeGate`, `fundGate`, `setChain`) encounters an expired gate, it expires the gate inline and refunds all balances to the owner. The caller receives `QUGATE_GATE_NOT_ACTIVE` and their invocation reward is refunded. This means most expiries happen on first interaction after the inactivity window, rather than waiting for the next `END_EPOCH` sweep. The `END_EPOCH` sweep remains as a safety net for gates that nobody interacts with.

### Heartbeat + Expiry Interaction

Calling heartbeat() refreshes lastActivityEpoch, preventing the 50-epoch expiry from firing. If the owner stops calling heartbeat(), the heartbeat trigger fires first (at thresholdEpochs), then payouts run. The gate won't expire during active payouts because END_EPOCH payout processing counts as implicit activity.

### Round Robin After Recipient Reduction

When updateGate reduces recipientCount, the roundRobinIndex is automatically reset to 0 if it would be out of bounds. This prevents payments to zeroed addresses.

### MULTISIG Proposal Expiry

Incomplete proposals (fewer than M votes) automatically reset after proposalExpiryEpochs. Funds remain in the gate and a new proposal can begin. Guardian votes are tracked via a bitmap — each guardian can only vote once per proposal.

### Fund Recovery

The owner can always recover funds via:
- closeGate: refunds currentBalance and reserve to owner
- withdrawReserve: retrieves reserve funds without closing
- cancelTimeLock: refunds TIME_LOCK balance (if cancellable)

No QU can be permanently locked in the contract (assuming the owner retains access).

---

## Status Codes

| Code | Constant | Description |
|------|----------|-------------|
| 0 | `QUGATE_SUCCESS` | Operation succeeded |
| -1 | `QUGATE_INVALID_GATE_ID` | Gate ID is 0, exceeds gateCount, or wrong generation |
| -2 | `QUGATE_GATE_NOT_ACTIVE` | Gate exists but has been closed or expired |
| -3 | `QUGATE_UNAUTHORIZED` | Caller is not the gate owner |
| -4 | `QUGATE_INVALID_MODE` | Mode value exceeds QUGATE_MODE_TIME_LOCK (8) |
| -5 | `QUGATE_INVALID_RECIPIENT_COUNT` | Recipient count is 0 or > 8 |
| -6 | `QUGATE_INVALID_RATIO` | Individual ratio > 10,000 or total ratio is 0 |
| -7 | `QUGATE_INSUFFICIENT_FEE` | Invocation reward < escalated creation fee |
| -8 | `QUGATE_NO_FREE_SLOTS` | Free-list empty and gateCount at QUGATE_MAX_GATES |
| -9 | `QUGATE_DUST_AMOUNT` | Send amount is 0 or below minimum (burned) |
| -10 | `QUGATE_INVALID_THRESHOLD` | Threshold is 0 for THRESHOLD mode |
| -11 | `QUGATE_INVALID_SENDER_COUNT` | allowedSenderCount exceeds QUGATE_MAX_RECIPIENTS |
| -12 | `QUGATE_CONDITIONAL_REJECTED` | Sender not in allowed list (funds bounced) |
| -13 | `QUGATE_INVALID_ORACLE_CONFIG` | Reserved (mode slot 5 not yet available) |
| -14 | `QUGATE_INVALID_CHAIN` | Chain target invalid, depth exceeded, or cycle detected |
| -15 | `QUGATE_OWNER_MISMATCH` | gate.owner != expectedOwner in sendToGateVerified |
| -16 | `QUGATE_HEARTBEAT_TRIGGERED` | heartbeat() called after gate already triggered |
| -17 | `QUGATE_HEARTBEAT_NOT_ACTIVE` | heartbeat() or configureHeartbeat() on non-HEARTBEAT gate |
| -18 | `QUGATE_HEARTBEAT_INVALID` | Invalid heartbeat config (bad shares, percent, threshold) |
| -19 | `QUGATE_MULTISIG_NOT_GUARDIAN` | Sender is not in the guardian list |
| -20 | `QUGATE_MULTISIG_ALREADY_VOTED` | Guardian already voted on the current proposal |
| -21 | `QUGATE_MULTISIG_INVALID_CONFIG` | Invalid config (0 guardians, required > count, etc) |
| -22 | `QUGATE_MULTISIG_NO_ACTIVE_PROP` | No active multisig proposal to query |
| -23 | `QUGATE_TIME_LOCK_ALREADY_FIRED` | Gate already unlocked and closed |
| -24 | `QUGATE_TIME_LOCK_NOT_CANCELLABLE` | cancelTimeLock() called but cancellable=0 |
| -25 | `QUGATE_TIME_LOCK_EPOCH_PAST` | unlockEpoch is in the past at configuration time |
| -26 | `QUGATE_ADMIN_GATE_REQUIRED` | Config change needs admin gate approval |
| -27 | `QUGATE_INVALID_ADMIN_GATE` | adminGateId doesn't exist or isn't MULTISIG mode |
| -28 | `QUGATE_INVALID_GATE_RECIPIENT` | recipientGateIds entry is invalid (bad slot/gen/inactive) |
| -29 | `QUGATE_INVALID_ADMIN_CYCLE` | adminGateId creates a circular admin chain (self or loop) |
| -30 | `QUGATE_MULTISIG_PROPOSAL_ACTIVE` | configureMultisig blocked while proposal is in progress |
| -31 | `QUGATE_INVALID_PARAMS` | Generic invalid parameter (e.g. bad lockMode or zero delayEpochs) |
| -32 | `QUGATE_UNSUPPORTED_MODE` | Mode is reserved and not yet available (e.g. ORACLE mode 5) |

---

## Log Types

### Success Events (low range)

| Value | Constant | Description |
|-------|----------|-------------|
| 1 | `QUGATE_LOG_GATE_CREATED` | Gate successfully created |
| 2 | `QUGATE_LOG_GATE_CLOSED` | Gate closed by owner |
| 3 | `QUGATE_LOG_GATE_UPDATED` | Gate configuration updated |
| 4 | `QUGATE_LOG_PAYMENT_FORWARDED` | Payment routed through gate |
| 5 | `QUGATE_LOG_PAYMENT_BOUNCED` | Payment bounced (CONDITIONAL reject) |
| 6 | `QUGATE_LOG_DUST_BURNED` | Dust amount burned |
| 7 | `QUGATE_LOG_FEE_CHANGED` | Fee parameter changed (future) |
| 8 | `QUGATE_LOG_GATE_EXPIRED` | Gate auto-closed due to inactivity |
| 9 | `QUGATE_LOG_ORACLE_TRIGGERED` | Reserved |
| 10 | `QUGATE_LOG_ORACLE_EXHAUSTED` | Reserved |
| 11 | `QUGATE_LOG_ORACLE_SUBSCRIBED` | Reserved |
| 12 | `QUGATE_LOG_CHAIN_HOP` | Chain hop executed — funds routed to next gate |
| 13 | `QUGATE_LOG_CHAIN_CYCLE` | Chain cycle detected or max depth exceeded |
| 14 | `QUGATE_LOG_CHAIN_HOP_INSUFFICIENT` | Hop fee not payable; funds stranded in currentBalance |
| 15 | `QUGATE_LOG_HEARTBEAT_CONFIGURED` | configureHeartbeat() called successfully |
| 16 | `QUGATE_LOG_HEARTBEAT_PULSE` | heartbeat() called, epoch counter reset |
| 17 | `QUGATE_LOG_HEARTBEAT_TRIGGERED` | Threshold exceeded, heartbeat gate triggered |
| 18 | `QUGATE_LOG_HEARTBEAT_PAYOUT` | Recurring payout dispatched to beneficiaries |
| 19 | `QUGATE_LOG_MULTISIG_VOTE` | Guardian voted on active proposal |
| 20 | `QUGATE_LOG_MULTISIG_EXECUTED` | M-of-N threshold reached, funds released |
| 21 | `QUGATE_LOG_MULTISIG_EXPIRED` | Proposal expired without reaching threshold |
| 22 | `QUGATE_LOG_MULTISIG_CONFIGURED` | configureMultisig() called successfully |
| 23 | `QUGATE_LOG_TIME_LOCK_FIRED` | Unlock epoch reached, funds released to target |
| 24 | `QUGATE_LOG_TIME_LOCK_CANCELLED` | Owner cancelled, funds refunded |
| 25 | `QUGATE_LOG_TIME_LOCK_CONFIGURED` | configureTimeLock() called successfully |
| 26 | `QUGATE_LOG_ADMIN_GATE_SET` | Admin gate assigned to a gate |
| 27 | `QUGATE_LOG_ADMIN_GATE_CLEARED` | Admin gate removed from a gate |
| 28 | `QUGATE_LOG_ADMIN_APPROVAL_USED` | Admin gate approval consumed for a config change |

### Failure Events (high range)

| Value | Constant | Description |
|-------|----------|-------------|
| 100 | `QUGATE_LOG_FAIL_INVALID_GATE` | Invalid gate ID |
| 101 | `QUGATE_LOG_FAIL_NOT_ACTIVE` | Gate not active |
| 102 | `QUGATE_LOG_FAIL_UNAUTHORIZED` | Unauthorized caller |
| 103 | `QUGATE_LOG_FAIL_INVALID_PARAMS` | Invalid parameters |
| 104 | `QUGATE_LOG_FAIL_INSUFFICIENT_FEE` | Fee too low |
| 105 | `QUGATE_LOG_FAIL_NO_SLOTS` | No slots available |
| 106 | `QUGATE_LOG_FAIL_OWNER_MISMATCH` | Owner verification failed (sendToGateVerified) |

### Log Structures

**QuGateLogger** (general events): `_contractIndex` (uint32), `_type` (uint32), `gateId` (uint64), `sender` (id), `amount` (sint64), `_terminator` (sint8).


---

## Building and Testing

### Guard Rails

Run this before pushing structural changes to `QuGate.h`:

```bash
python3 scripts/contract_guard.py
```

It currently checks:

- contract constants vs `contract_qugate.cpp` harness constants
- public functions accidentally calling private procedures
- obvious locals hotspots where routing locals are embedded into other locals structs

This is a fast repo-local safety net, not a replacement for a real core-lite compile.

### Prerequisites

- [qubic/core](https://github.com/qubic/core) build environment
- Google Test (for unit tests)
- C++17 compiler

### Unit Tests

```bash
g++ -std=c++17 -I. contract_qugate.cpp -lgtest -lgtest_main -o qugate_tests
./qugate_tests
```

The test suite (`contract_qugate.cpp`) contains 73 unit tests covering:
- All 8 active gate modes (split even/uneven/rounding, round-robin cycling, threshold accumulation/release, random selection, conditional whitelist/bounce, heartbeat dead-man's switch, M-of-N multisig approval, epoch-based time lock)
- Chain gates (hop fees, chain reserve, depth limits, cycle detection)
- Versioned gate IDs and sendToGateVerified
- Anti-spam features (escalating fees at 0/1024/2048 gates, dust burn, excess fee to reserve, gate expiry with balance refund, activity epoch tracking)
- Error cases (invalid gate ID, unauthorized close/update, insufficient fee, invalid mode/ratio/threshold/sender count)
- Free-list slot reuse
- totalBurned tracking

### Testnet (Core-Lite)

To run end-to-end tests against a live node:

1. **Clone and build core-lite** with QuGate registered:

```bash
git clone https://github.com/qubic/core-lite
cd core-lite

# Copy QuGate.h and set index
cp /path/to/QuGate.h src/contracts/QuGate.h
sed -i 's/#define CONTRACT_INDEX 25/#define CONTRACT_INDEX 26/' src/contracts/QuGate.h
```

**Required patches** (6 files — see `QUGATE-TESTNET-NODE.md` for full details):

1. `lib/platform_efi/uefi.h` — add `#define __cdecl` for Linux
2. `src/extensions/overload.h` — add `OutputString` stub
3. `src/contract_core/contract_exec.h` — graceful null function check (replaces crashing ASSERT)
4. `src/extensions/http/controller/rpc_live_controller.h` — 503 race condition fix for RPC thread safety
5. `src/public_settings.h` — set `EPOCH 1`, `TICK 0`
6. `src/contract_core/contract_def.h` — register QuGate at index 26
7. `src/contract_core/qpi_oracle_impl.h` — add `typename` for dependent type (upstream Clang bug)

**Build** (must use Clang — GCC missing BMI flags in upstream CMakeLists):

```bash
mkdir -p build && cd build
CC=clang CXX=clang++ cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DTESTNET=ON \
    -DTESTNET_PREFILL_QUS=ON
make -j$(nproc) Qubic
```

2. **Start the testnet node**:

```bash
# Stop any miner first — node needs ~27GB RAM
systemctl stop qlab 2>/dev/null

./build/src/Qubic --sm 3 --ticking-delay 1500  # HTTP RPC on port 41841
# Verify: curl http://localhost:41841/v1/latest-stats
```

3. **Run the tests** (see `tests/README.md` for full details):

```bash
export QUBIC_CLI=/path/to/qubic-cli
python3 tests/test_all_modes.py        # All 9 modes (25+ checks)
python3 tests/test_heartbeat.py        # HEARTBEAT mode (10 tests, epoch-dependent)
python3 tests/test_multisig.py         # MULTISIG mode (10 tests)
python3 tests/test_stress_50gates.py   # 50-gate stress test
python3 tests/test_attack_vectors.py   # Security edge cases
```

### Testnet Results

Tested on Qubic Core-Lite v1.283.0 (local testnet, 2026-04-03):
- **128/128 integration scenarios passing** across 8 parallel wallet lanes
- All 8 active gate modes verified: SPLIT, ROUND_ROBIN, THRESHOLD, RANDOM, CONDITIONAL, HEARTBEAT, MULTISIG, TIME_LOCK
- Full governance lifecycle: admin gate attachment, approval windows, governed mutations, expiry recovery
- Complete lifecycle coverage: idle charging, delinquency, expiry, stale-ID rejection, slot reuse
- Capacity tests: 8-recipient splits, zero-ratio skip, large-amount precision, gate-as-recipient mix
- Concurrency tests: parallel senders to same gate, fee escalation, maintenance charge accounting
- Attack/edge cases: unauthorized close, send-to-closed, dust burn, chain depth limit, double close, slot reuse
- Node stable throughout entire multi-hour run on fresh state

See `TESTNET_RESULTS.md` for detailed results.

---

## Known Limitations

1. **RANDOM mode is not cryptographically random.** It uses `tick()` as entropy, which provides unpredictability but not cryptographic guarantees. Not suitable for high-stakes randomness where manipulation resistance is critical.

2. **THRESHOLD mode is vulnerable to grief-by-dust.** An attacker can send many small amounts (above minSend) to slowly fill a threshold gate. This is a nuisance, not exploitable — the attacker loses QU on every send. The dust burn minimum mitigates the cheapest form of this attack.

3. **getGatesByOwner is O(n) linear scan.** It iterates all gate slots to find gates by owner. At 256 slots (X_MULTIPLIER=1) this is acceptable; at higher X_MULTIPLIER values it may become slow. Limited to 16 results.

4. **END_EPOCH expiry iterates all gates.** The expiry check runs every epoch and scans all gate slots. At high capacity this adds per-epoch computation. Lazy expiry on interaction handles most cases, reducing the practical load, but the full scan remains as a safety net.

5. **Shareholder governance is not yet wired.** Fee parameters are set at initialization and cannot be changed until `DEFINE_SHAREHOLDER_PROPOSAL_STORAGE` is enabled with a valid contract asset name.

6. **Maximum 8 recipients per gate.** Larger distributions can be achieved by chaining multiple SPLIT gates via chain gates (automatic forwarding, max 3 hops) or manual intermediary forwarding.

7. **Mode is immutable after creation.** To change a gate's mode, you must close it and create a new one.

8. **Slot exhaustion is expensive but possible.** A sufficiently motivated attacker willing to pay escalating fees and keep gates active (preventing expiry) could occupy all slots. This is a fundamental property of permissionless systems. The mitigations make it expensive, temporary, and self-healing — but not impossible.

9. **No refund mechanism for THRESHOLD below target.** If a THRESHOLD gate never reaches its target, funds are held until the owner closes the gate or the gate expires. There is no "cancel and refund senders" mechanism — held balance goes to the gate owner.

10. **Gate-as-recipient routing through chain hops.** When a gate-recipient target is accessed through chain forwarding (depth 2+), deferred dispatch is bounded to one level of nesting. Deeply nested gate-recipient chains (gate A → chain to gate B → gate-recipient gate C → gate-recipient gate D) may not fully propagate beyond 2 hops. Direct sends to gates with gate-recipients work correctly at any depth up to `MAX_CHAIN_DEPTH`.

---

## File Listing

| File | Description |
|------|-------------|
| `QuGate.h` | Contract source code (QPI-compliant, ~6700 lines) |
| `contract_qugate.cpp` | Unit test suite (73 tests, Google Test, Allman brace style) |
| `README.md` | Technical reference (this file) |
| `TESTNET_RESULTS.md` | Testnet verification results |
| `tests/` | Python integration test scripts (18 scripts, require live testnet node) |
| `tests/conftest.py` | Pytest config — skips integration tests when no node available |
| `.github/workflows/` | CI: contract verification, style lint, integration tests |

---

## QPI Compliance

QuGate is written to pass `qubic-contract-verify`. The `#ifndef CONTRACT_INDEX` preprocessor guard was removed; testnet builds pass the index via cmake flags (`-DCONTRACT_INDEX=26`).

| Requirement | Status |
|-------------|--------|
| No C-style arrays in state structs | `Array<T, N>` throughout; `.get()` / `.set()` access |
| No division operator (`/`) | All division via `QPI::div()` |
| No modulo operator (`%`) | All modulo via `QPI::mod()` |
| No double quotes, single quotes in source | Clean |
| No square bracket access (`[]`) | `.get()` / `.set()` only |
| `state.get()` / `state.mut()` dirty-tracking | Used exclusively |
| No `double`, `float`, `typedef`, `union` | Not used |
| No double underscores in identifiers | Not used |

**Contract index**: The preprocessor guard was removed for contract-verify compliance. Testnet builds pass `-DCONTRACT_INDEX=26` via cmake flags instead.

---

## Roadmap

- **Mainnet deployment** — pending computor vote on [Proposal PR #33](https://github.com/qubic/proposal/pull/33)
- **Ecosystem tooling** — SDK helpers, explorer integration, documentation site
- **Community adoption** — gather feedback from builders, iterate on governance parameters

This is intentionally small. QuGate is a primitive — the roadmap is driven by what the ecosystem needs, not feature bloat.

---

## Related Tools

### qubic-mcp — Model Context Protocol Server for Qubic

**Repository**: [github.com/fyllepo/qubic-mcp](https://github.com/fyllepo/qubic-mcp)

A Qubic RPC tool built by the same author. It provides a standardised interface for querying the Qubic network, including smart contract state. Useful for:

- **Testing QuGate**: Point it at a core-lite testnet RPC, register QuGate's contract schema, and query gate state directly — it handles base64 encoding/decoding and struct parsing automatically
- **Integration development**: Build tools and dashboards on top of QuGate without manually constructing RPC payloads
- **Network switching**: Supports `add_network` / `switch_network` to flip between mainnet and testnet endpoints

Actively maintained. Contributions welcome.

---

## License

To be determined — intended for contribution to the Qubic ecosystem.
