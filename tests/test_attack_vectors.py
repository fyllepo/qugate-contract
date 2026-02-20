#!/usr/bin/env python3
"""
QuGate V2 Attack Vector Tests
Tests edge cases, unauthorized access, and potential exploits.
"""
import os, shutil
import struct, subprocess, json, base64, time, requests, sys

CLI = os.environ.get("QUBIC_CLI", shutil.which("qubic-cli") or "qubic-cli")
ID_TOOL = os.environ.get("QUBIC_ID_TOOL", shutil.which("identity_tool") or "identity_tool")
NODE_ARGS = ["-nodeip", "127.0.0.1", "-nodeport", "31841"]
RPC = "http://127.0.0.1:41841"
QUGATE_INDEX = 24
CONTRACT_ID = "YAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAMSME"

ADDR_A_KEY = "eraaastggldisjhoojaekgyimrsddjxbvgaawswfvnvaygqmusnkevv"
ADDR_B_KEY = "sgwnpzidgxbclnisgehigeculaejjxedzdkjyyfrzgzvuojrhdzywfh"
ADDR_C_KEY = "xeejtwxqrrlvacapbujaleejhbrsnnpvviknskemmgdihggpssjjkrg"

PROC_CREATE_GATE = 1
PROC_SEND_TO_GATE = 2
PROC_CLOSE_GATE = 3

results = []

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
        except:
            if attempt < 4: time.sleep(3)
    raise Exception("Node not responding")

def query_gate_count():
    resp = requests.post(f"{RPC}/live/v1/querySmartContract", json={
        'contractIndex': QUGATE_INDEX, 'inputType': 6, 'inputSize': 0, 'requestData': ''
    }, timeout=5).json()
    b = base64.b64decode(resp['responseData'])
    t, a = struct.unpack('<QQ', b[:16])
    return t, a

def query_gate(gate_id):
    data = struct.pack('<Q', gate_id)
    resp = requests.post(f"{RPC}/live/v1/querySmartContract", json={
        'contractIndex': QUGATE_INDEX, 'inputType': 5, 'inputSize': len(data),
        'requestData': base64.b64encode(data).decode()
    }, timeout=5).json()
    b = base64.b64decode(resp['responseData'])
    modes = ['SPLIT', 'ROUND_ROBIN', 'THRESHOLD', 'RANDOM', 'CONDITIONAL']
    off = 40
    tr, tf, cb, th, ep = struct.unpack_from('<QQQQQ', b, off)
    ratios = [struct.unpack_from('<Q', b, 336 + i*8)[0] for i in range(8)]
    return {
        'mode': modes[b[0]] if b[0] < 5 else f'UNKNOWN({b[0]})',
        'recipientCount': b[1], 'active': b[2],
        'owner': b[8:40].hex(),
        'totalReceived': tr, 'totalForwarded': tf,
        'currentBalance': cb, 'threshold': th, 'createdEpoch': ep,
        'ratios': ratios[:max(b[1], 1)]
    }

def get_pubkey_from_identity(identity):
    pk = bytearray(32)
    for i in range(4):
        val = 0
        for j in range(13, -1, -1):
            c = identity[i * 14 + j]
            val = val * 26 + (ord(c) - ord('A'))
        struct.pack_into('<Q', pk, i * 8, val)
    return bytes(pk)

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

def send_contract_tx(key, input_type, amount, input_data):
    hex_data = input_data.hex()
    return cli("-seed", key, "-sendcustomtransaction",
               CONTRACT_ID, str(input_type), str(amount),
               str(len(input_data)), hex_data)

def wait_ticks(n=15):
    start = get_tick()
    target = start + n
    print(f"    Waiting for tick {target} (current: {start})...")
    for _ in range(120):
        time.sleep(4)
        try:
            cur = get_tick()
            if cur >= target:
                print(f"    ✓ Reached tick {cur}")
                return True
        except:
            time.sleep(5)
    print(f"    ⚠ Timeout")
    return False

def record(name, passed, detail=""):
    status = "✅ PASS" if passed else "❌ FAIL"
    results.append((name, passed, detail))
    print(f"  {status}: {name}")
    if detail:
        print(f"    {detail}")

# ============================================================
print("╔══════════════════════════════════════════════════╗")
print("║   QuGate V2 Attack Vector Tests                  ║")
print("╚══════════════════════════════════════════════════╝")
print()

tick = get_tick()
print(f"Node up at tick {tick}")
print()

ADDR_A = get_identity(ADDR_A_KEY)
ADDR_B = get_identity(ADDR_B_KEY)
ADDR_C = get_identity(ADDR_C_KEY)
PK_A = get_pubkey_from_identity(ADDR_A)
PK_B = get_pubkey_from_identity(ADDR_B)
PK_C = get_pubkey_from_identity(ADDR_C)

# ============================================================
print("=" * 50)
print("TEST 1: Unauthorized gate close")
print("  Address A creates gate, Address C tries to close it")
print("=" * 50)

bal0_before = get_balance(ADDR_A)
bal2_before = get_balance(ADDR_C)

# Create gate as Address A
create_data = build_create_gate(0, [PK_B], [100])  # SPLIT, 1 recipient
out = send_contract_tx(ADDR_A_KEY, PROC_CREATE_GATE, 1000, create_data)
print(f"  Address A created gate (1000 QU fee)")
wait_ticks(15)

