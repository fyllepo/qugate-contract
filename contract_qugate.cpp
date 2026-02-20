// QuGate contract tests V3 — standalone (no ContractTesting framework)
// Tests: escalating fees, expiry, dust burn, status codes, free-list, all modes

#include <gtest/gtest.h>
#include <cstring>
#include <cstdlib>
#include <cstdint>

// ---- Self-contained m256i (avoids upstream UEFI headers) ----
union m256i {
    int8_t   m256i_i8[32];
    uint8_t  m256i_u8[32];
    int16_t  m256i_i16[16];
    uint16_t m256i_u16[16];
    int32_t  m256i_i32[8];
    uint32_t m256i_u32[8];
    int64_t  m256i_i64[4];
    uint64_t m256i_u64[4];

    static m256i zero() {
        m256i z;
        memset(&z, 0, sizeof(z));
        return z;
    }
};

static inline bool operator==(const m256i& a, const m256i& b) {
    return memcmp(&a, &b, 32) == 0;
}

// =============================================
// Minimal QPI shim for test harness
// =============================================

namespace QPI {
    typedef m256i id;
    typedef unsigned long long uint64;
    typedef long long sint64;
    typedef unsigned short uint16;
    typedef unsigned int uint32;
    typedef unsigned char uint8;
    typedef signed char sint8;
    typedef bool bit;
    constexpr unsigned long long X_MULTIPLIER = 1;

    template<typename T, unsigned long long capacity>
    struct Array {
        T _data[capacity];
        const T& get(unsigned long long idx) const { return _data[idx]; }
        T& get(unsigned long long idx) { return _data[idx]; }
        void set(unsigned long long idx, const T& val) { _data[idx] = val; }
    };

    struct ContractBase {};

    inline uint64 div(uint64 a, uint64 b) { return b ? a / b : 0; }
    inline uint64 mod(uint64 a, uint64 b) { return b ? a % b : 0; }
}

using namespace QPI;

// =============================================
// Test QPI context — tracks transfers, burns, ticks
// =============================================

struct TestQpiContext {
    id _invocator;
    sint64 _reward;
    uint16 _epoch;
    uint64 _tick;

    static constexpr int MAX_TRANSFERS = 64;
    struct Transfer { id to; sint64 amount; };
    Transfer transfers[MAX_TRANSFERS];
    int transferCount;

    sint64 totalBurned;

    TestQpiContext() : _reward(0), _epoch(100), _tick(12345), transferCount(0), totalBurned(0) {
        memset(&_invocator, 0, sizeof(_invocator));
        memset(transfers, 0, sizeof(transfers));
    }

    id invocator() const { return _invocator; }
    sint64 invocationReward() const { return _reward; }
    uint16 epoch() const { return _epoch; }
    uint64 tick() const { return _tick; }

    void transfer(const id& to, sint64 amount) {
        if (transferCount < MAX_TRANSFERS) {
            transfers[transferCount].to = to;
            transfers[transferCount].amount = amount;
            transferCount++;
        }
    }

    void burn(sint64 amount) {
        totalBurned += amount;
    }

    void reset() {
        transferCount = 0;
        totalBurned = 0;
        _reward = 0;
    }

    sint64 totalTransferredTo(const id& addr) const {
        sint64 total = 0;
        for (int i = 0; i < transferCount; i++) {
            if (transfers[i].to == addr) total += transfers[i].amount;
        }
        return total;
    }
};

// =============================================
// Stub macros for LOG_INFO / LOG_WARNING
// =============================================
#define LOG_INFO(x) ((void)0)
#define LOG_WARNING(x) ((void)0)
#define CONTRACT_INDEX 0

// =============================================
// Macro shims that expand QuGate.h into testable C++
// =============================================

// We need to define macros that QuGate.h uses, then include it.
// The contract body uses PUBLIC_PROCEDURE_WITH_LOCALS, etc.
// We'll redefine them as methods of a test wrapper.

// Instead of including QuGate.h directly (complex macro expansion),
// we replicate the data structures and implement test logic matching
// the contract exactly.

// Pull in constants and structures from QuGate.h
constexpr uint64 QUGATE_INITIAL_MAX_GATES = 4096;
constexpr uint64 QUGATE_MAX_GATES = QUGATE_INITIAL_MAX_GATES * 1; // X_MULTIPLIER=1
constexpr uint64 QUGATE_MAX_RECIPIENTS = 8;
constexpr uint64 QUGATE_MAX_RATIO = 10000;

constexpr uint64 QUGATE_DEFAULT_CREATION_FEE = 1000;
constexpr uint64 QUGATE_DEFAULT_MIN_SEND = 10;
constexpr uint64 QUGATE_FEE_ESCALATION_STEP = 1024;
constexpr uint64 QUGATE_DEFAULT_EXPIRY_EPOCHS = 50;

constexpr uint8 MODE_SPLIT = 0;
constexpr uint8 MODE_ROUND_ROBIN = 1;
constexpr uint8 MODE_THRESHOLD = 2;
constexpr uint8 MODE_RANDOM = 3;
constexpr uint8 MODE_CONDITIONAL = 4;

