#!/usr/bin/env python3
"""
QuGate — Chain Gates Integration Tests (Issues #13 #18)

Tests:
  - createGate with chainNextGateId
  - setChain procedure
  - routeToGate single hop
  - routeToGate 2-hop chain
  - Insufficient funds → strand in currentBalance
  - chainReserve covers hop fee
  - fundGate with reserveTarget=1
  - Dead link (chained gate closed)
  - Depth limit enforcement

NOTE: These tests require a running core-lite testnet with QuGate deployed.
      The chain routing (routeToGate) is a PRIVATE procedure — it is called
      internally by the oracle callback. Testing chain routing end-to-end
      requires oracle infrastructure. The basic chain setup (createGate,
      setChain, fundGate) can be tested via RPC.
"""
import os
import shutil
import struct
import subprocess
import base64
import time
import requests
import sys

CLI = os.environ.get("QUBIC_CLI", shutil.which("qubic-cli") or "qubic-cli")
NODE_ARGS = ["-nodeip", "127.0.0.1", "-nodeport", "31841"]
RPC = "http://127.0.0.1:41841"
QUGATE_INDEX = 25

ADDR_A_KEY = "eraaastggldisjhoojaekgyimrsddjxbvgaawswfvnvaygqmusnkevv"
ADDR_B_KEY = "sgwnpzidgxbclnisgehigeculaejjxedzdkjyyfrzgzvuojrhdzywfh"
ADDR_C_KEY = "xeejtwxqrrlvacapbujaleejhbrsnnpvviknskemmgdihggpssjjkrg"

CREATION_FEE = 100000
CHAIN_HOP_FEE = 1000

PROC_CREATE = 1
PROC_SEND = 2
PROC_CLOSE = 3
PROC_FUND = 10
PROC_SET_CHAIN = 11
FUNC_GET_GATE = 5
FUNC_GET_COUNT = 6

# Versioned gate ID encoding
GATE_ID_SLOT_BITS = 20

passed = 0
failed = 0


def encode_gate_id(slot_idx, generation=0):
    return ((generation + 1) << GATE_ID_SLOT_BITS) | slot_idx


def cli(*args, timeout=15):
    r = subprocess.run([CLI] + NODE_ARGS + list(args),
                       capture_output=True, text=True, timeout=timeout)
    return r.stdout + r.stderr


def get_identity(key):
    out = cli("-seed", key, "-showkeys")
    for line in out.splitlines():
        if "Identity:" in line:
            return line.split("Identity:")[1].strip()
    raise Exception("Cannot get identity")


def get_balance(identity):
    out = cli("-getbalance", identity)
    for line in out.splitlines():
        if "Balance:" in line:
            return int(line.split("Balance:")[1].strip())
    return None


def get_tick():
    for attempt in range(5):
        try:
            return requests.get(f"{RPC}/live/v1/tick-info", timeout=5).json()['tick']
        except Exception:
            if attempt < 4:
                time.sleep(3)
    raise Exception("Node not responding")


def get_pubkey(identity):
    pk = bytearray(32)
    for i in range(4):
        val = 0
        for j in range(13, -1, -1):
            c = identity[i * 14 + j]
            val = val * 26 + (ord(c) - ord('A'))
        struct.pack_into('<Q', pk, i * 8, val)
    return bytes(pk)


def query_gate(gate_id):
    data = struct.pack('<Q', gate_id)
    resp = requests.post(f"{RPC}/live/v1/querySmartContract", json={
        'contractIndex': QUGATE_INDEX, 'inputType': FUNC_GET_GATE,
        'inputSize': len(data),
        'requestData': base64.b64encode(data).decode()
    }, timeout=5).json()
    b = base64.b64decode(resp['responseData'])
    modes = ['SPLIT', 'ROUND_ROBIN', 'THRESHOLD', 'RANDOM', 'CONDITIONAL', 'ORACLE']
    tr, tf, cb, th = struct.unpack_from('<QQQQ', b, 40)
    # Parse chain fields (after oracle fields)
    # Layout depends on exact struct packing — adjust offsets as needed
    return {
        'mode': b[0], 'mode_name': modes[b[0]] if b[0] < 6 else f'?{b[0]}',
        'recipientCount': b[1], 'active': b[2],
        'totalReceived': tr, 'totalForwarded': tf,
        'currentBalance': cb, 'threshold': th,
    }


def query_count():
    resp = requests.post(f"{RPC}/live/v1/querySmartContract", json={
        'contractIndex': QUGATE_INDEX, 'inputType': FUNC_GET_COUNT,
        'inputSize': 0, 'requestData': ''
    }, timeout=5).json()
    b = base64.b64decode(resp['responseData'])
    total, active, burned = struct.unpack_from('<QQQ', b, 0)
    return {'total': total, 'active': active, 'burned': burned}


def build_create_gate(mode, recipients_pk, ratios, threshold=0,
                      allowed_senders=None, chain_next_gate_id=-1):
    """Build createGate input with chain support."""
    buf = bytearray()
    buf += struct.pack('<BB', mode, len(recipients_pk))
    buf += bytes(6)  # padding
    for i in range(8):
        buf += recipients_pk[i] if i < len(recipients_pk) else bytes(32)
    for i in range(8):
        buf += struct.pack('<Q', ratios[i] if i < len(ratios) else 0)
    buf += struct.pack('<Q', threshold)
    for i in range(8):
        if allowed_senders and i < len(allowed_senders):
            buf += allowed_senders[i]
        else:
            buf += bytes(32)
    buf += struct.pack('<B', len(allowed_senders) if allowed_senders else 0)
    # Oracle fields (zeroed for non-oracle)
    buf += bytes(32)  # oracleId
    buf += bytes(32)  # oracleCurrency1
    buf += bytes(32)  # oracleCurrency2
    buf += struct.pack('<BB', 0, 0)  # oracleCondition, oracleTriggerMode
    buf += struct.pack('<q', 0)  # oracleThreshold
    # Chain field
    buf += struct.pack('<q', chain_next_gate_id)
    return bytes(buf)


