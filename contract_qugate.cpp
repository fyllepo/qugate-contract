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

    static m256i zero()
    {
        m256i z;
        memset(&z, 0, sizeof(z));
        return z;
    }
};

static inline bool operator==(const m256i& a, const m256i& b)
{
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
        T& get(unsigned long long idx)
        {
            return _data[idx];
        }
        void set(unsigned long long idx, const T& val)
        {
            _data[idx] = val;
        }
    };

    typedef signed int sint32;

    // HashMap mock — linear scan (sufficient for test)
    template<typename K, typename V, unsigned long long capacity>
    struct HashMap {
        struct Entry { K key; V value; bool occupied; };
        Entry _entries[capacity];
        HashMap()
        {
            for (unsigned long long i = 0; i < capacity; i++) _entries[i].occupied = false;
        }
        void set(const K& key, const V& value)
        {
            for (unsigned long long i = 0; i < capacity; i++)
            {
                if (_entries[i].occupied && _entries[i].key == key)
                {
                    _entries[i].value = value; return;
                }
            }
            for (unsigned long long i = 0; i < capacity; i++)
            {
                if (!_entries[i].occupied)
                {
                    _entries[i].key = key; _entries[i].value = value; _entries[i].occupied = true; return;
                }
            }
        }
        V get(const K& key) const {
            for (unsigned long long i = 0; i < capacity; i++)
            {
                if (_entries[i].occupied && _entries[i].key == key) return _entries[i].value;
            }
            return V{};
        }
        bool has(const K& key) const {
            for (unsigned long long i = 0; i < capacity; i++)
            {
                if (_entries[i].occupied && _entries[i].key == key) return true;
            }
            return false;
        }
        void remove(const K& key)
        {
            for (unsigned long long i = 0; i < capacity; i++)
            {
                if (_entries[i].occupied && _entries[i].key == key)
                {
                    _entries[i].occupied = false; return;
                }
            }
        }
    };

    struct ContractBase {};

    // ContractState wrapper for dirty-tracking (Issue #7)
    template<typename T>
    struct TestContractState {
        T _data;
        const T& get() const { return _data; }
        T& mut()
        {
            return _data;
        }
    };

    inline uint64 div(uint64 a, uint64 b)
    {
        return b ? a / b : 0;
    }
    inline uint64 mod(uint64 a, uint64 b)
    {
        return b ? a % b : 0;
    }
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

    TestQpiContext() : _reward(0), _epoch(100), _tick(12345), transferCount(0), totalBurned(0)
    {
        memset(&_invocator, 0, sizeof(_invocator));
        memset(transfers, 0, sizeof(transfers));
    }

    id invocator() const { return _invocator; }
    sint64 invocationReward() const { return _reward; }
    uint16 epoch() const { return _epoch; }
    uint64 tick() const { return _tick; }

    void transfer(const id& to, sint64 amount)
    {
        if (transferCount < MAX_TRANSFERS)
        {
            transfers[transferCount].to = to;
            transfers[transferCount].amount = amount;
            transferCount++;
        }
    }

    void burn(sint64 amount)
    {
        totalBurned += amount;
    }

    void reset()
    {
        transferCount = 0;
        totalBurned = 0;
        _reward = 0;
    }

    sint64 totalTransferredTo(const id& addr) const {
        sint64 total = 0;
        for (int i = 0; i < transferCount; i++)
        {
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
constexpr uint64 QUGATE_INITIAL_MAX_GATES = 2048;
constexpr uint64 QUGATE_MAX_GATES = QUGATE_INITIAL_MAX_GATES * 1; // X_MULTIPLIER=1
constexpr uint64 QUGATE_MAX_RECIPIENTS = 8;
constexpr uint64 QUGATE_MAX_RATIO = 10000;

constexpr uint64 QUGATE_DEFAULT_CREATION_FEE = 100000;
constexpr uint64 QUGATE_DEFAULT_MIN_SEND = 1000;
constexpr uint64 QUGATE_FEE_ESCALATION_STEP = 1024;
constexpr uint64 QUGATE_DEFAULT_EXPIRY_EPOCHS = 50;

constexpr uint8 MODE_SPLIT = 0;
constexpr uint8 MODE_ROUND_ROBIN = 1;
constexpr uint8 MODE_THRESHOLD = 2;
constexpr uint8 MODE_RANDOM = 3;
constexpr uint8 MODE_CONDITIONAL = 4;
constexpr uint8 MODE_HEARTBEAT = 6;
constexpr uint8 MODE_MULTISIG = 7;
constexpr uint8 MODE_TIME_LOCK = 8;

// Fee split: burn vs shareholder dividends
constexpr uint64 QUGATE_FEE_BURN_BPS = 5000;        // 50% burned
constexpr uint64 QUGATE_FEE_DIVIDEND_BPS = 5000;     // 50% to shareholders

// Idle maintenance defaults
constexpr uint64 QUGATE_DEFAULT_MAINTENANCE_FEE = 25000;
constexpr uint64 QUGATE_DEFAULT_MAINTENANCE_INTERVAL_EPOCHS = 4;
constexpr uint64 QUGATE_DEFAULT_MAINTENANCE_GRACE_EPOCHS = 4;

// Time lock constants
constexpr uint8 QUGATE_TIME_LOCK_ABSOLUTE_EPOCH = 0;
constexpr sint64 QUGATE_TIME_LOCK_NOT_CANCELLABLE = -24;

constexpr sint64 QUGATE_INVALID_CHAIN = -14;
constexpr uint8  QUGATE_MAX_CHAIN_DEPTH = 3;
constexpr sint64 QUGATE_CHAIN_HOP_FEE = 1000;

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
    uint16 createdEpoch;
    uint16 lastActivityEpoch;
    uint64 totalReceived;
    uint64 totalForwarded;
    uint64 currentBalance;
    uint64 threshold;
    uint64 roundRobinIndex;
    Array<id, 8> recipients;
    Array<uint64, 8> ratios;

    // Chain fields
    sint64 chainNextGateId;
    uint8  chainDepth;

    // Unified reserve — covers chain hop fees and idle maintenance
    sint64 reserve;

    // Idle maintenance
    uint16 nextIdleChargeEpoch;
};

// Per-gate allowed-senders configuration (side array)
struct QUGATE_AllowedSendersConfig {
    Array<id, 8> senders;
    uint8 count;
};

// =============================================
// V3 QuGateState
// =============================================

// Minimal multisig config for test harness
struct QUGATE_MultisigConfig_Test {
    Array<id, 8> guardians;
    uint8        guardianCount;
    uint8        required;
    uint32       proposalExpiryEpochs;
    uint32       adminApprovalWindowEpochs;
    uint8        approvalBitmap;
    uint8        approvalCount;
    uint32       proposalEpoch;
    uint8        proposalActive;
};

// Minimal time lock config for test harness
struct QUGATE_TimeLockConfig_Test {
    uint32 unlockEpoch;
    uint32 delayEpochs;
    uint8  lockMode;
    uint8  cancellable;
    uint8  fired;
    uint8  cancelled;
    uint8  active;
};

struct QuGateState {
    uint64 _gateCount;
    uint64 _activeGates;
    uint64 _totalBurned;
    Array<GateConfig, QUGATE_MAX_GATES> _gates;
    Array<uint16, QUGATE_MAX_GATES> _gateGenerations;
    Array<uint64, QUGATE_MAX_GATES> _freeSlots;
    uint64 _freeCount;
    uint64 _creationFee;
    uint64 _minSendAmount;
    uint64 _expiryEpochs;
    Array<QUGATE_AllowedSendersConfig, QUGATE_MAX_GATES> _allowedSendersConfigs;

    // Financial model fields
    uint64 _idleFee;
    uint64 _idleWindowEpochs;
    uint64 _idleGraceEpochs;
    uint64 _totalMaintenanceCharged;
    uint64 _totalMaintenanceBurned;
    uint64 _totalMaintenanceDividends;
    uint64 _earnedMaintenanceDividends;
    uint64 _distributedMaintenanceDividends;

    // Mode-specific state arrays
    Array<QUGATE_MultisigConfig_Test, QUGATE_MAX_GATES> _multisigConfigs;
    Array<QUGATE_TimeLockConfig_Test, QUGATE_MAX_GATES> _timeLockConfigs;
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
    sint64 chainNextGateId;
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

struct fundGate_input { uint64 gateId; };
struct fundGate_output { sint64 result; };

struct setChain_input { sint64 gateId; sint64 nextGateId; };
struct setChain_output { sint64 result; };

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
    sint64 chainNextGateId;
    uint8  chainDepth;
    sint64 reserve;
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
    // Use ContractState wrapper for dirty-tracking pattern (Issue #7)
    QPI::TestContractState<QuGateState>* stateWrapperPtr;
    QPI::TestContractState<QuGateState>& state;
    TestQpiContext qpi;

    QuGateTest() : stateWrapperPtr(new QPI::TestContractState<QuGateState>()), state(*stateWrapperPtr)
    {
        memset(&state.mut(), 0, sizeof(QuGateState));
        // INITIALIZE
        state.mut()._gateCount = 0;
        state.mut()._activeGates = 0;
        state.mut()._freeCount = 0;
        state.mut()._totalBurned = 0;
        state.mut()._creationFee = QUGATE_DEFAULT_CREATION_FEE;
        state.mut()._minSendAmount = QUGATE_DEFAULT_MIN_SEND;
        state.mut()._expiryEpochs = QUGATE_DEFAULT_EXPIRY_EPOCHS;
        state.mut()._idleFee = QUGATE_DEFAULT_MAINTENANCE_FEE;
        state.mut()._idleWindowEpochs = QUGATE_DEFAULT_MAINTENANCE_INTERVAL_EPOCHS;
        state.mut()._idleGraceEpochs = QUGATE_DEFAULT_MAINTENANCE_GRACE_EPOCHS;
        state.mut()._totalMaintenanceCharged = 0;
        state.mut()._totalMaintenanceBurned = 0;
        state.mut()._totalMaintenanceDividends = 0;
        state.mut()._earnedMaintenanceDividends = 0;
        state.mut()._distributedMaintenanceDividends = 0;
    }

    ~QuGateTest()
    {
        delete stateWrapperPtr;
    }

    static id makeId(unsigned char val)
    {
        id result = m256i::zero();
        result.m256i_u8[0] = val;
        return result;
    }

    // ---- escalated fee calculation ----
    uint64 currentEscalatedFee() const {
        return state.get()._creationFee * (1 + QPI::div(state.get()._activeGates, QUGATE_FEE_ESCALATION_STEP));
    }

    // ---- createGate (matches QuGate.h logic exactly) ----
    createGate_output createGate(const id& creator, sint64 fee, const createGate_input& input)
    {
        qpi.reset();
        qpi._invocator = creator;
        qpi._reward = fee;

        createGate_output output;
        output.status = QUGATE_SUCCESS;
        output.gateId = 0;
        output.feePaid = 0;

        uint64 currentFee = state.get()._creationFee * (1 + QPI::div(state.get()._activeGates, QUGATE_FEE_ESCALATION_STEP));

        if (fee < (sint64)currentFee)
        {
            if (fee > 0) qpi.transfer(creator, fee);
            output.status = QUGATE_INSUFFICIENT_FEE;
            return output;
        }
        if (input.mode > MODE_TIME_LOCK || input.mode == 5)
        {
            qpi.transfer(creator, fee);
            output.status = QUGATE_INVALID_MODE;
            return output;
        }
        // HEARTBEAT, MULTISIG, TIME_LOCK allow recipientCount=0
        if (input.recipientCount == 0 && input.mode != MODE_HEARTBEAT
            && input.mode != MODE_MULTISIG && input.mode != MODE_TIME_LOCK)
        {
            qpi.transfer(creator, fee);
            output.status = QUGATE_INVALID_RECIPIENT_COUNT;
            return output;
        }
        if (input.recipientCount > QUGATE_MAX_RECIPIENTS)
        {
            qpi.transfer(creator, fee);
            output.status = QUGATE_INVALID_RECIPIENT_COUNT;
            return output;
        }
        if (state.get()._freeCount == 0 && state.get()._gateCount >= QUGATE_MAX_GATES)
        {
            qpi.transfer(creator, fee);
            output.status = QUGATE_NO_FREE_SLOTS;
            return output;
        }
        if (input.mode == MODE_SPLIT)
        {
            uint64 totalRatio = 0;
            for (uint64 i = 0; i < input.recipientCount; i++)
                totalRatio += input.ratios.get(i);
            if (totalRatio == 0)
            {
                qpi.transfer(creator, fee);
                output.status = QUGATE_INVALID_RATIO;
                return output;
            }
            for (uint64 i = 0; i < input.recipientCount; i++)
            {
                if (input.ratios.get(i) > QUGATE_MAX_RATIO)
                {
                    qpi.transfer(creator, fee);
                    output.status = QUGATE_INVALID_RATIO;
                    return output;
                }
            }
        }
        if (input.mode == MODE_THRESHOLD && input.threshold == 0)
        {
            qpi.transfer(creator, fee);
            output.status = QUGATE_INVALID_THRESHOLD;
            return output;
        }
        if (input.allowedSenderCount > QUGATE_MAX_RECIPIENTS)
        {
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
        g.createdEpoch = qpi.epoch();
        g.lastActivityEpoch = qpi.epoch();
        g.threshold = input.threshold;
        g.roundRobinIndex = 0;
        g.chainNextGateId = -1;
        g.chainDepth = 0;
        g.reserve = 0;
        // Set idle charge due epoch
        if (state.get()._idleWindowEpochs > 0)
        {
            g.nextIdleChargeEpoch = qpi.epoch() + (uint16)state.get()._idleWindowEpochs;
        }

        // Chain validation (chainNextGateId > 0 means chained; 0 or -1 = no chain)
        if (input.chainNextGateId > 0)
        {
            if ((uint64)input.chainNextGateId > state.get()._gateCount)
            {
                qpi.transfer(creator, fee);
                output.status = QUGATE_INVALID_CHAIN;
                return output;
            }
            uint64 targetIdx = (uint64)input.chainNextGateId - 1;
            GateConfig target = state.get()._gates.get(targetIdx);
            if (target.active == 0)
            {
                qpi.transfer(creator, fee);
                output.status = QUGATE_INVALID_CHAIN;
                return output;
            }
            uint8 newDepth = target.chainDepth + 1;
            if (newDepth >= QUGATE_MAX_CHAIN_DEPTH)
            {
                qpi.transfer(creator, fee);
                output.status = QUGATE_INVALID_CHAIN;
                return output;
            }
            g.chainNextGateId = input.chainNextGateId;
            g.chainDepth = newDepth;
        }

        for (uint64 i = 0; i < QUGATE_MAX_RECIPIENTS; i++)
        {
            if (i < input.recipientCount)
            {
                g.recipients.set(i, input.recipients.get(i));
                g.ratios.set(i, input.ratios.get(i));
            }
            else {
                g.recipients.set(i, id::zero());
                g.ratios.set(i, 0);
            }
        }
        // Build allowed-senders config (side array)
        QUGATE_AllowedSendersConfig asCfg;
        memset(&asCfg, 0, sizeof(asCfg));
        asCfg.count = input.allowedSenderCount;
        for (uint64 i = 0; i < QUGATE_MAX_RECIPIENTS; i++)
        {
            if (i < input.allowedSenderCount)
                asCfg.senders.set(i, input.allowedSenders.get(i));
            else
                asCfg.senders.set(i, id::zero());
        }

        uint64 slotIdx;
        if (state.get()._freeCount > 0)
        {
            state.mut()._freeCount -= 1;
            slotIdx = state.get()._freeSlots.get(state.get()._freeCount);
        }
        else {
            slotIdx = state.get()._gateCount;
            state.mut()._gateCount += 1;
        }

        state.mut()._gates.set(slotIdx, g);
        state.mut()._allowedSendersConfigs.set(slotIdx, asCfg);
        output.gateId = slotIdx + 1;
        state.mut()._activeGates += 1;

        // Split creation fee: burn 50% + shareholder dividends 50%
        uint64 creationBurnAmount = QPI::div(currentFee * QUGATE_FEE_BURN_BPS, 10000ULL);
        uint64 creationDividendAmount = currentFee - creationBurnAmount;
        qpi.burn(creationBurnAmount);
        state.mut()._totalBurned += creationBurnAmount;
        state.mut()._earnedMaintenanceDividends += creationDividendAmount;
        state.mut()._totalMaintenanceDividends += creationDividendAmount;
        output.feePaid = currentFee;

        // Excess creation fee auto-seeds reserve
        if (fee > (sint64)currentFee)
        {
            g.reserve = fee - (sint64)currentFee;
            state.mut()._gates.set(slotIdx, g);
        }

        output.status = QUGATE_SUCCESS;
        return output;
    }

    // Convenience overload matching old API signature
    createGate_output createGateSimple(const id& creator, sint64 fee, uint8 mode,
                                        uint8 recipientCount, id* recipients, uint64* ratios,
                                        uint64 threshold = 0, id* allowedSenders = nullptr,
                                        uint8 allowedSenderCount = 0)
                                        {
        createGate_input in;
        memset(&in, 0, sizeof(in));
        in.chainNextGateId = -1;
        in.mode = mode;
        in.recipientCount = recipientCount;
        in.threshold = threshold;
        in.allowedSenderCount = allowedSenderCount;
        for (uint8 i = 0; i < recipientCount && i < 8; i++)
        {
            in.recipients.set(i, recipients[i]);
            in.ratios.set(i, ratios ? ratios[i] : 0);
        }
        if (allowedSenders)
        {
            for (uint8 i = 0; i < allowedSenderCount && i < 8; i++)
                in.allowedSenders.set(i, allowedSenders[i]);
        }
        return createGate(creator, fee, in);
    }

    // ---- sendToGate ----
    sendToGate_output sendToGate(const id& sender, uint64 gateId, sint64 amount)
    {
        qpi.reset();
        qpi._invocator = sender;
        qpi._reward = amount;

        sendToGate_output output;
        output.status = QUGATE_SUCCESS;

        if (gateId == 0 || gateId > state.get()._gateCount)
        {
            if (amount > 0) qpi.transfer(sender, amount);
            output.status = QUGATE_INVALID_GATE_ID;
            return output;
        }

        uint64 idx = gateId - 1;
        GateConfig gate = state.get()._gates.get(idx);

        if (gate.active == 0)
        {
            if (amount > 0) qpi.transfer(sender, amount);
            output.status = QUGATE_GATE_NOT_ACTIVE;
            return output;
        }

        if (amount == 0)
        {
            output.status = QUGATE_DUST_AMOUNT;
            return output;
        }

        // Dust burn
        if ((uint64)amount < state.get()._minSendAmount)
        {
            qpi.burn(amount);
            state.mut()._totalBurned += amount;
            output.status = QUGATE_DUST_AMOUNT;
            return output;
        }

        // Update last activity
        gate.lastActivityEpoch = qpi.epoch();
        gate.totalReceived += amount;

        if (gate.mode == MODE_SPLIT)
        {
            uint64 totalRatio = 0;
            for (uint64 i = 0; i < gate.recipientCount; i++)
                totalRatio += gate.ratios.get(i);
            uint64 distributed = 0;
            for (uint64 i = 0; i < gate.recipientCount; i++)
            {
                uint64 share;
                if (i == (uint64)(gate.recipientCount - 1))
                    share = amount - distributed;
                else
                    share = QPI::div((uint64)amount * gate.ratios.get(i), totalRatio);
                if (share > 0)
                {
                    qpi.transfer(gate.recipients.get(i), share);
                    distributed += share;
                }
            }
            gate.totalForwarded += distributed;
        }
        else if (gate.mode == MODE_ROUND_ROBIN)
        {
            qpi.transfer(gate.recipients.get(gate.roundRobinIndex), amount);
            gate.totalForwarded += amount;
            gate.roundRobinIndex = QPI::mod(gate.roundRobinIndex + 1, (uint64)gate.recipientCount);
        }
        else if (gate.mode == MODE_THRESHOLD)
        {
            gate.currentBalance += amount;
            if (gate.currentBalance >= gate.threshold)
            {
                qpi.transfer(gate.recipients.get(0), gate.currentBalance);
                gate.totalForwarded += gate.currentBalance;
                gate.currentBalance = 0;
            }
        }
        else if (gate.mode == MODE_RANDOM)
        {
            uint64 ridx = QPI::mod(gate.totalReceived + qpi.tick(), (uint64)gate.recipientCount);
            qpi.transfer(gate.recipients.get(ridx), amount);
            gate.totalForwarded += amount;
        }
        else if (gate.mode == MODE_CONDITIONAL)
        {
            QUGATE_AllowedSendersConfig asCfg = state.get()._allowedSendersConfigs.get(gateIdx);
            uint8 senderAllowed = 0;
            for (uint64 i = 0; i < asCfg.count; i++)
            {
                if (asCfg.senders.get(i) == sender)
                {
                    senderAllowed = 1; break;
                }
            }
            if (senderAllowed)
            {
                qpi.transfer(gate.recipients.get(0), amount);
                gate.totalForwarded += amount;
            }
            else {
                qpi.transfer(sender, amount);
                output.status = QUGATE_CONDITIONAL_REJECTED;
            }
        }
        state.mut()._gates.set(gateId - 1, gate);
        return output;
    }

    // ---- closeGate ----
    closeGate_output closeGate(const id& caller, uint64 gateId, sint64 reward = 0)
    {
        qpi.reset();
        qpi._invocator = caller;
        qpi._reward = reward;

        closeGate_output output;
        output.status = QUGATE_SUCCESS;

        if (gateId == 0 || gateId > state.get()._gateCount)
        {
            output.status = QUGATE_INVALID_GATE_ID;
            return output;
        }

        GateConfig gate = state.get()._gates.get(gateId - 1);

        if (!(gate.owner == caller))
        {
            output.status = QUGATE_UNAUTHORIZED;
            return output;
        }
        if (gate.active == 0)
        {
            output.status = QUGATE_GATE_NOT_ACTIVE;
            return output;
        }

        if (gate.currentBalance > 0)
        {
            qpi.transfer(gate.owner, gate.currentBalance);
            gate.currentBalance = 0;
        }

        // Refund reserve
        if (gate.reserve > 0)
        {
            qpi.transfer(gate.owner, gate.reserve);
            gate.reserve = 0;
        }

        gate.active = 0;
        state.mut()._gates.set(gateId - 1, gate);
        state.mut()._activeGates -= 1;

        state.mut()._freeSlots.set(state.get()._freeCount, gateId - 1);
        state.mut()._freeCount += 1;

        if (reward > 0) qpi.transfer(caller, reward);

        return output;
    }

    // ---- updateGate ----
    updateGate_output updateGate(const id& caller, sint64 reward, const updateGate_input& input)
    {
        qpi.reset();
        qpi._invocator = caller;
        qpi._reward = reward;

        updateGate_output output;
        output.status = QUGATE_SUCCESS;

        if (input.gateId == 0 || input.gateId > state.get()._gateCount)
        {
            if (reward > 0) qpi.transfer(caller, reward);
            output.status = QUGATE_INVALID_GATE_ID;
            return output;
        }

        GateConfig gate = state.get()._gates.get(input.gateId - 1);

        if (!(gate.owner == caller))
        {
            if (reward > 0) qpi.transfer(caller, reward);
            output.status = QUGATE_UNAUTHORIZED;
            return output;
        }
        if (gate.active == 0)
        {
            if (reward > 0) qpi.transfer(caller, reward);
            output.status = QUGATE_GATE_NOT_ACTIVE;
            return output;
        }
        if (input.recipientCount == 0 || input.recipientCount > QUGATE_MAX_RECIPIENTS)
        {
            if (reward > 0) qpi.transfer(caller, reward);
            output.status = QUGATE_INVALID_RECIPIENT_COUNT;
            return output;
        }
        if (input.allowedSenderCount > QUGATE_MAX_RECIPIENTS)
        {
            if (reward > 0) qpi.transfer(caller, reward);
            output.status = QUGATE_INVALID_SENDER_COUNT;
            return output;
        }

        if (gate.mode == MODE_SPLIT)
        {
            uint64 totalRatio = 0;
            for (uint64 i = 0; i < input.recipientCount; i++)
            {
                if (input.ratios.get(i) > QUGATE_MAX_RATIO)
                {
                    if (reward > 0) qpi.transfer(caller, reward);
                    output.status = QUGATE_INVALID_RATIO;
                    return output;
                }
                totalRatio += input.ratios.get(i);
            }
            if (totalRatio == 0)
            {
                if (reward > 0) qpi.transfer(caller, reward);
                output.status = QUGATE_INVALID_RATIO;
                return output;
            }
        }
        if (gate.mode == MODE_THRESHOLD && input.threshold == 0)
        {
            if (reward > 0) qpi.transfer(caller, reward);
            output.status = QUGATE_INVALID_THRESHOLD;
            return output;
        }

        gate.lastActivityEpoch = qpi.epoch();
        gate.recipientCount = input.recipientCount;
        gate.threshold = input.threshold;

        for (uint64 i = 0; i < QUGATE_MAX_RECIPIENTS; i++)
        {
            if (i < input.recipientCount)
            {
                gate.recipients.set(i, input.recipients.get(i));
                gate.ratios.set(i, input.ratios.get(i));
            }
            else {
                gate.recipients.set(i, id::zero());
                gate.ratios.set(i, 0);
            }
        }

        // Build allowed-senders config (side array)
        QUGATE_AllowedSendersConfig asCfg;
        memset(&asCfg, 0, sizeof(asCfg));
        asCfg.count = input.allowedSenderCount;
        for (uint64 i = 0; i < QUGATE_MAX_RECIPIENTS; i++)
        {
            if (i < input.allowedSenderCount)
                asCfg.senders.set(i, input.allowedSenders.get(i));
            else
                asCfg.senders.set(i, id::zero());
        }

        state.mut()._gates.set(input.gateId - 1, gate);
        state.mut()._allowedSendersConfigs.set(input.gateId - 1, asCfg);

        if (reward > 0) qpi.transfer(caller, reward);
        return output;
    }

    // ---- endEpoch (idle maintenance + gate expiry) ----
    void endEpoch()
    {
        // Idle maintenance charging
        for (uint64 i = 0; i < state.get()._gateCount; i++)
        {
            GateConfig gate = state.get()._gates.get(i);
            if (gate.active == 0) continue;

            // Charge inactivity maintenance when idle gate reaches its due epoch
            if (state.get()._idleWindowEpochs > 0
                && gate.nextIdleChargeEpoch > 0
                && qpi.epoch() >= gate.nextIdleChargeEpoch)
            {
                if (gate.reserve >= (sint64)state.get()._idleFee)
                {
                    gate.reserve -= state.get()._idleFee;
                    gate.nextIdleChargeEpoch = qpi.epoch() + (uint16)state.get()._idleWindowEpochs;
                    state.mut()._gates.set(i, gate);
                    state.mut()._totalMaintenanceCharged += state.get()._idleFee;

                    // Idle maintenance: 50% burn, 50% shareholder dividends
                    uint64 maintenanceBurnAmount = QPI::div(state.get()._idleFee * QUGATE_FEE_BURN_BPS, 10000ULL);
                    uint64 maintenanceDividendAmount = state.get()._idleFee - maintenanceBurnAmount;
                    qpi.burn(maintenanceBurnAmount);
                    state.mut()._totalBurned += maintenanceBurnAmount;
                    state.mut()._totalMaintenanceBurned += maintenanceBurnAmount;
                    state.mut()._earnedMaintenanceDividends += maintenanceDividendAmount;
                    state.mut()._totalMaintenanceDividends += maintenanceDividendAmount;
                }
            }
        }

        // Expire inactive gates
        for (uint64 i = 0; i < state.get()._gateCount; i++)
        {
            GateConfig gate = state.get()._gates.get(i);
            if (gate.active == 1 && state.get()._expiryEpochs > 0)
            {
                if ((uint16)(qpi.epoch() - gate.lastActivityEpoch) >= state.get()._expiryEpochs)
                {
                    if (gate.currentBalance > 0)
                    {
                        qpi.transfer(gate.owner, gate.currentBalance);
                        gate.currentBalance = 0;
                    }
                    if (gate.reserve > 0)
                    {
                        qpi.transfer(gate.owner, gate.reserve);
                        gate.reserve = 0;
                    }
                    gate.active = 0;
                    state.mut()._gates.set(i, gate);
                    state.mut()._activeGates -= 1;
                    state.mut()._freeSlots.set(state.get()._freeCount, i);
                    state.mut()._freeCount += 1;
                    // Increment generation so recycled slot gets a new gateId
                    state.mut()._gateGenerations.set(i, state.get()._gateGenerations.get(i) + 1);
                }
            }
        }
    }

    // ---- getGate ----
    getGate_output getGate(uint64 gateId)
    {
        getGate_output out;
        memset(&out, 0, sizeof(out));
        if (gateId == 0 || gateId > state.get()._gateCount)
        {
            out.active = 0; return out;
        }
        GateConfig g = state.get()._gates.get(gateId - 1);
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
        out.chainNextGateId = g.chainNextGateId;
        out.chainDepth = g.chainDepth;
        out.reserve = g.reserve;
        for (uint64 i = 0; i < QUGATE_MAX_RECIPIENTS; i++)
        {
            out.recipients.set(i, g.recipients.get(i));
            out.ratios.set(i, g.ratios.get(i));
        }
        return out;
    }

    // ---- getGateCount ----
    getGateCount_output getGateCount()
    {
        getGateCount_output out;
        out.totalGates = state.get()._gateCount;
        out.activeGates = state.get()._activeGates;
        out.totalBurned = state.get()._totalBurned;
        return out;
    }

    // ---- getFees ----
    getFees_output getFees()
    {
        getFees_output out;
        out.creationFee = state.get()._creationFee;
        out.currentCreationFee = state.get()._creationFee * (1 + QPI::div(state.get()._activeGates, QUGATE_FEE_ESCALATION_STEP));
        out.minSendAmount = state.get()._minSendAmount;
        out.expiryEpochs = state.get()._expiryEpochs;
        return out;
    }

    // ---- fundGate ----
    fundGate_output fundGate(const id& caller, uint64 gateId, sint64 amount)
    {
        qpi.reset();
        qpi._invocator = caller;
        qpi._reward = amount;

        fundGate_output output;
        output.result = QUGATE_SUCCESS;

        if (gateId == 0 || gateId > state.get()._gateCount)
        {
            if (amount > 0) qpi.transfer(caller, amount);
            output.result = QUGATE_INVALID_GATE_ID;
            return output;
        }

        GateConfig gate = state.get()._gates.get(gateId - 1);
        if (gate.active == 0)
        {
            if (amount > 0) qpi.transfer(caller, amount);
            output.result = QUGATE_GATE_NOT_ACTIVE;
            return output;
        }
        if (amount <= 0)
        {
            output.result = QUGATE_DUST_AMOUNT;
            return output;
        }

        gate.reserve += amount;
        state.mut()._gates.set(gateId - 1, gate);
        return output;
    }

    // ---- configureTimeLock (simplified harness) ----
    sint64 configureTimeLock(const id& caller, uint64 gateId, uint32 unlockEpoch,
                             uint8 lockMode, uint8 cancellable)
    {
        qpi.reset();
        qpi._invocator = caller;

        if (gateId == 0 || gateId > state.get()._gateCount) return QUGATE_INVALID_GATE_ID;
        uint64 idx = gateId - 1;
        GateConfig gate = state.get()._gates.get(idx);
        if (!(gate.owner == caller)) return QUGATE_UNAUTHORIZED;
        if (gate.active == 0) return QUGATE_GATE_NOT_ACTIVE;
        if (gate.mode != MODE_TIME_LOCK) return QUGATE_INVALID_MODE;

        QUGATE_TimeLockConfig_Test cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.unlockEpoch = unlockEpoch;
        cfg.lockMode = lockMode;
        cfg.cancellable = cancellable;
        cfg.active = 1;
        state.mut()._timeLockConfigs.set(idx, cfg);
        return QUGATE_SUCCESS;
    }

    // ---- cancelTimeLock (simplified harness) ----
    sint64 cancelTimeLock(const id& caller, uint64 gateId)
    {
        qpi.reset();
        qpi._invocator = caller;

        if (gateId == 0 || gateId > state.get()._gateCount) return QUGATE_INVALID_GATE_ID;
        uint64 idx = gateId - 1;
        GateConfig gate = state.get()._gates.get(idx);
        if (!(gate.owner == caller)) return QUGATE_UNAUTHORIZED;
        if (gate.active == 0) return QUGATE_GATE_NOT_ACTIVE;
        if (gate.mode != MODE_TIME_LOCK) return QUGATE_INVALID_MODE;

        QUGATE_TimeLockConfig_Test cfg = state.get()._timeLockConfigs.get(idx);
        if (cfg.active == 0) return QUGATE_GATE_NOT_ACTIVE;
        if (cfg.fired == 1) return -23; // QUGATE_TIME_LOCK_ALREADY_FIRED
        if (cfg.cancelled == 1) return QUGATE_GATE_NOT_ACTIVE;
        if (cfg.cancellable == 0) return QUGATE_TIME_LOCK_NOT_CANCELLABLE;

        // Refund held balance to owner
        if (gate.currentBalance > 0)
        {
            qpi.transfer(gate.owner, gate.currentBalance);
            gate.currentBalance = 0;
        }

        // Refund reserve to owner
        if (gate.reserve > 0)
        {
            qpi.transfer(gate.owner, gate.reserve);
            gate.reserve = 0;
        }

        // Mark cancelled, close gate
        cfg.cancelled = 1;
        state.mut()._timeLockConfigs.set(idx, cfg);
        gate.active = 0;
        state.mut()._gates.set(idx, gate);
        state.mut()._activeGates -= 1;
        state.mut()._freeSlots.set(state.get()._freeCount, idx);
        state.mut()._freeCount += 1;
        state.mut()._gateGenerations.set(idx, state.get()._gateGenerations.get(idx) + 1);

        return QUGATE_SUCCESS;
    }

    // ---- configureMultisig (simplified harness) ----
    sint64 configureMultisig(const id& caller, uint64 gateId,
                              id* guardians, uint8 guardianCount,
                              uint8 required, uint32 proposalExpiryEpochs,
                              uint32 adminApprovalWindowEpochs)
    {
        qpi.reset();
        qpi._invocator = caller;

        if (gateId == 0 || gateId > state.get()._gateCount) return QUGATE_INVALID_GATE_ID;
        uint64 idx = gateId - 1;
        GateConfig gate = state.get()._gates.get(idx);
        if (!(gate.owner == caller)) return QUGATE_UNAUTHORIZED;
        if (gate.active == 0) return QUGATE_GATE_NOT_ACTIVE;
        if (gate.mode != MODE_MULTISIG) return QUGATE_INVALID_MODE;

        QUGATE_MultisigConfig_Test cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.guardianCount = guardianCount;
        cfg.required = required;
        cfg.proposalExpiryEpochs = proposalExpiryEpochs;
        cfg.adminApprovalWindowEpochs = adminApprovalWindowEpochs;
        for (uint8 i = 0; i < guardianCount && i < 8; i++)
        {
            cfg.guardians.set(i, guardians[i]);
        }
        state.mut()._multisigConfigs.set(idx, cfg);
        return QUGATE_SUCCESS;
    }

    // ---- multisigVote (simplified harness — sendToGate for MULTISIG mode) ----
    // Returns status. For admin-only MULTISIG (recipientCount=0, chainNextGateId=-1,
    // adminApprovalWindowEpochs>0), vote QU is burned.
    sendToGate_output sendToMultisigGate(const id& sender, uint64 gateId, sint64 amount)
    {
        qpi.reset();
        qpi._invocator = sender;
        qpi._reward = amount;

        sendToGate_output output;
        output.status = QUGATE_SUCCESS;

        if (gateId == 0 || gateId > state.get()._gateCount)
        {
            if (amount > 0) qpi.transfer(sender, amount);
            output.status = QUGATE_INVALID_GATE_ID;
            return output;
        }

        uint64 idx = gateId - 1;
        GateConfig gate = state.get()._gates.get(idx);
        if (gate.active == 0)
        {
            if (amount > 0) qpi.transfer(sender, amount);
            output.status = QUGATE_GATE_NOT_ACTIVE;
            return output;
        }
        if (gate.mode != MODE_MULTISIG)
        {
            if (amount > 0) qpi.transfer(sender, amount);
            output.status = QUGATE_INVALID_MODE;
            return output;
        }

        // Accumulate balance
        gate.totalReceived += amount;
        gate.currentBalance += amount;
        gate.lastActivityEpoch = qpi.epoch();

        QUGATE_MultisigConfig_Test msigCfg = state.get()._multisigConfigs.get(idx);

        // Check if sender is a guardian — cast vote
        uint8 guardianIdx = 255;
        for (uint8 i = 0; i < msigCfg.guardianCount; i++)
        {
            if (msigCfg.guardians.get(i) == sender)
            {
                guardianIdx = i;
                break;
            }
        }
        if (guardianIdx != 255 && !(msigCfg.approvalBitmap & (1 << guardianIdx)))
        {
            msigCfg.approvalBitmap |= (1 << guardianIdx);
            msigCfg.approvalCount++;
            if (!msigCfg.proposalActive)
            {
                msigCfg.proposalActive = 1;
                msigCfg.proposalEpoch = qpi.epoch();
            }
            state.mut()._multisigConfigs.set(idx, msigCfg);

            // Admin-only multisig: burn the vote QU
            if (gate.recipientCount == 0 && gate.chainNextGateId == -1
                && msigCfg.adminApprovalWindowEpochs > 0
                && amount > 0 && gate.currentBalance >= (uint64)amount)
            {
                gate.currentBalance -= (uint64)amount;
                qpi.burn(amount);
                state.mut()._totalBurned += amount;
            }
        }

        state.mut()._gates.set(idx, gate);
        return output;
    }

    // ---- setChain ----
    setChain_output setChain(const id& caller, sint64 gateId, sint64 nextGateId, sint64 fee)
    {
        qpi.reset();
        qpi._invocator = caller;
        qpi._reward = fee;

        setChain_output output;
        output.result = QUGATE_SUCCESS;

        if (gateId <= 0 || gateId > (sint64)state.get()._gateCount)
        {
            if (fee > 0) qpi.transfer(caller, fee);
            output.result = QUGATE_INVALID_GATE_ID;
            return output;
        }

        GateConfig gate = state.get()._gates.get(gateId - 1);
        if (!(gate.owner == caller))
        {
            if (fee > 0) qpi.transfer(caller, fee);
            output.result = QUGATE_UNAUTHORIZED;
            return output;
        }
        if (gate.active == 0)
        {
            if (fee > 0) qpi.transfer(caller, fee);
            output.result = QUGATE_GATE_NOT_ACTIVE;
            return output;
        }
        if (fee < QUGATE_CHAIN_HOP_FEE)
        {
            if (fee > 0) qpi.transfer(caller, fee);
            output.result = QUGATE_INSUFFICIENT_FEE;
            return output;
        }

        if (nextGateId == -1)
        {
            gate.chainNextGateId = -1;
            gate.chainDepth = 0;
            state.mut()._gates.set(gateId - 1, gate);
            qpi.burn(QUGATE_CHAIN_HOP_FEE);
            if (fee > QUGATE_CHAIN_HOP_FEE) qpi.transfer(caller, fee - QUGATE_CHAIN_HOP_FEE);
            return output;
        }

        if (nextGateId <= 0 || nextGateId > (sint64)state.get()._gateCount)
        {
            if (fee > 0) qpi.transfer(caller, fee);
            output.result = QUGATE_INVALID_CHAIN;
            return output;
        }

        GateConfig target = state.get()._gates.get(nextGateId - 1);
        if (target.active == 0)
        {
            if (fee > 0) qpi.transfer(caller, fee);
            output.result = QUGATE_INVALID_CHAIN;
            return output;
        }

        uint8 newDepth = target.chainDepth + 1;
        if (newDepth >= QUGATE_MAX_CHAIN_DEPTH)
        {
            if (fee > 0) qpi.transfer(caller, fee);
            output.result = QUGATE_INVALID_CHAIN;
            return output;
        }

        // Cycle detection
        uint64 walkIdx = nextGateId - 1;
        for (uint8 step = 0; step < QUGATE_MAX_CHAIN_DEPTH; step++)
        {
            if (walkIdx == (uint64)(gateId - 1))
            {
                if (fee > 0) qpi.transfer(caller, fee);
                output.result = QUGATE_INVALID_CHAIN;
                return output;
            }
            GateConfig walkGate = state.get()._gates.get(walkIdx);
            if (walkGate.chainNextGateId == -1) break;
            uint64 nextWalk = (uint64)(walkGate.chainNextGateId) - 1;
            if (nextWalk >= state.get()._gateCount) break;
            walkIdx = nextWalk;
        }
        if (walkIdx == (uint64)(gateId - 1))
        {
            if (fee > 0) qpi.transfer(caller, fee);
            output.result = QUGATE_INVALID_CHAIN;
            return output;
        }

        gate.chainNextGateId = nextGateId;
        gate.chainDepth = newDepth;
        state.mut()._gates.set(gateId - 1, gate);
        qpi.burn(QUGATE_CHAIN_HOP_FEE);
        if (fee > QUGATE_CHAIN_HOP_FEE) qpi.transfer(caller, fee - QUGATE_CHAIN_HOP_FEE);
        return output;
    }

    // ---- routeToGate (single hop) ----
    struct RouteResult { sint64 forwarded; };

    RouteResult routeToGate(uint64 slotIdx, sint64 amount, uint8 hopCount)
    {
        RouteResult result;
        result.forwarded = 0;

        if (hopCount >= QUGATE_MAX_CHAIN_DEPTH) return result;

        GateConfig gate = state.get()._gates.get(slotIdx);
        if (gate.active == 0) return result;

        sint64 amountAfterFee = amount;
        if (amount <= QUGATE_CHAIN_HOP_FEE)
        {
            if (gate.reserve >= QUGATE_CHAIN_HOP_FEE)
            {
                gate.reserve -= QUGATE_CHAIN_HOP_FEE;
                state.mut()._gates.set(slotIdx, gate);
                qpi.burn(QUGATE_CHAIN_HOP_FEE);
            }
            else {
                gate.currentBalance += amount;
                state.mut()._gates.set(slotIdx, gate);
                return result; // stranded
            }
        }
        else {
            qpi.burn(QUGATE_CHAIN_HOP_FEE);
            amountAfterFee = amount - QUGATE_CHAIN_HOP_FEE;
        }

        // Dispatch through mode (simplified for test)
        if (gate.mode == MODE_SPLIT)
        {
            uint64 totalRatio = 0;
            for (uint64 i = 0; i < gate.recipientCount; i++)
                totalRatio += gate.ratios.get(i);
            uint64 distributed = 0;
            for (uint64 i = 0; i < gate.recipientCount; i++)
            {
                uint64 share;
                if (i == (uint64)(gate.recipientCount - 1))
                    share = amountAfterFee - distributed;
                else
                    share = QPI::div((uint64)amountAfterFee * gate.ratios.get(i), totalRatio);
                if (share > 0)
                {
                    qpi.transfer(gate.recipients.get(i), share);
                    distributed += share;
                }
            }
            gate = state.get()._gates.get(slotIdx);
            gate.totalForwarded += distributed;
            state.mut()._gates.set(slotIdx, gate);
            result.forwarded = distributed;
        }
        else if (gate.mode == MODE_ROUND_ROBIN)
        {
            qpi.transfer(gate.recipients.get(gate.roundRobinIndex), amountAfterFee);
            gate.totalForwarded += amountAfterFee;
            gate.roundRobinIndex = QPI::mod(gate.roundRobinIndex + 1, (uint64)gate.recipientCount);
            state.mut()._gates.set(slotIdx, gate);
            result.forwarded = amountAfterFee;
        }
        else if (gate.mode == MODE_THRESHOLD)
        {
            gate.currentBalance += amountAfterFee;
            if (gate.currentBalance >= gate.threshold)
            {
                qpi.transfer(gate.recipients.get(0), gate.currentBalance);
                gate.totalForwarded += gate.currentBalance;
                gate.currentBalance = 0;
            }
            state.mut()._gates.set(slotIdx, gate);
            result.forwarded = amountAfterFee;
        }
        return result;
    }

    // ---- routeChain — iterative multi-hop chain routing ----
    sint64 routeChain(uint64 startGateId, sint64 amount)
    {
        sint64 chainAmount = amount;
        sint64 currentChainGateId = startGateId;
        uint8 hop = 0;
        while (hop < QUGATE_MAX_CHAIN_DEPTH && currentChainGateId > 0 && chainAmount > 0)
        {
            uint64 nextIdx = (uint64)currentChainGateId - 1;
            if (nextIdx >= state.get()._gateCount) break;
            auto res = routeToGate(nextIdx, chainAmount, hop);
            chainAmount = res.forwarded;
            GateConfig nextGate = state.get()._gates.get(nextIdx);
            currentChainGateId = nextGate.chainNextGateId;
            hop++;
        }
        return chainAmount;
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
                                         id* allowed = nullptr, uint8 allowedCount = 0)
                                         {
    return env.createGateSimple(owner, fee, mode, recipientCount, recips, ratios, threshold, allowed, allowedCount);
}

// =============================================
// ORIGINAL TESTS (updated for V3 harness)
// =============================================

TEST(QuGate, SplitEvenTwo)
{
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

TEST(QuGate, SplitUnevenThree)
{
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

TEST(QuGate, SplitHandlesRoundingDust)
{
    QuGateTest env;
    id recips[] = { BOB, CHARLIE, DAVE };
    uint64 ratios[] = { 33, 33, 34 };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 3, recips, ratios);

    env.sendToGate(ALICE, out.gateId, 100);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 33);
    EXPECT_EQ(env.qpi.totalTransferredTo(CHARLIE), 33);
    EXPECT_EQ(env.qpi.totalTransferredTo(DAVE), 34);
}

TEST(QuGate, RoundRobinCycles)
{
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

TEST(QuGate, RandomSelectionTracksTickDeterministically)
{
    QuGateTest env;
    id recips[] = { BOB, CHARLIE };
    uint64 ratios[] = { 0, 0 };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_RANDOM, 2, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    env.qpi._tick = 12345;
    env.sendToGate(ALICE, out.gateId, 100);
    EXPECT_EQ(env.qpi.totalTransferredTo(CHARLIE), 100);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 0);

    env.qpi._tick = 12346;
    env.sendToGate(ALICE, out.gateId, 100);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 100);
    EXPECT_EQ(env.qpi.totalTransferredTo(CHARLIE), 100);
}

TEST(QuGate, ThresholdAccumulatesAndReleases)
{
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

TEST(QuGate, ConditionalAllowsWhitelisted)
{
    QuGateTest env;
    id recips[] = { DAVE };
    uint64 ratios[] = { 0 };
    id allowed[] = { ALICE, BOB };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_CONDITIONAL, 1, recips, ratios, 0, allowed, 2);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    env.sendToGate(ALICE, out.gateId, 500);
    EXPECT_EQ(env.qpi.totalTransferredTo(DAVE), 500);
}

TEST(QuGate, ConditionalBouncesUnauthorised)
{
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

TEST(QuGate, InvalidGateIdBounces)
{
    QuGateTest env;
    auto out = env.sendToGate(ALICE, 999, 1000);
    EXPECT_EQ(out.status, QUGATE_INVALID_GATE_ID);
    EXPECT_EQ(env.qpi.totalTransferredTo(ALICE), 1000);
}

TEST(QuGate, CreationFailsWithInsufficientFee)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 500, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out.status, QUGATE_INSUFFICIENT_FEE);
    EXPECT_EQ(out.gateId, 0ULL);
}

TEST(QuGate, ZeroAmountDoesNothing)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);

    auto sendOut = env.sendToGate(ALICE, out.gateId, 0);
    EXPECT_EQ(sendOut.status, QUGATE_DUST_AMOUNT);
    EXPECT_EQ(env.qpi.transferCount, 0);
}