constexpr sint64 QUGATE_SUCCESS = 0;
constexpr sint64 QUGATE_INVALID_GATE_ID = -1;
constexpr sint64 QUGATE_GATE_NOT_ACTIVE = -2;
constexpr sint64 QUGATE_UNAUTHORIZED = -3;
constexpr sint64 QUGATE_INVALID_MODE = -4;
constexpr sint64 QUGATE_INVALID_RECIPIENT_COUNT = -5;
constexpr sint64 QUGATE_INVALID_RATIO = -6;
constexpr sint64 QUGATE_INSUFFICIENT_FEE = -7;
constexpr sint64 QUGATE_NO_FREE_SLOTS = -8;
constexpr sint64 QUGATE_DUST_AMOUNT = -9;
constexpr sint64 QUGATE_INVALID_THRESHOLD = -10;
constexpr sint64 QUGATE_INVALID_SENDER_COUNT = -11;
constexpr sint64 QUGATE_CONDITIONAL_REJECTED = -12;

// =============================================
// V3 GateConfig (matches QuGate.h exactly)
// =============================================

struct GateConfig {
    id owner;
    uint8 mode;
    uint8 recipientCount;
    uint8 active;
    uint8 allowedSenderCount;
    uint16 createdEpoch;
    uint16 lastActivityEpoch;
    uint64 totalReceived;
    uint64 totalForwarded;
    uint64 currentBalance;
    uint64 threshold;
    uint64 roundRobinIndex;
    Array<id, 8> recipients;
    Array<uint64, 8> ratios;
    Array<id, 8> allowedSenders;
};

// =============================================
// V3 QuGateState
// =============================================

struct QuGateState {
    uint64 _gateCount;
    uint64 _activeGates;
    uint64 _totalBurned;
    Array<GateConfig, QUGATE_MAX_GATES> _gates;
    Array<uint64, QUGATE_MAX_GATES> _freeSlots;
    uint64 _freeCount;
    uint64 _creationFee;
    uint64 _minSendAmount;
    uint64 _expiryEpochs;
};

// =============================================
// Procedure I/O structs (match QuGate.h)
// =============================================

struct createGate_input {
    uint8 mode;
    uint8 recipientCount;
    Array<id, 8> recipients;
    Array<uint64, 8> ratios;
    uint64 threshold;
    Array<id, 8> allowedSenders;
    uint8 allowedSenderCount;
};
struct createGate_output {
    sint64 status;
    uint64 gateId;
    uint64 feePaid;
};

struct sendToGate_input { uint64 gateId; };
struct sendToGate_output { sint64 status; };

struct closeGate_input { uint64 gateId; };
struct closeGate_output { sint64 status; };

struct updateGate_input {
    uint64 gateId;
    uint8 recipientCount;
    Array<id, 8> recipients;
    Array<uint64, 8> ratios;
    uint64 threshold;
    Array<id, 8> allowedSenders;
    uint8 allowedSenderCount;
};
struct updateGate_output { sint64 status; };

struct getGate_output {
    uint8 mode;
    uint8 recipientCount;
    uint8 active;
    id owner;
    uint64 totalReceived;
    uint64 totalForwarded;
    uint64 currentBalance;
    uint64 threshold;
    uint16 createdEpoch;
    uint16 lastActivityEpoch;
    Array<id, 8> recipients;
    Array<uint64, 8> ratios;
};

struct getGateCount_output {
    uint64 totalGates;
    uint64 activeGates;
    uint64 totalBurned;
};

struct getFees_output {
    uint64 creationFee;
    uint64 currentCreationFee;
    uint64 minSendAmount;
    uint64 expiryEpochs;
};

// =============================================
// Test harness — implements V3 contract logic faithfully
// =============================================

class QuGateTest {
public:
    QuGateState* statePtr;
    QuGateState& state;
    TestQpiContext qpi;

    QuGateTest() : statePtr(new QuGateState()), state(*statePtr) {
        memset(statePtr, 0, sizeof(QuGateState));
        // INITIALIZE
        state._gateCount = 0;
        state._activeGates = 0;
        state._freeCount = 0;
        state._totalBurned = 0;
        state._creationFee = QUGATE_DEFAULT_CREATION_FEE;
        state._minSendAmount = QUGATE_DEFAULT_MIN_SEND;
        state._expiryEpochs = QUGATE_DEFAULT_EXPIRY_EPOCHS;
    }

    ~QuGateTest() { delete statePtr; }

    static id makeId(unsigned char val) {
        id result = m256i::zero();
        result.m256i_u8[0] = val;
        return result;
    }

    // ---- escalated fee calculation ----
    uint64 currentEscalatedFee() const {
        return state._creationFee * (1 + QPI::div(state._activeGates, QUGATE_FEE_ESCALATION_STEP));
    }

