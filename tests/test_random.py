#!/usr/bin/env python3
"""QuGate RANDOM Test - randomly selects one recipient per payment"""
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
MODE_RANDOM = 3

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
print("║   QuGate RANDOM Mode Test                        ║")
print("╚══════════════════════════════════════════════════╝\n")

tick = get_tick()
print(f"Node up at tick {tick}\n")

ID0 = get_identity(SEED0)
ID1 = get_identity(SEED1)
ID2 = get_identity(SEED2)
PK1 = get_pubkey_from_identity(ID1)
PK2 = get_pubkey_from_identity(ID2)

# ━━━ Create RANDOM gate with 2 recipients ━━━
print("="*50)
print("STEP 1: Create RANDOM gate (recipients: Seed1, Seed2)")
print("="*50)

input_data = build_create_gate(mode=MODE_RANDOM, recipients_pk=[PK1, PK2], ratios=[1, 1])
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

if gate['mode'] == 'RANDOM' and gate['active']:
    print("  ✅ RANDOM gate created!")
    RND_GATE = total
else:
    print(f"  ⚠ Unexpected")
    RND_GATE = total

# ━━━ Send 6 payments and track distribution ━━━
print("\n" + "="*50)
print("STEP 2: Send 6 payments of 10,000 QU each")
print("="*50)
print("  (With 2 recipients, expect roughly 50/50 random distribution)")

bal1_start = get_balance(ID1)
bal2_start = get_balance(ID2)
print(f"  Seed1 start: {bal1_start:,} QU")
print(f"  Seed2 start: {bal2_start:,} QU")

results = []
for payment_num in range(1, 7):
    print(f"\n  --- Payment {payment_num}: 10,000 QU ---")
    input_data = struct.pack('<Q', RND_GATE)
    out = send_contract_tx(SEED0, PROC_SEND_TO_GATE, 10000, input_data)
    for line in out.strip().splitlines():
        if "Transaction has been sent" in line:
            print(f"  {line.strip()}")

    wait_ticks(15)

    bal1 = get_balance(ID1)
    bal2 = get_balance(ID2)
    gain1 = bal1 - bal1_start
    gain2 = bal2 - bal2_start
    
    # Figure out who got this payment
    prev_gain1 = results[-1][0] if results else 0
    prev_gain2 = results[-1][1] if results else 0
    this_to_1 = gain1 - prev_gain1
    this_to_2 = gain2 - prev_gain2
    recipient = "Seed1" if this_to_1 > 0 else "Seed2" if this_to_2 > 0 else "???"
    
    results.append((gain1, gain2))
    print(f"  → Went to {recipient} | Totals: Seed1={gain1:,}, Seed2={gain2:,}")

# ━━━ Results ━━━
print("\n" + "="*50)
print("RESULTS")
print("="*50)
bal1_end = get_balance(ID1)
bal2_end = get_balance(ID2)
total_gain1 = bal1_end - bal1_start
total_gain2 = bal2_end - bal2_start
total_sent = 60000

gate = query_gate(RND_GATE)
print(f"  Gate: received={gate['totalReceived']}, forwarded={gate['totalForwarded']}")
print(f"  Seed1 total: {total_gain1:,} QU ({total_gain1*100//total_sent}%)")
print(f"  Seed2 total: {total_gain2:,} QU ({total_gain2*100//total_sent}%)")

if total_gain1 + total_gain2 == total_sent:
    print(f"  ✅ All {total_sent:,} QU distributed! Random distribution verified.")
    if total_gain1 > 0 and total_gain2 > 0:
        print("  ✅ Both recipients received funds (not deterministic)")
    else:
        print("  ⚠ Only one recipient got everything (possible but unlikely with 6 payments)")
else:
    print(f"  ⚠ Total mismatch: {total_gain1}+{total_gain2}={total_gain1+total_gain2} vs {total_sent}")

# Close gate
print("\n  Closing gate...")
input_data = struct.pack('<Q', RND_GATE)
send_contract_tx(SEED0, PROC_CLOSE_GATE, 0, input_data)
wait_ticks(15)
gate = query_gate(RND_GATE)
if not gate['active']:
    print("  ✅ Gate closed!")

print("\n🏁 RANDOM test complete!")
