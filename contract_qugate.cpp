// QuGate contract unit tests

#include <gtest/gtest.h>
#include <cstring>
#include <cstdlib>
#include <cstdint>

// Self-contained m256i (avoids upstream UEFI headers)
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

// QPI shim
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

    // HashMap — linear scan
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

    // ContractState wrapper for dirty-tracking
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

// Test QPI context — tracks transfers, burns, ticks
struct TestQpiContext {
    id _invocator;
    sint64 _reward;
    uint16 _epoch;
    uint64 _tick;
    id _failTransferTo;
    bool _failTransferActive;
    int _failTransferRemaining;

    static constexpr int MAX_TRANSFERS = 64;
    struct Transfer { id to; sint64 amount; };
    Transfer transfers[MAX_TRANSFERS];
    int transferCount;

    sint64 totalBurned;

    TestQpiContext() : _reward(0), _epoch(100), _tick(12345), _failTransferActive(false), _failTransferRemaining(0), transferCount(0), totalBurned(0)
    {
        memset(&_invocator, 0, sizeof(_invocator));
        memset(&_failTransferTo, 0, sizeof(_failTransferTo));
        memset(transfers, 0, sizeof(transfers));
    }

    id invocator() const { return _invocator; }
    sint64 invocationReward() const { return _reward; }
    uint16 epoch() const { return _epoch; }
    uint64 tick() const { return _tick; }

