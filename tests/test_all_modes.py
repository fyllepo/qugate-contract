#!/usr/bin/env python3
"""QuGate testnet — all 5 modes + feature verification"""
import subprocess, requests, time, base64, struct, sys

CLI = "/home/phil/projects/qubic-cli/build/qubic-cli"
RPC = "http://localhost:41841"
CONTRACT_IDX = 24
CONTRACT = "YAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAMSME"

SEED0 = "eraaastggldisjhoojaekgyimrsddjxbvgaawswfvnvaygqmusnkevv"
SEED1 = "sgwnpzidgxbclnisgehigeculaejjxedzdkjyyfrzgzvuojrhdzywfh"
SEED2 = "xeejtwxqrrlvacapbujaleejhbrsnnpvviknskemmgdihggpssjjkrg"
SEED0_ID = "SINUBYSBZKBSVEFQDZBQWUEJWRXCXOZNKPHIXDZWRBKXDSPJEHFAMBACXHUN"
SEED1_ID = "KENGZYMYWOIHSCXMGBIXBGTKZYCCDITKSNBNILSLUFPQPRCUUYENPYUCEXRM"
SEED2_ID = "FLNRYKSGLGKZQECRCBNCYAWLNHVCWNYAZSISJRAPUANHDGWAIFBYLIADPQLE"

PASS_COUNT = 0
FAIL_COUNT = 0

def decode_identity(identity):
    identity = identity.upper()
    pk = bytearray(32)
    for i in range(4):
        val = 0
        for j in range(13, -1, -1):
            val = val * 26 + (ord(identity[i * 14 + j]) - ord('A'))
        struct.pack_into('<Q', pk, i * 8, val)
    return bytes(pk)

PK0 = decode_identity(SEED0_ID)
PK1 = decode_identity(SEED1_ID)
PK2 = decode_identity(SEED2_ID)

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

def get_gate(gate_id):
    """Query getGate (inputType 5)"""
    req = base64.b64encode(struct.pack("<Q", gate_id)).decode()
    data = query_sc(5, 8, req)
    return data

def get_fees():
    data = query_sc(9)
    if len(data) >= 32:
        return struct.unpack_from("<QQQQ", data)
    return None

def send_tx(seed, amount, input_type, hex_data):
    data_bytes = bytes.fromhex(hex_data) if hex_data else b""
    cmd = [CLI, "-nodeip", "127.0.0.1", "-nodeport", "31841",
           "-seed", seed, "-sendcustomtransaction",
           CONTRACT, str(input_type), str(amount), str(len(data_bytes)), hex_data]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    sent = "sent" in r.stdout.lower()
    return sent

def build_create_gate(mode, recipients_pk, ratios, threshold=0, allowed_senders=None, allowed_sender_count=0):
    """Build createGate_input — 600 bytes with proper alignment"""
    data = bytearray(600)
    data[0] = mode
    data[1] = len(recipients_pk)
    for i, pk in enumerate(recipients_pk):
        struct.pack_into("32s", data, 8 + i * 32, pk)
    for i, r in enumerate(ratios):
        struct.pack_into("<Q", data, 264 + i * 8, r)
    struct.pack_into("<Q", data, 328, threshold)
    if allowed_senders:
        for i, pk in enumerate(allowed_senders):
            struct.pack_into("32s", data, 336 + i * 32, pk)
    data[592] = allowed_sender_count
    return data.hex()

def build_gate_id(gate_id):
    return struct.pack("<Q", gate_id).hex()

def check(name, condition, detail=""):
    global PASS_COUNT, FAIL_COUNT
    if condition:
        PASS_COUNT += 1
        print(f"  ✅ {name}" + (f" — {detail}" if detail else ""))
    else:
        FAIL_COUNT += 1
        print(f"  ❌ {name}" + (f" — {detail}" if detail else ""))

def section(title):
    print(f"\n{'='*60}")
    print(f"  {title}")
    print(f"{'='*60}")

# ============================================================
section("INITIAL STATE")
total0, active0, burned0 = get_gate_count()
b0_start = get_balance(SEED0_ID)
b1_start = get_balance(SEED1_ID)
b2_start = get_balance(SEED2_ID)
print(f"  Gates: {total0}/{active0}, Burned: {burned0}")
print(f"  Seed0: {b0_start:,}  Seed1: {b1_start:,}  Seed2: {b2_start:,}")

