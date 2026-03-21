// QuGate.h - Programmable Payment Gate Contract
// Short name: QUGATE
// Description: Universal payment routing with predefined gate modes.
//   Create gates with configurable rules, point payments at them,
//   and QU flows according to the logic.
//
// Gate Modes:
//   SPLIT       - Distribute to N addresses by ratio (e.g. 40/30/20/10)
//   ROUND_ROBIN - Cycle through addresses, one per payment
//   THRESHOLD   - Accumulate until amount reached, then forward
//   RANDOM      - Select one recipient per payment using tick-based entropy
//   CONDITIONAL - Only forward if sender matches whitelist, else bounce
//   ORACLE      - Oracle-triggered distribution based on price/time conditions
//
// Anti-Spam:
//   - Escalating creation fee: cost increases as capacity fills
//   - Gate expiry: inactive gates auto-close after N epochs
//   - Dust burn: sends below minimum are burned
//   - All fees are deflationary (burned, not accumulated)

//
// Architecture:
//   Gates are stored in a flat Array<GateConfig, QUGATE_MAX_GATES> indexed by slot (0-based
//   internally). External callers use versioned gate IDs:
//     gateId = ((generation + 1) << QUGATE_GATE_ID_SLOT_BITS) | slotIndex
//   The generation counter increments each time a slot is recycled, making stale IDs invalid.
//
//   Oracle gates (QUGATE_MODE_ORACLE) subscribe to a Qubic price oracle at the start of each
//   epoch (BEGIN_EPOCH). The oracle callback (OraclePriceNotification) fires once per configured
//   interval; when the trigger condition is met, accumulated funds are distributed and optionally
//   forwarded via chain gates.
//
//   Chain gates link up to QUGATE_MAX_CHAIN_DEPTH gates in sequence. After a gate distributes
//   funds, the forwarded amount is passed to the next gate in the chain (routeToGate). Each hop
//   burns QUGATE_CHAIN_HOP_FEE QU as an anti-spam measure.
//
//   All fees (creation, dust, chain hops) are burned via qpi.burn(). No QU accumulates in the
//   contract. The contract is purely deflationary.
//

using namespace QPI;

// Contract index — Pulse took index 24, QuGate uses 25
#ifndef CONTRACT_INDEX
#define CONTRACT_INDEX 25
#endif

// Capacity scales with network via X_MULTIPLIER
constexpr uint64 QUGATE_INITIAL_MAX_GATES = 4096;
constexpr uint64 QUGATE_MAX_GATES = QUGATE_INITIAL_MAX_GATES * X_MULTIPLIER;
constexpr uint64 QUGATE_MAX_RECIPIENTS = 8;
constexpr uint64 QUGATE_MAX_RATIO = 10000;       // Max ratio per recipient (prevents overflow)

// Default fees — initial values, changeable via shareholder vote
constexpr uint64 QUGATE_DEFAULT_CREATION_FEE = 100000;
constexpr uint64 QUGATE_DEFAULT_MIN_SEND = 1000;

// Escalating fee: fee = baseFee * (1 + activeGates / FEE_ESCALATION_STEP)
constexpr uint64 QUGATE_FEE_ESCALATION_STEP = 1024;

// Gate expiry: gates with no activity for this many epochs auto-close
constexpr uint64 QUGATE_DEFAULT_EXPIRY_EPOCHS = 50;

// Versioned gate ID encoding: gateId = ((generation+1) << SLOT_BITS) | slotIndex
constexpr uint64 QUGATE_GATE_ID_SLOT_BITS = 20;
constexpr uint64 QUGATE_GATE_ID_SLOT_MASK = (1ULL << QUGATE_GATE_ID_SLOT_BITS) - 1; // 0xFFFFF

// Query limits
constexpr uint64 QUGATE_MAX_OWNER_GATES = 16;    // max gates returned by getGatesByOwner
constexpr uint64 QUGATE_MAX_BATCH_GATES = 32;    // max gates in getGateBatch

// Gate modes
constexpr uint8 QUGATE_MODE_SPLIT = 0;
constexpr uint8 QUGATE_MODE_ROUND_ROBIN = 1;
constexpr uint8 QUGATE_MODE_THRESHOLD = 2;
constexpr uint8 QUGATE_MODE_RANDOM = 3;
constexpr uint8 QUGATE_MODE_CONDITIONAL = 4;
constexpr uint8 QUGATE_MODE_ORACLE = 5;
constexpr uint8 QUGATE_MODE_INHERITANCE = 6;  // Dead man's switch — keepAlive() or epoch-triggered payout

// Oracle condition types
constexpr uint8 QUGATE_ORACLE_COND_PRICE_ABOVE = 0;
constexpr uint8 QUGATE_ORACLE_COND_PRICE_BELOW = 1;
constexpr uint8 QUGATE_ORACLE_COND_TIME_AFTER  = 2;

// Oracle trigger modes
constexpr uint8 QUGATE_ORACLE_TRIGGER_ONCE      = 0;
constexpr uint8 QUGATE_ORACLE_TRIGGER_RECURRING = 1;

// Oracle fees and periods
constexpr sint64 QUGATE_ORACLE_SUBSCRIPTION_FEE = 10000; // QU per epoch (1-min interval)
constexpr uint32 QUGATE_ORACLE_NOTIFY_PERIOD_MS = 60000; // 1 minute

// Chain gate constants
constexpr uint8  QUGATE_MAX_CHAIN_DEPTH            = 3;
constexpr sint64 QUGATE_CHAIN_HOP_FEE              = 1000;
constexpr uint32 QUGATE_LOG_CHAIN_HOP              = 12;
constexpr uint32 QUGATE_LOG_CHAIN_CYCLE            = 13;
constexpr uint32 QUGATE_LOG_CHAIN_HOP_INSUFFICIENT = 14;

// Status codes — used in procedure outputs and logger _type
constexpr sint64 QUGATE_SUCCESS                = 0;   // Operation completed successfully
constexpr sint64 QUGATE_INVALID_GATE_ID        = -1;  // gateId is 0, exceeds gateCount, or wrong generation
constexpr sint64 QUGATE_GATE_NOT_ACTIVE        = -2;  // Gate exists but has been closed or expired
constexpr sint64 QUGATE_UNAUTHORIZED           = -3;  // Caller is not the gate owner
constexpr sint64 QUGATE_INVALID_MODE           = -4;  // Mode value exceeds QUGATE_MODE_ORACLE (5)
constexpr sint64 QUGATE_INVALID_RECIPIENT_COUNT = -5; // recipientCount is 0 or exceeds QUGATE_MAX_RECIPIENTS
constexpr sint64 QUGATE_INVALID_RATIO          = -6;  // Individual ratio exceeds QUGATE_MAX_RATIO, or total ratio is 0
constexpr sint64 QUGATE_INSUFFICIENT_FEE       = -7;  // invocationReward < escalated creation fee
constexpr sint64 QUGATE_NO_FREE_SLOTS          = -8;  // Free-list empty and gateCount at QUGATE_MAX_GATES
constexpr sint64 QUGATE_DUST_AMOUNT            = -9;  // Send amount is 0 or below _minSendAmount (burned)
constexpr sint64 QUGATE_INVALID_THRESHOLD      = -10; // Threshold is 0 for QUGATE_MODE_THRESHOLD gates
constexpr sint64 QUGATE_INVALID_SENDER_COUNT   = -11; // allowedSenderCount exceeds QUGATE_MAX_RECIPIENTS
constexpr sint64 QUGATE_CONDITIONAL_REJECTED   = -12; // Sender not in allowedSenders list; amount bounced
constexpr sint64 QUGATE_INVALID_ORACLE_CONFIG  = -13; // Invalid oracle condition, threshold, or trigger mode
constexpr sint64 QUGATE_INVALID_CHAIN          = -14; // Chain target invalid, depth exceeded, or cycle detected
constexpr sint64 QUGATE_OWNER_MISMATCH         = -15; // gate.owner != expectedOwner in sendToGateVerified
constexpr sint64 QUGATE_INHERITANCE_TRIGGERED  = -16; // keepAlive() called after inheritance already triggered
constexpr sint64 QUGATE_INHERITANCE_NOT_ACTIVE = -17; // keepAlive() or configureInheritance() on non-INHERITANCE gate
constexpr sint64 QUGATE_INVALID_INHERITANCE    = -18; // Invalid inheritance config (bad shares, percent, threshold)

// Log type constants (positive = success events, high numbers = actions)
constexpr uint32 QUGATE_LOG_GATE_CREATED = 1;
constexpr uint32 QUGATE_LOG_GATE_CLOSED = 2;
constexpr uint32 QUGATE_LOG_GATE_UPDATED = 3;
constexpr uint32 QUGATE_LOG_PAYMENT_FORWARDED = 4;
constexpr uint32 QUGATE_LOG_PAYMENT_BOUNCED = 5;
constexpr uint32 QUGATE_LOG_DUST_BURNED = 6;
constexpr uint32 QUGATE_LOG_FEE_CHANGED = 7;
constexpr uint32 QUGATE_LOG_GATE_EXPIRED = 8;
constexpr uint32 QUGATE_LOG_ORACLE_TRIGGERED  = 9;
constexpr uint32 QUGATE_LOG_ORACLE_EXHAUSTED  = 10;
constexpr uint32 QUGATE_LOG_ORACLE_SUBSCRIBED = 11;
constexpr uint32 QUGATE_LOG_INHERITANCE_CONFIGURED = 15;  // configureInheritance() called
constexpr uint32 QUGATE_LOG_INHERITANCE_KEEPALIVE  = 16;  // keepAlive() called, epoch reset
constexpr uint32 QUGATE_LOG_INHERITANCE_TRIGGERED  = 17;  // threshold exceeded, inheritance triggered
constexpr uint32 QUGATE_LOG_INHERITANCE_PAYOUT     = 18;  // recurring payout dispatched

// Failure log types use high range
constexpr uint32 QUGATE_LOG_FAIL_INVALID_GATE = 100;
constexpr uint32 QUGATE_LOG_FAIL_NOT_ACTIVE = 101;
constexpr uint32 QUGATE_LOG_FAIL_UNAUTHORIZED = 102;
constexpr uint32 QUGATE_LOG_FAIL_INVALID_PARAMS = 103;
constexpr uint32 QUGATE_LOG_FAIL_INSUFFICIENT_FEE = 104;
constexpr uint32 QUGATE_LOG_FAIL_NO_SLOTS = 105;
constexpr uint32 QUGATE_LOG_FAIL_OWNER_MISMATCH = 106;

// Asset name for shareholder proposals
constexpr uint64 QUGATE_CONTRACT_ASSET_NAME = 76228174763345ULL; // "QUGATE" as uint64 little-endian

// Future extension struct (Qubic convention)
struct QUGATE2
{
};

// =============================================
// Inheritance gate supporting structs
// (defined outside QUGATE so they can be used in StateData)
// =============================================

// Per-gate inheritance configuration
struct InheritanceConfig
{
    uint32  thresholdEpochs;       // epochs without keepAlive() before trigger (>= 1)
    uint32  lastKeepAliveEpoch;    // epoch of last keepAlive() call (or configureInheritance)
    uint8   payoutPercentPerEpoch; // % of balance to pay each epoch after trigger (1-100)
    sint64  minimumBalance;        // stop paying when balance drops below this
    uint8   active;                // 1 = inheritance mode enabled on this gate
    uint8   triggered;             // 1 once threshold exceeded
    uint32  triggerEpoch;          // epoch when triggered
};

// Per-beneficiary entry for inheritance distribution
struct InheritanceBeneficiary
{
    id      address;
    uint8   sharePercent;  // must sum to 100 across all beneficiaries for a gate
};

struct QUGATE : public ContractBase
{
public:
    // =============================================
    // Logging structs
    // =============================================

    struct QuGateLogger
    {
        uint32 _contractIndex;
        uint32 _type;           // maps to QUGATE_LOG_* constants
        uint64 gateId;
        id sender;
        sint64 amount;
        sint8 _terminator;      // Qubic logs data before this field only
    };

    // =============================================
    // Data structures for gate configuration
    // =============================================

    struct GateConfig
    {
        id     owner;               // Public key of the address that created this gate; only owner can close/update
        uint8  mode;                // Gate routing mode; one of QUGATE_MODE_* constants; immutable after creation
        uint8  recipientCount;      // Number of active recipients (1 to QUGATE_MAX_RECIPIENTS)
        uint8  active;              // 1 = gate is active and routing; 0 = closed or expired
        uint8  allowedSenderCount;  // Number of addresses in allowedSenders (0 to QUGATE_MAX_RECIPIENTS)
        uint16 createdEpoch;        // Network epoch when this gate was created
        uint16 lastActivityEpoch;   // Network epoch of last sendToGate or updateGate call; used for expiry
        uint64 totalReceived;       // Cumulative QU received by this gate across all sendToGate calls
        uint64 totalForwarded;      // Cumulative QU forwarded to recipients (excludes dust burns)
        uint64 currentBalance;      // Held balance awaiting release: THRESHOLD mode accumulates here; ORACLE mode accumulates here between oracle triggers
        uint64 threshold;           // Release threshold for THRESHOLD mode; 0 for other modes
        uint64 roundRobinIndex;     // Next recipient index for ROUND_ROBIN mode; wraps at recipientCount
        Array<id, 8> recipients;     // Recipient public keys; indices beyond recipientCount are zeroed
        Array<uint64, 8> ratios;         // Ratio per recipient for SPLIT and ORACLE modes; others may zero these
        Array<id, 8> allowedSenders; // Whitelist for CONDITIONAL mode; indices beyond allowedSenderCount are zeroed

        // Oracle mode fields — only populated when mode == QUGATE_MODE_ORACLE
        id     oracleId;               // Oracle provider identity (e.g. Price::getBinanceOracleId())
        id     oracleCurrency1;        // First currency of the price pair query
        id     oracleCurrency2;        // Second currency of the price pair query
        uint8  oracleCondition;        // Trigger condition type: QUGATE_ORACLE_COND_PRICE_ABOVE/BELOW/TIME_AFTER
        uint8  oracleTriggerMode;      // QUGATE_ORACLE_TRIGGER_ONCE (auto-close) or RECURRING (reset and continue)
        sint64 oracleThreshold;        // Trigger threshold: price ratio scaled by 1e6, or numerator value for TIME_AFTER
        sint64 oracleReserve;          // QU reserve to fund per-epoch oracle subscription fee (QUGATE_ORACLE_SUBSCRIPTION_FEE)
        sint32 oracleSubscriptionId;   // Subscription ID returned by SUBSCRIBE_ORACLE; -1 if not subscribed

