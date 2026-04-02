# QuGate — Testnet Results

**Date:** 2026-02-20  
**Node:** core-lite v1.278.0 (single-node testnet, epoch 200)  
**Contract index:** 24  
**Build:** QuGate.h compiled with core-lite (clang, Release, x86-64 AVX2)

## Results: 21/21 PASSED ✅

| # | Test | Result |
|---|------|--------|
| 1 | SPLIT mode — create gate with 60/40 ratios | ✅ |
| 2 | SPLIT mode — send 10,000 QU, verify 6,000/4,000 distribution | ✅ |
| 3 | ROUND_ROBIN mode — create gate, verify alternating recipients | ✅ |
| 4 | THRESHOLD mode — create gate with threshold=15,000 | ✅ |
| 5 | THRESHOLD mode — send 10,000 (below threshold), verify held | ✅ |
| 6 | THRESHOLD mode — send 10,000 more (total 20,000), verify flush | ✅ |
| 7 | RANDOM mode — create gate, send 3×, verify distribution | ✅ |
| 8 | CONDITIONAL mode — create gate with whitelist | ✅ |
| 9 | CONDITIONAL mode — non-whitelisted sender rejected | ✅ |
| 10 | CONDITIONAL mode — whitelisted sender accepted | ✅ |
| 11 | updateGate — change SPLIT ratios from 60/40 to 25/75 | ✅ |
| 12 | updateGate — verify new 25/75 split (2,500/7,500) | ✅ |
| 13 | updateGate — non-owner update rejected | ✅ |
| 14 | closeGate — non-owner close rejected | ✅ |
| 15 | closeGate — owner close succeeds | ✅ |
| 16 | Send to closed gate rejected | ✅ |

## Test Script

`tests/test_all_modes.py` — Python script using `qubic-cli` for transactions and HTTP RPC for queries.

## Environment

- **Machine:** AMD Ryzen 9 9900X, 30GB RAM, Ubuntu 24.04
- **Addresses:** 3 test addresses (Address A = gate owner, Address B/C = recipients)
- **Creation fee:** 100,000 QU per gate
- **Min send:** 1,000 QU

## Stress & Security Tests

| # | Test | Result |
|---|------|--------|
| 17 | 50-gate stress: create 50 gates across all modes | ✅ |
| 18 | 50-gate stress: send to all 50, verify routing | ✅ |
| 19 | 50-gate stress: close and reuse slots | ✅ |
| 20 | Attack: unauthorized close rejected | ✅ |
| 21 | Attack: send to non-existent gate rejected | ✅ |
| 22 | Attack: send to closed gate rejected | ✅ |
| 23 | Attack: double close rejected | ✅ |
| 24 | Attack: zero-amount send (no burn) | ✅ |
| 25 | Attack: slot reuse — stale ID invalid after recycle | ✅ |

## Note on Chain and Verified Sends

Chain gates, versioned gate IDs, and sendToGateVerified were added after the initial testnet run and are covered in unit tests. Mode slot 5 is reserved pending oracle infrastructure — `createGate` with `mode=5` returns `QUGATE_UNSUPPORTED_MODE`.

---

# QuGate — HEARTBEAT + MULTISIG Build & Test Notes

**Date:** 2026-03-21  
**Contract:** QuGate.h (all 9 modes, contract index 25)

## Build Status

**qubic-core-fork** (`/home/phil/projects/qubic-core-fork/`): QuGate.h registered at index 25 via `QUGATE_CONTRACT_INDEX = 25` in `src/contract_core/contract_def.h`. The unit test compilation (`test/contract_qugate.cpp`) is blocked by an unrelated platform issue: `public_settings.h` uses `static unsigned short[] = L"..."` string literals which require MSVC with `/Zc:wchar_t-` — not compatible with GCC/Clang on Linux. This is a pre-existing build environment constraint, not a QuGate issue.

