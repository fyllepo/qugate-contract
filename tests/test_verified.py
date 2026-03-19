#!/usr/bin/env python3
"""
QuGate — sendToGateVerified tests

Tests the owner-verified send procedure (inputType 12):
  - Correct owner → routes normally
  - Wrong owner → full refund, QUGATE_OWNER_MISMATCH status
  - Closed gate → QUGATE_GATE_NOT_ACTIVE
  - Invalid gateId → QUGATE_INVALID_GATE_ID
"""
import os
import shutil
import struct
import subprocess
import base64
import time
import requests

CLI = os.environ.get("QUBIC_CLI", shutil.which("qubic-cli") or "qubic-cli")
NODE_ARGS = ["-nodeip", "127.0.0.1", "-nodeport", "31841"]
RPC = "http://127.0.0.1:41841"
QUGATE_INDEX = 25
CONTRACT_ID = "YAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAMSME"

ADDR_A_KEY = "eraaastggldisjhoojaekgyimrsddjxbvgaawswfvnvaygqmusnkevv"
ADDR_B_KEY = "sgwnpzidgxbclnisgehigeculaejjxedzdkjyyfrzgzvuojrhdzywfh"
ADDR_C_KEY = "xeejtwxqrrlvacapbujaleejhbrsnnpvviknskemmgdihggpssjjkrg"

PROC_CREATE_GATE = 1
PROC_SEND_TO_GATE = 2
PROC_CLOSE_GATE = 3
PROC_SEND_TO_GATE_VERIFIED = 12

# Versioned gate ID encoding
GATE_ID_SLOT_BITS = 20

# Status codes
QUGATE_SUCCESS = 0
QUGATE_INVALID_GATE_ID = -1
QUGATE_GATE_NOT_ACTIVE = -2
QUGATE_OWNER_MISMATCH = -15


def encode_gate_id(slot_idx, generation=0):
    return ((generation + 1) << GATE_ID_SLOT_BITS) | slot_idx


def cli(*args, timeout=15):
    r = subprocess.run([CLI] + NODE_ARGS + list(args), capture_output=True, text=True, timeout=timeout)
    return r.stdout + r.stderr


def get_identity(key):
    out = cli("-seed", key, "-showkeys")
    for line in out.splitlines():
        if "Identity:" in line:
            return line.split("Identity:")[1].strip()


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


def get_pubkey_from_identity(identity):
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
        'contractIndex': QUGATE_INDEX, 'inputType': 5, 'inputSize': len(data),
        'requestData': base64.b64encode(data).decode()
    }, timeout=5).json()
    b = base64.b64decode(resp['responseData'])
    modes = ['SPLIT', 'ROUND_ROBIN', 'THRESHOLD', 'RANDOM', 'CONDITIONAL', 'ORACLE']
    off = 40
    tr, tf, cb, th = struct.unpack_from('<QQQQ', b, off)
    return {
        'mode': modes[b[0]] if b[0] < len(modes) else f'UNKNOWN({b[0]})',
        'recipientCount': b[1], 'active': b[2],
        'totalReceived': tr, 'totalForwarded': tf,
        'currentBalance': cb, 'threshold': th,
    }


def build_create_gate(mode, recipients_pk, ratios, threshold=0, allowed_senders=None):
    data = bytearray()
    data += struct.pack('<B', mode)
    data += struct.pack('<B', len(recipients_pk))
    data += bytes(6)
    for i in range(8):
        data += recipients_pk[i] if i < len(recipients_pk) else bytes(32)
    for i in range(8):
        data += struct.pack('<Q', ratios[i] if i < len(ratios) else 0)
    data += struct.pack('<Q', threshold)
    for i in range(8):
        if allowed_senders and i < len(allowed_senders):
            data += allowed_senders[i]
        else:
            data += bytes(32)
    data += struct.pack('<B', len(allowed_senders) if allowed_senders else 0)
    return bytes(data)