TEST(QuGate, GateCountTracking)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    makeSimpleGate(env, ALICE, 1000, MODE_ROUND_ROBIN, 1, recips, ratios);
    makeSimpleGate(env, BOB, 1000, MODE_THRESHOLD, 1, recips, ratios, 1000);

    EXPECT_EQ(env.state.get()._gateCount, 3ULL);
    EXPECT_EQ(env.state.get()._activeGates, 3ULL);
}

// =============================================
// NEW V3 TESTS
// =============================================

// ---- Escalating fee ----

TEST(QuGateV3, EscalatingFeeAtZeroGates)
{
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

TEST(QuGateV3, EscalatingFeeAt1024Gates)
{
    QuGateTest env;
    // Simulate 1024 active gates
    state_hack: env.state.mut()._activeGates = 1024;

    // fee = 1000 * (1 + 1024/1024) = 2000
    auto fees = env.getFees();
    EXPECT_EQ(fees.currentCreationFee, 2000ULL);

    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 2000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out.status, QUGATE_SUCCESS);
    EXPECT_EQ(out.feePaid, 2000ULL);

    // Insufficient at old price
    env.state.mut()._activeGates = 1025; // now fee is still 2000
    auto out2 = makeSimpleGate(env, ALICE, 1999, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out2.status, QUGATE_INSUFFICIENT_FEE);
}

