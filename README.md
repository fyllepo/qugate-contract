# QuGate — A Deflationary Network Primitive for Qubic

QuGate is a **network primitive** — shared, permissionless payment routing infrastructure for the Qubic network. One shared contract that the entire ecosystem can use, instead of every project building its own payment logic. All fees burned. Purely deflationary.

**Status**: Testnet verified. Preparing for mainnet proposal.
**Author**: fyllepo
**Repository**: [github.com/fyllepo/qugate-contract](https://github.com/fyllepo/qugate-contract)

---

## Table of Contents

1. [Overview](#overview)
2. [Gate Modes](#gate-modes)
3. [Contract Interface](#contract-interface)
4. [Wire Format](#wire-format)
5. [State Architecture](#state-architecture)
6. [Anti-Spam Mechanisms](#anti-spam-mechanisms)
7. [Fee Economics](#fee-economics)
8. [Shareholder Governance](#shareholder-governance)
9. [Design Decisions](#design-decisions)
10. [Status Codes](#status-codes)
11. [Log Types](#log-types)
12. [Building and Testing](#building-and-testing)
13. [Known Limitations](#known-limitations)
14. [File Listing](#file-listing)

---

## Overview

QuGate introduces **gates** — configurable routing nodes that automatically forward QU payments according to predefined rules. Each gate supports one of five modes. Gates are composable: the output of one gate can be the input of another, enabling arbitrarily complex payment pipelines without writing custom contracts.

### Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `QUGATE_MAX_GATES` | 4,096 x X_MULTIPLIER | Max concurrent gates (scales with network) |
| `QUGATE_MAX_RECIPIENTS` | 8 | Max recipients per gate |
| `QUGATE_MAX_RATIO` | 10,000 | Max ratio value per recipient |
| `QUGATE_DEFAULT_CREATION_FEE` | 100,000 QU | Initial base creation fee |
| `QUGATE_DEFAULT_MIN_SEND` | 1,000 QU | Initial minimum send amount |
| `QUGATE_FEE_ESCALATION_STEP` | 1,024 | Active gates per fee multiplier step |
| `QUGATE_DEFAULT_EXPIRY_EPOCHS` | 50 | Epochs of inactivity before auto-close |

---

## Gate Modes

### MODE_SPLIT (0) — Proportional Distribution

Distributes incoming payments to N recipients according to fixed ratios.

Each recipient's share is calculated as `(amount / totalRatio) * ratio`, with the last recipient receiving the remainder to avoid rounding loss. The overflow-safe formula used:

```
share = (amount / totalRatio) * ratio + ((amount % totalRatio) * ratio) / totalRatio
```

Ratios are relative, not percentages. Ratios of [3, 7] and [30, 70] produce identical splits.

**Validation**: Each ratio must be <= 10,000. Total ratio must be > 0. Recipient count 1-8.

### MODE_ROUND_ROBIN (1) — Rotating Distribution

Forwards each payment to the next recipient in sequence. An internal `roundRobinIndex` tracks position and wraps using `mod(index + 1, recipientCount)`.

Payment 1 goes to recipient 0, payment 2 to recipient 1, etc. After reaching the last recipient, it cycles back to recipient 0.

### MODE_THRESHOLD (2) — Accumulate and Forward

Holds incoming payments in `currentBalance` until the configured threshold is reached. When `currentBalance >= threshold`, the entire balance is transferred to recipient 0 and the balance resets to zero. The gate then begins accumulating again.

**Validation**: Threshold must be > 0.

If the gate is closed or expires while holding a balance, the held funds are refunded to the gate owner.

### MODE_RANDOM (3) — Probabilistic Distribution

Selects one recipient per payment using tick-based entropy:

```
recipientIdx = mod(totalReceived + tick(), recipientCount)
```

Where `totalReceived` is the gate's cumulative received amount (already incremented by the current payment) and `tick()` is the current Qubic tick number. The tick number is not controllable by the sender, providing sufficient unpredictability for payment routing, though this is not cryptographic randomness.

### MODE_CONDITIONAL (4) — Sender-Restricted Forwarding

Only forwards payments from addresses in the gate's `allowedSenders` list. Payments from unauthorized senders are bounced back (transferred back to the sender) with status `QUGATE_CONDITIONAL_REJECTED`.

When the sender is authorized, the full amount is forwarded to recipient 0.

**Validation**: `allowedSenderCount` must be <= 8.

---

## Contract Interface

### Procedures (State-Changing)

All procedures return a `status` field (sint64). Status 0 = success; negative values indicate errors (see [Status Codes](#status-codes)). Failed procedures refund any attached QU to the caller.

#### createGate (Input Type 1)

Creates a new gate. Requires payment of the current escalated creation fee.

**Input**:

| Field | Type | Size | Description |
|-------|------|------|-------------|
| mode | uint8 | 1 | Gate mode (0-4) |
| recipientCount | uint8 | 1 | Number of recipients (1-8) |
| recipients | Array\<id, 8\> | 256 | Recipient public keys (32 bytes each) |
| ratios | Array\<uint64, 8\> | 64 | Ratio per recipient (SPLIT mode) |
| threshold | uint64 | 8 | Threshold amount (THRESHOLD mode) |
| allowedSenders | Array\<id, 8\> | 256 | Allowed sender pubkeys (CONDITIONAL mode) |
| allowedSenderCount | uint8 | 1 | Number of allowed senders (0-8) |

**Output**:

| Field | Type | Description |
|-------|------|-------------|
| status | sint64 | 0 on success, negative on error |
| gateId | uint64 | Assigned gate ID (1-indexed) |
| feePaid | uint64 | Actual fee burned (after escalation) |

Wire size: ~600 bytes (input), 24 bytes (output). Exact padding depends on platform alignment.

**Behaviour**: Validates all parameters. Burns the escalated creation fee. Refunds any overpayment. Allocates a slot from the free-list if available, otherwise from the end of the array. Mode is immutable after creation.

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

**Behaviour**: Refunds any held balance (THRESHOLD mode) to the owner. Marks the gate inactive. Pushes the slot onto the free-list for reuse. Refunds any attached invocation reward.

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

**Output**:

| Field | Type | Description |
|-------|------|-------------|
| status | sint64 | 0 on success, negative on error |

Wire size: ~608 bytes (input). 8 bytes larger than createGate due to gateId replacing mode.

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

---

## Wire Format

All data is little-endian. Public keys (`id`) are 32 bytes. `Array<T, N>` is serialized as N contiguous elements with no length prefix.

### createGate_input Layout (~600 bytes)

```
Offset  Size   Field
------  -----  -----
0       1      mode (uint8)
1       1      recipientCount (uint8)
+pad    ...    (compiler alignment padding to Array boundary)
        256    recipients[8] (8 x 32-byte pubkeys)
        64     ratios[8] (8 x uint64)
        8      threshold (uint64)
        256    allowedSenders[8] (8 x 32-byte pubkeys)
        1      allowedSenderCount (uint8)
+pad    ...    (trailing padding)
```

### updateGate_input Layout (~608 bytes)

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

### GateConfig Structure (~400 bytes per gate)

```
owner               id (32 bytes)       Gate creator
mode                uint8               Gate mode (0-4, immutable)
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
```

### Free-List

Gate slots are reused via a stack-based free-list. When a gate is closed or expires, its slot index is pushed onto `_freeSlots`. When a new gate is created, the contract first pops from the free-list; only if empty does it allocate from the end of the array (incrementing `_gateCount`).

This prevents a permanent denial-of-service where an attacker creates and closes gates to exhaust the 4,096-slot array.

Total state size at X_MULTIPLIER=1: approximately 1.6 MB (4,096 gates x ~400 bytes + free-list overhead + metadata).

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
| 0 - 1,023 | 1x | 100,000 |
| 1,024 - 2,047 | 2x | 200,000 |
| 2,048 - 3,071 | 3x | 300,000 |
| 3,072 - 4,095 | 4x | 400,000 |

At X_MULTIPLIER=10 with 40,960 slots, the maximum multiplier would be 41x (4,100,000 QU). This makes bulk slot squatting progressively expensive.

Use `getFees()` to query the current escalated fee before creating a gate.

### Gate Expiry

Gates with no activity (no `sendToGate` or `updateGate`) for `_expiryEpochs` epochs are automatically closed during `END_EPOCH`. The expiry check uses:

```
if (epoch() - lastActivityEpoch >= expiryEpochs)  →  expire
```

Expired gates:
- Have any held balance (THRESHOLD mode) refunded to the owner
- Are marked inactive
- Have their slot pushed onto the free-list

Default expiry: 50 epochs (approximately 1 year at current epoch length). Set to 0 to disable expiry entirely. Shareholder-adjustable.

### Dust Burn

Sends below `_minSendAmount` (default: 1,000 QU) are burned via `qpi.burn()` and never forwarded. The sender receives status `QUGATE_DUST_AMOUNT`. Zero-amount sends return immediately with no burn.

---

## Fee Economics

QuGate is purely deflationary. All fees are burned via `qpi.burn()`:

- **Creation fees**: Burned on successful gate creation
- **Dust amounts**: Burned when sends are below minimum

No QU accumulates in the contract balance. No one profits from fees. The burned QU is permanently removed from circulation.

The `totalBurned` state variable tracks cumulative QU burned and is queryable via `getGateCount()`.

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

### Why 5 modes?

These five modes cover the most common payment routing patterns observed in blockchain applications: proportional splitting (revenue share), rotation (load balancing/fairness), accumulation (crowdfunding/escrow), randomization (lottery/raffle), and access control (whitelisting). Together they can be composed to handle most real-world payment flows without custom contracts.

### Why 8 max recipients?

8 recipients keeps per-gate state under 400 bytes while covering the vast majority of practical use cases (team payrolls, multi-way splits, worker pools). Larger recipient sets can be achieved by chaining SPLIT gates.

### Why ratio cap at 10,000?

Without a cap, `amount * ratio` could overflow uint64 for large payments combined with large ratios. Capping individual ratios at 10,000 ensures `amount * ratio` stays within uint64 range for any realistic payment amount. The overflow-safe division formula provides additional protection.

### Why immutable mode?

A gate's mode is set at creation and cannot be changed via `updateGate`. This prevents a class of attacks where a gate owner changes the mode after senders have configured their systems to expect specific routing behaviour. Recipients and ratios can still be updated, but the fundamental routing logic is fixed.

### Why a free-list?

Without slot reuse, an attacker could create and close 4,096 gates to permanently exhaust capacity. The free-list ensures closed gate slots are recycled. Combined with escalating fees, this makes the contract resilient to slot exhaustion attacks.

### Why burn, not revenue?

Accumulating fees in the contract would create a honeypot and complicate governance (who gets the revenue?). Burning is simpler, more secure, and benefits all QU holders equally through deflation.

### Why tick entropy for RANDOM?

`qpi.tick()` provides the only source of non-deterministic data available within the QPI. While not cryptographically random, the tick number is not controllable by the sender at the time of transaction submission, providing sufficient unpredictability for payment routing. The formula `mod(totalReceived + tick(), recipientCount)` ensures each payment produces a different selection even within the same tick.

---

## Status Codes

| Code | Constant | Description |
|------|----------|-------------|
| 0 | `QUGATE_SUCCESS` | Operation succeeded |
| -1 | `QUGATE_INVALID_GATE_ID` | Gate ID is 0 or exceeds gateCount |
| -2 | `QUGATE_GATE_NOT_ACTIVE` | Gate exists but is inactive |
| -3 | `QUGATE_UNAUTHORIZED` | Caller is not the gate owner |
| -4 | `QUGATE_INVALID_MODE` | Mode value > 4 |
| -5 | `QUGATE_INVALID_RECIPIENT_COUNT` | Recipient count is 0 or > 8 |
| -6 | `QUGATE_INVALID_RATIO` | Ratio > 10,000 or total ratio is 0 |
| -7 | `QUGATE_INSUFFICIENT_FEE` | Invocation reward < escalated creation fee |
| -8 | `QUGATE_NO_FREE_SLOTS` | No free-list slots and gateCount at capacity |
| -9 | `QUGATE_DUST_AMOUNT` | Send amount is 0 or below minimum |
| -10 | `QUGATE_INVALID_THRESHOLD` | Threshold is 0 for THRESHOLD mode |
| -11 | `QUGATE_INVALID_SENDER_COUNT` | Allowed sender count > 8 |
| -12 | `QUGATE_CONDITIONAL_REJECTED` | Sender not in allowed list (funds bounced) |

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

### Failure Events (high range)

| Value | Constant | Description |
|-------|----------|-------------|
| 100 | `QUGATE_LOG_FAIL_INVALID_GATE` | Invalid gate ID |
| 101 | `QUGATE_LOG_FAIL_NOT_ACTIVE` | Gate not active |
| 102 | `QUGATE_LOG_FAIL_UNAUTHORIZED` | Unauthorized caller |
| 103 | `QUGATE_LOG_FAIL_INVALID_PARAMS` | Invalid parameters |
| 104 | `QUGATE_LOG_FAIL_INSUFFICIENT_FEE` | Fee too low |
| 105 | `QUGATE_LOG_FAIL_NO_SLOTS` | No slots available |

### Log Structures

**QuGateLogger** (general events): `_contractIndex` (uint32), `_type` (uint32), `gateId` (uint64), `sender` (id), `amount` (sint64), `_terminator` (sint8).

**QuGateEventLogger** (detailed payment events): adds `recipient` (id) and `mode` (uint8) fields.

---

## Building and Testing

### Prerequisites

- [qubic/core](https://github.com/qubic/core) build environment
- Google Test (for unit tests)
- C++17 compiler

### Running Tests

```bash
cd tests/
g++ -std=c++17 -I../  ../contract_qugate.cpp -lgtest -lgtest_main -o qugate_tests
./qugate_tests
```

The test suite (`contract_qugate.cpp`) contains 40 unit tests covering:
- All 5 gate modes (split even/uneven/rounding, round-robin cycling, threshold accumulation/release, random selection, conditional whitelist/bounce)
- Anti-spam features (escalating fees at 0/1024/2048 gates, dust burn, fee overpayment refund, gate expiry with balance refund, activity epoch tracking)
- Error cases (invalid gate ID, unauthorized close/update, insufficient fee, invalid mode/ratio/threshold/sender count)
- Free-list slot reuse
- totalBurned tracking

### Testnet Results

Tested on Qubic Core-Lite (local testnet):
- All 5 modes verified with real contract execution
- 29 concurrent gates created and operated
- 7 attack vectors tested (unauthorized close, sends to non-existent/closed gates, double close, zero sends, slot reuse)
- 4+ hours continuous operation, zero memory growth

See `TESTNET_RESULTS.md` for detailed results.

---

## Known Limitations

1. **RANDOM mode is not cryptographically random.** It uses `tick()` as entropy, which provides unpredictability but not cryptographic guarantees. Not suitable for high-stakes randomness where manipulation resistance is critical.

2. **THRESHOLD mode is vulnerable to grief-by-dust.** An attacker can send many small amounts (above minSend) to slowly fill a threshold gate. This is a nuisance, not exploitable — the attacker loses QU on every send. The dust burn minimum mitigates the cheapest form of this attack.

3. **getGatesByOwner is O(n) linear scan.** It iterates all gate slots to find gates by owner. At 4,096 slots this is acceptable; at higher X_MULTIPLIER values it may become slow. Limited to 16 results.

4. **END_EPOCH expiry iterates all gates.** The expiry check runs every epoch and scans all gate slots. At high capacity this adds per-epoch computation.

5. **Shareholder governance is not yet wired.** Fee parameters are set at initialization and cannot be changed until `DEFINE_SHAREHOLDER_PROPOSAL_STORAGE` is enabled with a valid contract asset name.

6. **Maximum 8 recipients per gate.** Larger distributions require chaining multiple SPLIT gates.

7. **Mode is immutable after creation.** To change a gate's mode, you must close it and create a new one.

8. **Slot exhaustion is expensive but possible.** A sufficiently motivated attacker willing to pay escalating fees and keep gates active (preventing expiry) could occupy all slots. This is a fundamental property of permissionless systems. The mitigations make it expensive, temporary, and self-healing — but not impossible.

9. **No refund mechanism for THRESHOLD below target.** If a THRESHOLD gate never reaches its target, funds are held until the owner closes the gate or the gate expires. There is no "cancel and refund senders" mechanism — held balance goes to the gate owner.

---

## File Listing

| File | Description |
|------|-------------|
| `QuGate.h` | Contract source code (QPI-compliant) |
| `contract_qugate.cpp` | Test suite (40 unit tests, Google Test) |
| `README.md` | Technical reference (this file) |
| `PROPOSAL.md` | Deployment proposal for computor audience |
| `TODO.md` | Remaining tasks and roadmap |
| `TESTNET_RESULTS.md` | Testnet results |
| `CODE_REVIEW.md` | Code review notes |
| `FAILURE_LOG.md` | Test failure analysis |
| `tests/` | Python testnet test scripts |

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