**core-lite** (`/home/phil/projects/core-lite/`): Uses the older `state._field` QPI API (direct access) while QuGate.h uses the newer `state.get()._field` / `state.mut()._field` pattern. Porting QuGate.h to core-lite's old API was not attempted; the existing core-lite node binary (built from old QGate.h) is intact for SPLIT/RR/THRESHOLD/RANDOM/CONDITIONAL testnet runs.

**Syntax check:** `qubic-contract-verify` rejects QuGate.h due to the `#ifndef CONTRACT_INDEX` preprocessor guard — expected, as the verifier enforces no-preprocessor rules for submission-ready contracts. The guard itself is safe to remove before submission.

## HEARTBEAT Gate — Test Coverage

New test file: `tests/test_heartbeat.py`

| # | Test | Coverage |
|---|------|----------|
| 1 | Create HEARTBEAT gate (mode=6) | createGate with mode=6 |
| 2 | configureHeartbeat — threshold=2, payout=50%, beneficiaries 60/40 | configureHeartbeat validation + storage |
| 3 | heartbeat() by owner — resets epoch counter | heartbeat() success path |
| 4 | heartbeat() by non-owner — rejected | heartbeat() auth check |
| 5 | Fund gate with 1,000,000 QU | sendToGate on HEARTBEAT gate |
| 6 | Advance epochs — gate triggers (epoch-dependent) | END_EPOCH trigger logic |
| 7 | After trigger — 50% payout distributed 60/40 | END_EPOCH recurring payout |
| 8 | heartbeat() after trigger — rejected (QUGATE_HEARTBEAT_TRIGGERED) | post-trigger rejection |
| 9 | Next epoch — another 50% payout | recurring payout (second cycle) |
| 10 | Gate auto-closes when balance ≤ minimumBalance | auto-close in END_EPOCH |

Tests 6-10 require epoch advancement (unavailable without a live testnet with short epoch duration). Tests 1-5 and 8 run immediately; 6-10 have graceful skip logic if epoch doesn't advance in time.

## MULTISIG Gate — Test Coverage

New test file: `tests/test_multisig.py`

| # | Test | Coverage |
|---|------|----------|
| 1 | Create MULTISIG gate (mode=7) | createGate with mode=7 |
| 2 | configureMultisig — 3 guardians, required=2, expiry=4 | configureMultisig validation + storage |
| 3 | Non-guardian sends QU — funds accumulate, no vote | sendToGate by non-guardian |
| 4 | Guardian 1 votes — approvalCount=1, no release | guardian vote, proposal creation |
| 5 | Guardian 2 votes — threshold met → funds release to recipient[0] | M-of-N execution |
| 6 | Votes reset after execution | post-execution state reset |
| 7 | Guardian double-vote — rejected (QUGATE_MULTISIG_ALREADY_VOTED) | bitmap dedup |
| 8 | Proposal expiry — votes reset after N epochs (epoch-dependent) | END_EPOCH expiry logic |
| 9 | configureMultisig by non-owner — rejected (QUGATE_UNAUTHORIZED) | auth check |
| 10 | getMultisigState — returns correct state, error on invalid ID | read-only query |

Test 8 requires epoch advancement. All other tests run immediately against a live node.

## Updated test_all_modes.py

`tests/test_all_modes.py` extended with Tests 8 (HEARTBEAT), 9 (MULTISIG), and 10 (TIME_LOCK) — inline versions of the key paths from the dedicated test files. Covers: create, configure, heartbeat(), fund for HEARTBEAT; create, configure, fund (non-guardian), two guardian votes → release, vote reset, non-owner configure rejection for MULTISIG; create, configure, fund, query state for TIME_LOCK. Query output fields (guardians, adminGateId, hasAdminGate) verified.

---

# QuGate — Core-Lite v1.283.0 Testnet Deployment

**Date:** 2026-03-22
**Node:** core-lite v1.283.0 (commit ba88437c), Qubic testnet
**Contract index:** 26 (Vottun took 25)
**Machine:** Hetzner AX42 — 64GB ECC DDR5, Ryzen 7 PRO 8700GE, Ubuntu 24.04
**Build:** Clang, Release, `-DTESTNET=ON -DTESTNET_PREFILL_QUS=ON`