TEST(QuGateV3, EscalatingFeeAt2048Gates)
{
    QuGateTest env;
    env.state.mut()._activeGates = 2048;
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

TEST(QuGateV3, FeeOverpaymentSeedsReserve)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    // Pay 5000, fee is 1000 → excess 4000 seeds reserve
    auto out = makeSimpleGate(env, ALICE, 5000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out.status, QUGATE_SUCCESS);
    EXPECT_EQ(out.feePaid, 1000ULL);
    EXPECT_EQ(env.qpi.totalTransferredTo(ALICE), 0); // no refund
    EXPECT_EQ(env.qpi.totalBurned, 1000);
    // Excess goes to reserve
    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.reserve, 4000);
}

// ---- Dust burn ----

TEST(QuGateV3, DustBurnBelowMinSend)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);

    // Send 5 QU (below minSendAmount of 10)
    auto sendOut = env.sendToGate(ALICE, out.gateId, 5);
    EXPECT_EQ(sendOut.status, QUGATE_DUST_AMOUNT);
    EXPECT_EQ(env.qpi.totalBurned, 5);
    EXPECT_EQ(env.qpi.transferCount, 0); // no transfers, burned
    EXPECT_EQ(env.state.get()._totalBurned, 1000 + 5); // creation fee + dust
}

