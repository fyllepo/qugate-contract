#!/usr/bin/env python3
"""
QuGate — All 5 Gate Modes Test

Tests: SPLIT, ROUND_ROBIN, THRESHOLD, RANDOM, CONDITIONAL
Plus: updateGate, closeGate, non-owner rejection
"""
import struct, subprocess, json, base64, time, requests, sys

CLI = "/home/phil/projects/qubic-cli/build/qubic-cli"
NODE_ARGS = ["-nodeip", "127.0.0.1", "-nodeport", "31841"]
RPC = "http://127.0.0.1:41841"
CONTRACT_INDEX = 24
CONTRACT_ID = "YAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAMSME"

SEED0 = "eraaastggldisjhoojaekgyimrsddjxbvgaawswfvnvaygqmusnkevv"
SEED1 = "sgwnpzidgxbclnisgehigeculaejjxedzdkjyyfrzgzvuojrhdzywfh"
SEED2 = "xeejtwxqrrlvacapbujaleejhbrsnnpvviknskemmgdihggpssjjkrg"

CREATION_FEE = 100000
MIN_SEND = 1000

MODE_SPLIT = 0
MODE_ROUND_ROBIN = 1
MODE_THRESHOLD = 2
MODE_RANDOM = 3
MODE_CONDITIONAL = 4

PROC_CREATE = 1
PROC_SEND = 2
PROC_CLOSE = 3
PROC_UPDATE = 4
FUNC_GET_GATE = 5
FUNC_GET_COUNT = 6
FUNC_GET_FEES = 9

passed = 0
failed = 0

def cli(*args, timeout=15):
    r = subprocess.run([CLI] + NODE_ARGS + list(args), capture_output=True, text=True, timeout=timeout)
    return r.stdout + r.stderr

def get_identity(seed):
    out = cli("-seed", seed, "-showkeys")
    for line in out.splitlines():
        if "Identity:" in line:
            return line.split("Identity:")[1].strip()
    raise Exception("Cannot get identity")

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

def get_pubkey(identity):
    pk = bytearray(32)
    for i in range(4):
        val = 0
        for j in range(13, -1, -1):
            c = identity[i * 14 + j]
            val = val * 26 + (ord(c) - ord('A'))
        struct.pack_into('<Q', pk, i * 8, val)
    return bytes(pk)

def query_gate(gate_id):
    data = struct.pack('<Q', gate_id)
    resp = requests.post(f"{RPC}/live/v1/querySmartContract", json={
        'contractIndex': CONTRACT_INDEX, 'inputType': FUNC_GET_GATE, 'inputSize': len(data),
        'requestData': base64.b64encode(data).decode()
    }, timeout=5).json()
    b = base64.b64decode(resp['responseData'])
    modes = ['SPLIT', 'ROUND_ROBIN', 'THRESHOLD', 'RANDOM', 'CONDITIONAL']
    # Layout: mode(1) rcpCount(1) active(1) pad(5) owner(32) [offset 40] totRcv(8) totFwd(8) curBal(8) thresh(8) createdEp(2) lastEp(2) pad(4) [offset 80] recipients(256) ratios(64) = 400 bytes
    tr, tf, cb, th = struct.unpack_from('<QQQQ', b, 40)
    ratios = [struct.unpack_from('<Q', b, 336 + i*8)[0] for i in range(8)]
    return {
        'mode': b[0], 'mode_name': modes[b[0]] if b[0] < 5 else f'?{b[0]}',
        'recipientCount': b[1], 'active': b[2],
        'totalReceived': tr, 'totalForwarded': tf,
        'currentBalance': cb, 'threshold': th,
        'ratios': ratios[:max(b[1], 1)],
    }

def query_count():
    resp = requests.post(f"{RPC}/live/v1/querySmartContract", json={
        'contractIndex': CONTRACT_INDEX, 'inputType': FUNC_GET_COUNT, 'inputSize': 0, 'requestData': ''
    }, timeout=5).json()
    b = base64.b64decode(resp['responseData'])
    t, a = struct.unpack('<QQ', b[:16])
    return t, a