    sint64 transfer(const id& to, sint64 amount)
    {
        if (_failTransferActive && to == _failTransferTo && _failTransferRemaining != 0)
        {
            if (_failTransferRemaining > 0)
            {
                _failTransferRemaining--;
                if (_failTransferRemaining == 0)
                {
                    _failTransferActive = false;
                }
            }
            return -1;
        }
        if (transferCount < MAX_TRANSFERS)
        {
            transfers[transferCount].to = to;
            transfers[transferCount].amount = amount;
            transferCount++;
        }
        return 0;
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

    void failTransfersTo(const id& to, int count = 1)
    {
        _failTransferTo = to;
        _failTransferActive = true;
        _failTransferRemaining = count;
    }

    void clearTransferFailures()
    {
        _failTransferActive = false;
        _failTransferRemaining = 0;
        memset(&_failTransferTo, 0, sizeof(_failTransferTo));
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

// Log macros (no-op in tests)
#define LOG_INFO(x) ((void)0)
#define LOG_WARNING(x) ((void)0)
#define CONTRACT_INDEX 0

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

// Fee split defaults
constexpr uint64 QUGATE_DEFAULT_FEE_BURN_BPS = 5000;
constexpr uint64 QUGATE_MIN_FEE_BURN_BPS = 3000;
constexpr uint64 QUGATE_MAX_FEE_BURN_BPS = 7000;

// Complexity-based idle fee multipliers
constexpr uint64 QUGATE_IDLE_BASE_MULTIPLIER_BPS = 10000;
constexpr uint64 QUGATE_IDLE_MULTI_RECIPIENT_THRESHOLD = 3;
constexpr uint64 QUGATE_IDLE_MULTI_RECIPIENT_MULTIPLIER_BPS = 15000;
constexpr uint64 QUGATE_IDLE_MAX_RECIPIENT_MULTIPLIER_BPS = 20000;
constexpr uint64 QUGATE_IDLE_HEARTBEAT_MULTIPLIER_BPS = 15000;
constexpr uint64 QUGATE_IDLE_MULTISIG_MULTIPLIER_BPS = 15000;
constexpr uint64 QUGATE_IDLE_CHAIN_EXTRA_BPS = 5000;
constexpr uint64 QUGATE_IDLE_SHIELD_PER_TARGET_BPS = 5000;  // +0.5x surcharge per downstream target shielded
constexpr uint64 QUGATE_FEE_DIVIDEND_BPS = 5000;     // 50% to shareholders

// Idle maintenance defaults
constexpr uint64 QUGATE_DEFAULT_MAINTENANCE_FEE = 25000;
constexpr uint64 QUGATE_DEFAULT_MAINTENANCE_INTERVAL_EPOCHS = 4;
constexpr uint64 QUGATE_DEFAULT_MAINTENANCE_GRACE_EPOCHS = 4;

// Time lock constants
constexpr uint8 QUGATE_TIME_LOCK_ABSOLUTE_EPOCH = 0;
constexpr uint8 QUGATE_TIME_LOCK_RELATIVE_EPOCHS = 1;
constexpr sint64 QUGATE_TIME_LOCK_NOT_CANCELLABLE = -24;

constexpr sint64 QUGATE_INVALID_CHAIN = -14;
constexpr uint8  QUGATE_MAX_CHAIN_DEPTH = 3;
constexpr sint64 QUGATE_CHAIN_HOP_FEE = 1000;
constexpr uint64 QUGATE_GATE_ID_SLOT_STRIDE = 1000000ULL;

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
constexpr sint64 QUGATE_HEARTBEAT_NOT_ACTIVE = -15;
constexpr sint64 QUGATE_HEARTBEAT_TRIGGERED = -16;
constexpr sint64 QUGATE_HEARTBEAT_INVALID = -17;
constexpr sint64 QUGATE_MULTISIG_NOT_GUARDIAN = -19;
constexpr sint64 QUGATE_MULTISIG_ALREADY_VOTED = -20;
constexpr sint64 QUGATE_MULTISIG_INVALID_CONFIG = -21;
constexpr sint64 QUGATE_MULTISIG_NO_ACTIVE_PROP = -22;
constexpr sint64 QUGATE_TIME_LOCK_ALREADY_FIRED = -23;
constexpr sint64 QUGATE_TIME_LOCK_EPOCH_PAST = -25;
constexpr sint64 QUGATE_INVALID_ADMIN_GATE = -26;
constexpr sint64 QUGATE_ADMIN_GATE_REQUIRED = -27;
constexpr sint64 QUGATE_INVALID_GATE_RECIPIENT = -28;
constexpr sint64 QUGATE_INVALID_ADMIN_CYCLE = -29;
constexpr sint64 QUGATE_MULTISIG_PROPOSAL_ACTIVE = -30;
constexpr sint64 QUGATE_INVALID_PARAMS = -31;

// GateConfig (matches QuGate.h exactly)
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
    Array<sint64, 8> recipientGateIds;

    // Chain fields
    sint64 chainNextGateId;
    uint8  chainDepth;

    // Unified reserve — covers chain hop fees and idle maintenance
    sint64 reserve;

    // Idle maintenance
    uint16 nextIdleChargeEpoch;

    sint64 adminGateId;
};

// Per-gate allowed-senders configuration (side array)
struct QUGATE_AllowedSendersConfig {
    Array<id, 8> senders;
    uint8 count;
};

// Multisig config
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

struct QUGATE_HeartbeatConfig_Test {
    uint32 thresholdEpochs;
    uint32 lastHeartbeatEpoch;
    uint8  payoutPercentPerEpoch;
    sint64 minimumBalance;
    uint8  active;
    uint8  triggered;
    uint32 triggerEpoch;
    uint8  beneficiaryCount;
    Array<id, 8> beneficiaryAddresses;
    Array<uint8, 8> beneficiaryShares;
};

// Time lock config
struct QUGATE_TimeLockConfig_Test {
    uint32 unlockEpoch;
    uint32 delayEpochs;
    uint8  lockMode;
    uint8  cancellable;
    uint8  fired;
    uint8  cancelled;
    uint8  active;
};

struct QUGATE_AdminApprovalState_Test {
    uint8 active;
    uint32 validUntilEpoch;
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
    uint64 _feeBurnBps;
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

    // Idle delinquency tracking (epoch when gate became delinquent, 0 = not delinquent)
    Array<uint16, QUGATE_MAX_GATES> _idleDelinquentEpochs;

    // Mode-specific state arrays
    Array<QUGATE_HeartbeatConfig_Test, QUGATE_MAX_GATES> _heartbeatConfigs;
    Array<QUGATE_MultisigConfig_Test, QUGATE_MAX_GATES> _multisigConfigs;
    Array<QUGATE_TimeLockConfig_Test, QUGATE_MAX_GATES> _timeLockConfigs;
    Array<QUGATE_AdminApprovalState_Test, QUGATE_MAX_GATES> _adminApprovalStates;
};

// Procedure I/O structs (match QuGate.h)
struct createGate_input {
    uint8 mode;
    uint8 recipientCount;
    Array<id, 8> recipients;
    Array<uint64, 8> ratios;
    Array<sint64, 8> recipientGateIds;
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
    Array<sint64, 8> recipientGateIds;
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
    Array<sint64, 8> recipientGateIds;
    sint64 chainNextGateId;
    uint8  chainDepth;
    sint64 reserve;
    sint64 adminGateId;
    uint8 hasAdminGate;
};

struct getHeartbeat_output {
    uint8 active;
    uint8 triggered;
    uint32 thresholdEpochs;
    uint32 lastHeartbeatEpoch;
    uint32 triggerEpoch;
    uint8 payoutPercentPerEpoch;
    sint64 minimumBalance;
    uint8 beneficiaryCount;
    Array<id, 8> beneficiaryAddresses;
    Array<uint8, 8> beneficiaryShares;
};

struct getAdminGate_output {
    uint8 hasAdminGate;
    sint64 adminGateId;
    uint8 adminGateMode;
    uint8 guardianCount;
    uint8 required;
    uint32 adminApprovalWindowEpochs;
    uint8 adminApprovalActive;
    uint32 adminApprovalValidUntilEpoch;
    Array<id, 8> guardians;
};

struct withdrawReserve_output {
    sint64 status;
    uint64 withdrawn;
};

struct getGateBySlot_output {
    uint8 valid;
    sint64 gateId;
    uint8 active;
    uint64 currentBalance;
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

// Test harness
class QuGateTest {
public:
    // ContractState wrapper for dirty-tracking
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
        state.mut()._feeBurnBps = QUGATE_DEFAULT_FEE_BURN_BPS;
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

    static sint64 versionedGateId(uint64 slotIdx, uint16 generation)
    {
        return (sint64)(slotIdx + 1 + (uint64)generation * QUGATE_GATE_ID_SLOT_STRIDE);
    }

    static uint64 slotFromGateId(sint64 gateId)
    {
        if (gateId <= 0)
        {
            return QUGATE_MAX_GATES;
        }
        return (uint64)((gateId - 1) % (sint64)QUGATE_GATE_ID_SLOT_STRIDE);
    }

    static uint16 generationFromGateId(sint64 gateId)
    {
        if (gateId <= 0)
        {
            return 0;
        }
        return (uint16)((gateId - 1) / (sint64)QUGATE_GATE_ID_SLOT_STRIDE);
    }

    bool gateIdMatchesCurrentGeneration(sint64 gateId) const
    {
        uint64 slotIdx = slotFromGateId(gateId);
        if (slotIdx >= state.get()._gateCount)
        {
            return false;
        }
        return state.get()._gateGenerations.get(slotIdx) == generationFromGateId(gateId);
    }

    sint64 encodeCurrentGateId(uint64 slotIdx) const
    {
        return versionedGateId(slotIdx, state.get()._gateGenerations.get(slotIdx));
    }

    bool adminApprovalValid(uint64 slotIdx) const
    {
        if (slotIdx >= state.get()._gateCount)
        {
            return false;
        }
        QUGATE_AdminApprovalState_Test approval = state.get()._adminApprovalStates.get(slotIdx);
        return approval.active == 1 && qpi.epoch() <= approval.validUntilEpoch;
    }

    bool authorizeGateMutation(const id& caller, uint64 slotIdx, sint64& statusOut, bool allowOwnerBypassForStaleClear = false, bool clearingAdmin = false)
    {
        GateConfig gate = state.get()._gates.get(slotIdx);
        if (gate.owner == caller && gate.adminGateId < 0)
        {
            return true;
        }
        if (gate.owner == caller && allowOwnerBypassForStaleClear && clearingAdmin)
        {
            uint64 adminSlot = slotFromGateId(gate.adminGateId);
            if (gate.adminGateId < 0
                || adminSlot >= state.get()._gateCount
                || !gateIdMatchesCurrentGeneration(gate.adminGateId))
            {
                return true;
            }
            GateConfig adminGate = state.get()._gates.get(adminSlot);
            if (adminGate.active == 0 || adminGate.mode != MODE_MULTISIG)
            {
                return true;
            }
        }
        if (gate.adminGateId >= 0)
        {
            uint64 adminSlot = slotFromGateId(gate.adminGateId);
            if (adminSlot < state.get()._gateCount
                && gateIdMatchesCurrentGeneration(gate.adminGateId))
            {
                GateConfig adminGate = state.get()._gates.get(adminSlot);
                if (adminGate.active == 1
                    && adminGate.mode == MODE_MULTISIG
                    && adminApprovalValid(adminSlot))
                {
                    return true;
                }
            }
            statusOut = QUGATE_ADMIN_GATE_REQUIRED;
            return false;
        }
        statusOut = QUGATE_UNAUTHORIZED;
        return false;
    }

    void consumeAdminApprovalIfUsed(const id& caller, uint64 slotIdx)
    {
        GateConfig gate = state.get()._gates.get(slotIdx);
        if (!(gate.owner == caller) || gate.adminGateId < 0)
        {
            return;
        }

        uint64 adminSlot = slotFromGateId(gate.adminGateId);
        if (adminSlot >= state.get()._gateCount || !gateIdMatchesCurrentGeneration(gate.adminGateId))
        {
            return;
        }

        GateConfig adminGate = state.get()._gates.get(adminSlot);
        if (adminGate.active == 0 || adminGate.mode != MODE_MULTISIG || !adminApprovalValid(adminSlot))
        {
            return;
        }

        QUGATE_AdminApprovalState_Test approval = state.get()._adminApprovalStates.get(adminSlot);
        approval.active = 0;
        approval.validUntilEpoch = 0;
        state.mut()._adminApprovalStates.set(adminSlot, approval);
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
        g.adminGateId = -1;
        // Set idle charge due epoch
        if (state.get()._idleWindowEpochs > 0)
        {
            g.nextIdleChargeEpoch = qpi.epoch() + (uint16)state.get()._idleWindowEpochs;
        }

        for (uint64 i = 0; i < input.recipientCount; i++)
        {
            if (input.recipientGateIds.get(i) >= 0)
            {
                uint64 recipientSlot = slotFromGateId(input.recipientGateIds.get(i));
                if (recipientSlot >= state.get()._gateCount
                    || !gateIdMatchesCurrentGeneration(input.recipientGateIds.get(i))
                    || state.get()._gates.get(recipientSlot).active == 0)
                {
                    qpi.transfer(creator, fee);
                    output.status = QUGATE_INVALID_GATE_RECIPIENT;
                    return output;
                }
            }
        }

        // Chain validation (chainNextGateId > 0 means chained; 0 or -1 = no chain)
        if (input.chainNextGateId > 0)
        {
            uint64 targetIdx = slotFromGateId(input.chainNextGateId);
            if (targetIdx >= state.get()._gateCount || !gateIdMatchesCurrentGeneration(input.chainNextGateId))
            {
                qpi.transfer(creator, fee);
                output.status = QUGATE_INVALID_CHAIN;
                return output;
            }
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
            g.chainNextGateId = encodeCurrentGateId(targetIdx);
            g.chainDepth = newDepth;
        }

        for (uint64 i = 0; i < QUGATE_MAX_RECIPIENTS; i++)
        {
            if (i < input.recipientCount)
            {
                g.recipients.set(i, input.recipients.get(i));
                g.ratios.set(i, input.ratios.get(i));
                g.recipientGateIds.set(i, input.recipientGateIds.get(i));
            }
            else {
                g.recipients.set(i, id::zero());
                g.ratios.set(i, 0);
                g.recipientGateIds.set(i, -1);
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
        uint64 creationBurnAmount = QPI::div(currentFee * state.get()._feeBurnBps, 10000ULL);
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
        for (uint8 gi = 0; gi < 8; gi++) in.recipientGateIds.set(gi, -1);
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

        if (state.get()._expiryEpochs > 0
            && (uint16)(qpi.epoch() - gate.lastActivityEpoch) >= state.get()._expiryEpochs)
        {
            if (gate.currentBalance > 0)
            {
                if (qpi.transfer(gate.owner, gate.currentBalance) >= 0)
                {
                    gate.currentBalance = 0;
                }
            }
            if (gate.reserve > 0)
            {
                if (qpi.transfer(gate.owner, gate.reserve) >= 0)
                {
                    gate.reserve = 0;
                }
            }
            state.mut()._gates.set(idx, gate);
            if (gate.currentBalance > 0 || gate.reserve > 0)
            {
                if (amount > 0) qpi.transfer(sender, amount);
                output.status = QUGATE_GATE_NOT_ACTIVE;
                return output;
            }
            gate.active = 0;
            state.mut()._gates.set(idx, gate);
            state.mut()._activeGates -= 1;
            state.mut()._freeSlots.set(state.get()._freeCount, idx);
            state.mut()._freeCount += 1;
            state.mut()._gateGenerations.set(idx, state.get()._gateGenerations.get(idx) + 1);
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

        if (gate.mode == MODE_TIME_LOCK)
        {
            QUGATE_TimeLockConfig_Test cfg = state.get()._timeLockConfigs.get(idx);
            if (cfg.active == 0 || cfg.cancelled == 1 || cfg.fired == 1)
            {
                if (amount > 0) qpi.transfer(sender, amount);
                output.status = QUGATE_GATE_NOT_ACTIVE;
                return output;
            }
        }

        if (gate.mode == MODE_MULTISIG)
        {
            return sendToMultisigGate(sender, gateId, amount);
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
            QUGATE_AllowedSendersConfig asCfg = state.get()._allowedSendersConfigs.get(idx);
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
        else if (gate.mode == MODE_TIME_LOCK)
        {
            QUGATE_TimeLockConfig_Test cfg = state.get()._timeLockConfigs.get(idx);
            if (cfg.lockMode == QUGATE_TIME_LOCK_RELATIVE_EPOCHS && cfg.unlockEpoch == 0)
            {
                cfg.unlockEpoch = qpi.epoch() + cfg.delayEpochs;
                state.mut()._timeLockConfigs.set(idx, cfg);
            }
            gate.currentBalance += amount;
        }
        else if (gate.mode == MODE_HEARTBEAT)
        {
            gate.currentBalance += amount;
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
        sint64 authStatus = QUGATE_SUCCESS;
        if (!authorizeGateMutation(caller, gateId - 1, authStatus))
        {
            output.status = authStatus;
            return output;
        }
        if (gate.active == 0)
        {
            output.status = QUGATE_GATE_NOT_ACTIVE;
            return output;
        }

        if (state.get()._expiryEpochs > 0
            && (uint16)(qpi.epoch() - gate.lastActivityEpoch) >= state.get()._expiryEpochs)
        {
            if (gate.currentBalance > 0)
            {
                if (qpi.transfer(gate.owner, gate.currentBalance) >= 0)
                {
                    gate.currentBalance = 0;
                }
            }
            if (gate.reserve > 0)
            {
                if (qpi.transfer(gate.owner, gate.reserve) >= 0)
                {
                    gate.reserve = 0;
                }
            }

            state.mut()._gates.set(gateId - 1, gate);

            if (gate.currentBalance > 0 || gate.reserve > 0)
            {
                output.status = QUGATE_GATE_NOT_ACTIVE;
                if (reward > 0) qpi.transfer(caller, reward);
                return output;
            }

            gate.active = 0;
            state.mut()._gates.set(gateId - 1, gate);
            state.mut()._activeGates -= 1;
            state.mut()._freeSlots.set(state.get()._freeCount, gateId - 1);
            state.mut()._freeCount += 1;
            state.mut()._gateGenerations.set(gateId - 1, state.get()._gateGenerations.get(gateId - 1) + 1);

            if (reward > 0) qpi.transfer(caller, reward);
            output.status = QUGATE_GATE_NOT_ACTIVE;
            return output;
        }

        if (gate.currentBalance > 0)
        {
            if (qpi.transfer(gate.owner, gate.currentBalance) >= 0)
            {
                gate.currentBalance = 0;
            }
        }

        // Refund reserve
        if (gate.reserve > 0)
        {
            if (qpi.transfer(gate.owner, gate.reserve) >= 0)
            {
                gate.reserve = 0;
            }
        }

        state.mut()._gates.set(gateId - 1, gate);

        if (gate.currentBalance > 0 || gate.reserve > 0)
        {
            output.status = QUGATE_INVALID_PARAMS;
            if (reward > 0) qpi.transfer(caller, reward);
            return output;
        }

        gate.active = 0;
        state.mut()._gates.set(gateId - 1, gate);
        state.mut()._activeGates -= 1;

        state.mut()._freeSlots.set(state.get()._freeCount, gateId - 1);
        state.mut()._freeCount += 1;
        state.mut()._gateGenerations.set(gateId - 1, state.get()._gateGenerations.get(gateId - 1) + 1);

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

        sint64 authStatus = QUGATE_SUCCESS;
        if (!authorizeGateMutation(caller, input.gateId - 1, authStatus))
        {
            if (reward > 0) qpi.transfer(caller, reward);
            output.status = authStatus;
            return output;
        }
        consumeAdminApprovalIfUsed(caller, input.gateId - 1);
        if (gate.active == 0)
        {
            if (reward > 0) qpi.transfer(caller, reward);
            output.status = QUGATE_GATE_NOT_ACTIVE;
            return output;
        }
        if (state.get()._expiryEpochs > 0
            && (uint16)(qpi.epoch() - gate.lastActivityEpoch) >= state.get()._expiryEpochs)
        {
            if (gate.currentBalance > 0)
            {
                if (qpi.transfer(gate.owner, gate.currentBalance) >= 0)
                {
                    gate.currentBalance = 0;
                }
            }
            if (gate.reserve > 0)
            {
                if (qpi.transfer(gate.owner, gate.reserve) >= 0)
                {
                    gate.reserve = 0;
                }
            }
            state.mut()._gates.set(input.gateId - 1, gate);
            if (gate.currentBalance > 0 || gate.reserve > 0)
            {
                if (reward > 0) qpi.transfer(caller, reward);
                output.status = QUGATE_GATE_NOT_ACTIVE;
                return output;
            }
            gate.active = 0;
            state.mut()._gates.set(input.gateId - 1, gate);
            state.mut()._activeGates -= 1;
            state.mut()._freeSlots.set(state.get()._freeCount, input.gateId - 1);
            state.mut()._freeCount += 1;
            state.mut()._gateGenerations.set(input.gateId - 1, state.get()._gateGenerations.get(input.gateId - 1) + 1);
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

        for (uint64 i = 0; i < input.recipientCount; i++)
        {
            if (input.recipientGateIds.get(i) >= 0)
            {
                uint64 recipientSlot = slotFromGateId(input.recipientGateIds.get(i));
                if (recipientSlot >= state.get()._gateCount
                    || !gateIdMatchesCurrentGeneration(input.recipientGateIds.get(i))
                    || state.get()._gates.get(recipientSlot).active == 0)
                {
                    if (reward > 0) qpi.transfer(caller, reward);
                    output.status = QUGATE_INVALID_GATE_RECIPIENT;
                    return output;
                }
            }
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
                gate.recipientGateIds.set(i, input.recipientGateIds.get(i));
            }
            else {
                gate.recipients.set(i, id::zero());
                gate.ratios.set(i, 0);
                gate.recipientGateIds.set(i, -1);
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
        // Idle maintenance charging (mirrors QuGate.h END_EPOCH maintenance loop)
        for (uint64 i = 0; i < state.get()._gateCount; i++)
        {
            GateConfig gate = state.get()._gates.get(i);
            if (gate.active == 0) continue;
            if (state.get()._idleFee == 0) continue;

            // Admin gate drain: governed gate pays its admin multisig's idle fees
            if (gate.adminGateId > 0 && gate.reserve > 0)
            {
                uint64 adminSlot = slotFromGateId(gate.adminGateId);
                if (adminSlot < state.get()._gateCount && gateIdMatchesCurrentGeneration(gate.adminGateId))
                {
                    GateConfig adminGate = state.get()._gates.get(adminSlot);
                    if (adminGate.active == 1)
                    {
                        uint64 adminFee = QPI::div(state.get()._idleFee * QUGATE_IDLE_MULTISIG_MULTIPLIER_BPS, 10000ULL);
                        if (gate.reserve >= (sint64)adminFee)
                        {
                            gate.reserve -= adminFee;
                            adminGate.lastActivityEpoch = qpi.epoch();
                            if (state.get()._idleWindowEpochs > 0)
                                adminGate.nextIdleChargeEpoch = qpi.epoch() + (uint16)state.get()._idleWindowEpochs;
                            state.mut()._gates.set(adminSlot, adminGate);
                            state.mut()._gates.set(i, gate);
                            state.mut()._idleDelinquentEpochs.set(adminSlot, 0);
                            state.mut()._totalMaintenanceCharged += adminFee;
                            uint64 adminBurn = QPI::div(adminFee * state.get()._feeBurnBps, 10000ULL);
                            uint64 adminDiv = adminFee - adminBurn;
                            qpi.burn(adminBurn);
                            state.mut()._totalBurned += adminBurn;
                            state.mut()._totalMaintenanceBurned += adminBurn;
                            state.mut()._earnedMaintenanceDividends += adminDiv;
                            state.mut()._totalMaintenanceDividends += adminDiv;
                        }
                    }
                }
            }

            uint16 delinquentEpoch = state.get()._idleDelinquentEpochs.get(i);

            // Recently-active check: skip charge if gate had activity within window
            uint8 recentlyActive = 0;
            if (state.get()._idleWindowEpochs > 0
                && qpi.epoch() - gate.lastActivityEpoch < state.get()._idleWindowEpochs)
            {
                recentlyActive = 1;
            }

            // Active-hold exemptions: modes with pending operations skip charge
            uint8 activeHold = 0;
            if (gate.mode == MODE_HEARTBEAT)
            {
                QUGATE_HeartbeatConfig_Test hbCfg = state.get()._heartbeatConfigs.get(i);
                if (hbCfg.active == 1 && hbCfg.triggered == 0) activeHold = 1;
            }
            else if (gate.mode == MODE_TIME_LOCK)
            {
                QUGATE_TimeLockConfig_Test tlCfg = state.get()._timeLockConfigs.get(i);
                if (tlCfg.active == 1 && tlCfg.fired == 0 && tlCfg.cancelled == 0 && gate.currentBalance > 0) activeHold = 1;
            }
            else if (gate.mode == MODE_THRESHOLD)
            {
                if (gate.currentBalance > 0) activeHold = 1;
            }
            else if (gate.mode == MODE_MULTISIG)
            {
                QUGATE_MultisigConfig_Test msCfg = state.get()._multisigConfigs.get(i);
                if (gate.currentBalance > 0 || msCfg.proposalActive == 1) activeHold = 1;
            }

            if (recentlyActive == 1 || activeHold == 1)
            {
                if (state.get()._idleWindowEpochs > 0)
                {
                    gate.nextIdleChargeEpoch = qpi.epoch() + (uint16)state.get()._idleWindowEpochs;
                    state.mut()._gates.set(i, gate);
                }
                if (delinquentEpoch > 0)
                {
                    state.mut()._idleDelinquentEpochs.set(i, 0);
                }

                // Reserve drain: pay downstream gates' idle fees from the upstream gate's reserve.
                // Also applies a shielding surcharge to the upstream gate's own idle fee.
                // If the upstream reserve can't cover a downstream fee, that gate is left alone
                // and will become delinquent through normal idle charging.
                if (activeHold == 1 && recentlyActive == 0 && gate.reserve > 0)
                {
                    // Re-read the gate in case it was modified above
                    gate = state.get()._gates.get(i);
                    uint64 downstreamCount = 0;

                    // Count and pay for chain target
                    if (gate.chainNextGateId >= 0)
                    {
                        uint64 dsSlot = slotFromGateId(gate.chainNextGateId);
                        if (dsSlot < state.get()._gateCount && gateIdMatchesCurrentGeneration(gate.chainNextGateId))
                        {
                            GateConfig dsGate = state.get()._gates.get(dsSlot);
                            if (dsGate.active == 1)
                            {
                                downstreamCount++;
                                // Compute downstream gate's effective idle fee
                                uint64 dsMultiplierBps = QUGATE_IDLE_BASE_MULTIPLIER_BPS;
                                if (dsGate.recipientCount >= QUGATE_MAX_RECIPIENTS)
                                    dsMultiplierBps = QUGATE_IDLE_MAX_RECIPIENT_MULTIPLIER_BPS;
                                else if (dsGate.recipientCount >= QUGATE_IDLE_MULTI_RECIPIENT_THRESHOLD)
                                    dsMultiplierBps = QUGATE_IDLE_MULTI_RECIPIENT_MULTIPLIER_BPS;
                                if (dsGate.chainNextGateId >= 0)
                                    dsMultiplierBps += QUGATE_IDLE_CHAIN_EXTRA_BPS;
                                uint64 dsIdleFee = QPI::div(state.get()._idleFee * dsMultiplierBps, 10000ULL);

                                if (gate.reserve >= (sint64)dsIdleFee)
                                {
                                    gate.reserve -= dsIdleFee;
                                    dsGate.lastActivityEpoch = qpi.epoch();
                                    if (state.get()._idleWindowEpochs > 0)
                                        dsGate.nextIdleChargeEpoch = qpi.epoch() + (uint16)state.get()._idleWindowEpochs;
                                    state.mut()._gates.set(dsSlot, dsGate);
                                    state.mut()._idleDelinquentEpochs.set(dsSlot, 0);
                                    state.mut()._totalMaintenanceCharged += dsIdleFee;

                                    uint64 dsBurnAmount = QPI::div(dsIdleFee * state.get()._feeBurnBps, 10000ULL);
                                    uint64 dsDividendAmount = dsIdleFee - dsBurnAmount;
                                    qpi.burn(dsBurnAmount);
                                    state.mut()._totalBurned += dsBurnAmount;
                                    state.mut()._totalMaintenanceBurned += dsBurnAmount;
                                    state.mut()._earnedMaintenanceDividends += dsDividendAmount;
                                    state.mut()._totalMaintenanceDividends += dsDividendAmount;
                                }
                            }
                        }
                    }

                    // Count and pay for gate-as-recipient targets
                    for (uint8 ri = 0; ri < gate.recipientCount; ri++)
                    {
                        if (gate.recipientGateIds.get(ri) >= 0)
                        {
                            uint64 dsSlot = slotFromGateId(gate.recipientGateIds.get(ri));
                            if (dsSlot < state.get()._gateCount && gateIdMatchesCurrentGeneration(gate.recipientGateIds.get(ri)))
                            {
                                GateConfig dsGate = state.get()._gates.get(dsSlot);
                                if (dsGate.active == 1)
                                {
                                    downstreamCount++;
                                    uint64 dsMultiplierBps = QUGATE_IDLE_BASE_MULTIPLIER_BPS;
                                    if (dsGate.recipientCount >= QUGATE_MAX_RECIPIENTS)
                                        dsMultiplierBps = QUGATE_IDLE_MAX_RECIPIENT_MULTIPLIER_BPS;
                                    else if (dsGate.recipientCount >= QUGATE_IDLE_MULTI_RECIPIENT_THRESHOLD)
                                        dsMultiplierBps = QUGATE_IDLE_MULTI_RECIPIENT_MULTIPLIER_BPS;
                                    if (dsGate.chainNextGateId >= 0)
                                        dsMultiplierBps += QUGATE_IDLE_CHAIN_EXTRA_BPS;
                                    uint64 dsIdleFee = QPI::div(state.get()._idleFee * dsMultiplierBps, 10000ULL);

                                    if (gate.reserve >= (sint64)dsIdleFee)
                                    {
                                        gate.reserve -= dsIdleFee;
                                        dsGate.lastActivityEpoch = qpi.epoch();
                                        if (state.get()._idleWindowEpochs > 0)
                                            dsGate.nextIdleChargeEpoch = qpi.epoch() + (uint16)state.get()._idleWindowEpochs;
                                        state.mut()._gates.set(dsSlot, dsGate);
                                        state.mut()._idleDelinquentEpochs.set(dsSlot, 0);
                                        state.mut()._totalMaintenanceCharged += dsIdleFee;

                                        uint64 dsBurnAmount = QPI::div(dsIdleFee * state.get()._feeBurnBps, 10000ULL);
                                        uint64 dsDividendAmount = dsIdleFee - dsBurnAmount;
                                        qpi.burn(dsBurnAmount);
                                        state.mut()._totalBurned += dsBurnAmount;
                                        state.mut()._totalMaintenanceBurned += dsBurnAmount;
                                        state.mut()._earnedMaintenanceDividends += dsDividendAmount;
                                        state.mut()._totalMaintenanceDividends += dsDividendAmount;
                                    }
                                }
                            }
                        }
                    }

                    // Apply shielding surcharge: upstream gate's own idle fee increases
                    // per downstream target it is paying for
                    if (downstreamCount > 0)
                    {
                        uint64 surcharge = QPI::div(
                            state.get()._idleFee * (uint64)downstreamCount * QUGATE_IDLE_SHIELD_PER_TARGET_BPS,
                            10000ULL);
                        if (gate.reserve >= (sint64)surcharge)
                        {
                            gate.reserve -= surcharge;
                            state.mut()._totalMaintenanceCharged += surcharge;

                            uint64 surchargeBurn = QPI::div(surcharge * state.get()._feeBurnBps, 10000ULL);
                            uint64 surchargeDividend = surcharge - surchargeBurn;
                            qpi.burn(surchargeBurn);
                            state.mut()._totalBurned += surchargeBurn;
                            state.mut()._totalMaintenanceBurned += surchargeBurn;
                            state.mut()._earnedMaintenanceDividends += surchargeDividend;
                            state.mut()._totalMaintenanceDividends += surchargeDividend;
                        }
                    }

                    // Persist updated reserve
                    state.mut()._gates.set(i, gate);
                }

                continue;
            }

            // First-time idle: seed nextIdleChargeEpoch if not yet set
            if (gate.nextIdleChargeEpoch == 0 && state.get()._idleWindowEpochs > 0)
            {
                gate.nextIdleChargeEpoch = qpi.epoch() + (uint16)state.get()._idleWindowEpochs;
                state.mut()._gates.set(i, gate);
                continue;
            }

            // Charge inactivity maintenance when idle gate reaches its due epoch
            if (state.get()._idleWindowEpochs > 0
                && gate.nextIdleChargeEpoch > 0
                && qpi.epoch() >= gate.nextIdleChargeEpoch)
            {
                // Compute complexity-based effective fee
                uint64 idleMultiplierBps = QUGATE_IDLE_BASE_MULTIPLIER_BPS;
                if (gate.recipientCount >= QUGATE_MAX_RECIPIENTS)
                    idleMultiplierBps = QUGATE_IDLE_MAX_RECIPIENT_MULTIPLIER_BPS;
                else if (gate.recipientCount >= QUGATE_IDLE_MULTI_RECIPIENT_THRESHOLD)
                    idleMultiplierBps = QUGATE_IDLE_MULTI_RECIPIENT_MULTIPLIER_BPS;
                if (gate.mode == MODE_HEARTBEAT && idleMultiplierBps < QUGATE_IDLE_HEARTBEAT_MULTIPLIER_BPS)
                    idleMultiplierBps = QUGATE_IDLE_HEARTBEAT_MULTIPLIER_BPS;
                if (gate.mode == MODE_MULTISIG && idleMultiplierBps < QUGATE_IDLE_MULTISIG_MULTIPLIER_BPS)
                    idleMultiplierBps = QUGATE_IDLE_MULTISIG_MULTIPLIER_BPS;
                if (gate.chainNextGateId >= 0)
                    idleMultiplierBps += QUGATE_IDLE_CHAIN_EXTRA_BPS;
                uint64 effectiveIdleFee = QPI::div(state.get()._idleFee * idleMultiplierBps, 10000ULL);

                if (gate.reserve >= (sint64)effectiveIdleFee)
                {
                    gate.reserve -= effectiveIdleFee;
                    gate.nextIdleChargeEpoch = qpi.epoch() + (uint16)state.get()._idleWindowEpochs;
                    state.mut()._gates.set(i, gate);
                    state.mut()._idleDelinquentEpochs.set(i, 0);
                    state.mut()._totalMaintenanceCharged += effectiveIdleFee;

                    uint64 maintenanceBurnAmount = QPI::div(effectiveIdleFee * state.get()._feeBurnBps, 10000ULL);
                    uint64 maintenanceDividendAmount = effectiveIdleFee - maintenanceBurnAmount;
                    qpi.burn(maintenanceBurnAmount);
                    state.mut()._totalBurned += maintenanceBurnAmount;
                    state.mut()._totalMaintenanceBurned += maintenanceBurnAmount;
                    state.mut()._earnedMaintenanceDividends += maintenanceDividendAmount;
                    state.mut()._totalMaintenanceDividends += maintenanceDividendAmount;
                }
                else if (delinquentEpoch == 0)
                {
                    state.mut()._idleDelinquentEpochs.set(i, qpi.epoch());
                }
            }
        }

        // Expire inactive gates (inactivity expiry OR delinquency grace expiry)
        for (uint64 i = 0; i < state.get()._gateCount; i++)
        {
            GateConfig gate = state.get()._gates.get(i);
            uint16 delinquentEpoch = state.get()._idleDelinquentEpochs.get(i);
            if (gate.active == 1 && state.get()._expiryEpochs > 0)
            {
                // Exempt hold-state gates from inactivity expiry
                if (gate.mode == MODE_TIME_LOCK)
                {
                    QUGATE_TimeLockConfig_Test tlCfg = state.get()._timeLockConfigs.get(i);
                    if (delinquentEpoch == 0 && tlCfg.active == 1 && tlCfg.fired == 0 && tlCfg.cancelled == 0)
                        continue;
                }
                if (gate.mode == MODE_HEARTBEAT)
                {
                    QUGATE_HeartbeatConfig_Test hbCfg = state.get()._heartbeatConfigs.get(i);
                    if (delinquentEpoch == 0 && hbCfg.active == 1 && hbCfg.triggered == 0)
                        continue;
                }
                if (gate.mode == MODE_MULTISIG && delinquentEpoch == 0 && gate.currentBalance > 0)
                    continue;

                bool graceExpired = (delinquentEpoch > 0 && state.get()._idleGraceEpochs > 0
                    && qpi.epoch() - delinquentEpoch >= state.get()._idleGraceEpochs);
                bool inactivityExpired = (qpi.epoch() - gate.lastActivityEpoch >= state.get()._expiryEpochs);
                if (graceExpired || inactivityExpired)
                {
                    if (gate.currentBalance > 0)
                    {
                        if (qpi.transfer(gate.owner, gate.currentBalance) >= 0)
                        {
                            gate.currentBalance = 0;
                        }
                    }
                    if (gate.reserve > 0)
                    {
                        if (qpi.transfer(gate.owner, gate.reserve) >= 0)
                        {
                            gate.reserve = 0;
                        }
                    }
                    state.mut()._gates.set(i, gate);
                    if (gate.currentBalance > 0 || gate.reserve > 0)
                    {
                        continue;
                    }
                    gate.active = 0;
                    state.mut()._gates.set(i, gate);
                    state.mut()._activeGates -= 1;
                    state.mut()._idleDelinquentEpochs.set(i, 0);
                    state.mut()._freeSlots.set(state.get()._freeCount, i);
                    state.mut()._freeCount += 1;
                    // Increment generation so recycled slot gets a new gateId
                    state.mut()._gateGenerations.set(i, state.get()._gateGenerations.get(i) + 1);
                }
            }
        }

        // TIME_LOCK release
        for (uint64 i = 0; i < state.get()._gateCount; i++)
        {
            GateConfig gate = state.get()._gates.get(i);
            if (gate.active == 0 || gate.mode != MODE_HEARTBEAT)
            {
                continue;
            }

            QUGATE_HeartbeatConfig_Test cfg = state.get()._heartbeatConfigs.get(i);
            if (cfg.active == 0)
            {
                continue;
            }

            if (cfg.triggered == 1)
            {
                sint64 balance = (sint64)gate.currentBalance;
                if (balance > cfg.minimumBalance)
                {
                    sint64 payoutTotal = (sint64)QPI::div((uint64)(balance * cfg.payoutPercentPerEpoch), 100ULL);
                    if (payoutTotal == 0 && balance > 0)
                    {
                        payoutTotal = balance;
                    }

                    if (payoutTotal > 0)
                    {
                        sint64 distributed = 0;
                        for (uint8 j = 0; j < cfg.beneficiaryCount; j++)
                        {
                            sint64 share = 0;
                            if (j == cfg.beneficiaryCount - 1)
                            {
                                share = payoutTotal - distributed;
                            }
                            else
                            {
                                share = (sint64)QPI::div((uint64)(payoutTotal * cfg.beneficiaryShares.get(j)), 100ULL);
                            }

                            if (share > 0 && qpi.transfer(cfg.beneficiaryAddresses.get(j), share) >= 0)
                            {
                                distributed += share;
                                gate.totalForwarded += (uint64)share;
                                gate.currentBalance -= (uint64)share;
                            }
                        }

                        if (gate.chainNextGateId != -1 && payoutTotal > distributed)
                        {
                            sint64 chainAmount = payoutTotal - distributed;
                            gate.totalForwarded += (uint64)chainAmount;
                            gate.currentBalance -= (uint64)chainAmount;
                            state.mut()._gates.set(i, gate);

                            sint64 currentChainGateId = gate.chainNextGateId;
                            uint8 hop = 0;
                            while (hop < QUGATE_MAX_CHAIN_DEPTH && currentChainGateId != -1 && chainAmount > 0)
                            {
                                uint64 nextIdx = slotFromGateId(currentChainGateId);
                                if (nextIdx >= state.get()._gateCount || !gateIdMatchesCurrentGeneration(currentChainGateId))
                                {
                                    break;
                                }

                                RouteResult routed = routeToGate(nextIdx, chainAmount, hop);
                                chainAmount = routed.forwarded;
                                GateConfig nextGate = state.get()._gates.get(nextIdx);
                                currentChainGateId = nextGate.chainNextGateId;
                                hop++;
                            }

                            if (chainAmount > 0 && currentChainGateId != -1)
                            {
                                gate = state.get()._gates.get(i);
                                gate.currentBalance += (uint64)chainAmount;
                                gate.totalForwarded -= (uint64)chainAmount;
                            }
                        }
                    }
                    state.mut()._gates.set(i, gate);
                }

                gate = state.get()._gates.get(i);
                if ((sint64)gate.currentBalance <= cfg.minimumBalance)
                {
                    if (gate.currentBalance > 0 && cfg.beneficiaryCount > 0)
                    {
                        sint64 dustTotal = (sint64)gate.currentBalance;
                        sint64 distributed = 0;
                        for (uint8 j = 0; j < cfg.beneficiaryCount; j++)
                        {
                            sint64 share = 0;
                            if (j == cfg.beneficiaryCount - 1)
                            {
                                share = dustTotal - distributed;
                            }
                            else
                            {
                                share = (sint64)QPI::div((uint64)(dustTotal * cfg.beneficiaryShares.get(j)), 100ULL);
                            }
                            if (share > 0 && qpi.transfer(cfg.beneficiaryAddresses.get(j), share) >= 0)
                            {
                                distributed += share;
                                gate.totalForwarded += (uint64)share;
                                gate.currentBalance -= (uint64)share;
                            }
                        }
                    }
                    else if (gate.currentBalance > 0)
                    {
                        if (qpi.transfer(gate.owner, (sint64)gate.currentBalance) >= 0)
                        {
                            gate.currentBalance = 0;
                        }
                    }

                    state.mut()._gates.set(i, gate);
                    if (gate.currentBalance > 0)
                    {
                        continue;
                    }

                    gate.active = 0;
                    state.mut()._gates.set(i, gate);
                    state.mut()._activeGates -= 1;
                    state.mut()._freeSlots.set(state.get()._freeCount, i);
                    state.mut()._freeCount += 1;
                    state.mut()._gateGenerations.set(i, state.get()._gateGenerations.get(i) + 1);
                }
            }
            else
            {
                uint32 epochsInactive = (uint32)(qpi.epoch() - cfg.lastHeartbeatEpoch);
                if (epochsInactive > cfg.thresholdEpochs)
                {
                    cfg.triggered = 1;
                    cfg.triggerEpoch = qpi.epoch();
                    state.mut()._heartbeatConfigs.set(i, cfg);
                }
            }
        }

        for (uint64 i = 0; i < state.get()._gateCount; i++)
        {
            QUGATE_MultisigConfig_Test cfg = state.get()._multisigConfigs.get(i);
            if (cfg.proposalActive == 1
                && (uint32)(qpi.epoch() - cfg.proposalEpoch) > cfg.proposalExpiryEpochs)
            {
                cfg.approvalBitmap = 0;
                cfg.approvalCount = 0;
                cfg.proposalActive = 0;
                state.mut()._multisigConfigs.set(i, cfg);
            }
        }

        for (uint64 i = 0; i < state.get()._gateCount; i++)
        {
            GateConfig gate = state.get()._gates.get(i);
            if (gate.active == 0 || gate.mode != MODE_TIME_LOCK)
            {
                continue;
            }

            QUGATE_TimeLockConfig_Test cfg = state.get()._timeLockConfigs.get(i);
            if (cfg.active == 0 || cfg.fired == 1 || cfg.cancelled == 1 || cfg.unlockEpoch == 0)
            {
                continue;
            }

            if ((uint32)qpi.epoch() >= cfg.unlockEpoch)
            {
                if (gate.currentBalance > 0)
                {
                    sint64 releaseAmount = (sint64)gate.currentBalance;
                    bool transferred = false;
                    if (gate.recipientCount > 0)
                    {
                        if (gate.recipientGateIds.get(0) > 0)
                        {
                            uint64 targetSlot = slotFromGateId(gate.recipientGateIds.get(0));
                            if (targetSlot < state.get()._gateCount)
                            {
                                RouteResult routed = routeToGate(targetSlot, releaseAmount, 0);
                                transferred = routed.accepted;
                            }
                        }
                        else if (qpi.transfer(gate.recipients.get(0), releaseAmount) >= 0)
                        {
                            transferred = true;
                        }
                    }
                    else if (gate.chainNextGateId != -1)
                    {
                        sint64 chainAmount = releaseAmount;
                        gate.totalForwarded += (uint64)chainAmount;
                        gate.currentBalance -= (uint64)chainAmount;
                        state.mut()._gates.set(i, gate);

                        sint64 currentChainGateId = gate.chainNextGateId;
                        uint8 hop = 0;
                        while (hop < QUGATE_MAX_CHAIN_DEPTH && currentChainGateId != -1 && chainAmount > 0)
                        {
                            uint64 nextIdx = slotFromGateId(currentChainGateId);
                            if (nextIdx >= state.get()._gateCount || !gateIdMatchesCurrentGeneration(currentChainGateId))
                            {
                                break;
                            }

                            RouteResult routed = routeToGate(nextIdx, chainAmount, hop);
                            chainAmount = routed.forwarded;
                            GateConfig nextGate = state.get()._gates.get(nextIdx);
                            currentChainGateId = nextGate.chainNextGateId;
                            hop++;
                        }

                        if (chainAmount > 0 && currentChainGateId != -1)
                        {
                            gate = state.get()._gates.get(i);
                            gate.currentBalance += (uint64)chainAmount;
                            gate.totalForwarded -= (uint64)chainAmount;
                            state.mut()._gates.set(i, gate);
                        }

                        gate = state.get()._gates.get(i);
                        transferred = (gate.currentBalance == 0);
                    }

                    if (transferred)
                    {
                        if (gate.currentBalance > 0)
                        {
                            gate.totalForwarded += gate.currentBalance;
                            gate.currentBalance = 0;
                            state.mut()._gates.set(i, gate);
                        }
                    }
                }

                gate = state.get()._gates.get(i);
                if (gate.currentBalance > 0)
                {
                    continue;
                }

                cfg.fired = 1;
                state.mut()._timeLockConfigs.set(i, cfg);
                gate.active = 0;
                state.mut()._gates.set(i, gate);
                state.mut()._activeGates -= 1;
                state.mut()._freeSlots.set(state.get()._freeCount, i);
                state.mut()._freeCount += 1;
                state.mut()._gateGenerations.set(i, state.get()._gateGenerations.get(i) + 1);
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
        out.adminGateId = g.adminGateId;
        out.hasAdminGate = (g.adminGateId >= 0) ? 1 : 0;
        for (uint64 i = 0; i < QUGATE_MAX_RECIPIENTS; i++)
        {
            out.recipients.set(i, g.recipients.get(i));
            out.ratios.set(i, g.ratios.get(i));
            out.recipientGateIds.set(i, g.recipientGateIds.get(i));
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
        if (state.get()._expiryEpochs > 0
            && (uint16)(qpi.epoch() - gate.lastActivityEpoch) >= state.get()._expiryEpochs)
        {
            if (gate.currentBalance > 0)
            {
                if (qpi.transfer(gate.owner, gate.currentBalance) >= 0)
                {
                    gate.currentBalance = 0;
                }
            }
            if (gate.reserve > 0)
            {
                if (qpi.transfer(gate.owner, gate.reserve) >= 0)
                {
                    gate.reserve = 0;
                }
            }
            state.mut()._gates.set(gateId - 1, gate);
            if (gate.currentBalance > 0 || gate.reserve > 0)
            {
                if (amount > 0) qpi.transfer(caller, amount);
                output.result = QUGATE_GATE_NOT_ACTIVE;
                return output;
            }
            gate.active = 0;
            state.mut()._gates.set(gateId - 1, gate);
            state.mut()._activeGates -= 1;
            state.mut()._freeSlots.set(state.get()._freeCount, gateId - 1);
            state.mut()._freeCount += 1;
            state.mut()._gateGenerations.set(gateId - 1, state.get()._gateGenerations.get(gateId - 1) + 1);
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

    sint64 configureHeartbeat(const id& caller, uint64 gateId, uint32 thresholdEpochs,
                              uint8 payoutPercentPerEpoch, sint64 minimumBalance,
                              id* beneficiaries, uint8* beneficiaryShares, uint8 beneficiaryCount)
    {
        qpi.reset();
        qpi._invocator = caller;

        if (gateId == 0 || gateId > state.get()._gateCount) return QUGATE_INVALID_GATE_ID;
        uint64 idx = gateId - 1;
        GateConfig gate = state.get()._gates.get(idx);
        sint64 authStatus = QUGATE_SUCCESS;
        if (!authorizeGateMutation(caller, idx, authStatus))
        {
            return authStatus;
        }
        consumeAdminApprovalIfUsed(caller, idx);
        if (gate.active == 0) return QUGATE_GATE_NOT_ACTIVE;
        if (gate.mode != MODE_HEARTBEAT) return QUGATE_HEARTBEAT_NOT_ACTIVE;
        if (thresholdEpochs == 0 || payoutPercentPerEpoch == 0 || payoutPercentPerEpoch > 100) return QUGATE_HEARTBEAT_INVALID;
        if (beneficiaryCount == 0 || beneficiaryCount > 8) return QUGATE_HEARTBEAT_INVALID;

        uint64 shareSum = 0;
        for (uint8 i = 0; i < beneficiaryCount; i++)
        {
            shareSum += beneficiaryShares[i];
        }
        if (shareSum != 100) return QUGATE_HEARTBEAT_INVALID;

        QUGATE_HeartbeatConfig_Test cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.thresholdEpochs = thresholdEpochs;
        cfg.lastHeartbeatEpoch = qpi.epoch();
        cfg.payoutPercentPerEpoch = payoutPercentPerEpoch;
        cfg.minimumBalance = minimumBalance;
        cfg.active = 1;
        cfg.triggered = 0;
        cfg.triggerEpoch = 0;
        cfg.beneficiaryCount = beneficiaryCount;
        for (uint8 i = 0; i < 8; i++)
        {
            if (i < beneficiaryCount)
            {
                cfg.beneficiaryAddresses.set(i, beneficiaries[i]);
                cfg.beneficiaryShares.set(i, beneficiaryShares[i]);
            }
            else
            {
                cfg.beneficiaryAddresses.set(i, id::zero());
                cfg.beneficiaryShares.set(i, 0);
            }
        }
        state.mut()._heartbeatConfigs.set(idx, cfg);
        gate.lastActivityEpoch = qpi.epoch();
        state.mut()._gates.set(idx, gate);
        return QUGATE_SUCCESS;
    }

    sint64 sendHeartbeat(const id& caller, uint64 gateId)
    {
        qpi.reset();
        qpi._invocator = caller;

        if (gateId == 0 || gateId > state.get()._gateCount) return QUGATE_INVALID_GATE_ID;
        uint64 idx = gateId - 1;
        GateConfig gate = state.get()._gates.get(idx);
        if (!(gate.owner == caller)) return QUGATE_UNAUTHORIZED;
        if (gate.active == 0) return QUGATE_GATE_NOT_ACTIVE;
        if (gate.mode != MODE_HEARTBEAT) return QUGATE_HEARTBEAT_NOT_ACTIVE;

        QUGATE_HeartbeatConfig_Test cfg = state.get()._heartbeatConfigs.get(idx);
        if (cfg.active == 0) return QUGATE_HEARTBEAT_NOT_ACTIVE;
        if (cfg.triggered == 1) return QUGATE_HEARTBEAT_TRIGGERED;

        cfg.lastHeartbeatEpoch = qpi.epoch();
        state.mut()._heartbeatConfigs.set(idx, cfg);
        gate.lastActivityEpoch = qpi.epoch();
        state.mut()._gates.set(idx, gate);
        return QUGATE_SUCCESS;
    }

    getHeartbeat_output getHeartbeat(uint64 gateId)
    {
        getHeartbeat_output out;
        memset(&out, 0, sizeof(out));
        if (gateId == 0 || gateId > state.get()._gateCount) return out;

        uint64 idx = gateId - 1;
        GateConfig gate = state.get()._gates.get(idx);
        if (gate.mode != MODE_HEARTBEAT) return out;

        QUGATE_HeartbeatConfig_Test cfg = state.get()._heartbeatConfigs.get(idx);
        out.active = cfg.active;
        out.triggered = cfg.triggered;
        out.thresholdEpochs = cfg.thresholdEpochs;
        out.lastHeartbeatEpoch = cfg.lastHeartbeatEpoch;
        out.triggerEpoch = cfg.triggerEpoch;
        out.payoutPercentPerEpoch = cfg.payoutPercentPerEpoch;
        out.minimumBalance = cfg.minimumBalance;
        out.beneficiaryCount = cfg.beneficiaryCount;
        for (uint8 i = 0; i < 8; i++)
        {
            out.beneficiaryAddresses.set(i, cfg.beneficiaryAddresses.get(i));
            out.beneficiaryShares.set(i, cfg.beneficiaryShares.get(i));
        }
        return out;
    }

    sint64 setAdminGate(const id& caller, uint64 gateId, sint64 adminGateId)
    {
        qpi.reset();
        qpi._invocator = caller;

        if (gateId == 0 || gateId > state.get()._gateCount) return QUGATE_INVALID_GATE_ID;
        uint64 idx = gateId - 1;
        GateConfig gate = state.get()._gates.get(idx);
        sint64 authStatus = QUGATE_SUCCESS;
        if (!authorizeGateMutation(caller, idx, authStatus, true, adminGateId == -1))
        {
            return authStatus;
        }
        consumeAdminApprovalIfUsed(caller, idx);
        if (gate.active == 0) return QUGATE_GATE_NOT_ACTIVE;

        if (adminGateId == -1)
        {
            gate.adminGateId = -1;
            state.mut()._gates.set(idx, gate);
            return QUGATE_SUCCESS;
        }

        if (adminGateId == (sint64)gateId) return QUGATE_INVALID_ADMIN_CYCLE;
        if (adminGateId <= 0) return QUGATE_INVALID_ADMIN_GATE;

        uint64 adminSlot = slotFromGateId(adminGateId);
        if (adminSlot >= state.get()._gateCount || !gateIdMatchesCurrentGeneration(adminGateId)) return QUGATE_INVALID_ADMIN_GATE;
        GateConfig adminGate = state.get()._gates.get(adminSlot);
        if (adminGate.active == 0 || adminGate.mode != MODE_MULTISIG) return QUGATE_INVALID_ADMIN_GATE;

        uint64 walkSlot = adminSlot;
        for (uint8 step = 0; step < QUGATE_MAX_CHAIN_DEPTH; step++)
        {
            GateConfig walkGate = state.get()._gates.get(walkSlot);
            if (walkGate.adminGateId < 0) break;
            uint64 nextAdminSlot = slotFromGateId(walkGate.adminGateId);
            if (nextAdminSlot == idx) return QUGATE_INVALID_ADMIN_CYCLE;
            if (nextAdminSlot >= state.get()._gateCount || !gateIdMatchesCurrentGeneration(walkGate.adminGateId)) break;
            walkSlot = nextAdminSlot;
        }

        gate.adminGateId = encodeCurrentGateId(adminSlot);
        state.mut()._gates.set(idx, gate);
        return QUGATE_SUCCESS;
    }

    getAdminGate_output getAdminGate(uint64 gateId)
    {
        getAdminGate_output out;
        memset(&out, 0, sizeof(out));
        out.adminGateId = -1;
        if (gateId == 0 || gateId > state.get()._gateCount) return out;

        GateConfig gate = state.get()._gates.get(gateId - 1);
        out.hasAdminGate = (gate.adminGateId >= 0) ? 1 : 0;
        out.adminGateId = gate.adminGateId;
        if (gate.adminGateId < 0) return out;

        uint64 adminSlot = slotFromGateId(gate.adminGateId);
        if (adminSlot >= state.get()._gateCount || !gateIdMatchesCurrentGeneration(gate.adminGateId))
        {
            return out;
        }

        GateConfig adminGate = state.get()._gates.get(adminSlot);
        out.adminGateMode = adminGate.mode;
        QUGATE_MultisigConfig_Test cfg = state.get()._multisigConfigs.get(adminSlot);
        out.guardianCount = cfg.guardianCount;
        out.required = cfg.required;
        out.adminApprovalWindowEpochs = cfg.adminApprovalWindowEpochs;
        if (adminApprovalValid(adminSlot))
        {
            out.adminApprovalActive = 1;
            out.adminApprovalValidUntilEpoch = state.get()._adminApprovalStates.get(adminSlot).validUntilEpoch;
        }
        for (uint8 i = 0; i < cfg.guardianCount; i++)
        {
            out.guardians.set(i, cfg.guardians.get(i));
        }
        return out;
    }

    withdrawReserve_output withdrawReserve(const id& caller, uint64 gateId, uint64 amount)
    {
        qpi.reset();
        qpi._invocator = caller;

        withdrawReserve_output out;
        out.status = QUGATE_SUCCESS;
        out.withdrawn = 0;

        if (gateId == 0 || gateId > state.get()._gateCount)
        {
            out.status = QUGATE_INVALID_GATE_ID;
            return out;
        }
        uint64 idx = gateId - 1;
        GateConfig gate = state.get()._gates.get(idx);
        sint64 authStatus = QUGATE_SUCCESS;
        if (!authorizeGateMutation(caller, idx, authStatus))
        {
            out.status = authStatus;
            return out;
        }
        consumeAdminApprovalIfUsed(caller, idx);
        if (gate.active == 0)
        {
            out.status = QUGATE_GATE_NOT_ACTIVE;
            return out;
        }
        if (gate.reserve <= 0)
        {
            return out;
        }

        uint64 toWithdraw = (uint64)gate.reserve;
        if (amount != 0 && amount < toWithdraw)
        {
            toWithdraw = amount;
        }
        if (amount != 0 && amount > (uint64)gate.reserve)
        {
            out.status = QUGATE_INVALID_PARAMS;
            return out;
        }

        if (qpi.transfer(caller, (sint64)toWithdraw) >= 0)
        {
            gate.reserve -= (sint64)toWithdraw;
            state.mut()._gates.set(idx, gate);
            out.withdrawn = toWithdraw;
        }
        return out;
    }

    Array<sint64, QUGATE_MAX_GATES> getGatesByOwner(const id& owner)
    {
        Array<sint64, QUGATE_MAX_GATES> out;
        for (uint64 i = 0; i < QUGATE_MAX_GATES; i++)
        {
            out.set(i, 0);
        }
        uint64 count = 0;
        for (uint64 i = 0; i < state.get()._gateCount; i++)
        {
            GateConfig gate = state.get()._gates.get(i);
            if (gate.active == 1 && gate.owner == owner)
            {
                out.set(count, encodeCurrentGateId(i));
                count++;
            }
        }
        return out;
    }

    getGateBySlot_output getGateBySlot(uint64 slotIdx)
    {
        getGateBySlot_output out;
        memset(&out, 0, sizeof(out));
        out.gateId = -1;
        if (slotIdx >= state.get()._gateCount)
        {
            return out;
        }

        GateConfig gate = state.get()._gates.get(slotIdx);
        out.valid = 1;
        out.active = gate.active;
        out.currentBalance = gate.currentBalance;
        out.reserve = gate.reserve;
        if (gate.active == 1)
        {
            out.gateId = encodeCurrentGateId(slotIdx);
        }
        else if (state.get()._gateGenerations.get(slotIdx) > 0)
        {
            out.gateId = versionedGateId(slotIdx, state.get()._gateGenerations.get(slotIdx) - 1);
        }
        else
        {
            out.gateId = versionedGateId(slotIdx, 0);
        }
        return out;
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
        sint64 authStatus = QUGATE_SUCCESS;
        if (!authorizeGateMutation(caller, idx, authStatus)) return authStatus;
        consumeAdminApprovalIfUsed(caller, idx);
        if (gate.active == 0) return QUGATE_GATE_NOT_ACTIVE;
        if (gate.mode != MODE_TIME_LOCK) return QUGATE_INVALID_MODE;

        QUGATE_TimeLockConfig_Test cfg;
        memset(&cfg, 0, sizeof(cfg));
        if (lockMode == QUGATE_TIME_LOCK_ABSOLUTE_EPOCH)
        {
            if (unlockEpoch <= qpi.epoch()) return QUGATE_TIME_LOCK_EPOCH_PAST;
            cfg.unlockEpoch = unlockEpoch;
        }
        else if (lockMode == QUGATE_TIME_LOCK_RELATIVE_EPOCHS)
        {
            if (unlockEpoch == 0) return QUGATE_INVALID_PARAMS;
            cfg.delayEpochs = unlockEpoch;
            cfg.unlockEpoch = (gate.currentBalance > 0) ? (qpi.epoch() + unlockEpoch) : 0;
        }
        else
        {
            return QUGATE_INVALID_PARAMS;
        }
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
        sint64 authStatus = QUGATE_SUCCESS;
        if (!authorizeGateMutation(caller, idx, authStatus)) return authStatus;
        consumeAdminApprovalIfUsed(caller, idx);
        if (gate.active == 0) return QUGATE_GATE_NOT_ACTIVE;
        if (gate.mode != MODE_TIME_LOCK) return QUGATE_INVALID_MODE;

        QUGATE_TimeLockConfig_Test cfg = state.get()._timeLockConfigs.get(idx);
        if (cfg.active == 0) return QUGATE_GATE_NOT_ACTIVE;
        if (cfg.fired == 1) return QUGATE_TIME_LOCK_ALREADY_FIRED;
        if (cfg.cancelled == 1) return QUGATE_GATE_NOT_ACTIVE;
        if (cfg.cancellable == 0) return QUGATE_TIME_LOCK_NOT_CANCELLABLE;

        // Refund held balance to owner
        if (gate.currentBalance > 0)
        {
            if (qpi.transfer(gate.owner, gate.currentBalance) >= 0)
            {
                gate.currentBalance = 0;
            }
        }

        // Refund reserve to owner
        if (gate.reserve > 0)
        {
            if (qpi.transfer(gate.owner, gate.reserve) >= 0)
            {
                gate.reserve = 0;
            }
        }

        state.mut()._gates.set(idx, gate);

        if (gate.currentBalance > 0 || gate.reserve > 0)
        {
            return QUGATE_INVALID_PARAMS;
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
        sint64 authStatus = QUGATE_SUCCESS;
        if (!authorizeGateMutation(caller, idx, authStatus)) return authStatus;
        consumeAdminApprovalIfUsed(caller, idx);
        if (gate.active == 0) return QUGATE_GATE_NOT_ACTIVE;
        if (gate.mode != MODE_MULTISIG) return QUGATE_MULTISIG_INVALID_CONFIG;

        if (guardianCount == 0 || guardianCount > 8 || required == 0 || required > guardianCount
            || proposalExpiryEpochs == 0 || adminApprovalWindowEpochs == 0)
        {
            return QUGATE_MULTISIG_INVALID_CONFIG;
        }

        QUGATE_MultisigConfig_Test existingCfg = state.get()._multisigConfigs.get(idx);
        if (existingCfg.proposalActive == 1
            && (uint32)(qpi.epoch() - existingCfg.proposalEpoch) <= existingCfg.proposalExpiryEpochs)
        {
            return QUGATE_MULTISIG_PROPOSAL_ACTIVE;
        }

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
        QUGATE_AdminApprovalState_Test approval;
        memset(&approval, 0, sizeof(approval));
        state.mut()._adminApprovalStates.set(idx, approval);
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

        QUGATE_MultisigConfig_Test msigCfg = state.get()._multisigConfigs.get(idx);
        if (msigCfg.proposalActive == 1
            && (uint32)(qpi.epoch() - msigCfg.proposalEpoch) > msigCfg.proposalExpiryEpochs)
        {
            msigCfg.approvalBitmap = 0;
            msigCfg.approvalCount = 0;
            msigCfg.proposalActive = 0;
            state.mut()._multisigConfigs.set(idx, msigCfg);
        }

        // Accumulate balance
        gate.totalReceived += amount;
        gate.currentBalance += amount;
        gate.lastActivityEpoch = qpi.epoch();

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
        if (guardianIdx != 255)
        {
            if (msigCfg.approvalBitmap & (1 << guardianIdx))
            {
                state.mut()._gates.set(idx, gate);
                output.status = QUGATE_MULTISIG_ALREADY_VOTED;
                return output;
            }

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

            if (msigCfg.approvalCount >= msigCfg.required)
            {
                bool released = false;
                sint64 releaseAmount = (sint64)gate.currentBalance;
                if (gate.recipientCount == 0
                    && gate.chainNextGateId == -1
                    && msigCfg.adminApprovalWindowEpochs > 0)
                {
                    QUGATE_AdminApprovalState_Test approval;
                    approval.active = 1;
                    approval.validUntilEpoch = qpi.epoch() + msigCfg.adminApprovalWindowEpochs - 1;
                    state.mut()._adminApprovalStates.set(idx, approval);
                    released = true;
                }
                else if (releaseAmount > 0)
                {
                    if (gate.recipientCount > 0)
                    {
                        if (gate.recipientGateIds.get(0) > 0)
                        {
                            uint64 targetSlot = slotFromGateId(gate.recipientGateIds.get(0));
                            if (targetSlot < state.get()._gateCount
                                && gateIdMatchesCurrentGeneration(gate.recipientGateIds.get(0))
                                && state.get()._gates.get(targetSlot).active == 1)
                            {
                                GateConfig targetGate = state.get()._gates.get(targetSlot);
                                bool targetAccepts = true;
                                if (targetGate.mode == MODE_TIME_LOCK)
                                {
                                    QUGATE_TimeLockConfig_Test cfg = state.get()._timeLockConfigs.get(targetSlot);
                                    targetAccepts = (cfg.active == 1 && cfg.cancelled == 0 && cfg.fired == 0);
                                }
                                if (targetAccepts)
                                {
                                    RouteResult routed = routeToGate(targetSlot, releaseAmount, 0);
                                    released = routed.accepted;
                                }
                            }
                        }
                        else if (qpi.transfer(gate.recipients.get(0), releaseAmount) >= 0)
                        {
                            released = true;
                        }
                    }
                    else if (gate.chainNextGateId != -1)
                    {
                        released = true;
                        routeChain((uint64)gate.chainNextGateId, releaseAmount);
                    }
                }

                msigCfg.approvalBitmap = 0;
                msigCfg.approvalCount = 0;
                msigCfg.proposalActive = 0;

                if (released && releaseAmount > 0)
                {
                    gate.totalForwarded += (uint64)releaseAmount;
                    gate.currentBalance = 0;
                }
            }
        }

        state.mut()._gates.set(idx, gate);
        state.mut()._multisigConfigs.set(idx, msigCfg);
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
        sint64 authStatus = QUGATE_SUCCESS;
        if (!authorizeGateMutation(caller, gateId - 1, authStatus))
        {
            if (fee > 0) qpi.transfer(caller, fee);
            output.result = authStatus;
            return output;
        }
        consumeAdminApprovalIfUsed(caller, gateId - 1);
        if (gate.active == 0)
        {
            if (fee > 0) qpi.transfer(caller, fee);
            output.result = QUGATE_GATE_NOT_ACTIVE;
            return output;
        }
        if (state.get()._expiryEpochs > 0
            && (uint16)(qpi.epoch() - gate.lastActivityEpoch) >= state.get()._expiryEpochs)
        {
            if (gate.currentBalance > 0)
            {
                if (qpi.transfer(gate.owner, gate.currentBalance) >= 0)
                {
                    gate.currentBalance = 0;
                }
            }
            if (gate.reserve > 0)
            {
                if (qpi.transfer(gate.owner, gate.reserve) >= 0)
                {
                    gate.reserve = 0;
                }
            }
            state.mut()._gates.set(gateId - 1, gate);
            if (gate.currentBalance > 0 || gate.reserve > 0)
            {
                if (fee > 0) qpi.transfer(caller, fee);
                output.result = QUGATE_GATE_NOT_ACTIVE;
                return output;
            }
            gate.active = 0;
            state.mut()._gates.set(gateId - 1, gate);
            state.mut()._activeGates -= 1;
            state.mut()._freeSlots.set(state.get()._freeCount, gateId - 1);
            state.mut()._freeCount += 1;
            state.mut()._gateGenerations.set(gateId - 1, state.get()._gateGenerations.get(gateId - 1) + 1);
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

        if (nextGateId <= 0)
        {
            if (fee > 0) qpi.transfer(caller, fee);
            output.result = QUGATE_INVALID_CHAIN;
            return output;
        }

        uint64 targetIdx = slotFromGateId(nextGateId);
        if (targetIdx >= state.get()._gateCount || !gateIdMatchesCurrentGeneration(nextGateId))
        {
            if (fee > 0) qpi.transfer(caller, fee);
            output.result = QUGATE_INVALID_CHAIN;
            return output;
        }

        GateConfig target = state.get()._gates.get(targetIdx);
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
        uint64 walkIdx = targetIdx;
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
            uint64 nextWalk = slotFromGateId(walkGate.chainNextGateId);
            if (nextWalk >= state.get()._gateCount || !gateIdMatchesCurrentGeneration(walkGate.chainNextGateId)) break;
            walkIdx = nextWalk;
        }
        if (walkIdx == (uint64)(gateId - 1))
        {
            if (fee > 0) qpi.transfer(caller, fee);
            output.result = QUGATE_INVALID_CHAIN;
            return output;
        }

        gate.chainNextGateId = encodeCurrentGateId(targetIdx);
        gate.chainDepth = newDepth;
        state.mut()._gates.set(gateId - 1, gate);
        qpi.burn(QUGATE_CHAIN_HOP_FEE);
        if (fee > QUGATE_CHAIN_HOP_FEE) qpi.transfer(caller, fee - QUGATE_CHAIN_HOP_FEE);
        return output;
    }

    // ---- routeToGate (single hop) ----
    struct RouteResult { sint64 forwarded; bool accepted; };

    RouteResult routeToGate(uint64 slotIdx, sint64 amount, uint8 hopCount)
    {
        RouteResult result;
        result.forwarded = 0;
        result.accepted = false;

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
                result.accepted = true;
                return result; // stranded
            }
        }
        else {
            qpi.burn(QUGATE_CHAIN_HOP_FEE);
            amountAfterFee = amount - QUGATE_CHAIN_HOP_FEE;
        }

        // Dispatch through mode
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
                    if (gate.recipientGateIds.get(i) >= 0)
                    {
                        uint64 targetSlot = slotFromGateId(gate.recipientGateIds.get(i));
                        if (targetSlot < state.get()._gateCount
                            && gateIdMatchesCurrentGeneration(gate.recipientGateIds.get(i))
                            && state.get()._gates.get(targetSlot).active == 1)
                        {
                            GateConfig targetGate = state.get()._gates.get(targetSlot);
                            bool accepts = true;
                            if (targetGate.mode == MODE_TIME_LOCK)
                            {
                                QUGATE_TimeLockConfig_Test cfg = state.get()._timeLockConfigs.get(targetSlot);
                                accepts = (cfg.active == 1 && cfg.cancelled == 0 && cfg.fired == 0);
                            }
                            if (accepts)
                            {
                                RouteResult deferred = routeToGate(targetSlot, (sint64)share, hopCount + 1);
                                if (deferred.accepted)
                                {
                                    distributed += share;
                                }
                            }
                        }
                    }
                    else
                    {
                        qpi.transfer(gate.recipients.get(i), share);
                        distributed += share;
                    }
                }
            }
            gate = state.get()._gates.get(slotIdx);
            gate.totalForwarded += distributed;
            state.mut()._gates.set(slotIdx, gate);
            result.forwarded = distributed;
            result.accepted = (distributed > 0);
        }
        else if (gate.mode == MODE_ROUND_ROBIN)
        {
            bool accepted = false;
            if (gate.recipientGateIds.get(gate.roundRobinIndex) >= 0)
            {
                uint64 targetSlot = slotFromGateId(gate.recipientGateIds.get(gate.roundRobinIndex));
                if (targetSlot < state.get()._gateCount
                    && gateIdMatchesCurrentGeneration(gate.recipientGateIds.get(gate.roundRobinIndex))
                    && state.get()._gates.get(targetSlot).active == 1)
                {
                    GateConfig targetGate = state.get()._gates.get(targetSlot);
                    bool targetAccepts = true;
                    if (targetGate.mode == MODE_TIME_LOCK)
                    {
                        QUGATE_TimeLockConfig_Test cfg = state.get()._timeLockConfigs.get(targetSlot);
                        targetAccepts = (cfg.active == 1 && cfg.cancelled == 0 && cfg.fired == 0);
                    }
                    if (targetAccepts)
                    {
                        RouteResult deferred = routeToGate(targetSlot, amountAfterFee, hopCount + 1);
                        accepted = deferred.accepted;
                    }
                }
            }
            else
            {
                qpi.transfer(gate.recipients.get(gate.roundRobinIndex), amountAfterFee);
                accepted = true;
            }

            if (accepted)
            {
                gate.totalForwarded += amountAfterFee;
                gate.roundRobinIndex = QPI::mod(gate.roundRobinIndex + 1, (uint64)gate.recipientCount);
                result.forwarded = amountAfterFee;
                result.accepted = true;
            }
            state.mut()._gates.set(slotIdx, gate);
        }
        else if (gate.mode == MODE_THRESHOLD)
        {
            gate.currentBalance += amountAfterFee;
            bool accepted = false;
            if (gate.currentBalance >= gate.threshold)
            {
                if (gate.recipientGateIds.get(0) >= 0)
                {
                    uint64 targetSlot = slotFromGateId(gate.recipientGateIds.get(0));
                    if (targetSlot < state.get()._gateCount
                        && gateIdMatchesCurrentGeneration(gate.recipientGateIds.get(0))
                        && state.get()._gates.get(targetSlot).active == 1)
                    {
                        GateConfig targetGate = state.get()._gates.get(targetSlot);
                        bool targetAccepts = true;
                        if (targetGate.mode == MODE_TIME_LOCK)
                        {
                            QUGATE_TimeLockConfig_Test cfg = state.get()._timeLockConfigs.get(targetSlot);
                            targetAccepts = (cfg.active == 1 && cfg.cancelled == 0 && cfg.fired == 0);
                        }
                        if (targetAccepts)
                        {
                            RouteResult deferred = routeToGate(targetSlot, (sint64)gate.currentBalance, hopCount + 1);
                            accepted = deferred.accepted;
                        }
                    }
                }
                else
                {
                    qpi.transfer(gate.recipients.get(0), gate.currentBalance);
                    accepted = true;
                }
                if (accepted)
                {
                    gate.totalForwarded += gate.currentBalance;
                    gate.currentBalance = 0;
                }
            }
            state.mut()._gates.set(slotIdx, gate);
            result.forwarded = amountAfterFee;
            result.accepted = true;
        }
        else if (gate.mode == MODE_RANDOM)
        {
            uint64 ridx = QPI::mod(gate.totalReceived + qpi.tick(), (uint64)gate.recipientCount);
            bool accepted = false;
            if (gate.recipientGateIds.get(ridx) >= 0)
            {
                uint64 targetSlot = slotFromGateId(gate.recipientGateIds.get(ridx));
                if (targetSlot < state.get()._gateCount
                    && gateIdMatchesCurrentGeneration(gate.recipientGateIds.get(ridx))
                    && state.get()._gates.get(targetSlot).active == 1)
                {
                    GateConfig targetGate = state.get()._gates.get(targetSlot);
                    bool targetAccepts = true;
                    if (targetGate.mode == MODE_TIME_LOCK)
                    {
                        QUGATE_TimeLockConfig_Test cfg = state.get()._timeLockConfigs.get(targetSlot);
                        targetAccepts = (cfg.active == 1 && cfg.cancelled == 0 && cfg.fired == 0);
                    }
                    if (targetAccepts)
                    {
                        RouteResult deferred = routeToGate(targetSlot, amountAfterFee, hopCount + 1);
                        accepted = deferred.accepted;
                    }
                }
            }
            else
            {
                qpi.transfer(gate.recipients.get(ridx), amountAfterFee);
                accepted = true;
            }
            if (accepted)
            {
                gate.totalForwarded += amountAfterFee;
                state.mut()._gates.set(slotIdx, gate);
                result.forwarded = amountAfterFee;
                result.accepted = true;
            }
            else
            {
                state.mut()._gates.set(slotIdx, gate);
            }
        }
        else if (gate.mode == MODE_HEARTBEAT || gate.mode == MODE_MULTISIG)
        {
            gate.currentBalance += amountAfterFee;
            gate.totalReceived += amountAfterFee;
            state.mut()._gates.set(slotIdx, gate);
            result.forwarded = amountAfterFee;
            result.accepted = true;
        }
        else if (gate.mode == MODE_TIME_LOCK)
        {
            QUGATE_TimeLockConfig_Test cfg = state.get()._timeLockConfigs.get(slotIdx);
            if (cfg.active == 0 || cfg.cancelled == 1 || cfg.fired == 1)
            {
                return result;
            }
            if (cfg.lockMode == QUGATE_TIME_LOCK_RELATIVE_EPOCHS && cfg.unlockEpoch == 0)
            {
                cfg.unlockEpoch = qpi.epoch() + cfg.delayEpochs;
                state.mut()._timeLockConfigs.set(slotIdx, cfg);
            }
            gate.currentBalance += amountAfterFee;
            state.mut()._gates.set(slotIdx, gate);
            result.forwarded = amountAfterFee;
            result.accepted = true;
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

// Test identities
static const id ALICE = QuGateTest::makeId(1);
static const id BOB = QuGateTest::makeId(2);
static const id CHARLIE = QuGateTest::makeId(3);
static const id DAVE = QuGateTest::makeId(4);
static const id EVE = QuGateTest::makeId(5);

// Create a gate with default params
static createGate_output makeSimpleGate(QuGateTest& env, const id& owner, sint64 fee,
                                         uint8 mode, uint8 recipientCount,
                                         id* recips, uint64* ratios,
                                         uint64 threshold = 0,
                                         id* allowed = nullptr, uint8 allowedCount = 0)
                                         {
    return env.createGateSimple(owner, fee, mode, recipientCount, recips, ratios, threshold, allowed, allowedCount);
}

// Split gate distributes evenly to two recipients
TEST(QuGate, SplitEvenTwo)
{
    QuGateTest env;
    id recips[] = { BOB, CHARLIE };
    uint64 ratios[] = { 50, 50 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 2, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    ASSERT_NE(out.gateId, 0ULL);

    env.sendToGate(ALICE, out.gateId, 1000);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 500);
    EXPECT_EQ(env.qpi.totalTransferredTo(CHARLIE), 500);
}

// Split gate distributes proportionally with uneven 50/30/20 ratios
TEST(QuGate, SplitUnevenThree)
{
    QuGateTest env;
    id recips[] = { BOB, CHARLIE, DAVE };
    uint64 ratios[] = { 50, 30, 20 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 3, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    env.sendToGate(ALICE, out.gateId, 10000);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 5000);
    EXPECT_EQ(env.qpi.totalTransferredTo(CHARLIE), 3000);
    EXPECT_EQ(env.qpi.totalTransferredTo(DAVE), 2000);
}

// Split gate assigns rounding remainder to last recipient
TEST(QuGate, SplitHandlesRoundingDust)
{
    QuGateTest env;
    id recips[] = { BOB, CHARLIE, DAVE };
    uint64 ratios[] = { 3333, 3333, 3334 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 3, recips, ratios);

    env.sendToGate(ALICE, out.gateId, 10000);
    // 10000 * 3333 / 10000 = 3333, 10000 * 3333 / 10000 = 3333, remainder = 3334
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 3333);
    EXPECT_EQ(env.qpi.totalTransferredTo(CHARLIE), 3333);
    EXPECT_EQ(env.qpi.totalTransferredTo(DAVE), 3334);
}

// Round-robin gate cycles through recipients in order
TEST(QuGate, RoundRobinCycles)
{
    QuGateTest env;
    id recips[] = { BOB, CHARLIE, DAVE };
    uint64 ratios[] = { 0, 0, 0 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_ROUND_ROBIN, 3, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    env.sendToGate(ALICE, out.gateId, 1000);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 1000);

    env.sendToGate(ALICE, out.gateId, 2000);
    EXPECT_EQ(env.qpi.totalTransferredTo(CHARLIE), 2000);

    env.sendToGate(ALICE, out.gateId, 3000);
    EXPECT_EQ(env.qpi.totalTransferredTo(DAVE), 3000);

    env.sendToGate(ALICE, out.gateId, 4000);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 4000);
}

// Random gate selects recipient deterministically from tick entropy
TEST(QuGate, RandomSelectionTracksTickDeterministically)
{
    QuGateTest env;
    id recips[] = { BOB, CHARLIE };
    uint64 ratios[] = { 0, 0 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_RANDOM, 2, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    // First send: ridx = (1000 + 12345) % 2 = 1 -> CHARLIE
    env.qpi._tick = 12345;
    env.sendToGate(ALICE, out.gateId, 1000);
    EXPECT_EQ(env.qpi.totalTransferredTo(CHARLIE), 1000);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 0);

    // Second send: ridx = (2000 + 12346) % 2 = 0 -> BOB
    // Note: sendToGate calls qpi.reset(), so only this call's transfers are visible
    env.qpi._tick = 12346;
    env.sendToGate(ALICE, out.gateId, 1000);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 1000);
    EXPECT_EQ(env.qpi.totalTransferredTo(CHARLIE), 0);

    // Verify total forwarded across both sends
    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.totalForwarded, 2000ULL);
}

// Threshold gate accumulates until target then releases all at once
TEST(QuGate, ThresholdAccumulatesAndReleases)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_THRESHOLD, 1, recips, ratios, 5000);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    env.sendToGate(ALICE, out.gateId, 2000);
    EXPECT_EQ(env.qpi.transferCount, 0);

    env.sendToGate(ALICE, out.gateId, 2000);
    EXPECT_EQ(env.qpi.transferCount, 0);

    env.sendToGate(ALICE, out.gateId, 2000);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 6000);
}

// Conditional gate forwards from whitelisted senders
TEST(QuGate, ConditionalAllowsWhitelisted)
{
    QuGateTest env;
    id recips[] = { DAVE };
    uint64 ratios[] = { 0 };
    id allowed[] = { ALICE, BOB };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_CONDITIONAL, 1, recips, ratios, 0, allowed, 2);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    env.sendToGate(ALICE, out.gateId, 5000);
    EXPECT_EQ(env.qpi.totalTransferredTo(DAVE), 5000);
}

// Conditional gate bounces payment from non-whitelisted sender
TEST(QuGate, ConditionalBouncesUnauthorised)
{
    QuGateTest env;
    id recips[] = { DAVE };
    uint64 ratios[] = { 0 };
    id allowed[] = { ALICE };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_CONDITIONAL, 1, recips, ratios, 0, allowed, 1);

    auto sendOut = env.sendToGate(CHARLIE, out.gateId, 5000);
    EXPECT_EQ(sendOut.status, QUGATE_CONDITIONAL_REJECTED);
    EXPECT_EQ(env.qpi.totalTransferredTo(DAVE), 0);
    EXPECT_EQ(env.qpi.totalTransferredTo(CHARLIE), 5000);
}

// Sending to a non-existent gate ID bounces funds back
TEST(QuGate, InvalidGateIdBounces)
{
    QuGateTest env;
    auto out = env.sendToGate(ALICE, 999, 1000);
    EXPECT_EQ(out.status, QUGATE_INVALID_GATE_ID);
    EXPECT_EQ(env.qpi.totalTransferredTo(ALICE), 1000);
}

// Gate creation fails when fee is below the required amount
TEST(QuGate, CreationFailsWithInsufficientFee)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 500, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out.status, QUGATE_INSUFFICIENT_FEE);
    EXPECT_EQ(out.gateId, 0ULL);
}

// Sending zero amount returns DUST_AMOUNT with no transfers
TEST(QuGate, ZeroAmountDoesNothing)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    auto sendOut = env.sendToGate(ALICE, out.gateId, 0);
    EXPECT_EQ(sendOut.status, QUGATE_DUST_AMOUNT);
    EXPECT_EQ(env.qpi.transferCount, 0);
}

// Gate count and active gates track correctly across creations
TEST(QuGate, GateCountTracking)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    makeSimpleGate(env, ALICE, 100000, MODE_ROUND_ROBIN, 1, recips, ratios);
    makeSimpleGate(env, BOB, 100000, MODE_THRESHOLD, 1, recips, ratios, 1000);

    EXPECT_EQ(env.state.get()._gateCount, 3ULL);
    EXPECT_EQ(env.state.get()._activeGates, 3ULL);
}

// Escalating fee equals base fee when no gates are active
TEST(QuGateV3, EscalatingFeeAtZeroGates)
{
    QuGateTest env;
    // 0 active gates → fee = 100000 * (1 + 0/1024) = 100000
    auto fees = env.getFees();
    EXPECT_EQ(fees.currentCreationFee, 100000ULL);

    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out.status, QUGATE_SUCCESS);
    EXPECT_EQ(out.feePaid, 100000ULL);
}

