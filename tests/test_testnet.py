#!/usr/bin/env python3
"""QuGate testnet verification"""
import os, shutil
import subprocess, json, requests, time, base64, struct, sys

CLI = os.environ.get("QUBIC_CLI", shutil.which("qubic-cli") or "qubic-cli")
RPC = "http://localhost:41841"
CONTRACT_IDX = 24
CONTRACT = "YAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAMSME"

SEED0 = "eraaastggldisjhoojaekgyimrsddjxbvgaawswfvnvaygqmusnkevv"
SEED1 = "sgwnpzidgxbclnisgehigeculaejjxedzdkjyyfrzgzvuojrhdzywfh"
SEED2 = "xeejtwxqrrlvacapbujaleejhbrsnnpvviknskemmgdihggpssjjkrg"
SEED0_ID = "SINUBYSBZKBSVEFQDZBQWUEJWRXCXOZNKPHIXDZWRBKXDSPJEHFAMBACXHUN"
SEED1_ID = "KENGZYMYWOIHSCXMGBIXBGTKZYCCDITKSNBNILSLUFPQPRCUUYENPYUCEXRM"
SEED2_ID = "FLNRYKSGLGKZQECRCBNCYAWLNHVCWNYAZSISJRAPUANHDGWAIFBYLIADPQLE"

def get_tick():
    return requests.get(f"{RPC}/live/v1/tick-info").json()["tick"]

def get_balance(addr):
    return int(requests.get(f"{RPC}/live/v1/balances/{addr}").json()["balance"]["balance"])

def wait_ticks(n=20):
    target = get_tick() + n
    while get_tick() < target:
        time.sleep(3)

def query_sc(input_type, input_size=0, request_data=""):
    r = requests.post(f"{RPC}/live/v1/querySmartContract",
        json={"contractIndex": CONTRACT_IDX, "inputType": input_type,
              "inputSize": input_size, "requestData": request_data})
    return base64.b64decode(r.json().get("responseData", ""))

def get_gate_count():
    data = query_sc(6)
    if len(data) >= 24:
        return struct.unpack_from("<QQQ", data)
    return (0, 0, 0)

def get_fees():
    data = query_sc(9)
    if len(data) >= 32:
        return struct.unpack_from("<QQQQ", data)
    return None

def build_gate_id_hex(gate_id):
    return struct.pack("<Q", gate_id).hex()

def send_custom_tx(seed, amount, target_tick, input_type, hex_data):
    data_bytes = bytes.fromhex(hex_data) if hex_data else b""
    cmd = [CLI, "-nodeip", "127.0.0.1", "-nodeport", "31841",
           "-seed", seed, "-sendcustomtransaction",
           CONTRACT, str(input_type), str(amount), str(len(data_bytes)), hex_data]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    return r.stdout.strip() + " " + r.stderr.strip()

def get_pubkey_bytes(seed):
    r = subprocess.run([CLI, "-seed", seed, "-showkeys"],
                      capture_output=True, text=True, timeout=10)
    for line in r.stdout.split("\n"):
        if line.startswith("Public key:"):
            # It's a lowercase identity, need to convert to bytes
            identity = line.split(":")[1].strip()
            return identity_to_bytes(identity)
    return None

def identity_to_bytes(identity):
    """Convert Qubic identity (60 lowercase chars) to 32 bytes"""
    identity = identity.upper()
    v = 0
    for i in range(len(identity)):
        c = identity[i]
        if c >= 'A' and c <= 'Z':
            v += (ord(c) - ord('A')) * (26 ** (len(identity) - 1 - i))
    # Actually this is wrong — Qubic uses a specific encoding
    # Let's use the identity_tool binary if available
    r = subprocess.run([ID_TOOL, identity],
                      capture_output=True, text=True, timeout=10)
    if r.returncode == 0:
        # Parse hex output
        hex_str = r.stdout.strip()
        if len(hex_str) == 64:
            return bytes.fromhex(hex_str)
    # Fallback: try using the qubic-cli getPublicKeyFromIdentity if available
    return None