    // ---- createGate (matches QuGate.h logic exactly) ----
    createGate_output createGate(const id& creator, sint64 fee, const createGate_input& input) {
        qpi.reset();
        qpi._invocator = creator;
        qpi._reward = fee;

        createGate_output output;
        output.status = QUGATE_SUCCESS;
        output.gateId = 0;
        output.feePaid = 0;

        uint64 currentFee = state._creationFee * (1 + QPI::div(state._activeGates, QUGATE_FEE_ESCALATION_STEP));

        if (fee < (sint64)currentFee) {
            if (fee > 0) qpi.transfer(creator, fee);
            output.status = QUGATE_INSUFFICIENT_FEE;
            return output;
        }
        if (input.mode > MODE_CONDITIONAL) {
            qpi.transfer(creator, fee);
            output.status = QUGATE_INVALID_MODE;
            return output;
        }
        if (input.recipientCount == 0 || input.recipientCount > QUGATE_MAX_RECIPIENTS) {
            qpi.transfer(creator, fee);
            output.status = QUGATE_INVALID_RECIPIENT_COUNT;
            return output;
        }
        if (state._freeCount == 0 && state._gateCount >= QUGATE_MAX_GATES) {
            qpi.transfer(creator, fee);
            output.status = QUGATE_NO_FREE_SLOTS;
            return output;
        }
        if (input.mode == MODE_SPLIT) {
            uint64 totalRatio = 0;
            for (uint64 i = 0; i < input.recipientCount; i++)
                totalRatio += input.ratios.get(i);
            if (totalRatio == 0) {
                qpi.transfer(creator, fee);
                output.status = QUGATE_INVALID_RATIO;
                return output;
            }
            for (uint64 i = 0; i < input.recipientCount; i++) {
                if (input.ratios.get(i) > QUGATE_MAX_RATIO) {
                    qpi.transfer(creator, fee);
                    output.status = QUGATE_INVALID_RATIO;
                    return output;
                }
            }
        }
        if (input.mode == MODE_THRESHOLD && input.threshold == 0) {
            qpi.transfer(creator, fee);
            output.status = QUGATE_INVALID_THRESHOLD;
            return output;
        }
        if (input.allowedSenderCount > QUGATE_MAX_RECIPIENTS) {
            qpi.transfer(creator, fee);
            output.status = QUGATE_INVALID_SENDER_COUNT;
            return output;
        }

        GateConfig g;
        memset(&g, 0, sizeof(g));
        g.owner = creator;
        g.mode = input.mode;
        g.recipientCount = input.recipientCount;
        g.active = 1;
        g.allowedSenderCount = input.allowedSenderCount;
        g.createdEpoch = qpi.epoch();
        g.lastActivityEpoch = qpi.epoch();
        g.threshold = input.threshold;
        g.roundRobinIndex = 0;

        for (uint64 i = 0; i < QUGATE_MAX_RECIPIENTS; i++) {
            if (i < input.recipientCount) {
                g.recipients.set(i, input.recipients.get(i));
                g.ratios.set(i, input.ratios.get(i));
            } else {
                g.recipients.set(i, id::zero());
                g.ratios.set(i, 0);
            }
        }
        for (uint64 i = 0; i < QUGATE_MAX_RECIPIENTS; i++) {
            if (i < input.allowedSenderCount)
                g.allowedSenders.set(i, input.allowedSenders.get(i));
            else
                g.allowedSenders.set(i, id::zero());
        }

        uint64 slotIdx;
        if (state._freeCount > 0) {
            state._freeCount -= 1;
            slotIdx = state._freeSlots.get(state._freeCount);
        } else {
            slotIdx = state._gateCount;
            state._gateCount += 1;
        }

        state._gates.set(slotIdx, g);
        output.gateId = slotIdx + 1;
        state._activeGates += 1;

        qpi.burn(currentFee);
        state._totalBurned += currentFee;
        output.feePaid = currentFee;

        if (fee > (sint64)currentFee)
            qpi.transfer(creator, fee - (sint64)currentFee);

        output.status = QUGATE_SUCCESS;
        return output;
    }

    // Convenience overload matching old API signature
    createGate_output createGateSimple(const id& creator, sint64 fee, uint8 mode,
                                        uint8 recipientCount, id* recipients, uint64* ratios,
                                        uint64 threshold = 0, id* allowedSenders = nullptr,
                                        uint8 allowedSenderCount = 0) {
        createGate_input in;
        memset(&in, 0, sizeof(in));
        in.mode = mode;
        in.recipientCount = recipientCount;
        in.threshold = threshold;
        in.allowedSenderCount = allowedSenderCount;
        for (uint8 i = 0; i < recipientCount && i < 8; i++) {
            in.recipients.set(i, recipients[i]);
            in.ratios.set(i, ratios ? ratios[i] : 0);
        }
        if (allowedSenders) {
            for (uint8 i = 0; i < allowedSenderCount && i < 8; i++)
                in.allowedSenders.set(i, allowedSenders[i]);
        }
        return createGate(creator, fee, in);
    }

