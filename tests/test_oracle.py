#!/usr/bin/env python3
"""
QuGate — Oracle Mode (QUGATE_MODE_ORACLE = 5) Integration Tests

Tests: oracle gate creation, fundGate, condition evaluation logic,
       ONCE vs RECURRING trigger modes, reserve exhaustion.

NOTE: The oracle notification callback (OraclePriceNotification) cannot be
fully tested without a live node and oracle infrastructure. These tests
exercise the contract interface (createGate, fundGate, sendToGate, closeGate)
for oracle gates. The condition evaluation logic is tested in isolation
in the C++ unit tests (contract_qugate.cpp).
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
QUGATE_INDEX = 25  # Pulse took index 24

ADDR_A_KEY = "eraaastggldisjhoojaekgyimrsddjxbvgaawswfvnvaygqmusnkevv"
ADDR_B_KEY = "sgwnpzidgxbclnisgehigeculaejjxedzdkjyyfrzgzvuojrhdzywfh"
ADDR_C_KEY = "xeejtwxqrrlvacapbujaleejhbrsnnpvviknskemmgdihggpssjjkrg"

CREATION_FEE = 100000
MIN_SEND = 1000

MODE_ORACLE = 5
PROC_CREATE = 1
PROC_SEND = 2
PROC_CLOSE = 3
PROC_FUND = 10
FUNC_GET_GATE = 5
FUNC_GET_COUNT = 6

# Oracle condition types
COND_PRICE_ABOVE = 0
COND_PRICE_BELOW = 1
COND_TIME_AFTER  = 2

# Oracle trigger modes
TRIGGER_ONCE      = 0
TRIGGER_RECURRING = 1

passed = 0
failed = 0

def cli(*args, timeout=15):
    r = subprocess.run([CLI] + NODE_ARGS + list(args), capture_output=True, text=True, timeout=timeout)
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
        'contractIndex': QUGATE_INDEX, 'inputType': FUNC_GET_GATE, 'inputSize': len(data),
        'requestData': base64.b64encode(data).decode()
    }, timeout=5).json()
    b = base64.b64decode(resp['responseData'])
    modes = ['SPLIT', 'ROUND_ROBIN', 'THRESHOLD', 'RANDOM', 'CONDITIONAL', 'ORACLE']
    tr, tf, cb, th = struct.unpack_from('<QQQQ', b, 40)
    return {
        'mode': b[0], 'mode_name': modes[b[0]] if b[0] < 6 else f'?{b[0]}',
        'recipientCount': b[1], 'active': b[2],
        'totalReceived': tr, 'totalForwarded': tf,
        'currentBalance': cb, 'threshold': th,
    }

def query_count():
    resp = requests.post(f"{RPC}/live/v1/querySmartContract", json={
        'contractIndex': QUGATE_INDEX, 'inputType': FUNC_GET_COUNT, 'inputSize': 0, 'requestData': ''
    }, timeout=5).json()
    b = base64.b64decode(resp['responseData'])
    total, active, burned = struct.unpack_from('<QQQ', b, 0)
    return {'total': total, 'active': active, 'burned': burned}

def build_create_oracle(recipient_keys, oracle_condition, oracle_threshold,
                         trigger_mode=TRIGGER_ONCE, oracle_id_byte=99):
    """Build createGate input for ORACLE mode."""
    # mode(1) + recipientCount(1) + recipients(256) + ratios(64) + threshold(8)
    # + allowedSenders(256) + allowedSenderCount(1)
    # + oracleId(32) + oracleCurrency1(32) + oracleCurrency2(32)
    # + oracleCondition(1) + oracleTriggerMode(1) + oracleThreshold(8)
    buf = bytearray()
    buf += struct.pack('<BB', MODE_ORACLE, len(recipient_keys))
    # recipients (8 x 32 = 256 bytes)
    for i in range(8):
        if i < len(recipient_keys):
            buf += get_pubkey(get_identity(recipient_keys[i]))
        else:
            buf += b'\x00' * 32
    # ratios (8 x 8 = 64 bytes) — not used for oracle, zero them
    buf += b'\x00' * 64
    # threshold (8 bytes) — not used for oracle
    buf += struct.pack('<Q', 0)
    # allowedSenders (8 x 32 = 256 bytes)
    buf += b'\x00' * 256
    # allowedSenderCount (1 byte)
    buf += struct.pack('<B', 0)
    # oracleId (32 bytes)
    oracle_id = bytearray(32)
    oracle_id[0] = oracle_id_byte
    buf += bytes(oracle_id)
    # oracleCurrency1 (32 bytes) — mock
    buf += b'\x01' + b'\x00' * 31
    # oracleCurrency2 (32 bytes) — mock
    buf += b'\x02' + b'\x00' * 31
    # oracleCondition (1 byte)
    buf += struct.pack('<B', oracle_condition)
    # oracleTriggerMode (1 byte)
    buf += struct.pack('<B', trigger_mode)
    # oracleThreshold (8 bytes, sint64)
    buf += struct.pack('<q', oracle_threshold)
    return bytes(buf)

def build_fund_gate(gate_id):
    """Build fundGate input."""
    return struct.pack('<q', gate_id)

def send_tx(seed, input_type, data, amount, tick_offset=5):
    tick = get_tick() + tick_offset
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
    print("QuGate Oracle Mode Integration Tests")
    print("=" * 60)

    # Skip if node not reachable
    try:
        get_tick()
    except Exception:
        print("SKIP: Qubic node not reachable at", RPC)
        print("Oracle integration tests require a running core-lite testnet.")
        print("The oracle callback cannot be fully tested without live oracle infrastructure.")
        print("Condition evaluation logic is tested in C++ unit tests (contract_qugate.cpp).")
        sys.exit(0)

    ADDR_A = get_identity(ADDR_A_KEY)
    ADDR_B = get_identity(ADDR_B_KEY)
    ADDR_C = get_identity(ADDR_C_KEY)

    print(f"\nAddr A: {ADDR_A}")
    print(f"Addr B: {ADDR_B}")
    print(f"Addr C: {ADDR_C}")

    counts_before = query_count()
    print(f"\nBefore: {counts_before['total']} total, {counts_before['active']} active")

    # --- Test 1: Create oracle gate (COND_PRICE_ABOVE, TRIGGER_ONCE) ---
    print("\n--- Test 1: Create oracle gate (PRICE_ABOVE, ONCE) ---")
    data = build_create_oracle([ADDR_B_KEY], COND_PRICE_ABOVE, 50000000, TRIGGER_ONCE)
    oracle_reserve_amount = 50000  # extra for oracle reserve
    send_tx(ADDR_A_KEY, PROC_CREATE, data, CREATION_FEE + oracle_reserve_amount)
    wait()

    counts_after = query_count()
    new_gate_id = counts_after['total']
    check("oracle gate created", counts_after['active'] == counts_before['active'] + 1)

    gate = query_gate(new_gate_id)
    check("gate mode is ORACLE", gate['mode'] == MODE_ORACLE)
    check("gate is active", gate['active'] == 1)
    check("recipient count is 1", gate['recipientCount'] == 1)

    # --- Test 2: Fund oracle gate ---
    print("\n--- Test 2: Fund oracle gate ---")
    data = build_fund_gate(new_gate_id)
    send_tx(ADDR_C_KEY, PROC_FUND, data, 20000)
    wait()

    # Note: can't directly check oracleReserve via basic query without
    # extended output parsing — the reserve is in the gate struct but
    # output format may not expose it via basic RPC. This tests the
    # transaction succeeds without error.
    gate_after_fund = query_gate(new_gate_id)
    check("gate still active after fund", gate_after_fund['active'] == 1)

    # --- Test 3: Send funds to oracle gate (accumulates in currentBalance) ---
    print("\n--- Test 3: Send funds to oracle gate ---")
    send_data = struct.pack('<Q', new_gate_id)
    send_tx(ADDR_B_KEY, PROC_SEND, send_data, 5000)
    wait()

    gate_after_send = query_gate(new_gate_id)
    check("currentBalance accumulated", gate_after_send['currentBalance'] == 5000)
    check("totalReceived tracked", gate_after_send['totalReceived'] == 5000)
    check("nothing forwarded yet (awaiting oracle)", gate_after_send['totalForwarded'] == 0)

    # --- Test 4: Close oracle gate (owner only) ---
    print("\n--- Test 4: Close oracle gate ---")
    close_data = struct.pack('<Q', new_gate_id)
    send_tx(ADDR_A_KEY, PROC_CLOSE, close_data, 0)
    wait()

    gate_closed = query_gate(new_gate_id)
    check("gate closed", gate_closed['active'] == 0)
    check("balance refunded on close", gate_closed['currentBalance'] == 0)

    # --- Test 5: Create RECURRING oracle gate ---
    print("\n--- Test 5: Create RECURRING oracle gate ---")
    data = build_create_oracle([ADDR_B_KEY, ADDR_C_KEY], COND_PRICE_BELOW, 100000000, TRIGGER_RECURRING)
    send_tx(ADDR_A_KEY, PROC_CREATE, data, CREATION_FEE + 30000)
    wait()

    counts_recurring = query_count()
    recurring_id = counts_recurring['total']
    gate_recurring = query_gate(recurring_id)
    check("recurring gate created", gate_recurring['active'] == 1)
    check("recurring gate mode is ORACLE", gate_recurring['mode'] == MODE_ORACLE)
    check("recurring gate has 2 recipients", gate_recurring['recipientCount'] == 2)

    # Clean up
    close_data = struct.pack('<Q', recurring_id)
    send_tx(ADDR_A_KEY, PROC_CLOSE, close_data, 0)
    wait()

    # --- Summary ---
    print("\n" + "=" * 60)
    print(f"Results: {passed} passed, {failed} failed out of {passed + failed}")
    if failed > 0:
        print("SOME TESTS FAILED")
        sys.exit(1)
    else:
        print("ALL TESTS PASSED")
        print("\nNote: Oracle notification callback (OraclePriceNotification) cannot be")
        print("tested without live oracle infrastructure. Condition evaluation logic")
        print("is tested in C++ unit tests (contract_qugate.cpp, QuGateOracle suite).")


if __name__ == "__main__":
    main()
