#!/usr/bin/env python3
"""
QuGate — All 7 Gate Modes Test

Tests: SPLIT, ROUND_ROBIN, THRESHOLD, RANDOM, CONDITIONAL, HEARTBEAT, MULTISIG
Plus: updateGate, closeGate, non-owner rejection
"""
import os
import shutil
import struct
import subprocess
import base64
import time
import requests
import sys

CLI = os.environ.get("QUBIC_CLI", shutil.which("qubic-cli") or "qubic-cli")
NODE_ARGS = ["-nodeip", "127.0.0.1", "-nodeport", "31841"]
RPC = "http://127.0.0.1:41841"
CONTRACT_INDEX = 25  # Pulse took index 24, QuGate uses 25
CONTRACT_ID = "YAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAMSME"

ADDR_A_KEY = "eraaastggldisjhoojaekgyimrsddjxbvgaawswfvnvaygqmusnkevv"
ADDR_B_KEY = "sgwnpzidgxbclnisgehigeculaejjxedzdkjyyfrzgzvuojrhdzywfh"
ADDR_C_KEY = "xeejtwxqrrlvacapbujaleejhbrsnnpvviknskemmgdihggpssjjkrg"

CREATION_FEE = 100000
MIN_SEND = 1000

MODE_SPLIT = 0
MODE_ROUND_ROBIN = 1
MODE_THRESHOLD = 2
MODE_RANDOM = 3
MODE_CONDITIONAL = 4
MODE_ORACLE = 5
MODE_HEARTBEAT = 6
MODE_MULTISIG = 7

PROC_CREATE = 1
PROC_SEND = 2
PROC_CLOSE = 3
PROC_UPDATE = 4
FUNC_GET_GATE = 5
FUNC_GET_COUNT = 6
FUNC_GET_FEES = 9
PROC_CONFIGURE_HEARTBEAT = 13
PROC_HEARTBEAT = 14
FUNC_GET_HEARTBEAT = 15
PROC_CONFIGURE_MULTISIG = 16
FUNC_GET_MULTISIG_STATE = 17

# Versioned gate ID encoding
GATE_ID_SLOT_BITS = 20

def encode_gate_id(slot_idx, generation=0):
    return ((generation + 1) << GATE_ID_SLOT_BITS) | slot_idx

passed = 0
failed = 0

def cli(*args, timeout=15):
    r = subprocess.run([CLI] + NODE_ARGS + list(args), capture_output=True, text=True, timeout=timeout)
    return r.stdout + r.stderr

def get_identity(key):
    out = cli("-seed", key, "-showkeys")
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
        except Exception:
            if attempt < 4:
                time.sleep(3)
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