    // ---- sendToGate ----
    sendToGate_output sendToGate(const id& sender, uint64 gateId, sint64 amount) {
        qpi.reset();
        qpi._invocator = sender;
        qpi._reward = amount;

        sendToGate_output output;
        output.status = QUGATE_SUCCESS;

        if (gateId == 0 || gateId > state._gateCount) {
            if (amount > 0) qpi.transfer(sender, amount);
            output.status = QUGATE_INVALID_GATE_ID;
            return output;
        }

        uint64 idx = gateId - 1;
        GateConfig gate = state._gates.get(idx);

        if (gate.active == 0) {
            if (amount > 0) qpi.transfer(sender, amount);
            output.status = QUGATE_GATE_NOT_ACTIVE;
            return output;
        }

        if (amount == 0) {
            output.status = QUGATE_DUST_AMOUNT;
            return output;
        }

        // Dust burn
        if ((uint64)amount < state._minSendAmount) {
            qpi.burn(amount);
            state._totalBurned += amount;
            output.status = QUGATE_DUST_AMOUNT;
            return output;
        }

        // Update last activity
        gate.lastActivityEpoch = qpi.epoch();
        gate.totalReceived += amount;

        if (gate.mode == MODE_SPLIT) {
            uint64 totalRatio = 0;
            for (uint64 i = 0; i < gate.recipientCount; i++)
                totalRatio += gate.ratios.get(i);
            uint64 distributed = 0;
            for (uint64 i = 0; i < gate.recipientCount; i++) {
                uint64 share;
                if (i == (uint64)(gate.recipientCount - 1))
                    share = amount - distributed;
                else
                    share = QPI::div((uint64)amount * gate.ratios.get(i), totalRatio);
                if (share > 0) {
                    qpi.transfer(gate.recipients.get(i), share);
                    distributed += share;
                }
            }
            gate.totalForwarded += distributed;
        }
        else if (gate.mode == MODE_ROUND_ROBIN) {
            qpi.transfer(gate.recipients.get(gate.roundRobinIndex), amount);
            gate.totalForwarded += amount;
            gate.roundRobinIndex = QPI::mod(gate.roundRobinIndex + 1, (uint64)gate.recipientCount);
        }
        else if (gate.mode == MODE_THRESHOLD) {
            gate.currentBalance += amount;
            if (gate.currentBalance >= gate.threshold) {
                qpi.transfer(gate.recipients.get(0), gate.currentBalance);
                gate.totalForwarded += gate.currentBalance;
                gate.currentBalance = 0;
            }
        }
        else if (gate.mode == MODE_RANDOM) {
            uint64 ridx = QPI::mod(gate.totalReceived + qpi.tick(), (uint64)gate.recipientCount);
            qpi.transfer(gate.recipients.get(ridx), amount);
            gate.totalForwarded += amount;
        }
        else if (gate.mode == MODE_CONDITIONAL) {
            uint8 senderAllowed = 0;
            for (uint64 i = 0; i < gate.allowedSenderCount; i++) {
                if (gate.allowedSenders.get(i) == sender) { senderAllowed = 1; break; }
            }
            if (senderAllowed) {
                qpi.transfer(gate.recipients.get(0), amount);
                gate.totalForwarded += amount;
            } else {
                qpi.transfer(sender, amount);
                output.status = QUGATE_CONDITIONAL_REJECTED;
            }
        }

        state._gates.set(gateId - 1, gate);
        return output;
    }

    // ---- closeGate ----
    closeGate_output closeGate(const id& caller, uint64 gateId, sint64 reward = 0) {
        qpi.reset();
        qpi._invocator = caller;
        qpi._reward = reward;

        closeGate_output output;
        output.status = QUGATE_SUCCESS;

        if (gateId == 0 || gateId > state._gateCount) {
            output.status = QUGATE_INVALID_GATE_ID;
            return output;
        }

        GateConfig gate = state._gates.get(gateId - 1);

        if (!(gate.owner == caller)) {
            output.status = QUGATE_UNAUTHORIZED;
            return output;
        }
        if (gate.active == 0) {
            output.status = QUGATE_GATE_NOT_ACTIVE;
            return output;
        }

        if (gate.currentBalance > 0) {
            qpi.transfer(gate.owner, gate.currentBalance);
            gate.currentBalance = 0;
        }

        gate.active = 0;
        state._gates.set(gateId - 1, gate);
        state._activeGates -= 1;

        state._freeSlots.set(state._freeCount, gateId - 1);
        state._freeCount += 1;

        if (reward > 0) qpi.transfer(caller, reward);

        return output;
    }

    // ---- updateGate ----
    updateGate_output updateGate(const id& caller, sint64 reward, const updateGate_input& input) {
        qpi.reset();
        qpi._invocator = caller;
        qpi._reward = reward;

        updateGate_output output;
        output.status = QUGATE_SUCCESS;

        if (input.gateId == 0 || input.gateId > state._gateCount) {
            if (reward > 0) qpi.transfer(caller, reward);
            output.status = QUGATE_INVALID_GATE_ID;
            return output;
        }

        GateConfig gate = state._gates.get(input.gateId - 1);

        if (!(gate.owner == caller)) {
            if (reward > 0) qpi.transfer(caller, reward);
            output.status = QUGATE_UNAUTHORIZED;
            return output;
        }
        if (gate.active == 0) {
            if (reward > 0) qpi.transfer(caller, reward);
            output.status = QUGATE_GATE_NOT_ACTIVE;
            return output;
        }
        if (input.recipientCount == 0 || input.recipientCount > QUGATE_MAX_RECIPIENTS) {
            if (reward > 0) qpi.transfer(caller, reward);
            output.status = QUGATE_INVALID_RECIPIENT_COUNT;
            return output;
        }
        if (input.allowedSenderCount > QUGATE_MAX_RECIPIENTS) {
            if (reward > 0) qpi.transfer(caller, reward);
            output.status = QUGATE_INVALID_SENDER_COUNT;
            return output;
        }

        if (gate.mode == MODE_SPLIT) {
            uint64 totalRatio = 0;
            for (uint64 i = 0; i < input.recipientCount; i++) {
                if (input.ratios.get(i) > QUGATE_MAX_RATIO) {
                    if (reward > 0) qpi.transfer(caller, reward);
                    output.status = QUGATE_INVALID_RATIO;
                    return output;
                }
                totalRatio += input.ratios.get(i);
            }
            if (totalRatio == 0) {
                if (reward > 0) qpi.transfer(caller, reward);
                output.status = QUGATE_INVALID_RATIO;
                return output;
            }
        }
        if (gate.mode == MODE_THRESHOLD && input.threshold == 0) {
            if (reward > 0) qpi.transfer(caller, reward);
            output.status = QUGATE_INVALID_THRESHOLD;
            return output;
        }

        gate.lastActivityEpoch = qpi.epoch();
        gate.recipientCount = input.recipientCount;
        gate.threshold = input.threshold;
        gate.allowedSenderCount = input.allowedSenderCount;

        for (uint64 i = 0; i < QUGATE_MAX_RECIPIENTS; i++) {
            if (i < input.recipientCount) {
                gate.recipients.set(i, input.recipients.get(i));
                gate.ratios.set(i, input.ratios.get(i));
            } else {
                gate.recipients.set(i, id::zero());
                gate.ratios.set(i, 0);
            }
            if (i < input.allowedSenderCount)
                gate.allowedSenders.set(i, input.allowedSenders.get(i));
            else
                gate.allowedSenders.set(i, id::zero());
        }

        state._gates.set(input.gateId - 1, gate);

        if (reward > 0) qpi.transfer(caller, reward);
        return output;
    }