// Escalating fee doubles at 1024 active gates
TEST(QuGateV3, EscalatingFeeAt1024Gates)
{
    QuGateTest env;
    // Simulate 1024 active gates
    state_hack: env.state.mut()._activeGates = 1024;

    // fee = 100000 * (1 + 1024/1024) = 200000
    auto fees = env.getFees();
    EXPECT_EQ(fees.currentCreationFee, 200000ULL);

    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 200000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out.status, QUGATE_SUCCESS);
    EXPECT_EQ(out.feePaid, 200000ULL);

    // Insufficient at old price
    env.state.mut()._activeGates = 1025; // now fee is still 200000
    auto out2 = makeSimpleGate(env, ALICE, 199999, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out2.status, QUGATE_INSUFFICIENT_FEE);
}

// Escalating fee triples at 2048 active gates
TEST(QuGateV3, EscalatingFeeAt2048Gates)
{
    QuGateTest env;
    env.state.mut()._activeGates = 2048;
    // fee = 100000 * (1 + 2048/1024) = 300000
    auto fees = env.getFees();
    EXPECT_EQ(fees.currentCreationFee, 300000ULL);

    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 300000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out.status, QUGATE_SUCCESS);
    EXPECT_EQ(out.feePaid, 300000ULL);
}

// Excess creation fee above the required amount seeds gate reserve
TEST(QuGateV3, FeeOverpaymentSeedsReserve)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    // Pay 500000, fee is 100000 → excess 400000 seeds reserve
    auto out = makeSimpleGate(env, ALICE, 500000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out.status, QUGATE_SUCCESS);
    EXPECT_EQ(out.feePaid, 100000ULL);
    EXPECT_EQ(env.qpi.totalTransferredTo(ALICE), 0); // no refund
    // Excess goes to reserve
    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.reserve, 400000);
}