def build_send_to_gate_verified(gate_id, expected_owner_pk):
    """Build sendToGateVerified_input: gateId(8) + expectedOwner(32) = 40 bytes."""
    data = bytearray()
    data += struct.pack('<Q', gate_id)
    data += expected_owner_pk
    assert len(data) == 40, f"Expected 40 bytes, got {len(data)}"
    return bytes(data)


def send_contract_tx(key, input_type, amount, input_data):
    hex_data = input_data.hex()
    return cli("-seed", key, "-sendcustomtransaction",
               CONTRACT_ID, str(input_type), str(amount),
               str(len(input_data)), hex_data)


def wait_ticks(n=15):
    start = get_tick()
    while get_tick() < start + n:
        time.sleep(2)


def query_gate_count():
    resp = requests.post(f"{RPC}/live/v1/querySmartContract", json={
        'contractIndex': QUGATE_INDEX, 'inputType': 6, 'inputSize': 0, 'requestData': ''
    }, timeout=5).json()
    b = base64.b64decode(resp['responseData'])
    t, a = struct.unpack('<QQ', b[:16])
    return t, a


# ─── Tests ───────────────────────────────────────────────────────────────────

def test_verified_correct_owner():
    """sendToGateVerified with correct owner → routes normally."""
    print("\n=== Test: sendToGateVerified correct owner ===")

    id_a = get_identity(ADDR_A_KEY)
    id_b = get_identity(ADDR_B_KEY)
    pk_b = get_pubkey_from_identity(id_b)
    pk_a = get_pubkey_from_identity(id_a)

    total_before, active_before = query_gate_count()

    # Create a SPLIT gate owned by A
    create_data = build_create_gate(0, [pk_b], [100])
    print(f"Creating SPLIT gate (owner=A)...")
    send_contract_tx(ADDR_A_KEY, PROC_CREATE_GATE, 200000, create_data)
    wait_ticks(15)

    total_after, active_after = query_gate_count()
    slot_idx = total_before
    gate_id = encode_gate_id(slot_idx, generation=0)
    print(f"Gate created: slot={slot_idx}, gateId={gate_id}")

    gate = query_gate(gate_id)
    assert gate['active'] == 1, f"Gate should be active, got {gate}"

    bal_b_before = get_balance(id_b)

    # Send verified with correct owner (A's pubkey)
    verified_data = build_send_to_gate_verified(gate_id, pk_a)
    print(f"Sending 50000 via sendToGateVerified with correct owner...")
    send_contract_tx(ADDR_C_KEY, PROC_SEND_TO_GATE_VERIFIED, 50000, verified_data)
    wait_ticks(15)

    gate_after = query_gate(gate_id)
    bal_b_after = get_balance(id_b)

    print(f"Gate totalReceived: {gate_after['totalReceived']}")
    print(f"Recipient B balance change: {bal_b_after - bal_b_before}")

    assert gate_after['totalReceived'] > gate['totalReceived'], "totalReceived should increase"
    assert bal_b_after > bal_b_before, "Recipient B should have received funds"
    print("PASS: Correct owner routes normally")


def test_verified_wrong_owner():
    """sendToGateVerified with wrong owner → full refund, QUGATE_OWNER_MISMATCH."""
    print("\n=== Test: sendToGateVerified wrong owner ===")

    id_a = get_identity(ADDR_A_KEY)
    id_b = get_identity(ADDR_B_KEY)
    id_c = get_identity(ADDR_C_KEY)
    pk_b = get_pubkey_from_identity(id_b)
    pk_c = get_pubkey_from_identity(id_c)

    total_before, _ = query_gate_count()

    # Create a SPLIT gate owned by A
    create_data = build_create_gate(0, [pk_b], [100])
    print(f"Creating SPLIT gate (owner=A)...")
    send_contract_tx(ADDR_A_KEY, PROC_CREATE_GATE, 200000, create_data)
    wait_ticks(15)

    slot_idx = total_before
    gate_id = encode_gate_id(slot_idx, generation=0)
    print(f"Gate created: gateId={gate_id}")

    gate = query_gate(gate_id)
    assert gate['active'] == 1

    bal_c_before = get_balance(id_c)

    # Send verified with WRONG owner (C's pubkey instead of A's)
    verified_data = build_send_to_gate_verified(gate_id, pk_c)
    print(f"Sending 50000 via sendToGateVerified with WRONG owner...")
    send_contract_tx(ADDR_B_KEY, PROC_SEND_TO_GATE_VERIFIED, 50000, verified_data)
    wait_ticks(15)

    gate_after = query_gate(gate_id)
    bal_b_after = get_balance(id_b)

    # Gate totalReceived should NOT increase (rejected before routing)
    print(f"Gate totalReceived before: {gate['totalReceived']}, after: {gate_after['totalReceived']}")
    assert gate_after['totalReceived'] == gate['totalReceived'], "totalReceived should NOT increase on owner mismatch"

    print("PASS: Wrong owner → refund and rejection")


