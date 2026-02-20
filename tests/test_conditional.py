#!/usr/bin/env python3
"""QuGate CONDITIONAL Test - only forwards if sender is on whitelist"""
import os, shutil
import struct, subprocess, json, base64, time, requests, sys

CLI = os.environ.get("QUBIC_CLI", shutil.which("qubic-cli") or "qubic-cli")
NODE_ARGS = ["-nodeip", "127.0.0.1", "-nodeport", "31841"]
RPC = "http://127.0.0.1:41841"
QUGATE_INDEX = 24
CONTRACT_ID = "YAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAMSME"

SEED0 = "eraaastggldisjhoojaekgyimrsddjxbvgaawswfvnvaygqmusnkevv"
SEED1 = "sgwnpzidgxbclnisgehigeculaejjxedzdkjyyfrzgzvuojrhdzywfh"
SEED2 = "xeejtwxqrrlvacapbujaleejhbrsnnpvviknskemmgdihggpssjjkrg"

PROC_CREATE_GATE = 1
PROC_SEND_TO_GATE = 2
PROC_CLOSE_GATE = 3
MODE_CONDITIONAL = 4

def cli(*args, timeout=15):
    r = subprocess.run([CLI] + NODE_ARGS + list(args), capture_output=True, text=True, timeout=timeout)
    return r.stdout + r.stderr