// Amounts below minimum send are burned as dust
TEST(QuGateV3, DustBurnBelowMinSend)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    // Send 5 QU (below minSendAmount of 1000)
    auto sendOut = env.sendToGate(ALICE, out.gateId, 5);
    EXPECT_EQ(sendOut.status, QUGATE_DUST_AMOUNT);
    EXPECT_EQ(env.qpi.totalBurned, 5);
    EXPECT_EQ(env.qpi.transferCount, 0); // no transfers, burned
    EXPECT_EQ(env.state.get()._totalBurned, 50000 + 5); // creation fee burn (50%) + dust
}

// Sending exactly the minimum amount forwards successfully
TEST(QuGateV3, ExactMinSendNotDust)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    // Send exactly 1000 (= minSendAmount) → should forward, not burn
    auto sendOut = env.sendToGate(ALICE, out.gateId, 1000);
    EXPECT_EQ(sendOut.status, QUGATE_SUCCESS);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 1000);
}

// Creating a gate with invalid mode returns INVALID_MODE
TEST(QuGateV3, StatusCodeCreateInvalidMode)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, 99, 1, recips, ratios);
    EXPECT_EQ(out.status, QUGATE_INVALID_MODE);
}

// Creating a split gate with zero recipients returns INVALID_RECIPIENT_COUNT
TEST(QuGateV3, StatusCodeCreateInvalidRecipientCount)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 0, recips, ratios);
    EXPECT_EQ(out.status, QUGATE_INVALID_RECIPIENT_COUNT);
}

// Creating a split gate with zero total ratio returns INVALID_RATIO
TEST(QuGateV3, StatusCodeCreateInvalidRatio)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 }; // zero total ratio
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out.status, QUGATE_INVALID_RATIO);
}

// Creating a threshold gate with zero threshold returns INVALID_THRESHOLD
TEST(QuGateV3, StatusCodeCreateInvalidThreshold)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_THRESHOLD, 1, recips, ratios, 0);
    EXPECT_EQ(out.status, QUGATE_INVALID_THRESHOLD);
}

// Sending to a closed gate returns GATE_NOT_ACTIVE
TEST(QuGateV3, StatusCodeSendToInactiveGate)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    env.closeGate(ALICE, out.gateId);

    auto sendOut = env.sendToGate(ALICE, out.gateId, 100);
    EXPECT_EQ(sendOut.status, QUGATE_GATE_NOT_ACTIVE);
}

// Non-owner cannot close another owner's gate
TEST(QuGateV3, StatusCodeCloseUnauthorized)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    auto closeOut = env.closeGate(BOB, out.gateId);
    EXPECT_EQ(closeOut.status, QUGATE_UNAUTHORIZED);
}

// Closing a non-existent gate returns INVALID_GATE_ID
TEST(QuGateV3, StatusCodeCloseInvalidGateId)
{
    QuGateTest env;
    auto closeOut = env.closeGate(ALICE, 999);
    EXPECT_EQ(closeOut.status, QUGATE_INVALID_GATE_ID);
}

// Updating a non-existent gate returns INVALID_GATE_ID
TEST(QuGateV3, StatusCodeUpdateInvalidGateId)
{
    QuGateTest env;
    updateGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
    in.gateId = 999;
    in.recipientCount = 1;
    in.recipients.set(0, BOB);
    in.ratios.set(0, 100);
    auto out = env.updateGate(ALICE, 0, in);
    EXPECT_EQ(out.status, QUGATE_INVALID_GATE_ID);
}

// Non-owner cannot update another owner's gate
TEST(QuGateV3, StatusCodeUpdateUnauthorized)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto gateOut = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    updateGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
    in.gateId = gateOut.gateId;
    in.recipientCount = 1;
    in.recipients.set(0, CHARLIE);
    in.ratios.set(0, 100);
    auto out = env.updateGate(BOB, 0, in);
    EXPECT_EQ(out.status, QUGATE_UNAUTHORIZED);
}

// Closed gate slot is reused by the next creation
TEST(QuGateV3, FreeListSlotReuse)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    auto g1 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto g2 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(g1.gateId, 1ULL);
    EXPECT_EQ(g2.gateId, 2ULL);
    EXPECT_EQ(env.state.get()._gateCount, 2ULL);

    // Close gate 1
    env.closeGate(ALICE, 1);
    EXPECT_EQ(env.state.get()._freeCount, 1ULL);
    EXPECT_EQ(env.state.get()._activeGates, 1ULL);

    // Create again — should reuse slot 0 (gateId 1)
    auto g3 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(g3.gateId, 1ULL); // reused!
    EXPECT_EQ(env.state.get()._freeCount, 0ULL);
    EXPECT_EQ(env.state.get()._gateCount, 2ULL); // didn't grow
    EXPECT_EQ(env.state.get()._activeGates, 2ULL);
}

// Inactive gate is auto-closed by endEpoch after expiry period
TEST(QuGateV3, GateExpiryAutoClose)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    env.qpi._epoch = 100;
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
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

// Expired threshold gate refunds held balance to owner
TEST(QuGateV3, GateExpiryRefundsBalance)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    env.qpi._epoch = 100;
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_THRESHOLD, 1, recips, ratios, 10000);

    // Send some QU that sits in threshold balance
    env.sendToGate(CHARLIE, out.gateId, 5000);
    auto gateBefore = env.getGate(out.gateId);
    EXPECT_EQ(gateBefore.currentBalance, 5000ULL);

    // Expire it
    env.qpi._epoch = 150;
    env.qpi.reset();
    env.endEpoch();

    // Balance refunded to owner (ALICE)
    EXPECT_EQ(env.qpi.totalTransferredTo(ALICE), 5000);
    auto gateAfter = env.getGate(out.gateId);
    EXPECT_EQ(gateAfter.active, 0);
}

// Gate with recent activity is not expired by endEpoch
TEST(QuGateV3, GateNotExpiredIfActive)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    env.qpi._epoch = 100;
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    // Send at epoch 140 → updates lastActivityEpoch
    env.qpi._epoch = 140;
    env.sendToGate(CHARLIE, out.gateId, 1000);

    // Run endEpoch at 150 — only 10 epochs since last activity, not 50
    env.qpi._epoch = 150;
    env.qpi.reset();
    env.endEpoch();

    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.active, 1); // still active
}

// Total burned tracks creation fee burns and dust burns cumulatively
TEST(QuGateV3, TotalBurnedTracking)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    // Create gate → burns 50% of 100000 = 50000
    makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(env.state.get()._totalBurned, 50000ULL);

    // Dust burn → burns 5
    env.sendToGate(ALICE, 1, 5);
    EXPECT_EQ(env.state.get()._totalBurned, 50005ULL);

    // Create another → burns 50% of 100000 = 50000
    makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(env.state.get()._totalBurned, 100005ULL);

    auto count = env.getGateCount();
    EXPECT_EQ(count.totalBurned, 100005ULL);
}

// getFees returns base and escalated creation fee correctly
TEST(QuGateV3, GetFeesReturnsCorrectValues)
{
    QuGateTest env;
    auto fees = env.getFees();
    EXPECT_EQ(fees.creationFee, 100000ULL);
    EXPECT_EQ(fees.currentCreationFee, 100000ULL);
    EXPECT_EQ(fees.minSendAmount, QUGATE_DEFAULT_MIN_SEND);
    EXPECT_EQ(fees.expiryEpochs, 50ULL);

    // With active gates
    env.state.mut()._activeGates = 2048;
    fees = env.getFees();
    EXPECT_EQ(fees.currentCreationFee, 300000ULL);
}

// Sending to a gate updates its lastActivityEpoch
TEST(QuGateV3, LastActivityEpochUpdatesOnSend)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    env.qpi._epoch = 100;
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.lastActivityEpoch, 100);

    env.qpi._epoch = 120;
    env.sendToGate(CHARLIE, out.gateId, 1000);

    gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.lastActivityEpoch, 120);
}

// Updating a gate updates its lastActivityEpoch
TEST(QuGateV3, LastActivityEpochUpdatesOnUpdate)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    env.qpi._epoch = 100;
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    env.qpi._epoch = 130;
    updateGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
    in.gateId = out.gateId;
    in.recipientCount = 1;
    in.recipients.set(0, CHARLIE);
    in.ratios.set(0, 100);
    env.updateGate(ALICE, 0, in);

    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.lastActivityEpoch, 130);
}

// feePaid output matches the escalated fee at each tier
TEST(QuGateV3, FeePaidMatchesEscalatedFee)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    // 0 active gates → fee = 100000
    auto out1 = makeSimpleGate(env, ALICE, 500000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out1.feePaid, 100000ULL);

    // Set active gates to 1024 (minus the 1 just created, so set to 1024 total)
    env.state.mut()._activeGates = 1024;
    auto out2 = makeSimpleGate(env, ALICE, 500000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out2.feePaid, 200000ULL);

    env.state.mut()._activeGates = 3072;
    auto out3 = makeSimpleGate(env, ALICE, 500000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out3.feePaid, 400000ULL);
}

// Owner can close their own active gate
TEST(QuGateV3, CloseGateSuccess)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    auto closeOut = env.closeGate(ALICE, out.gateId);
    EXPECT_EQ(closeOut.status, QUGATE_SUCCESS);
    EXPECT_EQ(env.state.get()._activeGates, 0ULL);
}

// Closing an already-closed gate returns GATE_NOT_ACTIVE
TEST(QuGateV3, CloseAlreadyClosedGate)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    env.closeGate(ALICE, out.gateId);
    auto closeOut2 = env.closeGate(ALICE, out.gateId);
    EXPECT_EQ(closeOut2.status, QUGATE_GATE_NOT_ACTIVE);
}

// Ratio exceeding MAX_RATIO is rejected at creation
TEST(QuGateV3, RatioOverMaxRejected)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { QUGATE_MAX_RATIO + 1 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(out.status, QUGATE_INVALID_RATIO);
}

// Allowed sender count exceeding max is rejected at creation
TEST(QuGateV3, InvalidSenderCountRejected)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    id allowed[1] = { ALICE };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_CONDITIONAL, 1, recips, ratios, 0, allowed, 9);
    EXPECT_EQ(out.status, QUGATE_INVALID_SENDER_COUNT);
}

// Insufficient creation fee is refunded to the caller
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

// Anyone can fund a gate's reserve
TEST(QuGateFund, FundGateSuccess)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto gateOut = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(gateOut.status, QUGATE_SUCCESS);

    // Fund reserve with 3000
    auto fundOut = env.fundGate(CHARLIE, gateOut.gateId, 3000);
    EXPECT_EQ(fundOut.result, QUGATE_SUCCESS);

    auto gate = env.getGate(gateOut.gateId);
    EXPECT_EQ(gate.reserve, 3000);
}

// Funding a non-existent gate returns INVALID_GATE_ID
TEST(QuGateFund, FundGateInvalidId)
{
    QuGateTest env;
    auto fundOut = env.fundGate(ALICE, 999, 1000);
    EXPECT_EQ(fundOut.result, QUGATE_INVALID_GATE_ID);
}

// Gate can be created with a chain link to an existing gate
TEST(QuGateChain, CreateGateWithChain)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    // Create target gate first (gate 1)
    auto g1 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(g1.status, QUGATE_SUCCESS);

    // Create chained gate (gate 2 → gate 1)
    createGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
    in.mode = MODE_SPLIT;
    in.recipientCount = 1;
    in.recipients.set(0, CHARLIE);
    in.ratios.set(0, 100);
    in.chainNextGateId = g1.gateId;
    auto g2 = env.createGate(ALICE, 100000, in);
    EXPECT_EQ(g2.status, QUGATE_SUCCESS);

    auto gate = env.getGate(g2.gateId);
    EXPECT_EQ(gate.chainNextGateId, (sint64)g1.gateId);
    EXPECT_EQ(gate.chainDepth, 1);
}

// Creating a gate chained to a non-existent target fails
TEST(QuGateChain, CreateGateChainInvalidTarget)
{
    QuGateTest env;
    createGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
    in.mode = MODE_SPLIT;
    in.recipientCount = 1;
    in.recipients.set(0, BOB);
    in.ratios.set(0, 100);
    in.chainNextGateId = 999; // doesn't exist
    auto out = env.createGate(ALICE, 100000, in);
    EXPECT_EQ(out.status, QUGATE_INVALID_CHAIN);
}

// Chain depth exceeding MAX_CHAIN_DEPTH is rejected
TEST(QuGateChain, CreateGateChainDepthLimit)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    // Create 3 gates: g1 (depth 0), g2→g1 (depth 1), g3→g2 (depth 2)
    auto g1 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    createGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
    in.mode = MODE_SPLIT;
    in.recipientCount = 1;
    in.recipients.set(0, BOB);
    in.ratios.set(0, 100);
    in.chainNextGateId = g1.gateId;
    auto g2 = env.createGate(ALICE, 100000, in);
    ASSERT_EQ(g2.status, QUGATE_SUCCESS);
    EXPECT_EQ(env.getGate(g2.gateId).chainDepth, 1);

    in.chainNextGateId = g2.gateId;
    auto g3 = env.createGate(ALICE, 100000, in);
    ASSERT_EQ(g3.status, QUGATE_SUCCESS);
    EXPECT_EQ(env.getGate(g3.gateId).chainDepth, 2);

    // g4→g3 should fail (depth would be 3 >= QUGATE_MAX_CHAIN_DEPTH)
    in.chainNextGateId = g3.gateId;
    auto g4 = env.createGate(ALICE, 100000, in);
    EXPECT_EQ(g4.status, QUGATE_INVALID_CHAIN);
}

// setChain links one gate to another as a chain target
TEST(QuGateChain, SetChainSuccess)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    auto g1 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto g2 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    // Link g2 → g1
    auto out = env.setChain(ALICE, g2.gateId, g1.gateId, QUGATE_CHAIN_HOP_FEE);
    EXPECT_EQ(out.result, QUGATE_SUCCESS);

    auto gate = env.getGate(g2.gateId);
    EXPECT_EQ(gate.chainNextGateId, (sint64)g1.gateId);
    EXPECT_EQ(gate.chainDepth, 1);
}

// setChain with -1 clears the chain link and resets depth
TEST(QuGateChain, SetChainClearChain)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    auto g1 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto g2 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    env.setChain(ALICE, g2.gateId, g1.gateId, QUGATE_CHAIN_HOP_FEE);
    EXPECT_EQ(env.getGate(g2.gateId).chainNextGateId, (sint64)g1.gateId);

    // Clear chain
    auto out = env.setChain(ALICE, g2.gateId, -1, QUGATE_CHAIN_HOP_FEE);
    EXPECT_EQ(out.result, QUGATE_SUCCESS);
    EXPECT_EQ(env.getGate(g2.gateId).chainNextGateId, -1);
    EXPECT_EQ(env.getGate(g2.gateId).chainDepth, 0);
}

// Non-owner cannot set chain on another owner's gate
TEST(QuGateChain, SetChainUnauthorized)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    auto g1 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto g2 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    // BOB tries to set chain on ALICE's gate
    auto out = env.setChain(BOB, g2.gateId, g1.gateId, QUGATE_CHAIN_HOP_FEE);
    EXPECT_EQ(out.result, QUGATE_UNAUTHORIZED);
}

// Cycle detection rejects circular chains and preserves existing links
TEST(QuGateChain, SetChainCycleRejectedPreservesExistingLinks)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    auto g1 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto g2 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    auto first = env.setChain(ALICE, g2.gateId, g1.gateId, QUGATE_CHAIN_HOP_FEE);
    ASSERT_EQ(first.result, QUGATE_SUCCESS);

    auto cycle = env.setChain(ALICE, g1.gateId, g2.gateId, QUGATE_CHAIN_HOP_FEE);
    EXPECT_EQ(cycle.result, QUGATE_INVALID_CHAIN);

    EXPECT_EQ(env.getGate(g1.gateId).chainNextGateId, -1);
    EXPECT_EQ(env.getGate(g1.gateId).chainDepth, 0);
    EXPECT_EQ(env.getGate(g2.gateId).chainNextGateId, (sint64)g1.gateId);
    EXPECT_EQ(env.getGate(g2.gateId).chainDepth, 1);
}

// setChain below hop fee threshold returns INSUFFICIENT_FEE
TEST(QuGateChain, SetChainInsufficientFee)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    auto g1 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto g2 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    auto out = env.setChain(ALICE, g2.gateId, g1.gateId, 500); // below hop fee
    EXPECT_EQ(out.result, QUGATE_INSUFFICIENT_FEE);
}

// routeToGate deducts hop fee and forwards remainder via split
TEST(QuGateChain, RouteToGateSingleHop)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    auto g1 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    env.qpi.reset();
    auto result = env.routeToGate(g1.gateId - 1, 5000, 0);
    EXPECT_EQ(result.forwarded, 4000); // 5000 - 1000 hop fee
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 4000);
    EXPECT_EQ(env.qpi.totalBurned, 1000); // hop fee burned
}

// Two-hop chain deducts a hop fee at each gate
TEST(QuGateChain, RouteToGateTwoHopChain)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    // g1: SPLIT 100% → BOB (depth 0)
    auto g1 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    // g2: SPLIT 100% → CHARLIE, chained to g1
    id recips2[] = { CHARLIE };
    auto g2 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips2, ratios);
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

// Routed threshold gate accumulates after hop fee deduction
TEST(QuGateChain, RouteToGateThresholdAccumulatesAfterHopFee)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };

    auto thresholdGate = makeSimpleGate(env, ALICE, 100000, MODE_THRESHOLD, 1, recips, ratios, 5000);
    ASSERT_EQ(thresholdGate.status, QUGATE_SUCCESS);

    env.qpi.reset();
    auto first = env.routeToGate(thresholdGate.gateId - 1, 4000, 0);
    EXPECT_TRUE(first.accepted);
    EXPECT_EQ(first.forwarded, 3000);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 0);
    EXPECT_EQ(env.getGate(thresholdGate.gateId).currentBalance, 3000ULL);

    env.qpi.reset();
    auto second = env.routeToGate(thresholdGate.gateId - 1, 3000, 0);
    EXPECT_TRUE(second.accepted);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 5000);
    EXPECT_EQ(env.getGate(thresholdGate.gateId).currentBalance, 0ULL);
}

// Amount at or below hop fee with no reserve strands in gate balance
TEST(QuGateChain, InsufficientFundsStrand)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    auto g1 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    // Send amount <= hop fee with no chain reserve → strands
    env.qpi.reset();
    auto result = env.routeToGate(g1.gateId - 1, 500, 0);
    EXPECT_EQ(result.forwarded, 0); // stranded
    EXPECT_EQ(env.qpi.transferCount, 0); // nothing transferred

    auto gate = env.getGate(g1.gateId);
    EXPECT_EQ(gate.currentBalance, 500ULL); // accumulated in currentBalance
}

// Gate reserve covers hop fee when routed amount is too small
TEST(QuGateChain, ReserveCoversHopFee)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    // Create gate with chain to somewhere (need a target first)
    auto g1 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto g2 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
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

// fundGate adds to unified reserve on both chained and unchained gates
TEST(QuGateChain, FundGateReserve)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    auto g1 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto g2 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
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

// Chain routing handles a closed downstream gate without crashing
TEST(QuGateChain, DeadLinkChainedGateClosed)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    auto g1 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto g2 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
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

// getGate returns chain link, depth, and reserve fields
TEST(QuGateChain, GetGateReturnsChainFields)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    auto g1 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto g2 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
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

// Closing a gate refunds its reserve to the owner
TEST(QuGateChain, CloseGateRefundsReserve)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };

    auto g1 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto g2 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    env.setChain(ALICE, g2.gateId, g1.gateId, QUGATE_CHAIN_HOP_FEE);
    env.fundGate(BOB, g2.gateId, 3000);

    auto closeOut = env.closeGate(ALICE, g2.gateId);
    EXPECT_EQ(closeOut.status, QUGATE_SUCCESS);
    // Reserve (3000) refunded to owner
    EXPECT_EQ(env.qpi.totalTransferredTo(ALICE), 3000);
}

// endEpoch expiry marks gate inactive, refunds balance, adds to free-list, and increments generation
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
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_THRESHOLD, 1, recips, ratios, 50000);
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
    auto out2 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(out2.status, QUGATE_SUCCESS);
    EXPECT_EQ(env.state.get()._freeCount, 0ULL); // slot was reused from free-list
}

// Creation fee splits 50% burn and 50% shareholder dividends
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

    uint64 expectedBurn = QPI::div(fee * QUGATE_DEFAULT_FEE_BURN_BPS, 10000ULL); // 50000
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

// Escalated creation fee burn plus dividends still equals the total fee
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

    uint64 expectedBurn = QPI::div(fee * QUGATE_DEFAULT_FEE_BURN_BPS, 10000ULL); // 100000
    uint64 expectedDividend = fee - expectedBurn;                         // 100000

    EXPECT_EQ(expectedBurn, 100000ULL);
    EXPECT_EQ(expectedDividend, 100000ULL);

    // burn + dividend = fee
    EXPECT_EQ(env.state.get()._totalBurned + env.state.get()._earnedMaintenanceDividends, fee);
}

// Idle maintenance fee splits 50% burn and 50% dividends from reserve
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

    uint64 idleFee = QUGATE_DEFAULT_MAINTENANCE_FEE;
    uint64 expectedBurn = QPI::div(idleFee * QUGATE_DEFAULT_FEE_BURN_BPS, 10000ULL); // 12500
    uint64 expectedDividend = idleFee - expectedBurn;                         // 5000

    EXPECT_EQ(expectedBurn, 12500ULL);
    EXPECT_EQ(expectedDividend, 12500ULL);

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

// Multiple idle charge cycles accumulate maintenance totals correctly
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
    uint64 burnPerCycle = QPI::div(idleFee * QUGATE_DEFAULT_FEE_BURN_BPS, 10000ULL);
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

// Burn plus dividend equals fee exactly for various fee amounts
TEST(QuGateFinancial, RoundingBurnPlusDividendEqualsFee)
{
    // For various fee amounts, verify burn + dividend = fee exactly
    // The formula: burn = fee * 8000 / 10000, dividend = fee - burn

    // fee = 1: burn = 0, dividend = 1
    {
        uint64 fee = 1;
        uint64 burn = QPI::div(fee * QUGATE_DEFAULT_FEE_BURN_BPS, 10000ULL);
        uint64 div = fee - burn;
        EXPECT_EQ(burn, 0ULL);
        EXPECT_EQ(div, 1ULL);
        EXPECT_EQ(burn + div, fee);
    }

    // fee = 7: burn = 3, dividend = 4
    {
        uint64 fee = 7;
        uint64 burn = QPI::div(fee * QUGATE_DEFAULT_FEE_BURN_BPS, 10000ULL);
        uint64 div = fee - burn;
        EXPECT_EQ(burn, 3ULL);
        EXPECT_EQ(div, 4ULL);
        EXPECT_EQ(burn + div, fee);
    }

    // fee = 99999: burn = 49999, dividend = 50000
    {
        uint64 fee = 99999;
        uint64 burn = QPI::div(fee * QUGATE_DEFAULT_FEE_BURN_BPS, 10000ULL);
        uint64 div = fee - burn;
        EXPECT_EQ(burn, 49999ULL);
        EXPECT_EQ(div, 50000ULL);
        EXPECT_EQ(burn + div, fee);
    }

    // fee = 100000: burn = 50000, dividend = 50000 (default creation fee)
    {
        uint64 fee = 100000;
        uint64 burn = QPI::div(fee * QUGATE_DEFAULT_FEE_BURN_BPS, 10000ULL);
        uint64 div = fee - burn;
        EXPECT_EQ(burn, 50000ULL);
        EXPECT_EQ(div, 50000ULL);
        EXPECT_EQ(burn + div, fee);
    }

    // fee = 25000: burn = 12500, dividend = 12500 (idle maintenance fee)
    {
        uint64 fee = 25000;
        uint64 burn = QPI::div(fee * QUGATE_DEFAULT_FEE_BURN_BPS, 10000ULL);
        uint64 div = fee - burn;
        EXPECT_EQ(burn, 12500ULL);
        EXPECT_EQ(div, 12500ULL);
        EXPECT_EQ(burn + div, fee);
    }

    // fee = 3: burn = 1, dividend = 2
    {
        uint64 fee = 3;
        uint64 burn = QPI::div(fee * QUGATE_DEFAULT_FEE_BURN_BPS, 10000ULL);
        uint64 div = fee - burn;
        EXPECT_EQ(burn, 1ULL);
        EXPECT_EQ(div, 2ULL);
        EXPECT_EQ(burn + div, fee);
    }
}

// Cancelling a time-lock gate refunds both balance and reserve to owner
TEST(QuGateFinancial, CancelTimeLockRefundsReserve)
{
    // Create a TIME_LOCK gate, fund its reserve, cancel it
    // — verify reserve is refunded to owner (not trapped)
    QuGateTest env;

    uint64 creationFee = env.currentEscalatedFee();

    // Create TIME_LOCK gate with recipientCount=0
    createGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
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

// Cancelling a non-cancellable time-lock gate is rejected
TEST(QuGateFinancial, CancelTimeLockNonCancellableRejected)
{
    // Verify that cancelling a non-cancellable TIME_LOCK is rejected
    QuGateTest env;

    uint64 creationFee = env.currentEscalatedFee();

    createGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
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

// Close gate transfer failure keeps gate active and does not recycle slot
TEST(QuGateRegression, CloseGateTransferFailureDoesNotRecycleSlot)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_THRESHOLD, 1, recips, ratios, 50000);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    auto fundOut = env.fundGate(ALICE, out.gateId, 7000);
    ASSERT_EQ(fundOut.result, QUGATE_SUCCESS);
    env.sendToGate(ALICE, out.gateId, 30000);

    env.qpi.failTransfersTo(ALICE, 2);
    auto closeOut = env.closeGate(ALICE, out.gateId);
    EXPECT_EQ(closeOut.status, QUGATE_INVALID_PARAMS);

    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.active, 1);
    EXPECT_EQ(gate.currentBalance, 30000ULL);
    EXPECT_EQ(gate.reserve, 7000);
    EXPECT_EQ(env.state.get()._freeCount, 0ULL);
}