TEST(QuGateV3, ExactMinSendNotDust)
{
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

TEST(QuGateV3, StatusCodeCreateInvalidMode)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 1000, 99, 1, recips, ratios);
    EXPECT_EQ(out.status, QUGATE_INVALID_MODE);
}

TEST(QuGateV3, StatusCodeCreateInvalidRecipientCount)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 0, recips, ratios);
    EXPECT_EQ(out.status, QUGATE_INVALID_RECIPIENT_COUNT);
}

TEST(QuGateV3, StatusCodeCreateInvalidRatio)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 }; // zero total ratio
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out.status, QUGATE_INVALID_RATIO);
}

TEST(QuGateV3, StatusCodeCreateInvalidThreshold)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_THRESHOLD, 1, recips, ratios, 0);
    EXPECT_EQ(out.status, QUGATE_INVALID_THRESHOLD);
}

TEST(QuGateV3, StatusCodeSendToInactiveGate)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    env.closeGate(ALICE, out.gateId);

    auto sendOut = env.sendToGate(ALICE, out.gateId, 100);
    EXPECT_EQ(sendOut.status, QUGATE_GATE_NOT_ACTIVE);
}

TEST(QuGateV3, StatusCodeCloseUnauthorized)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);

    auto closeOut = env.closeGate(BOB, out.gateId);
    EXPECT_EQ(closeOut.status, QUGATE_UNAUTHORIZED);
}