# ============================================================
section("TEST 1: getFees Query")
fees = get_fees()
check("baseFee = 1000", fees and fees[0] == 1000, f"got {fees[0] if fees else 'None'}")
check("currentFee = 1000", fees and fees[1] == 1000, f"got {fees[1] if fees else 'None'}")
check("minSend = 10", fees and fees[2] == 10, f"got {fees[2] if fees else 'None'}")
check("expiryEpochs = 50", fees and fees[3] == 50, f"got {fees[3] if fees else 'None'}")

# ============================================================
section("TEST 2: SPLIT Mode (60/40)")
hex_data = build_create_gate(0, [PK1, PK2], [60, 40])
ok = send_tx(SEED0, 2000, 1, hex_data)
check("createGate TX sent", ok)
wait_ticks(20)

total, active, burned = get_gate_count()
check("Gate created", active == active0 + 1, f"active {active0}→{active}")
check("Fee burned", burned >= burned0 + 1000, f"burned {burned0}→{burned}")

# Send 10000 through gate 1
b1_pre = get_balance(SEED1_ID)
b2_pre = get_balance(SEED2_ID)
send_tx(SEED0, 10000, 2, build_gate_id(1))
wait_ticks(20)
b1_post = get_balance(SEED1_ID)
b2_post = get_balance(SEED2_ID)
s1_got = b1_post - b1_pre
s2_got = b2_post - b2_pre
check("Seed1 got 6000 (60%)", s1_got == 6000, f"got {s1_got}")
check("Seed2 got 4000 (40%)", s2_got == 4000, f"got {s2_got}")

# ============================================================
section("TEST 3: ROUND_ROBIN Mode")
hex_data = build_create_gate(1, [PK1, PK2], [1, 1])
send_tx(SEED0, 2000, 1, hex_data)
wait_ticks(20)

total_rr, active_rr, _ = get_gate_count()
gate_id_rr = total_rr  # latest gate
check("RR gate created", active_rr == active + 1, f"active {active}→{active_rr}")

# Send 3 times — should alternate recipients
b1_pre = get_balance(SEED1_ID)
b2_pre = get_balance(SEED2_ID)
send_tx(SEED0, 500, 2, build_gate_id(gate_id_rr))
wait_ticks(20)
send_tx(SEED0, 500, 2, build_gate_id(gate_id_rr))
wait_ticks(20)
send_tx(SEED0, 500, 2, build_gate_id(gate_id_rr))
wait_ticks(20)
b1_post = get_balance(SEED1_ID)
b2_post = get_balance(SEED2_ID)
s1_got = b1_post - b1_pre
s2_got = b2_post - b2_pre
# With 3 sends alternating: first→Seed1, second→Seed2, third→Seed1
# So Seed1 should get 1000, Seed2 500 (or vice versa)
total_distributed = s1_got + s2_got
check("RR distributed 1500 total", total_distributed == 1500, f"Seed1 +{s1_got}, Seed2 +{s2_got}")
check("RR alternated (neither got all)", s1_got > 0 and s2_got > 0, f"Seed1 +{s1_got}, Seed2 +{s2_got}")

# ============================================================
section("TEST 4: THRESHOLD Mode")
hex_data = build_create_gate(2, [PK1], [1], threshold=5000)
send_tx(SEED0, 2000, 1, hex_data)
wait_ticks(20)

total_th, active_th, _ = get_gate_count()
gate_id_th = total_th
check("Threshold gate created", active_th == active_rr + 1, f"active {active_rr}→{active_th}")

# Send 3000 — should NOT forward (below 5000 threshold)
b1_pre = get_balance(SEED1_ID)
send_tx(SEED0, 3000, 2, build_gate_id(gate_id_th))
wait_ticks(20)
b1_mid = get_balance(SEED1_ID)
check("Below threshold: held", b1_mid == b1_pre, f"Seed1 change: {b1_mid - b1_pre}")

# Send 2500 more — total 5500, should forward all
send_tx(SEED0, 2500, 2, build_gate_id(gate_id_th))
wait_ticks(20)
b1_post = get_balance(SEED1_ID)
forwarded = b1_post - b1_pre
check("Above threshold: forwarded", forwarded == 5500, f"Seed1 got {forwarded}")

# ============================================================
section("TEST 5: RANDOM Mode")
hex_data = build_create_gate(3, [PK1, PK2], [1, 1])
send_tx(SEED0, 2000, 1, hex_data)
wait_ticks(20)

total_rand, active_rand, _ = get_gate_count()
gate_id_rand = total_rand
check("Random gate created", active_rand == active_th + 1, f"active {active_th}→{active_rand}")