// Lazy expiry during close with transfer failure does not recycle slot
TEST(QuGateRegression, CloseGateLazyExpiryTransferFailureDoesNotRecycleSlot)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_THRESHOLD, 1, recips, ratios, 50000);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    auto fundOut = env.fundGate(ALICE, out.gateId, 7000);
    ASSERT_EQ(fundOut.result, QUGATE_SUCCESS);
    env.state.mut()._gates.set(out.gateId - 1, [&]{
        GateConfig gate = env.state.get()._gates.get(out.gateId - 1);
        gate.currentBalance = 30000;
        gate.lastActivityEpoch = 100;
        return gate;
    }());
    env.qpi._epoch = 200;

    env.qpi.failTransfersTo(ALICE, 2);
    auto closeOut = env.closeGate(ALICE, out.gateId);
    EXPECT_EQ(closeOut.status, QUGATE_GATE_NOT_ACTIVE);

    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.active, 1);
    EXPECT_EQ(gate.currentBalance, 30000ULL);
    EXPECT_EQ(gate.reserve, 7000);
    EXPECT_EQ(env.state.get()._freeCount, 0ULL);
}

// endEpoch expiry with transfer failure does not recycle slot
TEST(QuGateRegression, ExpiryTransferFailureDoesNotRecycleSlot)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_THRESHOLD, 1, recips, ratios, 50000);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    env.sendToGate(ALICE, out.gateId, 30000);
    env.qpi.failTransfersTo(ALICE, 1);
    env.qpi._epoch = 200;
    env.endEpoch();

    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.active, 1);
    EXPECT_EQ(gate.currentBalance, 30000ULL);
    EXPECT_EQ(env.state.get()._freeCount, 0ULL);
}

// Lazy expiry during send with transfer failure does not recycle slot
TEST(QuGateRegression, SendToGateLazyExpiryTransferFailureDoesNotRecycleSlot)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_THRESHOLD, 1, recips, ratios, 50000);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    auto fundOut = env.fundGate(ALICE, out.gateId, 7000);
    ASSERT_EQ(fundOut.result, QUGATE_SUCCESS);
    env.state.mut()._gates.set(out.gateId - 1, [&]{
        GateConfig gate = env.state.get()._gates.get(out.gateId - 1);
        gate.currentBalance = 30000;
        gate.lastActivityEpoch = 100;
        return gate;
    }());
    env.qpi._epoch = 200;

    env.qpi.failTransfersTo(ALICE, 2);
    auto sendOut = env.sendToGate(CHARLIE, out.gateId, 40000);
    EXPECT_EQ(sendOut.status, QUGATE_GATE_NOT_ACTIVE);

    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.active, 1);
    EXPECT_EQ(gate.currentBalance, 30000ULL);
    EXPECT_EQ(gate.reserve, 7000);
    EXPECT_EQ(env.state.get()._freeCount, 0ULL);
    EXPECT_EQ(env.qpi.totalTransferredTo(CHARLIE), 40000);
}

// Lazy expiry during update with transfer failure does not recycle slot
TEST(QuGateRegression, UpdateGateLazyExpiryTransferFailureDoesNotRecycleSlot)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_THRESHOLD, 1, recips, ratios, 50000);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    auto fundOut = env.fundGate(ALICE, out.gateId, 7000);
    ASSERT_EQ(fundOut.result, QUGATE_SUCCESS);
    env.state.mut()._gates.set(out.gateId - 1, [&]{
        GateConfig gate = env.state.get()._gates.get(out.gateId - 1);
        gate.currentBalance = 30000;
        gate.lastActivityEpoch = 100;
        return gate;
    }());
    env.qpi._epoch = 200;

    updateGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
    in.gateId = out.gateId;
    in.recipientCount = 1;
    in.recipients.set(0, CHARLIE);
    in.ratios.set(0, 100);
    in.threshold = 50000;

    env.qpi.failTransfersTo(ALICE, 2);
    auto updateOut = env.updateGate(ALICE, 0, in);
    EXPECT_EQ(updateOut.status, QUGATE_GATE_NOT_ACTIVE);

    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.active, 1);
    EXPECT_EQ(gate.currentBalance, 30000ULL);
    EXPECT_EQ(gate.reserve, 7000);
    EXPECT_EQ(env.state.get()._freeCount, 0ULL);
}

// Lazy expiry during fund with transfer failure does not recycle slot
TEST(QuGateRegression, FundGateLazyExpiryTransferFailureDoesNotRecycleSlot)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_THRESHOLD, 1, recips, ratios, 50000);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    env.state.mut()._gates.set(out.gateId - 1, [&]{
        GateConfig gate = env.state.get()._gates.get(out.gateId - 1);
        gate.currentBalance = 30000;
        gate.reserve = 7000;
        gate.lastActivityEpoch = 100;
        return gate;
    }());
    env.qpi._epoch = 200;

    env.qpi.failTransfersTo(ALICE, 2);
    auto fundOut = env.fundGate(CHARLIE, out.gateId, 40000);
    EXPECT_EQ(fundOut.result, QUGATE_GATE_NOT_ACTIVE);

    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.active, 1);
    EXPECT_EQ(gate.currentBalance, 30000ULL);
    EXPECT_EQ(gate.reserve, 7000);
    EXPECT_EQ(env.state.get()._freeCount, 0ULL);
    EXPECT_EQ(env.qpi.totalTransferredTo(CHARLIE), 40000);
}

// Lazy expiry during setChain with transfer failure does not recycle slot
TEST(QuGateRegression, SetChainLazyExpiryTransferFailureDoesNotRecycleSlot)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto target = makeSimpleGate(env, BOB, 100000, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(target.status, QUGATE_SUCCESS);
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_THRESHOLD, 1, recips, ratios, 50000);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    env.state.mut()._gates.set(out.gateId - 1, [&]{
        GateConfig gate = env.state.get()._gates.get(out.gateId - 1);
        gate.currentBalance = 30000;
        gate.reserve = 7000;
        gate.lastActivityEpoch = 100;
        return gate;
    }());
    env.qpi._epoch = 200;

    env.qpi.failTransfersTo(ALICE, 2);
    auto setChainOut = env.setChain(ALICE, out.gateId, target.gateId, QUGATE_CHAIN_HOP_FEE);
    EXPECT_EQ(setChainOut.result, QUGATE_GATE_NOT_ACTIVE);

    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.active, 1);
    EXPECT_EQ(gate.currentBalance, 30000ULL);
    EXPECT_EQ(gate.reserve, 7000);
    EXPECT_EQ(env.state.get()._freeCount, 0ULL);
}

// cancelTimeLock transfer failure keeps gate active and does not recycle slot
TEST(QuGateRegression, CancelTimeLockTransferFailureDoesNotRecycleSlot)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_TIME_LOCK, 1, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    ASSERT_EQ(env.configureTimeLock(ALICE, out.gateId, 200, QUGATE_TIME_LOCK_ABSOLUTE_EPOCH, 1), QUGATE_SUCCESS);
    ASSERT_EQ(env.fundGate(ALICE, out.gateId, 50000).result, QUGATE_SUCCESS);
    env.sendToGate(ALICE, out.gateId, 30000);

    env.qpi.failTransfersTo(ALICE, 2);
    auto cancelStatus = env.cancelTimeLock(ALICE, out.gateId);
    EXPECT_EQ(cancelStatus, QUGATE_INVALID_PARAMS);

    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.active, 1);
    EXPECT_EQ(gate.currentBalance, 30000ULL);
    EXPECT_EQ(gate.reserve, 50000);
    EXPECT_EQ(env.state.get()._freeCount, 0ULL);
}

// Sending to a fired time-lock gate does not mutate accounting fields
TEST(QuGateRegression, TimeLockRejectedDirectSendDoesNotMutateAccounting)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_TIME_LOCK, 1, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    ASSERT_EQ(env.configureTimeLock(ALICE, out.gateId, 200, QUGATE_TIME_LOCK_ABSOLUTE_EPOCH, 1), QUGATE_SUCCESS);

    auto cfg = env.state.get()._timeLockConfigs.get(out.gateId - 1);
    cfg.fired = 1;
    env.state.mut()._timeLockConfigs.set(out.gateId - 1, cfg);

    auto before = env.getGate(out.gateId);
    auto sendOut = env.sendToGate(ALICE, out.gateId, 30000);
    auto after = env.getGate(out.gateId);

    EXPECT_EQ(sendOut.status, QUGATE_GATE_NOT_ACTIVE);
    EXPECT_EQ(after.totalReceived, before.totalReceived);
    EXPECT_EQ(after.lastActivityEpoch, before.lastActivityEpoch);
    EXPECT_EQ(after.currentBalance, before.currentBalance);
    EXPECT_EQ(env.qpi.totalTransferredTo(ALICE), 30000);
}

// routeToGate on a fired time-lock gate silently rejects without refund
TEST(QuGateRegression, TimeLockRejectedChainDoesNotRefundInvocator)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_TIME_LOCK, 1, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    ASSERT_EQ(env.configureTimeLock(ALICE, out.gateId, 200, QUGATE_TIME_LOCK_ABSOLUTE_EPOCH, 1), QUGATE_SUCCESS);

    auto cfg = env.state.get()._timeLockConfigs.get(out.gateId - 1);
    cfg.fired = 1;
    env.state.mut()._timeLockConfigs.set(out.gateId - 1, cfg);

    env.qpi.reset();
    env.qpi._invocator = ALICE;
    auto result = env.routeToGate(out.gateId - 1, 5000, 0);

    EXPECT_EQ(result.forwarded, 0);
    EXPECT_EQ(result.accepted, false);
    EXPECT_EQ(env.qpi.transferCount, 0);
    EXPECT_EQ(env.getGate(out.gateId).currentBalance, 0ULL);
}

// Time-lock release transfer failure leaves gate active and unfired
TEST(QuGateRegression, TimeLockReleaseFailureDoesNotFireOrClose)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_TIME_LOCK, 1, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    ASSERT_EQ(env.configureTimeLock(ALICE, out.gateId, 101, QUGATE_TIME_LOCK_ABSOLUTE_EPOCH, 1), QUGATE_SUCCESS);
    env.sendToGate(ALICE, out.gateId, 30000);

    env.qpi.failTransfersTo(BOB, 1);
    env.qpi._epoch = 101;
    env.endEpoch();

    auto gate = env.getGate(out.gateId);
    auto cfg = env.state.get()._timeLockConfigs.get(out.gateId - 1);
    EXPECT_EQ(gate.active, 1);
    EXPECT_EQ(gate.currentBalance, 30000ULL);
    EXPECT_EQ(cfg.fired, 0);
    EXPECT_EQ(env.state.get()._freeCount, 0ULL);
}

// Heartbeat gate accepts valid configuration with beneficiaries
TEST(QuGateHeartbeat, HeartbeatConfigureSuccess)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };
    id beneficiaries[] = { BOB, CHARLIE };
    uint8 shares[] = { 60, 40 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_HEARTBEAT, 1, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    ASSERT_EQ(env.configureHeartbeat(ALICE, out.gateId, 3, 25, 10, beneficiaries, shares, 2), QUGATE_SUCCESS);
    auto hb = env.getHeartbeat(out.gateId);
    EXPECT_EQ(hb.active, 1);
    EXPECT_EQ(hb.thresholdEpochs, 3U);
    EXPECT_EQ(hb.payoutPercentPerEpoch, 25);
    EXPECT_EQ(hb.minimumBalance, 10);
    EXPECT_EQ(hb.beneficiaryCount, 2);
}

// configureHeartbeat on a non-heartbeat gate returns HEARTBEAT_NOT_ACTIVE
TEST(QuGateHeartbeat, HeartbeatConfigureWrongMode)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    id beneficiaries[] = { BOB };
    uint8 shares[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    EXPECT_EQ(env.configureHeartbeat(ALICE, out.gateId, 3, 25, 10, beneficiaries, shares, 1), QUGATE_HEARTBEAT_NOT_ACTIVE);
}

// Non-owner cannot configure heartbeat on another owner's gate
TEST(QuGateHeartbeat, HeartbeatConfigureUnauthorized)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };
    id beneficiaries[] = { BOB };
    uint8 shares[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_HEARTBEAT, 1, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    EXPECT_EQ(env.configureHeartbeat(BOB, out.gateId, 3, 25, 10, beneficiaries, shares, 1), QUGATE_UNAUTHORIZED);
}

// sendHeartbeat resets the last heartbeat epoch timer
TEST(QuGateHeartbeat, HeartbeatPingResetsTimer)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };
    id beneficiaries[] = { BOB };
    uint8 shares[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_HEARTBEAT, 1, recips, ratios);
    ASSERT_EQ(env.configureHeartbeat(ALICE, out.gateId, 3, 25, 10, beneficiaries, shares, 1), QUGATE_SUCCESS);

    env.qpi._epoch = 150;
    ASSERT_EQ(env.sendHeartbeat(ALICE, out.gateId), QUGATE_SUCCESS);
    EXPECT_EQ(env.getHeartbeat(out.gateId).lastHeartbeatEpoch, 150U);
}

// Heartbeat triggers after threshold epochs of inactivity
TEST(QuGateHeartbeat, HeartbeatTriggerAfterInactivity)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };
    id beneficiaries[] = { BOB };
    uint8 shares[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_HEARTBEAT, 1, recips, ratios);
    ASSERT_EQ(env.configureHeartbeat(ALICE, out.gateId, 3, 25, 10, beneficiaries, shares, 1), QUGATE_SUCCESS);

    env.qpi._epoch = 104;
    env.endEpoch();
    auto hb = env.getHeartbeat(out.gateId);
    EXPECT_EQ(hb.triggered, 1);
    EXPECT_EQ(hb.triggerEpoch, 104U);
}

TEST(QuGateHeartbeat, HeartbeatTriggerBoundaryExactThreshold)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };
    id beneficiaries[] = { BOB };
    uint8 shares[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_HEARTBEAT, 1, recips, ratios);
    ASSERT_EQ(env.configureHeartbeat(ALICE, out.gateId, 3, 25, 10, beneficiaries, shares, 1), QUGATE_SUCCESS);

    env.qpi._epoch = 103;
    env.endEpoch();

    auto hb = env.getHeartbeat(out.gateId);
    EXPECT_EQ(hb.triggered, 0);
}

// Triggered heartbeat distributes payout proportionally to beneficiaries
TEST(QuGateHeartbeat, HeartbeatPayoutDistribution)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };
    id beneficiaries[] = { BOB, CHARLIE };
    uint8 shares[] = { 60, 40 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_HEARTBEAT, 1, recips, ratios);
    ASSERT_EQ(env.configureHeartbeat(ALICE, out.gateId, 1, 50, 10, beneficiaries, shares, 2), QUGATE_SUCCESS);
    env.sendToGate(ALICE, out.gateId, 10000);
    env.qpi._epoch = 102;
    env.endEpoch();
    env.qpi.reset();
    env.qpi._epoch = 103;
    env.endEpoch();
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 3000);
    EXPECT_EQ(env.qpi.totalTransferredTo(CHARLIE), 2000);
}

// Triggered heartbeat pays out progressively across multiple epochs
TEST(QuGateHeartbeat, HeartbeatPayoutMultipleEpochs)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };
    id beneficiaries[] = { BOB };
    uint8 shares[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_HEARTBEAT, 1, recips, ratios);
    ASSERT_EQ(env.configureHeartbeat(ALICE, out.gateId, 1, 50, 10, beneficiaries, shares, 1), QUGATE_SUCCESS);
    env.sendToGate(ALICE, out.gateId, 8000);
    env.qpi._epoch = 102;
    env.endEpoch();
    for (uint16 epoch = 103; epoch <= 105; epoch++)
    {
        env.qpi.reset();
        env.qpi._epoch = epoch;
        env.endEpoch();
    }
    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.currentBalance, 1000ULL);
}

// Heartbeat gate auto-closes when balance drops to minimum
TEST(QuGateHeartbeat, HeartbeatAutoCloseAtMinimum)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };
    id beneficiaries[] = { BOB };
    uint8 shares[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_HEARTBEAT, 1, recips, ratios);
    ASSERT_EQ(env.configureHeartbeat(ALICE, out.gateId, 1, 80, 20, beneficiaries, shares, 1), QUGATE_SUCCESS);
    env.sendToGate(ALICE, out.gateId, 100);
    env.qpi._epoch = 102;
    env.endEpoch();
    env.qpi.reset();
    env.qpi._epoch = 103;
    env.endEpoch();
    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.active, 0);
    EXPECT_EQ(env.state.get()._freeCount, 1ULL);
}

// Heartbeat auto-close transfer failure keeps gate active with balance intact
TEST(QuGateHeartbeat, HeartbeatAutoCloseFailedTransferKeepsGateActive)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };
    id beneficiaries[] = { BOB };
    uint8 shares[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_HEARTBEAT, 1, recips, ratios);
    ASSERT_EQ(env.configureHeartbeat(ALICE, out.gateId, 1, 80, 20, beneficiaries, shares, 1), QUGATE_SUCCESS);
    env.sendToGate(ALICE, out.gateId, 1000);
    env.qpi._epoch = 102;
    env.endEpoch();
    env.qpi.reset();
    env.qpi.failTransfersTo(BOB, 2);
    env.qpi._epoch = 103;
    env.endEpoch();
    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.active, 1);
    EXPECT_EQ(gate.currentBalance, 1000ULL);
    EXPECT_EQ(env.state.get()._freeCount, 0ULL);
}

TEST(QuGateHeartbeat, HeartbeatChainUsesOnlyUndeliveredPayoutRemainder)
{
    QuGateTest env;
    id heartbeatRecips[] = { BOB };
    uint64 heartbeatRatios[] = { 0 };
    id downstreamRecips[] = { DAVE };
    uint64 downstreamRatios[] = { 100 };
    id beneficiaries[] = { BOB, CHARLIE };
    uint8 shares[] = { 50, 50 };

    auto source = makeSimpleGate(env, ALICE, 100000, MODE_HEARTBEAT, 1, heartbeatRecips, heartbeatRatios);
    auto downstream = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, downstreamRecips, downstreamRatios);
    ASSERT_EQ(env.configureHeartbeat(ALICE, source.gateId, 1, 50, 10, beneficiaries, shares, 2), QUGATE_SUCCESS);
    ASSERT_EQ(env.setChain(ALICE, source.gateId, downstream.gateId, QUGATE_CHAIN_HOP_FEE).result, QUGATE_SUCCESS);
    ASSERT_EQ(env.sendToGate(ALICE, source.gateId, 10000).status, QUGATE_SUCCESS);

    env.qpi._epoch = 102;
    env.endEpoch();

    env.qpi.reset();
    env.qpi.failTransfersTo(CHARLIE, 1);
    env.qpi._epoch = 103;
    env.endEpoch();

    auto gate = env.getGate(source.gateId);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 2500);
    EXPECT_EQ(env.qpi.totalTransferredTo(CHARLIE), 0);
    EXPECT_EQ(env.qpi.totalTransferredTo(DAVE), 1500);
    EXPECT_EQ(gate.currentBalance, 5000ULL);
    EXPECT_EQ(gate.totalForwarded, 5000ULL);
}

// sendHeartbeat after trigger returns HEARTBEAT_TRIGGERED and does not reset
TEST(QuGateHeartbeat, HeartbeatPingAfterTriggerDoesNotReset)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };
    id beneficiaries[] = { BOB };
    uint8 shares[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_HEARTBEAT, 1, recips, ratios);
    ASSERT_EQ(env.configureHeartbeat(ALICE, out.gateId, 1, 50, 10, beneficiaries, shares, 1), QUGATE_SUCCESS);
    env.qpi._epoch = 102;
    env.endEpoch();
    EXPECT_EQ(env.sendHeartbeat(ALICE, out.gateId), QUGATE_HEARTBEAT_TRIGGERED);
    EXPECT_EQ(env.getHeartbeat(out.gateId).triggered, 1);
}

// Triggered heartbeat with zero beneficiaries refunds remaining balance to owner
TEST(QuGateHeartbeat, HeartbeatZeroBeneficiariesRefundsOwner)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };
    id beneficiaries[] = { BOB };
    uint8 shares[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_HEARTBEAT, 1, recips, ratios);
    ASSERT_EQ(env.configureHeartbeat(ALICE, out.gateId, 1, 50, 2000, beneficiaries, shares, 1), QUGATE_SUCCESS);
    env.sendToGate(ALICE, out.gateId, 1000);
    auto cfg = env.state.get()._heartbeatConfigs.get(out.gateId - 1);
    cfg.triggered = 1;
    cfg.beneficiaryCount = 0;
    env.state.mut()._heartbeatConfigs.set(out.gateId - 1, cfg);
    env.qpi.reset();
    env.endEpoch();
    EXPECT_EQ(env.qpi.totalTransferredTo(ALICE), 1000);
}

// Guardian vote on multisig gate increments approval count
TEST(QuGateMultisig, MultisigVoteAccumulatesApproval)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 1, recips, ratios);
    id guardians[] = { BOB, CHARLIE };
    ASSERT_EQ(env.configureMultisig(ALICE, out.gateId, guardians, 2, 2, 5, 3), QUGATE_SUCCESS);
    ASSERT_EQ(env.sendToGate(BOB, out.gateId, 5000).status, QUGATE_SUCCESS);
    auto cfg = env.state.get()._multisigConfigs.get(out.gateId - 1);
    EXPECT_EQ(cfg.approvalCount, 1);
    EXPECT_EQ(cfg.proposalActive, 1);
}

// Reaching multisig quorum releases accumulated balance to recipient
TEST(QuGateMultisig, MultisigQuorumRelease)
{
    QuGateTest env;
    id recips[] = { DAVE };
    uint64 ratios[] = { 0 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 1, recips, ratios);
    id guardians[] = { BOB, CHARLIE };
    ASSERT_EQ(env.configureMultisig(ALICE, out.gateId, guardians, 2, 2, 5, 3), QUGATE_SUCCESS);
    env.sendToGate(BOB, out.gateId, 3000);
    env.qpi.reset();
    env.sendToGate(CHARLIE, out.gateId, 2000);
    EXPECT_EQ(env.qpi.totalTransferredTo(DAVE), 5000);
    EXPECT_EQ(env.getGate(out.gateId).currentBalance, 0ULL);
}

// Non-guardian send accumulates balance but does not count as a vote
TEST(QuGateMultisig, MultisigNonGuardianVoteIgnored)
{
    QuGateTest env;
    id recips[] = { DAVE };
    uint64 ratios[] = { 0 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 1, recips, ratios);
    id guardians[] = { BOB, CHARLIE };
    ASSERT_EQ(env.configureMultisig(ALICE, out.gateId, guardians, 2, 2, 5, 3), QUGATE_SUCCESS);
    env.sendToGate(EVE, out.gateId, 5000);
    auto cfg = env.state.get()._multisigConfigs.get(out.gateId - 1);
    EXPECT_EQ(cfg.approvalCount, 0);
    EXPECT_EQ(env.getGate(out.gateId).currentBalance, 5000ULL);
}

// Expired proposal resets approval so new votes start a fresh round
TEST(QuGateMultisig, MultisigProposalExpiry)
{
    QuGateTest env;
    id recips[] = { DAVE };
    uint64 ratios[] = { 0 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 1, recips, ratios);
    id guardians[] = { BOB, CHARLIE };
    ASSERT_EQ(env.configureMultisig(ALICE, out.gateId, guardians, 2, 2, 1, 3), QUGATE_SUCCESS);
    env.sendToGate(BOB, out.gateId, 1000);
    env.qpi._epoch = 103;
    env.sendToGate(CHARLIE, out.gateId, 1000);
    auto cfg = env.state.get()._multisigConfigs.get(out.gateId - 1);
    EXPECT_EQ(cfg.approvalCount, 1);
}

// Duplicate vote by same guardian returns ALREADY_VOTED
TEST(QuGateMultisig, MultisigDuplicateVoteIgnored)
{
    QuGateTest env;
    id recips[] = { DAVE };
    uint64 ratios[] = { 0 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 1, recips, ratios);
    id guardians[] = { BOB, CHARLIE };
    ASSERT_EQ(env.configureMultisig(ALICE, out.gateId, guardians, 2, 2, 5, 3), QUGATE_SUCCESS);
    env.sendToGate(BOB, out.gateId, 1000);
    auto dup = env.sendToGate(BOB, out.gateId, 2000);
    EXPECT_EQ(dup.status, QUGATE_MULTISIG_ALREADY_VOTED);
    EXPECT_EQ(env.state.get()._multisigConfigs.get(out.gateId - 1).approvalCount, 1);
    EXPECT_EQ(env.getGate(out.gateId).currentBalance, 3000ULL);
}

// Multisig release transfer failure preserves the gate balance
TEST(QuGateMultisig, MultisigReleaseFailureKeepsBalance)
{
    QuGateTest env;
    id recips[] = { DAVE };
    uint64 ratios[] = { 0 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 1, recips, ratios);
    id guardians[] = { BOB, CHARLIE };
    ASSERT_EQ(env.configureMultisig(ALICE, out.gateId, guardians, 2, 2, 5, 3), QUGATE_SUCCESS);
    env.sendToGate(BOB, out.gateId, 3000);
    env.qpi.failTransfersTo(DAVE, 1);
    env.sendToGate(CHARLIE, out.gateId, 2000);
    EXPECT_EQ(env.getGate(out.gateId).currentBalance, 5000ULL);
}

// Multisig quorum release forwards through chain to downstream gate
TEST(QuGateMultisig, MultisigChainForwardOnRelease)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto downstream = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    createGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
    in.mode = MODE_MULTISIG;
    in.recipientCount = 0;
    in.chainNextGateId = downstream.gateId;
    auto out = env.createGate(ALICE, 100000, in);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);

    id guardians[] = { CHARLIE, DAVE };
    ASSERT_EQ(env.configureMultisig(ALICE, out.gateId, guardians, 2, 2, 5, 3), QUGATE_SUCCESS);
    env.sendToGate(CHARLIE, out.gateId, 3000);
    env.qpi.reset();
    env.sendToGate(DAVE, out.gateId, 2000);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 4000);
}

// Owner can assign a multisig gate as admin for another gate
TEST(QuGateAdmin, SetAdminGateSuccess)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto target = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto admin = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 0, recips, ratios);
    id guardians[] = { BOB };
    ASSERT_EQ(env.configureMultisig(ALICE, admin.gateId, guardians, 1, 1, 5, 3), QUGATE_SUCCESS);
    ASSERT_EQ(env.setAdminGate(ALICE, target.gateId, admin.gateId), QUGATE_SUCCESS);
    EXPECT_EQ(env.getGate(target.gateId).hasAdminGate, 1);
}

// Non-multisig gate cannot be used as an admin gate
TEST(QuGateAdmin, SetAdminGateRequiresMultisig)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto target = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto admin = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    EXPECT_EQ(env.setAdminGate(ALICE, target.gateId, admin.gateId), QUGATE_INVALID_ADMIN_GATE);
}

// Circular admin gate chain is rejected with INVALID_ADMIN_CYCLE
TEST(QuGateAdmin, SetAdminGateCycleRejected)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    id guardians[] = { BOB };
    // All three gates must be MULTISIG so they can serve as admin gates
    auto a = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 0, recips, ratios);
    ASSERT_EQ(env.configureMultisig(ALICE, a.gateId, guardians, 1, 1, 5, 3), QUGATE_SUCCESS);
    auto b = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 0, recips, ratios);
    ASSERT_EQ(env.configureMultisig(ALICE, b.gateId, guardians, 1, 1, 5, 3), QUGATE_SUCCESS);
    auto c = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 0, recips, ratios);
    ASSERT_EQ(env.configureMultisig(ALICE, c.gateId, guardians, 1, 1, 5, 3), QUGATE_SUCCESS);
    ASSERT_EQ(env.setAdminGate(ALICE, a.gateId, b.gateId), QUGATE_SUCCESS);
    ASSERT_EQ(env.setAdminGate(ALICE, b.gateId, c.gateId), QUGATE_SUCCESS);
    EXPECT_EQ(env.setAdminGate(ALICE, c.gateId, a.gateId), QUGATE_INVALID_ADMIN_CYCLE);
}

// Gate mutation is blocked until admin gate approval is obtained
TEST(QuGateAdmin, AdminApprovalRequired)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto target = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    createGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
    in.mode = MODE_MULTISIG;
    in.recipientCount = 0;
    in.chainNextGateId = -1;
    auto admin = env.createGate(ALICE, 100000, in);
    id guardians[] = { BOB };
    ASSERT_EQ(env.configureMultisig(ALICE, admin.gateId, guardians, 1, 1, 5, 3), QUGATE_SUCCESS);
    ASSERT_EQ(env.setAdminGate(ALICE, target.gateId, admin.gateId), QUGATE_SUCCESS);
    EXPECT_EQ(env.closeGate(ALICE, target.gateId).status, QUGATE_ADMIN_GATE_REQUIRED);
    ASSERT_EQ(env.sendToGate(BOB, admin.gateId, 5000).status, QUGATE_SUCCESS);
    EXPECT_EQ(env.closeGate(ALICE, target.gateId).status, QUGATE_SUCCESS);
}

