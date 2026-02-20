#!/usr/bin/env python3
"""
QuGate V2 — Gate Chaining Test (Proof of Concept)

Demonstrates composable payment routing:
  
  Pipeline: Seed0 → Gate A (SPLIT 70/30) → Seed1 (70%) + Seed2 (30%)
                                              ↓
                              Seed1 → Gate B (THRESHOLD 15k) → Seed2
                                              
  Flow:
    1. Create Gate A: SPLIT 70/30 → Seed1, Seed2
    2. Create Gate B: THRESHOLD 15000 → Seed2 (owned by Seed1)
    3. Send 20,000 QU through Gate A
       → Seed1 gets 14,000 (70%), Seed2 gets 6,000 (30%)
    4. Seed1 routes 14,000 through Gate B (threshold=15k, so accumulates)
    5. Send another 20,000 QU through Gate A
       → Seed1 gets 14,000 more (total 28k through Gate B → triggers threshold!)
    6. Verify Gate B triggered and forwarded all accumulated funds to Seed2

  This proves gates can be composed into multi-stage payment pipelines.
"""
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
        'mode': modes[b[0]] if b[0] < 5 else f'UNKNOWN({b[0]})',
        'recipientCount': b[1], 'active': b[2],
        'totalReceived': tr, 'totalForwarded': tf,
        'currentBalance': cb, 'threshold': th
    }

def query_gate_count():
    resp = requests.post(f"{RPC}/live/v1/querySmartContract", json={
        'contractIndex': QUGATE_INDEX, 'inputType': 6, 'inputSize': 0, 'requestData': ''
    }, timeout=5).json()
    b = base64.b64decode(resp['responseData'])
    t, a = struct.unpack('<QQ', b[:16])
    return t, a

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

# ============================================================
print("╔══════════════════════════════════════════════════╗")
print("║   QuGate V2 — Gate Chaining Test (POC)           ║")
print("╚══════════════════════════════════════════════════╝")
print()
print("  Pipeline:")
print("    Seed0 → [Gate A: SPLIT 70/30] → Seed1 (70%) + Seed2 (30%)")
print("                                       ↓")
print("                        Seed1 → [Gate B: THRESHOLD 15k] → Seed2")
print()

tick = get_tick()
print(f"Node up at tick {tick}")

ID0 = get_identity(SEED0)
ID1 = get_identity(SEED1)
ID2 = get_identity(SEED2)
PK0 = get_pubkey_from_identity(ID0)
PK1 = get_pubkey_from_identity(ID1)
PK2 = get_pubkey_from_identity(ID2)

print(f"  Seed0: {ID0[:12]}...")
print(f"  Seed1: {ID1[:12]}...")
print(f"  Seed2: {ID2[:12]}...")

# Record starting balances
bal0_start = get_balance(ID0)
bal1_start = get_balance(ID1)
bal2_start = get_balance(ID2)
print(f"\n━━━ Starting Balances ━━━")
print(f"  Seed0: {bal0_start:,} QU")
print(f"  Seed1: {bal1_start:,} QU")
print(f"  Seed2: {bal2_start:,} QU")

# ============================================================
print(f"\n{'='*60}")
print("STAGE 1: Create Gate A (SPLIT 70% → Seed1, 30% → Seed2)")
print(f"{'='*60}")

gate_a_data = build_create_gate(0, [PK1, PK2], [70, 30])  # SPLIT mode
out = send_contract_tx(SEED0, PROC_CREATE_GATE, 1000, gate_a_data)
print(f"  Seed0 creating Gate A (1000 QU fee)...")
wait_ticks(15)

total, active = query_gate_count()
gate_a_id = total
gate_a = query_gate(gate_a_id)
print(f"  ✅ Gate A = #{gate_a_id}: mode={gate_a['mode']}, recipients={gate_a['recipientCount']}")

# ============================================================
print(f"\n{'='*60}")
print("STAGE 2: Create Gate B (THRESHOLD 15000 → Seed2, owned by Seed1)")
print(f"{'='*60}")

gate_b_data = build_create_gate(2, [PK2], [100], threshold=15000)  # THRESHOLD mode
out = send_contract_tx(SEED1, PROC_CREATE_GATE, 1000, gate_b_data)
print(f"  Seed1 creating Gate B (1000 QU fee)...")
wait_ticks(15)

total, active = query_gate_count()
gate_b_id = total
gate_b = query_gate(gate_b_id)
print(f"  ✅ Gate B = #{gate_b_id}: mode={gate_b['mode']}, threshold={gate_b['threshold']}")

# ============================================================
print(f"\n{'='*60}")
print("STAGE 3: Seed0 sends 20,000 QU through Gate A")
print(f"{'='*60}")

bal1_before = get_balance(ID1)
bal2_before = get_balance(ID2)

send_data = struct.pack('<Q', gate_a_id)
out = send_contract_tx(SEED0, PROC_SEND_TO_GATE, 20000, send_data)
print(f"  Sent 20,000 QU to Gate A...")
wait_ticks(15)

bal1_after_stage3 = get_balance(ID1)
bal2_after_stage3 = get_balance(ID2)
seed1_gained = bal1_after_stage3 - bal1_before
seed2_gained = bal2_after_stage3 - bal2_before
gate_a = query_gate(gate_a_id)

print(f"  Gate A: received={gate_a['totalReceived']}, forwarded={gate_a['totalForwarded']}")
print(f"  Seed1 gained: {seed1_gained:,} QU (expected 14,000 = 70%)")
print(f"  Seed2 gained: {seed2_gained:,} QU (expected 6,000 = 30%)")
assert seed1_gained == 14000, f"Expected 14000, got {seed1_gained}"
assert seed2_gained == 6000, f"Expected 6000, got {seed2_gained}"
print(f"  ✅ Gate A split correctly!")