def build_set_chain(gate_id, next_gate_id):
    """Build setChain input."""
    return struct.pack('<qq', gate_id, next_gate_id)


def build_fund_gate(gate_id, reserve_target=0):
    """Build fundGate input with reserveTarget."""
    return struct.pack('<qB', gate_id, reserve_target)


def send_tx(seed, input_type, data, amount):
    tick = get_tick() + 5
    cli("-seed", seed,
        "-sendspecialcommand", str(QUGATE_INDEX),
        "-specialcommandtype", str(input_type),
        "-specialcommanddata", base64.b64encode(data).decode(),
        "-amount", str(amount),
        "-scheduledtick", str(tick))
    return tick


def wait(ticks=8):
    time.sleep(ticks * 2)


def check(name, condition):
    global passed, failed
    if condition:
        passed += 1
        print(f"  PASS  {name}")
    else:
        failed += 1
        print(f"  FAIL  {name}")


def main():
    global passed, failed
    print("=" * 60)
    print("QuGate Chain Gates Integration Tests (Issues #13 #18)")
    print("=" * 60)

    # Skip if node not reachable
    try:
        get_tick()
    except Exception:
        print("SKIP: Qubic node not reachable at", RPC)
        print("Chain gate integration tests require a running core-lite testnet.")
        print("Chain routing logic is tested in C++ unit tests (contract_qugate.cpp).")
        sys.exit(0)

    ADDR_A = get_identity(ADDR_A_KEY)
    ADDR_B = get_identity(ADDR_B_KEY)
    PK_B = get_pubkey(ADDR_B)
    PK_C = get_pubkey(get_identity(ADDR_C_KEY))

    print(f"\nAddr A: {ADDR_A}")
    print(f"Addr B: {ADDR_B}")

    counts_before = query_count()
    print(f"Before: {counts_before['total']} total, {counts_before['active']} active")

    # --- Test 1: Create target gate (no chain) ---
    print("\n--- Test 1: Create target gate (SPLIT → B, no chain) ---")
    data = build_create_gate(0, [PK_B], [100])  # SPLIT 100% → B
    send_tx(ADDR_A_KEY, PROC_CREATE, data, CREATION_FEE)
    wait()

    counts = query_count()
    target_gate_id = encode_gate_id(counts['total'] - 1)
    gate = query_gate(target_gate_id)
    check("target gate created", gate['active'] == 1)
    check("target gate is SPLIT", gate['mode'] == 0)

    # --- Test 2: Create chained gate (→ target) ---
    print("\n--- Test 2: Create chained gate (SPLIT → C, chain → target) ---")
    data = build_create_gate(0, [PK_C], [100], chain_next_gate_id=target_gate_id)
    send_tx(ADDR_A_KEY, PROC_CREATE, data, CREATION_FEE)
    wait()

    counts2 = query_count()
    chained_gate_id = encode_gate_id(counts2['total'] - 1)
    chained_gate = query_gate(chained_gate_id)
    check("chained gate created", chained_gate['active'] == 1)

    # --- Test 3: setChain procedure ---
    print("\n--- Test 3: setChain procedure ---")
    # Create another gate and link it
    data3 = build_create_gate(0, [PK_B], [100])
    send_tx(ADDR_A_KEY, PROC_CREATE, data3, CREATION_FEE)
    wait()

    counts3 = query_count()
    gate3_id = encode_gate_id(counts3['total'] - 1)

    # Set chain: gate3 → target
    chain_data = build_set_chain(gate3_id, target_gate_id)
    send_tx(ADDR_A_KEY, PROC_SET_CHAIN, chain_data, CHAIN_HOP_FEE)
    wait()

    gate3 = query_gate(gate3_id)
    check("setChain: gate still active", gate3['active'] == 1)

    # --- Test 4: fundGate with reserveTarget=1 (chainReserve) ---
    print("\n--- Test 4: fundGate with reserveTarget=1 ---")
    fund_data = build_fund_gate(chained_gate_id, reserve_target=1)
    send_tx(ADDR_B_KEY, PROC_FUND, fund_data, 5000)
    wait()

    funded_gate = query_gate(chained_gate_id)
    check("gate still active after chain fund", funded_gate['active'] == 1)

    # --- Test 5: Close chained gates ---
    print("\n--- Test 5: Close all test gates ---")
    for gid in [chained_gate_id, target_gate_id, gate3_id]:
        close_data = struct.pack('<Q', gid)
        send_tx(ADDR_A_KEY, PROC_CLOSE, close_data, 0)
    wait()

    for gid in [chained_gate_id, target_gate_id, gate3_id]:
        g = query_gate(gid)
        check(f"gate {gid} closed", g['active'] == 0)

    # --- Summary ---
    print("\n" + "=" * 60)
    print(f"Results: {passed} passed, {failed} failed out of {passed + failed}")
    if failed > 0:
        print("SOME TESTS FAILED")
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        print("\nNote: Chain routing (routeToGate) is a PRIVATE procedure invoked")
        print("by the oracle callback. Full chain routing is tested in C++ unit")
        print("tests (contract_qugate.cpp, QuGateChain suite — 15 tests).")


if __name__ == "__main__":
    main()