def get_identity(seed):
    out = cli("-seed", seed, "-showkeys")
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
    return {
        'mode': modes[b[0]], 'recipientCount': b[1], 'active': b[2],
        'totalReceived': tr, 'totalForwarded': tf, 'currentBalance': cb,
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

def send_contract_tx(seed, input_type, amount, input_data):
    hex_data = input_data.hex()
    return cli("-seed", seed, "-sendcustomtransaction",
              CONTRACT_ID, str(input_type), str(amount),
              str(len(input_data)), hex_data)

def wait_ticks(n=15):
    start = get_tick()
    target = start + n
    print(f"    Waiting for tick {target} (current: {start})...")
    for _ in range(90):
        time.sleep(4)
        try:
            cur = get_tick()
            if cur >= target:
                print(f"    ✓ Reached tick {cur}")
                return True
        except:
            time.sleep(5)
    return False

# ============================================================
print("╔══════════════════════════════════════════════════╗")
print("║   QuGate CONDITIONAL Mode Test                   ║")
print("╚══════════════════════════════════════════════════╝\n")

tick = get_tick()
print(f"Node up at tick {tick}\n")

ID0 = get_identity(SEED0)
ID1 = get_identity(SEED1)
ID2 = get_identity(SEED2)
PK0 = get_pubkey_from_identity(ID0)
PK1 = get_pubkey_from_identity(ID1)
PK2 = get_pubkey_from_identity(ID2)

# ━━━ Create CONDITIONAL gate: only Seed0 allowed to send, forwards to Seed1 ━━━
print("="*50)
print("STEP 1: Create CONDITIONAL gate")
print("  Recipient: Seed1")
print("  Allowed sender: Seed0 only")
print("="*50)

input_data = build_create_gate(
    mode=MODE_CONDITIONAL,
    recipients_pk=[PK1],
    ratios=[100],
    threshold=0,
    allowed_senders=[PK0]
)
print(f"  Sending createGate tx (1000 QU fee)...")
out = send_contract_tx(SEED0, PROC_CREATE_GATE, 1000, input_data)
for line in out.strip().splitlines():
    if "Transaction has been sent" in line:
        print(f"  {line.strip()}")

wait_ticks(15)

total, active = query_gate_count()
print(f"\n  Gate count: total={total}, active={active}")
gate = query_gate(total)
print(f"  Gate #{total}: mode={gate['mode']}, recipients={gate['recipientCount']}, active={gate['active']}")

if gate['mode'] == 'CONDITIONAL' and gate['active']:
    print("  ✅ CONDITIONAL gate created!")
    COND_GATE = total
else:
    print(f"  ⚠ Unexpected")
    COND_GATE = total

# ━━━ Test 1: Allowed sender (Seed0) sends — should forward ━━━
print("\n" + "="*50)
print("STEP 2: Allowed sender (Seed0) sends 10,000 QU — should forward")
print("="*50)

bal1_before = get_balance(ID1)
bal0_before = get_balance(ID0)
print(f"  Seed0 before: {bal0_before:,} QU")
print(f"  Seed1 before: {bal1_before:,} QU")

input_data = struct.pack('<Q', COND_GATE)
out = send_contract_tx(SEED0, PROC_SEND_TO_GATE, 10000, input_data)
for line in out.strip().splitlines():
    if "Transaction has been sent" in line:
        print(f"  {line.strip()}")

wait_ticks(15)

gate = query_gate(COND_GATE)
bal1_after = get_balance(ID1)
gain1 = bal1_after - bal1_before
print(f"\n  Gate: received={gate['totalReceived']}, forwarded={gate['totalForwarded']}, balance={gate['currentBalance']}")
print(f"  Seed1 gained: {gain1:,}")

if gate['totalForwarded'] == 10000 and gain1 == 10000:
    print("  ✅ Allowed sender's payment forwarded correctly!")
elif gate['totalForwarded'] == 0:
    print("  ⚠ Payment not forwarded — may be held in balance")
else:
    print(f"  Forwarded={gate['totalForwarded']}, gain={gain1}")

# ━━━ Test 2: Unauthorized sender (Seed2) sends — should bounce/reject ━━━
print("\n" + "="*50)
print("STEP 3: Unauthorized sender (Seed2) sends 10,000 QU — should reject/bounce")
print("="*50)

bal1_before2 = get_balance(ID1)
bal2_before = get_balance(ID2)
print(f"  Seed2 before: {bal2_before:,} QU")
print(f"  Seed1 before: {bal1_before2:,} QU")

input_data = struct.pack('<Q', COND_GATE)
out = send_contract_tx(SEED2, PROC_SEND_TO_GATE, 10000, input_data)
for line in out.strip().splitlines():
    if "Transaction has been sent" in line:
        print(f"  {line.strip()}")

wait_ticks(15)

gate = query_gate(COND_GATE)
bal1_after2 = get_balance(ID1)
bal2_after = get_balance(ID2)
gain1_2 = bal1_after2 - bal1_before2
seed2_change = bal2_after - bal2_before
print(f"\n  Gate: received={gate['totalReceived']}, forwarded={gate['totalForwarded']}, balance={gate['currentBalance']}")
print(f"  Seed1 gained: {gain1_2}")
print(f"  Seed2 change: {seed2_change:,}")

if gate['totalForwarded'] == 10000 and gain1_2 == 0:
    print("  ✅ Unauthorized payment NOT forwarded! (gate rejected or held)")
    if seed2_change == -10000:
        print("  ⚠ Seed2 lost 10k — payment went to contract but wasn't forwarded (held in balance)")
    elif seed2_change == 0:
        print("  ✅ Seed2 didn't lose money — transaction was rejected")
elif gate['totalForwarded'] == 20000:
    print("  ⚠ Payment was forwarded anyway — CONDITIONAL mode may not be filtering")
else:
    print(f"  Forwarded={gate['totalForwarded']}, seed1_gain={gain1_2}, seed2_change={seed2_change}")

# ━━━ Test 3: Allowed sender sends again — confirm still works ━━━
print("\n" + "="*50)
print("STEP 4: Allowed sender (Seed0) sends 5,000 QU again")
print("="*50)

bal1_before3 = get_balance(ID1)

input_data = struct.pack('<Q', COND_GATE)
out = send_contract_tx(SEED0, PROC_SEND_TO_GATE, 5000, input_data)
for line in out.strip().splitlines():
    if "Transaction has been sent" in line:
        print(f"  {line.strip()}")

wait_ticks(15)

gate = query_gate(COND_GATE)
bal1_after3 = get_balance(ID1)
gain1_3 = bal1_after3 - bal1_before3
print(f"\n  Gate: received={gate['totalReceived']}, forwarded={gate['totalForwarded']}, balance={gate['currentBalance']}")
print(f"  Seed1 gained: {gain1_3:,}")

if gain1_3 == 5000:
    print("  ✅ Allowed sender still works after unauthorized attempt!")

# Close gate
print("\n  Closing gate...")
input_data = struct.pack('<Q', COND_GATE)
send_contract_tx(SEED0, PROC_CLOSE_GATE, 0, input_data)
wait_ticks(15)
gate = query_gate(COND_GATE)
if not gate['active']:
    print("  ✅ Gate closed!")

print("\n🏁 CONDITIONAL test complete!")