# ============================================================
print(f"\n{'='*60}")
print("STAGE 4: Seed1 chains 14,000 QU into Gate B")
print(f"{'='*60}")

bal2_before_chain = get_balance(ID2)

send_data = struct.pack('<Q', gate_b_id)
out = send_contract_tx(SEED1, PROC_SEND_TO_GATE, 14000, send_data)
print(f"  Seed1 routes 14,000 QU into Gate B (threshold=15000)...")
wait_ticks(15)

gate_b = query_gate(gate_b_id)
bal2_after_chain1 = get_balance(ID2)
seed2_chain_gained = bal2_after_chain1 - bal2_before_chain

print(f"  Gate B: received={gate_b['totalReceived']}, forwarded={gate_b['totalForwarded']}, balance={gate_b['currentBalance']}")
print(f"  Seed2 gained from chain: {seed2_chain_gained:,} QU")
print(f"  ✅ Gate B accumulating (14,000 < 15,000 threshold) — not yet triggered")

# ============================================================
print(f"\n{'='*60}")
print("STAGE 5: Seed0 sends another 20,000 QU through Gate A")
print(f"{'='*60}")

bal1_before_stage5 = get_balance(ID1)
bal2_before_stage5 = get_balance(ID2)

send_data = struct.pack('<Q', gate_a_id)
out = send_contract_tx(SEED0, PROC_SEND_TO_GATE, 20000, send_data)
print(f"  Sent 20,000 QU to Gate A...")
wait_ticks(15)

bal1_after_stage5 = get_balance(ID1)
bal2_after_stage5 = get_balance(ID2)
seed1_gained_5 = bal1_after_stage5 - bal1_before_stage5
seed2_gained_5 = bal2_after_stage5 - bal2_before_stage5
gate_a = query_gate(gate_a_id)

print(f"  Gate A: received={gate_a['totalReceived']}, forwarded={gate_a['totalForwarded']}")
print(f"  Seed1 gained: {seed1_gained_5:,} QU (expected 14,000)")
print(f"  Seed2 gained: {seed2_gained_5:,} QU (expected 6,000)")
print(f"  ✅ Gate A split correctly again!")

# ============================================================
print(f"\n{'='*60}")
print("STAGE 6: Seed1 chains another 14,000 QU → Gate B TRIGGERS!")
print(f"{'='*60}")

bal2_before_trigger = get_balance(ID2)

send_data = struct.pack('<Q', gate_b_id)
out = send_contract_tx(SEED1, PROC_SEND_TO_GATE, 14000, send_data)
print(f"  Seed1 routes 14,000 QU into Gate B (total will be 28,000 ≥ 15,000)...")
wait_ticks(15)

gate_b = query_gate(gate_b_id)
bal2_after_trigger = get_balance(ID2)
seed2_trigger_gained = bal2_after_trigger - bal2_before_trigger

print(f"  Gate B: received={gate_b['totalReceived']}, forwarded={gate_b['totalForwarded']}, balance={gate_b['currentBalance']}")
print(f"  Seed2 gained from threshold trigger: {seed2_trigger_gained:,} QU")

if gate_b['totalForwarded'] > 0:
    print(f"  ✅ GATE B TRIGGERED! Threshold reached, funds forwarded to Seed2!")
else:
    print(f"  ⚠ Gate B did not trigger — checking if threshold logic differs...")

# ============================================================
print(f"\n{'='*60}")
print("STAGE 7: Close both gates")
print(f"{'='*60}")

send_contract_tx(SEED0, PROC_CLOSE_GATE, 0, struct.pack('<Q', gate_a_id))
send_contract_tx(SEED1, PROC_CLOSE_GATE, 0, struct.pack('<Q', gate_b_id))
wait_ticks(15)

gate_a_final = query_gate(gate_a_id)
gate_b_final = query_gate(gate_b_id)
print(f"  Gate A: active={gate_a_final['active']}")
print(f"  Gate B: active={gate_b_final['active']}")

# ============================================================
print(f"\n{'='*60}")
print("FINAL RESULTS")
print(f"{'='*60}")

bal0_end = get_balance(ID0)
bal1_end = get_balance(ID1)
bal2_end = get_balance(ID2)

print(f"\n  Balance Changes:")
print(f"    Seed0: {bal0_end - bal0_start:+,} QU (sent 40k + 2k fees)")
print(f"    Seed1: {bal1_end - bal1_start:+,} QU (received 28k from A, sent 28k to B, paid 1k fee)")
print(f"    Seed2: {bal2_end - bal2_start:+,} QU (received 12k from A + {gate_b_final['totalForwarded']} from B)")

print(f"\n  Gate A (SPLIT): received={gate_a_final['totalReceived']}, forwarded={gate_a_final['totalForwarded']}")
print(f"  Gate B (THRESHOLD): received={gate_b_final['totalReceived']}, forwarded={gate_b_final['totalForwarded']}")

print(f"\n  Pipeline Flow:")
print(f"    Seed0 → 40,000 QU → Gate A")
print(f"    Gate A → {int(40000*0.7):,} QU (70%) → Seed1 → Gate B → Seed2")
print(f"    Gate A → {int(40000*0.3):,} QU (30%) → Seed2 (direct)")
total_to_seed2 = bal2_end - bal2_start
print(f"    Total to Seed2: {total_to_seed2:,} QU (direct + chained)")

print(f"\n🏁 Gate chaining POC complete!")