def send_tx(key, proc, amount, data):
    return cli("-seed", key, "-sendcustomtransaction",
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
        except Exception:
            time.sleep(5)
    print("    ⚠ timeout!")
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

ADDR_A = get_identity(ADDR_A_KEY)
ADDR_B = get_identity(ADDR_B_KEY)
ADDR_C = get_identity(ADDR_C_KEY)
PK_A = get_pubkey(ADDR_A)
PK_B = get_pubkey(ADDR_B)
PK_C = get_pubkey(ADDR_C)

tick = get_tick()
print(f"Node: tick={tick}, epoch=200")
print(f"Addr A: {ADDR_A}")
print(f"Addr B: {ADDR_B}")
print(f"Addr C: {ADDR_C}")
print()

# ============================================================
# TEST 1: SPLIT mode (60/40)
# ============================================================
print("─" * 60)
print("TEST 1: SPLIT mode (60/40)")
print("─" * 60)

before_total, _ = query_count()
data = build_create(MODE_SPLIT, [PK_B, PK_C], [60, 40])
send_tx(ADDR_A_KEY, PROC_CREATE, CREATION_FEE, data)
wait()

total, active = query_count()
gate_id = encode_gate_id(total - 1)
print(f"  (before={before_total}, after={total})")
gate = query_gate(gate_id)
check("Gate created", gate['active'] == 1, f"id={gate_id}, mode={gate['mode_name']}")
check("Ratios correct", gate['ratios'] == [60, 40], f"ratios={gate['ratios']}")

bal1_before = get_balance(ADDR_B)
bal2_before = get_balance(ADDR_C)

send_tx(ADDR_A_KEY, PROC_SEND, 10000, struct.pack('<Q', gate_id))
wait()

s1 = get_balance(ADDR_B) - bal1_before
s2 = get_balance(ADDR_C) - bal2_before
check("Split 60/40", s1 == 6000 and s2 == 4000, f"got {s1}/{s2}")

# ============================================================
# TEST 2: ROUND_ROBIN mode
# ============================================================
print()
print("─" * 60)
print("TEST 2: ROUND_ROBIN mode")
print("─" * 60)

data = build_create(MODE_ROUND_ROBIN, [PK_B, PK_C], [1, 1])
send_tx(ADDR_A_KEY, PROC_CREATE, CREATION_FEE, data)
wait()

total, _ = query_count()
rr_id = encode_gate_id(total - 1)
print(f"  (total={total})")
gate = query_gate(rr_id)
check("RR gate created", gate['active'] == 1, f"id={rr_id}")

# Send twice — should alternate
bal1_before = get_balance(ADDR_B)
bal2_before = get_balance(ADDR_C)

send_tx(ADDR_A_KEY, PROC_SEND, 5000, struct.pack('<Q', rr_id))
wait()

s1a = get_balance(ADDR_B) - bal1_before
s2a = get_balance(ADDR_C) - bal2_before

bal1_before = get_balance(ADDR_B)
bal2_before = get_balance(ADDR_C)

send_tx(ADDR_A_KEY, PROC_SEND, 5000, struct.pack('<Q', rr_id))
wait()

s1b = get_balance(ADDR_B) - bal1_before
s2b = get_balance(ADDR_C) - bal2_before

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

data = build_create(MODE_THRESHOLD, [PK_B, PK_C], [50, 50], threshold=15000)
send_tx(ADDR_A_KEY, PROC_CREATE, CREATION_FEE, data)
wait()

total, _ = query_count()
th_id = encode_gate_id(total - 1)
print(f"  (total={total})")
gate = query_gate(th_id)
check("Threshold gate created", gate['active'] == 1 and gate['threshold'] == 15000)

# Send 10k — below threshold, should accumulate
bal1_before = get_balance(ADDR_B)
bal2_before = get_balance(ADDR_C)

send_tx(ADDR_A_KEY, PROC_SEND, 10000, struct.pack('<Q', th_id))
wait()

gate = query_gate(th_id)
check("Below threshold: held", gate['currentBalance'] == 10000, f"balance={gate['currentBalance']}")

s1 = get_balance(ADDR_B) - bal1_before
s2 = get_balance(ADDR_C) - bal2_before
check("No distribution yet", s1 == 0 and s2 == 0, f"got {s1}/{s2}")

# Send 10k more — 20k total, above threshold, should flush all to recipient[0]
bal1_before = get_balance(ADDR_B)

send_tx(ADDR_A_KEY, PROC_SEND, 10000, struct.pack('<Q', th_id))
wait()

gate = query_gate(th_id)
s1 = get_balance(ADDR_B) - bal1_before
check("Above threshold: flushed to recipient[0]", s1 == 20000 and gate['currentBalance'] == 0,
      f"recipient[0] got {s1}, gate balance={gate['currentBalance']}")

# ============================================================
# TEST 4: RANDOM mode
# ============================================================
print()
print("─" * 60)
print("TEST 4: RANDOM mode")
print("─" * 60)

data = build_create(MODE_RANDOM, [PK_B, PK_C], [50, 50])
send_tx(ADDR_A_KEY, PROC_CREATE, CREATION_FEE, data)
wait()

total, _ = query_count()
rand_id = encode_gate_id(total - 1)
print(f"  (total={total})")
gate = query_gate(rand_id)
check("Random gate created", gate['active'] == 1)

# Send 3 times with waits between to ensure separate ticks
bal1_before = get_balance(ADDR_B)
bal2_before = get_balance(ADDR_C)

send_tx(ADDR_A_KEY, PROC_SEND, MIN_SEND, struct.pack('<Q', rand_id))
wait()
send_tx(ADDR_A_KEY, PROC_SEND, MIN_SEND, struct.pack('<Q', rand_id))
wait()
send_tx(ADDR_A_KEY, PROC_SEND, MIN_SEND, struct.pack('<Q', rand_id))
wait()

s1 = get_balance(ADDR_B) - bal1_before
s2 = get_balance(ADDR_C) - bal2_before
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

# Only ADDR_B_KEY is allowed to send
data = build_create(MODE_CONDITIONAL, [PK_B, PK_C], [50, 50], allowed_senders=[PK_B])
send_tx(ADDR_A_KEY, PROC_CREATE, CREATION_FEE, data)
wait()

total, _ = query_count()
cond_id = encode_gate_id(total - 1)
print(f"  (total={total})")
gate = query_gate(cond_id)
check("Conditional gate created", gate['active'] == 1)

# Non-whitelisted sender (ADDR_C_KEY) — should be rejected, refunded
bal2_before = get_balance(ADDR_C)
bal0_before = get_balance(ADDR_A)

send_tx(ADDR_A_KEY, PROC_SEND, 5000, struct.pack('<Q', cond_id))
wait()

bal0_after = get_balance(ADDR_A)
gate = query_gate(cond_id)
# If rejected, ADDR_A_KEY loses nothing (or gets refund). Gate should have 0 forwarded.
check("Non-whitelisted rejected", gate['totalForwarded'] == 0,
      f"forwarded={gate['totalForwarded']}, gate balance={gate['currentBalance']}")

# Whitelisted sender (ADDR_B_KEY) — sends to recipient[0] which is PK_B (ADDR_B_KEY itself)
# So ADDR_B_KEY net = -10000 (send) + 10000 (receive from gate) = 0
# Use gate stats to verify it was processed
gate_before = query_gate(cond_id)

send_tx(ADDR_B_KEY, PROC_SEND, 10000, struct.pack('<Q', cond_id))
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
update_data = build_update(gate_id, [PK_B, PK_C], [25, 75])
send_tx(ADDR_A_KEY, PROC_UPDATE, 0, update_data)
wait()

gate = query_gate(gate_id)
check("Ratios updated", gate['ratios'] == [25, 75], f"ratios={gate['ratios']}")

# Verify new split
bal1_before = get_balance(ADDR_B)
bal2_before = get_balance(ADDR_C)
send_tx(ADDR_A_KEY, PROC_SEND, 10000, struct.pack('<Q', gate_id))
wait()

s1 = get_balance(ADDR_B) - bal1_before
s2 = get_balance(ADDR_C) - bal2_before
check("New 25/75 split works", s1 == 2500 and s2 == 7500, f"got {s1}/{s2}")

# Non-owner update should fail
gate_before = query_gate(gate_id)
bad_update = build_update(gate_id, [PK_B, PK_C], [99, 1])
send_tx(ADDR_C_KEY, PROC_UPDATE, 0, bad_update)
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
send_tx(ADDR_C_KEY, PROC_CLOSE, 0, struct.pack('<Q', gate_id))
wait()
gate = query_gate(gate_id)
check("Non-owner close rejected", gate['active'] == 1)

# Owner close
send_tx(ADDR_A_KEY, PROC_CLOSE, 0, struct.pack('<Q', gate_id))
wait()
gate = query_gate(gate_id)
check("Owner close works", gate['active'] == 0)

# Send to closed gate should fail
bal1_before = get_balance(ADDR_B)
bal2_before = get_balance(ADDR_C)
send_tx(ADDR_A_KEY, PROC_SEND, 5000, struct.pack('<Q', gate_id))
wait()
s1 = get_balance(ADDR_B) - bal1_before
s2 = get_balance(ADDR_C) - bal2_before
check("Send to closed gate rejected", s1 == 0 and s2 == 0, f"got {s1}/{s2}")

# ============================================================
# ============================================================
# TEST 8: HEARTBEAT gate — create, configure, pulse, fund
# (Epoch-triggered payouts require epoch advancement; we test
#  the creation, configuration, and heartbeat() procedure only.)
# ============================================================
print()
print("─" * 60)
print("TEST 8: HEARTBEAT gate (mode=6) — create, configure, heartbeat()")
print("─" * 60)

before_total8, _ = query_count()
hb_data = build_create(MODE_HEARTBEAT, [PK_B], [1])
send_tx(ADDR_A_KEY, PROC_CREATE, CREATION_FEE, hb_data)
wait()

total8, _ = query_count()
hb_id = encode_gate_id(total8 - 1)
hb_gate = query_gate(hb_id)
check("HEARTBEAT gate created", hb_gate['active'] == 1 and hb_gate['mode'] == MODE_HEARTBEAT,
      f"id={hb_id}, mode={hb_gate['mode']}")

# configureHeartbeat: threshold=3 epochs, payout=25%, min_balance=5000, beneficiaries B(60)/C(40)
cfg_hb = bytearray()
cfg_hb += struct.pack('<Q', hb_id)       # gateId
cfg_hb += struct.pack('<I', 3)            # thresholdEpochs
cfg_hb += struct.pack('<B', 25)           # payoutPercentPerEpoch
cfg_hb += struct.pack('<q', 5000)         # minimumBalance
for i in range(8):                        # beneficiaryAddresses[8]
    cfg_hb += [PK_B, PK_C][i] if i < 2 else bytes(32)
for i in range(8):                        # beneficiaryShares[8]
    cfg_hb += struct.pack('<B', [60, 40, 0, 0, 0, 0, 0, 0][i])
cfg_hb += struct.pack('<B', 2)            # beneficiaryCount
send_tx(ADDR_A_KEY, PROC_CONFIGURE_HEARTBEAT, 0, bytes(cfg_hb))
wait()

# Query heartbeat state via getHeartbeat
hb_resp = requests.post(f"{RPC}/live/v1/querySmartContract", json={
    'contractIndex': CONTRACT_INDEX, 'inputType': FUNC_GET_HEARTBEAT, 'inputSize': 8,
    'requestData': base64.b64encode(struct.pack('<Q', hb_id)).decode()
}, timeout=5).json()
hb_b = base64.b64decode(hb_resp['responseData'])
hb_active = hb_b[0]
hb_triggered = hb_b[1]
hb_threshold = struct.unpack_from('<I', hb_b, 2)[0]
hb_pct = hb_b[14]
hb_bene_count = hb_b[23]
check("configureHeartbeat stored",
      hb_active == 1 and hb_threshold == 3 and hb_pct == 25,
      f"active={hb_active}, threshold={hb_threshold}, pct={hb_pct}")
check("Beneficiary count=2", hb_bene_count == 2, f"count={hb_bene_count}")
check("Not triggered initially", hb_triggered == 0)

# heartbeat() by owner — resets epoch counter
send_tx(ADDR_A_KEY, PROC_HEARTBEAT, 0, struct.pack('<Q', hb_id))
wait()

hb_resp2 = requests.post(f"{RPC}/live/v1/querySmartContract", json={
    'contractIndex': CONTRACT_INDEX, 'inputType': FUNC_GET_HEARTBEAT, 'inputSize': 8,
    'requestData': base64.b64encode(struct.pack('<Q', hb_id)).decode()
}, timeout=5).json()
hb_b2 = base64.b64decode(hb_resp2['responseData'])
check("heartbeat() accepted", hb_b2[1] == 0, f"still not triggered")

# heartbeat() by non-owner — rejected (epoch should not change)
hb_last_before = struct.unpack_from('<I', hb_b2, 6)[0]
send_tx(ADDR_B_KEY, PROC_HEARTBEAT, 0, struct.pack('<Q', hb_id))
wait()

hb_resp3 = requests.post(f"{RPC}/live/v1/querySmartContract", json={
    'contractIndex': CONTRACT_INDEX, 'inputType': FUNC_GET_HEARTBEAT, 'inputSize': 8,
    'requestData': base64.b64encode(struct.pack('<Q', hb_id)).decode()
}, timeout=5).json()
hb_b3 = base64.b64decode(hb_resp3['responseData'])
hb_last_after = struct.unpack_from('<I', hb_b3, 6)[0]
check("Non-owner heartbeat() rejected", hb_last_after == hb_last_before,
      f"epoch unchanged={hb_last_before}")

# Fund HEARTBEAT gate
send_tx(ADDR_A_KEY, PROC_SEND, 500_000, struct.pack('<Q', hb_id))
wait()
hb_gate_funded = query_gate(hb_id)
check("HEARTBEAT gate funded", hb_gate_funded['currentBalance'] == 500_000,
      f"balance={hb_gate_funded['currentBalance']}")

# ============================================================
# TEST 9: MULTISIG gate — create, configure, vote, execute
# ============================================================
print()
print("─" * 60)
print("TEST 9: MULTISIG gate (mode=7) — create, configure, vote, release")
print("─" * 60)

before_total9, _ = query_count()
ms_data = build_create(MODE_MULTISIG, [PK_B], [1])
send_tx(ADDR_A_KEY, PROC_CREATE, CREATION_FEE, ms_data)
wait()

total9, _ = query_count()
ms_id = encode_gate_id(total9 - 1)
ms_gate = query_gate(ms_id)
check("MULTISIG gate created", ms_gate['active'] == 1 and ms_gate['mode'] == MODE_MULTISIG,
      f"id={ms_id}, mode={ms_gate['mode']}")

# configureMultisig: 2 guardians (B, C), required=2, expiry=4 epochs
cfg_ms = bytearray()
cfg_ms += struct.pack('<I', ms_id & 0xFFFFFFFF)  # gateId (uint32)
for i in range(8):                                 # guardians[8]
    cfg_ms += [PK_B, PK_C][i] if i < 2 else bytes(32)
cfg_ms += struct.pack('<B', 2)                     # guardianCount
cfg_ms += struct.pack('<B', 2)                     # required
cfg_ms += struct.pack('<I', 4)                     # proposalExpiryEpochs
send_tx(ADDR_A_KEY, PROC_CONFIGURE_MULTISIG, 0, bytes(cfg_ms))
wait()

ms_state_resp = requests.post(f"{RPC}/live/v1/querySmartContract", json={
    'contractIndex': CONTRACT_INDEX, 'inputType': FUNC_GET_MULTISIG_STATE, 'inputSize': 4,
    'requestData': base64.b64encode(struct.pack('<I', ms_id & 0xFFFFFFFF)).decode()
}, timeout=5).json()
ms_b = base64.b64decode(ms_state_resp['responseData'])
ms_status = struct.unpack_from('<q', ms_b, 0)[0]
ms_guardian_count = ms_b[11]
ms_required = ms_b[10]
check("configureMultisig stored",
      ms_status == 0 and ms_guardian_count == 2 and ms_required == 2,
      f"status={ms_status}, guardians={ms_guardian_count}, required={ms_required}")
check("No active proposal initially", ms_b[16] == 0)

# Fund gate (non-guardian) — accumulates, no vote
send_tx(ADDR_A_KEY, PROC_SEND, 300_000, struct.pack('<Q', ms_id))
wait()

ms_funded = query_gate(ms_id)
check("MULTISIG gate funded", ms_funded['currentBalance'] >= 300_000,
      f"balance={ms_funded['currentBalance']}")

# Guardian B votes
send_tx(ADDR_B_KEY, PROC_SEND, MIN_SEND, struct.pack('<Q', ms_id))
wait()

ms_v1_resp = requests.post(f"{RPC}/live/v1/querySmartContract", json={
    'contractIndex': CONTRACT_INDEX, 'inputType': FUNC_GET_MULTISIG_STATE, 'inputSize': 4,
    'requestData': base64.b64encode(struct.pack('<I', ms_id & 0xFFFFFFFF)).decode()
}, timeout=5).json()
ms_b1 = base64.b64decode(ms_v1_resp['responseData'])
check("Guardian B vote registered (count=1)", ms_b1[9] == 1, f"count={ms_b1[9]}")
check("Proposal active after vote 1", ms_b1[16] == 1, f"active={ms_b1[16]}")
check("Funds NOT released yet (1/2)", query_gate(ms_id)['currentBalance'] > 0)

# Guardian C votes — threshold met
bal_b_ms_before = get_balance(ADDR_B)
ms_bal_before_exec = query_gate(ms_id)['currentBalance']
send_tx(ADDR_C_KEY, PROC_SEND, MIN_SEND, struct.pack('<Q', ms_id))
wait()

ms_v2_gate = query_gate(ms_id)
bal_b_ms_after = get_balance(ADDR_B)
ms_released = bal_b_ms_after - bal_b_ms_before
check("Funds released to recipient[0] (B) after 2/2 votes",
      ms_released >= ms_bal_before_exec - 10,
      f"released={ms_released}, was={ms_bal_before_exec}")
check("Gate balance cleared", ms_v2_gate['currentBalance'] < ms_bal_before_exec,
      f"balance={ms_v2_gate['currentBalance']}")

# Votes reset after execution
ms_v2_resp = requests.post(f"{RPC}/live/v1/querySmartContract", json={
    'contractIndex': CONTRACT_INDEX, 'inputType': FUNC_GET_MULTISIG_STATE, 'inputSize': 4,
    'requestData': base64.b64encode(struct.pack('<I', ms_id & 0xFFFFFFFF)).decode()
}, timeout=5).json()
ms_b2 = base64.b64decode(ms_v2_resp['responseData'])
check("Votes reset after execution", ms_b2[9] == 0 and ms_b2[16] == 0,
      f"count={ms_b2[9]}, active={ms_b2[16]}")

# Non-owner configureMultisig rejected
bad_ms_cfg = bytearray()
bad_ms_cfg += struct.pack('<I', ms_id & 0xFFFFFFFF)
for i in range(8):
    bad_ms_cfg += PK_A if i < 1 else bytes(32)
bad_ms_cfg += struct.pack('<B', 1)
bad_ms_cfg += struct.pack('<B', 1)
bad_ms_cfg += struct.pack('<I', 1)
send_tx(ADDR_B_KEY, PROC_CONFIGURE_MULTISIG, 0, bytes(bad_ms_cfg))
wait()

ms_after_bad = requests.post(f"{RPC}/live/v1/querySmartContract", json={
    'contractIndex': CONTRACT_INDEX, 'inputType': FUNC_GET_MULTISIG_STATE, 'inputSize': 4,
    'requestData': base64.b64encode(struct.pack('<I', ms_id & 0xFFFFFFFF)).decode()
}, timeout=5).json()
ms_bad_b = base64.b64decode(ms_after_bad['responseData'])
check("Non-owner configureMultisig rejected",
      ms_bad_b[11] == 2 and ms_bad_b[10] == 2,  # guardianCount and required unchanged
      f"guardians={ms_bad_b[11]}, required={ms_bad_b[10]}")

# ============================================================
# SUMMARY
# ============================================================
print()
print("=" * 60)
total = passed + failed
print(f"RESULTS: {passed}/{total} passed, {failed} failed")
print("=" * 60)

sys.exit(0 if failed == 0 else 1)