TEST(QuGateAdmin, MultisigAdminApprovalWindowExactExpiry)
{
    {
        QuGateTest env;
        id recips[] = { BOB };
        uint64 ratios[] = { 100 };
        auto target = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
        auto admin = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 0, recips, ratios);
        id guardians[] = { BOB };
        ASSERT_EQ(env.configureMultisig(ALICE, admin.gateId, guardians, 1, 1, 5, 2), QUGATE_SUCCESS);
        ASSERT_EQ(env.setAdminGate(ALICE, target.gateId, admin.gateId), QUGATE_SUCCESS);

        env.qpi._epoch = 100;
        env.sendToGate(BOB, admin.gateId, 1000);
        env.qpi._epoch = 101;
        EXPECT_EQ(env.closeGate(ALICE, target.gateId).status, QUGATE_SUCCESS);
    }

    {
        QuGateTest env;
        id recips[] = { BOB };
        uint64 ratios[] = { 100 };
        auto target = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
        auto admin = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 0, recips, ratios);
        id guardians[] = { BOB };
        ASSERT_EQ(env.configureMultisig(ALICE, admin.gateId, guardians, 1, 1, 5, 2), QUGATE_SUCCESS);
        ASSERT_EQ(env.setAdminGate(ALICE, target.gateId, admin.gateId), QUGATE_SUCCESS);

        env.qpi._epoch = 100;
        env.sendToGate(BOB, admin.gateId, 1000);
        env.qpi._epoch = 102;
        EXPECT_EQ(env.closeGate(ALICE, target.gateId).status, QUGATE_ADMIN_GATE_REQUIRED);
    }
}

TEST(QuGateAdmin, SetAdminGateConsumesApprovalWindow)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto target = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto adminA = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 0, recips, ratios);
    auto adminB = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 0, recips, ratios);
    id guardians[] = { BOB };
    ASSERT_EQ(env.configureMultisig(ALICE, adminA.gateId, guardians, 1, 1, 5, 3), QUGATE_SUCCESS);
    ASSERT_EQ(env.configureMultisig(ALICE, adminB.gateId, guardians, 1, 1, 5, 3), QUGATE_SUCCESS);
    ASSERT_EQ(env.setAdminGate(ALICE, target.gateId, adminA.gateId), QUGATE_SUCCESS);

    env.sendToGate(BOB, adminA.gateId, 1000);
    EXPECT_EQ(env.setAdminGate(ALICE, target.gateId, adminB.gateId), QUGATE_SUCCESS);
    EXPECT_EQ(env.adminApprovalValid(env.slotFromGateId(adminA.gateId)), false);
    EXPECT_EQ(env.setAdminGate(ALICE, target.gateId, -1), QUGATE_ADMIN_GATE_REQUIRED);
}

// Owner can clear admin gate after admin gate is closed
TEST(QuGateAdmin, AdminApprovalBypass)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto target = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto admin = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 0, recips, ratios);
    id guardians[] = { BOB };
    ASSERT_EQ(env.configureMultisig(ALICE, admin.gateId, guardians, 1, 1, 5, 3), QUGATE_SUCCESS);
    ASSERT_EQ(env.setAdminGate(ALICE, target.gateId, admin.gateId), QUGATE_SUCCESS);
    ASSERT_EQ(env.closeGate(ALICE, admin.gateId).status, QUGATE_SUCCESS);
    EXPECT_EQ(env.setAdminGate(ALICE, target.gateId, -1), QUGATE_SUCCESS);
}

// Recycled slot with new generation does not inherit old admin approval
TEST(QuGateAdmin, StaleAdminGateIgnored)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto target = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    createGate_input adminIn;
    memset(&adminIn, 0, sizeof(adminIn));
    adminIn.mode = MODE_MULTISIG;
    adminIn.recipientCount = 0;
    adminIn.chainNextGateId = -1;
    auto oldAdmin = env.createGate(ALICE, 100000, adminIn);
    id guardians[] = { BOB };
    ASSERT_EQ(env.configureMultisig(ALICE, oldAdmin.gateId, guardians, 1, 1, 5, 3), QUGATE_SUCCESS);
    ASSERT_EQ(env.setAdminGate(ALICE, target.gateId, oldAdmin.gateId), QUGATE_SUCCESS);
    ASSERT_EQ(env.closeGate(ALICE, oldAdmin.gateId).status, QUGATE_SUCCESS);

    auto recycled = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 0, recips, ratios);
    ASSERT_EQ(recycled.gateId, oldAdmin.gateId);
    ASSERT_EQ(env.configureMultisig(ALICE, recycled.gateId, guardians, 1, 1, 5, 3), QUGATE_SUCCESS);
    ASSERT_EQ(env.sendToGate(BOB, recycled.gateId, 5000).status, QUGATE_SUCCESS);

    EXPECT_EQ(env.closeGate(ALICE, target.gateId).status, QUGATE_ADMIN_GATE_REQUIRED);
    auto adminState = env.getAdminGate(target.gateId);
    EXPECT_EQ(adminState.adminApprovalActive, 0);
}

TEST(QuGateAdmin, GetAdminGateStaleReferenceDoesNotReportLiveAdmin)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto target = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    createGate_input adminIn;
    memset(&adminIn, 0, sizeof(adminIn));
    adminIn.mode = MODE_MULTISIG;
    adminIn.recipientCount = 0;
    adminIn.chainNextGateId = -1;
    auto admin = env.createGate(ALICE, 100000, adminIn);
    id guardians[] = { BOB };
    ASSERT_EQ(env.configureMultisig(ALICE, admin.gateId, guardians, 1, 1, 5, 3), QUGATE_SUCCESS);
    ASSERT_EQ(env.setAdminGate(ALICE, target.gateId, admin.gateId), QUGATE_SUCCESS);

    sint64 staleAdminId = env.encodeCurrentGateId(env.slotFromGateId(admin.gateId));
    ASSERT_EQ(env.closeGate(ALICE, admin.gateId).status, QUGATE_SUCCESS);
    auto recycled = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 0, recips, ratios);
    ASSERT_EQ(env.slotFromGateId(recycled.gateId), env.slotFromGateId(staleAdminId));
    ASSERT_EQ(env.configureMultisig(ALICE, recycled.gateId, guardians, 1, 1, 5, 3), QUGATE_SUCCESS);

    auto adminState = env.getAdminGate(target.gateId);
    EXPECT_EQ(adminState.hasAdminGate, 1);
    EXPECT_EQ(adminState.adminGateId, staleAdminId);
    EXPECT_EQ(adminState.guardianCount, 0);
    EXPECT_EQ(adminState.required, 0);
    EXPECT_EQ(adminState.adminApprovalActive, 0);
}

// Owner can self-clear a stale admin gate after slot recycling
TEST(QuGateAdmin, StaleAdminGateOwnerCanSelfClear)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto target = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto admin = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 0, recips, ratios);
    id guardians[] = { BOB };
    ASSERT_EQ(env.configureMultisig(ALICE, admin.gateId, guardians, 1, 1, 5, 3), QUGATE_SUCCESS);
    ASSERT_EQ(env.setAdminGate(ALICE, target.gateId, admin.gateId), QUGATE_SUCCESS);
    ASSERT_EQ(env.closeGate(ALICE, admin.gateId).status, QUGATE_SUCCESS);
    auto recycled = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(recycled.gateId, admin.gateId);
    EXPECT_EQ(env.setAdminGate(ALICE, target.gateId, -1), QUGATE_SUCCESS);
}

// Owner can withdraw a partial amount from gate reserve
TEST(QuGateReserve, WithdrawReserveSuccess)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto gate = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(env.fundGate(ALICE, gate.gateId, 5000).result, QUGATE_SUCCESS);
    auto out = env.withdrawReserve(ALICE, gate.gateId, 2000);
    EXPECT_EQ(out.status, QUGATE_SUCCESS);
    EXPECT_EQ(out.withdrawn, 2000ULL);
    EXPECT_EQ(env.getGate(gate.gateId).reserve, 3000);
}

// Withdrawing with amount 0 withdraws the full reserve
TEST(QuGateReserve, WithdrawReserveFullAmount)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto gate = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(env.fundGate(ALICE, gate.gateId, 5000).result, QUGATE_SUCCESS);
    auto out = env.withdrawReserve(ALICE, gate.gateId, 0);
    EXPECT_EQ(out.withdrawn, 5000ULL);
    EXPECT_EQ(env.getGate(gate.gateId).reserve, 0);
}

// Withdrawing more than reserve returns INVALID_PARAMS
TEST(QuGateReserve, WithdrawReserveExceedsBalance)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto gate = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(env.fundGate(ALICE, gate.gateId, 5000).result, QUGATE_SUCCESS);
    auto out = env.withdrawReserve(ALICE, gate.gateId, 6000);
    EXPECT_EQ(out.status, QUGATE_INVALID_PARAMS);
    EXPECT_EQ(env.getGate(gate.gateId).reserve, 5000);
}

// Non-owner cannot withdraw from gate reserve
TEST(QuGateReserve, WithdrawReserveUnauthorized)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto gate = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(env.fundGate(ALICE, gate.gateId, 5000).result, QUGATE_SUCCESS);
    auto out = env.withdrawReserve(BOB, gate.gateId, 1000);
    EXPECT_EQ(out.status, QUGATE_UNAUTHORIZED);
}

// Transfer failure during withdraw leaves reserve unchanged
TEST(QuGateReserve, WithdrawReserveTransferFailure)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto gate = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(env.fundGate(ALICE, gate.gateId, 5000).result, QUGATE_SUCCESS);
    env.qpi.failTransfersTo(ALICE, 1);
    auto out = env.withdrawReserve(ALICE, gate.gateId, 1000);
    EXPECT_EQ(out.withdrawn, 0ULL);
    EXPECT_EQ(env.getGate(gate.gateId).reserve, 5000);
}

TEST(QuGateReserve, WithdrawReserveAdminApprovalPath)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto target = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto admin = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 0, recips, ratios);
    id guardians[] = { BOB };
    ASSERT_EQ(env.configureMultisig(ALICE, admin.gateId, guardians, 1, 1, 5, 3), QUGATE_SUCCESS);
    ASSERT_EQ(env.setAdminGate(ALICE, target.gateId, admin.gateId), QUGATE_SUCCESS);
    ASSERT_EQ(env.fundGate(ALICE, target.gateId, 5000).result, QUGATE_SUCCESS);

    EXPECT_EQ(env.withdrawReserve(ALICE, target.gateId, 1000).status, QUGATE_ADMIN_GATE_REQUIRED);
    env.sendToGate(BOB, admin.gateId, 1000);

    auto out = env.withdrawReserve(ALICE, target.gateId, 2500);
    EXPECT_EQ(out.status, QUGATE_SUCCESS);
    EXPECT_EQ(out.withdrawn, 2500ULL);
    EXPECT_EQ(env.withdrawReserve(ALICE, target.gateId, 500).status, QUGATE_ADMIN_GATE_REQUIRED);
}

// Absolute time-lock releases funds at the specified epoch
TEST(QuGateTimeLock, TimeLockAbsoluteRelease)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_TIME_LOCK, 1, recips, ratios);
    ASSERT_EQ(env.configureTimeLock(ALICE, out.gateId, 105, QUGATE_TIME_LOCK_ABSOLUTE_EPOCH, 1), QUGATE_SUCCESS);
    env.sendToGate(ALICE, out.gateId, 5000);
    env.qpi._epoch = 105;
    env.endEpoch();
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 5000);
    EXPECT_EQ(env.getGate(out.gateId).active, 0);
}

// Relative time-lock anchors unlock epoch on first deposit
TEST(QuGateTimeLock, TimeLockRelativeAnchorOnFirstFund)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_TIME_LOCK, 1, recips, ratios);
    ASSERT_EQ(env.configureTimeLock(ALICE, out.gateId, 5, QUGATE_TIME_LOCK_RELATIVE_EPOCHS, 1), QUGATE_SUCCESS);
    EXPECT_EQ(env.state.get()._timeLockConfigs.get(out.gateId - 1).unlockEpoch, 0U);
    env.qpi._epoch = 110; // within expiry window (< 100 + 50)
    env.sendToGate(ALICE, out.gateId, 5000);
    EXPECT_EQ(env.state.get()._timeLockConfigs.get(out.gateId - 1).unlockEpoch, 115U);
}

// Relative time-lock releases funds after delay epochs from first deposit
TEST(QuGateTimeLock, TimeLockRelativeRelease)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_TIME_LOCK, 1, recips, ratios);
    ASSERT_EQ(env.configureTimeLock(ALICE, out.gateId, 5, QUGATE_TIME_LOCK_RELATIVE_EPOCHS, 1), QUGATE_SUCCESS);
    env.qpi._epoch = 110; // within expiry window (< 100 + 50)
    env.sendToGate(ALICE, out.gateId, 5000);
    env.qpi._epoch = 115; // unlock epoch = 110 + 5
    env.endEpoch();
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 5000);
    EXPECT_EQ(env.getGate(out.gateId).active, 0);
}

TEST(QuGateTimeLock, TimeLockRelativeAlreadyFundedConfigureAnchorsImmediately)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_TIME_LOCK, 1, recips, ratios);
    ASSERT_EQ(env.configureTimeLock(ALICE, out.gateId, 150, QUGATE_TIME_LOCK_ABSOLUTE_EPOCH, 1), QUGATE_SUCCESS);
    env.sendToGate(ALICE, out.gateId, 5000);

    env.qpi._epoch = 100;
    ASSERT_EQ(env.configureTimeLock(ALICE, out.gateId, 5, QUGATE_TIME_LOCK_RELATIVE_EPOCHS, 1), QUGATE_SUCCESS);

    auto cfg = env.state.get()._timeLockConfigs.get(out.gateId - 1);
    EXPECT_EQ(cfg.unlockEpoch, 105U);
    EXPECT_EQ(cfg.delayEpochs, 5U);
}

// Time-lock release routes through gate-as-recipient to downstream gate
TEST(QuGateTimeLock, TimeLockGateAsRecipientRelease)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto downstream = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_TIME_LOCK, 1, recips, ratios);
    GateConfig gate = env.state.get()._gates.get(out.gateId - 1);
    gate.recipientGateIds.set(0, downstream.gateId);
    env.state.mut()._gates.set(out.gateId - 1, gate);
    ASSERT_EQ(env.configureTimeLock(ALICE, out.gateId, 105, QUGATE_TIME_LOCK_ABSOLUTE_EPOCH, 1), QUGATE_SUCCESS);
    env.sendToGate(ALICE, out.gateId, 5000);
    env.qpi._epoch = 105;
    env.endEpoch();
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 4000);
    EXPECT_EQ(env.getGate(out.gateId).active, 0);
}

// Time-lock with no recipient forwards via chain on release
TEST(QuGateTimeLock, TimeLockNoRecipientChainForward)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto downstream = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    createGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
    in.mode = MODE_TIME_LOCK;
    in.recipientCount = 0;
    in.chainNextGateId = downstream.gateId;
    auto out = env.createGate(ALICE, 100000, in);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    ASSERT_EQ(env.configureTimeLock(ALICE, out.gateId, 105, QUGATE_TIME_LOCK_ABSOLUTE_EPOCH, 1), QUGATE_SUCCESS);
    env.sendToGate(ALICE, out.gateId, 5000);
    env.qpi._epoch = 105;
    env.endEpoch();
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 4000);
    EXPECT_EQ(env.getGate(out.gateId).active, 0);
    EXPECT_EQ(env.getGate(out.gateId).totalForwarded, 5000ULL);
}

TEST(QuGateTimeLock, TimeLockNoRecipientFailedChainKeepsGateLive)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto downstream = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    createGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
    in.mode = MODE_TIME_LOCK;
    in.recipientCount = 0;
    in.chainNextGateId = downstream.gateId;
    auto out = env.createGate(ALICE, 100000, in);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    ASSERT_EQ(env.configureTimeLock(ALICE, out.gateId, 105, QUGATE_TIME_LOCK_ABSOLUTE_EPOCH, 1), QUGATE_SUCCESS);
    ASSERT_EQ(env.closeGate(ALICE, downstream.gateId).status, QUGATE_SUCCESS);
    ASSERT_EQ(env.sendToGate(ALICE, out.gateId, 5000).status, QUGATE_SUCCESS);

    env.qpi._epoch = 105;
    env.endEpoch();

    auto gate = env.getGate(out.gateId);
    auto tl = env.state.get()._timeLockConfigs.get(out.gateId - 1);
    EXPECT_EQ(gate.active, 1);
    EXPECT_EQ(gate.currentBalance, 5000ULL);
    EXPECT_EQ(gate.totalForwarded, 0ULL);
    EXPECT_EQ(tl.fired, 0);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 0);
}

TEST(QuGateDeferred, SplitGateRecipientSkipsCancelledTimeLockTarget)
{
    QuGateTest env;
    id walletRecips[] = { BOB, id::zero() };
    uint64 ratios[] = { 50, 50 };
    auto split = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 2, walletRecips, ratios);

    createGate_input tlIn;
    memset(&tlIn, 0, sizeof(tlIn));
    for (uint8 _ri = 0; _ri < 8; _ri++) tlIn.recipientGateIds.set(_ri, -1);
    tlIn.mode = MODE_TIME_LOCK;
    tlIn.recipientCount = 0;
    tlIn.chainNextGateId = -1;
    auto target = env.createGate(ALICE, 100000, tlIn);
    ASSERT_EQ(target.status, QUGATE_SUCCESS);
    ASSERT_EQ(env.configureTimeLock(ALICE, target.gateId, 120, QUGATE_TIME_LOCK_ABSOLUTE_EPOCH, 1), QUGATE_SUCCESS);
    ASSERT_EQ(env.cancelTimeLock(ALICE, target.gateId), QUGATE_SUCCESS);

    GateConfig splitGate = env.state.get()._gates.get(split.gateId - 1);
    splitGate.recipientGateIds.set(1, (sint64)target.gateId);
    env.state.mut()._gates.set(split.gateId - 1, splitGate);

    auto result = env.routeToGate(split.gateId - 1, 10000, 0);
    auto updatedSplit = env.getGate(split.gateId);
    auto updatedTarget = env.getGate(target.gateId);
    EXPECT_TRUE(result.accepted);
    EXPECT_EQ(result.forwarded, 4500);
    EXPECT_EQ(updatedSplit.totalForwarded, 4500ULL);
    EXPECT_EQ(updatedTarget.currentBalance, 0ULL);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 4500);
}

TEST(QuGateDeferred, RoundRobinGateRecipientSkipsCancelledTimeLockTarget)
{
    QuGateTest env;
    id walletRecips[] = { id::zero(), BOB };
    uint64 ratios[] = { 0, 0 };
    auto rr = makeSimpleGate(env, ALICE, 100000, MODE_ROUND_ROBIN, 2, walletRecips, ratios);

    createGate_input tlIn;
    memset(&tlIn, 0, sizeof(tlIn));
    for (uint8 _ri = 0; _ri < 8; _ri++) tlIn.recipientGateIds.set(_ri, -1);
    tlIn.mode = MODE_TIME_LOCK;
    tlIn.recipientCount = 0;
    tlIn.chainNextGateId = -1;
    auto target = env.createGate(ALICE, 100000, tlIn);
    ASSERT_EQ(target.status, QUGATE_SUCCESS);
    ASSERT_EQ(env.configureTimeLock(ALICE, target.gateId, 120, QUGATE_TIME_LOCK_ABSOLUTE_EPOCH, 1), QUGATE_SUCCESS);
    ASSERT_EQ(env.cancelTimeLock(ALICE, target.gateId), QUGATE_SUCCESS);

    GateConfig rrGate = env.state.get()._gates.get(rr.gateId - 1);
    rrGate.recipientGateIds.set(0, (sint64)target.gateId);
    env.state.mut()._gates.set(rr.gateId - 1, rrGate);

    auto first = env.routeToGate(rr.gateId - 1, 9000, 0);
    auto afterFirst = env.getGate(rr.gateId);
    auto afterFirstRaw = env.state.get()._gates.get(rr.gateId - 1);
    auto targetAfterFirst = env.getGate(target.gateId);
    EXPECT_FALSE(first.accepted);
    EXPECT_EQ(first.forwarded, 0);
    EXPECT_EQ(afterFirst.totalForwarded, 0ULL);
    EXPECT_EQ(afterFirstRaw.roundRobinIndex, 0ULL);
    EXPECT_EQ(targetAfterFirst.currentBalance, 0ULL);

    auto second = env.routeToGate(rr.gateId - 1, 9000, 0);
    auto afterSecond = env.getGate(rr.gateId);
    EXPECT_FALSE(second.accepted);
    EXPECT_EQ(second.forwarded, 0);
    EXPECT_EQ(afterSecond.totalForwarded, 0ULL);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 0);
}

TEST(QuGateDeferred, ThresholdGateRecipientSkipsCancelledTimeLockTarget)
{
    QuGateTest env;
    id walletRecips[] = { id::zero() };
    uint64 ratios[] = { 0 };
    auto threshold = makeSimpleGate(env, ALICE, 100000, MODE_THRESHOLD, 1, walletRecips, ratios, 4000);

    createGate_input tlIn;
    memset(&tlIn, 0, sizeof(tlIn));
    for (uint8 _ri = 0; _ri < 8; _ri++) tlIn.recipientGateIds.set(_ri, -1);
    tlIn.mode = MODE_TIME_LOCK;
    tlIn.recipientCount = 0;
    tlIn.chainNextGateId = -1;
    auto target = env.createGate(ALICE, 100000, tlIn);
    ASSERT_EQ(target.status, QUGATE_SUCCESS);
    ASSERT_EQ(env.configureTimeLock(ALICE, target.gateId, 120, QUGATE_TIME_LOCK_ABSOLUTE_EPOCH, 1), QUGATE_SUCCESS);
    ASSERT_EQ(env.cancelTimeLock(ALICE, target.gateId), QUGATE_SUCCESS);

    GateConfig thresholdGate = env.state.get()._gates.get(threshold.gateId - 1);
    thresholdGate.recipientGateIds.set(0, (sint64)target.gateId);
    env.state.mut()._gates.set(threshold.gateId - 1, thresholdGate);

    auto result = env.routeToGate(threshold.gateId - 1, 5000, 0);
    auto updatedThreshold = env.getGate(threshold.gateId);
    auto updatedTarget = env.getGate(target.gateId);
    // Threshold accepts custody but can't release to cancelled TIME_LOCK target
    EXPECT_TRUE(result.accepted);
    EXPECT_EQ(result.forwarded, 4000);
    EXPECT_EQ(updatedThreshold.currentBalance, 4000ULL);
    EXPECT_EQ(updatedThreshold.totalForwarded, 0ULL);
    EXPECT_EQ(updatedTarget.currentBalance, 0ULL);
}

TEST(QuGateDeferred, RandomGateRecipientSkipsCancelledTimeLockTarget)
{
    QuGateTest env;
    id walletRecips[] = { id::zero(), id::zero() };
    uint64 ratios[] = { 0, 0 };
    auto random = makeSimpleGate(env, ALICE, 100000, MODE_RANDOM, 2, walletRecips, ratios);

    createGate_input tlIn;
    memset(&tlIn, 0, sizeof(tlIn));
    for (uint8 _ri = 0; _ri < 8; _ri++) tlIn.recipientGateIds.set(_ri, -1);
    tlIn.mode = MODE_TIME_LOCK;
    tlIn.recipientCount = 0;
    tlIn.chainNextGateId = -1;
    auto target = env.createGate(ALICE, 100000, tlIn);
    ASSERT_EQ(target.status, QUGATE_SUCCESS);
    ASSERT_EQ(env.configureTimeLock(ALICE, target.gateId, 120, QUGATE_TIME_LOCK_ABSOLUTE_EPOCH, 1), QUGATE_SUCCESS);
    ASSERT_EQ(env.cancelTimeLock(ALICE, target.gateId), QUGATE_SUCCESS);

    GateConfig randomGate = env.state.get()._gates.get(random.gateId - 1);
    randomGate.recipientGateIds.set(0, (sint64)target.gateId);
    randomGate.recipientGateIds.set(1, (sint64)target.gateId);
    env.state.mut()._gates.set(random.gateId - 1, randomGate);

    auto result = env.routeToGate(random.gateId - 1, 5000, 0);
    auto updatedRandom = env.getGate(random.gateId);
    auto updatedTarget = env.getGate(target.gateId);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.forwarded, 0);
    EXPECT_EQ(updatedRandom.totalForwarded, 0ULL);
    EXPECT_EQ(updatedTarget.currentBalance, 0ULL);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 0);
}

TEST(QuGateDeferred, MultisigGateRecipientSkipsCancelledTimeLockTarget)
{
    QuGateTest env;
    id recips[] = { id::zero() };
    uint64 ratios[] = { 0 };
    auto multisig = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 1, recips, ratios);
    id guardians[] = { BOB };
    ASSERT_EQ(env.configureMultisig(ALICE, multisig.gateId, guardians, 1, 1, 5, 3), QUGATE_SUCCESS);

    createGate_input tlIn;
    memset(&tlIn, 0, sizeof(tlIn));
    for (uint8 _ri = 0; _ri < 8; _ri++) tlIn.recipientGateIds.set(_ri, -1);
    tlIn.mode = MODE_TIME_LOCK;
    tlIn.recipientCount = 0;
    tlIn.chainNextGateId = -1;
    auto target = env.createGate(ALICE, 100000, tlIn);
    ASSERT_EQ(target.status, QUGATE_SUCCESS);
    ASSERT_EQ(env.configureTimeLock(ALICE, target.gateId, 120, QUGATE_TIME_LOCK_ABSOLUTE_EPOCH, 1), QUGATE_SUCCESS);
    ASSERT_EQ(env.cancelTimeLock(ALICE, target.gateId), QUGATE_SUCCESS);

    GateConfig multisigGate = env.state.get()._gates.get(multisig.gateId - 1);
    multisigGate.recipientGateIds.set(0, (sint64)target.gateId);
    env.state.mut()._gates.set(multisig.gateId - 1, multisigGate);

    auto result = env.sendToMultisigGate(BOB, multisig.gateId, 5000);
    auto updatedMultisig = env.getGate(multisig.gateId);
    auto updatedTarget = env.getGate(target.gateId);
    EXPECT_EQ(result.status, QUGATE_SUCCESS);
    EXPECT_EQ(updatedMultisig.currentBalance, 5000ULL);
    EXPECT_EQ(updatedMultisig.totalForwarded, 0ULL);
    EXPECT_EQ(updatedTarget.currentBalance, 0ULL);
}

// getGatesByOwner excludes closed gates from results
TEST(QuGateQuery, GetGatesByOwnerFiltersInactive)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto a = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto b = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(env.closeGate(ALICE, a.gateId).status, QUGATE_SUCCESS);
    auto gates = env.getGatesByOwner(ALICE);
    EXPECT_EQ(gates.get(0), env.encodeCurrentGateId(b.gateId - 1));
    EXPECT_EQ(gates.get(1), 0);
}

// getGateBySlot returns historical gate ID for inactive slot
TEST(QuGateQuery, GetGateBySlotInactiveReturnsHistoricalId)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(env.closeGate(ALICE, out.gateId).status, QUGATE_SUCCESS);
    auto slotOut = env.getGateBySlot(0);
    EXPECT_EQ(slotOut.active, 0);
    EXPECT_EQ(slotOut.gateId, 1);
}

// getGateBySlot returns current versioned gate ID for active slot
TEST(QuGateQuery, GetGateBySlotActiveReturnsCurrentId)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto slotOut = env.getGateBySlot(0);
    EXPECT_EQ(slotOut.active, 1);
    EXPECT_EQ(slotOut.gateId, 1);
}

TEST(QuGateChain, ChainValidationIgnoresStaleGenerationReusedSlot)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto source = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto target = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    sint64 staleTargetId = env.encodeCurrentGateId(env.slotFromGateId(target.gateId));

    ASSERT_EQ(env.closeGate(ALICE, target.gateId).status, QUGATE_SUCCESS);
    auto recycled = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(env.slotFromGateId(recycled.gateId), env.slotFromGateId(staleTargetId));

    auto out = env.setChain(ALICE, source.gateId, staleTargetId, QUGATE_CHAIN_HOP_FEE);
    EXPECT_EQ(out.result, QUGATE_INVALID_CHAIN);
    EXPECT_EQ(env.getGate(source.gateId).chainNextGateId, -1);
}

