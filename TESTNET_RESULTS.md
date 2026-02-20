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
