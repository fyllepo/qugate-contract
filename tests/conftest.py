"""
Pytest conftest — skip integration tests when no testnet node is available.

All tests in this directory are integration tests that require a running
Qubic testnet node at http://127.0.0.1:41841. In CI (GitHub Actions),
no node is available, so we skip the entire test suite gracefully.
"""
import pytest
import requests


def _node_reachable():
    try:
        r = requests.get("http://127.0.0.1:41841/live/v1/tick-info", timeout=2)
        return r.status_code == 200
    except Exception:
        return False


# Cache the result once per session
_NODE_OK = None


def pytest_collection_modifyitems(config, items):
    global _NODE_OK
    if _NODE_OK is None:
        _NODE_OK = _node_reachable()

    if not _NODE_OK:
        skip_marker = pytest.mark.skip(
            reason="Testnet node not reachable at 127.0.0.1:41841 — skipping integration tests"
        )
        for item in items:
            item.add_marker(skip_marker)
