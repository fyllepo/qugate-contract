# QuGate Testnet Test Scripts

End-to-end tests that run against a live Qubic Core-Lite testnet node. Each script creates a gate, sends transactions, verifies results, and closes the gate.

## Prerequisites

- **Qubic Core-Lite** testnet running with QuGate registered at contract index 24
- **qubic-cli** built at `~/projects/qubic-cli/build/qubic-cli`
- **Python 3** with `requests` (`pip install requests`)
- Node listening on `localhost:31841` (binary) and `localhost:41841` (HTTP RPC)
- TESTNET mode enabled (3 addresses with 10B QU each)

## Test Scripts

| Script | Mode | What it tests |
|--------|------|---------------|
| `test_split.py` | SPLIT (0) | 60/40 ratio distribution, multiple payments, gate close |
| `test_round_robin.py` | ROUND_ROBIN (1) | Cycling through recipients across 3 payments |
| `test_threshold.py` | THRESHOLD (2) | Accumulation below threshold, trigger at threshold, reset |
| `test_random.py` | RANDOM (3) | 6 payments to verify tick-based distribution |
| `test_conditional.py` | CONDITIONAL (4) | Allowed sender forwards, unauthorized sender rejected |

## Running

```bash
# Start the testnet node first
cd ~/projects/core-lite/build/src
./Qubic --ticking-delay 5000 &

# Wait for tick > 43,910,000, then run tests
cd tests/
python3 test_split.py
python3 test_round_robin.py
python3 test_threshold.py
python3 test_random.py
python3 test_conditional.py
```

⚠️ **Node state resets on every restart.** Run all tests in one session — gates and balances don't persist across reboots.

## Configuration

All scripts use these defaults (edit at the top of each file):

```python
CLI = "/home/phil/projects/qubic-cli/build/qubic-cli"
RPC = "http://127.0.0.1:41841"
QUGATE_INDEX = 24
CONTRACT_ID = "YAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAMSME"
```

## Testnet Addresses

| Address | Identity | Balance |
|------|----------|---------|
| Address A | SINUBYSBZKBS... | 10B QU |
| Address B | KENGZYMYWOIH... | 10B QU |
| Address C | FLNRYKSGLGKZ... | 10B QU |

## Results

See [TESTNET_RESULTS.md](../TESTNET_RESULTS.md) for full test output and analysis.

| `test_testnet.py` | All modes | Core testnet verification |
| `test_all_modes.py` | All modes | All 5 modes + feature verification |