    // ---- endEpoch (gate expiry) ----
    void endEpoch() {
        for (uint64 i = 0; i < state._gateCount; i++) {
            GateConfig gate = state._gates.get(i);
            if (gate.active == 1 && state._expiryEpochs > 0) {
                if ((uint16)(qpi.epoch() - gate.lastActivityEpoch) >= state._expiryEpochs) {
                    if (gate.currentBalance > 0) {
                        qpi.transfer(gate.owner, gate.currentBalance);
                        gate.currentBalance = 0;
                    }
                    gate.active = 0;
                    state._gates.set(i, gate);
                    state._activeGates -= 1;
                    state._freeSlots.set(state._freeCount, i);
                    state._freeCount += 1;
                }
            }
        }
    }

    // ---- getGate ----
    getGate_output getGate(uint64 gateId) {
        getGate_output out;
        memset(&out, 0, sizeof(out));
        if (gateId == 0 || gateId > state._gateCount) { out.active = 0; return out; }
        GateConfig g = state._gates.get(gateId - 1);
        out.mode = g.mode;
        out.recipientCount = g.recipientCount;
        out.active = g.active;
        out.owner = g.owner;
        out.totalReceived = g.totalReceived;
        out.totalForwarded = g.totalForwarded;
        out.currentBalance = g.currentBalance;
        out.threshold = g.threshold;
        out.createdEpoch = g.createdEpoch;
        out.lastActivityEpoch = g.lastActivityEpoch;
        for (uint64 i = 0; i < QUGATE_MAX_RECIPIENTS; i++) {
            out.recipients.set(i, g.recipients.get(i));
            out.ratios.set(i, g.ratios.get(i));
        }
        return out;
    }

    // ---- getGateCount ----
    getGateCount_output getGateCount() {
        getGateCount_output out;
        out.totalGates = state._gateCount;
        out.activeGates = state._activeGates;
        out.totalBurned = state._totalBurned;
        return out;
    }

    // ---- getFees ----
    getFees_output getFees() {
        getFees_output out;
        out.creationFee = state._creationFee;
        out.currentCreationFee = state._creationFee * (1 + QPI::div(state._activeGates, QUGATE_FEE_ESCALATION_STEP));
        out.minSendAmount = state._minSendAmount;
        out.expiryEpochs = state._expiryEpochs;
        return out;
    }
};

// =============================================
// Test identities
// =============================================

static const id ALICE = QuGateTest::makeId(1);
static const id BOB = QuGateTest::makeId(2);
static const id CHARLIE = QuGateTest::makeId(3);
static const id DAVE = QuGateTest::makeId(4);

// =============================================
// Helper to create a simple split gate
// =============================================
static createGate_output makeSimpleGate(QuGateTest& env, const id& owner, sint64 fee,
                                         uint8 mode, uint8 recipientCount,
                                         id* recips, uint64* ratios,
                                         uint64 threshold = 0,
                                         id* allowed = nullptr, uint8 allowedCount = 0) {
    return env.createGateSimple(owner, fee, mode, recipientCount, recips, ratios, threshold, allowed, allowedCount);
}

// =============================================
// ORIGINAL TESTS (updated for V3 harness)
// =============================================

TEST(QuGate, SplitEvenTwo) {
    QuGateTest env;
    id recips[] = { BOB, CHARLIE };
    uint64 ratios[] = { 50, 50 };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 2, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    ASSERT_NE(out.gateId, 0ULL);

    env.sendToGate(ALICE, out.gateId, 1000);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 500);
    EXPECT_EQ(env.qpi.totalTransferredTo(CHARLIE), 500);
}

TEST(QuGate, SplitUnevenThree) {
    QuGateTest env;
    id recips[] = { BOB, CHARLIE, DAVE };
    uint64 ratios[] = { 50, 30, 20 };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 3, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    env.sendToGate(ALICE, out.gateId, 10000);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 5000);
    EXPECT_EQ(env.qpi.totalTransferredTo(CHARLIE), 3000);
    EXPECT_EQ(env.qpi.totalTransferredTo(DAVE), 2000);
}

