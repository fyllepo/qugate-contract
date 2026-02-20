# QuGate Testnet Test Scripts

End-to-end tests that run against a live Qubic Core-Lite testnet node. Each script creates gates, sends transactions, verifies results, and closes gates.

## Prerequisites

- **Qubic Core-Lite** testnet running with QuGate registered at contract index 24
- **qubic-cli** on your PATH (or set `QUBIC_CLI` env var)
- **Python 3** with `requests` (`pip install requests`)
- Node listening on `localhost:31841` (TCP) and `localhost:41841` (HTTP RPC)
- Testnet mode enabled (3 addresses with initial QU balance)

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `QUBIC_CLI` | `qubic-cli` (from PATH) | Path to qubic-cli binary |
| `QUBIC_ID_TOOL` | `identity_tool` (from PATH) | Path to identity_tool binary |

## Test Scripts

| Script | What it tests |
|--------|---------------|
| `test_all_modes.py` | All 5 modes + updateGate + closeGate + access control (21 checks) |
| `test_stress_50gates.py` | 50-gate stress test across all modes + slot reuse |
| `test_split.py` | SPLIT mode distribution |
| `test_round_robin.py` | ROUND_ROBIN cycling |
| `test_threshold.py` | THRESHOLD accumulation and trigger |
| `test_random.py` | RANDOM tick-based distribution |
| `test_conditional.py` | CONDITIONAL sender whitelist |
| `test_attack_vectors.py` | Security: unauthorized access, closed gates, edge cases |
| `test_gate_lifecycle.py` | Full lifecycle: create → send → update → close |
| `test_gate_chaining.py` | Multi-gate pipeline composability |
| `test_multi_sender.py` | Multiple senders to same gate |

## Running

```bash
# Set CLI path if not on PATH
export QUBIC_CLI=/path/to/qubic-cli

# Start the testnet node first, then run tests
python3 tests/test_all_modes.py
python3 tests/test_stress_50gates.py
```

⚠️ **Node state resets on every restart.** Run all tests in one session.

## Configuration

All scripts use these defaults:

```python
CLI = os.environ.get("QUBIC_CLI", shutil.which("qubic-cli") or "qubic-cli")
RPC = "http://127.0.0.1:41841"
CONTRACT_INDEX = 24
```

## Results

See [TESTNET_RESULTS.md](../TESTNET_RESULTS.md) for results.