def build_create(mode, recipients_pk, ratios, threshold=0, allowed_senders=None):
    data = bytearray()
    data += struct.pack('<B', mode)
    data += struct.pack('<B', len(recipients_pk))
    data += bytes(6)  # padding
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

def build_update(gate_id, recipients_pk, ratios, threshold=0, allowed_senders=None):
    data = bytearray()
    data += struct.pack('<Q', gate_id)
    data += struct.pack('<B', len(recipients_pk))
    data += bytes(7)  # padding
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
    data += bytes(7)  # trailing padding
    assert len(data) == 608, f"Expected 608, got {len(data)}"
    return bytes(data)

def send_tx(seed, proc, amount, data):
    return cli("-seed", seed, "-sendcustomtransaction",
               CONTRACT_ID, str(proc), str(amount),
               str(len(data)), data.hex())

def wait(n=15):
    start = get_tick()
    target = start + n
    print(f"    waiting for tick {target}...")
    for _ in range(180):
        time.sleep(4)
        try:
            if get_tick() >= target:
                return True
        except:
            time.sleep(5)
    print(f"    ⚠ timeout!")
    return False

def check(name, condition, detail=""):
    global passed, failed
    if condition:
        passed += 1
        print(f"  ✅ {name}" + (f" — {detail}" if detail else ""))
    else:
        failed += 1
        print(f"  ❌ {name}" + (f" — {detail}" if detail else ""))

# ============================================================
print("=" * 60)
print("QuGate — Full 5-Mode Testnet Test")
print("=" * 60)
print()

ID0 = get_identity(SEED0)
ID1 = get_identity(SEED1)
ID2 = get_identity(SEED2)
PK0 = get_pubkey(ID0)
PK1 = get_pubkey(ID1)
PK2 = get_pubkey(ID2)

tick = get_tick()
print(f"Node: tick={tick}, epoch=200")
print(f"Addr A: {ID0}")
print(f"Addr B: {ID1}")
print(f"Addr C: {ID2}")
print()

# ============================================================
# TEST 1: SPLIT mode (60/40)
# ============================================================
print("─" * 60)
print("TEST 1: SPLIT mode (60/40)")
print("─" * 60)

before_total, _ = query_count()
data = build_create(MODE_SPLIT, [PK1, PK2], [60, 40])
send_tx(SEED0, PROC_CREATE, CREATION_FEE, data)
wait()

total, active = query_count()
gate_id = total
print(f"  (before={before_total}, after={total})")
gate = query_gate(gate_id)
check("Gate created", gate['active'] == 1, f"id={gate_id}, mode={gate['mode_name']}")
check("Ratios correct", gate['ratios'] == [60, 40], f"ratios={gate['ratios']}")

bal1_before = get_balance(ID1)
bal2_before = get_balance(ID2)

send_tx(SEED0, PROC_SEND, 10000, struct.pack('<Q', gate_id))
wait()

s1 = get_balance(ID1) - bal1_before
s2 = get_balance(ID2) - bal2_before
check("Split 60/40", s1 == 6000 and s2 == 4000, f"got {s1}/{s2}")

# ============================================================
# TEST 2: ROUND_ROBIN mode
# ============================================================
print()
print("─" * 60)
print("TEST 2: ROUND_ROBIN mode")
print("─" * 60)

data = build_create(MODE_ROUND_ROBIN, [PK1, PK2], [1, 1])
send_tx(SEED0, PROC_CREATE, CREATION_FEE, data)
wait()

total, _ = query_count()
rr_id = total
print(f"  (total={total})")
gate = query_gate(rr_id)
check("RR gate created", gate['active'] == 1, f"id={rr_id}")

# Send twice — should alternate
bal1_before = get_balance(ID1)
bal2_before = get_balance(ID2)

send_tx(SEED0, PROC_SEND, 5000, struct.pack('<Q', rr_id))
wait()