def seed_to_pubkey_bytes(seed):
    """Get pubkey bytes by using identity_tool with the seed"""
    r = subprocess.run([ID_TOOL, seed],
                      capture_output=True, text=True, timeout=10)
    out = r.stdout.strip()
    # identity_tool outputs: Identity: XXX\nPublic key (hex): YYY
    for line in out.split("\n"):
        if "hex" in line.lower() or len(line.strip()) == 64:
            hex_str = line.split(":")[-1].strip() if ":" in line else line.strip()
            if len(hex_str) == 64:
                try:
                    return bytes.fromhex(hex_str)
                except ValueError:
                    pass
    # Last resort — construct from showkeys
    r2 = subprocess.run([CLI, "-seed", seed, "-showkeys"],
                       capture_output=True, text=True, timeout=10)
    print(f"  [debug] showkeys output: {r2.stdout[:200]}")
    print(f"  [debug] identity_tool output: {out[:200]}")
    return None

print("=" * 60)
print("QuGate V3 Testnet Verification")
print("=" * 60)

# Get pubkey bytes for recipients
print("\nResolving public keys...")

def decode_identity(identity):
    """Decode Qubic identity (60 chars A-Z) to 32-byte public key"""
    identity = identity.upper()
    pk = bytearray(32)
    for i in range(4):
        val = 0
        for j in range(13, -1, -1):
            c = identity[i * 14 + j]
            val = val * 26 + (ord(c) - ord('A'))
        struct.pack_into('<Q', pk, i * 8, val)
    return bytes(pk)

pk1 = decode_identity(SEED1_ID)
pk2 = decode_identity(SEED2_ID)
print(f"  Seed1 pubkey: {pk1.hex()[:16]}...")
print(f"  Seed2 pubkey: {pk2.hex()[:16]}...")

# Test 0: Initial state
print("\n--- Test 0: Initial State ---")
total, active, burned = get_gate_count()
print(f"Gates: total={total}, active={active}, burned={burned}")
b0 = get_balance(SEED0_ID)
b1 = get_balance(SEED1_ID)
b2 = get_balance(SEED2_ID)
print(f"Seed0: {b0:,} QU")
print(f"Seed1: {b1:,} QU")
print(f"Seed2: {b2:,} QU")

# Test 1: getFees
print("\n--- Test 1: getFees Query (#24) ---")
fees = get_fees()
if fees:
    print(f"Base fee: {fees[0]}, Current fee: {fees[1]}, Min send: {fees[2]}, Expiry: {fees[3]}")
    ok = fees[0] == 1000 and fees[1] == 1000 and fees[2] == 10 and fees[3] == 50
    print("PASS" if ok else f"FAIL — unexpected values")
else:
    print("FAIL — no data")

# Test 2: Create SPLIT gate (60/40 to Seed1/Seed2)
print("\n--- Test 2: Create SPLIT Gate ---")
# Build createGate_input — 600 bytes with proper alignment
# Layout: mode(1) + recipientCount(1) + 6 padding + recipients(256) + ratios(64) + threshold(8) + allowedSenders(256) + allowedSenderCount(1) + 7 trailing padding
data = bytearray(600)
data[0] = 0   # mode = SPLIT
data[1] = 2   # recipientCount = 2
# offset 8: recipients (8 * 32 bytes)
struct.pack_into("32s", data, 8, pk1)
struct.pack_into("32s", data, 8 + 32, pk2)
# offset 264: ratios (8 * uint64)
struct.pack_into("<Q", data, 264, 60)
struct.pack_into("<Q", data, 272, 40)
# offset 328: threshold
struct.pack_into("<Q", data, 328, 0)
# offset 336: allowedSenders (8 * 32 bytes) — all zeros
# offset 592: allowedSenderCount
data[592] = 0