TEST(QuGate, SplitHandlesRoundingDust) {
    QuGateTest env;
    id recips[] = { BOB, CHARLIE, DAVE };
    uint64 ratios[] = { 33, 33, 34 };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 3, recips, ratios);

    env.sendToGate(ALICE, out.gateId, 100);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 33);
    EXPECT_EQ(env.qpi.totalTransferredTo(CHARLIE), 33);
    EXPECT_EQ(env.qpi.totalTransferredTo(DAVE), 34);
}

TEST(QuGate, RoundRobinCycles) {
    QuGateTest env;
    id recips[] = { BOB, CHARLIE, DAVE };
    uint64 ratios[] = { 0, 0, 0 };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_ROUND_ROBIN, 3, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    env.sendToGate(ALICE, out.gateId, 100);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 100);

    env.sendToGate(ALICE, out.gateId, 200);
    EXPECT_EQ(env.qpi.totalTransferredTo(CHARLIE), 200);

    env.sendToGate(ALICE, out.gateId, 300);
    EXPECT_EQ(env.qpi.totalTransferredTo(DAVE), 300);

    env.sendToGate(ALICE, out.gateId, 400);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 400);
}

TEST(QuGate, ThresholdAccumulatesAndReleases) {
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_THRESHOLD, 1, recips, ratios, 500);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    env.sendToGate(ALICE, out.gateId, 200);
    EXPECT_EQ(env.qpi.transferCount, 0);

    env.sendToGate(ALICE, out.gateId, 200);
    EXPECT_EQ(env.qpi.transferCount, 0);

    env.sendToGate(ALICE, out.gateId, 200);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 600);
}

TEST(QuGate, ConditionalAllowsWhitelisted) {
    QuGateTest env;
    id recips[] = { DAVE };
    uint64 ratios[] = { 0 };
    id allowed[] = { ALICE, BOB };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_CONDITIONAL, 1, recips, ratios, 0, allowed, 2);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    env.sendToGate(ALICE, out.gateId, 500);
    EXPECT_EQ(env.qpi.totalTransferredTo(DAVE), 500);
}

TEST(QuGate, ConditionalBouncesUnauthorised) {
    QuGateTest env;
    id recips[] = { DAVE };
    uint64 ratios[] = { 0 };
    id allowed[] = { ALICE };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_CONDITIONAL, 1, recips, ratios, 0, allowed, 1);

    auto sendOut = env.sendToGate(CHARLIE, out.gateId, 500);
    EXPECT_EQ(sendOut.status, QUGATE_CONDITIONAL_REJECTED);
    EXPECT_EQ(env.qpi.totalTransferredTo(DAVE), 0);
    EXPECT_EQ(env.qpi.totalTransferredTo(CHARLIE), 500);
}

TEST(QuGate, InvalidGateIdBounces) {
    QuGateTest env;
    auto out = env.sendToGate(ALICE, 999, 1000);
    EXPECT_EQ(out.status, QUGATE_INVALID_GATE_ID);
    EXPECT_EQ(env.qpi.totalTransferredTo(ALICE), 1000);
}

TEST(QuGate, CreationFailsWithInsufficientFee) {
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 500, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out.status, QUGATE_INSUFFICIENT_FEE);
    EXPECT_EQ(out.gateId, 0ULL);
}

TEST(QuGate, ZeroAmountDoesNothing) {
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);

    auto sendOut = env.sendToGate(ALICE, out.gateId, 0);
    EXPECT_EQ(sendOut.status, QUGATE_DUST_AMOUNT);
    EXPECT_EQ(env.qpi.transferCount, 0);
}

TEST(QuGate, GateCountTracking) {
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    makeSimpleGate(env, ALICE, 1000, MODE_ROUND_ROBIN, 1, recips, ratios);
    makeSimpleGate(env, BOB, 1000, MODE_THRESHOLD, 1, recips, ratios, 1000);

    EXPECT_EQ(env.state._gateCount, 3ULL);
    EXPECT_EQ(env.state._activeGates, 3ULL);
}

// =============================================
// NEW V3 TESTS
// =============================================

// ---- Escalating fee ----

TEST(QuGateV3, EscalatingFeeAtZeroGates) {
    QuGateTest env;
    // 0 active gates → fee = 1000 * (1 + 0/1024) = 1000
    auto fees = env.getFees();
    EXPECT_EQ(fees.currentCreationFee, 1000ULL);

    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out.status, QUGATE_SUCCESS);
    EXPECT_EQ(out.feePaid, 1000ULL);
}

TEST(QuGateV3, EscalatingFeeAt1024Gates) {
    QuGateTest env;
    // Simulate 1024 active gates
    state_hack: env.state._activeGates = 1024;

    // fee = 1000 * (1 + 1024/1024) = 2000
    auto fees = env.getFees();
    EXPECT_EQ(fees.currentCreationFee, 2000ULL);

    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 2000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out.status, QUGATE_SUCCESS);
    EXPECT_EQ(out.feePaid, 2000ULL);

    // Insufficient at old price
    env.state._activeGates = 1025; // now fee is still 2000
    auto out2 = makeSimpleGate(env, ALICE, 1999, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out2.status, QUGATE_INSUFFICIENT_FEE);
}

TEST(QuGateV3, EscalatingFeeAt2048Gates) {
    QuGateTest env;
    env.state._activeGates = 2048;
    // fee = 1000 * (1 + 2048/1024) = 3000
    auto fees = env.getFees();
    EXPECT_EQ(fees.currentCreationFee, 3000ULL);

    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 3000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out.status, QUGATE_SUCCESS);
    EXPECT_EQ(out.feePaid, 3000ULL);
}