        // Chain gate fields — only active when chainNextGateId != -1
        sint64 chainNextGateId;   // Versioned gate ID of next gate in chain; -1 if this is a terminal gate
        sint64 chainReserve;      // QU reserve to pay hop fees when forwarded amount is insufficient
        uint8  chainDepth;        // This gate's position in its chain (0 = root/trigger, increments toward leaf)
    };

    // =============================================
    // Procedure inputs/outputs
    // =============================================

    // Parameters for creating a new gate. Fields irrelevant to the chosen mode should be zeroed.
    struct createGate_input
    {
        uint8 mode;
        uint8 recipientCount;
        Array<id, 8> recipients;
        Array<uint64, 8> ratios;
        uint64 threshold;
        Array<id, 8> allowedSenders;
        uint8 allowedSenderCount;

        // Oracle mode fields
        id     oracleId;
        id     oracleCurrency1;
        id     oracleCurrency2;
        uint8  oracleCondition;
        uint8  oracleTriggerMode;
        sint64 oracleThreshold;

        // Chain gate fields
        sint64 chainNextGateId;   // -1 for no chain
    };
    struct createGate_output
    {
        sint64 status;          //
        uint64 gateId;
        uint64 feePaid;         // actual fee charged (for transparency)
    };

    // Target gate to route QU through. Attach QU as invocationReward.
    struct sendToGate_input
    {
        uint64 gateId;
    };
    struct sendToGate_output
    {
        sint64 status;          //
    };

    // Closes gate and refunds held balance to owner. Gate must be active and caller must be owner.
    struct closeGate_input
    {
        uint64 gateId;
    };
    struct closeGate_output
    {
        sint64 status;          //
    };

    // Updates gate recipients/ratios/threshold. Mode cannot be changed. Owner only.
    struct updateGate_input
    {
        uint64 gateId;
        uint8 recipientCount;
        Array<id, 8> recipients;
        Array<uint64, 8> ratios;
        uint64 threshold;
        Array<id, 8> allowedSenders;
        uint8 allowedSenderCount;
    };
    struct updateGate_output
    {
        sint64 status;          //
    };

    // Adds invocationReward to a gate reserve. reserveTarget: 0=oracleReserve, 1=chainReserve.
    struct fundGate_input
    {
        sint64 gateId;
        uint8  reserveTarget;  // 0 = oracleReserve, 1 = chainReserve
    };
    struct fundGate_output
    {
        sint64 result;
    };

    // Sets or clears the chain link on a gate. nextGateId=-1 clears the chain. Owner only. Burns QUGATE_CHAIN_HOP_FEE.
    struct setChain_input
    {
        sint64 gateId;
        sint64 nextGateId;    // -1 to clear chain
    };
    struct setChain_output
    {
        sint64 result;
    };

    // Like sendToGate, but asserts gate.owner == expectedOwner before routing. Full refund if mismatch.
    struct sendToGateVerified_input
    {
        uint64 gateId;
        id     expectedOwner;   // must match gate.owner — full refund if mismatch
    };
    struct sendToGateVerified_output
    {
        sint64 status;
    };

    // Configure inheritance mode on an INHERITANCE gate.
    // Owner-only. Must be called before the gate can trigger.
    struct configureInheritance_input
    {
        uint64 gateId;
        uint32 thresholdEpochs;          // >= 1
        uint8  payoutPercentPerEpoch;     // 1–100
        sint64 minimumBalance;            // stop payouts when balance falls below this
        Array<id, 8>    beneficiaryAddresses;
        Array<uint8, 8> beneficiaryShares; // must sum to 100
        uint8  beneficiaryCount;           // 1–8
    };
    struct configureInheritance_output
    {
        sint64 status;
    };

    // Reset the keepAlive epoch counter. Owner-only. Rejected after trigger.
    struct keepAlive_input
    {
        uint64 gateId;
    };
    struct keepAlive_output
    {
        sint64 status;
        uint32 epochRecorded;
    };

    // Query inheritance config and beneficiaries for a gate.
    struct getInheritance_input
    {
        uint64 gateId;
    };
    struct getInheritance_output
    {
        uint8   active;
        uint8   triggered;
        uint32  thresholdEpochs;
        uint32  lastKeepAliveEpoch;
        uint32  triggerEpoch;
        uint8   payoutPercentPerEpoch;
        sint64  minimumBalance;
        uint8   beneficiaryCount;
        Array<id, 8>    beneficiaryAddresses;
        Array<uint8, 8> beneficiaryShares;
    };

    // =============================================
    // Function inputs/outputs (read-only queries)
    // =============================================

    struct getGate_input
    {
        uint64 gateId;
    };
    struct getGate_output
    {
        uint8 mode;
        uint8 recipientCount;
        uint8 active;
        id owner;
        uint64 totalReceived;
        uint64 totalForwarded;
        uint64 currentBalance;
        uint64 threshold;
        uint16 createdEpoch;                            //
        uint16 lastActivityEpoch;                       //
        Array<id, 8> recipients;
        Array<uint64, 8> ratios;
        Array<id, 8> allowedSenders;
        uint8 allowedSenderCount;

        // Oracle mode fields
        sint64 oracleReserve;
        sint32 oracleSubscriptionId;

        // Chain gate fields
        sint64 chainNextGateId;
        sint64 chainReserve;
        uint8  chainDepth;
    };

    struct getGateCount_input
    {
    };
    struct getGateCount_output
    {
        uint64 totalGates;
        uint64 activeGates;
        uint64 totalBurned;     //
    };

    struct getGatesByOwner_input
    {
        id owner;
    };
    struct getGatesByOwner_output
    {
        Array<uint64, QUGATE_MAX_OWNER_GATES> gateIds;
        uint64 count;
    };

    // Batch gate query
    struct getGateBatch_input
    {
        Array<uint64, QUGATE_MAX_BATCH_GATES> gateIds;
    };
    struct getGateBatch_output
    {
        Array<getGate_output, QUGATE_MAX_BATCH_GATES> gates;
    };

    // Fee query — includes current escalated fee and expiry setting
    struct getFees_input
    {
    };
    struct getFees_output
    {
        uint64 creationFee;         // base fee
        uint64 currentCreationFee;  // actual fee right now (after escalation)
        uint64 minSendAmount;
        uint64 expiryEpochs;       //
    };

public:
    // =============================================
    // Contract state (wrapped in StateData for dirty-tracking)
    // =============================================

    struct StateData
    {
        uint64 _gateCount;
        uint64 _activeGates;
        uint64 _totalBurned;        // cumulative QU burned
        Array<GateConfig, QUGATE_MAX_GATES> _gates;

        // Generation counter per slot — increments on reuse (versioned gate IDs)
        Array<uint16, QUGATE_MAX_GATES> _gateGenerations;

        // Free-list for slot reuse
        Array<uint64, QUGATE_MAX_GATES> _freeSlots;
        uint64 _freeCount;

        // Shareholder-adjustable parameters
        uint64 _creationFee;        // base creation fee
        uint64 _minSendAmount;
        uint64 _expiryEpochs;      // epochs of inactivity before auto-close

        // O(1) oracle callback lookup. Maintained by BEGIN_EPOCH and all close paths.
        // O(1) reverse lookup: oracleSubscriptionId → slot index
        HashMap<sint32, uint64, 512> _subscriptionToSlot;

        // Inheritance mode state — indexed by gate slot (same index as _gates)
        Array<InheritanceConfig, QUGATE_MAX_GATES> _inheritanceConfigs;
        // Flat beneficiary storage: index = slotIdx * 8 + beneficiaryIndex
        Array<InheritanceBeneficiary, QUGATE_MAX_GATES * 8> _inheritanceBeneficiaries;
        Array<uint8, QUGATE_MAX_GATES> _inheritanceBeneficiaryCount;
    };

    // =============================================
    // Locals — all variables declared here, not inline
    // =============================================

    struct createGate_locals
    {
        QuGateLogger logger;
        GateConfig newGate;
        uint64 totalRatio;
        uint64 i;
        uint64 slotIdx;
        uint64 currentFee;     // escalated fee
    };

    struct processSplit_input
    {
        uint64 gateIdx;
        sint64 amount;
    };
    struct processSplit_output
    {
        uint64 forwarded;
    };
    struct processSplit_locals
    {
        GateConfig gate;
        uint64 totalRatio;
        uint64 share;
        uint64 distributed;
        uint64 i;
    };

    struct processRoundRobin_input
    {
        uint64 gateIdx;
        sint64 amount;
    };
    struct processRoundRobin_output
    {
        uint64 forwarded;
    };
    struct processRoundRobin_locals
    {
        GateConfig gate;
    };

    struct processThreshold_input
    {
        uint64 gateIdx;
        sint64 amount;
    };
    struct processThreshold_output
    {
        uint64 forwarded;
    };
    struct processThreshold_locals
    {
        GateConfig gate;
    };

    struct processRandom_input
    {
        uint64 gateIdx;
        sint64 amount;
    };
    struct processRandom_output
    {
        uint64 forwarded;
    };
    struct processRandom_locals
    {
        GateConfig gate;
        uint64 recipientIdx;
    };

    struct processConditional_input
    {
        uint64 gateIdx;
        sint64 amount;
    };
    struct processConditional_output
    {
        sint64 status;
        uint64 forwarded;
    };
    struct processConditional_locals
    {
        GateConfig gate;
        uint64 i;
        uint8 senderAllowed;
    };


    // routeToGate — single-hop chain routing (non-recursive)
    struct routeToGate_input
    {
        uint64 slotIdx;
        sint64 amount;
        uint8  hopCount;
    };
    struct routeToGate_output
    {
        sint64 forwarded;
    };
    struct routeToGate_locals
    {
        GateConfig gate;
        sint64 amountAfterFee;
        QuGateLogger logger;
        processSplit_input splitIn;
        processSplit_output splitOut;
        processSplit_locals splitLocals;
        processRoundRobin_input rrIn;
        processRoundRobin_output rrOut;
        processRoundRobin_locals rrLocals;
        processThreshold_input threshIn;
        processThreshold_output threshOut;
        processThreshold_locals threshLocals;
        processRandom_input randIn;
        processRandom_output randOut;
        processRandom_locals randLocals;
        processConditional_input condIn;
        processConditional_output condOut;
        processConditional_locals condLocals;
    };

    struct fundGate_locals
    {
        QuGateLogger logger;
        GateConfig gate;
        uint64 slotIdx;
        uint64 encodedGen;
    };

    struct setChain_locals
    {
        QuGateLogger logger;
        GateConfig gate;
        GateConfig targetGate;
        uint64 slotIdx;
        uint64 encodedGen;
        uint64 targetSlot;
        uint64 targetEncodedGen;
        uint8 newDepth;
        uint64 walkSlot;
        uint8 walkStep;
        GateConfig walkGate;
    };

    struct sendToGateVerified_locals
    {
        QuGateLogger logger;
        GateConfig gate;
        sint64 amount;
        uint64 slotIdx;
        uint64 encodedGen;
        processSplit_input splitIn;
        processSplit_output splitOut;
        processSplit_locals splitLocals;
        processRoundRobin_input rrIn;
        processRoundRobin_output rrOut;
        processRoundRobin_locals rrLocals;
        processThreshold_input threshIn;
        processThreshold_output threshOut;
        processThreshold_locals threshLocals;
        processRandom_input randIn;
        processRandom_output randOut;
        processRandom_locals randLocals;
        processConditional_input condIn;
        processConditional_output condOut;
        processConditional_locals condLocals;
        routeToGate_input chainIn;
        routeToGate_output chainOut;
        routeToGate_locals chainLocals;
    };

    struct sendToGate_locals
    {
        QuGateLogger logger;
        GateConfig gate;
        sint64 amount;
        uint64 slotIdx;
        uint64 encodedGen;
        processSplit_input splitIn;
        processSplit_output splitOut;
        processSplit_locals splitLocals;
        processRoundRobin_input rrIn;
        processRoundRobin_output rrOut;
        processRoundRobin_locals rrLocals;
        processThreshold_input threshIn;
        processThreshold_output threshOut;
        processThreshold_locals threshLocals;
        processRandom_input randIn;
        processRandom_output randOut;
        processRandom_locals randLocals;
        processConditional_input condIn;
        processConditional_output condOut;
        processConditional_locals condLocals;
        // Chain forwarding
        routeToGate_input chainIn;
        routeToGate_output chainOut;
        routeToGate_locals chainLocals;
        GateConfig nextChainGate;
    };

    struct closeGate_locals
    {
        QuGateLogger logger;
        GateConfig gate;
        uint64 slotIdx;
        uint64 encodedGen;
        InheritanceConfig inhZeroCfg;
    };

    struct updateGate_locals
    {
        QuGateLogger logger;
        GateConfig gate;
        uint64 totalRatio;
        uint64 i;
        uint64 slotIdx;
        uint64 encodedGen;
    };

    struct getGate_locals
    {
        GateConfig gate;
        uint64 i;
        uint64 slotIdx;
        uint64 encodedGen;
    };

    struct getGateBatch_locals                           //
    {
        uint64 i;
        uint64 j;
        GateConfig gate;
        getGate_output entry;
        uint64 slotIdx;
        uint64 encodedGen;
    };

    struct getGatesByOwner_locals
    {
        uint64 i;
    };

    struct END_EPOCH_locals
    {
        uint64 i;
        QuGateLogger logger;
        GateConfig gate;
        // Inheritance processing
        InheritanceConfig inhCfg;
        sint64 inhBalance;
        sint64 inhPayoutTotal;
        sint64 inhShare;
        sint64 inhPriorSum;
        uint8  inhJ;
        uint8  inhK;
        InheritanceBeneficiary inhBene;
        InheritanceBeneficiary inhPriorBene;
        uint8  inhBeneCount;
        uint32 inhEpochsInactive;
    };

    // Oracle notification callback types
    typedef OracleNotificationInput<OI::Price> OraclePriceNotification_input;
    typedef NoData OraclePriceNotification_output;