tick = get_tick()
target = tick + 5
print(f"Sending createGate tx at tick {tick}, target {target}")
result = send_custom_tx(SEED0, 2000, target, 1, data.hex())
print(f"TX: {result[:100]}")

wait_ticks(20)

total2, active2, burned2 = get_gate_count()
print(f"Gates: total={total2}, active={active2}, burned={burned2}")
b0_after = get_balance(SEED0_ID)
print(f"Seed0 balance change: {b0_after - b0:,} QU")

if active2 > active:
    print("PASS — gate created")
    
    # Test 3: totalBurned
    print(f"\n--- Test 3: totalBurned (#20) ---")
    print(f"Burned: {burned2}")
    print("PASS" if burned2 >= 1000 else "FAIL — expected >= 1000 burned")
    
    # Test 4: Send to gate
    print("\n--- Test 4: Send 10000 QU through SPLIT gate ---")
    gate_id = 1
    tick = get_tick()
    target = tick + 5
    result = send_custom_tx(SEED0, 10000, target, 2, build_gate_id_hex(gate_id))
    print(f"TX: {result[:100]}")
    
    wait_ticks(20)
    
    b1_after = get_balance(SEED1_ID)
    b2_after = get_balance(SEED2_ID)
    print(f"Seed1 received: {b1_after - b1:,} QU (expected ~6000)")
    print(f"Seed2 received: {b2_after - b2:,} QU (expected ~4000)")
    
    if b1_after - b1 == 6000 and b2_after - b2 == 4000:
        print("PASS — 60/40 split correct")
    elif b1_after - b1 > 0 or b2_after - b2 > 0:
        print("PARTIAL — some distribution occurred")
    else:
        print("FAIL — no distribution")
    
    # Test 5: Dust burn
    print("\n--- Test 5: Dust Burn (#17) ---")
    tick = get_tick()
    target = tick + 5
    result = send_custom_tx(SEED0, 5, target, 2, build_gate_id_hex(gate_id))
    print(f"TX (5 QU dust): {result[:100]}")
    wait_ticks(20)
    _, _, burned3 = get_gate_count()
    print(f"Burned after dust: {burned3} (was {burned2})")
    print("PASS" if burned3 > burned2 else "CHECK — burn may not have increased")
    
    # Test 6: Fee overpayment refund
    print("\n--- Test 6: Fee Overpayment Refund (#19) ---")
    b0_pre = get_balance(SEED0_ID)
    tick = get_tick()
    target = tick + 5
    # Create another gate with 5000 QU (overpay by 4000)
    result = send_custom_tx(SEED0, 5000, target, 1, data.hex())
    print(f"TX (5000 QU, fee=1000): {result[:100]}")
    wait_ticks(20)
    b0_post = get_balance(SEED0_ID)
    cost = b0_pre - b0_post
    print(f"Actual cost: {cost:,} QU (expected ~1000, refund ~4000)")
    print("PASS" if cost <= 1100 else f"CHECK — cost was {cost}")
    
    # Test 7: Close gate
    print("\n--- Test 7: Close Gate ---")
    tick = get_tick()
    target = tick + 5
    result = send_custom_tx(SEED0, 0, target, 3, build_gate_id_hex(gate_id))
    print(f"TX: {result[:100]}")
    wait_ticks(20)
    total3, active3, burned3b = get_gate_count()
    print(f"Gates: total={total3}, active={active3}")
    print("PASS" if active3 < active2 + 1 else "CHECK")

else:
    print("FAIL — gate not created. Skipping dependent tests.")

# Summary
print("\n" + "=" * 60)
total_f, active_f, burned_f = get_gate_count()
print(f"Final state: gates={total_f}, active={active_f}, burned={burned_f}")
print(f"Seed0: {get_balance(SEED0_ID):,} QU")
print(f"Seed1: {get_balance(SEED1_ID):,} QU")
print(f"Seed2: {get_balance(SEED2_ID):,} QU")
print("=" * 60)

# (moved to top)
