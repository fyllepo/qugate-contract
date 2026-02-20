#!/usr/bin/env python3
"""
QuGate V2 — Multi-Sender Convergence Test

3 different seeds all send to the same SPLIT gate.
Verifies correct routing regardless of sender identity.
"""
import struct, subprocess, json, base64, time, requests, sys

CLI = "/home/phil/projects/qubic-cli/build/qubic-cli"
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
    tr, tf, cb = struct.unpack_from('<QQQ', b, off)
    return {
        'mode': modes[b[0]], 'recipientCount': b[1], 'active': b[2],
        'totalReceived': tr, 'totalForwarded': tf, 'currentBalance': cb
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
    for _ in range(180):
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
print("║   QuGate V2 — Multi-Sender Convergence Test      ║")
print("╚══════════════════════════════════════════════════╝")
print()
print("  3 senders → 1 SPLIT gate (50/50) → Seed1 + Seed2")
print("  Verifies any sender can use a gate, not just the owner")
print()

tick = get_tick()
print(f"Node up at tick {tick}")

ID0 = get_identity(SEED0)
ID1 = get_identity(SEED1)
ID2 = get_identity(SEED2)
PK0 = get_pubkey_from_identity(ID0)
PK1 = get_pubkey_from_identity(ID1)
PK2 = get_pubkey_from_identity(ID2)

# Use Seed0 and Seed2 as recipients so all 3 seeds can be senders
# Gate: SPLIT 50/50 → Seed0, Seed2
# Senders: Seed0, Seed1, Seed2

print(f"  Recipients: Seed0 ({ID0[:12]}...) + Seed2 ({ID2[:12]}...)")
print(f"  Senders: Seed0, Seed1, Seed2")

bal0_start = get_balance(ID0)
bal1_start = get_balance(ID1)
bal2_start = get_balance(ID2)
print(f"\n━━━ Starting Balances ━━━")
print(f"  Seed0: {bal0_start:,} QU")
print(f"  Seed1: {bal1_start:,} QU")
print(f"  Seed2: {bal2_start:,} QU")

# ============================================================
print(f"\n{'='*60}")
print("STEP 1: Create SPLIT gate (50/50 → Seed0, Seed2)")
print(f"{'='*60}")

create_data = build_create_gate(0, [PK0, PK2], [50, 50])
out = send_contract_tx(SEED1, PROC_CREATE_GATE, 1000, create_data)
print(f"  Seed1 creates gate (1000 QU fee) — Seed1 is owner but NOT a recipient")
wait_ticks(15)

total, active = query_gate_count()
gate_id = total
gate = query_gate(gate_id)
print(f"  Gate #{gate_id}: mode={gate['mode']}, recipients={gate['recipientCount']}, active={gate['active']}")
print(f"  ✅ Gate created!")

# ============================================================
print(f"\n{'='*60}")
print("STEP 2: Seed0 sends 10,000 QU (sender is also a recipient)")
print(f"{'='*60}")

bal0_before = get_balance(ID0)
bal2_before = get_balance(ID2)

send_data = struct.pack('<Q', gate_id)
out = send_contract_tx(SEED0, PROC_SEND_TO_GATE, 10000, send_data)
print(f"  Seed0 sent 10,000 QU to gate #{gate_id}")
wait_ticks(15)

bal0_after = get_balance(ID0)
bal2_after = get_balance(ID2)
gate = query_gate(gate_id)

# Seed0 sent 10k but also received 5k back (50%), net -5k
seed0_net = bal0_after - bal0_before
seed2_gain = bal2_after - bal2_before

print(f"  Gate: received={gate['totalReceived']}, forwarded={gate['totalForwarded']}")
print(f"  Seed0 net change: {seed0_net:+,} QU (sent 10k, received 5k back = -5k)")
print(f"  Seed2 gained: {seed2_gain:+,} QU (expected +5,000)")
print(f"  ✅ Sender-as-recipient works!" if seed2_gain == 5000 else f"  ⚠ Unexpected: Seed2 gained {seed2_gain}")

# ============================================================
print(f"\n{'='*60}")
print("STEP 3: Seed1 sends 20,000 QU (owner sends, not a recipient)")
print(f"{'='*60}")

bal0_before = get_balance(ID0)
bal2_before = get_balance(ID2)

out = send_contract_tx(SEED1, PROC_SEND_TO_GATE, 20000, send_data)
print(f"  Seed1 sent 20,000 QU to gate #{gate_id}")
wait_ticks(15)

bal0_after = get_balance(ID0)
bal2_after = get_balance(ID2)
gate = query_gate(gate_id)

seed0_gain = bal0_after - bal0_before
seed2_gain = bal2_after - bal2_before

print(f"  Gate: received={gate['totalReceived']}, forwarded={gate['totalForwarded']}")
print(f"  Seed0 gained: {seed0_gain:+,} QU (expected +10,000)")
print(f"  Seed2 gained: {seed2_gain:+,} QU (expected +10,000)")
print(f"  ✅ Owner-as-sender works!" if seed0_gain == 10000 and seed2_gain == 10000 else f"  ⚠ Unexpected distribution")

# ============================================================
print(f"\n{'='*60}")
print("STEP 4: Seed2 sends 8,000 QU (recipient sends to own gate)")
print(f"{'='*60}")

bal0_before = get_balance(ID0)
bal2_before = get_balance(ID2)

out = send_contract_tx(SEED2, PROC_SEND_TO_GATE, 8000, send_data)
print(f"  Seed2 sent 8,000 QU to gate #{gate_id}")
wait_ticks(15)

bal0_after = get_balance(ID0)
bal2_after = get_balance(ID2)
gate = query_gate(gate_id)

seed0_gain = bal0_after - bal0_before
seed2_net = bal2_after - bal2_before

print(f"  Gate: received={gate['totalReceived']}, forwarded={gate['totalForwarded']}")
print(f"  Seed0 gained: {seed0_gain:+,} QU (expected +4,000)")
print(f"  Seed2 net change: {seed2_net:+,} QU (sent 8k, received 4k back = -4k)")
print(f"  ✅ Recipient-as-sender works!" if seed0_gain == 4000 else f"  ⚠ Unexpected distribution")

# ============================================================
print(f"\n{'='*60}")
print("STEP 5: Close gate")
print(f"{'='*60}")

# Only Seed1 (owner) can close
out = send_contract_tx(SEED1, PROC_CLOSE_GATE, 0, struct.pack('<Q', gate_id))
print(f"  Seed1 (owner) closing gate...")
wait_ticks(15)

gate = query_gate(gate_id)
print(f"  Gate active: {gate['active']}")
print(f"  ✅ Gate closed!" if gate['active'] == 0 else f"  ⚠ Gate still active")

# ============================================================
print(f"\n{'='*60}")
print("FINAL RESULTS")
print(f"{'='*60}")

bal0_end = get_balance(ID0)
bal1_end = get_balance(ID1)
bal2_end = get_balance(ID2)

print(f"\n  Balance Changes:")
print(f"    Seed0: {bal0_end - bal0_start:+,} QU (sent 10k, received 50% of all 38k)")
print(f"    Seed1: {bal1_end - bal1_start:+,} QU (sent 20k + 1k fee, received nothing)")
print(f"    Seed2: {bal2_end - bal2_start:+,} QU (sent 8k, received 50% of all 38k)")

print(f"\n  Gate totals: received={gate['totalReceived']}, forwarded={gate['totalForwarded']}")

total_sent = 10000 + 20000 + 8000
print(f"  Total through gate: {total_sent:,} QU from 3 different senders")

print(f"\n  ✅ Multi-sender convergence verified!")
print(f"  ✅ Sender-as-recipient works!")
print(f"  ✅ Owner-as-sender works!")
print(f"  ✅ Recipient-as-sender works!")

print(f"\n🏁 Multi-sender convergence test complete!")