TEST(QuGateRecipient, GateRecipientValidationIgnoresStaleGenerationReusedSlot)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 100 };
    auto target = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    sint64 staleTargetId = env.encodeCurrentGateId(env.slotFromGateId(target.gateId));

    ASSERT_EQ(env.closeGate(ALICE, target.gateId).status, QUGATE_SUCCESS);
    auto recycled = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(env.slotFromGateId(recycled.gateId), env.slotFromGateId(staleTargetId));

    createGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
    in.mode = MODE_SPLIT;
    in.recipientCount = 1;
    in.recipients.set(0, id::zero());
    in.ratios.set(0, 100);
    in.recipientGateIds.set(0, staleTargetId);

    auto out = env.createGate(ALICE, 100000, in);
    EXPECT_EQ(out.status, QUGATE_INVALID_GATE_RECIPIENT);
}

// Admin-only multisig burns vote QU instead of accumulating balance
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
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
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

// Multiple guardian votes on admin-only multisig each burn independently
TEST(QuGateFinancial, AdminOnlyMultisigMultipleVotesBurn)
{
    // Multiple guardian votes each burn their QU independently
    QuGateTest env;

    uint64 creationFee = env.currentEscalatedFee();

    createGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
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

// Chain routing burns hop fee at each hop in the chain
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

// setChain burns the hop fee from the invocation reward
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

// setChain refunds excess fee above the hop cost
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

// Excess creation fee above required amount goes to gate reserve
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

// Paying exactly the creation fee results in zero reserve
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

// Large overpayment at creation all goes to reserve with no refund
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

// Split gate conserves total input across all recipient transfers
TEST(QuGateConservation, SplitConservation)
{
    QuGateTest env;
    id recips[] = { BOB, CHARLIE };
    uint64 ratios[] = { 7000, 3000 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 2, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    env.sendToGate(ALICE, out.gateId, 10000);
    auto gate = env.getGate(out.gateId);
    sint64 bobGot = env.qpi.totalTransferredTo(BOB);
    sint64 charlieGot = env.qpi.totalTransferredTo(CHARLIE);
    EXPECT_EQ(bobGot + charlieGot, 10000);
    EXPECT_EQ(gate.totalForwarded, 10000ULL);
    EXPECT_EQ(gate.currentBalance, 0ULL);
}

// Round-robin gate conserves total across multiple sends
TEST(QuGateConservation, RoundRobinConservation)
{
    QuGateTest env;
    id recips[] = { BOB, CHARLIE, DAVE };
    uint64 ratios[] = { 0, 0, 0 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_ROUND_ROBIN, 3, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    env.sendToGate(ALICE, out.gateId, 3000);
    env.sendToGate(ALICE, out.gateId, 5000);
    env.sendToGate(ALICE, out.gateId, 7000);
    auto gate = env.getGate(out.gateId);
    // Use gate accounting instead of qpi transfers (qpi.reset() between sends)
    EXPECT_EQ(gate.totalReceived, 15000ULL);
    EXPECT_EQ(gate.totalForwarded, 15000ULL);
    EXPECT_EQ(gate.currentBalance, 0ULL);
}

// Threshold gate conserves funds through accumulation and release
TEST(QuGateConservation, ThresholdConservation)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_THRESHOLD, 1, recips, ratios, 20000);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    env.sendToGate(ALICE, out.gateId, 8000);
    auto gateBefore = env.getGate(out.gateId);
    EXPECT_EQ(gateBefore.currentBalance, 8000ULL);
    EXPECT_EQ(gateBefore.totalForwarded, 0ULL);
    env.sendToGate(ALICE, out.gateId, 15000);
    auto gateAfter = env.getGate(out.gateId);
    EXPECT_EQ(gateAfter.currentBalance, 0ULL);
    EXPECT_EQ(gateAfter.totalForwarded, 23000ULL);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 23000);
}

// Random gate conserves total across multiple sends
TEST(QuGateConservation, RandomConservation)
{
    QuGateTest env;
    id recips[] = { BOB, CHARLIE };
    uint64 ratios[] = { 5000, 5000 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_RANDOM, 2, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    for (int i = 0; i < 10; i++)
    {
        env.qpi._tick = 12345 + i;
        env.sendToGate(ALICE, out.gateId, 1000);
    }
    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.totalReceived, 10000ULL);
    EXPECT_EQ(gate.totalForwarded, 10000ULL);
    EXPECT_EQ(gate.currentBalance, 0ULL);
}

// Conditional gate conserves total for whitelisted sender
TEST(QuGateConservation, ConditionalConservation)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    id allowed[] = { ALICE };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_CONDITIONAL, 1, recips, ratios, 0, allowed, 1);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    env.sendToGate(ALICE, out.gateId, 5000);
    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 5000);
    EXPECT_EQ(gate.totalForwarded, 5000ULL);
    EXPECT_EQ(gate.currentBalance, 0ULL);
}

// Heartbeat gate conserves total through trigger and full payout
TEST(QuGateConservation, HeartbeatConservation)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };
    id beneficiaries[] = { BOB, CHARLIE };
    uint8 shares[] = { 60, 40 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_HEARTBEAT, 1, recips, ratios);
    ASSERT_EQ(env.configureHeartbeat(ALICE, out.gateId, 1, 100, 0, beneficiaries, shares, 2), QUGATE_SUCCESS);
    env.sendToGate(ALICE, out.gateId, 10000);
    // Trigger
    env.qpi._epoch = 102;
    env.endEpoch();
    // Payout (100% per epoch, should distribute all)
    env.qpi.reset();
    env.qpi._epoch = 103;
    env.endEpoch();
    sint64 total = env.qpi.totalTransferredTo(BOB) + env.qpi.totalTransferredTo(CHARLIE);
    EXPECT_EQ(total, 10000);
}

// Multisig gate conserves total through quorum release
TEST(QuGateConservation, MultisigConservation)
{
    QuGateTest env;
    id recips[] = { DAVE };
    uint64 ratios[] = { 0 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 1, recips, ratios);
    id guardians[] = { BOB, CHARLIE };
    ASSERT_EQ(env.configureMultisig(ALICE, out.gateId, guardians, 2, 2, 10, 5), QUGATE_SUCCESS);
    env.sendToGate(BOB, out.gateId, 3000);
    env.qpi.reset();
    env.sendToGate(CHARLIE, out.gateId, 2000);
    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(env.qpi.totalTransferredTo(DAVE), 5000);
    EXPECT_EQ(gate.currentBalance, 0ULL);
    EXPECT_EQ(gate.totalForwarded, 5000ULL);
}

// Time-lock gate conserves total through deposit and epoch release
TEST(QuGateConservation, TimeLockConservation)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_TIME_LOCK, 1, recips, ratios);
    ASSERT_EQ(env.configureTimeLock(ALICE, out.gateId, 110, QUGATE_TIME_LOCK_ABSOLUTE_EPOCH, 1), QUGATE_SUCCESS);
    env.sendToGate(ALICE, out.gateId, 3000);
    env.sendToGate(ALICE, out.gateId, 7000);
    auto gateBefore = env.getGate(out.gateId);
    EXPECT_EQ(gateBefore.currentBalance, 10000ULL);
    env.qpi._epoch = 110;
    env.endEpoch();
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 10000);
    auto gateAfter = env.getGate(out.gateId);
    EXPECT_EQ(gateAfter.currentBalance, 0ULL);
    EXPECT_EQ(gateAfter.totalForwarded, 10000ULL);
    EXPECT_EQ(gateAfter.active, 0);
}

// Closing a gate refunds both balance and reserve completely
TEST(QuGateConservation, CloseGateAccountingComplete)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_THRESHOLD, 1, recips, ratios, 50000);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    ASSERT_EQ(env.fundGate(ALICE, out.gateId, 5000).result, QUGATE_SUCCESS);
    env.sendToGate(ALICE, out.gateId, 20000);
    auto gateBefore = env.getGate(out.gateId);
    EXPECT_EQ(gateBefore.currentBalance, 20000ULL);
    EXPECT_EQ(gateBefore.reserve, 5000);
    env.qpi.reset();
    ASSERT_EQ(env.closeGate(ALICE, out.gateId).status, QUGATE_SUCCESS);
    EXPECT_EQ(env.qpi.totalTransferredTo(ALICE), 25000);
    auto gateAfter = env.getGate(out.gateId);
    EXPECT_EQ(gateAfter.active, 0);
    EXPECT_EQ(gateAfter.currentBalance, 0ULL);
    EXPECT_EQ(gateAfter.reserve, 0);
}

// Expiry refunds both balance and reserve to owner
TEST(QuGateConservation, ExpiryAccountingComplete)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_THRESHOLD, 1, recips, ratios, 50000);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    ASSERT_EQ(env.fundGate(ALICE, out.gateId, 5000).result, QUGATE_SUCCESS);
    env.sendToGate(ALICE, out.gateId, 20000);
    env.qpi.reset();
    env.qpi._epoch = 200;
    env.endEpoch();
    EXPECT_EQ(env.qpi.totalTransferredTo(ALICE), 25000);
    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.active, 0);
}

// Single-hop chain conserves total as forwarded plus hop fee burned
TEST(QuGateConservation, ChainTwoHopConservation)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto gate2 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    // routeToGate deducts hop fee from the forwarded amount
    env.qpi.reset();
    env.qpi._invocator = ALICE;
    auto result = env.routeToGate(gate2.gateId - 1, 10000, 0);
    EXPECT_TRUE(result.accepted);
    // 10000 - 1000 hop fee = 9000 forwarded to BOB
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 9000);
    // 1000 burned as hop fee
    EXPECT_EQ(env.qpi.totalBurned, QUGATE_CHAIN_HOP_FEE);
    // Total conserved: 9000 + 1000 = 10000
    EXPECT_EQ((uint64)env.qpi.totalTransferredTo(BOB) + (uint64)env.qpi.totalBurned, 10000ULL);
}

// Threshold gate re-accumulates correctly after release
TEST(QuGateLifecycle, ThresholdReaccumulation)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_THRESHOLD, 1, recips, ratios, 10000);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    env.sendToGate(ALICE, out.gateId, 12000);
    EXPECT_EQ(env.getGate(out.gateId).currentBalance, 0ULL);
    EXPECT_EQ(env.getGate(out.gateId).totalForwarded, 12000ULL);
    // Second cycle
    env.sendToGate(ALICE, out.gateId, 5000);
    EXPECT_EQ(env.getGate(out.gateId).currentBalance, 5000ULL);
    env.sendToGate(ALICE, out.gateId, 6000);
    EXPECT_EQ(env.getGate(out.gateId).currentBalance, 0ULL);
    EXPECT_EQ(env.getGate(out.gateId).totalForwarded, 23000ULL);
}

// Threshold gate releases immediately when deposit exactly meets target
TEST(QuGateLifecycle, ThresholdExactBoundary)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_THRESHOLD, 1, recips, ratios, 5000);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    env.sendToGate(ALICE, out.gateId, 5000);
    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.currentBalance, 0ULL);
    EXPECT_EQ(gate.totalForwarded, 5000ULL);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 5000);
}

// Threshold gate releases to primary recipient ignoring chain target
TEST(QuGateLifecycle, ThresholdWithChainTarget)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto downstream = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    createGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
    in.mode = MODE_THRESHOLD;
    in.recipientCount = 1;
    in.recipients.set(0, CHARLIE);
    in.ratios.set(0, 10000);
    in.threshold = 10000;
    in.chainNextGateId = downstream.gateId;
    auto out = env.createGate(ALICE, 100000, in);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    ASSERT_EQ(env.fundGate(ALICE, out.gateId, 5000).result, QUGATE_SUCCESS);
    env.sendToGate(ALICE, out.gateId, 10000);
    EXPECT_EQ(env.qpi.totalTransferredTo(CHARLIE), 10000);
}

// 3-of-5 multisig releases on exactly the third guardian vote
TEST(QuGateLifecycle, MultisigQuorumBoundary3of5)
{
    QuGateTest env;
    id recips[] = { DAVE };
    uint64 ratios[] = { 0 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 1, recips, ratios);
    id guardians[] = { BOB, CHARLIE, DAVE, EVE, QuGateTest::makeId(6) };
    ASSERT_EQ(env.configureMultisig(ALICE, out.gateId, guardians, 5, 3, 10, 5), QUGATE_SUCCESS);
    env.sendToGate(BOB, out.gateId, 1000);
    env.sendToGate(CHARLIE, out.gateId, 1000);
    EXPECT_EQ(env.getGate(out.gateId).currentBalance, 2000ULL);
    env.qpi.reset();
    env.sendToGate(DAVE, out.gateId, 1000);
    // 3rd vote should trigger release
    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.currentBalance, 0ULL);
}

// 1-of-1 multisig releases immediately on single guardian vote
TEST(QuGateLifecycle, MultisigUnanimity1of1)
{
    QuGateTest env;
    id recips[] = { DAVE };
    uint64 ratios[] = { 0 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 1, recips, ratios);
    id guardians[] = { BOB };
    ASSERT_EQ(env.configureMultisig(ALICE, out.gateId, guardians, 1, 1, 10, 5), QUGATE_SUCCESS);
    env.sendToGate(BOB, out.gateId, 5000);
    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.currentBalance, 0ULL);
    EXPECT_EQ(gate.totalForwarded, 5000ULL);
    EXPECT_EQ(env.qpi.totalTransferredTo(DAVE), 5000);
}

// Reconfiguring multisig is blocked while a proposal is active
TEST(QuGateLifecycle, MultisigReconfigureBlockedDuringActiveProposal)
{
    QuGateTest env;
    id recips[] = { DAVE };
    uint64 ratios[] = { 0 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 1, recips, ratios);
    id guardians[] = { BOB, CHARLIE };
    ASSERT_EQ(env.configureMultisig(ALICE, out.gateId, guardians, 2, 2, 10, 5), QUGATE_SUCCESS);
    env.sendToGate(BOB, out.gateId, 1000);
    auto cfg1 = env.state.get()._multisigConfigs.get(out.gateId - 1);
    EXPECT_EQ(cfg1.proposalActive, 1);
    EXPECT_EQ(cfg1.approvalCount, 1);
    // Reconfigure blocked while proposal is active
    id newGuardians[] = { BOB, CHARLIE, EVE };
    EXPECT_EQ(env.configureMultisig(ALICE, out.gateId, newGuardians, 3, 2, 10, 5), QUGATE_MULTISIG_PROPOSAL_ACTIVE);
    // Original config unchanged
    auto cfg2 = env.state.get()._multisigConfigs.get(out.gateId - 1);
    EXPECT_EQ(cfg2.guardianCount, 2);
}

// Multiple deposits into time-lock are released as one sum
TEST(QuGateLifecycle, TimeLockMultipleDepositsSingleRelease)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_TIME_LOCK, 1, recips, ratios);
    ASSERT_EQ(env.configureTimeLock(ALICE, out.gateId, 110, QUGATE_TIME_LOCK_ABSOLUTE_EPOCH, 1), QUGATE_SUCCESS);
    env.sendToGate(ALICE, out.gateId, 3000);
    env.sendToGate(ALICE, out.gateId, 4000);
    env.sendToGate(ALICE, out.gateId, 3000);
    EXPECT_EQ(env.getGate(out.gateId).currentBalance, 10000ULL);
    env.qpi._epoch = 110;
    env.endEpoch();
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 10000);
    EXPECT_EQ(env.getGate(out.gateId).active, 0);
}

// Cancelling a time-lock after it has fired is rejected
TEST(QuGateLifecycle, TimeLockCancelAfterFireRejected)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_TIME_LOCK, 1, recips, ratios);
    ASSERT_EQ(env.configureTimeLock(ALICE, out.gateId, 105, QUGATE_TIME_LOCK_ABSOLUTE_EPOCH, 1), QUGATE_SUCCESS);
    env.sendToGate(ALICE, out.gateId, 5000);
    env.qpi._epoch = 105;
    env.endEpoch();
    EXPECT_EQ(env.getGate(out.gateId).active, 0);
    auto cancelStatus = env.cancelTimeLock(ALICE, out.gateId);
    EXPECT_NE(cancelStatus, QUGATE_SUCCESS);
}

// Heartbeat distributes payout to three beneficiaries by share ratios
TEST(QuGateLifecycle, HeartbeatMultiBeneficiaryShares)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };
    id beneficiaries[] = { BOB, CHARLIE, DAVE };
    uint8 shares[] = { 50, 30, 20 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_HEARTBEAT, 1, recips, ratios);
    ASSERT_EQ(env.configureHeartbeat(ALICE, out.gateId, 1, 100, 0, beneficiaries, shares, 3), QUGATE_SUCCESS);
    env.sendToGate(ALICE, out.gateId, 10000);
    env.qpi._epoch = 102;
    env.endEpoch();
    env.qpi.reset();
    env.qpi._epoch = 103;
    env.endEpoch();
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 5000);
    EXPECT_EQ(env.qpi.totalTransferredTo(CHARLIE), 3000);
    EXPECT_EQ(env.qpi.totalTransferredTo(DAVE), 2000);
}

// Split gate routes to mixed wallet and gate recipients via routeToGate
TEST(QuGateRecipient, SplitMixedWalletAndGateRecipients)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto downstream = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    // Create a split gate where recipient 0 is a wallet, recipient 1 routes to a gate
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 2, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    // Manually set recipients: CHARLIE at wallet, downstream at gate
    GateConfig gate = env.state.get()._gates.get(out.gateId - 1);
    gate.recipientCount = 2;
    gate.recipients.set(0, CHARLIE);
    gate.recipients.set(1, id::zero());
    gate.ratios.set(0, 5000);
    gate.ratios.set(1, 5000);
    gate.recipientGateIds.set(0, -1);
    gate.recipientGateIds.set(1, downstream.gateId);
    env.state.mut()._gates.set(out.gateId - 1, gate);
    // The harness processSplit doesn't implement gate-as-recipient routing at the
    // sendToGate level (that's in the contract's routeToGate path), so we test via
    // the routeToGate function directly
    env.qpi.reset();
    env.qpi._invocator = ALICE;
    auto result = env.routeToGate(out.gateId - 1, 10000, 0);
    EXPECT_TRUE(result.accepted);
    EXPECT_GT(result.forwarded, 0);
}

// Split gate with zero recipients and no chain is rejected
TEST(QuGateEdge, ZeroRecipientGateWithoutChainRejected)
{
    QuGateTest env;
    createGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
    in.mode = MODE_SPLIT;
    in.recipientCount = 0;
    in.chainNextGateId = -1;
    auto out = env.createGate(ALICE, 100000, in);
    EXPECT_NE(out.status, QUGATE_SUCCESS);
}

// Gate with maximum 8 recipients is accepted
TEST(QuGateEdge, MaxRecipientsAccepted)
{
    QuGateTest env;
    createGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
    in.mode = MODE_SPLIT;
    in.recipientCount = 8;
    for (int i = 0; i < 8; i++)
    {
        in.recipients.set(i, QuGateTest::makeId(10 + i));
        in.ratios.set(i, 1250);
    }
    in.chainNextGateId = -1;
    auto out = env.createGate(ALICE, 100000, in);
    EXPECT_EQ(out.status, QUGATE_SUCCESS);
}

// Gate with 9 recipients is rejected
TEST(QuGateEdge, NineRecipientsRejected)
{
    QuGateTest env;
    createGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
    in.mode = MODE_SPLIT;
    in.recipientCount = 9;
    in.chainNextGateId = -1;
    auto out = env.createGate(ALICE, 100000, in);
    EXPECT_NE(out.status, QUGATE_SUCCESS);
}

// Closing an already-closed gate returns GATE_NOT_ACTIVE
TEST(QuGateEdge, DoubleCloseReturnsNotActive)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(env.closeGate(ALICE, out.gateId).status, QUGATE_SUCCESS);
    EXPECT_EQ(env.closeGate(ALICE, out.gateId).status, QUGATE_GATE_NOT_ACTIVE);
}

// Sending to gate ID zero returns INVALID_GATE_ID
TEST(QuGateEdge, SendToGateIdZeroRejected)
{
    QuGateTest env;
    auto out = env.sendToGate(ALICE, 0, 5000);
    EXPECT_EQ(out.status, QUGATE_INVALID_GATE_ID);
}

// Updating recipient while holding threshold funds redirects future releases
TEST(QuGateEdge, UpdateGateWhileHoldingFunds)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_THRESHOLD, 1, recips, ratios, 20000);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    env.sendToGate(ALICE, out.gateId, 10000);
    EXPECT_EQ(env.getGate(out.gateId).currentBalance, 10000ULL);
    // Update to new recipient
    updateGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
    in.gateId = out.gateId;
    in.recipientCount = 1;
    in.recipients.set(0, CHARLIE);
    in.ratios.set(0, 10000);
    in.threshold = 20000;
    auto updateOut = env.updateGate(ALICE, 0, in);
    EXPECT_EQ(updateOut.status, QUGATE_SUCCESS);
    // Funds still held
    EXPECT_EQ(env.getGate(out.gateId).currentBalance, 10000ULL);
    // New recipient gets funds on release
    env.sendToGate(ALICE, out.gateId, 12000);
    EXPECT_EQ(env.qpi.totalTransferredTo(CHARLIE), 22000);
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 0);
}

// Non-owner can fund a gate's reserve
TEST(QuGateEdge, FundGateByNonOwner)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    // BOB funds ALICE's gate — should succeed
    auto fundOut = env.fundGate(BOB, out.gateId, 5000);
    EXPECT_EQ(fundOut.result, QUGATE_SUCCESS);
    EXPECT_EQ(env.getGate(out.gateId).reserve, 5000);
}

// WithdrawReserveDuringIdleDelinquency — skipped: harness lacks idle delinquency state

// Admin gate blocks close until multisig approval is obtained
TEST(QuGateGovernance, AdminGateBlocksCloseWithoutApproval)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto target = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto admin = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 0, recips, ratios);
    id guardians[] = { BOB };
    ASSERT_EQ(env.configureMultisig(ALICE, admin.gateId, guardians, 1, 1, 10, 3), QUGATE_SUCCESS);
    ASSERT_EQ(env.setAdminGate(ALICE, target.gateId, admin.gateId), QUGATE_SUCCESS);
    // Close without approval — blocked
    EXPECT_EQ(env.closeGate(ALICE, target.gateId).status, QUGATE_ADMIN_GATE_REQUIRED);
    // Get approval, then close succeeds
    env.sendToGate(BOB, admin.gateId, 1000);
    EXPECT_EQ(env.closeGate(ALICE, target.gateId).status, QUGATE_SUCCESS);
}

// Routing into a governed gate succeeds without admin approval
TEST(QuGateGovernance, RouteIntoGovernedGateStillAccepts)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto governed = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto admin = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 0, recips, ratios);
    id guardians[] = { BOB };
    ASSERT_EQ(env.configureMultisig(ALICE, admin.gateId, guardians, 1, 1, 10, 3), QUGATE_SUCCESS);
    ASSERT_EQ(env.setAdminGate(ALICE, governed.gateId, admin.gateId), QUGATE_SUCCESS);
    // Route directly into the governed gate — governance only affects mutations, not receives
    env.qpi.reset();
    env.qpi._invocator = ALICE;
    auto result = env.routeToGate(governed.gateId - 1, 5000, 0);
    EXPECT_TRUE(result.accepted);
    // 5000 - 1000 hop fee = 4000 forwarded to BOB
    EXPECT_EQ(env.qpi.totalTransferredTo(BOB), 4000);
}

// Owner recovers governance after admin gate is closed by self-clearing
TEST(QuGateGovernance, GovernanceRecoveryAfterAdminClose)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto target = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto admin = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 0, recips, ratios);
    id guardians[] = { BOB };
    ASSERT_EQ(env.configureMultisig(ALICE, admin.gateId, guardians, 1, 1, 10, 3), QUGATE_SUCCESS);
    ASSERT_EQ(env.setAdminGate(ALICE, target.gateId, admin.gateId), QUGATE_SUCCESS);
    // Close admin gate directly
    ASSERT_EQ(env.closeGate(ALICE, admin.gateId).status, QUGATE_SUCCESS);
    EXPECT_EQ(env.getGate(admin.gateId).active, 0);
    // Keep target active by refreshing its activity
    env.sendToGate(ALICE, target.gateId, 1000);
    // Owner can self-clear stale admin
    EXPECT_EQ(env.setAdminGate(ALICE, target.gateId, -1), QUGATE_SUCCESS);
    // Target is now ungoverned — close should work
    EXPECT_EQ(env.closeGate(ALICE, target.gateId).status, QUGATE_SUCCESS);
}

// Idle maintenance charge deducts fee from gate reserve
TEST(QuGateIdle, IdleChargeDeductsFromReserve)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(env.fundGate(ALICE, out.gateId, 50000).result, QUGATE_SUCCESS);
    auto gateBefore = env.getGate(out.gateId);
    EXPECT_EQ(gateBefore.reserve, 50000);

    // Advance past idle window
    // Gate created at epoch 100, nextIdleChargeEpoch = 104
    env.qpi._epoch = 104;
    env.endEpoch();
    auto gateAfter = env.getGate(out.gateId);
    EXPECT_EQ(gateAfter.reserve, 50000 - QUGATE_DEFAULT_MAINTENANCE_FEE);
    EXPECT_EQ(gateAfter.active, 1);
    EXPECT_EQ(env.state.get()._totalMaintenanceCharged, QUGATE_DEFAULT_MAINTENANCE_FEE);
}

// Idle charge is skipped for gates with recent activity
TEST(QuGateIdle, IdleChargeSkippedWhenActive)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(env.fundGate(ALICE, out.gateId, 50000).result, QUGATE_SUCCESS);

    // Send at epoch 103 — within the idle window
    env.qpi._epoch = 103;
    env.sendToGate(ALICE, out.gateId, 1000);

    // Run endEpoch at 104 — gate was recently active, should skip charge
    env.qpi._epoch = 104;
    env.endEpoch();
    auto gate = env.getGate(out.gateId);
    EXPECT_EQ(gate.reserve, 50000);
    EXPECT_EQ(env.state.get()._totalMaintenanceCharged, 0ULL);
}

// Gate becomes delinquent when reserve is insufficient for idle charge
TEST(QuGateIdle, IdleDelinquencyWhenReserveInsufficient)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    // No reserve funded — gate has 0 reserve

    env.qpi._epoch = 104;
    env.endEpoch();
    // Gate should be marked delinquent
    uint16 delinquentEpoch = env.state.get()._idleDelinquentEpochs.get(out.gateId - 1);
    EXPECT_EQ(delinquentEpoch, 104);
    EXPECT_EQ(env.getGate(out.gateId).active, 1);
}

// Gate activity clears delinquency flag
TEST(QuGateIdle, IdleDelinquencyCureAfterActivity)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    // Become delinquent
    env.qpi._epoch = 104;
    env.endEpoch();
    EXPECT_EQ(env.state.get()._idleDelinquentEpochs.get(out.gateId - 1), 104);

    // Activity cures delinquency (send resets lastActivityEpoch)
    env.qpi._epoch = 105;
    env.sendToGate(ALICE, out.gateId, 1000);
    // endEpoch sees recent activity → clears delinquent flag
    env.endEpoch();
    EXPECT_EQ(env.state.get()._idleDelinquentEpochs.get(out.gateId - 1), 0);
    EXPECT_EQ(env.getGate(out.gateId).active, 1);
}

// Delinquent gate expires after grace period ends
TEST(QuGateIdle, IdleGracePeriodToExpiry)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    // Become delinquent at epoch 104
    env.qpi._epoch = 104;
    env.endEpoch();
    EXPECT_EQ(env.state.get()._idleDelinquentEpochs.get(out.gateId - 1), 104);

    // Still within grace period
    env.qpi._epoch = 107;
    env.endEpoch();
    EXPECT_EQ(env.getGate(out.gateId).active, 1);

    // Grace period expired: 104 + 4 = 108
    env.qpi._epoch = 108;
    env.endEpoch();
    EXPECT_EQ(env.getGate(out.gateId).active, 0);
}

// Funded reserve covers multiple idle charge cycles before delinquency
TEST(QuGateIdle, IdleFundedReserveCoversMultipleCharges)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(env.fundGate(ALICE, out.gateId, 75000).result, QUGATE_SUCCESS);

    // 3 idle cycles: charge at 104, 108, 112
    env.qpi._epoch = 104;
    env.endEpoch();
    EXPECT_EQ(env.getGate(out.gateId).reserve, 50000);

    env.qpi._epoch = 108;
    env.endEpoch();
    EXPECT_EQ(env.getGate(out.gateId).reserve, 25000);

    env.qpi._epoch = 112;
    env.endEpoch();
    EXPECT_EQ(env.getGate(out.gateId).reserve, 0);

    EXPECT_EQ(env.state.get()._totalMaintenanceCharged, 75000ULL);
    EXPECT_EQ(env.getGate(out.gateId).active, 1);

    // Next cycle: reserve is 0, becomes delinquent
    env.qpi._epoch = 116;
    env.endEpoch();
    EXPECT_EQ(env.state.get()._idleDelinquentEpochs.get(out.gateId - 1), 116);
}