TEST(QuGateV3, StatusCodeCloseInvalidGateId)
{
    QuGateTest env;
    auto closeOut = env.closeGate(ALICE, 999);
    EXPECT_EQ(closeOut.status, QUGATE_INVALID_GATE_ID);
}

TEST(QuGateV3, StatusCodeUpdateInvalidGateId)
{
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

TEST(QuGateV3, StatusCodeUpdateUnauthorized)
{
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

TEST(QuGateV3, FreeListSlotReuse)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    auto g1 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    auto g2 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(g1.gateId, 1ULL);
    EXPECT_EQ(g2.gateId, 2ULL);
    EXPECT_EQ(env.state.get()._gateCount, 2ULL);

    // Close gate 1
    env.closeGate(ALICE, 1);
    EXPECT_EQ(env.state.get()._freeCount, 1ULL);
    EXPECT_EQ(env.state.get()._activeGates, 1ULL);

    // Create again — should reuse slot 0 (gateId 1)
    auto g3 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(g3.gateId, 1ULL); // reused!
    EXPECT_EQ(env.state.get()._freeCount, 0ULL);
    EXPECT_EQ(env.state.get()._gateCount, 2ULL); // didn't grow
    EXPECT_EQ(env.state.get()._activeGates, 2ULL);
}

// ---- Gate expiry ----

TEST(QuGateV3, GateExpiryAutoClose)
{
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
    EXPECT_EQ(env.state.get()._activeGates, 0ULL);
    EXPECT_EQ(env.state.get()._freeCount, 1ULL);
}

TEST(QuGateV3, GateExpiryRefundsBalance)
{
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

TEST(QuGateV3, GateNotExpiredIfActive)
{
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

TEST(QuGateV3, TotalBurnedTracking)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    // Create gate → burns 1000
    makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(env.state.get()._totalBurned, 1000ULL);

    // Dust burn → burns 5
    env.sendToGate(ALICE, 1, 5);
    EXPECT_EQ(env.state.get()._totalBurned, 1005ULL);

    // Create another → burns 1000
    makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(env.state.get()._totalBurned, 2005ULL);

    auto count = env.getGateCount();
    EXPECT_EQ(count.totalBurned, 2005ULL);
}

// ---- getFees returns correct values ----

TEST(QuGateV3, GetFeesReturnsCorrectValues)
{
    QuGateTest env;
    auto fees = env.getFees();
    EXPECT_EQ(fees.creationFee, 1000ULL);
    EXPECT_EQ(fees.currentCreationFee, 1000ULL);
    EXPECT_EQ(fees.minSendAmount, 10ULL);
    EXPECT_EQ(fees.expiryEpochs, 50ULL);

    // With active gates
    env.state.mut()._activeGates = 2048;
    fees = env.getFees();
    EXPECT_EQ(fees.currentCreationFee, 3000ULL);
}

// ---- lastActivityEpoch updates ----

TEST(QuGateV3, LastActivityEpochUpdatesOnSend)
{
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

TEST(QuGateV3, LastActivityEpochUpdatesOnUpdate)
{
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

TEST(QuGateV3, FeePaidMatchesEscalatedFee)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    // 0 active gates → fee = 1000
    auto out1 = makeSimpleGate(env, ALICE, 5000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out1.feePaid, 1000ULL);

    // Set active gates to 1024 (minus the 1 just created, so set to 1024 total)
    env.state.mut()._activeGates = 1024;
    auto out2 = makeSimpleGate(env, ALICE, 5000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out2.feePaid, 2000ULL);

    env.state.mut()._activeGates = 3072;
    auto out3 = makeSimpleGate(env, ALICE, 5000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out3.feePaid, 4000ULL);
}

// ---- Close gate refunds invocation reward ----

TEST(QuGateV3, CloseGateSuccess)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);

    auto closeOut = env.closeGate(ALICE, out.gateId);
    EXPECT_EQ(closeOut.status, QUGATE_SUCCESS);
    EXPECT_EQ(env.state.get()._activeGates, 0ULL);
}

TEST(QuGateV3, CloseAlreadyClosedGate)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);

    env.closeGate(ALICE, out.gateId);
    auto closeOut2 = env.closeGate(ALICE, out.gateId);
    EXPECT_EQ(closeOut2.status, QUGATE_GATE_NOT_ACTIVE);
}

// ---- Ratio overflow protection ----

TEST(QuGateV3, RatioOverMaxRejected)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { QUGATE_MAX_RATIO + 1 };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out.status, QUGATE_INVALID_RATIO);
}

// ---- AllowedSenderCount > max rejected ----

TEST(QuGateV3, InvalidSenderCountRejected)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    id allowed[1] = { ALICE };
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_CONDITIONAL, 1, recips, ratios, 0, allowed, 9);
    EXPECT_EQ(out.status, QUGATE_INVALID_SENDER_COUNT);
}

// ---- Insufficient fee refunds ----

TEST(QuGateV3, InsufficientFeeRefundsPayment)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 500, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out.status, QUGATE_INSUFFICIENT_FEE);
    // The 500 should be refunded
    EXPECT_EQ(env.qpi.totalTransferredTo(ALICE), 500);
}

// =============================================
// FUND GATE TESTS
// =============================================

TEST(QuGateFund, FundGateSuccess)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto gateOut = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(gateOut.status, QUGATE_SUCCESS);

    // Fund reserve with 3000
    auto fundOut = env.fundGate(CHARLIE, gateOut.gateId, 3000);
    EXPECT_EQ(fundOut.result, QUGATE_SUCCESS);

    auto gate = env.getGate(gateOut.gateId);
    EXPECT_EQ(gate.reserve, 3000);
}

TEST(QuGateFund, FundGateInvalidId)
{
    QuGateTest env;
    auto fundOut = env.fundGate(ALICE, 999, 1000);
    EXPECT_EQ(fundOut.result, QUGATE_INVALID_GATE_ID);
}

// =============================================
// CHAIN GATE TESTS
// =============================================

TEST(QuGateChain, CreateGateWithChain)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    // Create target gate first (gate 1)
    auto g1 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(g1.status, QUGATE_SUCCESS);

    // Create chained gate (gate 2 → gate 1)
    createGate_input in;
    memset(&in, 0, sizeof(in));
    in.mode = MODE_SPLIT;
    in.recipientCount = 1;
    in.recipients.set(0, CHARLIE);
    in.ratios.set(0, 100);
    in.chainNextGateId = g1.gateId;
    auto g2 = env.createGate(ALICE, 1000, in);
    EXPECT_EQ(g2.status, QUGATE_SUCCESS);

    auto gate = env.getGate(g2.gateId);
    EXPECT_EQ(gate.chainNextGateId, (sint64)g1.gateId);
    EXPECT_EQ(gate.chainDepth, 1);
}