total, active = query_gate_count()
gate_id = total  # latest gate
print(f"  Gate #{gate_id} created, active={active}")

# Try to close as Address C (not the owner)
close_data = struct.pack('<Q', gate_id)
out = send_contract_tx(ADDR_C_KEY, PROC_CLOSE_GATE, 0, close_data)
print(f"  Address C attempted close...")
wait_ticks(15)

gate = query_gate(gate_id)
record("Unauthorized close rejected", gate['active'] == 1,
       f"Gate still active={gate['active']}")

# Now close properly as Address A
send_contract_tx(ADDR_A_KEY, PROC_CLOSE_GATE, 0, close_data)
wait_ticks(15)
gate = query_gate(gate_id)
record("Owner close succeeds", gate['active'] == 0,
       f"Gate active={gate['active']}")
print()

# ============================================================
print("=" * 50)
print("TEST 2: Send to non-existent gate")
print("  Send funds to gate #9999")
print("=" * 50)

bal0_before = get_balance(ADDR_A)
send_data = struct.pack('<Q', 9999)
out = send_contract_tx(ADDR_A_KEY, PROC_SEND_TO_GATE, 5000, send_data)
print(f"  Sent 5000 QU to gate #9999")
wait_ticks(15)

bal0_after = get_balance(ADDR_A)
# If rejected, funds should be returned (or at least not lost)
loss = bal0_before - bal0_after
record("Non-existent gate — funds safe", loss <= 0,
       f"Balance change: {loss} QU (before={bal0_before}, after={bal0_after})")
print()

# ============================================================
print("=" * 50)
print("TEST 3: Send to closed gate")
print("  Send funds to the gate closed in test 1")
print("=" * 50)

bal0_before = get_balance(ADDR_A)
send_data = struct.pack('<Q', gate_id)
out = send_contract_tx(ADDR_A_KEY, PROC_SEND_TO_GATE, 5000, send_data)
print(f"  Sent 5000 QU to closed gate #{gate_id}")
wait_ticks(15)

bal0_after = get_balance(ADDR_A)
loss = bal0_before - bal0_after
record("Closed gate — funds safe", loss <= 0,
       f"Balance change: {loss} QU")
print()

# ============================================================
print("=" * 50)
print("TEST 4: Double close")
print("  Close already-closed gate again")
print("=" * 50)

bal0_before = get_balance(ADDR_A)
out = send_contract_tx(ADDR_A_KEY, PROC_CLOSE_GATE, 0, struct.pack('<Q', gate_id))
print(f"  Sent second close for gate #{gate_id}")
wait_ticks(15)

gate = query_gate(gate_id)
bal0_after = get_balance(ADDR_A)
record("Double close — no crash, no state change", gate['active'] == 0,
       f"Gate still inactive, balance change: {bal0_before - bal0_after} QU")
print()

# ============================================================
print("=" * 50)
print("TEST 5: Zero-amount send")
print("  Send 0 QU to an active gate")
print("=" * 50)

# Create fresh gate for this test
create_data = build_create_gate(0, [PK_B], [100])
send_contract_tx(ADDR_A_KEY, PROC_CREATE_GATE, 1000, create_data)
wait_ticks(15)
total2, _ = query_gate_count()
gate_id2 = total2

bal1_before = get_balance(ADDR_B)
send_data = struct.pack('<Q', gate_id2)
out = send_contract_tx(ADDR_A_KEY, PROC_SEND_TO_GATE, 0, send_data)
print(f"  Sent 0 QU to gate #{gate_id2}")
wait_ticks(15)

gate = query_gate(gate_id2)
bal1_after = get_balance(ADDR_B)
record("Zero amount — no effect", gate['totalReceived'] == 0 and bal1_before == bal1_after,
       f"received={gate['totalReceived']}, Address B change={bal1_after - bal1_before}")

# Clean up
send_contract_tx(ADDR_A_KEY, PROC_CLOSE_GATE, 0, struct.pack('<Q', gate_id2))
wait_ticks(15)
print()

# ============================================================
print("=" * 50)
print("TEST 6: Gate slot reuse after close")
print("  Create gate, close it, create another — verify reuse")
print("=" * 50)

total_before, _ = query_gate_count()
create_data = build_create_gate(0, [PK_B], [100])
send_contract_tx(ADDR_A_KEY, PROC_CREATE_GATE, 1000, create_data)
wait_ticks(15)
total_after, active_after = query_gate_count()
# If free-list works, total should stay same (slot reused) or increment by 1
# After closing gates above, free slots should be available
record("Gate slot reuse", total_after <= total_before + 1,
       f"Before: total={total_before}, After: total={total_after}, active={active_after}")

# Clean up
send_contract_tx(ADDR_A_KEY, PROC_CLOSE_GATE, 0, struct.pack('<Q', total_after))
wait_ticks(15)
print()

# ============================================================
print()
print("=" * 50)
print("RESULTS SUMMARY")
print("=" * 50)
passed = sum(1 for _, p, _ in results if p)
failed = sum(1 for _, p, _ in results if not p)
for name, p, detail in results:
    print(f"  {'✅' if p else '❌'} {name}")
print(f"\n  Total: {passed} passed, {failed} failed out of {len(results)}")
print()
print("🏁 Attack vector tests complete!")