    struct OraclePriceNotification_locals
    {
        uint64 slotIdx;
        GateConfig gate;
        QuGateLogger logger;
        uint8 conditionMet;
        sint64 priceScaled;
        sint64 splitAmount;
        processSplit_input splitIn;
        processSplit_output splitOut;
        processSplit_locals splitLocals;
        // Chain forwarding locals
        routeToGate_input chainIn;
        routeToGate_output chainOut;
        routeToGate_locals chainLocals;
        GateConfig nextChainGate;
    };

    struct BEGIN_EPOCH_locals
    {
        uint64 i;
        GateConfig gate;
        QuGateLogger logger;
        sint32 subId;
    };

    struct configureInheritance_locals
    {
        QuGateLogger logger;
        GateConfig gate;
        uint64 slotIdx;
        uint64 encodedGen;
        uint8  i;
        uint16 shareSum;
        InheritanceConfig cfg;
        InheritanceBeneficiary bene;
    };

    struct keepAlive_locals
    {
        QuGateLogger logger;
        GateConfig gate;
        uint64 slotIdx;
        uint64 encodedGen;
        InheritanceConfig cfg;
    };

    struct getInheritance_locals
    {
        GateConfig gate;
        uint64 slotIdx;
        uint64 encodedGen;
        InheritanceConfig cfg;
        InheritanceBeneficiary bene;
        uint8 i;
    };



    // =============================================
    // Procedures
    // =============================================

    PUBLIC_PROCEDURE_WITH_LOCALS(createGate)
    {
        output.status = QUGATE_SUCCESS;
        output.gateId = 0;
        output.feePaid = 0;

        // Init logger
        locals.logger._contractIndex = CONTRACT_INDEX;
        locals.logger.sender = qpi.invocator();
        locals.logger.gateId = 0;
        locals.logger.amount = qpi.invocationReward();

        // Calculate escalated fee: baseFee * (1 + activeGates / STEP)
        locals.currentFee = state.get()._creationFee * (1 + QPI::div(state.get()._activeGates, QUGATE_FEE_ESCALATION_STEP));

        // Validate creation fee (escalated)
        if (qpi.invocationReward() < (sint64)locals.currentFee)
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            output.status = QUGATE_INSUFFICIENT_FEE;
            locals.logger._type = QUGATE_LOG_FAIL_INSUFFICIENT_FEE;
            LOG_INFO(locals.logger);
            return;
        }

        // Validate mode
        if (input.mode > QUGATE_MODE_INHERITANCE)
        {
            // Refund all
            qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.status = QUGATE_INVALID_MODE;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate recipient count
        if (input.recipientCount == 0 || input.recipientCount > QUGATE_MAX_RECIPIENTS)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.status = QUGATE_INVALID_RECIPIENT_COUNT;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Check capacity — try free-list first
        if (state.get()._freeCount == 0 && state.get()._gateCount >= QUGATE_MAX_GATES)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.status = QUGATE_NO_FREE_SLOTS;
            locals.logger._type = QUGATE_LOG_FAIL_NO_SLOTS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate SPLIT ratios
        if (input.mode == QUGATE_MODE_SPLIT)
        {
            locals.totalRatio = 0;
            for (locals.i = 0; locals.i < input.recipientCount; locals.i++)
            {
                if (input.ratios.get(locals.i) > QUGATE_MAX_RATIO)
                {
                    qpi.transfer(qpi.invocator(), qpi.invocationReward());
                    output.status = QUGATE_INVALID_RATIO;
                    locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                    LOG_WARNING(locals.logger);
                    return;
                }
                locals.totalRatio += input.ratios.get(locals.i);
            }
            if (locals.totalRatio == 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
                output.status = QUGATE_INVALID_RATIO;
                locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                LOG_WARNING(locals.logger);
                return;
            }
        }

        // Validate THRESHOLD > 0
        if (input.mode == QUGATE_MODE_THRESHOLD && input.threshold == 0)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.status = QUGATE_INVALID_THRESHOLD;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate allowedSenderCount
        if (input.allowedSenderCount > QUGATE_MAX_RECIPIENTS)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.status = QUGATE_INVALID_SENDER_COUNT;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate ORACLE mode
        // NOTE: TIME_AFTER (condition 2) requires an oracle whose reply.numerator encodes unix time
        if (input.mode == QUGATE_MODE_ORACLE)
        {
            if (input.oracleCondition > QUGATE_ORACLE_COND_TIME_AFTER
                || input.oracleThreshold <= 0
                || input.oracleTriggerMode > QUGATE_ORACLE_TRIGGER_RECURRING)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
                output.status = QUGATE_INVALID_ORACLE_CONFIG;
                locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                LOG_WARNING(locals.logger);
                return;
            }
        }

        // Build the gate config
        locals.newGate.owner = qpi.invocator();
        locals.newGate.mode = input.mode;
        locals.newGate.recipientCount = input.recipientCount;
        locals.newGate.active = 1;
        locals.newGate.allowedSenderCount = input.allowedSenderCount;
        locals.newGate.createdEpoch = qpi.epoch();       // uint16
        locals.newGate.lastActivityEpoch = qpi.epoch();  //
        locals.newGate.totalReceived = 0;
        locals.newGate.totalForwarded = 0;
        locals.newGate.currentBalance = 0;
        locals.newGate.threshold = input.threshold;
        locals.newGate.roundRobinIndex = 0;

        // Oracle fields
        if (input.mode == QUGATE_MODE_ORACLE)
        {
            locals.newGate.oracleId = input.oracleId;
            locals.newGate.oracleCurrency1 = input.oracleCurrency1;
            locals.newGate.oracleCurrency2 = input.oracleCurrency2;
            locals.newGate.oracleCondition = input.oracleCondition;
            locals.newGate.oracleTriggerMode = input.oracleTriggerMode;
            locals.newGate.oracleThreshold = input.oracleThreshold;
            locals.newGate.oracleReserve = 0; // set below from excess fee
            locals.newGate.oracleSubscriptionId = -1;
        }
        else
        {
            locals.newGate.oracleId = id::zero();
            locals.newGate.oracleCurrency1 = id::zero();
            locals.newGate.oracleCurrency2 = id::zero();
            locals.newGate.oracleCondition = 0;
            locals.newGate.oracleTriggerMode = 0;
            locals.newGate.oracleThreshold = 0;
            locals.newGate.oracleReserve = 0;
            locals.newGate.oracleSubscriptionId = -1;
        }

        // Chain fields
        locals.newGate.chainNextGateId = -1;
        locals.newGate.chainReserve = 0;
        locals.newGate.chainDepth = 0;

        for (locals.i = 0; locals.i < QUGATE_MAX_RECIPIENTS; locals.i++)
        {
            if (locals.i < input.recipientCount)
            {
                locals.newGate.recipients.set(locals.i, input.recipients.get(locals.i));
                locals.newGate.ratios.set(locals.i, input.ratios.get(locals.i));
            }
            else
            {
                locals.newGate.recipients.set(locals.i, id::zero());
                locals.newGate.ratios.set(locals.i, 0);
            }
        }

        for (locals.i = 0; locals.i < QUGATE_MAX_RECIPIENTS; locals.i++)
        {
            if (locals.i < input.allowedSenderCount)
            {
                locals.newGate.allowedSenders.set(locals.i, input.allowedSenders.get(locals.i));
            }
            else
            {
                locals.newGate.allowedSenders.set(locals.i, id::zero());
            }
        }

        // Allocate slot — free-list first, then fresh
        if (state.get()._freeCount > 0)
        {
            state.mut()._freeCount -= 1;
            locals.slotIdx = state.get()._freeSlots.get(state.get()._freeCount);
        }
        else
        {
            locals.slotIdx = state.get()._gateCount;
            state.mut()._gateCount += 1;
        }

        // Validate chain if specified
        if (input.chainNextGateId != -1)
        {
            // Decode versioned gateId: lower 20 bits = slotIndex, upper bits = generation
            // Decode target slot from versioned gate ID
            uint64 chainTargetSlot = (uint64)(input.chainNextGateId) & QUGATE_GATE_ID_SLOT_MASK;
            uint64 chainTargetGen = (uint64)(input.chainNextGateId) >> QUGATE_GATE_ID_SLOT_BITS;
            if (input.chainNextGateId <= 0
                || chainTargetSlot >= state.get()._gateCount
                || chainTargetGen == 0
                || state.get()._gateGenerations.get(chainTargetSlot) != (uint16)(chainTargetGen - 1))
            {
                // Rollback: fresh slot decrements _gateCount; recycled slot returns to free-list.
                // Undo slot allocation — return slot to free-list or decrement gateCount
                if (locals.slotIdx < state.get()._gateCount - 1)
                {
                    // Slot was from free-list — push back
                    state.mut()._freeSlots.set(state.get()._freeCount, locals.slotIdx);
                    state.mut()._freeCount += 1;
                }
                else
                {
                    state.mut()._gateCount -= 1;
                }
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
                output.status = QUGATE_INVALID_CHAIN;
                locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                LOG_WARNING(locals.logger);
                return;
            }

            GateConfig chainTarget = state.get()._gates.get(chainTargetSlot);
            if (chainTarget.active == 0)
            {
                // Undo slot allocation
                if (locals.slotIdx < state.get()._gateCount - 1)
                {
                    state.mut()._freeSlots.set(state.get()._freeCount, locals.slotIdx);
                    state.mut()._freeCount += 1;
                }
                else
                {
                    state.mut()._gateCount -= 1;
                }
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
                output.status = QUGATE_INVALID_CHAIN;
                locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                LOG_WARNING(locals.logger);
                return;
            }

            // Compute depth
            uint8 newDepth = chainTarget.chainDepth + 1;
            if (newDepth >= QUGATE_MAX_CHAIN_DEPTH)
            {
                if (locals.slotIdx < state.get()._gateCount - 1)
                {
                    state.mut()._freeSlots.set(state.get()._freeCount, locals.slotIdx);
                    state.mut()._freeCount += 1;
                }
                else
                {
                    state.mut()._gateCount -= 1;
                }
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
                output.status = QUGATE_INVALID_CHAIN;
                locals.logger._type = QUGATE_LOG_CHAIN_CYCLE;
                LOG_WARNING(locals.logger);
                return;
            }

            // Cycle detection: walk forward from target up to QUGATE_MAX_CHAIN_DEPTH steps
            {
                uint64 walkSlot = chainTargetSlot;
                uint8 walkStep = 0;
                while (walkStep < QUGATE_MAX_CHAIN_DEPTH)
                {
                    if (walkSlot == locals.slotIdx)
                    {
                        if (locals.slotIdx < state.get()._gateCount - 1)
                        {
                            state.mut()._freeSlots.set(state.get()._freeCount, locals.slotIdx);
                            state.mut()._freeCount += 1;
                        }
                        else
                        {
                            state.mut()._gateCount -= 1;
                        }
                        qpi.transfer(qpi.invocator(), qpi.invocationReward());
                        output.status = QUGATE_INVALID_CHAIN;
                        locals.logger._type = QUGATE_LOG_CHAIN_CYCLE;
                        LOG_WARNING(locals.logger);
                        return;
                    }
                    GateConfig walkGate = state.get()._gates.get(walkSlot);
                    if (walkGate.chainNextGateId == -1)
                    {
                        break;
                    }
                    uint64 nextWalkSlot = (uint64)(walkGate.chainNextGateId) & QUGATE_GATE_ID_SLOT_MASK;
                    if (nextWalkSlot >= state.get()._gateCount)
                    {
                        break;
                    }
                    walkSlot = nextWalkSlot;
                    walkStep++;
                }
                if (walkSlot == locals.slotIdx)
                {
                    if (locals.slotIdx < state.get()._gateCount - 1)
                    {
                        state.mut()._freeSlots.set(state.get()._freeCount, locals.slotIdx);
                        state.mut()._freeCount += 1;
                    }
                    else
                    {
                        state.mut()._gateCount -= 1;
                    }
                    qpi.transfer(qpi.invocator(), qpi.invocationReward());
                    output.status = QUGATE_INVALID_CHAIN;
                    locals.logger._type = QUGATE_LOG_CHAIN_CYCLE;
                    LOG_WARNING(locals.logger);
                    return;
                }
            }

            locals.newGate.chainNextGateId = input.chainNextGateId;
            locals.newGate.chainDepth = newDepth;
        }

        state.mut()._gates.set(locals.slotIdx, locals.newGate);
        output.gateId = ((uint64)(state.get()._gateGenerations.get(locals.slotIdx) + 1) << QUGATE_GATE_ID_SLOT_BITS) | locals.slotIdx;
        state.mut()._activeGates += 1;

        // Burn the escalated creation fee
        qpi.burn(locals.currentFee);
        state.mut()._totalBurned += locals.currentFee;
        output.feePaid = locals.currentFee;