s1a = get_balance(ID1) - bal1_before
s2a = get_balance(ID2) - bal2_before

bal1_before = get_balance(ID1)
bal2_before = get_balance(ID2)

send_tx(SEED0, PROC_SEND, 5000, struct.pack('<Q', rr_id))
wait()

s1b = get_balance(ID1) - bal1_before
s2b = get_balance(ID2) - bal2_before

# One should get 5000 then the other
check("RR alternates", (s1a == 5000 and s2b == 5000) or (s2a == 5000 and s1b == 5000),
      f"send1: {s1a}/{s2a}, send2: {s1b}/{s2b}")

# ============================================================
# TEST 3: THRESHOLD mode
# ============================================================
print()
print("─" * 60)
print("TEST 3: THRESHOLD mode (threshold=15000)")
print("─" * 60)

data = build_create(MODE_THRESHOLD, [PK1, PK2], [50, 50], threshold=15000)
send_tx(SEED0, PROC_CREATE, CREATION_FEE, data)
wait()

total, _ = query_count()
th_id = total
print(f"  (total={total})")
gate = query_gate(th_id)
check("Threshold gate created", gate['active'] == 1 and gate['threshold'] == 15000)

# Send 10k — below threshold, should accumulate
bal1_before = get_balance(ID1)
bal2_before = get_balance(ID2)

send_tx(SEED0, PROC_SEND, 10000, struct.pack('<Q', th_id))
wait()

gate = query_gate(th_id)
check("Below threshold: held", gate['currentBalance'] == 10000, f"balance={gate['currentBalance']}")

s1 = get_balance(ID1) - bal1_before
s2 = get_balance(ID2) - bal2_before
check("No distribution yet", s1 == 0 and s2 == 0, f"got {s1}/{s2}")

# Send 10k more — 20k total, above threshold, should flush all to recipient[0]
bal1_before = get_balance(ID1)

send_tx(SEED0, PROC_SEND, 10000, struct.pack('<Q', th_id))
wait()

gate = query_gate(th_id)
s1 = get_balance(ID1) - bal1_before
check("Above threshold: flushed to recipient[0]", s1 == 20000 and gate['currentBalance'] == 0,
      f"recipient[0] got {s1}, gate balance={gate['currentBalance']}")

# ============================================================
# TEST 4: RANDOM mode
# ============================================================
print()
print("─" * 60)
print("TEST 4: RANDOM mode")
print("─" * 60)

data = build_create(MODE_RANDOM, [PK1, PK2], [50, 50])
send_tx(SEED0, PROC_CREATE, CREATION_FEE, data)
wait()

total, _ = query_count()
rand_id = total
print(f"  (total={total})")
gate = query_gate(rand_id)
check("Random gate created", gate['active'] == 1)

# Send 3 times with waits between to ensure separate ticks
bal1_before = get_balance(ID1)
bal2_before = get_balance(ID2)

send_tx(SEED0, PROC_SEND, MIN_SEND, struct.pack('<Q', rand_id))
wait()
send_tx(SEED0, PROC_SEND, MIN_SEND, struct.pack('<Q', rand_id))
wait()
send_tx(SEED0, PROC_SEND, MIN_SEND, struct.pack('<Q', rand_id))
wait()

s1 = get_balance(ID1) - bal1_before
s2 = get_balance(ID2) - bal2_before
total_out = s1 + s2
check("Random distributed", total_out == 3 * MIN_SEND, f"total={total_out}, split={s1}/{s2}")
check("Random picked recipients", s1 > 0 or s2 > 0, f"{s1}/{s2}")

# ============================================================
# TEST 5: CONDITIONAL mode
# ============================================================
print()
print("─" * 60)
print("TEST 5: CONDITIONAL mode (whitelist)")
print("─" * 60)

# Only SEED1 is allowed to send
data = build_create(MODE_CONDITIONAL, [PK1, PK2], [50, 50], allowed_senders=[PK1])
send_tx(SEED0, PROC_CREATE, CREATION_FEE, data)
wait()