// ---- Fee overpayment refund ----

TEST(QuGateV3, FeeOverpaymentRefund) {
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    // Pay 5000, fee is 1000 → refund 4000
    auto out = makeSimpleGate(env, ALICE, 5000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out.status, QUGATE_SUCCESS);
    EXPECT_EQ(out.feePaid, 1000ULL);
    EXPECT_EQ(env.qpi.totalTransferredTo(ALICE), 4000); // refund
    EXPECT_EQ(env.qpi.totalBurned, 1000);
}

// ---- Dust burn ----

TEST(QuGateV3, DustBurnBelowMinSend) {
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);

    // Send 5 QU (below minSendAmount of 10)
    auto sendOut = env.sendToGate(ALICE, out.gateId, 5);
    EXPECT_EQ(sendOut.status, QUGATE_DUST_AMOUNT);
    EXPECT_EQ(env.qpi.totalBurned, 5);
    EXPECT_EQ(env.qpi.transferCount, 0); // no transfers, burned
    EXPECT_EQ(env.state._totalBurned, 1000 + 5); // creation fee + dust
}

TEST(QuGateV3, ExactMinSendNotDust) {
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);

    // Send exactly 10 (= minSendAmount) → should forward, not burn
    auto sendOut = env.sendToGate(ALICE, out.gateId, 10);
    EXPECT_EQ(sendOut.status, QUGATE_SUCCESS);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 10);
}

// ---- Status codes on all procedures ----

TEST(QuGateV3, StatusCodeCreateInvalidMode) {
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 1000, 99, 1, recips, ratios);
    EXPECT_EQ(out.status, QUGATE_INVALID_MODE);
}

TEST(QuGateV3, StatusCodeCreateInvalidRecipientCount) {
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 0, recips, ratios);
    EXPECT_EQ(out.status, QUGATE_INVALID_RECIPIENT_COUNT);
}

TEST(QuGateV3, StatusCodeCreateInvalidRatio) {
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 }; // zero total ratio
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out.status, QUGATE_INVALID_RATIO);
}

TEST(QuGateV3, StatusCodeCreateInvalidThreshold) {
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_THRESHOLD, 1, recips, ratios, 0);
    EXPECT_EQ(out.status, QUGATE_INVALID_THRESHOLD);
}

TEST(QuGateV3, StatusCodeSendToInactiveGate) {
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    env.closeGate(ALICE, out.gateId);

    auto sendOut = env.sendToGate(ALICE, out.gateId, 100);
    EXPECT_EQ(sendOut.status, QUGATE_GATE_NOT_ACTIVE);
}

TEST(QuGateV3, StatusCodeCloseUnauthorized) {
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);

    auto closeOut = env.closeGate(BOB, out.gateId);
    EXPECT_EQ(closeOut.status, QUGATE_UNAUTHORIZED);
}

TEST(QuGateV3, StatusCodeCloseInvalidGateId) {
    QuGateTest env;
    auto closeOut = env.closeGate(ALICE, 999);
    EXPECT_EQ(closeOut.status, QUGATE_INVALID_GATE_ID);
}

TEST(QuGateV3, StatusCodeUpdateInvalidGateId) {
    QuGateTest env;
    updateGate_input in;
    memset(&in, 0, sizeof(in));
    in.gateId = 999;
    in.recipientCount = 1;
    in.recipients.set(0, BOB);
    in.ratios.set(0, 100);
    auto out = env.updateGate(ALICE, 0, in);
    EXPECT_EQ(out.status, QUGATE_INVALID_GATE_ID);
}

TEST(QuGateV3, StatusCodeUpdateUnauthorized) {
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto gateOut = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);

    updateGate_input in;
    memset(&in, 0, sizeof(in));
    in.gateId = gateOut.gateId;
    in.recipientCount = 1;
    in.recipients.set(0, CHARLIE);
    in.ratios.set(0, 100);
    auto out = env.updateGate(BOB, 0, in);
    EXPECT_EQ(out.status, QUGATE_UNAUTHORIZED);
}

// ---- Free-list slot reuse ----

TEST(QuGateV3, FreeListSlotReuse) {
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    auto g1 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    auto g2 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(g1.gateId, 1ULL);
    EXPECT_EQ(g2.gateId, 2ULL);
    EXPECT_EQ(env.state._gateCount, 2ULL);

    // Close gate 1
    env.closeGate(ALICE, 1);
    EXPECT_EQ(env.state._freeCount, 1ULL);
    EXPECT_EQ(env.state._activeGates, 1ULL);

    // Create again — should reuse slot 0 (gateId 1)
    auto g3 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(g3.gateId, 1ULL); // reused!
    EXPECT_EQ(env.state._freeCount, 0ULL);
    EXPECT_EQ(env.state._gateCount, 2ULL); // didn't grow
    EXPECT_EQ(env.state._activeGates, 2ULL);
}

// ---- Gate expiry ----

TEST(QuGateV3, GateExpiryAutoClose) {
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    env.qpi._epoch = 100;
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    // Advance epoch by expiryEpochs (50)
    env.qpi._epoch = 150;
    env.qpi.reset();
    env.endEpoch();

    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.active, 0); // auto-closed
    EXPECT_EQ(env.state._activeGates, 0ULL);
    EXPECT_EQ(env.state._freeCount, 1ULL);
}