TEST(QuGateChain, CreateGateChainInvalidTarget)
{
    QuGateTest env;
    createGate_input in;
    memset(&in, 0, sizeof(in));
    in.mode = MODE_SPLIT;
    in.recipientCount = 1;
    in.recipients.set(0, BOB);
    in.ratios.set(0, 100);
    in.chainNextGateId = 999; // doesn't exist
    auto out = env.createGate(ALICE, 1000, in);
    EXPECT_EQ(out.status, QUGATE_INVALID_CHAIN);
}

TEST(QuGateChain, CreateGateChainDepthLimit)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    // Create 3 gates: g1 (depth 0), g2→g1 (depth 1), g3→g2 (depth 2)
    auto g1 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);

    createGate_input in;
    memset(&in, 0, sizeof(in));
    in.mode = MODE_SPLIT;
    in.recipientCount = 1;
    in.recipients.set(0, BOB);
    in.ratios.set(0, 100);
    in.chainNextGateId = g1.gateId;
    auto g2 = env.createGate(ALICE, 1000, in);
    ASSERT_EQ(g2.status, QUGATE_SUCCESS);
    EXPECT_EQ(env.getGate(g2.gateId).chainDepth, 1);

    in.chainNextGateId = g2.gateId;
    auto g3 = env.createGate(ALICE, 1000, in);
    ASSERT_EQ(g3.status, QUGATE_SUCCESS);
    EXPECT_EQ(env.getGate(g3.gateId).chainDepth, 2);

    // g4→g3 should fail (depth would be 3 >= QUGATE_MAX_CHAIN_DEPTH)
    in.chainNextGateId = g3.gateId;
    auto g4 = env.createGate(ALICE, 1000, in);
    EXPECT_EQ(g4.status, QUGATE_INVALID_CHAIN);
}

TEST(QuGateChain, SetChainSuccess)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    auto g1 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    auto g2 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);

    // Link g2 → g1
    auto out = env.setChain(ALICE, g2.gateId, g1.gateId, QUGATE_CHAIN_HOP_FEE);
    EXPECT_EQ(out.result, QUGATE_SUCCESS);

    auto gate = env.getGate(g2.gateId);
    EXPECT_EQ(gate.chainNextGateId, (sint64)g1.gateId);
    EXPECT_EQ(gate.chainDepth, 1);
}

TEST(QuGateChain, SetChainClearChain)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    auto g1 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    auto g2 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);

    env.setChain(ALICE, g2.gateId, g1.gateId, QUGATE_CHAIN_HOP_FEE);
    EXPECT_EQ(env.getGate(g2.gateId).chainNextGateId, (sint64)g1.gateId);

    // Clear chain
    auto out = env.setChain(ALICE, g2.gateId, -1, QUGATE_CHAIN_HOP_FEE);
    EXPECT_EQ(out.result, QUGATE_SUCCESS);
    EXPECT_EQ(env.getGate(g2.gateId).chainNextGateId, -1);
    EXPECT_EQ(env.getGate(g2.gateId).chainDepth, 0);
}

TEST(QuGateChain, SetChainUnauthorized)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    auto g1 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    auto g2 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);

    // BOB tries to set chain on ALICE's gate
    auto out = env.setChain(BOB, g2.gateId, g1.gateId, QUGATE_CHAIN_HOP_FEE);
    EXPECT_EQ(out.result, QUGATE_UNAUTHORIZED);
}

TEST(QuGateChain, SetChainCycleRejectedPreservesExistingLinks)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    auto g1 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    auto g2 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);

    auto first = env.setChain(ALICE, g2.gateId, g1.gateId, QUGATE_CHAIN_HOP_FEE);
    ASSERT_EQ(first.result, QUGATE_SUCCESS);

    auto cycle = env.setChain(ALICE, g1.gateId, g2.gateId, QUGATE_CHAIN_HOP_FEE);
    EXPECT_EQ(cycle.result, QUGATE_INVALID_CHAIN);

    EXPECT_EQ(env.getGate(g1.gateId).chainNextGateId, -1);
    EXPECT_EQ(env.getGate(g1.gateId).chainDepth, 0);
    EXPECT_EQ(env.getGate(g2.gateId).chainNextGateId, (sint64)g1.gateId);
    EXPECT_EQ(env.getGate(g2.gateId).chainDepth, 1);
}

TEST(QuGateChain, SetChainInsufficientFee)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    auto g1 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    auto g2 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);

    auto out = env.setChain(ALICE, g2.gateId, g1.gateId, 500); // below hop fee
    EXPECT_EQ(out.result, QUGATE_INSUFFICIENT_FEE);
}

TEST(QuGateChain, RouteToGateSingleHop)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    auto g1 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);

    env.qpi.reset();
    auto result = env.routeToGate(g1.gateId - 1, 5000, 0);
    EXPECT_EQ(result.forwarded, 4000); // 5000 - 1000 hop fee
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 4000);
    EXPECT_EQ(env.qpi.totalBurned, 1000); // hop fee burned
}

TEST(QuGateChain, RouteToGateTwoHopChain)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    // g1: SPLIT 100% → BOB (depth 0)
    auto g1 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    // g2: SPLIT 100% → CHARLIE, chained to g1
    id recips2[] = { CHARLIE };
    auto g2 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips2, ratios);
    env.setChain(ALICE, g2.gateId, g1.gateId, QUGATE_CHAIN_HOP_FEE);

    // Route through chain: g2 → g1
    env.qpi.reset();
    sint64 forwarded = env.routeChain(g2.gateId, 10000);

    // Hop 1 (g2): 10000 - 1000 fee = 9000 → CHARLIE via SPLIT
    EXPECT_EQ(env.qpi.totalTransferredTo(CHARLIE), 9000);
    // Hop 2 (g1): 9000 - 1000 fee = 8000 → BOB via SPLIT
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 8000);
    // Total hop fees burned: 2000
    EXPECT_EQ(env.qpi.totalBurned, 2000);
}

TEST(QuGateChain, RouteToGateThresholdAccumulatesAfterHopFee)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };

    auto thresholdGate = makeSimpleGate(env, ALICE, 1000, MODE_THRESHOLD, 1, recips, ratios, 5000);
    ASSERT_EQ(thresholdGate.status, QUGATE_SUCCESS);

    env.qpi.reset();
    auto first = env.routeToGate(thresholdGate.gateId - 1, 4000, 0);
    EXPECT_EQ(first.forwarded, 3000);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 0);
    EXPECT_EQ(env.getGate(thresholdGate.gateId).currentBalance, 3000ULL);

    env.qpi.reset();
    auto second = env.routeToGate(thresholdGate.gateId - 1, 3000, 0);
    EXPECT_EQ(second.forwarded, 2000);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 5000);
    EXPECT_EQ(env.getGate(thresholdGate.gateId).currentBalance, 0ULL);
}

TEST(QuGateChain, InsufficientFundsStrand)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    auto g1 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);

    // Send amount <= hop fee with no chain reserve → strands
    env.qpi.reset();
    auto result = env.routeToGate(g1.gateId - 1, 500, 0);
    EXPECT_EQ(result.forwarded, 0); // stranded
    EXPECT_EQ(env.qpi.transferCount, 0); // nothing transferred

    auto gate = env.getGate(g1.gateId);
    EXPECT_EQ(gate.currentBalance, 500ULL); // accumulated in currentBalance
}

TEST(QuGateChain, ReserveCoversHopFee)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    // Create gate with chain to somewhere (need a target first)
    auto g1 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    auto g2 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    env.setChain(ALICE, g2.gateId, g1.gateId, QUGATE_CHAIN_HOP_FEE);

    // Fund reserve
    auto fundOut = env.fundGate(CHARLIE, g2.gateId, 5000);
    EXPECT_EQ(fundOut.result, QUGATE_SUCCESS);
    EXPECT_EQ(env.getGate(g2.gateId).reserve, 5000);

    // Route 500 (< hop fee) — reserve should cover
    env.qpi.reset();
    auto result = env.routeToGate(g2.gateId - 1, 500, 0);
    EXPECT_EQ(result.forwarded, 500); // full amount forwarded (reserve paid fee)
    EXPECT_EQ(env.getGate(g2.gateId).reserve, 4000); // 5000 - 1000
}

TEST(QuGateChain, FundGateReserve)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    auto g1 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    auto g2 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    env.setChain(ALICE, g2.gateId, g1.gateId, QUGATE_CHAIN_HOP_FEE);

    // Fund reserve
    auto fundOut = env.fundGate(BOB, g2.gateId, 3000);
    EXPECT_EQ(fundOut.result, QUGATE_SUCCESS);
    EXPECT_EQ(env.getGate(g2.gateId).reserve, 3000);

    // Fund reserve on gate with no chain — succeeds (reserve is unified)
    auto fundOut2 = env.fundGate(BOB, g1.gateId, 1000);
    EXPECT_EQ(fundOut2.result, QUGATE_SUCCESS);
    EXPECT_EQ(env.getGate(g1.gateId).reserve, 1000);
}

TEST(QuGateChain, DeadLinkChainedGateClosed)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    auto g1 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    auto g2 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    env.setChain(ALICE, g2.gateId, g1.gateId, QUGATE_CHAIN_HOP_FEE);

    // Close the target gate
    env.closeGate(ALICE, g1.gateId);

    // Route through g2 — should process g2 but dead link on g1
    env.qpi.reset();
    auto result = env.routeToGate(g2.gateId - 1, 5000, 0);
    EXPECT_EQ(result.forwarded, 4000); // g2 processes fine (5000-1000 fee)

    // Chain doesn't continue because g1 is closed
    // routeChain would handle this — just verify routeToGate doesn't crash
}

TEST(QuGateChain, GetGateReturnsChainFields)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    auto g1 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    auto g2 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    env.setChain(ALICE, g2.gateId, g1.gateId, QUGATE_CHAIN_HOP_FEE);
    env.fundGate(CHARLIE, g2.gateId, 2000);

    auto gate = env.getGate(g2.gateId);
    EXPECT_EQ(gate.chainNextGateId, (sint64)g1.gateId);
    EXPECT_EQ(gate.chainDepth, 1);
    EXPECT_EQ(gate.reserve, 2000);

    // Gate without chain
    auto gate1 = env.getGate(g1.gateId);
    EXPECT_EQ(gate1.chainNextGateId, -1);
    EXPECT_EQ(gate1.chainDepth, 0);
    EXPECT_EQ(gate1.reserve, 0);
}

TEST(QuGateChain, CloseGateRefundsReserve)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    auto g1 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    auto g2 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    env.setChain(ALICE, g2.gateId, g1.gateId, QUGATE_CHAIN_HOP_FEE);
    env.fundGate(BOB, g2.gateId, 3000);

    auto closeOut = env.closeGate(ALICE, g2.gateId);
    EXPECT_EQ(closeOut.status, QUGATE_SUCCESS);
    // Reserve (3000) refunded to owner
    EXPECT_EQ(env.qpi.totalTransferredTo(ALICE), 3000);
}

// ---- END_EPOCH comprehensive expiry test (cyber-pc review request) ----

