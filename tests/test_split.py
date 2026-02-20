#!/usr/bin/env python3
"""
QuGate Testnet Scenario Tests
Uses qubic-cli for transactions, HTTP RPC for queries.
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

# Input types
PROC_CREATE_GATE = 1
PROC_SEND_TO_GATE = 2
PROC_CLOSE_GATE = 3

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
            if attempt < 4:
                time.sleep(3)
    raise Exception("Node not responding after 5 attempts")

def query_gate_count():
    for attempt in range(5):
        try:
            resp = requests.post(f"{RPC}/live/v1/querySmartContract", json={
                'contractIndex': QUGATE_INDEX, 'inputType': 6, 'inputSize': 0, 'requestData': ''
            }, timeout=5).json()
            b = base64.b64decode(resp['responseData'])
            t, a = struct.unpack('<QQ', b[:16])
            return t, a
        except:
            if attempt < 4:
                time.sleep(3)
            else:
                restart_node()
                return query_gate_count()

def query_gate(gate_id):
    data = struct.pack('<Q', gate_id)
    for attempt in range(5):
        try:
            resp = requests.post(f"{RPC}/live/v1/querySmartContract", json={
                'contractIndex': QUGATE_INDEX, 'inputType': 5, 'inputSize': len(data),
                'requestData': base64.b64encode(data).decode()
            }, timeout=5).json()
            b = base64.b64decode(resp['responseData'])
            break
        except:
            if attempt < 4:
                time.sleep(3)
            else:
                restart_node()
                return query_gate(gate_id)
    else:
        raise Exception("Failed to query gate")
    modes = ['SPLIT', 'ROUND_ROBIN', 'THRESHOLD', 'RANDOM', 'CONDITIONAL']
    off = 40
    tr, tf, cb, th, ep = struct.unpack_from('<QQQQQ', b, off)
    # Get ratios at offset 80 + 8*32 = 336
    ratios = [struct.unpack_from('<Q', b, 336 + i*8)[0] for i in range(8)]
    return {
        'mode': modes[b[0]], 'recipientCount': b[1], 'active': b[2],
        'owner': b[8:40].hex(), 'totalReceived': tr, 'totalForwarded': tf,
        'currentBalance': cb, 'threshold': th, 'createdEpoch': ep,
        'ratios': ratios[:b[1]]
    }

def get_pubkey_from_identity(identity):
    """Reverse Qubic base26 identity (first 56 chars) to 32-byte pubkey"""
    pk = bytearray(32)
    for i in range(4):
        val = 0
        for j in range(13, -1, -1):
            c = identity[i * 14 + j]
            val = val * 26 + (ord(c) - ord('A'))
        struct.pack_into('<Q', pk, i * 8, val)
    return bytes(pk)

def build_create_gate(mode, recipients_pk, ratios, threshold=0, allowed_senders=None):
    """Build createGate_input with proper alignment"""
    data = bytearray()
    data += struct.pack('<B', mode)
    data += struct.pack('<B', len(recipients_pk))
    data += bytes(6)  # padding for id alignment
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

def build_send_to_gate(gate_id):
    return struct.pack('<Q', gate_id)

def build_close_gate(gate_id):
    return struct.pack('<Q', gate_id)

def send_contract_tx(key, input_type, amount, input_data):
    """Send a transaction to the QuGate contract using qubic-cli"""
    hex_data = input_data.hex()
    out = cli("-seed", key, "-sendcustomtransaction",
              CONTRACT_ID, str(input_type), str(amount),
              str(len(input_data)), hex_data)
    return out

def wait_ticks(n=15):
    """Wait for n ticks to pass, with node crash recovery"""
    try:
        start = get_tick()
    except:
        print("    Node down, restarting...")
        restart_node()
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
            print("    Node crashed, restarting...")
            restart_node()
    print(f"    ⚠ Timeout waiting for ticks")
    return False

def restart_node():
    """Restart the Qubic node"""
    subprocess.run(["pkill", "-9", "Qubic"], capture_output=True)
    time.sleep(2)
    subprocess.Popen(
        ["./Qubic", "--ticking-delay", "5000"],
        cwd=os.environ.get("CORE_LITE_DIR", "."),
        stdout=open("/tmp/qubic.log", "w"),
        stderr=subprocess.STDOUT
    )
    # Wait for ready
    for i in range(60):
        time.sleep(3)
        try:
            t = requests.get(f"{RPC}/live/v1/tick-info", timeout=2).json().get('tick', 0)
            if t > 43910000:
                print(f"    Node restarted at tick {t}")
                return
        except:
            pass
    raise Exception("Failed to restart node")

def print_balances():
    for name, key in [("Address A", ADDR_A_KEY), ("Address B", ADDR_B_KEY), ("Address C", ADDR_C_KEY)]:
        identity = get_identity(key)
        bal = get_balance(identity)
        print(f"    {name} ({identity[:12]}...): {bal:,} QU")

# ============================================================
# MAIN
# ============================================================
print("╔══════════════════════════════════════════════════╗")
print("║      QuGate Testnet Scenario Tests               ║")
print("╚══════════════════════════════════════════════════╝\n")

# Wait for node
print("Checking node...")
try:
    tick = get_tick()
    print(f"  Node up at tick {tick}\n")
except:
    print("  Node not responding! Start it first.")
    sys.exit(1)

# Get identities and pubkeys
ADDR_A = get_identity(ADDR_A_KEY)
ADDR_B = get_identity(ADDR_B_KEY)
ADDR_C = get_identity(ADDR_C_KEY)
PK_B = get_pubkey_from_identity(ADDR_B)
PK_C = get_pubkey_from_identity(ADDR_C)

print(f"Address A: {ADDR_A}")
print(f"Address B: {ADDR_B}")
print(f"Address C: {ADDR_C}")
print(f"Contract: {CONTRACT_ID}")

# Initial state
print("\n━━━ Initial State ━━━")
total, active = query_gate_count()
print(f"  Gates: total={total}, active={active}")
print_balances()

# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
# SCENARIO 1: Create SPLIT gate (60/40)
# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
print("\n" + "="*50)
print("SCENARIO 1: Create SPLIT gate (60% → Address B, 40% → Address C)")
print("="*50)

input_data = build_create_gate(mode=0, recipients_pk=[PK_B, PK_C], ratios=[60, 40])
print(f"  Input size: {len(input_data)} bytes")
print(f"  Sending createGate tx (1000 QU fee)...")
out = send_contract_tx(ADDR_A_KEY, PROC_CREATE_GATE, 1000, input_data)
print(f"  CLI: {out.strip()}")

wait_ticks(15)

total, active = query_gate_count()
print(f"\n  Gate count: total={total}, active={active}")

# Find the newest gate
gate = query_gate(total)
print(f"  Gate #{total}: {gate}")

if gate['active'] and gate['ratios'] == [60, 40]:
    print("  ✅ SPLIT gate created with correct ratios!")
    SPLIT_GATE_ID = total
else:
    print(f"  ⚠ Gate created but ratios={gate['ratios']}")
    SPLIT_GATE_ID = total

# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
# SCENARIO 2: Send 10000 QU through SPLIT gate
# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
print("\n" + "="*50)
print(f"SCENARIO 2: Send 10,000 QU through gate #{SPLIT_GATE_ID}")
print("="*50)

bal_before_1 = get_balance(ADDR_B)
bal_before_2 = get_balance(ADDR_C)
print(f"  Address B before: {bal_before_1:,} QU")
print(f"  Address C before: {bal_before_2:,} QU")

input_data = build_send_to_gate(SPLIT_GATE_ID)
print(f"  Sending 10,000 QU to gate #{SPLIT_GATE_ID}...")
out = send_contract_tx(ADDR_A_KEY, PROC_SEND_TO_GATE, 10000, input_data)
print(f"  CLI: {out.strip()}")

wait_ticks(15)

gate = query_gate(SPLIT_GATE_ID)
print(f"\n  Gate #{SPLIT_GATE_ID}: received={gate['totalReceived']}, forwarded={gate['totalForwarded']}, balance={gate['currentBalance']}")

bal_after_1 = get_balance(ADDR_B)
bal_after_2 = get_balance(ADDR_C)
gain1 = bal_after_1 - bal_before_1
gain2 = bal_after_2 - bal_before_2
print(f"  Address B after: {bal_after_1:,} QU (gained {gain1})")
print(f"  Address C after: {bal_after_2:,} QU (gained {gain2})")

if gain1 == 6000 and gain2 == 4000:
    print("  ✅ Perfect 60/40 split!")
elif gain1 + gain2 == 10000:
    print(f"  ✅ Split worked! Ratio: {gain1}/{gain2} ({gain1*100//10000}%/{gain2*100//10000}%)")
else:
    print(f"  ⚠ Unexpected: gains don't add to 10000 (got {gain1}+{gain2}={gain1+gain2})")

# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
# SCENARIO 3: Send more QU - verify consistency
# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
print("\n" + "="*50)
print(f"SCENARIO 3: Send 50,000 QU through same gate")
print("="*50)

bal_before_1 = get_balance(ADDR_B)
bal_before_2 = get_balance(ADDR_C)

input_data = build_send_to_gate(SPLIT_GATE_ID)
print(f"  Sending 50,000 QU...")
out = send_contract_tx(ADDR_A_KEY, PROC_SEND_TO_GATE, 50000, input_data)
print(f"  CLI: {out.strip()}")

wait_ticks(15)

gate = query_gate(SPLIT_GATE_ID)
print(f"\n  Gate: received={gate['totalReceived']}, forwarded={gate['totalForwarded']}")

bal_after_1 = get_balance(ADDR_B)
bal_after_2 = get_balance(ADDR_C)
gain1 = bal_after_1 - bal_before_1
gain2 = bal_after_2 - bal_before_2
print(f"  Address B gained: {gain1} (expected 30000)")
print(f"  Address C gained: {gain2} (expected 20000)")

if gain1 == 30000 and gain2 == 20000:
    print("  ✅ Perfect 60/40 split on 50k!")
else:
    print(f"  Result: {gain1}+{gain2}={gain1+gain2}")

# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
# SCENARIO 4: Close the gate
# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
print("\n" + "="*50)
print(f"SCENARIO 4: Close gate #{SPLIT_GATE_ID}")
print("="*50)

input_data = build_close_gate(SPLIT_GATE_ID)
print(f"  Sending closeGate tx...")
out = send_contract_tx(ADDR_A_KEY, PROC_CLOSE_GATE, 0, input_data)
print(f"  CLI: {out.strip()}")

wait_ticks(15)

gate = query_gate(SPLIT_GATE_ID)
print(f"  Gate #{SPLIT_GATE_ID} active: {gate['active']}")
total, active = query_gate_count()
print(f"  Gate count: total={total}, active={active}")

if not gate['active']:
    print("  ✅ Gate successfully closed!")
else:
    print("  ⚠ Gate still active")

# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
# FINAL SUMMARY
# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
print("\n" + "="*50)
print("FINAL STATE")
print("="*50)
total, active = query_gate_count()
print(f"  Gates: total={total}, active={active}")
print_balances()

print("\n🏁 All scenarios complete!")