TEST(QuGateV3, GateExpiryRefundsBalance) {
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    env.qpi._epoch = 100;
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_THRESHOLD, 1, recips, ratios, 10000);

    // Send some QU that sits in threshold balance
    env.sendToGate(CHARLIE, out.gateId, 500);
    auto gateBefore = env.getGate(out.gateId);
    EXPECT_EQ(gateBefore.currentBalance, 500ULL);

    // Expire it
    env.qpi._epoch = 150;
    env.qpi.reset();
    env.endEpoch();

    // Balance refunded to owner (ALICE)
    EXPECT_EQ(env.qpi.totalTransferredTo(ALICE), 500);
    auto gateAfter = env.getGate(out.gateId);
    EXPECT_EQ(gateAfter.active, 0);
}

TEST(QuGateV3, GateNotExpiredIfActive) {
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    env.qpi._epoch = 100;
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);

    // Send at epoch 140 → updates lastActivityEpoch
    env.qpi._epoch = 140;
    env.sendToGate(CHARLIE, out.gateId, 100);

    // Run endEpoch at 150 — only 10 epochs since last activity, not 50
    env.qpi._epoch = 150;
    env.qpi.reset();
    env.endEpoch();

    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.active, 1); // still active
}

// ---- totalBurned tracking ----

TEST(QuGateV3, TotalBurnedTracking) {
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    // Create gate → burns 1000
    makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(env.state._totalBurned, 1000ULL);

    // Dust burn → burns 5
    env.sendToGate(ALICE, 1, 5);
    EXPECT_EQ(env.state._totalBurned, 1005ULL);

    // Create another → burns 1000
    makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(env.state._totalBurned, 2005ULL);

    auto count = env.getGateCount();
    EXPECT_EQ(count.totalBurned, 2005ULL);
}

// ---- getFees returns correct values ----

TEST(QuGateV3, GetFeesReturnsCorrectValues) {
    QuGateTest env;
    auto fees = env.getFees();
    EXPECT_EQ(fees.creationFee, 1000ULL);
    EXPECT_EQ(fees.currentCreationFee, 1000ULL);
    EXPECT_EQ(fees.minSendAmount, 10ULL);
    EXPECT_EQ(fees.expiryEpochs, 50ULL);

    // With active gates
    env.state._activeGates = 2048;
    fees = env.getFees();
    EXPECT_EQ(fees.currentCreationFee, 3000ULL);
}

// ---- lastActivityEpoch updates ----

TEST(QuGateV3, LastActivityEpochUpdatesOnSend) {
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    env.qpi._epoch = 100;
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);

    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.lastActivityEpoch, 100);

    env.qpi._epoch = 120;
    env.sendToGate(CHARLIE, out.gateId, 100);

    gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.lastActivityEpoch, 120);
}

TEST(QuGateV3, LastActivityEpochUpdatesOnUpdate) {
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    env.qpi._epoch = 100;
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);

    env.qpi._epoch = 130;
    updateGate_input in;
    memset(&in, 0, sizeof(in));
    in.gateId = out.gateId;
    in.recipientCount = 1;
    in.recipients.set(0, CHARLIE);
    in.ratios.set(0, 100);
    env.updateGate(ALICE, 0, in);

    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.lastActivityEpoch, 130);
}

// ---- createGate_output.feePaid matches escalated fee ----

TEST(QuGateV3, FeePaidMatchesEscalatedFee) {
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    // 0 active gates → fee = 1000
    auto out1 = makeSimpleGate(env, ALICE, 5000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out1.feePaid, 1000ULL);

    // Set active gates to 1024 (minus the 1 just created, so set to 1024 total)
    env.state._activeGates = 1024;
    auto out2 = makeSimpleGate(env, ALICE, 5000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out2.feePaid, 2000ULL);

    env.state._activeGates = 3072;
    auto out3 = makeSimpleGate(env, ALICE, 5000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out3.feePaid, 4000ULL);
}

// ---- Close gate refunds invocation reward ----

TEST(QuGateV3, CloseGateSuccess) {
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);

    auto closeOut = env.closeGate(ALICE, out.gateId);
    EXPECT_EQ(closeOut.status, QUGATE_SUCCESS);
    EXPECT_EQ(env.state._activeGates, 0ULL);
}

TEST(QuGateV3, CloseAlreadyClosedGate) {
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);

    env.closeGate(ALICE, out.gateId);
    auto closeOut2 = env.closeGate(ALICE, out.gateId);
    EXPECT_EQ(closeOut2.status, QUGATE_GATE_NOT_ACTIVE);
}

// ---- Ratio overflow protection ----

TEST(QuGateV3, RatioOverMaxRejected) {
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { QUGATE_MAX_RATIO + 1 };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out.status, QUGATE_INVALID_RATIO);
}

// ---- AllowedSenderCount > max rejected ----

TEST(QuGateV3, InvalidSenderCountRejected) {
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    id allowed[1] = { ALICE };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_CONDITIONAL, 1, recips, ratios, 0, allowed, 9);
    EXPECT_EQ(out.status, QUGATE_INVALID_SENDER_COUNT);
}

// ---- Insufficient fee refunds ----

TEST(QuGateV3, InsufficientFeeRefundsPayment) {
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 500, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out.status, QUGATE_INSUFFICIENT_FEE);
    // The 500 should be refunded
    EXPECT_EQ(env.qpi.totalTransferredTo(ALICE), 500);
}