total, _ = query_count()
cond_id = total
print(f"  (total={total})")
gate = query_gate(cond_id)
check("Conditional gate created", gate['active'] == 1)

# Non-whitelisted sender (SEED2) — should be rejected, refunded
bal2_before = get_balance(ID2)
bal0_before = get_balance(ID0)

send_tx(SEED0, PROC_SEND, 5000, struct.pack('<Q', cond_id))
wait()

bal0_after = get_balance(ID0)
gate = query_gate(cond_id)
# If rejected, SEED0 loses nothing (or gets refund). Gate should have 0 forwarded.
check("Non-whitelisted rejected", gate['totalForwarded'] == 0,
      f"forwarded={gate['totalForwarded']}, gate balance={gate['currentBalance']}")

# Whitelisted sender (SEED1) — sends to recipient[0] which is PK1 (SEED1 itself)
# So SEED1 net = -10000 (send) + 10000 (receive from gate) = 0
# Use gate stats to verify it was processed
gate_before = query_gate(cond_id)

send_tx(SEED1, PROC_SEND, 10000, struct.pack('<Q', cond_id))
wait()

gate_after = query_gate(cond_id)
check("Whitelisted sender accepted", gate_after['totalForwarded'] == 10000,
      f"forwarded={gate_after['totalForwarded']}, received={gate_after['totalReceived']}")

# ============================================================
# TEST 6: updateGate
# ============================================================
print()
print("─" * 60)
print("TEST 6: updateGate (change SPLIT ratios)")
print("─" * 60)

# Update gate_id (the SPLIT gate from test 1) from 60/40 to 25/75
update_data = build_update(gate_id, [PK1, PK2], [25, 75])
send_tx(SEED0, PROC_UPDATE, 0, update_data)
wait()

gate = query_gate(gate_id)
check("Ratios updated", gate['ratios'] == [25, 75], f"ratios={gate['ratios']}")

# Verify new split
bal1_before = get_balance(ID1)
bal2_before = get_balance(ID2)
send_tx(SEED0, PROC_SEND, 10000, struct.pack('<Q', gate_id))
wait()

s1 = get_balance(ID1) - bal1_before
s2 = get_balance(ID2) - bal2_before
check("New 25/75 split works", s1 == 2500 and s2 == 7500, f"got {s1}/{s2}")

# Non-owner update should fail
gate_before = query_gate(gate_id)
bad_update = build_update(gate_id, [PK1, PK2], [99, 1])
send_tx(SEED2, PROC_UPDATE, 0, bad_update)
wait()

gate_after = query_gate(gate_id)
check("Non-owner update rejected", gate_after['ratios'] == gate_before['ratios'])

# ============================================================
# TEST 7: closeGate
# ============================================================
print()
print("─" * 60)
print("TEST 7: closeGate + non-owner rejection")
print("─" * 60)

# Non-owner close should fail
send_tx(SEED2, PROC_CLOSE, 0, struct.pack('<Q', gate_id))
wait()
gate = query_gate(gate_id)
check("Non-owner close rejected", gate['active'] == 1)

# Owner close
send_tx(SEED0, PROC_CLOSE, 0, struct.pack('<Q', gate_id))
wait()
gate = query_gate(gate_id)
check("Owner close works", gate['active'] == 0)

# Send to closed gate should fail
bal1_before = get_balance(ID1)
bal2_before = get_balance(ID2)
send_tx(SEED0, PROC_SEND, 5000, struct.pack('<Q', gate_id))
wait()
s1 = get_balance(ID1) - bal1_before
s2 = get_balance(ID2) - bal2_before
check("Send to closed gate rejected", s1 == 0 and s2 == 0, f"got {s1}/{s2}")

# ============================================================
# SUMMARY
# ============================================================
print()
print("=" * 60)
total = passed + failed
print(f"RESULTS: {passed}/{total} passed, {failed} failed")
print("=" * 60)

sys.exit(0 if failed == 0 else 1)