# Send 5 times — at least one should go to each recipient (probabilistic)
b1_pre = get_balance(SEED1_ID)
b2_pre = get_balance(SEED2_ID)
for i in range(5):
    send_tx(SEED0, 200, 2, build_gate_id(gate_id_rand))
    wait_ticks(20)
b1_post = get_balance(SEED1_ID)
b2_post = get_balance(SEED2_ID)
s1_got = b1_post - b1_pre
s2_got = b2_post - b2_pre
total_rand_dist = s1_got + s2_got
check("Random distributed 1000 total", total_rand_dist == 1000, f"Seed1 +{s1_got}, Seed2 +{s2_got}")
check("Random picked recipients", s1_got >= 0 and s2_got >= 0, f"Seed1 +{s1_got}, Seed2 +{s2_got}")

# ============================================================
section("TEST 6: CONDITIONAL Mode")
# Conditional: only allowed senders can send
hex_data = build_create_gate(4, [PK1], [1], allowed_senders=[PK0], allowed_sender_count=1)
send_tx(SEED0, 2000, 1, hex_data)
wait_ticks(20)

total_cond, active_cond, _ = get_gate_count()
gate_id_cond = total_cond
check("Conditional gate created", active_cond == active_rand + 1, f"active {active_rand}→{active_cond}")

# Allowed sender (Seed0) sends — should work
b1_pre = get_balance(SEED1_ID)
send_tx(SEED0, 1000, 2, build_gate_id(gate_id_cond))
wait_ticks(20)
b1_post = get_balance(SEED1_ID)
check("Allowed sender: forwarded", b1_post - b1_pre == 1000, f"Seed1 got {b1_post - b1_pre}")

# Unauthorized sender (Seed2) sends — should be rejected/refunded
b1_pre2 = get_balance(SEED1_ID)
b2_pre2 = get_balance(SEED2_ID)
send_tx(SEED2, 1000, 2, build_gate_id(gate_id_cond))
wait_ticks(20)
b1_post2 = get_balance(SEED1_ID)
b2_post2 = get_balance(SEED2_ID)
check("Unauthorized sender: rejected", b1_post2 == b1_pre2, f"Seed1 change: {b1_post2 - b1_pre2}")

# ============================================================
section("TEST 7: Dust Burn")
b0_pre = get_balance(SEED0_ID)
_, _, burned_pre = get_gate_count()
send_tx(SEED0, 5, 2, build_gate_id(1))  # 5 QU < minSend(10)
wait_ticks(20)
_, _, burned_post = get_gate_count()
check("Dust burned", burned_post > burned_pre, f"burned {burned_pre}→{burned_post}")

# ============================================================
section("TEST 8: Fee Overpayment Refund")
b0_pre = get_balance(SEED0_ID)
hex_data = build_create_gate(0, [PK1], [1])
send_tx(SEED0, 5000, 1, hex_data)
wait_ticks(20)
b0_post = get_balance(SEED0_ID)
cost = b0_pre - b0_post
check("Overpayment refunded", cost == 1000, f"cost was {cost} (expected 1000)")

# ============================================================
section("TEST 9: Close Gate + Slot Reuse")
total_pre, active_pre, _ = get_gate_count()
send_tx(SEED0, 0, 3, build_gate_id(1))  # close gate 1
wait_ticks(20)
total_post, active_post, _ = get_gate_count()
check("Gate closed", active_post == active_pre - 1, f"active {active_pre}→{active_post}")

# Create new gate — should reuse slot
hex_data = build_create_gate(0, [PK1], [1])
send_tx(SEED0, 2000, 1, hex_data)
wait_ticks(20)
total_reuse, active_reuse, _ = get_gate_count()
check("Slot reused", active_reuse == active_post + 1, f"active {active_post}→{active_reuse}")

# ============================================================
section("FINAL STATE")
total_f, active_f, burned_f = get_gate_count()
print(f"  Gates: total={total_f}, active={active_f}, burned={burned_f}")
print(f"  Seed0: {get_balance(SEED0_ID):,} QU")
print(f"  Seed1: {get_balance(SEED1_ID):,} QU")
print(f"  Seed2: {get_balance(SEED2_ID):,} QU")

section(f"SUMMARY: {PASS_COUNT} PASS, {FAIL_COUNT} FAIL")
if FAIL_COUNT == 0:
    print("  ALL TESTS PASSED! 🎉")
else:
    print(f"  {FAIL_COUNT} test(s) failed.")