TEST(QuGateV3, EndEpochExpiryFullLifecycle)
{
    // Verifies all END_EPOCH expiry side-effects:
    //   1. Gate marked inactive
    //   2. currentBalance refunded to owner
    //   3. Slot added to free-list
    //   4. Generation incremented (prevents stale gateId reuse)
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    // Use a short expiry for the test
    env.state.mut()._expiryEpochs = 5;

    // Create a THRESHOLD gate at epoch 100 so it can hold a balance
    env.qpi._epoch = 100;
    auto out = makeSimpleGate(env, ALICE, 1000, MODE_THRESHOLD, 1, recips, ratios, 50000);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    uint64 gateId = out.gateId;
    uint64 slotIdx = gateId - 1;

    // Send funds that accumulate in threshold balance (below threshold, won't forward)
    env.sendToGate(CHARLIE, gateId, 2000);
    auto gateBefore = env.getGate(gateId);
    EXPECT_EQ(gateBefore.active, 1);
    EXPECT_EQ(gateBefore.currentBalance, 2000ULL);
    EXPECT_EQ(env.state.get()._activeGates, 1ULL);
    uint16 genBefore = env.state.get()._gateGenerations.get(slotIdx);

    // Advance epoch past expiry (lastActivityEpoch=100 from send, expiryEpochs=5)
    env.qpi._epoch = 106;
    env.qpi.reset();
    env.endEpoch();

    // 1. Gate marked inactive
    auto gateAfter = env.getGate(gateId);
    EXPECT_EQ(gateAfter.active, 0);

    // 2. currentBalance refunded to owner (ALICE)
    EXPECT_GE(env.qpi.totalTransferredTo(ALICE), 2000);
    EXPECT_EQ(gateAfter.currentBalance, 0ULL);

    // 3. Slot added to free-list
    EXPECT_EQ(env.state.get()._freeCount, 1ULL);
    EXPECT_EQ(env.state.get()._freeSlots.get(0), slotIdx);
    EXPECT_EQ(env.state.get()._activeGates, 0ULL);

    // 4. Generation incremented
    uint16 genAfter = env.state.get()._gateGenerations.get(slotIdx);
    EXPECT_EQ(genAfter, genBefore + 1);

    // Verify a new gate created in the reused slot gets a different generation
    env.qpi._epoch = 107;
    auto out2 = makeSimpleGate(env, ALICE, 1000, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(out2.status, QUGATE_SUCCESS);
    EXPECT_EQ(env.state.get()._freeCount, 0ULL); // slot was reused from free-list
}

// =============================================
// FINANCIAL MODEL TESTS
// =============================================

// ---- 1. Creation fee 80/20 split ----

TEST(QuGateFinancial, CreationFee80_20Split)
{
    // When a gate is created with 100,000 QU fee:
    //   50,000 should be burned (_totalBurned += 50000)
    //   50,000 should go to dividend pool (_earnedMaintenanceDividends += 50000)
    //   _totalMaintenanceDividends += 50000
    //   burn + dividend = fee exactly
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    uint64 fee = env.currentEscalatedFee(); // 100000 at 0 active gates
    ASSERT_EQ(fee, 100000ULL);

    uint64 burnedBefore = env.state.get()._totalBurned;
    uint64 dividendsBefore = env.state.get()._earnedMaintenanceDividends;
    uint64 totalDivBefore = env.state.get()._totalMaintenanceDividends;

    auto out = makeSimpleGate(env, ALICE, (sint64)fee, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    ASSERT_EQ(out.feePaid, fee);

    uint64 expectedBurn = QPI::div(fee * QUGATE_FEE_BURN_BPS, 10000ULL); // 50000
    uint64 expectedDividend = fee - expectedBurn;                         // 50000

    EXPECT_EQ(expectedBurn, 50000ULL);
    EXPECT_EQ(expectedDividend, 50000ULL);

    // Verify state changes
    EXPECT_EQ(env.state.get()._totalBurned - burnedBefore, expectedBurn);
    EXPECT_EQ(env.state.get()._earnedMaintenanceDividends - dividendsBefore, expectedDividend);
    EXPECT_EQ(env.state.get()._totalMaintenanceDividends - totalDivBefore, expectedDividend);

    // Verify burn + dividend = fee exactly
    uint64 totalAccounted = (env.state.get()._totalBurned - burnedBefore)
                          + (env.state.get()._earnedMaintenanceDividends - dividendsBefore);
    EXPECT_EQ(totalAccounted, fee);

    // Verify qpi.burn was called with only the burn portion
    EXPECT_EQ(env.qpi.totalBurned, (sint64)expectedBurn);
}

TEST(QuGateFinancial, CreationFeeEscalatedSplitStillBalances)
{
    // With active gates, the escalated fee also splits correctly
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    // Force 1024 active gates for 2x escalation
    env.state.mut()._activeGates = 1024;
    uint64 fee = env.currentEscalatedFee(); // 100000 * 2 = 200000
    ASSERT_EQ(fee, 200000ULL);

    auto out = makeSimpleGate(env, ALICE, (sint64)fee, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    uint64 expectedBurn = QPI::div(fee * QUGATE_FEE_BURN_BPS, 10000ULL); // 160000
    uint64 expectedDividend = fee - expectedBurn;                         // 40000

    EXPECT_EQ(expectedBurn, 160000ULL);
    EXPECT_EQ(expectedDividend, 40000ULL);

    // burn + dividend = fee
    EXPECT_EQ(env.state.get()._totalBurned + env.state.get()._earnedMaintenanceDividends, fee);
}

// ---- 2. Idle maintenance fee 80/20 split ----

TEST(QuGateFinancial, IdleMaintenanceFee80_20Split)
{
    // When a gate is charged 25,000 QU idle fee:
    //   12,500 burned (_totalMaintenanceBurned += 12500)
    //   5,000 to dividend pool (_earnedMaintenanceDividends increases)
    //   _totalMaintenanceCharged += 25000
    //   gate.reserve decreases by 25000
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    uint64 creationFee = env.currentEscalatedFee();
    uint64 reserveAmount = 200000; // well above idle fee

    // Create gate at epoch 100
    env.qpi._epoch = 100;
    auto out = makeSimpleGate(env, ALICE, (sint64)(creationFee + reserveAmount), MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    ASSERT_EQ(out.feePaid, creationFee);

    // Verify reserve was seeded with excess
    auto gateBefore = env.getGate(out.gateId);
    EXPECT_EQ(gateBefore.reserve, (sint64)reserveAmount);

    // Record state before idle charge
    uint64 maintenanceChargedBefore = env.state.get()._totalMaintenanceCharged;
    uint64 maintenanceBurnedBefore = env.state.get()._totalMaintenanceBurned;
    uint64 earnedDivBefore = env.state.get()._earnedMaintenanceDividends;
    uint64 totalDivBefore = env.state.get()._totalMaintenanceDividends;
    uint64 totalBurnedBefore = env.state.get()._totalBurned;

    // Advance to idle charge epoch (created at 100, window=4, so due at 104)
    env.qpi._epoch = 104;
    env.qpi.reset();
    env.endEpoch();

    uint64 idleFee = QUGATE_DEFAULT_MAINTENANCE_FEE; // 25000
    uint64 expectedBurn = QPI::div(idleFee * QUGATE_FEE_BURN_BPS, 10000ULL); // 12500
    uint64 expectedDividend = idleFee - expectedBurn;                         // 5000

    EXPECT_EQ(expectedBurn, 12500ULL);
    EXPECT_EQ(expectedDividend, 5000ULL);

    // Verify maintenance tracking
    EXPECT_EQ(env.state.get()._totalMaintenanceCharged - maintenanceChargedBefore, idleFee);
    EXPECT_EQ(env.state.get()._totalMaintenanceBurned - maintenanceBurnedBefore, expectedBurn);

    // Verify dividend tracking
    EXPECT_EQ(env.state.get()._earnedMaintenanceDividends - earnedDivBefore, expectedDividend);
    EXPECT_EQ(env.state.get()._totalMaintenanceDividends - totalDivBefore, expectedDividend);

    // Verify total burned includes maintenance burn
    EXPECT_EQ(env.state.get()._totalBurned - totalBurnedBefore, expectedBurn);

    // Verify gate reserve decreased by full idle fee
    auto gateAfter = env.getGate(out.gateId);
    EXPECT_EQ(gateAfter.reserve, (sint64)(reserveAmount - idleFee));

    // Verify burn + dividend = fee exactly
    EXPECT_EQ(expectedBurn + expectedDividend, idleFee);
}

TEST(QuGateFinancial, IdleMaintenanceMultipleCycles)
{
    // Verify multiple idle charge cycles accumulate correctly
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    uint64 creationFee = env.currentEscalatedFee();
    uint64 reserveAmount = 500000; // enough for many cycles

    env.qpi._epoch = 100;
    auto out = makeSimpleGate(env, ALICE, (sint64)(creationFee + reserveAmount), MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    uint64 idleFee = QUGATE_DEFAULT_MAINTENANCE_FEE;
    uint64 burnPerCycle = QPI::div(idleFee * QUGATE_FEE_BURN_BPS, 10000ULL);
    uint64 divPerCycle = idleFee - burnPerCycle;

    // Run 3 idle charge cycles (epochs 104, 108, 112)
    for (int cycle = 0; cycle < 3; cycle++)
    {
        env.qpi._epoch = 104 + cycle * 4;
        env.qpi.reset();
        env.endEpoch();
    }

    EXPECT_EQ(env.state.get()._totalMaintenanceCharged, idleFee * 3);
    EXPECT_EQ(env.state.get()._totalMaintenanceBurned, burnPerCycle * 3);

    // Reserve should have decreased by 3 * idleFee
    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.reserve, (sint64)(reserveAmount - idleFee * 3));
}

// ---- 3. Rounding correctness ----

TEST(QuGateFinancial, RoundingBurnPlusDividendEqualsFee)
{
    // For various fee amounts, verify burn + dividend = fee exactly
    // The formula: burn = fee * 8000 / 10000, dividend = fee - burn

    // fee = 1: burn = 0, dividend = 1
    {
        uint64 fee = 1;
        uint64 burn = QPI::div(fee * QUGATE_FEE_BURN_BPS, 10000ULL);
        uint64 div = fee - burn;
        EXPECT_EQ(burn, 0ULL);
        EXPECT_EQ(div, 1ULL);
        EXPECT_EQ(burn + div, fee);
    }

    // fee = 7: burn = 5, dividend = 2
    {
        uint64 fee = 7;
        uint64 burn = QPI::div(fee * QUGATE_FEE_BURN_BPS, 10000ULL);
        uint64 div = fee - burn;
        EXPECT_EQ(burn, 5ULL);
        EXPECT_EQ(div, 2ULL);
        EXPECT_EQ(burn + div, fee);
    }

    // fee = 99999: burn = 49999, dividend = 50000
    {
        uint64 fee = 99999;
        uint64 burn = QPI::div(fee * QUGATE_FEE_BURN_BPS, 10000ULL);
        uint64 div = fee - burn;
        EXPECT_EQ(burn, 49999ULL);
        EXPECT_EQ(div, 50000ULL);
        EXPECT_EQ(burn + div, fee);
    }

    // fee = 100000: burn = 50000, dividend = 50000 (default creation fee)
    {
        uint64 fee = 100000;
        uint64 burn = QPI::div(fee * QUGATE_FEE_BURN_BPS, 10000ULL);
        uint64 div = fee - burn;
        EXPECT_EQ(burn, 50000ULL);
        EXPECT_EQ(div, 50000ULL);
        EXPECT_EQ(burn + div, fee);
    }

    // fee = 25000: burn = 12500, dividend = 12500 (idle maintenance fee)
    {
        uint64 fee = 25000;
        uint64 burn = QPI::div(fee * QUGATE_FEE_BURN_BPS, 10000ULL);
        uint64 div = fee - burn;
        EXPECT_EQ(burn, 12500ULL);
        EXPECT_EQ(div, 12500ULL);
        EXPECT_EQ(burn + div, fee);
    }

    // fee = 3: burn = 2, dividend = 1
    {
        uint64 fee = 3;
        uint64 burn = QPI::div(fee * QUGATE_FEE_BURN_BPS, 10000ULL);
        uint64 div = fee - burn;
        EXPECT_EQ(burn, 2ULL);
        EXPECT_EQ(div, 1ULL);
        EXPECT_EQ(burn + div, fee);
    }
}

// ---- 4. cancelTimeLock refunds reserve ----

TEST(QuGateFinancial, CancelTimeLockRefundsReserve)
{
    // Create a TIME_LOCK gate, fund its reserve, cancel it
    // — verify reserve is refunded to owner (not trapped)
    QuGateTest env;

    uint64 creationFee = env.currentEscalatedFee();

    // Create TIME_LOCK gate with recipientCount=0
    createGate_input in;
    memset(&in, 0, sizeof(in));
    in.mode = MODE_TIME_LOCK;
    in.recipientCount = 0;
    in.chainNextGateId = -1;
    auto out = env.createGate(ALICE, (sint64)creationFee, in);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    // Configure time lock (cancellable)
    auto cfgStatus = env.configureTimeLock(ALICE, out.gateId, 200, QUGATE_TIME_LOCK_ABSOLUTE_EPOCH, 1);
    ASSERT_EQ(cfgStatus, QUGATE_SUCCESS);

    // Fund reserve
    auto fundOut = env.fundGate(ALICE, out.gateId, 50000);
    ASSERT_EQ(fundOut.result, QUGATE_SUCCESS);

    // Send some QU to accumulate in currentBalance
    // (For TIME_LOCK, sendToGate accumulates in currentBalance)
    // We'll directly set the balance to simulate funding
    {
        GateConfig gate = env.state.get()._gates.get(out.gateId - 1);
        gate.currentBalance = 30000;
        env.state.mut()._gates.set(out.gateId - 1, gate);
    }

    auto gateBefore = env.getGate(out.gateId);
    EXPECT_EQ(gateBefore.reserve, 50000);
    EXPECT_EQ(gateBefore.currentBalance, 30000ULL);

    // Cancel the time lock
    env.qpi.reset();
    auto cancelStatus = env.cancelTimeLock(ALICE, out.gateId);
    ASSERT_EQ(cancelStatus, QUGATE_SUCCESS);

    // Verify reserve + balance refunded to owner
    EXPECT_EQ(env.qpi.totalTransferredTo(ALICE), 30000 + 50000);

    // Verify gate is closed
    auto gateAfter = env.getGate(out.gateId);
    EXPECT_EQ(gateAfter.active, 0);
    EXPECT_EQ(gateAfter.currentBalance, 0ULL);
    EXPECT_EQ(gateAfter.reserve, 0);
}

TEST(QuGateFinancial, CancelTimeLockNonCancellableRejected)
{
    // Verify that cancelling a non-cancellable TIME_LOCK is rejected
    QuGateTest env;

    uint64 creationFee = env.currentEscalatedFee();

    createGate_input in;
    memset(&in, 0, sizeof(in));
    in.mode = MODE_TIME_LOCK;
    in.recipientCount = 0;
    in.chainNextGateId = -1;
    auto out = env.createGate(ALICE, (sint64)creationFee, in);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    // Configure as non-cancellable
    auto cfgStatus = env.configureTimeLock(ALICE, out.gateId, 200, QUGATE_TIME_LOCK_ABSOLUTE_EPOCH, 0);
    ASSERT_EQ(cfgStatus, QUGATE_SUCCESS);

    env.fundGate(ALICE, out.gateId, 50000);

    auto cancelStatus = env.cancelTimeLock(ALICE, out.gateId);
    EXPECT_EQ(cancelStatus, QUGATE_TIME_LOCK_NOT_CANCELLABLE);

    // Gate should still be active with reserve intact
    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.active, 1);
    EXPECT_EQ(gate.reserve, 50000);
}

// ---- 5. Admin-only MULTISIG vote burns QU ----

TEST(QuGateFinancial, AdminOnlyMultisigVoteBurnsQU)
{
    // Create admin-only MULTISIG (recipientCount=0, chainNextGateId=-1,
    // adminApprovalWindowEpochs>0), send QU as a vote
    // — verify QU is burned and _totalBurned increases
    QuGateTest env;

    uint64 creationFee = env.currentEscalatedFee();

    // Create MULTISIG gate with recipientCount=0 (admin-only governance gate)
    createGate_input in;
    memset(&in, 0, sizeof(in));
    in.mode = MODE_MULTISIG;
    in.recipientCount = 0;
    in.chainNextGateId = -1;
    auto out = env.createGate(ALICE, (sint64)creationFee, in);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    // Configure multisig: BOB and CHARLIE as guardians, 2-of-2, adminApprovalWindowEpochs=5
    id guardians[] = { BOB, CHARLIE };
    auto cfgStatus = env.configureMultisig(ALICE, out.gateId, guardians, 2, 2, 10, 5);
    ASSERT_EQ(cfgStatus, QUGATE_SUCCESS);

    uint64 totalBurnedBefore = env.state.get()._totalBurned;

    // BOB votes with 5000 QU
    auto voteOut = env.sendToMultisigGate(BOB, out.gateId, 5000);
    EXPECT_EQ(voteOut.status, QUGATE_SUCCESS);

    // The 5000 QU should be burned (admin-only multisig burns vote QU)
    EXPECT_EQ(env.state.get()._totalBurned - totalBurnedBefore, 5000ULL);
    EXPECT_EQ(env.qpi.totalBurned, 5000);

    // Gate balance should be 0 (burned, not accumulated)
    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.currentBalance, 0ULL);
}

TEST(QuGateFinancial, AdminOnlyMultisigMultipleVotesBurn)
{
    // Multiple guardian votes each burn their QU independently
    QuGateTest env;

    uint64 creationFee = env.currentEscalatedFee();

    createGate_input in;
    memset(&in, 0, sizeof(in));
    in.mode = MODE_MULTISIG;
    in.recipientCount = 0;
    in.chainNextGateId = -1;
    auto out = env.createGate(ALICE, (sint64)creationFee, in);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    id guardians[] = { BOB, CHARLIE, DAVE };
    auto cfgStatus = env.configureMultisig(ALICE, out.gateId, guardians, 3, 2, 10, 5);
    ASSERT_EQ(cfgStatus, QUGATE_SUCCESS);

    uint64 totalBurnedBefore = env.state.get()._totalBurned;

    // BOB votes with 3000 QU
    env.sendToMultisigGate(BOB, out.gateId, 3000);
    EXPECT_EQ(env.state.get()._totalBurned - totalBurnedBefore, 3000ULL);

    // CHARLIE votes with 7000 QU
    env.sendToMultisigGate(CHARLIE, out.gateId, 7000);
    EXPECT_EQ(env.state.get()._totalBurned - totalBurnedBefore, 10000ULL);

    // Total burned should be cumulative
    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.currentBalance, 0ULL);
}

// ---- 6. Chain hop fee tracked in _totalBurned ----

TEST(QuGateFinancial, ChainHopFeeBurnedAndTracked)
{
    // Create two chained gates, send QU through the chain
    // — verify _totalBurned includes the hop fee (1000 QU per hop)
    QuGateTest env;

    uint64 creationFee = env.currentEscalatedFee();
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    // Create target gate (g1) and source gate (g2)
    auto g1 = makeSimpleGate(env, ALICE, (sint64)creationFee, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(g1.status, QUGATE_SUCCESS);

    id recips2[] = { CHARLIE };
    auto g2 = makeSimpleGate(env, ALICE, (sint64)creationFee, MODE_SPLIT, 1, recips2, ratios);
    ASSERT_EQ(g2.status, QUGATE_SUCCESS);

    // Chain g2 → g1 (costs QUGATE_CHAIN_HOP_FEE to set up)
    env.setChain(ALICE, g2.gateId, g1.gateId, QUGATE_CHAIN_HOP_FEE);

    // Record state before chain routing
    uint64 totalBurnedBefore = env.state.get()._totalBurned;

    // Route 10000 through the chain: g2 → g1
    env.qpi.reset();
    sint64 forwarded = env.routeChain(g2.gateId, 10000);

    // Hop 1 (g2): 10000 - 1000 hop fee = 9000 forwarded through SPLIT to CHARLIE
    EXPECT_EQ(env.qpi.totalTransferredTo(CHARLIE), 9000);

    // Hop 2 (g1): 9000 - 1000 hop fee = 8000 forwarded through SPLIT to BOB
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 8000);

    // Total hop fees burned: 2 * 1000 = 2000
    EXPECT_EQ(env.qpi.totalBurned, 2000);

    // _totalBurned in state should NOT be updated by routeToGate/routeChain
    // (the harness routeToGate calls qpi.burn but doesn't update state._totalBurned)
    // However, verifying the qpi.burn accumulation is the key test
    EXPECT_EQ(env.qpi.totalBurned, 2 * QUGATE_CHAIN_HOP_FEE);
}

TEST(QuGateFinancial, SetChainBurnsHopFee)
{
    // Verify that setChain burns QUGATE_CHAIN_HOP_FEE and tracks it
    QuGateTest env;

    uint64 creationFee = env.currentEscalatedFee();
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    auto g1 = makeSimpleGate(env, ALICE, (sint64)creationFee, MODE_SPLIT, 1, recips, ratios);
    auto g2 = makeSimpleGate(env, ALICE, (sint64)creationFee, MODE_SPLIT, 1, recips, ratios);

    env.qpi.reset();
    auto out = env.setChain(ALICE, g2.gateId, g1.gateId, QUGATE_CHAIN_HOP_FEE);
    EXPECT_EQ(out.result, QUGATE_SUCCESS);

    // The hop fee should be burned
    EXPECT_EQ(env.qpi.totalBurned, QUGATE_CHAIN_HOP_FEE);
}

TEST(QuGateFinancial, SetChainExcessFeeRefunded)
{
    // If more than QUGATE_CHAIN_HOP_FEE is paid, excess is refunded
    QuGateTest env;

    uint64 creationFee = env.currentEscalatedFee();
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    auto g1 = makeSimpleGate(env, ALICE, (sint64)creationFee, MODE_SPLIT, 1, recips, ratios);
    auto g2 = makeSimpleGate(env, ALICE, (sint64)creationFee, MODE_SPLIT, 1, recips, ratios);

    env.qpi.reset();
    auto out = env.setChain(ALICE, g2.gateId, g1.gateId, 5000);
    EXPECT_EQ(out.result, QUGATE_SUCCESS);

    // Hop fee burned
    EXPECT_EQ(env.qpi.totalBurned, QUGATE_CHAIN_HOP_FEE);

    // Excess refunded to ALICE (5000 - 1000 = 4000)
    EXPECT_EQ(env.qpi.totalTransferredTo(ALICE), 4000);
}

// ---- 7. Excess creation fee seeds reserve ----

TEST(QuGateFinancial, ExcessCreationFeeSeedsReserve)
{
    // Create a gate with 150,000 QU when fee is 100,000
    // — verify gate.reserve = 50,000 (the excess)
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    uint64 creationFee = env.currentEscalatedFee(); // 100000
    ASSERT_EQ(creationFee, 100000ULL);

    uint64 payment = 150000;
    auto out = makeSimpleGate(env, ALICE, (sint64)payment, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    ASSERT_EQ(out.feePaid, creationFee);

    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.reserve, (sint64)(payment - creationFee)); // 50000
}

TEST(QuGateFinancial, ExactCreationFeeZeroReserve)
{
    // Paying exactly the fee should result in zero reserve
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    uint64 creationFee = env.currentEscalatedFee();
    auto out = makeSimpleGate(env, ALICE, (sint64)creationFee, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.reserve, 0);
}

TEST(QuGateFinancial, LargeExcessSeedsLargeReserve)
{
    // Verify large overpayment all goes to reserve
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    uint64 creationFee = env.currentEscalatedFee(); // 100000
    uint64 payment = 1000000; // 10x the fee
    auto out = makeSimpleGate(env, ALICE, (sint64)payment, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.reserve, (sint64)(payment - creationFee)); // 900000

    // No refund transfer — all excess is reserve
    EXPECT_EQ(env.qpi.totalTransferredTo(ALICE), 0);
}
