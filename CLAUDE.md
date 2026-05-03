# QuGate Contract — Agent Instructions

## Project
QuGate is a Qubic blockchain smart contract — a programmable payment routing primitive with 8 active gate modes (slot 5 reserved). Single file: `QuGate.h`.

## Contract Index
**28** — hardcoded in the core-lite build at `contract_def.h`. NOT in QuGate.h (removed for contract-verify compliance). Testnet builds pass `-DCONTRACT_INDEX=28` via cmake.

## Gate Modes
| Mode | ID | Description |
|------|----|-------------|
| SPLIT | 0 | Distribute by ratio to N recipients |
| ROUND_ROBIN | 1 | Cycle through recipients |
| THRESHOLD | 2 | Accumulate until target, then release |
| RANDOM | 3 | Select one recipient per payment (tick entropy, not cryptographic) |
| CONDITIONAL | 4 | Only forward from whitelisted senders |
| _(reserved)_ | 5 | Reserved for future use |
| HEARTBEAT | 6 | Dead-man's switch — distribute if no ping for N epochs |
| MULTISIG | 7 | M-of-N guardian approval before release |
| TIME_LOCK | 8 | Hold until unlock epoch |

## Key Architecture
- **Versioned gate IDs**: `gateId = ((generation+1) << 20) | slotIndex`. Prevents stale ID reuse.
- **Gate-as-Recipient**: `recipientGateIds[8]` array — recipients can be other gates. `-1` = wallet, `>= 0` = gate ID. Routed internally via `routeToGate()`.
- **Deferred routing pattern**: Mode processors can't call `routeToGate` directly (circular struct). They populate `deferredGateSlots/deferredGateAmounts` in output. Callers dispatch. Recovery: if routeToGate fails (target closed), undelivered amount returns to source gate's currentBalance.
- **Chain forwarding**: `chainNextGateId` forwards remaining balance after distribution. Max 3 hops. All active modes supported.
- **Unified reserve**: Single `reserve` field per gate covers both chain hop fees and idle maintenance. Excess creation fee auto-seeds it. `fundGate(gateId)` tops it up.
- **Funding source**: Per-gate `_fundingSourceGateIds` state array. `-1` = self-funded (default). When set via `setFundingSource`, idle fees are charged from the source gate's reserve instead of the gate's own reserve. Falls back to self-funded if source is inactive or has insufficient reserve. Refreshes `lastActivityEpoch` on funded gate to prevent inactivity expiry. Admin gate drain is separate (always from governed gate's reserve).
- **Governed fee split**: `_feeBurnBps` state variable (default 5000 = 50%, range 3000-7000). Applied to creation fees, idle maintenance, and heartbeat config fees.
- **Complexity-based idle fees**: Base fee (25K) scaled by gate complexity: 1x simple, 1.5x for 3+ recipients/HEARTBEAT/MULTISIG, 2x for 8 recipients, +0.5x for chain links.
- **Heartbeat ping**: `heartbeat()` charges flat 1,000 QU anti-spam fee. `configureHeartbeat()` charges threshold-scaled fee: `creationFee * (1 + thresholdEpochs / idleWindow)`.
- **Admin gate expiry exemption**: Admin-only multisigs that govern at least one active gate are exempt from expiry (scanned in expiry loop). Admin drain only refreshes activity on successful payment.
- **Transfer-first**: All `qpi.transfer()` calls check `>= 0` before mutating state. Tagged `[QG-01]` through `[QG-17]`.
- **invReward capture**: Every procedure captures `qpi.invocationReward()` into `locals.invReward` at entry.

## Procedures & Functions (27 total: 15 procedures + 12 functions)
| # | Name | Type | Fee |
|---|------|------|-----|
| 1 | `createGate` | Procedure | Escalated creation fee (burn/dividend split) |
| 2 | `sendToGate` | Procedure | Attached QU forwarded through gate |
| 3 | `closeGate` | Procedure | Free (refunds balance + reserve) |
| 4 | `updateGate` | Procedure | 1,000 QU anti-spam (100% burned) |
| 5 | `getGate` | Function | — |
| 6 | `getGateCount` | Function | — |
| 7 | `getGatesByOwner` | Function | — |
| 8 | `getGateBatch` | Function | — |
| 9 | `getFees` | Function | — |
| 10 | `fundGate` | Procedure | Free (all QU goes to reserve) |
| 11 | `setChain` | Procedure | 1,000 QU hop fee (100% burned) |
| 12 | `sendToGateVerified` | Procedure | Attached QU forwarded (owner-verified) |
| 13 | `configureHeartbeat` | Procedure | Threshold-scaled fee (burn/dividend split) |
| 14 | `heartbeat` | Procedure | 1,000 QU anti-spam (burn/dividend split) |
| 15 | `getHeartbeat` | Function | — |
| 16 | `configureMultisig` | Procedure | 1,000 QU anti-spam (100% burned) |
| 17 | `getMultisigState` | Function | — |
| 18 | `configureTimeLock` | Procedure | Duration-scaled fee (burn/dividend split) |
| 19 | `cancelTimeLock` | Procedure | 1,000 QU anti-spam (100% burned) |
| 20 | `getTimeLockState` | Function | — |
| 21 | `setAdminGate` | Procedure | 1,000 QU anti-spam (100% burned) |
| 22 | `getAdminGate` | Function | — |
| 23 | `withdrawReserve` | Procedure | Free (transfers from reserve to owner) |
| 24 | `getGatesByMode` | Function | — |
| 25 | `getGateBySlot` | Function | — |
| 26 | `getLatestExecution` | Function | — |
| 27 | `setFundingSource` | Procedure | 1,000 QU anti-spam (100% burned) |

## Struct Sizes (must match demo encoders)
| Struct | Size | Key offset |
|--------|------|------------|
| `createGate_input` | 672 bytes | `chainNextGateId` at 600, `recipientGateIds` at 608 |
| `updateGate_input` | 672 bytes | `recipientGateIds` at 608 (unchanged) |
| `getGate_output` | 784 bytes | `reserve` at 680, `recipientGateIds` at 712, `fundingSourceGateId` at 776 |
| `fundGate_input` | 8 bytes | just `gateId` (no reserveTarget) |
| `withdrawReserve_input` | 16 bytes | `gateId` at 0, `amount` at 8 (no reserveTarget) |
| `setFundingSource_input` | 16 bytes | `gateId` at 0, `fundingSourceGateId` at 8 |
| `heartbeat_output` | 20 bytes | `status` at 0, `epochRecorded` at 8, `feePaid` at 12 |

## Error Codes
| Code | Constant | Added |
|------|----------|-------|
| -28 | `QUGATE_INVALID_GATE_RECIPIENT` | Gate-as-recipient validation |
| -29 | `QUGATE_INVALID_ADMIN_CYCLE` | Admin gate circular chain |
| -30 | `QUGATE_MULTISIG_PROPOSAL_ACTIVE` | configureMultisig blocked during active proposal |
| -31 | `QUGATE_INVALID_PARAMS` | Generic invalid parameter |
| -32 | `QUGATE_INVALID_FUNDING_SOURCE` | Funding source gate invalid, closed, or owner mismatch |

## Critical Rules
1. **Never change struct layouts** without updating demo encoders in `fyllepo/qugate-demo` — `packages/qugate-sdk/src/encoding.ts`
2. **All new `qpi.transfer()` calls** must check return `>= 0` before state mutation
3. **All new procedures** must capture `locals.invReward = qpi.invocationReward()` at entry
4. **Allman brace style** — opening brace on next line. CI lint enforces this.
5. **No preprocessor directives** (`#define`, `#ifndef`) — `qubic-contract-verify` rejects them
6. **Global scope names** must start with `QUGATE` (structs outside the main struct)
7. **`recipientCount=0` is valid** for HEARTBEAT, MULTISIG, TIME_LOCK modes
8. **No inactivity expiry** — gates only expire via delinquency (can't pay idle fee → 4-epoch grace → expired). Paying idle fees from reserve counts as slot usage. Hold-state gates (HEARTBEAT, TIME_LOCK, MULTISIG, THRESHOLD) are exempt from idle fees while in hold. Admin-only multisigs governing active gates are exempt from expiry.
9. **Admin drain fires per idle cycle** — admin gate drain checks `nextIdleChargeEpoch` to fire once per idle window, not every epoch
10. **Anti-spam fees charge after validation** — all validation (gate ID, auth, mode, params) completes before the fee is burned. Rejected calls are fully refunded. No duplicate fee blocks.

## Testing
- `contract_qugate.cpp` — 236 unit tests (Google Test, Allman style)
- `tests/` — 18 Python integration test files, 132 scenarios (require live testnet node at 127.0.0.1:41841)
- CI: style lint ✅, integration tests skip in CI ✅
- Guard rails: `scripts/contract_guard.py` checks harness constant drift, public-function/private-procedure misuse, and warns on large locals hotspots

## Refactor Guard Rails
- Run `python3 scripts/contract_guard.py` before pushing structural changes to `QuGate.h`
- Dedupe is secondary to locals-size safety; avoid embedding large `*_locals` trees inside public `*_locals`
- Any refactor that touches routing/auth helpers should be validated against core-lite before it is treated as safe

## Files
| File | Purpose |
|------|---------|
| `QuGate.h` | Contract source (single file) |
| `contract_qugate.cpp` | Unit tests |
| `tests/` | Integration tests (need testnet) |
| `README.md` | Full technical reference |
| `TESTNET_RESULTS.md` | Testnet verification results |

## Commit Style
- Author: `Phil Elliott <phil@codeiq.co.uk>` (no co-authors)
- Conventional commits: `fix:`, `feat:`, `chore:`, `docs:`, `style:`
- Reference issue numbers: `Closes #XX`