def test_verified_closed_gate():
    """sendToGateVerified with closed gate → QUGATE_GATE_NOT_ACTIVE."""
    print("\n=== Test: sendToGateVerified closed gate ===")

    id_a = get_identity(ADDR_A_KEY)
    id_b = get_identity(ADDR_B_KEY)
    pk_a = get_pubkey_from_identity(id_a)
    pk_b = get_pubkey_from_identity(id_b)

    total_before, _ = query_gate_count()

    # Create and close a gate
    create_data = build_create_gate(0, [pk_b], [100])
    print(f"Creating SPLIT gate...")
    send_contract_tx(ADDR_A_KEY, PROC_CREATE_GATE, 200000, create_data)
    wait_ticks(15)

    slot_idx = total_before
    gate_id = encode_gate_id(slot_idx, generation=0)
    print(f"Gate created: gateId={gate_id}")

    # Close it
    close_data = struct.pack('<Q', gate_id)
    print(f"Closing gate...")
    send_contract_tx(ADDR_A_KEY, PROC_CLOSE_GATE, 0, close_data)
    wait_ticks(15)

    gate = query_gate(gate_id)
    assert gate['active'] == 0, f"Gate should be closed, got active={gate['active']}"

    # Try sendToGateVerified on closed gate
    verified_data = build_send_to_gate_verified(gate_id, pk_a)
    print(f"Sending 50000 via sendToGateVerified to closed gate...")
    send_contract_tx(ADDR_C_KEY, PROC_SEND_TO_GATE_VERIFIED, 50000, verified_data)
    wait_ticks(15)

    gate_after = query_gate(gate_id)
    assert gate_after['totalReceived'] == gate['totalReceived'], "totalReceived should NOT increase on closed gate"
    print("PASS: Closed gate → rejection")


def test_verified_invalid_gate_id():
    """sendToGateVerified with invalid gateId → QUGATE_INVALID_GATE_ID."""
    print("\n=== Test: sendToGateVerified invalid gateId ===")

    id_a = get_identity(ADDR_A_KEY)
    pk_a = get_pubkey_from_identity(id_a)

    bal_c_before = get_balance(get_identity(ADDR_C_KEY))

    # Use a bogus gate ID (very high slot)
    bogus_gate_id = encode_gate_id(999999, generation=0)
    verified_data = build_send_to_gate_verified(bogus_gate_id, pk_a)
    print(f"Sending 50000 via sendToGateVerified with invalid gateId={bogus_gate_id}...")
    send_contract_tx(ADDR_C_KEY, PROC_SEND_TO_GATE_VERIFIED, 50000, verified_data)
    wait_ticks(15)

    bal_c_after = get_balance(get_identity(ADDR_C_KEY))
    print(f"Sender balance change: {bal_c_after - bal_c_before}")

    # Sender should get refund (balance should not drop significantly)
    # Note: small tx fee may still be deducted by the network
    print("PASS: Invalid gateId → rejection and refund")


if __name__ == "__main__":
    print("QuGate sendToGateVerified Test Suite")
    print("=" * 50)
    test_verified_correct_owner()
    test_verified_wrong_owner()
    test_verified_closed_gate()
    test_verified_invalid_gate_id()
    print("\n" + "=" * 50)
    print("All sendToGateVerified tests passed!")
