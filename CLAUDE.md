# QuGate Contract — Agent Instructions

## Project
QuGate is a Qubic blockchain smart contract — a programmable payment routing primitive with 9 gate modes. Single file: `QuGate.h` (~5800 lines).

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
| _(reserved)_ | 5 | Reserved — oracle mode pending infrastructure |
| HEARTBEAT | 6 | Dead-man's switch — distribute if no ping for N epochs |
| MULTISIG | 7 | M-of-N guardian approval before release |
| TIME_LOCK | 8 | Hold until unlock epoch |

## Key Architecture
- **Versioned gate IDs**: `gateId = ((generation+1) << 20) | slotIndex`. Prevents stale ID reuse.
- **Gate-as-Recipient**: `recipientGateIds[8]` array — recipients can be other gates. `-1` = wallet, `>= 0` = gate ID. Routed internally via `routeToGate()`.
- **Deferred routing pattern**: Mode processors can't call `routeToGate` directly (circular struct). They populate `deferredGateSlots/deferredGateAmounts` in output. Callers dispatch.
- **Chain forwarding**: `chainNextGateId` forwards remaining balance after distribution. Max 3 hops. All active modes supported.
- **Transfer-first**: All `qpi.transfer()` calls check `>= 0` before mutating state. Tagged `[QG-01]` through `[QG-17]`.
- **invReward capture**: Every procedure captures `qpi.invocationReward()` into `locals.invReward` at entry.

## Struct Sizes (must match demo encoders)
| Struct | Size | Key offset |
|--------|------|------------|
| `createGate_input` | 784 bytes | `recipientGateIds` at 720 |
| `updateGate_input` | 672 bytes | `recipientGateIds` at 608 |
| `getGate_output` | 784 bytes | `recipientGateIds` at 720 |

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
8. **Expiry exemptions**: TIME_LOCK (active+unfired), HEARTBEAT (active+untriggered), MULTISIG (has balance) — exempt from inactivity expiry in END_EPOCH

## Testing
- `contract_qugate.cpp` — 70 unit tests (Google Test, Allman style)
- `tests/` — 17 Python integration tests (require live testnet node at 127.0.0.1:41841)
- CI: style lint ✅, integration tests skip in CI ✅, contract-verify allows oracle template failure
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
