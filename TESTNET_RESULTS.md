# QuGate Testnet Results

**Date**: 2026-02-18  
**Node**: e01 (Electron-01), core-lite testnet, epoch 200  
**Contract**: QuGate index 24 (`YAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAMSME`)  
**Ticking delay**: 5000ms (~30s effective tick rate under load)

## Summary

| Mode | Result | Notes |
|------|--------|-------|
| SPLIT | ✅ PASS | Perfect 60/40 distribution |
| ROUND_ROBIN | ✅ PASS | Correct cycling logic |
| THRESHOLD | ✅ PASS | Accumulation + trigger + reset |
| RANDOM | ✅ PASS | Tick-based entropy, both recipients received |
| CONDITIONAL | ✅ PASS | Whitelist enforced, unauthorized rejected |

## Detailed Results

### SPLIT Mode ✅

**Setup**: 60% → Address B, 40% → Address C

| Payment | Amount | Address B Gained | Address C Gained | Status |
|---------|--------|-------------|-------------|--------|
| #1 | 10,000 QU | +6,000 (60%) | +4,000 (40%) | ✅ Perfect |
| #2 | 50,000 QU | +30,000 (60%) | +20,000 (40%) | ✅ Perfect |

Gate closed successfully. Total forwarded: 60,000 QU.

### ROUND_ROBIN Mode ✅

**Setup**: 2 recipients (Address B, Address C), 3 payments of 10,000 QU each

| Payment | Recipient | Amount |
|---------|-----------|--------|
| #1 | Address B | 10,000 QU |
| #2 | Address C | 10,000 QU |
| #3 | Address B | 10,000 QU |

**Result**: Address B gained 20,000 QU, Address C gained 10,000 QU  
Proper Address B→Address C→Address B round-robin cycling ✅

### THRESHOLD Mode ✅

**Setup**: Threshold = 25,000 QU, 1 recipient (Address B)

| Step | Payment | Balance | Forwarded | Status |
|------|---------|---------|-----------|--------|
| 1 | 10,000 QU | 10,000 | 0 | ✅ Accumulated |
| 2 | 10,000 QU | 20,000 | 0 | ✅ Still accumulating |
| 3 | 5,000 QU | 0 | 25,000 | ✅ Threshold hit! |
| 4 | 10,000 QU | 10,000 | 25,000 | ✅ Re-accumulating |

Gate closed successfully. Threshold trigger and reset both verified.

### RANDOM Mode ✅

**Setup**: 2 recipients (Address B, Address C), 6 payments of 10,000 QU each

| Payment | Tick | Recipient |
|---------|------|-----------|
| #1 | 43910691 | Address C |
| #2 | 43910706 | Address B |
| #3 | 43910721 | Address C |
| #4 | 43910736 | Address C |
| #5 | 43910751 | Address B |
| #6 | 43910753 | Address C |

**Result**: Address B = 20,000 QU (33%), Address C = 40,000 QU (66%)  
Uses `qpi.tick()` for entropy — both recipients received funds ✅

### CONDITIONAL Mode ✅

**Setup**: Recipient = Address B, Allowed sender = Address A only

| Step | Sender | Amount | Forwarded? | Status |
|------|--------|--------|-----------|--------|
| Allowed send | Address A | 10,000 QU | ✅ Yes → Address B | Correct |
| Unauthorized send | Address C | 10,000 QU | ❌ No (rejected) | Correct — Address C didn't lose funds |
| Allowed send after rejection | Address A | 5,000 QU | ✅ Yes → Address B | Still works after unauthorized attempt |

Gate closed successfully. Total forwarded: 15,000 QU (all from Address A).

## Multi-Sender Convergence Test ✅

3 different addresses all sent to the same SPLIT gate (50/50 → Address A, Address C).

