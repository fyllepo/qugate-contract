# QuGate Contract — Agent Instructions

## Project
QuGate is a Qubic blockchain smart contract — a programmable payment routing primitive with 8 active gate modes (slot 5 reserved). Single file: `QuGate.h`.

## Contract Index
**26** — hardcoded in the core-lite build at `contract_def.h`. NOT in QuGate.h (removed for contract-verify compliance). Testnet builds pass `-DCONTRACT_INDEX=26` via cmake.

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
- **Deferred routing pattern**: Mode processors can't call `routeToGate` directly (circular struct). They populate `deferredGateSlots/deferredGateAmounts` in output. Callers dispatch.
- **Chain forwarding**: `chainNextGateId` forwards remaining balance after distribution. Max 3 hops. All active modes supported.
- **Unified reserve**: Single `reserve` field per gate covers both chain hop fees and idle maintenance. Excess creation fee auto-seeds it. `fundGate(gateId)` tops it up.
- **Governed fee split**: `_feeBurnBps` state variable (default 5000 = 50%, range 3000-7000). Applied to creation fees, idle maintenance, heartbeat pings, and heartbeat config fees.
- **Complexity-based idle fees**: Base fee (25K) scaled by gate complexity: 1x simple, 1.5x for 3+ recipients/HEARTBEAT/MULTISIG, 2x for 8 recipients, +0.5x for chain links. Downstream drain and admin drain also use complexity multipliers.
- **Heartbeat pay-per-cycle**: `heartbeat()` charges the gate's full per-cycle maintenance cost (own idle fee + downstream drain + surcharge + admin drain). `configureHeartbeat()` charges threshold-scaled fee: `creationFee * (1 + thresholdEpochs / idleWindow)`.
- **Admin gate expiry exemption**: Admin-only multisigs that govern at least one active gate are exempt from expiry (scanned in expiry loop). Admin drain only refreshes activity on successful payment.
- **Transfer-first**: All `qpi.transfer()` calls check `>= 0` before mutating state. Tagged `[QG-01]` through `[QG-17]`.
- **invReward capture**: Every procedure captures `qpi.invocationReward()` into `locals.invReward` at entry.

## Struct Sizes (must match demo encoders)
| Struct | Size | Key offset |
|--------|------|------------|
| `createGate_input` | 672 bytes | `chainNextGateId` at 600, `recipientGateIds` at 608 |
| `updateGate_input` | 672 bytes | `recipientGateIds` at 608 (unchanged) |
| `getGate_output` | 776 bytes | `reserve` at 680, `recipientGateIds` at 712 |
| `fundGate_input` | 8 bytes | just `gateId` (no reserveTarget) |
| `withdrawReserve_input` | 16 bytes | `gateId` at 0, `amount` at 8 (no reserveTarget) |

## Error Codes
| Code | Constant | Added |
|------|----------|-------|
| -28 | `QUGATE_INVALID_GATE_RECIPIENT` | Gate-as-recipient validation |
| -29 | `QUGATE_INVALID_ADMIN_CYCLE` | Admin gate circular chain |
| -30 | `QUGATE_MULTISIG_PROPOSAL_ACTIVE` | configureMultisig blocked during active proposal |

## Critical Rules
1. **Never change struct layouts** without updating demo encoders in `fyllepo/qugate-demo` — `server/src/services/encoding.service.ts`
2. **All new `qpi.transfer()` calls** must check return `>= 0` before state mutation
3. **All new procedures** must capture `locals.invReward = qpi.invocationReward()` at entry
4. **Allman brace style** — opening brace on next line. CI lint enforces this.
5. **No preprocessor directives** (`#define`, `#ifndef`) — `qubic-contract-verify` rejects them
6. **Global scope names** must start with `QUGATE` (structs outside the main struct)
7. **`recipientCount=0` is valid** for HEARTBEAT, MULTISIG, TIME_LOCK modes
8. **Expiry exemptions**: TIME_LOCK (active+unfired), HEARTBEAT (active+untriggered), MULTISIG (has balance), admin-only MULTISIG (governs active gate) — exempt from inactivity expiry in END_EPOCH
9. **Drain fires per idle cycle** — admin drain and downstream reserve drain check `nextIdleChargeEpoch` to fire once per idle window, not every epoch
10. **configureHeartbeat charges after validation** — all validation (gate ID, auth, mode, params) completes before the threshold-scaled fee is charged. Rejected calls are fully refunded.

## Testing
- `contract_qugate.cpp` — 205 unit tests (Google Test, Allman style)
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