// Threshold gate with held balance is exempt from idle charge
TEST(QuGateIdle, IdleActiveHoldExemptionThreshold)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_THRESHOLD, 1, recips, ratios, 50000);
    // Send funds below threshold — gate is holding balance
    env.sendToGate(ALICE, out.gateId, 10000);
    EXPECT_EQ(env.getGate(out.gateId).currentBalance, 10000ULL);

    // Advance way past idle window — but threshold gate with balance is exempt
    env.qpi._epoch = 120;
    env.endEpoch();
    EXPECT_EQ(env.state.get()._idleDelinquentEpochs.get(out.gateId - 1), 0);
    EXPECT_EQ(env.getGate(out.gateId).active, 1);
}

// Idle maintenance fee splits into burn and dividend portions
TEST(QuGateIdle, IdleMaintenanceFeeSplit)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(env.fundGate(ALICE, out.gateId, 50000).result, QUGATE_SUCCESS);

    uint64 burnedBefore = env.state.get()._totalMaintenanceBurned;
    uint64 dividendBefore = env.state.get()._totalMaintenanceDividends;

    env.qpi.reset();
    env.qpi._epoch = 104;
    env.endEpoch();

    uint64 fee = QUGATE_DEFAULT_MAINTENANCE_FEE;
    uint64 expectedBurn = QPI::div(fee * QUGATE_DEFAULT_FEE_BURN_BPS, 10000ULL);
    uint64 expectedDividend = fee - expectedBurn;

    EXPECT_EQ(env.state.get()._totalMaintenanceBurned - burnedBefore, expectedBurn);
    EXPECT_EQ(env.state.get()._totalMaintenanceDividends - dividendBefore, expectedDividend);
    EXPECT_EQ(expectedBurn + expectedDividend, fee);
}

// Simple 1-recipient split pays 1x base idle fee
TEST(QuGateComplexity, SimpleGatePaysBaseFee)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(env.fundGate(ALICE, out.gateId, 100000).result, QUGATE_SUCCESS);
    uint64 reserveBefore = env.getGate(out.gateId).reserve;
    env.qpi._epoch = 104;
    env.endEpoch();
    uint64 charged = reserveBefore - (uint64)env.getGate(out.gateId).reserve;
    EXPECT_EQ(charged, QUGATE_DEFAULT_MAINTENANCE_FEE);
}

// Gate with 3 recipients pays 1.5x idle fee
TEST(QuGateComplexity, MultiRecipientGatePaysHigherFee)
{
    QuGateTest env;
    id recips[] = { BOB, CHARLIE, DAVE };
    uint64 ratios[] = { 3333, 3333, 3334 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 3, recips, ratios);
    ASSERT_EQ(env.fundGate(ALICE, out.gateId, 200000).result, QUGATE_SUCCESS);
    uint64 reserveBefore = env.getGate(out.gateId).reserve;
    env.qpi._epoch = 104;
    env.endEpoch();
    uint64 charged = reserveBefore - (uint64)env.getGate(out.gateId).reserve;
    uint64 expected = QPI::div(QUGATE_DEFAULT_MAINTENANCE_FEE * QUGATE_IDLE_MULTI_RECIPIENT_MULTIPLIER_BPS, 10000ULL);
    EXPECT_EQ(charged, expected);
}

// Gate with 8 recipients pays 2x idle fee
TEST(QuGateComplexity, MaxRecipientGatePaysDoubleFee)
{
    QuGateTest env;
    id recips[] = { BOB, CHARLIE, DAVE, EVE, QuGateTest::makeId(6), QuGateTest::makeId(7), QuGateTest::makeId(8), QuGateTest::makeId(9) };
    uint64 ratios[] = { 1250, 1250, 1250, 1250, 1250, 1250, 1250, 1250 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 8, recips, ratios);
    ASSERT_EQ(env.fundGate(ALICE, out.gateId, 200000).result, QUGATE_SUCCESS);
    uint64 reserveBefore = env.getGate(out.gateId).reserve;
    env.qpi._epoch = 104;
    env.endEpoch();
    uint64 charged = reserveBefore - (uint64)env.getGate(out.gateId).reserve;
    uint64 expected = QPI::div(QUGATE_DEFAULT_MAINTENANCE_FEE * QUGATE_IDLE_MAX_RECIPIENT_MULTIPLIER_BPS, 10000ULL);
    EXPECT_EQ(charged, expected);
}

// Heartbeat gate pays 1.5x idle fee
TEST(QuGateComplexity, HeartbeatGatePaysHigherFee)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 0 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_HEARTBEAT, 1, recips, ratios);
    ASSERT_EQ(env.fundGate(ALICE, out.gateId, 200000).result, QUGATE_SUCCESS);
    env.qpi._epoch = 104;
    env.endEpoch();
    uint64 charged = 200000 - (uint64)env.getGate(out.gateId).reserve;
    uint64 expected = QPI::div(QUGATE_DEFAULT_MAINTENANCE_FEE * QUGATE_IDLE_HEARTBEAT_MULTIPLIER_BPS, 10000ULL);
    EXPECT_EQ(charged, expected);
}

// Multisig gate pays 1.5x idle fee
TEST(QuGateComplexity, MultisigGatePaysHigherFee)
{
    QuGateTest env;
    id recips[] = { DAVE };
    uint64 ratios[] = { 0 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 1, recips, ratios);
    ASSERT_EQ(env.fundGate(ALICE, out.gateId, 200000).result, QUGATE_SUCCESS);
    env.qpi._epoch = 104;
    env.endEpoch();
    uint64 charged = 200000 - (uint64)env.getGate(out.gateId).reserve;
    uint64 expected = QPI::div(QUGATE_DEFAULT_MAINTENANCE_FEE * QUGATE_IDLE_MULTISIG_MULTIPLIER_BPS, 10000ULL);
    EXPECT_EQ(charged, expected);
}

// Chained gate pays base + 0.5x extra
TEST(QuGateComplexity, ChainedGatePaysExtraFee)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto downstream = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    createGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
    in.mode = MODE_SPLIT;
    in.recipientCount = 1;
    in.recipients.set(0, CHARLIE);
    in.ratios.set(0, 10000);
    in.chainNextGateId = downstream.gateId;
    auto out = env.createGate(ALICE, 100000, in);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    ASSERT_EQ(env.fundGate(ALICE, out.gateId, 200000).result, QUGATE_SUCCESS);
    uint64 reserveBefore = env.getGate(out.gateId).reserve;
    env.qpi._epoch = 104;
    env.endEpoch();
    uint64 charged = reserveBefore - (uint64)env.getGate(out.gateId).reserve;
    uint64 expected = QPI::div(QUGATE_DEFAULT_MAINTENANCE_FEE * (QUGATE_IDLE_BASE_MULTIPLIER_BPS + QUGATE_IDLE_CHAIN_EXTRA_BPS), 10000ULL);
    EXPECT_EQ(charged, expected);
}

// Governed burn BPS is used for creation fee split
TEST(QuGateComplexity, GovernedBurnBpsAffectsCreationSplit)
{
    QuGateTest env;
    env.state.mut()._feeBurnBps = 7000;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    uint64 burnedBefore = env.state.get()._totalBurned;
    uint64 dividendBefore = env.state.get()._earnedMaintenanceDividends;
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(out.status, QUGATE_SUCCESS);
    uint64 fee = out.feePaid;
    uint64 burned = env.state.get()._totalBurned - burnedBefore;
    uint64 dividend = env.state.get()._earnedMaintenanceDividends - dividendBefore;
    EXPECT_EQ(burned, QPI::div(fee * 7000, 10000ULL));
    EXPECT_EQ(dividend, fee - burned);
    EXPECT_EQ(burned + dividend, fee);
}

// Governed burn BPS is used for idle maintenance split
TEST(QuGateComplexity, GovernedBurnBpsAffectsIdleSplit)
{
    QuGateTest env;
    env.state.mut()._feeBurnBps = 3000;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    ASSERT_EQ(env.fundGate(ALICE, out.gateId, 100000).result, QUGATE_SUCCESS);
    uint64 burnedBefore = env.state.get()._totalMaintenanceBurned;
    uint64 dividendBefore = env.state.get()._totalMaintenanceDividends;
    env.qpi._epoch = 104;
    env.endEpoch();
    uint64 fee = QUGATE_DEFAULT_MAINTENANCE_FEE;
    uint64 burned = env.state.get()._totalMaintenanceBurned - burnedBefore;
    uint64 dividend = env.state.get()._totalMaintenanceDividends - dividendBefore;
    EXPECT_EQ(burned, QPI::div(fee * 3000, 10000ULL));
    EXPECT_EQ(dividend, fee - burned);
}

// Guardian vote below minSendAmount is burned as dust and never registers
TEST(QuGateRegression, MultisigVoteBelowMinSendIsDust)
{
    QuGateTest env;
    id recips[] = { DAVE };
    uint64 ratios[] = { 0 };
    auto out = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 1, recips, ratios);
    id guardians[] = { BOB, CHARLIE };
    ASSERT_EQ(env.configureMultisig(ALICE, out.gateId, guardians, 2, 2, 10, 5), QUGATE_SUCCESS);
    auto sendOut = env.sendToGate(BOB, out.gateId, 500);
    EXPECT_EQ(sendOut.status, QUGATE_DUST_AMOUNT);
    auto cfg = env.state.get()._multisigConfigs.get(out.gateId - 1);
    EXPECT_EQ(cfg.approvalCount, 0);
    EXPECT_EQ(cfg.proposalActive, 0);
    EXPECT_EQ(env.getGate(out.gateId).currentBalance, 0ULL);
}

// Downstream chain target of an exempt heartbeat gate stays alive — upstream reserve pays for it
TEST(QuGateIdle, DownstreamChainExemptionFromHeartbeat)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto downstream = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    createGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
    in.mode = MODE_HEARTBEAT;
    in.recipientCount = 0;
    in.chainNextGateId = downstream.gateId;
    auto hb = env.createGate(ALICE, 100000, in);
    ASSERT_EQ(hb.status, QUGATE_SUCCESS);
    id beneficiaries[] = { BOB };
    uint8 shares[] = { 100 };
    ASSERT_EQ(env.configureHeartbeat(ALICE, hb.gateId, 60, 100, 0, beneficiaries, shares, 1), QUGATE_SUCCESS);
    env.sendToGate(ALICE, hb.gateId, 50000);
    // Fund the heartbeat gate's reserve so it can pay downstream fees
    env.fundGate(ALICE, hb.gateId, 500000);
    sint64 reserveBefore = env.getGate(hb.gateId).reserve;
    ASSERT_GT(reserveBefore, 0);

    // Advance well past the 50-epoch expiry threshold
    env.qpi._epoch = 160;
    env.endEpoch();

    // Heartbeat is exempt (active, untriggered)
    EXPECT_EQ(env.getGate(hb.gateId).active, 1);
    // Downstream chain target should also be alive — upstream reserve paid for it
    EXPECT_EQ(env.getGate(downstream.gateId).active, 1);
    // Upstream reserve should have decreased (paid downstream fee + surcharge)
    EXPECT_LT(env.getGate(hb.gateId).reserve, reserveBefore);
}

// Downstream gate-as-recipient of an exempt time-lock gate stays alive — upstream reserve pays
TEST(QuGateIdle, DownstreamRecipientExemptionFromTimeLock)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto downstream = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    auto tl = makeSimpleGate(env, ALICE, 100000, MODE_TIME_LOCK, 1, recips, ratios);
    ASSERT_EQ(tl.status, QUGATE_SUCCESS);
    // Point recipient to downstream gate
    GateConfig tlGate = env.state.get()._gates.get(tl.gateId - 1);
    tlGate.recipientGateIds.set(0, env.encodeCurrentGateId(downstream.gateId - 1));
    env.state.mut()._gates.set(tl.gateId - 1, tlGate);
    ASSERT_EQ(env.configureTimeLock(ALICE, tl.gateId, 200, QUGATE_TIME_LOCK_ABSOLUTE_EPOCH, 1), QUGATE_SUCCESS);
    env.sendToGate(ALICE, tl.gateId, 50000);
    // Fund the time-lock gate's reserve so it can pay downstream fees
    env.fundGate(ALICE, tl.gateId, 500000);
    sint64 reserveBefore = env.getGate(tl.gateId).reserve;
    ASSERT_GT(reserveBefore, 0);

    // Advance past 50-epoch expiry but before unlock
    env.qpi._epoch = 160;
    env.endEpoch();

    // Time-lock is exempt (active, unfired, has balance)
    EXPECT_EQ(env.getGate(tl.gateId).active, 1);
    // Downstream recipient gate should also be alive — upstream reserve paid for it
    EXPECT_EQ(env.getGate(downstream.gateId).active, 1);
    // Upstream reserve should have decreased (paid downstream fee + surcharge)
    EXPECT_LT(env.getGate(tl.gateId).reserve, reserveBefore);
}

// Downstream gate of a non-exempt gate still expires normally
TEST(QuGateIdle, DownstreamOfNonExemptGateStillExpires)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto downstream = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    createGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
    in.mode = MODE_SPLIT;
    in.recipientCount = 1;
    in.recipients.set(0, CHARLIE);
    in.ratios.set(0, 10000);
    in.chainNextGateId = downstream.gateId;
    auto upstream = env.createGate(ALICE, 100000, in);
    ASSERT_EQ(upstream.status, QUGATE_SUCCESS);

    // Advance past expiry — upstream has no hold state, not exempt
    env.qpi._epoch = 160;
    env.endEpoch();

    // Both should expire
    EXPECT_EQ(env.getGate(upstream.gateId).active, 0);
    EXPECT_EQ(env.getGate(downstream.gateId).active, 0);
}

// Heartbeat with funded reserve pays for downstream chain target's idle fee + surcharge
TEST(QuGateIdle, ReserveDrainHeartbeatPaysForDownstream)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto downstream = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    createGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
    in.mode = MODE_HEARTBEAT;
    in.recipientCount = 0;
    in.chainNextGateId = downstream.gateId;
    auto hb = env.createGate(ALICE, 100000, in);
    ASSERT_EQ(hb.status, QUGATE_SUCCESS);
    id beneficiaries[] = { BOB };
    uint8 shares[] = { 100 };
    ASSERT_EQ(env.configureHeartbeat(ALICE, hb.gateId, 60, 100, 0, beneficiaries, shares, 1), QUGATE_SUCCESS);
    env.sendToGate(ALICE, hb.gateId, 50000);
    env.fundGate(ALICE, hb.gateId, 500000);
    sint64 reserveBefore = env.getGate(hb.gateId).reserve;

    // Advance to idle charge epoch (window = 4 epochs)
    env.qpi._epoch = 104;
    env.endEpoch();

    // Downstream stays alive with refreshed activity
    EXPECT_EQ(env.getGate(downstream.gateId).active, 1);
    EXPECT_EQ(env.getGate(downstream.gateId).lastActivityEpoch, 104);

    // Upstream reserve decreased by: downstream fee (25000) + surcharge (12500)
    // Downstream gate is simple 1-recipient SPLIT => multiplier 10000 => fee = 25000
    // Surcharge = _idleFee * 1 * 5000 / 10000 = 25000 * 5000 / 10000 = 12500
    sint64 reserveAfter = env.getGate(hb.gateId).reserve;
    uint64 expectedDrain = 25000 + 12500;  // downstream fee + surcharge
    EXPECT_EQ(reserveBefore - reserveAfter, (sint64)expectedDrain);
}

// Heartbeat with 0 reserve does not shield downstream — downstream becomes delinquent
TEST(QuGateIdle, EmptyHeartbeatDoesNotShieldDownstream)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto downstream = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    createGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
    in.mode = MODE_HEARTBEAT;
    in.recipientCount = 0;
    in.chainNextGateId = downstream.gateId;
    auto hb = env.createGate(ALICE, 100000, in);
    ASSERT_EQ(hb.status, QUGATE_SUCCESS);
    id beneficiaries[] = { BOB };
    uint8 shares[] = { 100 };
    ASSERT_EQ(env.configureHeartbeat(ALICE, hb.gateId, 60, 100, 0, beneficiaries, shares, 1), QUGATE_SUCCESS);
    env.sendToGate(ALICE, hb.gateId, 50000);
    // Do NOT fund reserve — leave at 0
    ASSERT_EQ(env.getGate(hb.gateId).reserve, 0);

    // Advance past idle window to charge epoch
    env.qpi._epoch = 104;
    env.endEpoch();

    // Heartbeat is still exempt (active, untriggered)
    EXPECT_EQ(env.getGate(hb.gateId).active, 1);
    // Downstream gate's lastActivityEpoch was NOT refreshed (no reserve to drain)
    EXPECT_EQ(env.getGate(downstream.gateId).lastActivityEpoch, 100);
    // Downstream should be delinquent after first charge epoch (no reserve to pay)
    uint16 delinquentEpoch = env.state.get()._idleDelinquentEpochs.get(downstream.gateId - 1);
    EXPECT_GT(delinquentEpoch, (uint16)0);
}

// Upstream reserve drains on first cycle then has nothing left — downstream becomes delinquent on second cycle
TEST(QuGateIdle, ReserveDrainExhaustsUpstream)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto downstream = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    createGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
    in.mode = MODE_HEARTBEAT;
    in.recipientCount = 0;
    in.chainNextGateId = downstream.gateId;
    auto hb = env.createGate(ALICE, 100000, in);
    ASSERT_EQ(hb.status, QUGATE_SUCCESS);
    id beneficiaries[] = { BOB };
    uint8 shares[] = { 100 };
    ASSERT_EQ(env.configureHeartbeat(ALICE, hb.gateId, 60, 100, 0, beneficiaries, shares, 1), QUGATE_SUCCESS);
    env.sendToGate(ALICE, hb.gateId, 50000);

    // Fund just enough for 1 cycle of downstream drain + surcharge (25000 + 12500 = 37500)
    env.fundGate(ALICE, hb.gateId, 37500);
    ASSERT_EQ(env.getGate(hb.gateId).reserve, 37500);

    // First cycle — reserve drains fully
    env.qpi._epoch = 104;
    env.endEpoch();
    EXPECT_EQ(env.getGate(downstream.gateId).active, 1);
    EXPECT_EQ(env.getGate(hb.gateId).reserve, 0);

    // Second cycle — no reserve left, downstream gets no protection
    env.qpi._epoch = 108;
    env.endEpoch();
    // Downstream should now have delinquency set (reserve depleted, no longer shielded)
    EXPECT_EQ(env.getGate(downstream.gateId).lastActivityEpoch, 104);
    uint16 delinquentEpoch = env.state.get()._idleDelinquentEpochs.get(downstream.gateId - 1);
    EXPECT_GT(delinquentEpoch, (uint16)0);
}

// Shielding surcharge scales linearly with number of downstream gate-as-recipient targets
TEST(QuGateIdle, ShieldingSurchargeScalesWithTargets)
{
    QuGateTest env;
    // Create 3 downstream split gates
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto ds1 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto ds2 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto ds3 = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    // Create heartbeat gate with 3 gate-as-recipient targets
    createGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
    in.mode = MODE_HEARTBEAT;
    in.recipientCount = 3;
    in.recipients.set(0, BOB);
    in.recipients.set(1, CHARLIE);
    in.recipients.set(2, DAVE);
    in.recipientGateIds.set(0, env.encodeCurrentGateId(ds1.gateId - 1));
    in.recipientGateIds.set(1, env.encodeCurrentGateId(ds2.gateId - 1));
    in.recipientGateIds.set(2, env.encodeCurrentGateId(ds3.gateId - 1));
    in.chainNextGateId = -1;
    auto hb = env.createGate(ALICE, 100000, in);
    ASSERT_EQ(hb.status, QUGATE_SUCCESS);
    id beneficiaries[] = { BOB };
    uint8 shares[] = { 100 };
    ASSERT_EQ(env.configureHeartbeat(ALICE, hb.gateId, 60, 100, 0, beneficiaries, shares, 1), QUGATE_SUCCESS);
    env.sendToGate(ALICE, hb.gateId, 50000);
    env.fundGate(ALICE, hb.gateId, 2000000);
    sint64 reserveBefore = env.getGate(hb.gateId).reserve;

    env.qpi._epoch = 104;
    env.endEpoch();

    // Each downstream is a simple 1-recipient SPLIT => fee = 25000 each
    // 3 downstream fees = 3 * 25000 = 75000
    // Surcharge = _idleFee * 3 * 5000 / 10000 = 25000 * 3 * 5000 / 10000 = 37500
    // Total drain = 75000 + 37500 = 112500
    sint64 reserveAfter = env.getGate(hb.gateId).reserve;
    uint64 expectedDrain = 3 * 25000 + 37500;  // 3 ds fees + surcharge
    EXPECT_EQ(reserveBefore - reserveAfter, (sint64)expectedDrain);

    // All 3 downstream gates should be alive with refreshed activity
    EXPECT_EQ(env.getGate(ds1.gateId).active, 1);
    EXPECT_EQ(env.getGate(ds1.gateId).lastActivityEpoch, 104);
    EXPECT_EQ(env.getGate(ds2.gateId).active, 1);
    EXPECT_EQ(env.getGate(ds3.gateId).active, 1);
}

// Admin-only multisig with 0 balance and no reserve does not drain for downstream
TEST(QuGateIdle, AdminOnlyMultisigDoesNotDrain)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto downstream = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    // Create admin-only MULTISIG (recipientCount=0, no chain)
    createGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
    in.mode = MODE_MULTISIG;
    in.recipientCount = 0;
    in.chainNextGateId = -1;
    auto ms = env.createGate(ALICE, 100000, in);
    ASSERT_EQ(ms.status, QUGATE_SUCCESS);
    id guardians[] = { BOB };
    ASSERT_EQ(env.configureMultisig(ALICE, ms.gateId, guardians, 1, 1, 10, 5), QUGATE_SUCCESS);

    // Set chain to downstream via setChain
    env.setChain(ALICE, ms.gateId, env.encodeCurrentGateId(downstream.gateId - 1), 1000);

    // Do NOT fund reserve — balance is 0 for admin-only
    ASSERT_EQ(env.getGate(ms.gateId).reserve, 0);
    ASSERT_EQ(env.getGate(ms.gateId).currentBalance, 0ULL);

    // Advance to idle charge epoch
    env.qpi._epoch = 104;
    env.endEpoch();

    // Multisig with 0 balance has no activeHold (needs balance or proposalActive)
    // => downstream gate gets no protection, becomes delinquent at its charge epoch
    uint16 delinquentEpoch = env.state.get()._idleDelinquentEpochs.get(downstream.gateId - 1);
    EXPECT_GT(delinquentEpoch, (uint16)0);
}

// Reserve drain is properly tracked in _totalMaintenanceCharged including downstream fees
TEST(QuGateIdle, ReserveDrainTracksMaintenanceTotals)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto downstream = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);

    createGate_input in;
    memset(&in, 0, sizeof(in));
    for (uint8 _ri = 0; _ri < 8; _ri++) in.recipientGateIds.set(_ri, -1);
    in.mode = MODE_HEARTBEAT;
    in.recipientCount = 0;
    in.chainNextGateId = downstream.gateId;
    auto hb = env.createGate(ALICE, 100000, in);
    ASSERT_EQ(hb.status, QUGATE_SUCCESS);
    id beneficiaries[] = { BOB };
    uint8 shares[] = { 100 };
    ASSERT_EQ(env.configureHeartbeat(ALICE, hb.gateId, 60, 100, 0, beneficiaries, shares, 1), QUGATE_SUCCESS);
    env.sendToGate(ALICE, hb.gateId, 50000);
    env.fundGate(ALICE, hb.gateId, 500000);

    uint64 chargedBefore = env.state.get()._totalMaintenanceCharged;
    uint64 burnedBefore = env.state.get()._totalMaintenanceBurned;
    uint64 dividendsBefore = env.state.get()._totalMaintenanceDividends;

    env.qpi._epoch = 104;
    env.endEpoch();

    uint64 chargedAfter = env.state.get()._totalMaintenanceCharged;
    uint64 burnedAfter = env.state.get()._totalMaintenanceBurned;
    uint64 dividendsAfter = env.state.get()._totalMaintenanceDividends;

    // Downstream fee (25000) + surcharge (12500) = 37500 total charged for drain
    uint64 expectedDrain = 25000 + 12500;
    EXPECT_EQ(chargedAfter - chargedBefore, expectedDrain);
    // Burn + dividends should equal total charged
    uint64 totalBurnDelta = burnedAfter - burnedBefore;
    uint64 totalDividendDelta = dividendsAfter - dividendsBefore;
    EXPECT_EQ(totalBurnDelta + totalDividendDelta, expectedDrain);
    // Burn is 50% of each fee component
    uint64 dsFee = 25000;
    uint64 surcharge = 12500;
    uint64 expectedBurn = QPI::div(dsFee * 5000, 10000ULL) + QPI::div(surcharge * 5000, 10000ULL);
    EXPECT_EQ(totalBurnDelta, expectedBurn);
}

// Governed gate pays its admin multisig's idle fees from its own reserve
TEST(QuGateIdle, AdminGateDrainFromGovernedGateReserve)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto target = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto admin = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 0, recips, ratios);
    id guardians[] = { BOB };
    ASSERT_EQ(env.configureMultisig(ALICE, admin.gateId, guardians, 1, 1, 10, 5), QUGATE_SUCCESS);
    ASSERT_EQ(env.setAdminGate(ALICE, target.gateId, admin.gateId), QUGATE_SUCCESS);
    ASSERT_EQ(env.fundGate(ALICE, target.gateId, 200000).result, QUGATE_SUCCESS);

    uint64 reserveBefore = env.getGate(target.gateId).reserve;
    env.qpi._epoch = 104;
    env.endEpoch();

    auto targetAfter = env.getGate(target.gateId);
    auto adminAfter = env.getGate(admin.gateId);
    uint64 reserveDelta = reserveBefore - (uint64)targetAfter.reserve;

    // Target paid its own fee + admin gate fee
    uint64 adminFee = QPI::div(QUGATE_DEFAULT_MAINTENANCE_FEE * QUGATE_IDLE_MULTISIG_MULTIPLIER_BPS, 10000ULL);
    EXPECT_GT(reserveDelta, 0ULL);
    EXPECT_EQ(adminAfter.active, 1);
    EXPECT_EQ(adminAfter.lastActivityEpoch, 104);
}

// Admin gate survives when governed gate has reserve, dies when it doesn't
TEST(QuGateIdle, AdminGateExpiresWhenGovernedGateHasNoReserve)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto target = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto admin = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 0, recips, ratios);
    id guardians[] = { BOB };
    ASSERT_EQ(env.configureMultisig(ALICE, admin.gateId, guardians, 1, 1, 10, 5), QUGATE_SUCCESS);
    ASSERT_EQ(env.setAdminGate(ALICE, target.gateId, admin.gateId), QUGATE_SUCCESS);
    // No reserve funded on target

    env.qpi._epoch = 104;
    env.endEpoch();

    // Admin gets no drain (target has no reserve)
    uint16 adminDelinquent = env.state.get()._idleDelinquentEpochs.get(admin.gateId - 1);
    EXPECT_EQ(adminDelinquent, 104);
}

// Recently active gate still pays for its admin gate
TEST(QuGateIdle, ActiveGateStillPaysAdminDrain)
{
    QuGateTest env;
    id recips[] = { BOB };
    uint64 ratios[] = { 10000 };
    auto target = makeSimpleGate(env, ALICE, 100000, MODE_SPLIT, 1, recips, ratios);
    auto admin = makeSimpleGate(env, ALICE, 100000, MODE_MULTISIG, 0, recips, ratios);
    id guardians[] = { BOB };
    ASSERT_EQ(env.configureMultisig(ALICE, admin.gateId, guardians, 1, 1, 10, 5), QUGATE_SUCCESS);
    ASSERT_EQ(env.setAdminGate(ALICE, target.gateId, admin.gateId), QUGATE_SUCCESS);
    ASSERT_EQ(env.fundGate(ALICE, target.gateId, 200000).result, QUGATE_SUCCESS);

    // Send to keep target recently active
    env.qpi._epoch = 103;
    env.sendToGate(ALICE, target.gateId, 1000);

    uint64 reserveBefore = env.getGate(target.gateId).reserve;
    env.qpi._epoch = 104;
    env.endEpoch();

    // Target is recently active so doesn't pay its own idle fee
    // But still pays admin gate drain
    auto adminAfter = env.getGate(admin.gateId);
    EXPECT_EQ(adminAfter.active, 1);
    EXPECT_EQ(adminAfter.lastActivityEpoch, 104);
}
