#!/usr/bin/env python3
"""QuGate THRESHOLD Test - accumulate until threshold, then forward"""
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
MODE_THRESHOLD = 2

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
    ratios = [struct.unpack_from('<Q', b, 336 + i*8)[0] for i in range(8)]
    return {
        'mode': modes[b[0]], 'recipientCount': b[1], 'active': b[2],
        'owner': b[8:40].hex(), 'totalReceived': tr, 'totalForwarded': tf,
        'currentBalance': cb, 'threshold': th, 'createdEpoch': ep,
        'ratios': ratios[:b[1]]
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
    out = cli("-seed", seed, "-sendcustomtransaction",
              CONTRACT_ID, str(input_type), str(amount),
              str(len(input_data)), hex_data)
    return out

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
    print(f"    ⚠ Timeout")
    return False

# ============================================================
print("╔══════════════════════════════════════════════════╗")
print("║   QuGate THRESHOLD Mode Test                     ║")
print("╚══════════════════════════════════════════════════╝\n")

tick = get_tick()
print(f"Node up at tick {tick}\n")

ID0 = get_identity(SEED0)
ID1 = get_identity(SEED1)
ID2 = get_identity(SEED2)
PK1 = get_pubkey_from_identity(ID1)

# ━━━ Create THRESHOLD gate: forward to Seed1 when balance >= 25,000 ━━━
print("="*50)
print("STEP 1: Create THRESHOLD gate (threshold=25000, recipient=Seed1)")
print("="*50)

THRESHOLD_AMOUNT = 25000
input_data = build_create_gate(
    mode=MODE_THRESHOLD,
    recipients_pk=[PK1],
    ratios=[100],
    threshold=THRESHOLD_AMOUNT
)
print(f"  Threshold: {THRESHOLD_AMOUNT:,} QU")
print(f"  Sending createGate tx (1000 QU fee)...")
out = send_contract_tx(SEED0, PROC_CREATE_GATE, 1000, input_data)
for line in out.strip().splitlines():
    if "Transaction has been sent" in line:
        print(f"  {line.strip()}")

wait_ticks(15)

total, active = query_gate_count()
print(f"\n  Gate count: total={total}, active={active}")
gate = query_gate(total)
print(f"  Gate #{total}: mode={gate['mode']}, threshold={gate['threshold']}, active={gate['active']}")

if gate['mode'] == 'THRESHOLD' and gate['active'] and gate['threshold'] == THRESHOLD_AMOUNT:
    print("  ✅ THRESHOLD gate created!")
    TH_GATE = total
else:
    print(f"  ⚠ Unexpected state")
    TH_GATE = total

# ━━━ Payment 1: 10,000 QU (below threshold — should accumulate) ━━━
print("\n" + "="*50)
print("STEP 2: Send 10,000 QU (below threshold — should accumulate)")
print("="*50)

bal1_start = get_balance(ID1)
print(f"  Seed1 before: {bal1_start:,} QU")

input_data = struct.pack('<Q', TH_GATE)
out = send_contract_tx(SEED0, PROC_SEND_TO_GATE, 10000, input_data)
for line in out.strip().splitlines():
    if "Transaction has been sent" in line:
        print(f"  {line.strip()}")

wait_ticks(15)

gate = query_gate(TH_GATE)
bal1 = get_balance(ID1)
print(f"\n  Gate: received={gate['totalReceived']}, forwarded={gate['totalForwarded']}, balance={gate['currentBalance']}")
print(f"  Seed1 gained: {bal1 - bal1_start}")

if gate['currentBalance'] == 10000 and gate['totalForwarded'] == 0:
    print("  ✅ Correctly accumulated — threshold not yet reached!")
elif gate['totalForwarded'] > 0:
    print("  ⚠ Forwarded early — threshold logic may differ")
else:
    print(f"  Balance={gate['currentBalance']}, forwarded={gate['totalForwarded']}")

# ━━━ Payment 2: 10,000 more QU (total 20k, still below 25k threshold) ━━━
print("\n" + "="*50)
print("STEP 3: Send 10,000 more QU (total 20k — still below threshold)")
print("="*50)

input_data = struct.pack('<Q', TH_GATE)
out = send_contract_tx(SEED0, PROC_SEND_TO_GATE, 10000, input_data)
for line in out.strip().splitlines():
    if "Transaction has been sent" in line:
        print(f"  {line.strip()}")

wait_ticks(15)

gate = query_gate(TH_GATE)
bal1 = get_balance(ID1)
print(f"\n  Gate: received={gate['totalReceived']}, forwarded={gate['totalForwarded']}, balance={gate['currentBalance']}")
print(f"  Seed1 total gained: {bal1 - bal1_start}")

if gate['currentBalance'] == 20000 and gate['totalForwarded'] == 0:
    print("  ✅ Still accumulating — 20k < 25k threshold")
elif gate['totalForwarded'] > 0:
    print("  ⚠ Forwarded — threshold logic may differ")

# ━━━ Payment 3: 5,000 more QU (total 25k — hits threshold!) ━━━
print("\n" + "="*50)
print("STEP 4: Send 5,000 more QU (total 25k — HITS THRESHOLD!)")
print("="*50)

input_data = struct.pack('<Q', TH_GATE)
out = send_contract_tx(SEED0, PROC_SEND_TO_GATE, 5000, input_data)
for line in out.strip().splitlines():
    if "Transaction has been sent" in line:
        print(f"  {line.strip()}")

wait_ticks(15)

gate = query_gate(TH_GATE)
bal1 = get_balance(ID1)
gain = bal1 - bal1_start
print(f"\n  Gate: received={gate['totalReceived']}, forwarded={gate['totalForwarded']}, balance={gate['currentBalance']}")
print(f"  Seed1 total gained: {gain:,}")

if gate['totalForwarded'] == 25000 and gate['currentBalance'] == 0:
    print("  ✅ THRESHOLD triggered! All 25,000 QU forwarded to Seed1!")
elif gate['totalForwarded'] > 0:
    print(f"  Forwarded {gate['totalForwarded']} — threshold triggered (balance={gate['currentBalance']})")
else:
    print(f"  ⚠ Not forwarded yet — balance={gate['currentBalance']}")

# ━━━ Payment 4: Send another 10k (should accumulate again from zero) ━━━
print("\n" + "="*50)
print("STEP 5: Send 10,000 QU after threshold reset (should accumulate again)")
print("="*50)

input_data = struct.pack('<Q', TH_GATE)
out = send_contract_tx(SEED0, PROC_SEND_TO_GATE, 10000, input_data)
for line in out.strip().splitlines():
    if "Transaction has been sent" in line:
        print(f"  {line.strip()}")

wait_ticks(15)

gate = query_gate(TH_GATE)
print(f"\n  Gate: received={gate['totalReceived']}, forwarded={gate['totalForwarded']}, balance={gate['currentBalance']}")

if gate['currentBalance'] == 10000:
    print("  ✅ Re-accumulating after threshold reset!")
elif gate['currentBalance'] == 0 and gate['totalForwarded'] == 35000:
    print("  Gate forwarded immediately — threshold is one-shot, not resetting")

# Close gate
print("\n  Closing gate...")
input_data = struct.pack('<Q', TH_GATE)
send_contract_tx(SEED0, PROC_CLOSE_GATE, 0, input_data)
wait_ticks(15)
gate = query_gate(TH_GATE)
if not gate['active']:
    print("  ✅ Gate closed!")

print("\n🏁 THRESHOLD test complete!")
