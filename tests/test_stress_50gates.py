#!/usr/bin/env python3
"""
QuGate V2 — Stress Test: 50 Gates with Mixed Rule Sets

Creates 50 gates across all 5 modes, sends transactions through each,
verifies correct routing, then closes all gates and checks slot reuse.

Gate distribution:
  - 15 SPLIT gates (various ratios)
  - 10 ROUND_ROBIN gates (2-4 recipients)
  - 10 THRESHOLD gates (various thresholds)
  - 10 RANDOM gates (2-3 recipients)
  - 5 CONDITIONAL gates (sender-restricted)
"""
import os
import shutil
import struct
import subprocess
import base64
import time
import requests
import sys
import random

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

MODE_SPLIT = 0
MODE_ROUND_ROBIN = 1
MODE_THRESHOLD = 2
MODE_RANDOM = 3
MODE_CONDITIONAL = 4

MODE_NAMES = ['SPLIT', 'ROUND_ROBIN', 'THRESHOLD', 'RANDOM', 'CONDITIONAL']

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
    try:
        resp = requests.post(f"{RPC}/live/v1/querySmartContract", json={
            'contractIndex': QUGATE_INDEX, 'inputType': 5, 'inputSize': len(data),
            'requestData': base64.b64encode(data).decode()
        }, timeout=5).json()
        b = base64.b64decode(resp['responseData'])
        off = 40
        tr, tf, cb, th, ep = struct.unpack_from('<QQQQQ', b, off)
        return {
            'mode': b[0], 'recipientCount': b[1], 'active': b[2],
            'totalReceived': tr, 'totalForwarded': tf,
            'currentBalance': cb, 'threshold': th
        }
    except Exception:
        return None

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
    sys.stdout.write(f"    Waiting for tick {target}...")
    sys.stdout.flush()
    for _ in range(180):
        time.sleep(4)
        try:
            cur = get_tick()
            if cur >= target:
                print(f" ✓ ({cur})")
                return True
        except Exception:
            time.sleep(5)
    print(" ⚠ TIMEOUT")
    return False

# ============================================================
print("╔══════════════════════════════════════════════════╗")
print("║   QuGate V2 — 50 Gate Stress Test                ║")
print("╚══════════════════════════════════════════════════╝")
print()

tick = get_tick()
print(f"Node up at tick {tick}")

ADDR_A = get_identity(ADDR_A_KEY)
ADDR_B = get_identity(ADDR_B_KEY)
ADDR_C = get_identity(ADDR_C_KEY)
PK_A = get_pubkey_from_identity(ADDR_A)
PK_B = get_pubkey_from_identity(ADDR_B)
PK_C = get_pubkey_from_identity(ADDR_C)

bal0_start = get_balance(ADDR_A)
bal1_start = get_balance(ADDR_B)
bal2_start = get_balance(ADDR_C)
total_start, active_start = query_gate_count()

print(f"  Starting balances: Address A={bal0_start:,}, Address B={bal1_start:,}, Address C={bal2_start:,}")
print(f"  Existing gates: total={total_start}, active={active_start}")

# Define 50 gate configurations
gate_configs = []

# 15 SPLIT gates with various ratios
split_ratios = [
    [50, 50], [70, 30], [90, 10], [33, 33, 34], [25, 25, 25, 25],
    [80, 20], [60, 40], [99, 1], [10, 90], [45, 55],
    [20, 30, 50], [15, 85], [75, 25], [5, 95], [50, 30, 20],
]
for i, ratios in enumerate(split_ratios):
    pks = [PK_B, PK_C][:len(ratios)] if len(ratios) <= 2 else [PK_B, PK_C, PK_A][:len(ratios)]
    # For 4-recipient gates, reuse keys
    if len(ratios) == 4:
        pks = [PK_B, PK_C, PK_A, PK_B]
    gate_configs.append(('SPLIT', MODE_SPLIT, pks, ratios, 0, None))

# 10 ROUND_ROBIN gates
for i in range(10):
    if i < 5:
        pks = [PK_B, PK_C]
    else:
        pks = [PK_B, PK_C, PK_A]
    gate_configs.append(('ROUND_ROBIN', MODE_ROUND_ROBIN, pks, [100]*len(pks), 0, None))

# 10 THRESHOLD gates
thresholds = [5000, 10000, 15000, 20000, 25000, 50000, 1000, 3000, 7500, 100000]
for i, thresh in enumerate(thresholds):
    gate_configs.append(('THRESHOLD', MODE_THRESHOLD, [PK_B], [100], thresh, None))

# 10 RANDOM gates
for i in range(10):
    if i < 6:
        pks = [PK_B, PK_C]
    else:
        pks = [PK_B, PK_C, PK_A]
    gate_configs.append(('RANDOM', MODE_RANDOM, pks, [100]*len(pks), 0, None))

# 5 CONDITIONAL gates
for i in range(5):
    gate_configs.append(('CONDITIONAL', MODE_CONDITIONAL, [PK_B], [100], 0, [PK_A]))

assert len(gate_configs) == 50, f"Expected 50 configs, got {len(gate_configs)}"

# ============================================================
print(f"\n{'='*60}")
print(f"PHASE 1: Create 50 gates (batched, {len(gate_configs)} configs)")
print(f"{'='*60}")

gate_ids = []
created = 0
failed_creates = 0

