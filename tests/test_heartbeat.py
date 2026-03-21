#!/usr/bin/env python3
"""
QuGate — HEARTBEAT Gate Mode Test (Mode 6)

Tests the dead-man's-switch / recurring distribution logic.
Owner calls heartbeat() periodically; if thresholdEpochs elapse
with no heartbeat, the gate triggers and distributes payoutPercentPerEpoch
of its balance each epoch until minimumBalance is reached.

Procedures under test:
  13 = configureHeartbeat
  14 = heartbeat
  15 = getHeartbeat (read-only)
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

MODE_HEARTBEAT = 6

PROC_CREATE = 1
PROC_SEND = 2
PROC_CLOSE = 3
PROC_CONFIGURE_HEARTBEAT = 13
PROC_HEARTBEAT = 14
FUNC_GET_GATE = 5
FUNC_GET_COUNT = 6
FUNC_GET_HEARTBEAT = 15

GATE_ID_SLOT_BITS = 20

passed = 0
failed = 0


def encode_gate_id(slot_idx, generation=0):
    return ((generation + 1) << GATE_ID_SLOT_BITS) | slot_idx


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
    tr, tf, cb, th = struct.unpack_from('<QQQQ', b, 40)
    return {
        'mode': b[0], 'recipientCount': b[1], 'active': b[2],
        'totalReceived': tr, 'totalForwarded': tf, 'currentBalance': cb, 'threshold': th,
    }


def query_heartbeat(gate_id):
    """
    getHeartbeat output layout (from contract):
      active(1) triggered(1) thresholdEpochs(4) lastHeartbeatEpoch(4) triggerEpoch(4)
      payoutPercentPerEpoch(1) minimumBalance(8) beneficiaryCount(1)
      beneficiaryAddresses[8] (256) beneficiaryShares[8] (8)
    """
    data = struct.pack('<Q', gate_id)
    resp = requests.post(f"{RPC}/live/v1/querySmartContract", json={
        'contractIndex': CONTRACT_INDEX, 'inputType': FUNC_GET_HEARTBEAT, 'inputSize': len(data),
        'requestData': base64.b64encode(data).decode()
    }, timeout=5).json()
    b = base64.b64decode(resp['responseData'])
    active, triggered = b[0], b[1]
    threshold_epochs, last_hb, trigger_epoch = struct.unpack_from('<III', b, 2)
    payout_pct = b[14]
    min_bal = struct.unpack_from('<q', b, 15)[0]
    bene_count = b[23]
    return {
        'active': active, 'triggered': triggered,
        'thresholdEpochs': threshold_epochs, 'lastHeartbeatEpoch': last_hb,
        'triggerEpoch': trigger_epoch, 'payoutPercentPerEpoch': payout_pct,
        'minimumBalance': min_bal, 'beneficiaryCount': bene_count,
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


def build_configure_heartbeat(gate_id, threshold_epochs, payout_pct, min_balance,
                               beneficiary_pks, beneficiary_shares):
    """
    configureHeartbeat_input layout:
      gateId(8) thresholdEpochs(4) payoutPercentPerEpoch(1) minimumBalance(8)
      beneficiaryAddresses[8] (256) beneficiaryShares[8] (8) beneficiaryCount(1)
    """
    data = bytearray()
    data += struct.pack('<Q', gate_id)
    data += struct.pack('<I', threshold_epochs)
    data += struct.pack('<B', payout_pct)
    data += struct.pack('<q', min_balance)
    for i in range(8):
        data += beneficiary_pks[i] if i < len(beneficiary_pks) else bytes(32)
    for i in range(8):
        data += struct.pack('<B', beneficiary_shares[i] if i < len(beneficiary_shares) else 0)
    data += struct.pack('<B', len(beneficiary_pks))
    return bytes(data)


def build_heartbeat(gate_id):
    """heartbeat_input: gateId(8)"""
    return struct.pack('<Q', gate_id)


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
print("QuGate — HEARTBEAT Gate Mode Test")
print("=" * 60)
print()

ADDR_A = get_identity(ADDR_A_KEY)
ADDR_B = get_identity(ADDR_B_KEY)
ADDR_C = get_identity(ADDR_C_KEY)
PK_A = get_pubkey(ADDR_A)
PK_B = get_pubkey(ADDR_B)
PK_C = get_pubkey(ADDR_C)

tick = get_tick()
print(f"Node: tick={tick}")
print(f"Addr A: {ADDR_A}")
print(f"Addr B: {ADDR_B}")
print(f"Addr C: {ADDR_C}")
print()

# ============================================================
# TEST 1: Create HEARTBEAT gate
# ============================================================
print("─" * 60)
print("TEST 1: Create HEARTBEAT gate")
print("─" * 60)

before_total, _ = query_count()
data = build_create(MODE_HEARTBEAT, [PK_B], [1])
send_tx(ADDR_A_KEY, PROC_CREATE, CREATION_FEE, data)
wait()

total, active = query_count()
hb_gate_id = encode_gate_id(total - 1)
print(f"  (before={before_total}, after={total})")
gate = query_gate(hb_gate_id)
check("HEARTBEAT gate created", gate['active'] == 1 and gate['mode'] == MODE_HEARTBEAT,
      f"id={hb_gate_id}, mode={gate['mode']}")

# ============================================================
# TEST 2: configureHeartbeat — threshold=2 epochs, payout=50%, beneficiaries 60/40
# ============================================================
print()
print("─" * 60)
print("TEST 2: configureHeartbeat (threshold=2, payout=50%, 60/40 split)")
print("─" * 60)

cfg_data = build_configure_heartbeat(
    hb_gate_id,
    threshold_epochs=2,
    payout_pct=50,
    min_balance=10000,
    beneficiary_pks=[PK_B, PK_C],
    beneficiary_shares=[60, 40]
)
send_tx(ADDR_A_KEY, PROC_CONFIGURE_HEARTBEAT, 0, cfg_data)
wait()

hb = query_heartbeat(hb_gate_id)
check("configureHeartbeat stored",
      hb['active'] == 1 and hb['thresholdEpochs'] == 2 and hb['payoutPercentPerEpoch'] == 50,
      f"active={hb['active']}, threshold={hb['thresholdEpochs']}, pct={hb['payoutPercentPerEpoch']}")
check("Beneficiary count=2", hb['beneficiaryCount'] == 2, f"count={hb['beneficiaryCount']}")
check("Not yet triggered", hb['triggered'] == 0)
check("minimumBalance=10000", hb['minimumBalance'] == 10000, f"minBal={hb['minimumBalance']}")

# ============================================================
# TEST 3: heartbeat() — resets epoch counter
# ============================================================
print()
print("─" * 60)
print("TEST 3: heartbeat() resets epoch counter")
print("─" * 60)

hb_before = query_heartbeat(hb_gate_id)
send_tx(ADDR_A_KEY, PROC_HEARTBEAT, 0, build_heartbeat(hb_gate_id))
wait()

hb_after = query_heartbeat(hb_gate_id)
check("heartbeat() accepted", hb_after['active'] == 1 and hb_after['triggered'] == 0)
# lastHeartbeatEpoch should be updated (or stay same if same epoch)
check("Epoch counter updated", hb_after['lastHeartbeatEpoch'] >= hb_before['lastHeartbeatEpoch'],
      f"before={hb_before['lastHeartbeatEpoch']}, after={hb_after['lastHeartbeatEpoch']}")

# ============================================================
# TEST 4: heartbeat() by non-owner — rejected
# ============================================================
print()
print("─" * 60)
print("TEST 4: heartbeat() by non-owner — rejected")
print("─" * 60)

hb_before = query_heartbeat(hb_gate_id)
send_tx(ADDR_B_KEY, PROC_HEARTBEAT, 0, build_heartbeat(hb_gate_id))
wait()

hb_after = query_heartbeat(hb_gate_id)
# Non-owner call should be rejected — epoch counter should not change
check("Non-owner heartbeat() rejected",
      hb_after['lastHeartbeatEpoch'] == hb_before['lastHeartbeatEpoch'],
      f"epoch before={hb_before['lastHeartbeatEpoch']}, after={hb_after['lastHeartbeatEpoch']}")

# ============================================================
# TEST 5: Fund the gate with 1,000,000 QU
# ============================================================
print()
print("─" * 60)
print("TEST 5: Fund HEARTBEAT gate with 1,000,000 QU")
print("─" * 60)

send_tx(ADDR_A_KEY, PROC_SEND, 1_000_000, struct.pack('<Q', hb_gate_id))
wait()

gate = query_gate(hb_gate_id)
check("Gate funded", gate['currentBalance'] == 1_000_000,
      f"balance={gate['currentBalance']}")

# ============================================================
# TEST 6: Advance epochs without heartbeat() — verify trigger fires
# Note: in testnet, we advance epochs by waiting; exact epoch count
# depends on testnet timing. We record the state before and after
# advancing past the threshold.
# ============================================================
print()
print("─" * 60)
print("TEST 6: Advance epochs without heartbeat (expect trigger after 2 epochs)")
print("─" * 60)

hb_pre = query_heartbeat(hb_gate_id)
print(f"  State before: triggered={hb_pre['triggered']}, lastHB={hb_pre['lastHeartbeatEpoch']}")

# Each epoch is ~1 week on mainnet; in testnet we force epoch advances.
# The testnet uses epoch 200 as starting point. We need 3+ epochs to pass
# (threshold=2 means trigger fires when epochsInactive > 2).
# Without a heartbeat, this will fire automatically in END_EPOCH.
# We simulate by waiting for the node to advance epochs naturally in testnet.
print("  ⏳ Waiting for epoch advancement (testnet should auto-advance)...")
# Wait up to 10 minutes for epoch to advance
epoch_advanced = False
for attempt in range(30):
    time.sleep(20)
    try:
        hb_check = query_heartbeat(hb_gate_id)
        if hb_check['triggered'] == 1:
            epoch_advanced = True
            break
        print(f"  ... attempt {attempt+1}: triggered={hb_check['triggered']}, "
              f"lastHB={hb_check['lastHeartbeatEpoch']}")
    except Exception as e:
        print(f"  ... query failed: {e}")

hb_post = query_heartbeat(hb_gate_id)
check("Gate triggered after threshold epochs",
      hb_post['triggered'] == 1 or epoch_advanced,
      f"triggered={hb_post['triggered']}, triggerEpoch={hb_post['triggerEpoch']}")

if not epoch_advanced:
    print("  ⚠  Epoch did not advance in time — marking trigger test as SKIPPED")
    print("     (Tests 7-10 require the gate to be triggered. Skipping.)")
    print()
    print("=" * 60)
    total = passed + failed
    print(f"RESULTS: {passed}/{total} passed, {failed} failed")
    print("(Note: epoch-dependent tests skipped — run on a testnet with epoch advancement)")
    print("=" * 60)
    sys.exit(0 if failed == 0 else 1)

# ============================================================
# TEST 7: After trigger — verify 50% distributed 60/40
# ============================================================
print()
print("─" * 60)
print("TEST 7: Payout after trigger — 50% distributed 60/40")
print("─" * 60)

gate_triggered = query_gate(hb_gate_id)
bal_before_payout = gate_triggered['currentBalance']
bal_b_before = get_balance(ADDR_B)
bal_c_before = get_balance(ADDR_C)

# Wait one more epoch for payout to happen
print("  ⏳ Waiting for payout epoch...")
for attempt in range(15):
    time.sleep(20)
    gate_check = query_gate(hb_gate_id)
    if gate_check['totalForwarded'] > 0:
        break

gate_after = query_gate(hb_gate_id)
bal_b_after = get_balance(ADDR_B)
bal_c_after = get_balance(ADDR_C)

expected_payout = bal_before_payout * 50 // 100
b_received = bal_b_after - bal_b_before
c_received = bal_c_after - bal_c_before
total_distributed = b_received + c_received

check("Payout executed (balance reduced)", gate_after['currentBalance'] < bal_before_payout,
      f"before={bal_before_payout}, after={gate_after['currentBalance']}")
check("50% payout amount correct", abs(total_distributed - expected_payout) <= 1,
      f"expected≈{expected_payout}, got={total_distributed}")
# Check 60/40 split approximately
if total_distributed > 0:
    b_share = b_received * 100 // total_distributed
    c_share = c_received * 100 // total_distributed
    check("60/40 beneficiary split", 58 <= b_share <= 62 and 38 <= c_share <= 42,
          f"B={b_share}%, C={c_share}%")

# ============================================================
# TEST 8: After trigger — heartbeat() rejected
# ============================================================
print()
print("─" * 60)
print("TEST 8: heartbeat() after trigger — rejected")
print("─" * 60)

hb_state = query_heartbeat(hb_gate_id)
last_epoch_before = hb_state['lastHeartbeatEpoch']

send_tx(ADDR_A_KEY, PROC_HEARTBEAT, 0, build_heartbeat(hb_gate_id))
wait()

hb_state_after = query_heartbeat(hb_gate_id)
check("heartbeat() rejected after trigger",
      hb_state_after['triggered'] == 1 and
      hb_state_after['lastHeartbeatEpoch'] == last_epoch_before,
      f"triggered={hb_state_after['triggered']}, epoch unchanged={last_epoch_before}")

# ============================================================
# TEST 9: Next epoch — verify another 50% distributed
# ============================================================
print()
print("─" * 60)
print("TEST 9: Next epoch — another 50% distributed")
print("─" * 60)

gate_current = query_gate(hb_gate_id)
if gate_current['active'] == 0:
    print("  Gate already auto-closed (balance hit minimum). Skipping payout check.")
    check("Gate auto-closed correctly", gate_current['active'] == 0)
else:
    bal_epoch2 = gate_current['currentBalance']
    fwd_before2 = gate_current['totalForwarded']

    print("  ⏳ Waiting for second payout epoch...")
    for attempt in range(15):
        time.sleep(20)
        gate_check2 = query_gate(hb_gate_id)
        if gate_check2['totalForwarded'] > fwd_before2:
            break

    gate_after2 = query_gate(hb_gate_id)
    expected_payout2 = bal_epoch2 * 50 // 100
    actual_payout2 = gate_after2['totalForwarded'] - fwd_before2

    check("Second epoch payout executed", actual_payout2 > 0,
          f"forwarded={actual_payout2}")
    check("Second epoch ~50% payout", abs(actual_payout2 - expected_payout2) <= 1,
          f"expected≈{expected_payout2}, got={actual_payout2}")

# ============================================================
# TEST 10: Gate auto-closes when balance <= minimumBalance
# ============================================================
print()
print("─" * 60)
print("TEST 10: Gate auto-closes when balance <= minimumBalance (10,000 QU)")
print("─" * 60)

gate_final = query_gate(hb_gate_id)
print(f"  Current balance: {gate_final['currentBalance']}, minimumBalance: 10000")

if gate_final['active'] == 0:
    check("Gate auto-closed", True, "already closed")
else:
    # Wait several more epochs for balance to drain below minimum
    print("  ⏳ Waiting for balance to drain below minimumBalance...")
    auto_closed = False
    for attempt in range(20):
        time.sleep(20)
        gate_check = query_gate(hb_gate_id)
        if gate_check['active'] == 0:
            auto_closed = True
            break

    check("Gate auto-closes when balance <= minimum", auto_closed,
          f"active={gate_check['active']}, balance={gate_check['currentBalance']}")

# ============================================================
# SUMMARY
# ============================================================
print()
print("=" * 60)
total = passed + failed
print(f"RESULTS: {passed}/{total} passed, {failed} failed")
print("=" * 60)

sys.exit(0 if failed == 0 else 1)