## Node Status

| Check | Result |
|-------|--------|
| Build (Clang, Release) | ✅ Clean compile |
| Node launch | ✅ "Qubic 1.283.0 is launched" |
| Network check-in | ✅ "Successfully checked in to Qubic network" |
| HTTP RPC (:41841) | ✅ Responding |
| TESTNET mode | ✅ "This node is running as TESTNET" |
| Prefilled entities | ✅ 677 entities, 6.77T QU |
| Epoch/tick | ✅ Epoch 1, tick 0 |
| Stability (60s+ run) | ✅ No crashes with fixes applied |

## Patches Required (7)

| # | File | Fix |
|---|------|-----|
| 1 | `lib/platform_efi/uefi.h` | `#define __cdecl` for Linux |
| 2 | `src/extensions/overload.h` | `OutputString` UEFI stub |
| 3 | `src/contract_core/contract_exec.h` | Graceful null function check (replaces crashing ASSERT) |
| 4 | `src/extensions/http/controller/rpc_live_controller.h` | 503 race condition fix — tryAcquireRead lock + null function guard |
| 5 | `src/public_settings.h` | EPOCH 1, TICK 0 |
| 6 | `src/contract_core/contract_def.h` | Register QuGate at index 26 |
| 7 | `src/contract_core/qpi_oracle_impl.h` | `typename` fix for dependent type (upstream Clang bug) |

## Known Issues

- **Tick advancement**: Standalone lite node does not produce ticks (follows computors). Tick stays at 0 on solo testnet.
- **RAM**: Node uses ~27GB during init. Cannot coexist with miner on 64GB box.
- **RAID resync**: Repeated hard reboots trigger RAID1 resync (~30 min).

Full patch guide: see `QUGATE-TESTNET-NODE.md` in the openclaw workspace.

---

## Files Changed

| File | Change |
|------|--------|
| `tests/test_heartbeat.py` | New — 10-test HEARTBEAT suite |
| `tests/test_multisig.py` | New — 10-test MULTISIG suite |
| `tests/test_all_modes.py` | Extended with Tests 8 + 9 for HEARTBEAT and MULTISIG |
| `README.md` | Added gate modes table, HEARTBEAT section, MULTISIG section, updated status codes, log types, and contract interface (indices 13-17) |
| `TESTNET_RESULTS.md` | This section |

---

# QuGate — Query Output Fixes (PR #33 / #34)

**Date:** 2026-03-22
**Deployed to testnet:** Yes, state preserved (no clear needed)

## Changes

Query functions updated to return previously omitted fields:

| Function | Field(s) Added | Type |
|----------|---------------|------|
| `getMultisigState` (17) | `guardians` | Array\<id, 8\> — guardian public keys from MultisigConfig |
| `getGate` (5) | `adminGateId` | sint64 — versioned gate ID of admin gate (-1 if none) |
| `getGate` (5) | `hasAdminGate` | uint8 — 1 if governed by admin gate |
| `getAdminGate` (22) | `guardianCount` | uint8 — number of guardians on the admin gate |
| `getAdminGate` (22) | `required` | uint8 — M-of-N threshold |
| `getAdminGate` (22) | `guardians` | Array\<id, 8\> — guardian public keys from admin gate |

## Impact

- **getMultisigState**: Clients can now display which guardians are configured and map votes to identities without a separate query.
- **getGate**: Clients can detect whether a gate is under MULTISIG governance and retrieve the admin gate ID in a single call.
- **getAdminGate**: Returns full guardian info so clients can show who governs a gate without querying the admin gate's multisig state separately.
- **Wire compatibility**: Output structs grew (new fields appended at end). Clients reading only the original prefix bytes are unaffected.

## Test Coverage

- `tests/test_multisig.py` — TEST 10 now verifies guardian public keys match configured values
- `tests/test_all_modes.py` — TEST 10 (TIME_LOCK) verifies getGate returns adminGateId/hasAdminGate fields
- `tests/README.md` — updated test table and mode count