        // Handle excess: for ORACLE mode, excess goes to oracleReserve; otherwise refund
        if (qpi.invocationReward() > (sint64)locals.currentFee)
        {
            if (input.mode == QUGATE_MODE_ORACLE)
            {
                locals.newGate.oracleReserve = qpi.invocationReward() - (sint64)locals.currentFee;
                state.mut()._gates.set(locals.slotIdx, locals.newGate);
            }
            else
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward() - (sint64)locals.currentFee);
            }
        }

        output.status = QUGATE_SUCCESS;

        // Log success
        locals.logger._type = QUGATE_LOG_GATE_CREATED;
        locals.logger.gateId = output.gateId;
        LOG_INFO(locals.logger);
    }

    // =============================================
    // Private mode processors
    // =============================================

    PRIVATE_PROCEDURE_WITH_LOCALS(processSplit)
    {
        locals.gate = state.get()._gates.get(input.gateIdx);

        locals.totalRatio = 0;
        for (locals.i = 0; locals.i < locals.gate.recipientCount; locals.i++)
        {
            locals.totalRatio += locals.gate.ratios.get(locals.i);
        }

        locals.distributed = 0;
        for (locals.i = 0; locals.i < locals.gate.recipientCount; locals.i++)
        {
            if (locals.i == locals.gate.recipientCount - 1)
            {
                locals.share = input.amount - locals.distributed;
            }
            else
            {
                locals.share = QPI::div((uint64)input.amount, locals.totalRatio) * locals.gate.ratios.get(locals.i)
                    + QPI::div(QPI::mod((uint64)input.amount, locals.totalRatio) * locals.gate.ratios.get(locals.i), locals.totalRatio);
            }

            if (locals.share > 0)
            {
                qpi.transfer(locals.gate.recipients.get(locals.i), locals.share);
                locals.distributed += locals.share;
            }
        }

        locals.gate.totalForwarded += locals.distributed;
        state.mut()._gates.set(input.gateIdx, locals.gate);
        output.forwarded = locals.distributed;
    }

    PRIVATE_PROCEDURE_WITH_LOCALS(processRoundRobin)
    {
        locals.gate = state.get()._gates.get(input.gateIdx);

        qpi.transfer(locals.gate.recipients.get(locals.gate.roundRobinIndex), input.amount);
        locals.gate.totalForwarded += input.amount;
        locals.gate.roundRobinIndex = QPI::mod(locals.gate.roundRobinIndex + 1, (uint64)locals.gate.recipientCount);

        state.mut()._gates.set(input.gateIdx, locals.gate);
        output.forwarded = input.amount;
    }

    PRIVATE_PROCEDURE_WITH_LOCALS(processThreshold)
    {
        locals.gate = state.get()._gates.get(input.gateIdx);

        locals.gate.currentBalance += input.amount;
        output.forwarded = 0;

        if (locals.gate.currentBalance >= locals.gate.threshold)
        {
            output.forwarded = locals.gate.currentBalance;
            locals.gate.totalForwarded += locals.gate.currentBalance;

            // If chained, don't transfer to recipient — chain code will handle forwarding
            if (locals.gate.chainNextGateId == -1)
            {
                qpi.transfer(locals.gate.recipients.get(0), locals.gate.currentBalance);
            }

            locals.gate.currentBalance = 0;
        }

        state.mut()._gates.set(input.gateIdx, locals.gate);
    }

    PRIVATE_PROCEDURE_WITH_LOCALS(processRandom)
    {
        locals.gate = state.get()._gates.get(input.gateIdx);

        // NOTE: entropy is publicly observable — not cryptographically random
        locals.recipientIdx = QPI::mod(locals.gate.totalReceived + qpi.tick(), (uint64)locals.gate.recipientCount);
        qpi.transfer(locals.gate.recipients.get(locals.recipientIdx), input.amount);
        locals.gate.totalForwarded += input.amount;

        state.mut()._gates.set(input.gateIdx, locals.gate);
        output.forwarded = input.amount;
    }

    PRIVATE_PROCEDURE_WITH_LOCALS(processConditional)
    {
        locals.gate = state.get()._gates.get(input.gateIdx);
        output.status = QUGATE_SUCCESS;
        output.forwarded = 0;

        locals.senderAllowed = 0;
        for (locals.i = 0; locals.i < locals.gate.allowedSenderCount; locals.i++)
        {
            if (locals.senderAllowed == 0 && locals.gate.allowedSenders.get(locals.i) == qpi.invocator())
            {
                locals.senderAllowed = 1;
            }
        }

        if (locals.senderAllowed)
        {
            qpi.transfer(locals.gate.recipients.get(0), input.amount);
            locals.gate.totalForwarded += input.amount;
            output.forwarded = input.amount;
        }
        else
        {
            qpi.transfer(qpi.invocator(), input.amount);
            output.status = QUGATE_CONDITIONAL_REJECTED;
        }

        state.mut()._gates.set(input.gateIdx, locals.gate);
    }

    // =============================================
    // routeToGate — single-hop chain routing (non-recursive)
    // =============================================

    // routeToGate — executes a single routing hop for chain forwarding.
    // Routes `input.amount` (minus hop fee) through the gate at `input.slotIdx` according to
    // that gate's mode. Does NOT recurse or continue the chain — the caller (OraclePriceNotification)
    // is responsible for iterating subsequent hops.
    //
    // Hop fee priority:
    //   1. Deducted from forwarded amount if amount > QUGATE_CHAIN_HOP_FEE
    //   2. Deducted from gate.chainReserve if amount <= QUGATE_CHAIN_HOP_FEE and reserve is sufficient
    //   3. Funds stranded in gate.currentBalance if both are insufficient (QUGATE_LOG_CHAIN_HOP_INSUFFICIENT)
    //
    // input.hopCount is checked against QUGATE_MAX_CHAIN_DEPTH as a runtime safety guard.
    // The caller should never exceed this, but the guard prevents runaway execution if it does.
    PRIVATE_PROCEDURE_WITH_LOCALS(routeToGate)
    {
        output.forwarded = 0;

        if (input.hopCount >= QUGATE_MAX_CHAIN_DEPTH)
        {
            locals.logger._contractIndex = CONTRACT_INDEX;
            locals.logger._type = QUGATE_LOG_CHAIN_CYCLE;
            locals.logger.gateId = input.slotIdx + 1;
            locals.logger.amount = input.amount;
            LOG_INFO(locals.logger);
            return;
        }

        locals.gate = state.get()._gates.get(input.slotIdx);
        if (locals.gate.active == 0)
        {
            return;
        }

        // Hop fee resolution
        locals.amountAfterFee = input.amount;
        if (input.amount <= QUGATE_CHAIN_HOP_FEE)
        {
            if (locals.gate.chainReserve >= QUGATE_CHAIN_HOP_FEE)
            {
                locals.gate.chainReserve -= QUGATE_CHAIN_HOP_FEE;
                state.mut()._gates.set(input.slotIdx, locals.gate);
                qpi.burn(QUGATE_CHAIN_HOP_FEE);
                // amountAfterFee stays as input.amount (reserve paid the fee)
            }
            else
            {
                // Strand — accumulate in currentBalance
                locals.gate.currentBalance += input.amount;
                state.mut()._gates.set(input.slotIdx, locals.gate);
                locals.logger._contractIndex = CONTRACT_INDEX;
                locals.logger._type = QUGATE_LOG_CHAIN_HOP_INSUFFICIENT;
                locals.logger.gateId = input.slotIdx + 1;
                locals.logger.amount = input.amount;
                LOG_INFO(locals.logger);
                return;
            }
        }
        else
        {
            qpi.burn(QUGATE_CHAIN_HOP_FEE);
            locals.amountAfterFee = input.amount - QUGATE_CHAIN_HOP_FEE;
        }

        // Update totalReceived and activity for the chained gate
        locals.gate.totalReceived += locals.amountAfterFee;
        locals.gate.lastActivityEpoch = qpi.epoch();
        state.mut()._gates.set(input.slotIdx, locals.gate);

        // Route through this gate's mode
        if (locals.gate.mode == QUGATE_MODE_SPLIT)
        {
            locals.splitIn.gateIdx = input.slotIdx;
            locals.splitIn.amount = locals.amountAfterFee;
            processSplit(qpi, state, locals.splitIn, locals.splitOut, locals.splitLocals);
            output.forwarded = locals.splitOut.forwarded;
        }
        else if (locals.gate.mode == QUGATE_MODE_ROUND_ROBIN)
        {
            locals.rrIn.gateIdx = input.slotIdx;
            locals.rrIn.amount = locals.amountAfterFee;
            processRoundRobin(qpi, state, locals.rrIn, locals.rrOut, locals.rrLocals);
            output.forwarded = locals.rrOut.forwarded;
        }
        else if (locals.gate.mode == QUGATE_MODE_THRESHOLD)
        {
            locals.threshIn.gateIdx = input.slotIdx;
            locals.threshIn.amount = locals.amountAfterFee;
            processThreshold(qpi, state, locals.threshIn, locals.threshOut, locals.threshLocals);
            output.forwarded = locals.threshOut.forwarded;
        }
        else if (locals.gate.mode == QUGATE_MODE_RANDOM)
        {
            locals.randIn.gateIdx = input.slotIdx;
            locals.randIn.amount = locals.amountAfterFee;
            processRandom(qpi, state, locals.randIn, locals.randOut, locals.randLocals);
            output.forwarded = locals.randOut.forwarded;
        }
        else if (locals.gate.mode == QUGATE_MODE_ORACLE)
        {
            // ORACLE mode in chain: accumulate into currentBalance
            locals.gate = state.get()._gates.get(input.slotIdx);
            locals.gate.currentBalance += locals.amountAfterFee;
            state.mut()._gates.set(input.slotIdx, locals.gate);
            output.forwarded = locals.amountAfterFee;
        }
        // NOTE: CONDITIONAL mode as chain target requires SELF in allowedSenders — invocator here is the contract, not an external sender.

        // Log hop
        locals.logger._contractIndex = CONTRACT_INDEX;
        locals.logger._type = QUGATE_LOG_CHAIN_HOP;
        locals.logger.gateId = input.slotIdx + 1;
        locals.logger.amount = locals.amountAfterFee;
        LOG_INFO(locals.logger);
    }

    // =============================================
    // Procedures
    // =============================================

    PUBLIC_PROCEDURE_WITH_LOCALS(sendToGate)
    {
        output.status = QUGATE_SUCCESS;

        locals.logger._contractIndex = CONTRACT_INDEX;
        locals.logger.sender = qpi.invocator();
        locals.logger.amount = qpi.invocationReward();
        locals.logger.gateId = input.gateId;

        // Decode versioned gateId: lower 20 bits = slotIndex, upper bits = generation
        locals.slotIdx = input.gateId & QUGATE_GATE_ID_SLOT_MASK;
        locals.encodedGen = (input.gateId >> QUGATE_GATE_ID_SLOT_BITS);
        if (input.gateId == 0
            || locals.slotIdx >= state.get()._gateCount
            || locals.encodedGen == 0
            || state.get()._gateGenerations.get(locals.slotIdx) != (uint16)(locals.encodedGen - 1))
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            output.status = QUGATE_INVALID_GATE_ID;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_GATE;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.gate = state.get()._gates.get(locals.slotIdx);

        if (locals.gate.active == 0)
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.amount = qpi.invocationReward();
        if (locals.amount <= 0)
        {
            output.status = QUGATE_DUST_AMOUNT;
            return;
        }

        if (locals.amount < (sint64)state.get()._minSendAmount)
        {
            qpi.burn(locals.amount);
            state.mut()._totalBurned += locals.amount;
            output.status = QUGATE_DUST_AMOUNT;
            locals.logger._type = QUGATE_LOG_DUST_BURNED;
            LOG_INFO(locals.logger);
            return;
        }

        // Update activity and track received
        locals.gate.lastActivityEpoch = qpi.epoch();
        locals.gate.totalReceived += locals.amount;
        state.mut()._gates.set(locals.slotIdx, locals.gate);

        // Dispatch to mode-specific handler
        if (locals.gate.mode == QUGATE_MODE_SPLIT)
        {
            locals.splitIn.gateIdx = locals.slotIdx;
            locals.splitIn.amount = locals.amount;
            processSplit(qpi, state, locals.splitIn, locals.splitOut, locals.splitLocals);
            locals.logger._type = QUGATE_LOG_PAYMENT_FORWARDED;
            LOG_INFO(locals.logger);
        }
        else if (locals.gate.mode == QUGATE_MODE_ROUND_ROBIN)
        {
            locals.rrIn.gateIdx = locals.slotIdx;
            locals.rrIn.amount = locals.amount;
            processRoundRobin(qpi, state, locals.rrIn, locals.rrOut, locals.rrLocals);
            locals.logger._type = QUGATE_LOG_PAYMENT_FORWARDED;
            LOG_INFO(locals.logger);
        }
        else if (locals.gate.mode == QUGATE_MODE_THRESHOLD)
        {
            locals.threshIn.gateIdx = locals.slotIdx;
            locals.threshIn.amount = locals.amount;
            processThreshold(qpi, state, locals.threshIn, locals.threshOut, locals.threshLocals);
            if (locals.threshOut.forwarded > 0)
            {
                locals.logger._type = QUGATE_LOG_PAYMENT_FORWARDED;
                LOG_INFO(locals.logger);
            }
        }
        else if (locals.gate.mode == QUGATE_MODE_RANDOM)
        {
            locals.randIn.gateIdx = locals.slotIdx;
            locals.randIn.amount = locals.amount;
            processRandom(qpi, state, locals.randIn, locals.randOut, locals.randLocals);
            locals.logger._type = QUGATE_LOG_PAYMENT_FORWARDED;
            LOG_INFO(locals.logger);
        }
        else if (locals.gate.mode == QUGATE_MODE_CONDITIONAL)
        {
            locals.condIn.gateIdx = locals.slotIdx;
            locals.condIn.amount = locals.amount;
            processConditional(qpi, state, locals.condIn, locals.condOut, locals.condLocals);
            if (locals.condOut.status == QUGATE_SUCCESS)
            {
                locals.logger._type = QUGATE_LOG_PAYMENT_FORWARDED;
                LOG_INFO(locals.logger);
            }
            else
            {
                output.status = locals.condOut.status;
                locals.logger._type = QUGATE_LOG_PAYMENT_BOUNCED;
                LOG_INFO(locals.logger);
            }
        }
        else if (locals.gate.mode == QUGATE_MODE_ORACLE)
        {
            // ORACLE mode: accumulate into currentBalance, oracle callback distributes
            locals.gate = state.get()._gates.get(locals.slotIdx);
            locals.gate.currentBalance += locals.amount;
            state.mut()._gates.set(locals.slotIdx, locals.gate);
            locals.logger._type = QUGATE_LOG_PAYMENT_FORWARDED;
            LOG_INFO(locals.logger);
        }
        else if (locals.gate.mode == QUGATE_MODE_INHERITANCE)
        {
            // INHERITANCE mode: accumulate into currentBalance, END_EPOCH distributes after trigger
            locals.gate = state.get()._gates.get(locals.slotIdx);
            locals.gate.currentBalance += locals.amount;
            state.mut()._gates.set(locals.slotIdx, locals.gate);
            locals.logger._type = QUGATE_LOG_PAYMENT_FORWARDED;
            LOG_INFO(locals.logger);
        }

        // Chain forwarding: if this gate has a chain link, forward to the next gate
        locals.gate = state.get()._gates.get(locals.slotIdx);
        if (locals.gate.chainNextGateId != -1 && locals.gate.mode != QUGATE_MODE_ORACLE)
        {
            // Determine forwarded amount from mode handler outputs
            sint64 chainAmount = 0;
            if (locals.gate.mode == QUGATE_MODE_SPLIT)
            {
                chainAmount = locals.splitOut.forwarded;
            }
            else if (locals.gate.mode == QUGATE_MODE_ROUND_ROBIN)
            {
                chainAmount = locals.rrOut.forwarded;
            }
            else if (locals.gate.mode == QUGATE_MODE_THRESHOLD)
            {
                chainAmount = locals.threshOut.forwarded;
            }
            else if (locals.gate.mode == QUGATE_MODE_RANDOM)
            {
                chainAmount = locals.randOut.forwarded;
            }
            else if (locals.gate.mode == QUGATE_MODE_CONDITIONAL && locals.condOut.status == QUGATE_SUCCESS)
            {
                chainAmount = locals.condOut.forwarded;
            }

            if (chainAmount > 0)
            {
                sint64 currentChainGateId = locals.gate.chainNextGateId;
                uint8 hop = 0;
                while (hop < QUGATE_MAX_CHAIN_DEPTH && currentChainGateId != -1 && chainAmount > 0)
                {
                    uint64 nextSlot = (uint64)currentChainGateId & QUGATE_GATE_ID_SLOT_MASK;
                    uint64 nextGen = (uint64)currentChainGateId >> QUGATE_GATE_ID_SLOT_BITS;
                    if (nextSlot >= state.get()._gateCount || nextGen == 0
                        || state.get()._gateGenerations.get(nextSlot) != (uint16)(nextGen - 1))
                    {
                        break; // dead link
                    }
                    locals.chainIn.slotIdx = nextSlot;
                    locals.chainIn.amount = chainAmount;
                    locals.chainIn.hopCount = hop;
                    routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                    chainAmount = locals.chainOut.forwarded;
                    locals.nextChainGate = state.get()._gates.get(nextSlot);
                    currentChainGateId = locals.nextChainGate.chainNextGateId;
                    hop += 1;
                }
            }
        }
    }

    PUBLIC_PROCEDURE_WITH_LOCALS(sendToGateVerified)
    {
        output.status = QUGATE_SUCCESS;

        locals.logger._contractIndex = CONTRACT_INDEX;
        locals.logger.sender = qpi.invocator();
        locals.logger.amount = qpi.invocationReward();
        locals.logger.gateId = input.gateId;

        // Decode versioned gateId: lower 20 bits = slotIndex, upper bits = generation
        locals.slotIdx = input.gateId & QUGATE_GATE_ID_SLOT_MASK;
        locals.encodedGen = (input.gateId >> QUGATE_GATE_ID_SLOT_BITS);
        if (input.gateId == 0
            || locals.slotIdx >= state.get()._gateCount
            || locals.encodedGen == 0
            || state.get()._gateGenerations.get(locals.slotIdx) != (uint16)(locals.encodedGen - 1))
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            output.status = QUGATE_INVALID_GATE_ID;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_GATE;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.gate = state.get()._gates.get(locals.slotIdx);

        if (locals.gate.active == 0)
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Owner verification — refund and reject if mismatch
        if (locals.gate.owner != input.expectedOwner)
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            output.status = QUGATE_OWNER_MISMATCH;
            locals.logger._type = QUGATE_LOG_FAIL_OWNER_MISMATCH;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.amount = qpi.invocationReward();
        if (locals.amount <= 0)
        {
            output.status = QUGATE_DUST_AMOUNT;
            return;
        }

        if (locals.amount < (sint64)state.get()._minSendAmount)
        {
            qpi.burn(locals.amount);
            state.mut()._totalBurned += locals.amount;
            output.status = QUGATE_DUST_AMOUNT;
            locals.logger._type = QUGATE_LOG_DUST_BURNED;
            LOG_INFO(locals.logger);
            return;
        }

        // Update activity and track received
        locals.gate.lastActivityEpoch = qpi.epoch();
        locals.gate.totalReceived += locals.amount;
        state.mut()._gates.set(locals.slotIdx, locals.gate);

        // Dispatch to mode-specific handler
        if (locals.gate.mode == QUGATE_MODE_SPLIT)
        {
            locals.splitIn.gateIdx = locals.slotIdx;
            locals.splitIn.amount = locals.amount;
            processSplit(qpi, state, locals.splitIn, locals.splitOut, locals.splitLocals);
            locals.logger._type = QUGATE_LOG_PAYMENT_FORWARDED;
            LOG_INFO(locals.logger);
        }
        else if (locals.gate.mode == QUGATE_MODE_ROUND_ROBIN)
        {
            locals.rrIn.gateIdx = locals.slotIdx;
            locals.rrIn.amount = locals.amount;
            processRoundRobin(qpi, state, locals.rrIn, locals.rrOut, locals.rrLocals);
            locals.logger._type = QUGATE_LOG_PAYMENT_FORWARDED;
            LOG_INFO(locals.logger);
        }
        else if (locals.gate.mode == QUGATE_MODE_THRESHOLD)
        {
            locals.threshIn.gateIdx = locals.slotIdx;
            locals.threshIn.amount = locals.amount;
            processThreshold(qpi, state, locals.threshIn, locals.threshOut, locals.threshLocals);
            if (locals.threshOut.forwarded > 0)
            {
                locals.logger._type = QUGATE_LOG_PAYMENT_FORWARDED;
                LOG_INFO(locals.logger);
            }
        }
        else if (locals.gate.mode == QUGATE_MODE_RANDOM)
        {
            locals.randIn.gateIdx = locals.slotIdx;
            locals.randIn.amount = locals.amount;
            processRandom(qpi, state, locals.randIn, locals.randOut, locals.randLocals);
            locals.logger._type = QUGATE_LOG_PAYMENT_FORWARDED;
            LOG_INFO(locals.logger);
        }
        else if (locals.gate.mode == QUGATE_MODE_CONDITIONAL)
        {
            locals.condIn.gateIdx = locals.slotIdx;
            locals.condIn.amount = locals.amount;
            processConditional(qpi, state, locals.condIn, locals.condOut, locals.condLocals);
            if (locals.condOut.status == QUGATE_SUCCESS)
            {
                locals.logger._type = QUGATE_LOG_PAYMENT_FORWARDED;
                LOG_INFO(locals.logger);
            }
            else
            {
                output.status = locals.condOut.status;
                locals.logger._type = QUGATE_LOG_PAYMENT_BOUNCED;
                LOG_INFO(locals.logger);
            }
        }
        else if (locals.gate.mode == QUGATE_MODE_ORACLE)
        {
            // ORACLE mode: accumulate into currentBalance, oracle callback distributes
            locals.gate = state.get()._gates.get(locals.slotIdx);
            locals.gate.currentBalance += locals.amount;
            state.mut()._gates.set(locals.slotIdx, locals.gate);
            locals.logger._type = QUGATE_LOG_PAYMENT_FORWARDED;
            LOG_INFO(locals.logger);
        }
        else if (locals.gate.mode == QUGATE_MODE_INHERITANCE)
        {
            // INHERITANCE mode: accumulate into currentBalance, END_EPOCH distributes after trigger
            locals.gate = state.get()._gates.get(locals.slotIdx);
            locals.gate.currentBalance += locals.amount;
            state.mut()._gates.set(locals.slotIdx, locals.gate);
            locals.logger._type = QUGATE_LOG_PAYMENT_FORWARDED;
            LOG_INFO(locals.logger);
        }
    }

    PUBLIC_PROCEDURE_WITH_LOCALS(closeGate)
    {
        output.status = QUGATE_SUCCESS;

        // Init logger
        locals.logger._contractIndex = CONTRACT_INDEX;
        locals.logger.sender = qpi.invocator();
        locals.logger.gateId = input.gateId;
        locals.logger.amount = 0;

        // Decode versioned gateId: lower 20 bits = slotIndex, upper bits = generation
        locals.slotIdx = input.gateId & QUGATE_GATE_ID_SLOT_MASK;
        locals.encodedGen = (input.gateId >> QUGATE_GATE_ID_SLOT_BITS);
        if (input.gateId == 0
            || locals.slotIdx >= state.get()._gateCount
            || locals.encodedGen == 0
            || state.get()._gateGenerations.get(locals.slotIdx) != (uint16)(locals.encodedGen - 1))
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            output.status = QUGATE_INVALID_GATE_ID;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_GATE;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.gate = state.get()._gates.get(locals.slotIdx);

        if (locals.gate.owner != qpi.invocator())
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            output.status = QUGATE_UNAUTHORIZED;
            locals.logger._type = QUGATE_LOG_FAIL_UNAUTHORIZED;
            LOG_WARNING(locals.logger);
            return;
        }

        if (locals.gate.active == 0)
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Refund any held balance (THRESHOLD / ORACLE mode)
        if (locals.gate.currentBalance > 0)
        {
            qpi.transfer(locals.gate.owner, locals.gate.currentBalance);
            locals.gate.currentBalance = 0;
        }

        // Refund oracle reserve and unsubscribe
        if (locals.gate.mode == QUGATE_MODE_ORACLE)
        {
            if (locals.gate.oracleReserve > 0)
            {
                qpi.transfer(locals.gate.owner, locals.gate.oracleReserve);
                locals.gate.oracleReserve = 0;
            }
            if (locals.gate.oracleSubscriptionId >= 0)
            {
                state.mut()._subscriptionToSlot.removeByKey(locals.gate.oracleSubscriptionId);
                qpi.unsubscribeOracle(locals.gate.oracleSubscriptionId);
                locals.gate.oracleSubscriptionId = -1;
            }
        }

        // Refund chain reserve
        if (locals.gate.chainReserve > 0)
        {
            qpi.transfer(locals.gate.owner, locals.gate.chainReserve);
            locals.gate.chainReserve = 0;
        }

        // Clear inheritance config on close
        if (locals.gate.mode == QUGATE_MODE_INHERITANCE)
        {
            locals.inhZeroCfg.thresholdEpochs = 0;
            locals.inhZeroCfg.lastKeepAliveEpoch = 0;
            locals.inhZeroCfg.payoutPercentPerEpoch = 0;
            locals.inhZeroCfg.minimumBalance = 0;
            locals.inhZeroCfg.active = 0;
            locals.inhZeroCfg.triggered = 0;
            locals.inhZeroCfg.triggerEpoch = 0;
            state.mut()._inheritanceConfigs.set(locals.slotIdx, locals.inhZeroCfg);
            state.mut()._inheritanceBeneficiaryCount.set(locals.slotIdx, 0);
        }

        // Guard against double-close underflow
        if (locals.gate.active == 1)
        {
            locals.gate.active = 0;
            state.mut()._gates.set(locals.slotIdx, locals.gate);
            state.mut()._activeGates -= 1;

            // Push slot onto free-list for reuse
            state.mut()._freeSlots.set(state.get()._freeCount, locals.slotIdx);
            state.mut()._freeCount += 1;

            // Increment generation so recycled slot gets a new gateId
            state.mut()._gateGenerations.set(locals.slotIdx, state.get()._gateGenerations.get(locals.slotIdx) + 1);
        }

        // Refund invocation reward
        if (qpi.invocationReward() > 0)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward());
        }

        // Log success
        locals.logger._type = QUGATE_LOG_GATE_CLOSED;
        LOG_INFO(locals.logger);
    }

    PUBLIC_PROCEDURE_WITH_LOCALS(updateGate)
    {
        output.status = QUGATE_SUCCESS;

        // Init logger
        locals.logger._contractIndex = CONTRACT_INDEX;
        locals.logger.sender = qpi.invocator();
        locals.logger.gateId = input.gateId;
        locals.logger.amount = 0;

        // Decode versioned gateId: lower 20 bits = slotIndex, upper bits = generation
        locals.slotIdx = input.gateId & QUGATE_GATE_ID_SLOT_MASK;
        locals.encodedGen = (input.gateId >> QUGATE_GATE_ID_SLOT_BITS);
        if (input.gateId == 0
            || locals.slotIdx >= state.get()._gateCount
            || locals.encodedGen == 0
            || state.get()._gateGenerations.get(locals.slotIdx) != (uint16)(locals.encodedGen - 1))
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            output.status = QUGATE_INVALID_GATE_ID;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_GATE;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.gate = state.get()._gates.get(locals.slotIdx);

        if (locals.gate.owner != qpi.invocator())
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            output.status = QUGATE_UNAUTHORIZED;
            locals.logger._type = QUGATE_LOG_FAIL_UNAUTHORIZED;
            LOG_WARNING(locals.logger);
            return;
        }

        if (locals.gate.active == 0)
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Oracle gates with pending balance cannot change recipients — prevents rug-pull of accumulated funds
        if (locals.gate.mode == QUGATE_MODE_ORACLE && locals.gate.currentBalance > 0)
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            output.status = QUGATE_UNAUTHORIZED;
            locals.logger._type = QUGATE_LOG_FAIL_UNAUTHORIZED;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate new recipient count
        if (input.recipientCount == 0 || input.recipientCount > QUGATE_MAX_RECIPIENTS)
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            output.status = QUGATE_INVALID_RECIPIENT_COUNT;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate allowedSenderCount bounds
        if (input.allowedSenderCount > QUGATE_MAX_RECIPIENTS)
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            output.status = QUGATE_INVALID_SENDER_COUNT;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate SPLIT ratios if gate is SPLIT mode
        if (locals.gate.mode == QUGATE_MODE_SPLIT)
        {
            locals.totalRatio = 0;
            for (locals.i = 0; locals.i < input.recipientCount; locals.i++)
            {
                if (input.ratios.get(locals.i) > QUGATE_MAX_RATIO)
                {
                    if (qpi.invocationReward() > 0)
                    {
                        qpi.transfer(qpi.invocator(), qpi.invocationReward());
                    }
                    output.status = QUGATE_INVALID_RATIO;
                    locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                    LOG_WARNING(locals.logger);
                    return;
                }
                locals.totalRatio += input.ratios.get(locals.i);
            }
            if (locals.totalRatio == 0)
            {
                if (qpi.invocationReward() > 0)
                {
                    qpi.transfer(qpi.invocator(), qpi.invocationReward());
                }
                output.status = QUGATE_INVALID_RATIO;
                locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                LOG_WARNING(locals.logger);
                return;
            }
        }

        // Validate THRESHOLD if gate is THRESHOLD mode
        if (locals.gate.mode == QUGATE_MODE_THRESHOLD && input.threshold == 0)
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            output.status = QUGATE_INVALID_THRESHOLD;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Update last activity epoch
        locals.gate.lastActivityEpoch = qpi.epoch();

        // Update configuration (mode stays the same)
        locals.gate.recipientCount = input.recipientCount;
        locals.gate.threshold = input.threshold;
        locals.gate.allowedSenderCount = input.allowedSenderCount;

        // Use locals.i, zero stale slots
        for (locals.i = 0; locals.i < QUGATE_MAX_RECIPIENTS; locals.i++)
        {
            if (locals.i < input.recipientCount)
            {
                locals.gate.recipients.set(locals.i, input.recipients.get(locals.i));
                locals.gate.ratios.set(locals.i, input.ratios.get(locals.i));
            }
            else
            {
                locals.gate.recipients.set(locals.i, id::zero());
                locals.gate.ratios.set(locals.i, 0);
            }

            if (locals.i < input.allowedSenderCount)
            {
                locals.gate.allowedSenders.set(locals.i, input.allowedSenders.get(locals.i));
            }
            else
            {
                locals.gate.allowedSenders.set(locals.i, id::zero());
            }
        }

        state.mut()._gates.set(locals.slotIdx, locals.gate);

        if (qpi.invocationReward() > 0)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward());
        }

        // Log success
        locals.logger._type = QUGATE_LOG_GATE_UPDATED;
        LOG_INFO(locals.logger);
    }

    // =============================================
    // fundGate — add QU to oracle gate reserve
    // =============================================

    PUBLIC_PROCEDURE_WITH_LOCALS(fundGate)
    {
        output.result = QUGATE_SUCCESS;

        locals.logger._contractIndex = CONTRACT_INDEX;
        locals.logger.sender = qpi.invocator();
        locals.logger.gateId = input.gateId;
        locals.logger.amount = qpi.invocationReward();

        // Decode versioned gateId: lower 20 bits = slotIndex, upper bits = generation
        locals.slotIdx = input.gateId & QUGATE_GATE_ID_SLOT_MASK;
        locals.encodedGen = (input.gateId >> QUGATE_GATE_ID_SLOT_BITS);
        if (input.gateId <= 0
            || locals.slotIdx >= state.get()._gateCount
            || locals.encodedGen == 0
            || state.get()._gateGenerations.get(locals.slotIdx) != (uint16)(locals.encodedGen - 1))
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            output.result = QUGATE_INVALID_GATE_ID;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_GATE;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.gate = state.get()._gates.get(locals.slotIdx);

        if (locals.gate.active == 0)
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            output.result = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        if (qpi.invocationReward() <= 0)
        {
            output.result = QUGATE_DUST_AMOUNT;
            return;
        }

        if (input.reserveTarget == 0)
        {
            // Oracle reserve
            if (locals.gate.mode != QUGATE_MODE_ORACLE)
            {
                if (qpi.invocationReward() > 0)
                {
                    qpi.transfer(qpi.invocator(), qpi.invocationReward());
                }
                output.result = QUGATE_INVALID_ORACLE_CONFIG;
                locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                LOG_WARNING(locals.logger);
                return;
            }
            locals.gate.oracleReserve += qpi.invocationReward();
        }
        else if (input.reserveTarget == 1)
        {
            // Chain reserve — gate must have a chain link
            if (locals.gate.chainNextGateId == -1)
            {
                if (qpi.invocationReward() > 0)
                {
                    qpi.transfer(qpi.invocator(), qpi.invocationReward());
                }
                output.result = QUGATE_INVALID_CHAIN;
                locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                LOG_WARNING(locals.logger);
                return;
            }
            locals.gate.chainReserve += qpi.invocationReward();
        }
        else
        {
            // Invalid reserveTarget
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            output.result = QUGATE_INVALID_CHAIN;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        state.mut()._gates.set(locals.slotIdx, locals.gate);
        output.result = QUGATE_SUCCESS;
    }

    // =============================================
    // Oracle notification callback
    // NOTE: Cannot be fully tested without a live node — condition logic tested in isolation
    // =============================================

    PRIVATE_PROCEDURE_WITH_LOCALS(OraclePriceNotification)
    {
        // Only process successful oracle replies
        if (input.status != 3) // ORACLE_QUERY_STATUS_SUCCESS
        {
            return;
        }

        // O(1) lookup via HashMap
        locals.slotIdx = QUGATE_MAX_GATES; // sentinel = not found
        if (state.get()._subscriptionToSlot.contains(input.subscriptionId))
        {
            state.get()._subscriptionToSlot.get(input.subscriptionId, locals.slotIdx);
        }
        if (locals.slotIdx >= state.get()._gateCount)
        {
            return; // not found
        }

        locals.gate = state.get()._gates.get(locals.slotIdx);
        if (locals.gate.active == 0 || locals.gate.mode != QUGATE_MODE_ORACLE)
        {
            return;
        }
        if (locals.gate.oracleSubscriptionId != input.subscriptionId)
        {
            return; // safety check
        }

        // Evaluate condition
        locals.conditionMet = 0;
        if (locals.gate.oracleCondition == QUGATE_ORACLE_COND_PRICE_ABOVE)
        {
            if (input.reply.denominator > 0)
            {
                locals.priceScaled = input.reply.numerator * 1000000 / input.reply.denominator;
                if (locals.priceScaled > locals.gate.oracleThreshold)
                {
                    locals.conditionMet = 1;
                }
            }
        }
        else if (locals.gate.oracleCondition == QUGATE_ORACLE_COND_PRICE_BELOW)
        {
            if (input.reply.denominator > 0)
            {
                locals.priceScaled = input.reply.numerator * 1000000 / input.reply.denominator;
                if (locals.priceScaled < locals.gate.oracleThreshold)
                {
                    locals.conditionMet = 1;
                }
            }
        }
        else if (locals.gate.oracleCondition == QUGATE_ORACLE_COND_TIME_AFTER)
        {
            // TIME_AFTER: treat oracle reply numerator as a timestamp-like value
            if (input.reply.numerator > locals.gate.oracleThreshold)
            {
                locals.conditionMet = 1;
            }
        }

        if (locals.conditionMet && locals.gate.currentBalance > 0)
        {
            // Store amount, zero balance, and write gate back BEFORE calling processSplit
            locals.splitAmount = locals.gate.currentBalance;
            locals.gate.currentBalance = 0;
            state.mut()._gates.set(locals.slotIdx, locals.gate);

            // Distribute using ratio-aware split
            locals.splitIn.gateIdx = locals.slotIdx;
            locals.splitIn.amount = locals.splitAmount;
            processSplit(qpi, state, locals.splitIn, locals.splitOut, locals.splitLocals);

            // Re-read gate after processSplit updated totalForwarded
            locals.gate = state.get()._gates.get(locals.slotIdx);

            // Log oracle triggered
            locals.logger._contractIndex = CONTRACT_INDEX;
            locals.logger._type = QUGATE_LOG_ORACLE_TRIGGERED;
            locals.logger.gateId = ((uint64)(state.get()._gateGenerations.get(locals.slotIdx) + 1) << QUGATE_GATE_ID_SLOT_BITS) | locals.slotIdx;
            locals.logger.sender = id::zero();
            locals.logger.amount = locals.splitOut.forwarded;
            LOG_INFO(locals.logger);

            // Chain forwarding after oracle payout
            if (locals.gate.chainNextGateId != -1)
            {
                sint64 chainAmount = locals.splitOut.forwarded;
                sint64 currentChainGateId = locals.gate.chainNextGateId;
                uint8 hop = 0;
                while (hop < QUGATE_MAX_CHAIN_DEPTH && currentChainGateId != -1 && chainAmount > 0)
                {
                    uint64 nextSlot = (uint64)currentChainGateId & QUGATE_GATE_ID_SLOT_MASK;
                    uint64 nextGen = (uint64)currentChainGateId >> QUGATE_GATE_ID_SLOT_BITS;
                    if (nextSlot >= state.get()._gateCount || nextGen == 0
                        || state.get()._gateGenerations.get(nextSlot) != (uint16)(nextGen - 1))
                    {
                        break; // dead link
                    }
                    locals.chainIn.slotIdx = nextSlot;
                    locals.chainIn.amount = chainAmount;
                    locals.chainIn.hopCount = hop;
                    routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                    chainAmount = locals.chainOut.forwarded;
                    // Read updated gate to get its chainNextGateId
                    locals.nextChainGate = state.get()._gates.get(nextSlot);
                    currentChainGateId = locals.nextChainGate.chainNextGateId;
                    hop++;
                }
            }

            // If ONCE mode, close gate after trigger (guard against double-close underflow)
            if (locals.gate.oracleTriggerMode == QUGATE_ORACLE_TRIGGER_ONCE && locals.gate.active == 1)
            {
                if (locals.gate.oracleReserve > 0)
                {
                    qpi.transfer(locals.gate.owner, locals.gate.oracleReserve);
                    locals.gate.oracleReserve = 0;
                }
                if (locals.gate.oracleSubscriptionId >= 0)
                {
                    state.mut()._subscriptionToSlot.removeByKey(locals.gate.oracleSubscriptionId);
                    qpi.unsubscribeOracle(locals.gate.oracleSubscriptionId);
                    locals.gate.oracleSubscriptionId = -1;
                }
                locals.gate.active = 0;
                state.mut()._activeGates -= 1;
                state.mut()._freeSlots.set(state.get()._freeCount, locals.slotIdx);
                state.mut()._freeCount += 1;

                // Increment generation so recycled slot gets a new gateId
                state.mut()._gateGenerations.set(locals.slotIdx, state.get()._gateGenerations.get(locals.slotIdx) + 1);
            }
        }

        state.mut()._gates.set(locals.slotIdx, locals.gate);
    }

    // =============================================
    // Functions (read-only)
    // =============================================

    PUBLIC_FUNCTION_WITH_LOCALS(getGate)
    {
        // Decode versioned gateId: lower 20 bits = slotIndex, upper bits = generation
        locals.slotIdx = input.gateId & QUGATE_GATE_ID_SLOT_MASK;
        locals.encodedGen = (input.gateId >> QUGATE_GATE_ID_SLOT_BITS);
        if (input.gateId == 0
            || locals.slotIdx >= state.get()._gateCount
            || locals.encodedGen == 0
            || state.get()._gateGenerations.get(locals.slotIdx) != (uint16)(locals.encodedGen - 1))
        {
            output.active = 0;
            return;
        }

        locals.gate = state.get()._gates.get(locals.slotIdx);
        output.mode = locals.gate.mode;
        output.recipientCount = locals.gate.recipientCount;
        output.active = locals.gate.active;
        output.owner = locals.gate.owner;
        output.totalReceived = locals.gate.totalReceived;
        output.totalForwarded = locals.gate.totalForwarded;
        output.currentBalance = locals.gate.currentBalance;
        output.threshold = locals.gate.threshold;
        output.createdEpoch = locals.gate.createdEpoch;
        output.lastActivityEpoch = locals.gate.lastActivityEpoch;  //

        for (locals.i = 0; locals.i < QUGATE_MAX_RECIPIENTS; locals.i++)
        {
            output.recipients.set(locals.i, locals.gate.recipients.get(locals.i));
            output.ratios.set(locals.i, locals.gate.ratios.get(locals.i));
            output.allowedSenders.set(locals.i, locals.gate.allowedSenders.get(locals.i));
        }
        output.allowedSenderCount = locals.gate.allowedSenderCount;
        output.oracleReserve = locals.gate.oracleReserve;
        output.oracleSubscriptionId = locals.gate.oracleSubscriptionId;
        output.chainNextGateId = locals.gate.chainNextGateId;
        output.chainReserve = locals.gate.chainReserve;
        output.chainDepth = locals.gate.chainDepth;
    }

    PUBLIC_FUNCTION(getGateCount)
    {
        output.totalGates = state.get()._gateCount;
        output.activeGates = state.get()._activeGates;
        output.totalBurned = state.get()._totalBurned;         //
    }

    PUBLIC_FUNCTION_WITH_LOCALS(getGatesByOwner)
    {
        output.count = 0;
        for (locals.i = 0; locals.i < state.get()._gateCount && output.count < QUGATE_MAX_OWNER_GATES; locals.i++)
        {
            if (state.get()._gates.get(locals.i).owner == input.owner)
            {
                output.gateIds.set(output.count,
                    ((uint64)(state.get()._gateGenerations.get(locals.i) + 1) << QUGATE_GATE_ID_SLOT_BITS) | locals.i);
                output.count += 1;
            }
        }
    }

    // Batch gate query — fetch up to QUGATE_MAX_BATCH_GATES gates in one call
    PUBLIC_FUNCTION_WITH_LOCALS(getGateBatch)
    {
        for (locals.i = 0; locals.i < QUGATE_MAX_BATCH_GATES; locals.i++)
        {
            // Decode versioned gateId: lower 20 bits = slotIndex, upper bits = generation
            locals.slotIdx = input.gateIds.get(locals.i) & QUGATE_GATE_ID_SLOT_MASK;
            locals.encodedGen = (input.gateIds.get(locals.i) >> QUGATE_GATE_ID_SLOT_BITS);
            if (input.gateIds.get(locals.i) == 0
                || locals.slotIdx >= state.get()._gateCount
                || locals.encodedGen == 0
                || state.get()._gateGenerations.get(locals.slotIdx) != (uint16)(locals.encodedGen - 1))
            {
                // Zero the entry for invalid IDs — clear all fields to avoid stale data
                locals.entry.mode = 0;
                locals.entry.recipientCount = 0;
                locals.entry.active = 0;
                locals.entry.owner = id::zero();
                locals.entry.totalReceived = 0;
                locals.entry.totalForwarded = 0;
                locals.entry.currentBalance = 0;
                locals.entry.threshold = 0;
                locals.entry.createdEpoch = 0;
                locals.entry.lastActivityEpoch = 0;
                locals.entry.oracleReserve = 0;
                locals.entry.oracleSubscriptionId = -1;
                locals.entry.chainNextGateId = -1;
                locals.entry.chainReserve = 0;
                locals.entry.chainDepth = 0;
                for (locals.j = 0; locals.j < QUGATE_MAX_RECIPIENTS; locals.j++)
                {
                    locals.entry.recipients.set(locals.j, id::zero());
                    locals.entry.ratios.set(locals.j, 0);
                }
                output.gates.set(locals.i, locals.entry);
            }
            else
            {
                locals.gate = state.get()._gates.get(locals.slotIdx);

                locals.entry.mode = locals.gate.mode;
                locals.entry.recipientCount = locals.gate.recipientCount;
                locals.entry.active = locals.gate.active;
                locals.entry.owner = locals.gate.owner;
                locals.entry.totalReceived = locals.gate.totalReceived;
                locals.entry.totalForwarded = locals.gate.totalForwarded;
                locals.entry.currentBalance = locals.gate.currentBalance;
                locals.entry.threshold = locals.gate.threshold;
                locals.entry.createdEpoch = locals.gate.createdEpoch;
                locals.entry.lastActivityEpoch = locals.gate.lastActivityEpoch;
                locals.entry.oracleReserve = locals.gate.oracleReserve;
                locals.entry.oracleSubscriptionId = locals.gate.oracleSubscriptionId;
                locals.entry.chainNextGateId = locals.gate.chainNextGateId;
                locals.entry.chainReserve = locals.gate.chainReserve;
                locals.entry.chainDepth = locals.gate.chainDepth;

                for (locals.j = 0; locals.j < QUGATE_MAX_RECIPIENTS; locals.j++)
                {
                    locals.entry.recipients.set(locals.j, locals.gate.recipients.get(locals.j));
                    locals.entry.ratios.set(locals.j, locals.gate.ratios.get(locals.j));
                }

                output.gates.set(locals.i, locals.entry);
            }
        }
    }

    // Fee query
    PUBLIC_FUNCTION(getFees)
    {
        output.creationFee = state.get()._creationFee;
        output.currentCreationFee = state.get()._creationFee * (1 + QPI::div(state.get()._activeGates, QUGATE_FEE_ESCALATION_STEP));
        output.minSendAmount = state.get()._minSendAmount;
        output.expiryEpochs = state.get()._expiryEpochs;
    }

    // =============================================
    // setChain — update chain link on an existing gate
    // =============================================

    PUBLIC_PROCEDURE_WITH_LOCALS(setChain)
    {
        output.result = QUGATE_SUCCESS;

        locals.logger._contractIndex = CONTRACT_INDEX;
        locals.logger.sender = qpi.invocator();
        locals.logger.gateId = input.gateId;
        locals.logger.amount = 0;

        // Decode versioned gateId: lower 20 bits = slotIndex, upper bits = generation
        // Decode source gate
        locals.slotIdx = (uint64)(input.gateId) & QUGATE_GATE_ID_SLOT_MASK;
        locals.encodedGen = (uint64)(input.gateId) >> QUGATE_GATE_ID_SLOT_BITS;
        if (input.gateId <= 0
            || locals.slotIdx >= state.get()._gateCount
            || locals.encodedGen == 0
            || state.get()._gateGenerations.get(locals.slotIdx) != (uint16)(locals.encodedGen - 1))
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            output.result = QUGATE_INVALID_GATE_ID;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_GATE;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.gate = state.get()._gates.get(locals.slotIdx);

        if (locals.gate.owner != qpi.invocator())
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            output.result = QUGATE_UNAUTHORIZED;
            locals.logger._type = QUGATE_LOG_FAIL_UNAUTHORIZED;
            LOG_WARNING(locals.logger);
            return;
        }

        if (locals.gate.active == 0)
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            output.result = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Require hop fee as update cost
        if (qpi.invocationReward() < QUGATE_CHAIN_HOP_FEE)
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            output.result = QUGATE_INSUFFICIENT_FEE;
            locals.logger._type = QUGATE_LOG_FAIL_INSUFFICIENT_FEE;
            LOG_WARNING(locals.logger);
            return;
        }

        if (input.nextGateId == -1)
        {
            // Clear chain
            locals.gate.chainNextGateId = -1;
            locals.gate.chainDepth = 0;
            state.mut()._gates.set(locals.slotIdx, locals.gate);
            qpi.burn(QUGATE_CHAIN_HOP_FEE);
            if (qpi.invocationReward() > QUGATE_CHAIN_HOP_FEE)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward() - QUGATE_CHAIN_HOP_FEE);
            }
            output.result = QUGATE_SUCCESS;
            return;
        }

        // Decode versioned gateId: lower 20 bits = slotIndex, upper bits = generation
        // Decode target gate
        locals.targetSlot = (uint64)(input.nextGateId) & QUGATE_GATE_ID_SLOT_MASK;
        locals.targetEncodedGen = (uint64)(input.nextGateId) >> QUGATE_GATE_ID_SLOT_BITS;
        if (input.nextGateId <= 0
            || locals.targetSlot >= state.get()._gateCount
            || locals.targetEncodedGen == 0
            || state.get()._gateGenerations.get(locals.targetSlot) != (uint16)(locals.targetEncodedGen - 1))
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            output.result = QUGATE_INVALID_CHAIN;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.targetGate = state.get()._gates.get(locals.targetSlot);
        if (locals.targetGate.active == 0)
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            output.result = QUGATE_INVALID_CHAIN;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Depth check
        locals.newDepth = locals.targetGate.chainDepth + 1;
        if (locals.newDepth >= QUGATE_MAX_CHAIN_DEPTH)
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            output.result = QUGATE_INVALID_CHAIN;
            locals.logger._type = QUGATE_LOG_CHAIN_CYCLE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Cycle detection: walk forward from target
        locals.walkSlot = locals.targetSlot;
        locals.walkStep = 0;
        while (locals.walkStep < QUGATE_MAX_CHAIN_DEPTH)
        {
            if (locals.walkSlot == locals.slotIdx)
            {
                if (qpi.invocationReward() > 0)
                {
                    qpi.transfer(qpi.invocator(), qpi.invocationReward());
                }
                output.result = QUGATE_INVALID_CHAIN;
                locals.logger._type = QUGATE_LOG_CHAIN_CYCLE;
                LOG_WARNING(locals.logger);
                return;
            }
            locals.walkGate = state.get()._gates.get(locals.walkSlot);
            if (locals.walkGate.chainNextGateId == -1)
            {
                break;
            }
            uint64 nextWalkSlot = (uint64)(locals.walkGate.chainNextGateId) & QUGATE_GATE_ID_SLOT_MASK;
            if (nextWalkSlot >= state.get()._gateCount)
            {
                break;
            }
            locals.walkSlot = nextWalkSlot;
            locals.walkStep++;
        }
        if (locals.walkSlot == locals.slotIdx)
        {
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            output.result = QUGATE_INVALID_CHAIN;
            locals.logger._type = QUGATE_LOG_CHAIN_CYCLE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Apply
        locals.gate.chainNextGateId = input.nextGateId;
        locals.gate.chainDepth = locals.newDepth;
        state.mut()._gates.set(locals.slotIdx, locals.gate);

        qpi.burn(QUGATE_CHAIN_HOP_FEE);
        if (qpi.invocationReward() > QUGATE_CHAIN_HOP_FEE)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward() - QUGATE_CHAIN_HOP_FEE);
        }

        output.result = QUGATE_SUCCESS;
    }

    // =============================================
    // configureInheritance — set up dead man's switch on an INHERITANCE gate
    // =============================================

    PUBLIC_PROCEDURE_WITH_LOCALS(configureInheritance)
    {
        output.status = QUGATE_SUCCESS;

        locals.logger._contractIndex = CONTRACT_INDEX;
        locals.logger.sender = qpi.invocator();
        locals.logger.gateId = input.gateId;
        locals.logger.amount = 0;

        // Refund any attached QU
        if (qpi.invocationReward() > 0)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward());
        }

        // Decode versioned gateId
        locals.slotIdx = input.gateId & QUGATE_GATE_ID_SLOT_MASK;
        locals.encodedGen = (input.gateId >> QUGATE_GATE_ID_SLOT_BITS);
        if (input.gateId == 0
            || locals.slotIdx >= state.get()._gateCount
            || locals.encodedGen == 0
            || state.get()._gateGenerations.get(locals.slotIdx) != (uint16)(locals.encodedGen - 1))
        {
            output.status = QUGATE_INVALID_GATE_ID;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_GATE;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.gate = state.get()._gates.get(locals.slotIdx);

        // Must be gate owner
        if (locals.gate.owner != qpi.invocator())
        {
            output.status = QUGATE_UNAUTHORIZED;
            locals.logger._type = QUGATE_LOG_FAIL_UNAUTHORIZED;
            LOG_WARNING(locals.logger);
            return;
        }

        // Gate must be active
        if (locals.gate.active == 0)
        {
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Gate must be INHERITANCE mode
        if (locals.gate.mode != QUGATE_MODE_INHERITANCE)
        {
            output.status = QUGATE_INHERITANCE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Cannot reconfigure after trigger
        if (state.get()._inheritanceConfigs.get(locals.slotIdx).triggered == 1)
        {
            output.status = QUGATE_INHERITANCE_TRIGGERED;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate thresholdEpochs >= 1
        if (input.thresholdEpochs == 0)
        {
            output.status = QUGATE_INVALID_INHERITANCE;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate payoutPercentPerEpoch 1-100
        if (input.payoutPercentPerEpoch == 0 || input.payoutPercentPerEpoch > 100)
        {
            output.status = QUGATE_INVALID_INHERITANCE;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate beneficiary count
        if (input.beneficiaryCount == 0 || input.beneficiaryCount > 8)
        {
            output.status = QUGATE_INVALID_INHERITANCE;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate shares sum to 100
        locals.shareSum = 0;
        for (locals.i = 0; locals.i < input.beneficiaryCount; locals.i++)
        {
            locals.shareSum += input.beneficiaryShares.get(locals.i);
        }
        if (locals.shareSum != 100)
        {
            output.status = QUGATE_INVALID_INHERITANCE;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Store config
        locals.cfg.thresholdEpochs = input.thresholdEpochs;
        locals.cfg.lastKeepAliveEpoch = (uint32)qpi.epoch();
        locals.cfg.payoutPercentPerEpoch = input.payoutPercentPerEpoch;
        locals.cfg.minimumBalance = input.minimumBalance;
        locals.cfg.active = 1;
        locals.cfg.triggered = 0;
        locals.cfg.triggerEpoch = 0;
        state.mut()._inheritanceConfigs.set(locals.slotIdx, locals.cfg);

        // Store beneficiaries
        state.mut()._inheritanceBeneficiaryCount.set(locals.slotIdx, input.beneficiaryCount);
        for (locals.i = 0; locals.i < 8; locals.i++)
        {
            if (locals.i < input.beneficiaryCount)
            {
                locals.bene.address = input.beneficiaryAddresses.get(locals.i);
                locals.bene.sharePercent = input.beneficiaryShares.get(locals.i);
            }
            else
            {
                locals.bene.address = id::zero();
                locals.bene.sharePercent = 0;
            }
            state.mut()._inheritanceBeneficiaries.set(locals.slotIdx * 8 + locals.i, locals.bene);
        }

        // Update gate activity
        locals.gate.lastActivityEpoch = qpi.epoch();
        state.mut()._gates.set(locals.slotIdx, locals.gate);

        locals.logger._type = QUGATE_LOG_INHERITANCE_CONFIGURED;
        LOG_INFO(locals.logger);
    }

    // =============================================
    // keepAlive — reset the epoch counter (owner only, before trigger)
    // =============================================

    PUBLIC_PROCEDURE_WITH_LOCALS(keepAlive)
    {
        output.status = QUGATE_SUCCESS;
        output.epochRecorded = 0;

        locals.logger._contractIndex = CONTRACT_INDEX;
        locals.logger.sender = qpi.invocator();
        locals.logger.gateId = input.gateId;
        locals.logger.amount = 0;

        // Refund any attached QU
        if (qpi.invocationReward() > 0)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward());
        }

        // Decode versioned gateId
        locals.slotIdx = input.gateId & QUGATE_GATE_ID_SLOT_MASK;
        locals.encodedGen = (input.gateId >> QUGATE_GATE_ID_SLOT_BITS);
        if (input.gateId == 0
            || locals.slotIdx >= state.get()._gateCount
            || locals.encodedGen == 0
            || state.get()._gateGenerations.get(locals.slotIdx) != (uint16)(locals.encodedGen - 1))
        {
            output.status = QUGATE_INVALID_GATE_ID;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_GATE;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.gate = state.get()._gates.get(locals.slotIdx);

        // Must be gate owner
        if (locals.gate.owner != qpi.invocator())
        {
            output.status = QUGATE_UNAUTHORIZED;
            locals.logger._type = QUGATE_LOG_FAIL_UNAUTHORIZED;
            LOG_WARNING(locals.logger);
            return;
        }

        // Gate must be active
        if (locals.gate.active == 0)
        {
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Gate must be INHERITANCE mode
        if (locals.gate.mode != QUGATE_MODE_INHERITANCE)
        {
            output.status = QUGATE_INHERITANCE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.cfg = state.get()._inheritanceConfigs.get(locals.slotIdx);

        // Inheritance must be configured (active)
        if (locals.cfg.active == 0)
        {
            output.status = QUGATE_INHERITANCE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Cannot keepAlive after trigger
        if (locals.cfg.triggered == 1)
        {
            output.status = QUGATE_INHERITANCE_TRIGGERED;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Reset the epoch counter
        locals.cfg.lastKeepAliveEpoch = (uint32)qpi.epoch();
        state.mut()._inheritanceConfigs.set(locals.slotIdx, locals.cfg);

        // Update gate activity
        locals.gate.lastActivityEpoch = qpi.epoch();
        state.mut()._gates.set(locals.slotIdx, locals.gate);

        output.epochRecorded = locals.cfg.lastKeepAliveEpoch;

        locals.logger._type = QUGATE_LOG_INHERITANCE_KEEPALIVE;
        locals.logger.amount = locals.cfg.lastKeepAliveEpoch;
        LOG_INFO(locals.logger);
    }

    // =============================================
    // getInheritance — read-only query for inheritance config
    // =============================================

    PUBLIC_FUNCTION_WITH_LOCALS(getInheritance)
    {
        output.active = 0;
        output.triggered = 0;
        output.thresholdEpochs = 0;
        output.lastKeepAliveEpoch = 0;
        output.triggerEpoch = 0;
        output.payoutPercentPerEpoch = 0;
        output.minimumBalance = 0;
        output.beneficiaryCount = 0;

        // Decode versioned gateId
        locals.slotIdx = input.gateId & QUGATE_GATE_ID_SLOT_MASK;
        locals.encodedGen = (input.gateId >> QUGATE_GATE_ID_SLOT_BITS);
        if (input.gateId == 0
            || locals.slotIdx >= state.get()._gateCount
            || locals.encodedGen == 0
            || state.get()._gateGenerations.get(locals.slotIdx) != (uint16)(locals.encodedGen - 1))
        {
            return;
        }

        locals.gate = state.get()._gates.get(locals.slotIdx);
        if (locals.gate.mode != QUGATE_MODE_INHERITANCE)
        {
            return;
        }

        locals.cfg = state.get()._inheritanceConfigs.get(locals.slotIdx);
        output.active = locals.cfg.active;
        output.triggered = locals.cfg.triggered;
        output.thresholdEpochs = locals.cfg.thresholdEpochs;
        output.lastKeepAliveEpoch = locals.cfg.lastKeepAliveEpoch;
        output.triggerEpoch = locals.cfg.triggerEpoch;
        output.payoutPercentPerEpoch = locals.cfg.payoutPercentPerEpoch;
        output.minimumBalance = locals.cfg.minimumBalance;

        output.beneficiaryCount = state.get()._inheritanceBeneficiaryCount.get(locals.slotIdx);
        for (locals.i = 0; locals.i < 8; locals.i++)
        {
            locals.bene = state.get()._inheritanceBeneficiaries.get(locals.slotIdx * 8 + locals.i);
            output.beneficiaryAddresses.set(locals.i, locals.bene.address);
            output.beneficiaryShares.set(locals.i, locals.bene.sharePercent);
        }
    }

    // =============================================
    // Registration
    // =============================================

    REGISTER_USER_FUNCTIONS_AND_PROCEDURES()
    {
        // Index assignments: 1=createGate 2=sendToGate 3=closeGate 4=updateGate 5=getGate 6=getGateCount 7=getGatesByOwner 8=getGateBatch 9=getFees 10=fundGate 11=setChain 12=sendToGateVerified 13=configureInheritance 14=keepAlive 15=getInheritance
        REGISTER_USER_PROCEDURE(createGate, 1);
        REGISTER_USER_PROCEDURE(sendToGate, 2);
        REGISTER_USER_PROCEDURE(closeGate, 3);
        REGISTER_USER_PROCEDURE(updateGate, 4);
        REGISTER_USER_FUNCTION(getGate, 5);
        REGISTER_USER_FUNCTION(getGateCount, 6);
        REGISTER_USER_FUNCTION(getGatesByOwner, 7);
        REGISTER_USER_FUNCTION(getGateBatch, 8);
        REGISTER_USER_FUNCTION(getFees, 9);
        REGISTER_USER_PROCEDURE(fundGate, 10);
        REGISTER_USER_PROCEDURE(setChain, 11);
        REGISTER_USER_PROCEDURE(sendToGateVerified, 12);
        REGISTER_USER_PROCEDURE(configureInheritance, 13);
        REGISTER_USER_PROCEDURE(keepAlive, 14);
        REGISTER_USER_FUNCTION(getInheritance, 15);
        REGISTER_USER_PROCEDURE_NOTIFICATION(OraclePriceNotification);
    }

    // =============================================
    // System procedures
    // =============================================

    INITIALIZE()
    {
        state.mut()._gateCount = 0;
        state.mut()._activeGates = 0;
        state.mut()._freeCount = 0;
        state.mut()._totalBurned = 0;                           //
        state.mut()._creationFee = QUGATE_DEFAULT_CREATION_FEE;  //
        state.mut()._minSendAmount = QUGATE_DEFAULT_MIN_SEND;    //
        state.mut()._expiryEpochs = QUGATE_DEFAULT_EXPIRY_EPOCHS; //

        // Zero all generation counters
        for (uint64 i = 0; i < QUGATE_MAX_GATES; i++)
        {
            state.mut()._gateGenerations.set(i, 0);
        }
    }

    BEGIN_EPOCH_WITH_LOCALS()
    {
        // Subscribe oracle gates at the start of each epoch
        for (locals.i = 0; locals.i < state.get()._gateCount; locals.i++)
        {
            locals.gate = state.get()._gates.get(locals.i);
            if (locals.gate.active == 1 && locals.gate.mode == QUGATE_MODE_ORACLE)
            {
                if (locals.gate.oracleReserve >= QUGATE_ORACLE_SUBSCRIPTION_FEE)
                {
                    // Construct the Price oracle query
                    OI::Price::OracleQuery query;
                    query.oracle = locals.gate.oracleId;
                    query.currency1 = locals.gate.oracleCurrency1;
                    query.currency2 = locals.gate.oracleCurrency2;

                    locals.subId = SUBSCRIBE_ORACLE(OI::Price, query, OraclePriceNotification,
                        QUGATE_ORACLE_NOTIFY_PERIOD_MS, true);

                    if (locals.subId >= 0)
                    {
                        locals.gate.oracleSubscriptionId = locals.subId;
                        locals.gate.oracleReserve -= QUGATE_ORACLE_SUBSCRIPTION_FEE;
                        state.mut()._gates.set(locals.i, locals.gate);
                        state.mut()._subscriptionToSlot.set(locals.subId, locals.i);

                        locals.logger._contractIndex = CONTRACT_INDEX;
                        locals.logger._type = QUGATE_LOG_ORACLE_SUBSCRIBED;
                        locals.logger.gateId = ((uint64)(state.get()._gateGenerations.get(locals.i) + 1) << QUGATE_GATE_ID_SLOT_BITS) | locals.i;
                        locals.logger.sender = locals.gate.owner;
                        locals.logger.amount = QUGATE_ORACLE_SUBSCRIPTION_FEE;
                        LOG_INFO(locals.logger);
                    }
                }
                else
                {
                    // Oracle reserve exhausted — log and subscription lapses
                    locals.logger._contractIndex = CONTRACT_INDEX;
                    locals.logger._type = QUGATE_LOG_ORACLE_EXHAUSTED;
                    locals.logger.gateId = ((uint64)(state.get()._gateGenerations.get(locals.i) + 1) << QUGATE_GATE_ID_SLOT_BITS) | locals.i;
                    locals.logger.sender = locals.gate.owner;
                    locals.logger.amount = locals.gate.oracleReserve;
                    LOG_INFO(locals.logger);
                }
            }
        }
    }

    END_EPOCH_WITH_LOCALS()
    {
        // Expire inactive gates
        for (locals.i = 0; locals.i < state.get()._gateCount; locals.i++)
        {
            locals.gate = state.get()._gates.get(locals.i);

            // active==1 guard prevents double-close / activeGates underflow (intentional)
            if (locals.gate.active == 1 && state.get()._expiryEpochs > 0)
            {
                if (qpi.epoch() - locals.gate.lastActivityEpoch >= state.get()._expiryEpochs)
                {
                    // Refund any held balance (THRESHOLD / ORACLE mode)
                    if (locals.gate.currentBalance > 0)
                    {
                        qpi.transfer(locals.gate.owner, locals.gate.currentBalance);
                        locals.gate.currentBalance = 0;
                    }

                    // Refund oracle reserve on expiry
                    if (locals.gate.mode == QUGATE_MODE_ORACLE)
                    {
                        if (locals.gate.oracleReserve > 0)
                        {
                            qpi.transfer(locals.gate.owner, locals.gate.oracleReserve);
                            locals.gate.oracleReserve = 0;
                        }
                        if (locals.gate.oracleSubscriptionId >= 0)
                        {
                            state.mut()._subscriptionToSlot.removeByKey(locals.gate.oracleSubscriptionId);
                        }
                    }

                    // Refund chain reserve on expiry
                    if (locals.gate.chainReserve > 0)
                    {
                        qpi.transfer(locals.gate.owner, locals.gate.chainReserve);
                        locals.gate.chainReserve = 0;
                    }

                    locals.gate.active = 0;
                    state.mut()._gates.set(locals.i, locals.gate);
                    state.mut()._activeGates -= 1;

                    // Push slot onto free-list
                    state.mut()._freeSlots.set(state.get()._freeCount, locals.i);
                    state.mut()._freeCount += 1;

                    // Log expiry (before generation increment, so we log the expired gate's ID)
                    locals.logger._contractIndex = CONTRACT_INDEX;
                    locals.logger._type = QUGATE_LOG_GATE_EXPIRED;
                    locals.logger.gateId = ((uint64)(state.get()._gateGenerations.get(locals.i) + 1) << QUGATE_GATE_ID_SLOT_BITS) | locals.i;

                    // Increment generation so recycled slot gets a new gateId
                    state.mut()._gateGenerations.set(locals.i, state.get()._gateGenerations.get(locals.i) + 1);
                    locals.logger.sender = locals.gate.owner;
                    locals.logger.amount = 0;
                    LOG_INFO(locals.logger);
                }
            }
        }

        // TODO: Check shareholder proposals and apply fee/expiry changes
        // When DEFINE_SHAREHOLDER_PROPOSAL_STORAGE is enabled:
        //   - Check if any proposal passed quorum
        //   - If so, update state.mut()._creationFee, state.mut()._minSendAmount, state.mut()._expiryEpochs
        //   - Log fee change event

        // =============================================
        // Inheritance gate processing — epoch-based dead man's switch
        // =============================================
        for (locals.i = 0; locals.i < state.get()._gateCount; locals.i++)
        {
            locals.gate = state.get()._gates.get(locals.i);

            // Skip non-INHERITANCE gates and inactive gates
            if (locals.gate.active == 0 || locals.gate.mode != QUGATE_MODE_INHERITANCE)
            {
                continue;
            }

            locals.inhCfg = state.get()._inheritanceConfigs.get(locals.i);

            // Skip if inheritance not yet configured
            if (locals.inhCfg.active == 0)
            {
                continue;
            }

            if (locals.inhCfg.triggered == 1)
            {
                // Already triggered — execute recurring payout
                locals.inhBalance = (sint64)locals.gate.currentBalance;
                if (locals.inhBalance > locals.inhCfg.minimumBalance)
                {
                    locals.inhPayoutTotal = locals.inhBalance * (sint64)locals.inhCfg.payoutPercentPerEpoch / 100;
                    if (locals.inhPayoutTotal > 0)
                    {
                        locals.inhBeneCount = state.get()._inheritanceBeneficiaryCount.get(locals.i);

                        for (locals.inhJ = 0; locals.inhJ < locals.inhBeneCount; locals.inhJ++)
                        {
                            locals.inhBene = state.get()._inheritanceBeneficiaries.get(locals.i * 8 + locals.inhJ);
                            if (locals.inhJ == locals.inhBeneCount - 1)
                            {
                                // Last beneficiary gets remainder to avoid dust from rounding
                                locals.inhPriorSum = 0;
                                for (locals.inhK = 0; locals.inhK < locals.inhJ; locals.inhK++)
                                {
                                    locals.inhPriorBene = state.get()._inheritanceBeneficiaries.get(locals.i * 8 + locals.inhK);
                                    locals.inhPriorSum += locals.inhPayoutTotal * (sint64)locals.inhPriorBene.sharePercent / 100;
                                }
                                locals.inhShare = locals.inhPayoutTotal - locals.inhPriorSum;
                            }
                            else
                            {
                                locals.inhShare = locals.inhPayoutTotal * (sint64)locals.inhBene.sharePercent / 100;
                            }

                            if (locals.inhShare > 0)
                            {
                                qpi.transfer(locals.inhBene.address, locals.inhShare);
                                locals.gate.totalForwarded += (uint64)locals.inhShare;
                                locals.gate.currentBalance -= (uint64)locals.inhShare;
                            }
                        }

                        state.mut()._gates.set(locals.i, locals.gate);

                        // Log payout
                        locals.logger._contractIndex = CONTRACT_INDEX;
                        locals.logger._type = QUGATE_LOG_INHERITANCE_PAYOUT;
                        locals.logger.gateId = ((uint64)(state.get()._gateGenerations.get(locals.i) + 1) << QUGATE_GATE_ID_SLOT_BITS) | locals.i;
                        locals.logger.sender = locals.gate.owner;
                        locals.logger.amount = locals.inhPayoutTotal;
                        LOG_INFO(locals.logger);
                    }

                    // Auto-close gate if balance dropped to or below minimum
                    locals.gate = state.get()._gates.get(locals.i);
                    if ((sint64)locals.gate.currentBalance <= locals.inhCfg.minimumBalance)
                    {
                        // Refund any remaining dust to owner rather than lose it
                        if (locals.gate.currentBalance > 0)
                        {
                            qpi.transfer(locals.gate.owner, locals.gate.currentBalance);
                            locals.gate.currentBalance = 0;
                        }

                        locals.gate.active = 0;
                        state.mut()._gates.set(locals.i, locals.gate);
                        state.mut()._activeGates -= 1;

                        state.mut()._freeSlots.set(state.get()._freeCount, locals.i);
                        state.mut()._freeCount += 1;

                        state.mut()._gateGenerations.set(locals.i, state.get()._gateGenerations.get(locals.i) + 1);

                        locals.logger._contractIndex = CONTRACT_INDEX;
                        locals.logger._type = QUGATE_LOG_GATE_CLOSED;
                        locals.logger.gateId = ((uint64)(state.get()._gateGenerations.get(locals.i)) << QUGATE_GATE_ID_SLOT_BITS) | locals.i;
                        locals.logger.sender = locals.gate.owner;
                        locals.logger.amount = 0;
                        LOG_INFO(locals.logger);
                    }
                }
            }
            else
            {
                // Not yet triggered — check if threshold exceeded
                locals.inhEpochsInactive = (uint32)qpi.epoch() - locals.inhCfg.lastKeepAliveEpoch;
                if (locals.inhEpochsInactive > locals.inhCfg.thresholdEpochs)
                {
                    // Trigger inheritance!
                    locals.inhCfg.triggered = 1;
                    locals.inhCfg.triggerEpoch = (uint32)qpi.epoch();
                    state.mut()._inheritanceConfigs.set(locals.i, locals.inhCfg);

                    locals.logger._contractIndex = CONTRACT_INDEX;
                    locals.logger._type = QUGATE_LOG_INHERITANCE_TRIGGERED;
                    locals.logger.gateId = ((uint64)(state.get()._gateGenerations.get(locals.i) + 1) << QUGATE_GATE_ID_SLOT_BITS) | locals.i;
                    locals.logger.sender = locals.gate.owner;
                    locals.logger.amount = (sint64)locals.gate.currentBalance;
                    LOG_INFO(locals.logger);
                }
            }
        }
    }

    BEGIN_TICK()
    {
    }
    END_TICK()
    {
    }

};