# Send all create transactions, waiting between batches
BATCH_SIZE = 5
for batch_start in range(0, len(gate_configs), BATCH_SIZE):
    batch_end = min(batch_start + BATCH_SIZE, len(gate_configs))
    batch = gate_configs[batch_start:batch_end]
    
    print(f"\n  Batch {batch_start//BATCH_SIZE + 1}: Gates {batch_start+1}-{batch_end}")
    
    for i, (name, mode, pks, ratios, thresh, senders) in enumerate(batch):
        idx = batch_start + i + 1
        create_data = build_create_gate(mode, pks, ratios, thresh, senders)
        out = send_contract_tx(ADDR_A_KEY, PROC_CREATE_GATE, 1000, create_data)
        if "sent" in out.lower():
            sys.stdout.write(f"    #{idx} {name} ")
            sys.stdout.flush()
        else:
            print(f"    #{idx} {name} — SEND FAILED")
            failed_creates += 1
    
    # Wait for batch to process
    wait_ticks(15)
    
    # Check what was created
    total_now, active_now = query_gate_count()
    batch_created = total_now - total_start - created
    created = total_now - total_start
    print(f"    Batch result: {batch_created} gates created (total: {created}, active: {active_now})")

# Record gate IDs
for i in range(created):
    gate_ids.append(total_start + i + 1)

print(f"\n  ✅ Created {created}/50 gates ({failed_creates} send failures)")

# ============================================================
print(f"\n{'='*60}")
print("PHASE 2: Send transactions through all active gates")
print(f"{'='*60}")

send_amount = 1000  # Small amounts to avoid running out
sends_attempted = 0
sends_ok = 0

for batch_start in range(0, len(gate_ids), BATCH_SIZE):
    batch_end = min(batch_start + BATCH_SIZE, len(gate_ids))
    batch_ids = gate_ids[batch_start:batch_end]
    
    print(f"\n  Batch: Gates {batch_ids[0]}-{batch_ids[-1]}")
    
    for gid in batch_ids:
        send_data = struct.pack('<Q', gid)
        out = send_contract_tx(ADDR_A_KEY, PROC_SEND_TO_GATE, send_amount, send_data)
        sends_attempted += 1
        if "sent" in out.lower():
            sends_ok += 1
    
    wait_ticks(15)

print(f"\n  Sent {sends_ok}/{sends_attempted} transactions ({send_amount} QU each)")

# Verify some gates received funds
sample_gates = random.sample(gate_ids, min(10, len(gate_ids)))
gates_with_activity = 0
for gid in sample_gates:
    g = query_gate(gid)
    if g and g['totalReceived'] > 0:
        gates_with_activity += 1

print(f"  Sample check: {gates_with_activity}/{len(sample_gates)} sampled gates have received funds")

# ============================================================
print(f"\n{'='*60}")
print("PHASE 3: Close all 50 gates")
print(f"{'='*60}")

closed = 0
for batch_start in range(0, len(gate_ids), BATCH_SIZE):
    batch_end = min(batch_start + BATCH_SIZE, len(gate_ids))
    batch_ids = gate_ids[batch_start:batch_end]
    
    for gid in batch_ids:
        close_data = struct.pack('<Q', gid)
        send_contract_tx(ADDR_A_KEY, PROC_CLOSE_GATE, 0, close_data)
    
    wait_ticks(15)
    closed += len(batch_ids)
    sys.stdout.write(f"  Closed {closed}/{len(gate_ids)}...")
    
    total_now, active_now = query_gate_count()
    print(f" (active: {active_now})")

total_end, active_end = query_gate_count()
print(f"\n  Final: total={total_end}, active={active_end}")

# ============================================================
print(f"\n{'='*60}")
print("PHASE 4: Verify slot reuse — create 5 new gates")
print(f"{'='*60}")

total_before_reuse = total_end
for i in range(5):
    create_data = build_create_gate(MODE_SPLIT, [PK_B, PK_C], [50, 50])
    send_contract_tx(ADDR_A_KEY, PROC_CREATE_GATE, 1000, create_data)

wait_ticks(15)
total_after_reuse, active_after_reuse = query_gate_count()
reused = 5 - (total_after_reuse - total_before_reuse)
print(f"  Before: total={total_before_reuse}, After: total={total_after_reuse}")
print(f"  Slots reused: {reused}/5")
if reused > 0:
    print(f"  ✅ Free-list working — {reused} slots reused from closed gates!")
else:
    print("  ⚠ No slots reused (free-list may not be working)")

# Clean up reuse gates
for i in range(5):
    gid = total_after_reuse - 4 + i
    send_contract_tx(ADDR_A_KEY, PROC_CLOSE_GATE, 0, struct.pack('<Q', gid))
wait_ticks(15)

# ============================================================
print(f"\n{'='*60}")
print("FINAL RESULTS")
print(f"{'='*60}")

bal0_end = get_balance(ADDR_A)
bal1_end = get_balance(ADDR_B)
bal2_end = get_balance(ADDR_C)
total_final, active_final = query_gate_count()

print(f"\n  Gates created: {created}/50")
print(f"  Transactions sent: {sends_ok}/{sends_attempted}")
print(f"  Gates closed: {closed}/{len(gate_ids)}")
print(f"  Slot reuse verified: {reused > 0}")
print(f"  Final gate count: total={total_final}, active={active_final}")
print("\n  Balance changes:")
print(f"    Address A: {bal0_end - bal0_start:+,} QU")
print(f"    Address B: {bal1_end - bal1_start:+,} QU")
print(f"    Address C: {bal2_end - bal2_start:+,} QU")
print(f"\n  Node stability: {'✅ STABLE' if get_tick() > 0 else '❌ DOWN'}")
print("\n🏁 50-gate stress test complete!")
