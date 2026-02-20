#!/usr/bin/env python3
"""QuGate ROUND_ROBIN Test"""
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
QUGATE_INDEX = 24
CONTRACT_ID = "YAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAMSME"

ADDR_A_KEY = "eraaastggldisjhoojaekgyimrsddjxbvgaawswfvnvaygqmusnkevv"
ADDR_B_KEY = "sgwnpzidgxbclnisgehigeculaejjxedzdkjyyfrzgzvuojrhdzywfh"
ADDR_C_KEY = "xeejtwxqrrlvacapbujaleejhbrsnnpvviknskemmgdihggpssjjkrg"

PROC_CREATE_GATE = 1
PROC_SEND_TO_GATE = 2
PROC_CLOSE_GATE = 3
MODE_ROUND_ROBIN = 1

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

def send_contract_tx(key, input_type, amount, input_data):
    hex_data = input_data.hex()
    out = cli("-seed", key, "-sendcustomtransaction",
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
        except Exception:
            time.sleep(5)
    print("    ⚠ Timeout")
    return False

# ============================================================
print("╔══════════════════════════════════════════════════╗")
print("║   QuGate ROUND_ROBIN Mode Test                   ║")
print("╚══════════════════════════════════════════════════╝\n")

tick = get_tick()
print(f"Node up at tick {tick}\n")

ADDR_A = get_identity(ADDR_A_KEY)
ADDR_B = get_identity(ADDR_B_KEY)
ADDR_C = get_identity(ADDR_C_KEY)
PK_B = get_pubkey_from_identity(ADDR_B)
PK_C = get_pubkey_from_identity(ADDR_C)

# ━━━ Create ROUND_ROBIN gate with 2 recipients ━━━
print("="*50)
print("STEP 1: Create ROUND_ROBIN gate (Address B, Address C)")
print("="*50)

# For round robin, ratios aren't used but we still fill them
input_data = build_create_gate(mode=MODE_ROUND_ROBIN, recipients_pk=[PK_B, PK_C], ratios=[1, 1])
print(f"  Input size: {len(input_data)} bytes")
print("  Sending createGate tx (1000 QU fee)...")
out = send_contract_tx(ADDR_A_KEY, PROC_CREATE_GATE, 1000, input_data)
for line in out.strip().splitlines():
    if "Transaction has been sent" in line or "TxHash" in line or "Tick:" in line:
        print(f"  {line.strip()}")

wait_ticks(15)

total, active = query_gate_count()
print(f"\n  Gate count: total={total}, active={active}")
gate = query_gate(total)
print(f"  Gate #{total}: mode={gate['mode']}, recipients={gate['recipientCount']}, active={gate['active']}")

if gate['mode'] == 'ROUND_ROBIN' and gate['active']:
    print("  ✅ ROUND_ROBIN gate created!")
    RR_GATE = total
else:
    print(f"  ⚠ Unexpected: mode={gate['mode']}, active={gate['active']}")
    RR_GATE = total

# ━━━ Send 3 payments — should alternate: Address B, Address C, Address B ━━━
print("\n" + "="*50)
print("STEP 2: Send 3 payments of 10,000 QU each (expect round-robin)")
print("="*50)

bal1_start = get_balance(ADDR_B)
bal2_start = get_balance(ADDR_C)
print(f"  Address B start: {bal1_start:,} QU")
print(f"  Address C start: {bal2_start:,} QU")

for payment_num in range(1, 4):
    print(f"\n  --- Payment {payment_num}: 10,000 QU ---")
    input_data = struct.pack('<Q', RR_GATE)
    out = send_contract_tx(ADDR_A_KEY, PROC_SEND_TO_GATE, 10000, input_data)
    for line in out.strip().splitlines():
        if "Transaction has been sent" in line:
            print(f"  {line.strip()}")

    wait_ticks(15)

    bal1 = get_balance(ADDR_B)
    bal2 = get_balance(ADDR_C)
    gain1 = bal1 - bal1_start
    gain2 = bal2 - bal2_start
    print(f"  Address B total gained: {gain1:,} QU")
    print(f"  Address C total gained: {gain2:,} QU")

    gate = query_gate(RR_GATE)
    print(f"  Gate: received={gate['totalReceived']}, forwarded={gate['totalForwarded']}")

# Final check
print("\n" + "="*50)
print("RESULTS")
print("="*50)
bal1_end = get_balance(ADDR_B)
bal2_end = get_balance(ADDR_C)
total_gain1 = bal1_end - bal1_start
total_gain2 = bal2_end - bal2_start
print(f"  Address B total gained: {total_gain1:,} QU")
print(f"  Address C total gained: {total_gain2:,} QU")

# Round robin with 2 recipients, 3 payments:
# Payment 1 → Address B (10k), Payment 2 → Address C (10k), Payment 3 → Address B (10k)
# Expected: Address B = +20k, Address C = +10k
if total_gain1 == 20000 and total_gain2 == 10000:
    print("  ✅ Perfect ROUND_ROBIN! (Address B→Address C→Address B)")
elif total_gain1 == 10000 and total_gain2 == 20000:
    print("  ✅ ROUND_ROBIN works! (started with Address C→Address B→Address C)")
elif total_gain1 + total_gain2 == 30000:
    print(f"  ✅ All 30k distributed! Pattern: Address B={total_gain1}, Address C={total_gain2}")
else:
    print(f"  ⚠ Unexpected distribution: {total_gain1} + {total_gain2} = {total_gain1+total_gain2}")

# Close gate
print("\n  Closing gate...")
input_data = struct.pack('<Q', RR_GATE)
send_contract_tx(ADDR_A_KEY, PROC_CLOSE_GATE, 0, input_data)
wait_ticks(15)
gate = query_gate(RR_GATE)
print(f"  Gate active: {gate['active']}")
if not gate['active']:
    print("  ✅ Gate closed!")

print("\n🏁 ROUND_ROBIN test complete!")