| Step | Sender | Amount | Result |
|------|--------|--------|--------|
| Address A sends (sender = recipient) | Address A | 10,000 QU | Address A net -5k (sent 10k, got 5k back), Address C +5k ✅ |
| Address B sends (owner, not recipient) | Address B | 20,000 QU | Address A +10k, Address C +10k ✅ |
| Address C sends (recipient = sender) | Address C | 8,000 QU | Address A +4k, Address C net -4k ✅ |

**Total**: 38,000 QU from 3 senders, all correctly routed.  
**Verified**: Sender-as-recipient ✅, Owner-as-sender ✅, Recipient-as-sender ✅

## Full Gate Lifecycle Test ✅ (with findings)

Tested the complete lifecycle: Create → Send → Update → Send → Verify → Close

| Step | Result |
|------|--------|
| Create SPLIT 60/40 | ✅ Gate created, ratios confirmed |
| Send 10k (verify 60/40) | ✅ Address B +6k, Address C +4k — perfect |
| Update ratios to 20/80 | ⚠️ **Ratios did not change** — still 60/40 |
| Send 10k (verify new split) | ⚠️ Still 60/40 — confirms update didn't apply |
| Non-owner update attempt | ✅ Rejected — ratios unchanged |
| Update mode SPLIT→ROUND_ROBIN | ⚠️ **Mode did not change** — still SPLIT |
| Close gate | ✅ Closed successfully |

**Key Finding**: `updateGate` transactions are accepted by the node but **do not modify gate state**. This likely indicates the `updateGate_input` struct has a different layout than expected. **This needs investigation before mainnet** — the update functionality exists in the contract code but the wire format needs to be verified.

**Note**: This is not a security issue — the gate correctly rejects unauthorized updates and continues functioning with its original configuration.

## Gate Chaining POC ✅

**Pipeline**: Address A → Gate A (SPLIT 70/30) → Address B → Gate B (THRESHOLD 15k) → Address C

- Gate A correctly split 40k QU: 28k (70%) to Address B, 12k (30%) to Address C
- Address B chained 28k into Gate B across 2 rounds (14k + 14k)
- Gate B accumulated 14k (below threshold), then triggered at 28k → forwarded all to Address C
- Total to Address C: 40k QU (12k direct + 28k chained)
- Proves composable multi-gate payment pipelines work ✅

## 50-Gate Stress Test ✅

**Config**: 50 gates attempted across all 5 modes (15 SPLIT, 10 ROUND_ROBIN, 10 THRESHOLD, 10 RANDOM, 5 CONDITIONAL)

| Phase | Result |
|-------|--------|
| Create | 29/50 gates created (nonce collisions dropped 21 rapid-fire txs) |
| Send | 29/29 transactions sent successfully (1000 QU each) |
| Verify | 10/10 sampled gates confirmed receiving funds |
| Close | 29/29 gates closed successfully |
| Slot reuse | 5/5 slots reused from free-list ✅ |

**Balance changes**: Address A -49,710 QU, Address B +10,320 QU, Address C +7,390 QU  
**Node stability**: ✅ STABLE throughout (4+ hours continuous operation, 20.5GB RSS flat)  
**Note**: 21 of 50 create transactions were dropped due to sending 5 txs from same address in rapid succession (nonce collision). This is a testnet CLI limitation, not a contract bug.

## Attack Vector Tests (7/7 pass) ✅

| Test | Result |
|------|--------|
| Unauthorized close | Rejected — gate stays active ✅ |
| Owner close | Succeeds ✅ |
| Non-existent gate send | Funds safe (0 QU lost) ✅ |
| Closed gate send | Funds safe (0 QU lost) ✅ |
| Double close | No crash, no state change ✅ |
| Zero-amount send | No effect ✅ |
| Gate slot reuse | Free-list works ✅ |

## Environment

- **Machine**: AMD Ryzen 9 9900X, 30GB RAM, Ubuntu 24.04
- **Memory**: Qubic node ~20.5GB RSS, stable throughout all tests
- **Failsafes**: Watchdog (15s checks), OOM score adj (Qubic=1000, sshd=-1000), 40GB swap, swappiness=100
- **Build**: core-lite, clang, Release, AVX512=OFF
