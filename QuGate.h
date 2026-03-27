// QuGate.h - Programmable Payment Gate Contract
// Short name: QUGATE
// Description: Universal payment routing with predefined gate modes.
//   Create gates with configurable rules, point payments at them,
//   and QU flows according to the logic.
//
// Gate Modes:
//   SPLIT       - Distribute to N addresses by ratio (e.g. 40:30:20:10)
//   ROUND_ROBIN - Cycle through addresses, one per payment
//   THRESHOLD   - Accumulate until amount reached, then forward
//   RANDOM      - Select one recipient per payment using tick-based entropy
//   CONDITIONAL - Only forward if sender matches whitelist, else bounce
//   ORACLE      - Oracle-triggered distribution based on price/time conditions
//   HEARTBEAT   - Dead-man's switch; distributes if not pinged within N epochs
//                 Recipients configured via configureHeartbeat (recipientCount=0 at creation)
//   MULTISIG    - M-of-N guardian approval before funds release
//                 Guardians configured via configureMultisig (recipientCount=0 at creation)
//   TIME_LOCK   - Funds locked until a target epoch, then released
//                 Unlock configured via configureTimeLock (recipientCount=0 at creation)
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

// Contract index — assigned by core after proposal approval.
// When building testnet locally, pass -DCONTRACT_INDEX=26 via cmake flags
// instead of modifying this file (qubic-contract-verify rejects preprocessor directives).

// Capacity scales with network via X_MULTIPLIER
constexpr uint64 QUGATE_INITIAL_MAX_GATES = 256;  // testnet size; scale up for mainnet
constexpr uint64 QUGATE_MAX_GATES = QUGATE_INITIAL_MAX_GATES * X_MULTIPLIER;
constexpr uint64 QUGATE_MAX_RECIPIENTS = 8;
constexpr uint64 QUGATE_MAX_RATIO = 10000;       // Max ratio per recipient (prevents overflow)

// Default fees — initial values, changeable via shareholder vote
constexpr uint64 QUGATE_DEFAULT_CREATION_FEE = 100000;
constexpr uint64 QUGATE_DEFAULT_MIN_SEND = 1000;

// Escalating fee: fee = baseFee * (1 + QPI::div(activeGates, FEE_ESCALATION_STEP))
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
constexpr uint8 QUGATE_MODE_HEARTBEAT = 6;  // Heartbeat gate — heartbeat() or epoch-triggered payout
constexpr uint8 QUGATE_MODE_MULTISIG = 7;     // M-of-N guardian approval before funds release
constexpr uint8 QUGATE_MODE_TIME_LOCK = 8;    // Hold funds until a target epoch, then release to recipient

// Minimal execution observability outcome types
constexpr uint8 QUGATE_EXEC_NONE      = 0;
constexpr uint8 QUGATE_EXEC_FORWARDED = 1;
constexpr uint8 QUGATE_EXEC_HELD      = 2;
constexpr uint8 QUGATE_EXEC_REFUNDED  = 3;
constexpr uint8 QUGATE_EXEC_BURNED    = 4;
constexpr uint8 QUGATE_EXEC_REJECTED  = 5;

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
constexpr sint64 QUGATE_INVALID_MODE           = -4;  // Mode value exceeds QUGATE_MODE_TIME_LOCK (8)
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
constexpr sint64 QUGATE_HEARTBEAT_TRIGGERED  = -16; // heartbeat() called after heartbeat already triggered
constexpr sint64 QUGATE_HEARTBEAT_NOT_ACTIVE = -17; // heartbeat() or configureHeartbeat() on non-HEARTBEAT gate
constexpr sint64 QUGATE_HEARTBEAT_INVALID    = -18; // Invalid heartbeat config (bad shares, percent, threshold)
constexpr sint64 QUGATE_MULTISIG_NOT_GUARDIAN    = -19; // sender not in guardian list
constexpr sint64 QUGATE_MULTISIG_ALREADY_VOTED   = -20; // guardian already voted this proposal
constexpr sint64 QUGATE_MULTISIG_INVALID_CONFIG  = -21; // bad guardians/required config
constexpr sint64 QUGATE_MULTISIG_NO_ACTIVE_PROP  = -22; // no active proposal to query
constexpr sint64 QUGATE_TIME_LOCK_ALREADY_FIRED   = -23; // gate already unlocked and closed
constexpr sint64 QUGATE_TIME_LOCK_NOT_CANCELLABLE = -24; // cancelTimeLock() called but cancellable=0
constexpr sint64 QUGATE_TIME_LOCK_EPOCH_PAST      = -25; // unlockEpoch is in the past at creation
constexpr uint8 QUGATE_TIME_LOCK_ABSOLUTE_EPOCH   = 0;
constexpr uint8 QUGATE_TIME_LOCK_RELATIVE_EPOCHS  = 1;
constexpr sint64 QUGATE_ADMIN_GATE_REQUIRED       = -26; // config change needs admin gate approval
constexpr sint64 QUGATE_INVALID_ADMIN_GATE        = -27; // adminGateId doesn't exist or isn't MULTISIG mode
constexpr sint64 QUGATE_INVALID_GATE_RECIPIENT    = -28; // recipientGateIds entry is invalid (bad slot/gen/inactive)
constexpr sint64 QUGATE_INVALID_ADMIN_CYCLE       = -29; // adminGateId creates a circular admin chain (self or loop)
constexpr sint64 QUGATE_MULTISIG_PROPOSAL_ACTIVE  = -30; // configureMultisig blocked while proposal is in progress

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
constexpr uint32 QUGATE_LOG_HEARTBEAT_CONFIGURED = 15;  // configureHeartbeat() called
constexpr uint32 QUGATE_LOG_HEARTBEAT_PULSE  = 16;  // heartbeat() called, epoch reset
constexpr uint32 QUGATE_LOG_HEARTBEAT_TRIGGERED  = 17;  // threshold exceeded, heartbeat triggered
constexpr uint32 QUGATE_LOG_HEARTBEAT_PAYOUT     = 18;  // recurring payout dispatched
constexpr uint32 QUGATE_LOG_MULTISIG_VOTE       = 19; // guardian voted
constexpr uint32 QUGATE_LOG_MULTISIG_EXECUTED   = 20; // threshold reached, funds released
constexpr uint32 QUGATE_LOG_MULTISIG_EXPIRED    = 21; // proposal expired, reset
constexpr uint32 QUGATE_LOG_MULTISIG_CONFIGURED = 22; // guardians/threshold updated
constexpr uint32 QUGATE_LOG_TIME_LOCK_FIRED      = 23; // unlock epoch reached, funds released
constexpr uint32 QUGATE_LOG_TIME_LOCK_CANCELLED  = 24; // owner cancelled, funds refunded
constexpr uint32 QUGATE_LOG_TIME_LOCK_CONFIGURED = 25; // time lock created
constexpr uint32 QUGATE_LOG_ADMIN_GATE_SET       = 26; // admin gate assigned to a gate
constexpr uint32 QUGATE_LOG_ADMIN_GATE_CLEARED   = 27; // admin gate removed from a gate
constexpr uint32 QUGATE_LOG_ADMIN_APPROVAL_USED  = 28; // admin gate approval consumed for a config change

// Failure log types use high range
constexpr uint32 QUGATE_LOG_FAIL_INVALID_GATE = 100;
constexpr uint32 QUGATE_LOG_FAIL_NOT_ACTIVE = 101;
constexpr uint32 QUGATE_LOG_FAIL_UNAUTHORIZED = 102;
constexpr uint32 QUGATE_LOG_FAIL_INVALID_PARAMS = 103;
constexpr uint32 QUGATE_LOG_FAIL_INSUFFICIENT_FEE = 104;
constexpr uint32 QUGATE_LOG_FAIL_NO_SLOTS = 105;
constexpr uint32 QUGATE_LOG_FAIL_OWNER_MISMATCH = 106;

// Asset name for shareholder proposals
constexpr uint64 QUGATE_CONTRACT_ASSET_NAME = 76228174763345ULL; // QUGATE as uint64 little-endian

// Future extension struct (Qubic convention)
struct QUGATE2
{
};

// =============================================
// Heartbeat gate supporting structs
// (defined outside QUGATE so they can be used in StateData)
// =============================================

// Per-gate heartbeat configuration
struct QUGATE_HeartbeatConfig
{
    uint32  thresholdEpochs;       // epochs without heartbeat() before trigger (>= 1)
    uint32  lastHeartbeatEpoch;    // epoch of last heartbeat() call (or configureHeartbeat)
    uint8   payoutPercentPerEpoch; // percent of balance to pay each epoch after trigger (1-100)
    sint64  minimumBalance;        // stop paying when balance drops below this
    uint8   active;                // 1 = heartbeat mode enabled on this gate
    uint8   triggered;             // 1 once threshold exceeded
    uint32  triggerEpoch;          // epoch when triggered
    Array<id, 8>    beneficiaryAddresses;  // inline beneficiary addresses
    Array<uint8, 8> beneficiaryShares;     // inline beneficiary share percents
    uint8   beneficiaryCount;
};

// =============================================
// Multisig gate supporting struct
// (defined outside QUGATE so it can be used in StateData)
// =============================================

struct QUGATE_MultisigConfig
{
    Array<id, 8> guardians;
    uint8        guardianCount;
    uint8        required;              // M of N required approvals
    uint32       proposalExpiryEpochs;  // epochs before unfinished proposal resets
    uint32       adminApprovalWindowEpochs; // epochs a reached quorum authorizes governed actions
    uint8        approvalBitmap;        // bit i set = guardian i has voted
    uint8        approvalCount;         // current vote count
    uint32       proposalEpoch;         // epoch when first vote was cast
    uint8        proposalActive;        // 1 if proposal in progress
};

struct QUGATE_AdminApprovalState
{
    uint8  active;               // 1 if admin approval can currently authorize a governed action
    uint32 validUntilEpoch;      // inclusive: approval valid while currentEpoch <= validUntilEpoch
};

// =============================================
// TIME_LOCK gate supporting struct
// (defined outside QUGATE so it can be used in StateData)
// =============================================

struct QUGATE_TimeLockConfig
{
    uint32 unlockEpoch;    // epoch when funds release
    uint32 delayEpochs;    // relative delay from first funding when lockMode == RELATIVE
    uint8  lockMode;       // 0 = absolute epoch, 1 = relative epochs
    uint8  cancellable;    // 1 = owner can cancel and refund before unlock
    uint8  fired;          // 1 once funds have been released
    uint8  cancelled;      // 1 if cancelled by owner
    uint8  active;         // 1 = time lock is configured on this gate
};

// HeartbeatBeneficiary removed — beneficiaries are now stored inline in QUGATE_HeartbeatConfig

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

        // Admin gate fields — governance via MULTISIG quorum
        sint64 adminGateId;       // -1 = no admin gate (owner-only). Set to a MULTISIG gate ID for quorum-controlled config.
        uint8  hasAdminGate;      // 1 if adminGateId is set

        // Gate-as-recipient — allows recipients to be other gates for internal routing
        Array<sint64, 8> recipientGateIds;  // Per-recipient: -1 = wallet, >= 0 = versioned gate ID
    };

    struct QUGATE_LatestExecution
    {
        uint8  valid;                  // 1 if this slot has an observed latest execution
        uint8  mode;                   // Gate mode at execution time
        uint8  outcomeType;            // QUGATE_EXEC_*
        uint8  selectedRecipientIndex; // 255 = none / not applicable
        sint64 selectedDownstreamGateId; // recipientGateIds[selectedRecipientIndex] or -1
        uint64 forwardedAmount;
        uint64 observedTick;
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

        // Gate-as-recipient
        Array<sint64, 8> recipientGateIds;  // -1 = wallet, >= 0 = gate ID (default all -1)
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

        // Gate-as-recipient
        Array<sint64, 8> recipientGateIds;  // -1 = wallet, >= 0 = gate ID
    };
    struct updateGate_output
    {
        sint64 status;          //
    };

    // Adds invocationReward to a gate reserve. reserveTarget: 0=oracleReserve, 1=chainReserve.
    struct fundGate_input
    {
        uint64 gateId;
        uint8  reserveTarget;  // 0 = oracleReserve, 1 = chainReserve
    };
    struct fundGate_output
    {
        sint64 result;
    };

    // Sets or clears the chain link on a gate. nextGateId=-1 clears the chain. Owner only. Burns QUGATE_CHAIN_HOP_FEE.
    struct setChain_input
    {
        uint64 gateId;
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

    // Configure heartbeat mode on a HEARTBEAT gate.
    // Owner-only. Must be called before the gate can trigger.
    struct configureHeartbeat_input
    {
        uint64 gateId;
        uint32 thresholdEpochs;          // >= 1
        uint8  payoutPercentPerEpoch;     // 1–100
        sint64 minimumBalance;            // stop payouts when balance falls below this
        Array<id, 8>    beneficiaryAddresses;
        Array<uint8, 8> beneficiaryShares; // must sum to 100
        uint8  beneficiaryCount;           // 1–8
    };
    struct configureHeartbeat_output
    {
        sint64 status;
    };

    // Reset the heartbeat epoch counter. Owner-only. Rejected after trigger.
    struct heartbeat_input
    {
        uint64 gateId;
    };
    struct heartbeat_output
    {
        sint64 status;
        uint32 epochRecorded;
    };

    // Query heartbeat config and beneficiaries for a gate.
    struct getHeartbeat_input
    {
        uint64 gateId;
    };
    struct getHeartbeat_output
    {
        uint8   active;
        uint8   triggered;
        uint32  thresholdEpochs;
        uint32  lastHeartbeatEpoch;
        uint32  triggerEpoch;
        uint8   payoutPercentPerEpoch;
        sint64  minimumBalance;
        uint8   beneficiaryCount;
        Array<id, 8>    beneficiaryAddresses;
        Array<uint8, 8> beneficiaryShares;
    };

    // Configure M-of-N multisig guardians/threshold on a MULTISIG gate. Owner-only.
    struct configureMultisig_input
    {
        uint64       gateId;
        Array<id, 8> guardians;
        uint8        guardianCount;           // 1-8
        uint8        required;                // 1-guardianCount
        uint32       proposalExpiryEpochs;    // >= 1
        uint32       adminApprovalWindowEpochs; // >= 1
    };
    struct configureMultisig_output
    {
        sint64 status;
    };

    // Query current multisig proposal state for a MULTISIG gate.
    struct getMultisigState_input
    {
        uint64 gateId;
    };
    struct getMultisigState_output
    {
        sint64 status;
        uint8  approvalBitmap;
        uint8  approvalCount;
        uint8  required;
        uint8  guardianCount;
        uint32 proposalEpoch;
        uint8  proposalActive;
        Array<id, 8> guardians;  // Guardian public keys from QUGATE_MultisigConfig
    };

    // Configure TIME_LOCK mode on a TIME_LOCK gate. Owner-only.
    struct configureTimeLock_input
    {
        uint64 gateId;
        uint32 unlockEpoch;   // absolute mode: must be > current epoch
        uint32 delayEpochs;   // relative mode: must be > 0
        uint8  lockMode;      // 0 = absolute epoch, 1 = relative epochs
        uint8  cancellable;   // 1 = allow owner to cancel before unlock
    };
    struct configureTimeLock_output
    {
        sint64 status;
    };

    // Cancel a TIME_LOCK gate (owner-only, only if cancellable=1).
    struct cancelTimeLock_input
    {
        uint64 gateId;
    };
    struct cancelTimeLock_output
    {
        sint64 status;
    };

    // Set or clear the admin gate on a gate. Owner-only if no admin gate set; admin gate approval required if already set.
    struct setAdminGate_input
    {
        uint64 gateId;
        sint64 adminGateId;   // -1 to clear admin gate
    };
    struct setAdminGate_output
    {
        sint64 status;
    };

    // Query admin gate configuration for a gate.
    struct getAdminGate_input
    {
        uint64 gateId;
    };
    struct getAdminGate_output
    {
        uint8  hasAdminGate;
        sint64 adminGateId;
        uint8  adminGateMode;  // mode of the admin gate (should be QUGATE_MODE_MULTISIG)
        uint8  guardianCount;      // Number of guardians on the admin gate
        uint8  required;           // M-of-N threshold
        uint32 adminApprovalWindowEpochs;
        uint8  adminApprovalActive;
        uint32 adminApprovalValidUntilEpoch;
        Array<id, 8> guardians;    // Guardian public keys from the admin gate's QUGATE_MultisigConfig
    };

    // Withdraw from a gate's oracleReserve or chainReserve without closing. Owner only (or admin gate).
    struct withdrawReserve_input
    {
        uint64 gateId;
        uint8  reserveTarget;  // 0 = oracleReserve, 1 = chainReserve
        uint64 amount;         // 0 = withdraw all
    };
    struct withdrawReserve_output
    {
        sint64 status;
        uint64 withdrawn;
    };

    // Query gates by mode — returns up to 16 active gates matching a given mode.
    struct getGatesByMode_input
    {
        uint8 mode;
    };
    struct getGatesByMode_output
    {
        Array<uint64, QUGATE_MAX_OWNER_GATES> gateIds;
        uint64 count;
    };

    // Query gate by raw slot index (no generation validation).
    // Returns gate data regardless of active/closed/recycled state.
    // Useful for listing all gates including closed ones.
    struct getGateBySlot_input
    {
        uint64 slotIndex;
    };
    struct getGateBySlot_output
    {
        uint8 valid;            // 1 if slot is within gateCount range
        uint64 gateId;          // current versioned gate ID for this slot
        uint16 generation;      // current generation counter
        // Same fields as getGate_output:
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
        Array<id, 8> allowedSenders;
        uint8 allowedSenderCount;
        sint64 oracleReserve;
        sint32 oracleSubscriptionId;
        sint64 chainNextGateId;
        sint64 chainReserve;
        uint8  chainDepth;
        sint64 adminGateId;
        uint8  hasAdminGate;
        Array<sint64, 8> recipientGateIds;
    };
    struct getGateBySlot_locals
    {
        GateConfig gate;
        uint64 i;
    };

    struct getLatestExecution_input
    {
        uint64 gateId;
    };
    struct getLatestExecution_output
    {
        uint8  valid;
        uint8  mode;
        uint8  outcomeType;
        uint8  selectedRecipientIndex;
        sint64 selectedDownstreamGateId;
        uint64 forwardedAmount;
        uint64 observedTick;
    };
    struct getLatestExecution_locals
    {
        uint64 slotIdx;
        uint64 encodedGen;
        QUGATE_LatestExecution latestExecution;
    };

    // Query TIME_LOCK state for a gate.
    struct getTimeLockState_input
    {
        uint64 gateId;
    };
    struct getTimeLockState_output
    {
        sint64 status;
        uint32 unlockEpoch;
        uint32 delayEpochs;
        uint8  lockMode;
        uint8  cancellable;
        uint8  fired;
        uint8  cancelled;
        uint8  active;
        sint64 currentBalance;
        uint32 currentEpoch;
        uint32 epochsRemaining;  // 0 if fired or past unlock epoch
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
        sint64 adminGateId;    // -1 if no admin gate
        uint8  hasAdminGate;   // 1 if governed by admin gate

        // Gate-as-recipient
        Array<sint64, 8> recipientGateIds;
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
        HashMap<sint32, uint64, QUGATE_MAX_GATES> _subscriptionToSlot;

        // Heartbeat mode state — indexed by gate slot (same index as _gates)
        // Beneficiaries are stored inline in QUGATE_HeartbeatConfig
        Array<QUGATE_HeartbeatConfig, QUGATE_MAX_GATES> _heartbeatConfigs;

        // Multisig mode state — indexed by gate slot (same index as _gates)
        Array<QUGATE_MultisigConfig, QUGATE_MAX_GATES> _multisigConfigs;

        // Admin approval window state — indexed by gate slot (same index as _gates)
        Array<QUGATE_AdminApprovalState, QUGATE_MAX_GATES> _adminApprovalStates;

        // TIME_LOCK mode state — indexed by gate slot (same index as _gates)
        Array<QUGATE_TimeLockConfig, QUGATE_MAX_GATES> _timeLockConfigs;

        // Minimal latest execution metadata — one record per gate slot
        Array<QUGATE_LatestExecution, QUGATE_MAX_GATES> _latestExecutions;
    };

    // =============================================
    // Locals — all variables declared here, not inline
    // =============================================

    struct createGate_locals
    {
        sint64 invReward;
        QuGateLogger logger;
        GateConfig newGate;
        QUGATE_AdminApprovalState adminApprovalZero;
        QUGATE_LatestExecution latestExecZero;
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
        // Gate-as-recipient deferred routing
        uint8 deferredCount;
        Array<uint64, 8> deferredGateSlots;    // resolved slot indices
        Array<sint64, 8> deferredGateAmounts;   // amount per deferred gate-recipient
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
        // Gate-as-recipient deferred routing (single recipient)
        uint8 deferredToGate;     // 1 if the target recipient was a gate
        uint64 deferredGateSlot;  // resolved slot index
        sint64 deferredGateAmount; // amount to route
    };
    struct processRoundRobin_locals
    {
        GateConfig gate;
        QUGATE_LatestExecution latestExec;
    };

    struct processThreshold_input
    {
        uint64 gateIdx;
        sint64 amount;
    };
    struct processThreshold_output
    {
        uint64 forwarded;
        // Gate-as-recipient deferred routing (single recipient)
        uint8 deferredToGate;     // 1 if the target recipient was a gate
        uint64 deferredGateSlot;  // resolved slot index
        sint64 deferredGateAmount; // amount to route
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
        // Gate-as-recipient deferred routing (single recipient)
        uint8 deferredToGate;     // 1 if the target recipient was a gate
        uint64 deferredGateSlot;  // resolved slot index
        sint64 deferredGateAmount; // amount to route
    };
    struct processRandom_locals
    {
        GateConfig gate;
        uint64 recipientIdx;
        QUGATE_LatestExecution latestExec;
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
        // Gate-as-recipient deferred routing (single recipient)
        uint8 deferredToGate;     // 1 if the target recipient was a gate
        uint64 deferredGateSlot;  // resolved slot index
        sint64 deferredGateAmount; // amount to route
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
        // Deferred gate-as-recipient routing — caller must dispatch via routeToGate
        uint8 deferredCount;
        Array<uint64, 8> deferredGateSlots;
        Array<sint64, 8> deferredGateAmounts;
        uint8 deferredHopCount;
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

    struct processMultisigVote_input { uint64 slotIdx; uint64 gateId; sint64 amount; };
    struct processMultisigVote_output { sint64 status; uint8 deferredToGate; uint64 deferredGateSlot; sint64 deferredGateAmount; uint8 chainForward; sint64 chainForwardAmount; };
    struct processMultisigVote_locals { GateConfig gate; QUGATE_MultisigConfig msigCfg; QUGATE_AdminApprovalState adminApproval; uint8 foundGuardian; uint8 guardianIdx; QuGateLogger logger; };

    struct fundGate_locals
    {
        sint64 invReward;
        QuGateLogger logger;
        GateConfig gate;
        uint64 slotIdx;
        uint64 encodedGen;
    };

    struct setChain_locals
    {
        sint64 invReward;
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
        // Admin gate check
        uint64 adminCheckSlot;
        GateConfig adminCheckGate;
        QUGATE_MultisigConfig adminCheckMs;
        QUGATE_AdminApprovalState adminCheckApproval;
        uint8 adminApprovalUsed;
    };

    struct sendToGateVerified_locals
    {
        sint64 invReward;
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
        // Chain forwarding
        GateConfig nextChainGate;
        // Saved deferred gate-recipients from chain hops (snapshot before dispatch)
        uint8 savedDeferredCount;
        Array<uint64, 8> savedDeferredSlots;
        Array<sint64, 8> savedDeferredAmounts;
        uint8 savedDeferredHopCount;
        uint8 deferredDispatchIdx;
        // Multisig processing
        processMultisigVote_input msigIn;
        processMultisigVote_output msigOut;
        processMultisigVote_locals msigLocals;
        // TIME_LOCK processing
        QUGATE_TimeLockConfig tlCfg;
    };

    struct sendToGate_locals
    {
        sint64 invReward;
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
        // Saved deferred gate-recipients from chain hops (snapshot before dispatch)
        uint8 savedDeferredCount;
        Array<uint64, 8> savedDeferredSlots;
        Array<sint64, 8> savedDeferredAmounts;
        uint8 savedDeferredHopCount;
        uint8 deferredDispatchIdx;
        // Multisig processing
        processMultisigVote_input msigIn;
        processMultisigVote_output msigOut;
        processMultisigVote_locals msigLocals;
        // TIME_LOCK processing
        QUGATE_TimeLockConfig tlCfg;
    };

    struct closeGate_locals
    {
        sint64 invReward;
        QuGateLogger logger;
        GateConfig gate;
        uint64 slotIdx;
        uint64 encodedGen;
        QUGATE_HeartbeatConfig hbZeroCfg;
        QUGATE_MultisigConfig msigZeroCfg;
        QUGATE_AdminApprovalState adminApprovalZero;
        QUGATE_TimeLockConfig tlZeroCfg;
        // Admin gate check
        uint64 adminCheckSlot;
        GateConfig adminCheckGate;
        QUGATE_MultisigConfig adminCheckMs;
        QUGATE_AdminApprovalState adminCheckApproval;
        uint8 adminApprovalUsed;
    };

    struct updateGate_locals
    {
        sint64 invReward;
        QuGateLogger logger;
        GateConfig gate;
        uint64 totalRatio;
        uint64 i;
        uint64 slotIdx;
        uint64 encodedGen;
        // Admin gate check
        uint64 adminCheckSlot;
        GateConfig adminCheckGate;
        QUGATE_MultisigConfig adminCheckMs;
        QUGATE_AdminApprovalState adminCheckApproval;
        uint8 adminApprovalUsed;
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
        // Heartbeat processing
        QUGATE_HeartbeatConfig inhCfg;
        sint64 inhBalance;
        sint64 inhPayoutTotal;
        sint64 inhShare;
        sint64 inhPriorSum;
        uint8  inhJ;
        uint8  inhK;
        uint8  inhBeneCount;
        uint32 inhEpochsInactive;
        // Heartbeat chain forwarding
        routeToGate_input rtIn;
        routeToGate_output rtOut;
        routeToGate_locals rtLocals;
        // Multisig processing
        QUGATE_MultisigConfig msigCfg;
        // TIME_LOCK processing
        QUGATE_TimeLockConfig tlCfg;
        sint64 tlReleaseAmount;
        // Deferred gate-recipient dispatch (chain hops in END_EPOCH)
        uint8 savedDeferredCount;
        Array<uint64, 8> savedDeferredSlots;
        Array<sint64, 8> savedDeferredAmounts;
        uint8 savedDeferredHopCount;
        uint8 deferredDispatchIdx;
    };

    struct configureTimeLock_locals
    {
        sint64 invReward;
        QuGateLogger logger;
        GateConfig gate;
        uint64 slotIdx;
        uint64 encodedGen;
        QUGATE_TimeLockConfig cfg;
        // Admin gate check
        uint64 adminCheckSlot;
        GateConfig adminCheckGate;
        QUGATE_MultisigConfig adminCheckMs;
        QUGATE_AdminApprovalState adminCheckApproval;
        uint8 adminApprovalUsed;
    };

    struct cancelTimeLock_locals
    {
        sint64 invReward;
        QuGateLogger logger;
        GateConfig gate;
        uint64 slotIdx;
        uint64 encodedGen;
        QUGATE_TimeLockConfig cfg;
        QUGATE_TimeLockConfig zeroCfg;
        // Admin gate check
        uint64 adminCheckSlot;
        GateConfig adminCheckGate;
        QUGATE_MultisigConfig adminCheckMs;
        QUGATE_AdminApprovalState adminCheckApproval;
        uint8 adminApprovalUsed;
    };

    struct getTimeLockState_locals
    {
        GateConfig gate;
        uint64 slotIdx;
        uint64 encodedGen;
        QUGATE_TimeLockConfig cfg;
    };

    struct setAdminGate_locals
    {
        sint64 invReward;
        QuGateLogger logger;
        GateConfig gate;
        GateConfig adminGate;
        uint64 slotIdx;
        uint64 encodedGen;
        uint64 adminSlot;
        uint64 adminApprovalSlot;
        uint64 adminEncodedGen;
        QUGATE_MultisigConfig msCfg;
        QUGATE_AdminApprovalState adminApproval;
        uint8 adminApproved;
        uint8 adminApprovalUsed;
        // Cycle detection
        uint64 walkSlot;
        GateConfig walkGate;
        uint8 walkStep;
    };

    struct getAdminGate_locals
    {
        GateConfig gate;
        GateConfig adminGate;
        uint64 slotIdx;
        uint64 encodedGen;
        uint64 adminSlot;
        QUGATE_MultisigConfig adminCfg;
        QUGATE_AdminApprovalState adminApproval;
        uint8 i;
    };

    struct withdrawReserve_locals
    {
        sint64 invReward;
        QuGateLogger logger;
        GateConfig gate;
        uint64 slotIdx;
        uint64 encodedGen;
        // Admin gate check
        uint64 adminCheckSlot;
        GateConfig adminCheckGate;
        QUGATE_MultisigConfig adminCheckMs;
        QUGATE_AdminApprovalState adminCheckApproval;
        uint8 adminApprovalUsed;
    };

    struct getGatesByMode_locals
    {
        uint64 i;
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
        // Gate-as-recipient deferred dispatch temps
        uint8 savedDeferredCount;
        uint8 savedDeferredHopCount;
        uint8 deferredDispatchIdx;
        Array<uint64, 8> savedDeferredSlots;
        Array<sint64, 8> savedDeferredAmounts;
    };

    struct BEGIN_EPOCH_locals
    {
        uint64 i;
        GateConfig gate;
        QuGateLogger logger;
        sint32 subId;
    };

    struct configureHeartbeat_locals
    {
        sint64 invReward;
        QuGateLogger logger;
        GateConfig gate;
        uint64 slotIdx;
        uint64 encodedGen;
        uint8  i;
        uint16 shareSum;
        QUGATE_HeartbeatConfig cfg;
        // Admin gate check
        uint64 adminCheckSlot;
        GateConfig adminCheckGate;
        QUGATE_MultisigConfig adminCheckMs;
        QUGATE_AdminApprovalState adminCheckApproval;
        uint8 adminApprovalUsed;
    };

    struct heartbeat_locals
    {
        sint64 invReward;
        QuGateLogger logger;
        GateConfig gate;
        uint64 slotIdx;
        uint64 encodedGen;
        QUGATE_HeartbeatConfig cfg;
    };

    struct getHeartbeat_locals
    {
        GateConfig gate;
        uint64 slotIdx;
        uint64 encodedGen;
        QUGATE_HeartbeatConfig cfg;
        uint8 i;
    };

    struct configureMultisig_locals
    {
        sint64 invReward;
        QuGateLogger logger;
        GateConfig gate;
        uint64 slotIdx;
        uint64 encodedGen;
        QUGATE_MultisigConfig cfg;
        uint8 i;
        uint8 j;
        QUGATE_AdminApprovalState adminApprovalZero;
        // Admin gate check
        uint64 adminCheckSlot;
        GateConfig adminCheckGate;
        QUGATE_MultisigConfig adminCheckMs;
        QUGATE_AdminApprovalState adminCheckApproval;
        uint8 adminApprovalUsed;
    };

    struct getMultisigState_locals
    {
        GateConfig gate;
        uint64 slotIdx;
        uint64 encodedGen;
        QUGATE_MultisigConfig cfg;
        uint8 i;
    };



    // =============================================
    // Procedures
    // =============================================

    PUBLIC_PROCEDURE_WITH_LOCALS(createGate)
    {
        locals.invReward = qpi.invocationReward();
        output.status = QUGATE_SUCCESS;
        output.gateId = 0;
        output.feePaid = 0;

        // Init logger
        locals.logger._contractIndex = CONTRACT_INDEX;
        locals.logger.sender = qpi.invocator();
        locals.logger.gateId = 0;
        locals.logger.amount = locals.invReward;

        // Calculate escalated fee: baseFee * (1 + QPI::div(activeGates, STEP))
        locals.currentFee = state.get()._creationFee * (1 + QPI::div(state.get()._activeGates, QUGATE_FEE_ESCALATION_STEP));

        // Validate creation fee (escalated)
        if (locals.invReward < (sint64)locals.currentFee)
        {
            if (locals.invReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invReward);
            }
            output.status = QUGATE_INSUFFICIENT_FEE;
            locals.logger._type = QUGATE_LOG_FAIL_INSUFFICIENT_FEE;
            LOG_INFO(locals.logger);
            return;
        }

        // Validate mode
        if (input.mode > QUGATE_MODE_TIME_LOCK)
        {
            // Refund all
            qpi.transfer(qpi.invocator(), locals.invReward);
            output.status = QUGATE_INVALID_MODE;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate recipient count
        if (input.recipientCount > QUGATE_MAX_RECIPIENTS)
        {
            qpi.transfer(qpi.invocator(), locals.invReward);
            output.status = QUGATE_INVALID_RECIPIENT_COUNT;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }
        // Modes 6 (HEARTBEAT), 7 (MULTISIG), 8 (TIME_LOCK) configure recipients
        // via separate procedures, so recipientCount == 0 is valid at creation time.
        if (input.recipientCount == 0 && input.chainNextGateId == -1
            && input.mode != QUGATE_MODE_HEARTBEAT
            && input.mode != QUGATE_MODE_MULTISIG
            && input.mode != QUGATE_MODE_TIME_LOCK)
        {
            qpi.transfer(qpi.invocator(), locals.invReward);
            output.status = QUGATE_INVALID_RECIPIENT_COUNT;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Check capacity — try free-list first
        if (state.get()._freeCount == 0 && state.get()._gateCount >= QUGATE_MAX_GATES)
        {
            qpi.transfer(qpi.invocator(), locals.invReward);
            output.status = QUGATE_NO_FREE_SLOTS;
            locals.logger._type = QUGATE_LOG_FAIL_NO_SLOTS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate SPLIT ratios
        if (input.mode == QUGATE_MODE_SPLIT && input.recipientCount > 0)
        {
            locals.totalRatio = 0;
            for (locals.i = 0; locals.i < input.recipientCount; locals.i++)
            {
                if (input.ratios.get(locals.i) > QUGATE_MAX_RATIO)
                {
                    qpi.transfer(qpi.invocator(), locals.invReward);
                    output.status = QUGATE_INVALID_RATIO;
                    locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                    LOG_WARNING(locals.logger);
                    return;
                }
                locals.totalRatio += input.ratios.get(locals.i);
            }
            if (locals.totalRatio == 0)
            {
                qpi.transfer(qpi.invocator(), locals.invReward);
                output.status = QUGATE_INVALID_RATIO;
                locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                LOG_WARNING(locals.logger);
                return;
            }
        }

        // Validate THRESHOLD > 0
        if (input.mode == QUGATE_MODE_THRESHOLD && input.threshold == 0)
        {
            qpi.transfer(qpi.invocator(), locals.invReward);
            output.status = QUGATE_INVALID_THRESHOLD;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate allowedSenderCount
        if (input.allowedSenderCount > QUGATE_MAX_RECIPIENTS)
        {
            qpi.transfer(qpi.invocator(), locals.invReward);
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
                qpi.transfer(qpi.invocator(), locals.invReward);
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

        // Admin gate fields
        locals.newGate.adminGateId = -1;
        locals.newGate.hasAdminGate = 0;

        // Gate-as-recipient: init all to -1 (wallet), then copy from input
        for (locals.i = 0; locals.i < QUGATE_MAX_RECIPIENTS; locals.i++)
        {
            locals.newGate.recipientGateIds.set(locals.i, -1);
        }

        for (locals.i = 0; locals.i < QUGATE_MAX_RECIPIENTS; locals.i++)
        {
            if (locals.i < input.recipientCount)
            {
                locals.newGate.recipients.set(locals.i, input.recipients.get(locals.i));
                locals.newGate.ratios.set(locals.i, input.ratios.get(locals.i));
                locals.newGate.recipientGateIds.set(locals.i, input.recipientGateIds.get(locals.i));
            }
            else
            {
                locals.newGate.recipients.set(locals.i, id::zero());
                locals.newGate.ratios.set(locals.i, 0);
                // recipientGateIds already set to -1 above
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

        // Validate gate-as-recipient IDs (must be -1 or valid active gate with correct generation)
        for (locals.i = 0; locals.i < input.recipientCount; locals.i++)
        {
            if (input.recipientGateIds.get(locals.i) >= 0)
            {
                uint64 rgSlot = (uint64)(input.recipientGateIds.get(locals.i)) & QUGATE_GATE_ID_SLOT_MASK;
                uint64 rgGen = (uint64)(input.recipientGateIds.get(locals.i)) >> QUGATE_GATE_ID_SLOT_BITS;
                if (rgSlot >= state.get()._gateCount
                    || rgGen == 0
                    || state.get()._gateGenerations.get(rgSlot) != (uint16)(rgGen - 1)
                    || state.get()._gates.get(rgSlot).active == 0)
                {
                    qpi.transfer(qpi.invocator(), locals.invReward);
                    output.status = QUGATE_INVALID_GATE_RECIPIENT;
                    locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                    LOG_WARNING(locals.logger);
                    return;
                }
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
                qpi.transfer(qpi.invocator(), locals.invReward);
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
                qpi.transfer(qpi.invocator(), locals.invReward);
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
                qpi.transfer(qpi.invocator(), locals.invReward);
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
                        qpi.transfer(qpi.invocator(), locals.invReward);
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
                    qpi.transfer(qpi.invocator(), locals.invReward);
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
        locals.adminApprovalZero.active = 0;
        locals.adminApprovalZero.validUntilEpoch = 0;
        state.mut()._adminApprovalStates.set(locals.slotIdx, locals.adminApprovalZero);
        locals.latestExecZero.valid = 0;
        locals.latestExecZero.mode = 0;
        locals.latestExecZero.outcomeType = QUGATE_EXEC_NONE;
        locals.latestExecZero.selectedRecipientIndex = 255;
        locals.latestExecZero.selectedDownstreamGateId = -1;
        locals.latestExecZero.forwardedAmount = 0;
        locals.latestExecZero.observedTick = 0;
        state.mut()._latestExecutions.set(locals.slotIdx, locals.latestExecZero);
        output.gateId = ((uint64)(state.get()._gateGenerations.get(locals.slotIdx) + 1) << QUGATE_GATE_ID_SLOT_BITS) | locals.slotIdx;
        state.mut()._activeGates += 1;

        // Burn the escalated creation fee
        qpi.burn(locals.currentFee);
        state.mut()._totalBurned += locals.currentFee;
        output.feePaid = locals.currentFee;

        // Handle excess: for ORACLE mode, excess goes to oracleReserve; otherwise refund
        if (locals.invReward > (sint64)locals.currentFee)
        {
            if (input.mode == QUGATE_MODE_ORACLE)
            {
                locals.newGate.oracleReserve = locals.invReward - (sint64)locals.currentFee;
                state.mut()._gates.set(locals.slotIdx, locals.newGate);
            }
            else
            {
                qpi.transfer(qpi.invocator(), locals.invReward - (sint64)locals.currentFee);
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

        if (locals.gate.recipientCount == 0)
        {
            locals.gate.totalForwarded += input.amount;
            state.mut()._gates.set(input.gateIdx, locals.gate);
            output.forwarded = input.amount;
            return;
        }

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
                if (locals.gate.recipientGateIds.get(locals.i) >= 0)
                {
                    // Gate-as-recipient: defer routing to caller
                    uint64 targetSlot = (uint64)(locals.gate.recipientGateIds.get(locals.i)) & QUGATE_GATE_ID_SLOT_MASK;
                    uint64 targetGen = (uint64)(locals.gate.recipientGateIds.get(locals.i)) >> QUGATE_GATE_ID_SLOT_BITS;
                    if (targetSlot < state.get()._gateCount && targetGen > 0
                        && state.get()._gateGenerations.get(targetSlot) == (uint16)(targetGen - 1)
                        && state.get()._gates.get(targetSlot).active == 1)
                    {
                        output.deferredGateSlots.set(output.deferredCount, targetSlot);
                        output.deferredGateAmounts.set(output.deferredCount, (sint64)locals.share);
                        output.deferredCount++;
                        locals.distributed += locals.share;
                    }
                    // else: target gate invalid/closed — share stays in contract (not lost)
                }
                else
                {
                    if (qpi.transfer(locals.gate.recipients.get(locals.i), locals.share) >= 0) // [QG-07]
                    {
                        locals.distributed += locals.share;
                    }
                }
            }
        }

        locals.gate.totalForwarded += locals.distributed;
        state.mut()._gates.set(input.gateIdx, locals.gate);
        output.forwarded = locals.distributed;
    }

    PRIVATE_PROCEDURE_WITH_LOCALS(processRoundRobin)
    {
        locals.gate = state.get()._gates.get(input.gateIdx);
        locals.latestExec.valid = 1;
        locals.latestExec.mode = locals.gate.mode;
        locals.latestExec.outcomeType = QUGATE_EXEC_NONE;
        locals.latestExec.selectedRecipientIndex = 255;
        locals.latestExec.selectedDownstreamGateId = -1;
        locals.latestExec.forwardedAmount = 0;
        locals.latestExec.observedTick = qpi.tick();

        if (locals.gate.recipientCount == 0)
        {
            locals.gate.totalForwarded += input.amount;
            state.mut()._gates.set(input.gateIdx, locals.gate);
            locals.latestExec.outcomeType = QUGATE_EXEC_FORWARDED;
            locals.latestExec.forwardedAmount = input.amount;
            state.mut()._latestExecutions.set(input.gateIdx, locals.latestExec);
            output.forwarded = input.amount;
            return;
        }

        if (locals.gate.recipientGateIds.get(locals.gate.roundRobinIndex) >= 0)
        {
            // Gate-as-recipient: defer routing to caller
            uint64 targetSlot = (uint64)(locals.gate.recipientGateIds.get(locals.gate.roundRobinIndex)) & QUGATE_GATE_ID_SLOT_MASK;
            uint64 targetGen = (uint64)(locals.gate.recipientGateIds.get(locals.gate.roundRobinIndex)) >> QUGATE_GATE_ID_SLOT_BITS;
            if (targetSlot < state.get()._gateCount && targetGen > 0
                && state.get()._gateGenerations.get(targetSlot) == (uint16)(targetGen - 1)
                && state.get()._gates.get(targetSlot).active == 1)
            {
                output.deferredToGate = 1;
                output.deferredGateSlot = targetSlot;
                output.deferredGateAmount = input.amount;
                locals.gate.totalForwarded += input.amount;
                locals.latestExec.outcomeType = QUGATE_EXEC_FORWARDED;
                locals.latestExec.selectedRecipientIndex = (uint8)locals.gate.roundRobinIndex;
                locals.latestExec.selectedDownstreamGateId = locals.gate.recipientGateIds.get(locals.gate.roundRobinIndex);
                locals.latestExec.forwardedAmount = input.amount;
                locals.gate.roundRobinIndex = QPI::mod(locals.gate.roundRobinIndex + 1, (uint64)locals.gate.recipientCount);
                output.forwarded = input.amount;
            }
        }
        else
        {
            if (qpi.transfer(locals.gate.recipients.get(locals.gate.roundRobinIndex), input.amount) >= 0) // [QG-08]
            {
                locals.gate.totalForwarded += input.amount;
                locals.latestExec.outcomeType = QUGATE_EXEC_FORWARDED;
                locals.latestExec.selectedRecipientIndex = (uint8)locals.gate.roundRobinIndex;
                locals.latestExec.selectedDownstreamGateId = -1;
                locals.latestExec.forwardedAmount = input.amount;
                locals.gate.roundRobinIndex = QPI::mod(locals.gate.roundRobinIndex + 1, (uint64)locals.gate.recipientCount);
                output.forwarded = input.amount;
            }
        }

        state.mut()._gates.set(input.gateIdx, locals.gate);
        state.mut()._latestExecutions.set(input.gateIdx, locals.latestExec);
    }

    PRIVATE_PROCEDURE_WITH_LOCALS(processThreshold)
    {
        locals.gate = state.get()._gates.get(input.gateIdx);

        locals.gate.currentBalance += input.amount;
        output.forwarded = 0;

        if (locals.gate.currentBalance >= locals.gate.threshold)
        {
            // [QG-09] Transfer-first: only update state if transfer succeeds (or chained)
            if (locals.gate.recipientCount > 0 && locals.gate.chainNextGateId == -1)
            {
                if (locals.gate.recipientGateIds.get(0) >= 0)
                {
                    // Gate-as-recipient: defer routing to caller
                    uint64 targetSlot = (uint64)(locals.gate.recipientGateIds.get(0)) & QUGATE_GATE_ID_SLOT_MASK;
                    uint64 targetGen = (uint64)(locals.gate.recipientGateIds.get(0)) >> QUGATE_GATE_ID_SLOT_BITS;
                    if (targetSlot < state.get()._gateCount && targetGen > 0
                        && state.get()._gateGenerations.get(targetSlot) == (uint16)(targetGen - 1)
                        && state.get()._gates.get(targetSlot).active == 1)
                    {
                        output.deferredToGate = 1;
                        output.deferredGateSlot = targetSlot;
                        output.deferredGateAmount = (sint64)locals.gate.currentBalance;
                        output.forwarded = locals.gate.currentBalance;
                        locals.gate.totalForwarded += locals.gate.currentBalance;
                        locals.gate.currentBalance = 0;
                    }
                }
                else
                {
                    if (qpi.transfer(locals.gate.recipients.get(0), locals.gate.currentBalance) >= 0)
                    {
                        output.forwarded = locals.gate.currentBalance;
                        locals.gate.totalForwarded += locals.gate.currentBalance;
                        locals.gate.currentBalance = 0;
                    }
                }
            }
            else
            {
                // Chained: no direct transfer, chain code handles forwarding
                output.forwarded = locals.gate.currentBalance;
                locals.gate.totalForwarded += locals.gate.currentBalance;
                locals.gate.currentBalance = 0;
            }
        }

        state.mut()._gates.set(input.gateIdx, locals.gate);
    }

    PRIVATE_PROCEDURE_WITH_LOCALS(processRandom)
    {
        locals.gate = state.get()._gates.get(input.gateIdx);
        locals.latestExec.valid = 1;
        locals.latestExec.mode = locals.gate.mode;
        locals.latestExec.outcomeType = QUGATE_EXEC_NONE;
        locals.latestExec.selectedRecipientIndex = 255;
        locals.latestExec.selectedDownstreamGateId = -1;
        locals.latestExec.forwardedAmount = 0;
        locals.latestExec.observedTick = qpi.tick();

        if (locals.gate.recipientCount == 0)
        {
            locals.gate.totalForwarded += input.amount;
            state.mut()._gates.set(input.gateIdx, locals.gate);
            locals.latestExec.outcomeType = QUGATE_EXEC_FORWARDED;
            locals.latestExec.forwardedAmount = input.amount;
            state.mut()._latestExecutions.set(input.gateIdx, locals.latestExec);
            output.forwarded = input.amount;
            return;
        }

        // NOTE: entropy is publicly observable — not cryptographically random
        locals.recipientIdx = QPI::mod(locals.gate.totalReceived + qpi.tick(), (uint64)locals.gate.recipientCount);
        if (locals.gate.recipientGateIds.get(locals.recipientIdx) >= 0)
        {
            // Gate-as-recipient: defer routing to caller
            uint64 targetSlot = (uint64)(locals.gate.recipientGateIds.get(locals.recipientIdx)) & QUGATE_GATE_ID_SLOT_MASK;
            uint64 targetGen = (uint64)(locals.gate.recipientGateIds.get(locals.recipientIdx)) >> QUGATE_GATE_ID_SLOT_BITS;
            if (targetSlot < state.get()._gateCount && targetGen > 0
                && state.get()._gateGenerations.get(targetSlot) == (uint16)(targetGen - 1)
                && state.get()._gates.get(targetSlot).active == 1)
            {
                output.deferredToGate = 1;
                output.deferredGateSlot = targetSlot;
                output.deferredGateAmount = input.amount;
                locals.gate.totalForwarded += input.amount;
                locals.latestExec.outcomeType = QUGATE_EXEC_FORWARDED;
                locals.latestExec.selectedRecipientIndex = (uint8)locals.recipientIdx;
                locals.latestExec.selectedDownstreamGateId = locals.gate.recipientGateIds.get(locals.recipientIdx);
                locals.latestExec.forwardedAmount = input.amount;
                output.forwarded = input.amount;
            }
        }
        else
        {
            if (qpi.transfer(locals.gate.recipients.get(locals.recipientIdx), input.amount) >= 0) // [QG-10]
            {
                locals.gate.totalForwarded += input.amount;
                locals.latestExec.outcomeType = QUGATE_EXEC_FORWARDED;
                locals.latestExec.selectedRecipientIndex = (uint8)locals.recipientIdx;
                locals.latestExec.selectedDownstreamGateId = -1;
                locals.latestExec.forwardedAmount = input.amount;
                output.forwarded = input.amount;
            }
        }

        state.mut()._gates.set(input.gateIdx, locals.gate);
        state.mut()._latestExecutions.set(input.gateIdx, locals.latestExec);
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
            if (locals.gate.recipientCount == 0)
            {
                // No recipients — chain forwarding will handle delivery
                locals.gate.totalForwarded += input.amount;
                output.forwarded = input.amount;
            }
            else
            {
                if (locals.gate.recipientGateIds.get(0) >= 0)
                {
                    // Gate-as-recipient: defer routing to caller
                    uint64 targetSlot = (uint64)(locals.gate.recipientGateIds.get(0)) & QUGATE_GATE_ID_SLOT_MASK;
                    uint64 targetGen = (uint64)(locals.gate.recipientGateIds.get(0)) >> QUGATE_GATE_ID_SLOT_BITS;
                    if (targetSlot < state.get()._gateCount && targetGen > 0
                        && state.get()._gateGenerations.get(targetSlot) == (uint16)(targetGen - 1)
                        && state.get()._gates.get(targetSlot).active == 1)
                    {
                        output.deferredToGate = 1;
                        output.deferredGateSlot = targetSlot;
                        output.deferredGateAmount = input.amount;
                        locals.gate.totalForwarded += input.amount;
                        output.forwarded = input.amount;
                    }
                }
                else
                {
                    if (qpi.transfer(locals.gate.recipients.get(0), input.amount) >= 0) // [QG-11]
                    {
                        locals.gate.totalForwarded += input.amount;
                        output.forwarded = input.amount;
                    }
                }
            }
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
    // processMultisigVote — shared guardian voting logic for MULTISIG gates.
    // Called from sendToGate and sendToGateVerified after funds are accumulated.
    PRIVATE_PROCEDURE_WITH_LOCALS(processMultisigVote)
    {
        output.status = QUGATE_SUCCESS;

        // Check if sender is a guardian
        locals.msigCfg = state.get()._multisigConfigs.get(input.slotIdx);
        locals.foundGuardian = 0;
        locals.guardianIdx = 0;
        for (uint8 _gi = 0; _gi < locals.msigCfg.guardianCount; _gi++)
        {
            if (locals.foundGuardian == 0 && locals.msigCfg.guardians.get(_gi) == qpi.invocator())
            {
                locals.foundGuardian = 1;
                locals.guardianIdx = _gi;
            }
        }

        if (locals.foundGuardian == 1)
        {
            // Check proposal expiry
            if (locals.msigCfg.proposalActive == 1
                && (uint32)qpi.epoch() - locals.msigCfg.proposalEpoch > locals.msigCfg.proposalExpiryEpochs)
            {
                // Proposal expired — reset
                locals.msigCfg.approvalBitmap = 0;
                locals.msigCfg.approvalCount = 0;
                locals.msigCfg.proposalActive = 0;
                state.mut()._multisigConfigs.set(input.slotIdx, locals.msigCfg);
                locals.logger._contractIndex = CONTRACT_INDEX;
                locals.logger._type = QUGATE_LOG_MULTISIG_EXPIRED;
                locals.logger.gateId = input.gateId;
                locals.logger.sender = qpi.invocator();
                locals.logger.amount = 0;
                LOG_INFO(locals.logger);
            }

            // Check if already voted
            if ((locals.msigCfg.approvalBitmap & (1 << locals.guardianIdx)) != 0)
            {
                output.status = QUGATE_MULTISIG_ALREADY_VOTED;
                return;
            }

            // Record vote
            locals.msigCfg.approvalBitmap = locals.msigCfg.approvalBitmap | (uint8)(1 << locals.guardianIdx);
            locals.msigCfg.approvalCount++;
            if (locals.msigCfg.proposalActive == 0)
            {
                locals.msigCfg.proposalActive = 1;
                locals.msigCfg.proposalEpoch = (uint32)qpi.epoch();
            }
            state.mut()._multisigConfigs.set(input.slotIdx, locals.msigCfg);

            locals.logger._contractIndex = CONTRACT_INDEX;
            locals.logger._type = QUGATE_LOG_MULTISIG_VOTE;
            locals.logger.gateId = input.gateId;
            locals.logger.sender = qpi.invocator();
            locals.logger.amount = input.amount;
            LOG_INFO(locals.logger);

            // Check threshold
            locals.gate = state.get()._gates.get(input.slotIdx);
            sint64 releaseAmount = input.amount;
            if (locals.gate.recipientCount == 0
                && locals.gate.chainNextGateId == -1
                && locals.msigCfg.adminApprovalWindowEpochs > 0
                && input.amount > 0
                && locals.gate.currentBalance >= (uint64)input.amount)
            {
                // Admin-only multisig votes are burned immediately instead of accumulating.
                locals.gate.currentBalance -= (uint64)input.amount;
                state.mut()._gates.set(input.slotIdx, locals.gate);
            }

            if (locals.msigCfg.approvalCount >= locals.msigCfg.required)
            {
                if (locals.gate.recipientCount == 0
                    && locals.gate.chainNextGateId == -1
                    && locals.msigCfg.adminApprovalWindowEpochs > 0)
                {
                    locals.adminApproval.active = 1;
                    locals.adminApproval.validUntilEpoch = (uint32)qpi.epoch() + locals.msigCfg.adminApprovalWindowEpochs - 1;
                    state.mut()._adminApprovalStates.set(input.slotIdx, locals.adminApproval);

                    // Reset proposal after explicit admin approval is granted.
                    locals.msigCfg.approvalBitmap = 0;
                    locals.msigCfg.approvalCount = 0;
                    locals.msigCfg.proposalActive = 0;
                    state.mut()._multisigConfigs.set(input.slotIdx, locals.msigCfg);
                }
                else if (locals.gate.currentBalance > 0)
                {
                    // Transfer balance to target (recipients.get(0) is the target address)
                    releaseAmount = (sint64)locals.gate.currentBalance;
                    uint8 transferred = 0;
                    if (locals.gate.recipientCount > 0)
                    {
                        if (locals.gate.recipientGateIds.get(0) >= 0)
                        {
                            // Gate-as-recipient: defer routing to caller
                            uint64 targetSlot = (uint64)(locals.gate.recipientGateIds.get(0)) & QUGATE_GATE_ID_SLOT_MASK;
                            uint64 targetGen = (uint64)(locals.gate.recipientGateIds.get(0)) >> QUGATE_GATE_ID_SLOT_BITS;
                            if (targetSlot < state.get()._gateCount && targetGen > 0
                                && state.get()._gateGenerations.get(targetSlot) == (uint16)(targetGen - 1)
                                && state.get()._gates.get(targetSlot).active == 1)
                            {
                                output.deferredToGate = 1;
                                output.deferredGateSlot = targetSlot;
                                output.deferredGateAmount = releaseAmount;
                                transferred = 1;
                            }
                        }
                        else
                        {
                            if (qpi.transfer(locals.gate.recipients.get(0), releaseAmount) >= 0) // [QG-12]
                            {
                                transferred = 1;
                            }
                        }
                    }
                    else if (locals.gate.chainNextGateId != -1)
                    {
                        transferred = 1; // No recipient, chain will handle
                        output.chainForward = 1;
                        output.chainForwardAmount = releaseAmount;
                    }
                    // else: no recipients AND no chain — funds stay in currentBalance
                    if (transferred)
                    {
                        locals.gate.totalForwarded += (uint64)releaseAmount;
                        locals.gate.currentBalance = 0;
                        state.mut()._gates.set(input.slotIdx, locals.gate);

                        // Reset proposal
                        locals.msigCfg.approvalBitmap = 0;
                        locals.msigCfg.approvalCount = 0;
                        locals.msigCfg.proposalActive = 0;
                        state.mut()._multisigConfigs.set(input.slotIdx, locals.msigCfg);
                    }
                }

                locals.logger._contractIndex = CONTRACT_INDEX;
                locals.logger._type = QUGATE_LOG_MULTISIG_EXECUTED;
                locals.logger.gateId = input.gateId;
                locals.logger.sender = qpi.invocator();
                locals.logger.amount = releaseAmount;
                LOG_INFO(locals.logger);
            }
        }
    }

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
            // Bubble deferred gate-recipients to caller (no self-recursion)
            output.deferredCount = locals.splitOut.deferredCount;
            output.deferredHopCount = input.hopCount + 1;
            for (uint8 _dg = 0; _dg < locals.splitOut.deferredCount; _dg++)
            {
                output.deferredGateSlots.set(_dg, locals.splitOut.deferredGateSlots.get(_dg));
                output.deferredGateAmounts.set(_dg, locals.splitOut.deferredGateAmounts.get(_dg));
            }
        }
        else if (locals.gate.mode == QUGATE_MODE_ROUND_ROBIN)
        {
            locals.rrIn.gateIdx = input.slotIdx;
            locals.rrIn.amount = locals.amountAfterFee;
            processRoundRobin(qpi, state, locals.rrIn, locals.rrOut, locals.rrLocals);
            output.forwarded = locals.rrOut.forwarded;
            if (locals.rrOut.deferredToGate == 1)
            {
                output.deferredCount = 1;
                output.deferredHopCount = input.hopCount + 1;
                output.deferredGateSlots.set(0, locals.rrOut.deferredGateSlot);
                output.deferredGateAmounts.set(0, locals.rrOut.deferredGateAmount);
            }
        }
        else if (locals.gate.mode == QUGATE_MODE_THRESHOLD)
        {
            locals.threshIn.gateIdx = input.slotIdx;
            locals.threshIn.amount = locals.amountAfterFee;
            processThreshold(qpi, state, locals.threshIn, locals.threshOut, locals.threshLocals);
            output.forwarded = locals.threshOut.forwarded;
            if (locals.threshOut.deferredToGate == 1)
            {
                output.deferredCount = 1;
                output.deferredHopCount = input.hopCount + 1;
                output.deferredGateSlots.set(0, locals.threshOut.deferredGateSlot);
                output.deferredGateAmounts.set(0, locals.threshOut.deferredGateAmount);
            }
        }
        else if (locals.gate.mode == QUGATE_MODE_RANDOM)
        {
            locals.randIn.gateIdx = input.slotIdx;
            locals.randIn.amount = locals.amountAfterFee;
            processRandom(qpi, state, locals.randIn, locals.randOut, locals.randLocals);
            output.forwarded = locals.randOut.forwarded;
            if (locals.randOut.deferredToGate == 1)
            {
                output.deferredCount = 1;
                output.deferredHopCount = input.hopCount + 1;
                output.deferredGateSlots.set(0, locals.randOut.deferredGateSlot);
                output.deferredGateAmounts.set(0, locals.randOut.deferredGateAmount);
            }
        }
        else if (locals.gate.mode == QUGATE_MODE_ORACLE)
        {
            // ORACLE mode in chain: accumulate into currentBalance
            locals.gate = state.get()._gates.get(input.slotIdx);
            locals.gate.currentBalance += locals.amountAfterFee;
            state.mut()._gates.set(input.slotIdx, locals.gate);
            output.forwarded = locals.amountAfterFee;
        }
        else if (locals.gate.mode == QUGATE_MODE_HEARTBEAT
                 || locals.gate.mode == QUGATE_MODE_MULTISIG
                 || locals.gate.mode == QUGATE_MODE_TIME_LOCK)
        {
            // Accumulate-on-arrival modes: funds held in currentBalance until mode-specific release
            // HEARTBEAT: released by epoch trigger after inactivity threshold
            // MULTISIG: released when guardians reach quorum
            // TIME_LOCK: released at unlockEpoch
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
        locals.invReward = qpi.invocationReward();
        output.status = QUGATE_SUCCESS;

        locals.logger._contractIndex = CONTRACT_INDEX;
        locals.logger.sender = qpi.invocator();
        locals.logger.amount = locals.invReward;
        locals.logger.gateId = input.gateId;

        // Decode versioned gateId: lower 20 bits = slotIndex, upper bits = generation
        locals.slotIdx = input.gateId & QUGATE_GATE_ID_SLOT_MASK;
        locals.encodedGen = (input.gateId >> QUGATE_GATE_ID_SLOT_BITS);
        if (input.gateId == 0
            || locals.slotIdx >= state.get()._gateCount
            || locals.encodedGen == 0
            || state.get()._gateGenerations.get(locals.slotIdx) != (uint16)(locals.encodedGen - 1))
        {
            if (locals.invReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invReward);
            }
            output.status = QUGATE_INVALID_GATE_ID;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_GATE;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.gate = state.get()._gates.get(locals.slotIdx);

        if (locals.gate.active == 0)
        {
            if (locals.invReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invReward);
            }
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Lazy expiry check: expire gate if inactive too long
        if (state.get()._expiryEpochs > 0
            && qpi.epoch() - locals.gate.lastActivityEpoch >= state.get()._expiryEpochs
            && locals.gate.active == 1)
        {
            if (locals.gate.currentBalance > 0)
            {
                if (qpi.transfer(locals.gate.owner, locals.gate.currentBalance) >= 0) // [QG-01]
                {
                    locals.gate.currentBalance = 0;
                }
            }
            if (locals.gate.mode == QUGATE_MODE_ORACLE && locals.gate.oracleReserve > 0)
            {
                if (qpi.transfer(locals.gate.owner, locals.gate.oracleReserve) >= 0) // [QG-01]
                {
                    locals.gate.oracleReserve = 0;
                }
            }
            if (locals.gate.chainReserve > 0)
            {
                if (qpi.transfer(locals.gate.owner, locals.gate.chainReserve) >= 0) // [QG-01]
                {
                    locals.gate.chainReserve = 0;
                }
            }
            locals.gate.active = 0;
            state.mut()._gates.set(locals.slotIdx, locals.gate);
            state.mut()._activeGates -= 1;
            state.mut()._freeSlots.set(state.get()._freeCount, locals.slotIdx);
            state.mut()._freeCount += 1;
            state.mut()._gateGenerations.set(locals.slotIdx, state.get()._gateGenerations.get(locals.slotIdx) + 1);
            if (locals.invReward > 0) qpi.transfer(qpi.invocator(), locals.invReward);
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_GATE_EXPIRED;
            LOG_INFO(locals.logger);
            return;
        }

        locals.amount = locals.invReward;
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
            // Route deferred gate-recipients
            for (uint8 _dg = 0; _dg < locals.splitOut.deferredCount; _dg++)
            {
                locals.chainIn.slotIdx = locals.splitOut.deferredGateSlots.get(_dg);
                locals.chainIn.amount = locals.splitOut.deferredGateAmounts.get(_dg);
                locals.chainIn.hopCount = 0;
                routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                // Dispatch deferred gate-as-recipient routing from chain hop
                locals.savedDeferredCount = locals.chainOut.deferredCount;
                locals.savedDeferredHopCount = locals.chainOut.deferredHopCount;
                for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                {
                    locals.savedDeferredSlots.set(locals.deferredDispatchIdx, locals.chainOut.deferredGateSlots.get(locals.deferredDispatchIdx));
                    locals.savedDeferredAmounts.set(locals.deferredDispatchIdx, locals.chainOut.deferredGateAmounts.get(locals.deferredDispatchIdx));
                }
                for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                {
                    locals.chainIn.slotIdx = locals.savedDeferredSlots.get(locals.deferredDispatchIdx);
                    locals.chainIn.amount = locals.savedDeferredAmounts.get(locals.deferredDispatchIdx);
                    locals.chainIn.hopCount = locals.savedDeferredHopCount;
                    routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                }
            }
        }
        else if (locals.gate.mode == QUGATE_MODE_ROUND_ROBIN)
        {
            locals.rrIn.gateIdx = locals.slotIdx;
            locals.rrIn.amount = locals.amount;
            processRoundRobin(qpi, state, locals.rrIn, locals.rrOut, locals.rrLocals);
            locals.logger._type = QUGATE_LOG_PAYMENT_FORWARDED;
            LOG_INFO(locals.logger);
            if (locals.rrOut.deferredToGate == 1)
            {
                locals.chainIn.slotIdx = locals.rrOut.deferredGateSlot;
                locals.chainIn.amount = locals.rrOut.deferredGateAmount;
                locals.chainIn.hopCount = 0;
                routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                // Dispatch deferred gate-as-recipient routing from chain hop
                locals.savedDeferredCount = locals.chainOut.deferredCount;
                locals.savedDeferredHopCount = locals.chainOut.deferredHopCount;
                for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                {
                    locals.savedDeferredSlots.set(locals.deferredDispatchIdx, locals.chainOut.deferredGateSlots.get(locals.deferredDispatchIdx));
                    locals.savedDeferredAmounts.set(locals.deferredDispatchIdx, locals.chainOut.deferredGateAmounts.get(locals.deferredDispatchIdx));
                }
                for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                {
                    locals.chainIn.slotIdx = locals.savedDeferredSlots.get(locals.deferredDispatchIdx);
                    locals.chainIn.amount = locals.savedDeferredAmounts.get(locals.deferredDispatchIdx);
                    locals.chainIn.hopCount = locals.savedDeferredHopCount;
                    routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                }
            }
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
            if (locals.threshOut.deferredToGate == 1)
            {
                locals.chainIn.slotIdx = locals.threshOut.deferredGateSlot;
                locals.chainIn.amount = locals.threshOut.deferredGateAmount;
                locals.chainIn.hopCount = 0;
                routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                // Dispatch deferred gate-as-recipient routing from chain hop
                locals.savedDeferredCount = locals.chainOut.deferredCount;
                locals.savedDeferredHopCount = locals.chainOut.deferredHopCount;
                for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                {
                    locals.savedDeferredSlots.set(locals.deferredDispatchIdx, locals.chainOut.deferredGateSlots.get(locals.deferredDispatchIdx));
                    locals.savedDeferredAmounts.set(locals.deferredDispatchIdx, locals.chainOut.deferredGateAmounts.get(locals.deferredDispatchIdx));
                }
                for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                {
                    locals.chainIn.slotIdx = locals.savedDeferredSlots.get(locals.deferredDispatchIdx);
                    locals.chainIn.amount = locals.savedDeferredAmounts.get(locals.deferredDispatchIdx);
                    locals.chainIn.hopCount = locals.savedDeferredHopCount;
                    routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                }
            }
        }
        else if (locals.gate.mode == QUGATE_MODE_RANDOM)
        {
            locals.randIn.gateIdx = locals.slotIdx;
            locals.randIn.amount = locals.amount;
            processRandom(qpi, state, locals.randIn, locals.randOut, locals.randLocals);
            locals.logger._type = QUGATE_LOG_PAYMENT_FORWARDED;
            LOG_INFO(locals.logger);
            if (locals.randOut.deferredToGate == 1)
            {
                locals.chainIn.slotIdx = locals.randOut.deferredGateSlot;
                locals.chainIn.amount = locals.randOut.deferredGateAmount;
                locals.chainIn.hopCount = 0;
                routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                // Dispatch deferred gate-as-recipient routing from chain hop
                locals.savedDeferredCount = locals.chainOut.deferredCount;
                locals.savedDeferredHopCount = locals.chainOut.deferredHopCount;
                for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                {
                    locals.savedDeferredSlots.set(locals.deferredDispatchIdx, locals.chainOut.deferredGateSlots.get(locals.deferredDispatchIdx));
                    locals.savedDeferredAmounts.set(locals.deferredDispatchIdx, locals.chainOut.deferredGateAmounts.get(locals.deferredDispatchIdx));
                }
                for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                {
                    locals.chainIn.slotIdx = locals.savedDeferredSlots.get(locals.deferredDispatchIdx);
                    locals.chainIn.amount = locals.savedDeferredAmounts.get(locals.deferredDispatchIdx);
                    locals.chainIn.hopCount = locals.savedDeferredHopCount;
                    routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                }
            }
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
                if (locals.condOut.deferredToGate == 1)
                {
                    locals.chainIn.slotIdx = locals.condOut.deferredGateSlot;
                    locals.chainIn.amount = locals.condOut.deferredGateAmount;
                    locals.chainIn.hopCount = 0;
                    routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                }
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
        else if (locals.gate.mode == QUGATE_MODE_HEARTBEAT)
        {
            // HEARTBEAT mode: accumulate into currentBalance, END_EPOCH distributes after trigger
            locals.gate = state.get()._gates.get(locals.slotIdx);
            locals.gate.currentBalance += locals.amount;
            state.mut()._gates.set(locals.slotIdx, locals.gate);
            locals.logger._type = QUGATE_LOG_PAYMENT_FORWARDED;
            LOG_INFO(locals.logger);
        }
        else if (locals.gate.mode == QUGATE_MODE_MULTISIG)
        {
            // MULTISIG mode: always accumulate funds; guardian senders also cast a vote
            locals.gate = state.get()._gates.get(locals.slotIdx);
            locals.gate.currentBalance += locals.amount;
            state.mut()._gates.set(locals.slotIdx, locals.gate);
            locals.logger._type = QUGATE_LOG_PAYMENT_FORWARDED;
            LOG_INFO(locals.logger);

            locals.msigIn.slotIdx = locals.slotIdx;
            locals.msigIn.gateId = input.gateId;
            locals.msigIn.amount = locals.amount;
            processMultisigVote(qpi, state, locals.msigIn, locals.msigOut, locals.msigLocals);
            if (locals.msigOut.status != QUGATE_SUCCESS)
            {
                output.status = locals.msigOut.status;
                return;
            }
            if (locals.msigOut.deferredToGate == 1)
            {
                locals.chainIn.slotIdx = locals.msigOut.deferredGateSlot;
                locals.chainIn.amount = locals.msigOut.deferredGateAmount;
                locals.chainIn.hopCount = 0;
                routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                // Dispatch deferred gate-as-recipient routing from chain hop
                locals.savedDeferredCount = locals.chainOut.deferredCount;
                locals.savedDeferredHopCount = locals.chainOut.deferredHopCount;
                for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                {
                    locals.savedDeferredSlots.set(locals.deferredDispatchIdx, locals.chainOut.deferredGateSlots.get(locals.deferredDispatchIdx));
                    locals.savedDeferredAmounts.set(locals.deferredDispatchIdx, locals.chainOut.deferredGateAmounts.get(locals.deferredDispatchIdx));
                }
                for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                {
                    locals.chainIn.slotIdx = locals.savedDeferredSlots.get(locals.deferredDispatchIdx);
                    locals.chainIn.amount = locals.savedDeferredAmounts.get(locals.deferredDispatchIdx);
                    locals.chainIn.hopCount = locals.savedDeferredHopCount;
                    routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                }
            }
            // Chain forwarding on MULTISIG release (recipientCount==0 with chain target)
            if (locals.msigOut.chainForward == 1)
            {
                locals.gate = state.get()._gates.get(locals.slotIdx);
                sint64 currentChainGateId = locals.gate.chainNextGateId;
                if (currentChainGateId != -1)
                {
                    uint64 nextSlot = (uint64)currentChainGateId & QUGATE_GATE_ID_SLOT_MASK;
                    uint64 nextGen = (uint64)currentChainGateId >> QUGATE_GATE_ID_SLOT_BITS;
                    if (nextSlot < state.get()._gateCount && nextGen > 0
                        && state.get()._gateGenerations.get(nextSlot) == (uint16)(nextGen - 1))
                    {
                        locals.chainIn.slotIdx = nextSlot;
                        locals.chainIn.amount = locals.msigOut.chainForwardAmount;
                        locals.chainIn.hopCount = 0;
                        routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                    }
                }
            }
        }

        else if (locals.gate.mode == QUGATE_MODE_TIME_LOCK)
        {
            // TIME_LOCK mode: only accepts funds when configured; relative mode anchors unlock on first funding
            locals.gate = state.get()._gates.get(locals.slotIdx);
            locals.tlCfg = state.get()._timeLockConfigs.get(locals.slotIdx);
            if (locals.tlCfg.active == 0 || locals.tlCfg.cancelled == 1)
            {
                qpi.transfer(qpi.invocator(), locals.amount);
            }
            else if (locals.tlCfg.fired == 1)
            {
                // Gate has already fired — refund sender
                qpi.transfer(qpi.invocator(), locals.amount);
            }
            else
            {
                if (locals.tlCfg.lockMode == QUGATE_TIME_LOCK_RELATIVE_EPOCHS && locals.tlCfg.unlockEpoch == 0)
                {
                    locals.tlCfg.unlockEpoch = (uint32)qpi.epoch() + locals.tlCfg.delayEpochs;
                    state.mut()._timeLockConfigs.set(locals.slotIdx, locals.tlCfg);
                }
                locals.gate.currentBalance += locals.amount;
                state.mut()._gates.set(locals.slotIdx, locals.gate);
                locals.logger._type = QUGATE_LOG_PAYMENT_FORWARDED;
                LOG_INFO(locals.logger);
            }
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

            // Accumulate-on-arrival modes: chainAmount is 0 at send time
            // (funds accumulate, chain forwarding happens at release time via
            // processMultisigVote / END_EPOCH heartbeat / END_EPOCH time-lock)

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
                    // Dispatch deferred gate-as-recipient routing from chain hop
                    locals.savedDeferredCount = locals.chainOut.deferredCount;
                    locals.savedDeferredHopCount = locals.chainOut.deferredHopCount;
                    for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                    {
                        locals.savedDeferredSlots.set(locals.deferredDispatchIdx, locals.chainOut.deferredGateSlots.get(locals.deferredDispatchIdx));
                        locals.savedDeferredAmounts.set(locals.deferredDispatchIdx, locals.chainOut.deferredGateAmounts.get(locals.deferredDispatchIdx));
                    }
                    for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                    {
                        locals.chainIn.slotIdx = locals.savedDeferredSlots.get(locals.deferredDispatchIdx);
                        locals.chainIn.amount = locals.savedDeferredAmounts.get(locals.deferredDispatchIdx);
                        locals.chainIn.hopCount = locals.savedDeferredHopCount;
                        routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                    }
                    locals.nextChainGate = state.get()._gates.get(nextSlot);
                    currentChainGateId = locals.nextChainGate.chainNextGateId;
                    hop += 1;
                }
                // Failsafe: if chain forwarding couldn't deliver, revert to currentBalance
                if (chainAmount > 0)
                {
                    locals.gate = state.get()._gates.get(locals.slotIdx);
                    locals.gate.currentBalance += (uint64)chainAmount;
                    locals.gate.totalForwarded -= (uint64)chainAmount; // undo the premature forward count
                    state.mut()._gates.set(locals.slotIdx, locals.gate);
                    locals.logger._type = QUGATE_LOG_CHAIN_HOP_INSUFFICIENT;
                    locals.logger.gateId = input.gateId;
                    locals.logger.amount = chainAmount;
                    LOG_INFO(locals.logger);
                }
            }
        }
    }

    PUBLIC_PROCEDURE_WITH_LOCALS(sendToGateVerified)
    {
        locals.invReward = qpi.invocationReward();
        output.status = QUGATE_SUCCESS;

        locals.logger._contractIndex = CONTRACT_INDEX;
        locals.logger.sender = qpi.invocator();
        locals.logger.amount = locals.invReward;
        locals.logger.gateId = input.gateId;

        // Decode versioned gateId: lower 20 bits = slotIndex, upper bits = generation
        locals.slotIdx = input.gateId & QUGATE_GATE_ID_SLOT_MASK;
        locals.encodedGen = (input.gateId >> QUGATE_GATE_ID_SLOT_BITS);
        if (input.gateId == 0
            || locals.slotIdx >= state.get()._gateCount
            || locals.encodedGen == 0
            || state.get()._gateGenerations.get(locals.slotIdx) != (uint16)(locals.encodedGen - 1))
        {
            if (locals.invReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invReward);
            }
            output.status = QUGATE_INVALID_GATE_ID;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_GATE;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.gate = state.get()._gates.get(locals.slotIdx);

        if (locals.gate.active == 0)
        {
            if (locals.invReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invReward);
            }
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Lazy expiry check: expire gate if inactive too long
        if (state.get()._expiryEpochs > 0
            && qpi.epoch() - locals.gate.lastActivityEpoch >= state.get()._expiryEpochs
            && locals.gate.active == 1)
        {
            if (locals.gate.currentBalance > 0)
            {
                if (qpi.transfer(locals.gate.owner, locals.gate.currentBalance) >= 0) // [QG-02]
                {
                    locals.gate.currentBalance = 0;
                }
            }
            if (locals.gate.mode == QUGATE_MODE_ORACLE && locals.gate.oracleReserve > 0)
            {
                if (qpi.transfer(locals.gate.owner, locals.gate.oracleReserve) >= 0) // [QG-02]
                {
                    locals.gate.oracleReserve = 0;
                }
            }
            if (locals.gate.chainReserve > 0)
            {
                if (qpi.transfer(locals.gate.owner, locals.gate.chainReserve) >= 0) // [QG-02]
                {
                    locals.gate.chainReserve = 0;
                }
            }
            locals.gate.active = 0;
            state.mut()._gates.set(locals.slotIdx, locals.gate);
            state.mut()._activeGates -= 1;
            state.mut()._freeSlots.set(state.get()._freeCount, locals.slotIdx);
            state.mut()._freeCount += 1;
            state.mut()._gateGenerations.set(locals.slotIdx, state.get()._gateGenerations.get(locals.slotIdx) + 1);
            if (locals.invReward > 0) qpi.transfer(qpi.invocator(), locals.invReward);
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_GATE_EXPIRED;
            LOG_INFO(locals.logger);
            return;
        }

        // Owner verification — refund and reject if mismatch
        if (locals.gate.owner != input.expectedOwner)
        {
            if (locals.invReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invReward);
            }
            output.status = QUGATE_OWNER_MISMATCH;
            locals.logger._type = QUGATE_LOG_FAIL_OWNER_MISMATCH;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.amount = locals.invReward;
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
            // Route deferred gate-recipients
            for (uint8 _dg = 0; _dg < locals.splitOut.deferredCount; _dg++)
            {
                locals.chainIn.slotIdx = locals.splitOut.deferredGateSlots.get(_dg);
                locals.chainIn.amount = locals.splitOut.deferredGateAmounts.get(_dg);
                locals.chainIn.hopCount = 0;
                routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                // Dispatch deferred gate-as-recipient routing from chain hop
                locals.savedDeferredCount = locals.chainOut.deferredCount;
                locals.savedDeferredHopCount = locals.chainOut.deferredHopCount;
                for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                {
                    locals.savedDeferredSlots.set(locals.deferredDispatchIdx, locals.chainOut.deferredGateSlots.get(locals.deferredDispatchIdx));
                    locals.savedDeferredAmounts.set(locals.deferredDispatchIdx, locals.chainOut.deferredGateAmounts.get(locals.deferredDispatchIdx));
                }
                for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                {
                    locals.chainIn.slotIdx = locals.savedDeferredSlots.get(locals.deferredDispatchIdx);
                    locals.chainIn.amount = locals.savedDeferredAmounts.get(locals.deferredDispatchIdx);
                    locals.chainIn.hopCount = locals.savedDeferredHopCount;
                    routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                }
            }
        }
        else if (locals.gate.mode == QUGATE_MODE_ROUND_ROBIN)
        {
            locals.rrIn.gateIdx = locals.slotIdx;
            locals.rrIn.amount = locals.amount;
            processRoundRobin(qpi, state, locals.rrIn, locals.rrOut, locals.rrLocals);
            locals.logger._type = QUGATE_LOG_PAYMENT_FORWARDED;
            LOG_INFO(locals.logger);
            if (locals.rrOut.deferredToGate == 1)
            {
                locals.chainIn.slotIdx = locals.rrOut.deferredGateSlot;
                locals.chainIn.amount = locals.rrOut.deferredGateAmount;
                locals.chainIn.hopCount = 0;
                routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                // Dispatch deferred gate-as-recipient routing from chain hop
                locals.savedDeferredCount = locals.chainOut.deferredCount;
                locals.savedDeferredHopCount = locals.chainOut.deferredHopCount;
                for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                {
                    locals.savedDeferredSlots.set(locals.deferredDispatchIdx, locals.chainOut.deferredGateSlots.get(locals.deferredDispatchIdx));
                    locals.savedDeferredAmounts.set(locals.deferredDispatchIdx, locals.chainOut.deferredGateAmounts.get(locals.deferredDispatchIdx));
                }
                for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                {
                    locals.chainIn.slotIdx = locals.savedDeferredSlots.get(locals.deferredDispatchIdx);
                    locals.chainIn.amount = locals.savedDeferredAmounts.get(locals.deferredDispatchIdx);
                    locals.chainIn.hopCount = locals.savedDeferredHopCount;
                    routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                }
            }
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
            if (locals.threshOut.deferredToGate == 1)
            {
                locals.chainIn.slotIdx = locals.threshOut.deferredGateSlot;
                locals.chainIn.amount = locals.threshOut.deferredGateAmount;
                locals.chainIn.hopCount = 0;
                routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                // Dispatch deferred gate-as-recipient routing from chain hop
                locals.savedDeferredCount = locals.chainOut.deferredCount;
                locals.savedDeferredHopCount = locals.chainOut.deferredHopCount;
                for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                {
                    locals.savedDeferredSlots.set(locals.deferredDispatchIdx, locals.chainOut.deferredGateSlots.get(locals.deferredDispatchIdx));
                    locals.savedDeferredAmounts.set(locals.deferredDispatchIdx, locals.chainOut.deferredGateAmounts.get(locals.deferredDispatchIdx));
                }
                for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                {
                    locals.chainIn.slotIdx = locals.savedDeferredSlots.get(locals.deferredDispatchIdx);
                    locals.chainIn.amount = locals.savedDeferredAmounts.get(locals.deferredDispatchIdx);
                    locals.chainIn.hopCount = locals.savedDeferredHopCount;
                    routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                }
            }
        }
        else if (locals.gate.mode == QUGATE_MODE_RANDOM)
        {
            locals.randIn.gateIdx = locals.slotIdx;
            locals.randIn.amount = locals.amount;
            processRandom(qpi, state, locals.randIn, locals.randOut, locals.randLocals);
            locals.logger._type = QUGATE_LOG_PAYMENT_FORWARDED;
            LOG_INFO(locals.logger);
            if (locals.randOut.deferredToGate == 1)
            {
                locals.chainIn.slotIdx = locals.randOut.deferredGateSlot;
                locals.chainIn.amount = locals.randOut.deferredGateAmount;
                locals.chainIn.hopCount = 0;
                routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                // Dispatch deferred gate-as-recipient routing from chain hop
                locals.savedDeferredCount = locals.chainOut.deferredCount;
                locals.savedDeferredHopCount = locals.chainOut.deferredHopCount;
                for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                {
                    locals.savedDeferredSlots.set(locals.deferredDispatchIdx, locals.chainOut.deferredGateSlots.get(locals.deferredDispatchIdx));
                    locals.savedDeferredAmounts.set(locals.deferredDispatchIdx, locals.chainOut.deferredGateAmounts.get(locals.deferredDispatchIdx));
                }
                for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                {
                    locals.chainIn.slotIdx = locals.savedDeferredSlots.get(locals.deferredDispatchIdx);
                    locals.chainIn.amount = locals.savedDeferredAmounts.get(locals.deferredDispatchIdx);
                    locals.chainIn.hopCount = locals.savedDeferredHopCount;
                    routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                }
            }
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
                if (locals.condOut.deferredToGate == 1)
                {
                    locals.chainIn.slotIdx = locals.condOut.deferredGateSlot;
                    locals.chainIn.amount = locals.condOut.deferredGateAmount;
                    locals.chainIn.hopCount = 0;
                    routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                }
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
        else if (locals.gate.mode == QUGATE_MODE_HEARTBEAT)
        {
            // HEARTBEAT mode: accumulate into currentBalance, END_EPOCH distributes after trigger
            locals.gate = state.get()._gates.get(locals.slotIdx);
            locals.gate.currentBalance += locals.amount;
            state.mut()._gates.set(locals.slotIdx, locals.gate);
            locals.logger._type = QUGATE_LOG_PAYMENT_FORWARDED;
            LOG_INFO(locals.logger);
        }
        else if (locals.gate.mode == QUGATE_MODE_MULTISIG)
        {
            // MULTISIG mode: always accumulate funds; guardian senders also cast a vote
            locals.gate = state.get()._gates.get(locals.slotIdx);
            locals.gate.currentBalance += locals.amount;
            state.mut()._gates.set(locals.slotIdx, locals.gate);
            locals.logger._type = QUGATE_LOG_PAYMENT_FORWARDED;
            LOG_INFO(locals.logger);

            locals.msigIn.slotIdx = locals.slotIdx;
            locals.msigIn.gateId = input.gateId;
            locals.msigIn.amount = locals.amount;
            processMultisigVote(qpi, state, locals.msigIn, locals.msigOut, locals.msigLocals);
            if (locals.msigOut.status != QUGATE_SUCCESS)
            {
                output.status = locals.msigOut.status;
                return;
            }
            if (locals.msigOut.deferredToGate == 1)
            {
                locals.chainIn.slotIdx = locals.msigOut.deferredGateSlot;
                locals.chainIn.amount = locals.msigOut.deferredGateAmount;
                locals.chainIn.hopCount = 0;
                routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                // Dispatch deferred gate-as-recipient routing from chain hop
                locals.savedDeferredCount = locals.chainOut.deferredCount;
                locals.savedDeferredHopCount = locals.chainOut.deferredHopCount;
                for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                {
                    locals.savedDeferredSlots.set(locals.deferredDispatchIdx, locals.chainOut.deferredGateSlots.get(locals.deferredDispatchIdx));
                    locals.savedDeferredAmounts.set(locals.deferredDispatchIdx, locals.chainOut.deferredGateAmounts.get(locals.deferredDispatchIdx));
                }
                for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                {
                    locals.chainIn.slotIdx = locals.savedDeferredSlots.get(locals.deferredDispatchIdx);
                    locals.chainIn.amount = locals.savedDeferredAmounts.get(locals.deferredDispatchIdx);
                    locals.chainIn.hopCount = locals.savedDeferredHopCount;
                    routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                }
            }
            // Chain forwarding on MULTISIG release (recipientCount==0 with chain target)
            if (locals.msigOut.chainForward == 1)
            {
                locals.gate = state.get()._gates.get(locals.slotIdx);
                sint64 currentChainGateId = locals.gate.chainNextGateId;
                if (currentChainGateId != -1)
                {
                    uint64 nextSlot = (uint64)currentChainGateId & QUGATE_GATE_ID_SLOT_MASK;
                    uint64 nextGen = (uint64)currentChainGateId >> QUGATE_GATE_ID_SLOT_BITS;
                    if (nextSlot < state.get()._gateCount && nextGen > 0
                        && state.get()._gateGenerations.get(nextSlot) == (uint16)(nextGen - 1))
                    {
                        locals.chainIn.slotIdx = nextSlot;
                        locals.chainIn.amount = locals.msigOut.chainForwardAmount;
                        locals.chainIn.hopCount = 0;
                        routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                    }
                }
            }
        }
        else if (locals.gate.mode == QUGATE_MODE_TIME_LOCK)
        {
            // TIME_LOCK mode: only accepts funds when configured; relative mode anchors unlock on first funding
            locals.gate = state.get()._gates.get(locals.slotIdx);
            locals.tlCfg = state.get()._timeLockConfigs.get(locals.slotIdx);
            if (locals.tlCfg.active == 0 || locals.tlCfg.cancelled == 1)
            {
                qpi.transfer(qpi.invocator(), locals.amount);
            }
            else if (locals.tlCfg.fired == 1)
            {
                // Gate has already fired — refund sender
                qpi.transfer(qpi.invocator(), locals.amount);
            }
            else
            {
                if (locals.tlCfg.lockMode == QUGATE_TIME_LOCK_RELATIVE_EPOCHS && locals.tlCfg.unlockEpoch == 0)
                {
                    locals.tlCfg.unlockEpoch = (uint32)qpi.epoch() + locals.tlCfg.delayEpochs;
                    state.mut()._timeLockConfigs.set(locals.slotIdx, locals.tlCfg);
                }
                locals.gate.currentBalance += locals.amount;
                state.mut()._gates.set(locals.slotIdx, locals.gate);
                locals.logger._type = QUGATE_LOG_PAYMENT_FORWARDED;
                LOG_INFO(locals.logger);
            }
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

            // Accumulate-on-arrival modes: chainAmount is 0 at send time
            // (funds accumulate, chain forwarding happens at release time via
            // processMultisigVote / END_EPOCH heartbeat / END_EPOCH time-lock)

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
                    // Dispatch deferred gate-as-recipient routing from chain hop
                    locals.savedDeferredCount = locals.chainOut.deferredCount;
                    locals.savedDeferredHopCount = locals.chainOut.deferredHopCount;
                    for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                    {
                        locals.savedDeferredSlots.set(locals.deferredDispatchIdx, locals.chainOut.deferredGateSlots.get(locals.deferredDispatchIdx));
                        locals.savedDeferredAmounts.set(locals.deferredDispatchIdx, locals.chainOut.deferredGateAmounts.get(locals.deferredDispatchIdx));
                    }
                    for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                    {
                        locals.chainIn.slotIdx = locals.savedDeferredSlots.get(locals.deferredDispatchIdx);
                        locals.chainIn.amount = locals.savedDeferredAmounts.get(locals.deferredDispatchIdx);
                        locals.chainIn.hopCount = locals.savedDeferredHopCount;
                        routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                    }
                    locals.nextChainGate = state.get()._gates.get(nextSlot);
                    currentChainGateId = locals.nextChainGate.chainNextGateId;
                    hop += 1;
                }
                // Failsafe: if chain forwarding couldn't deliver, revert to currentBalance
                if (chainAmount > 0)
                {
                    locals.gate = state.get()._gates.get(locals.slotIdx);
                    locals.gate.currentBalance += (uint64)chainAmount;
                    locals.gate.totalForwarded -= (uint64)chainAmount; // undo the premature forward count
                    state.mut()._gates.set(locals.slotIdx, locals.gate);
                    locals.logger._type = QUGATE_LOG_CHAIN_HOP_INSUFFICIENT;
                    locals.logger.gateId = input.gateId;
                    locals.logger.amount = chainAmount;
                    LOG_INFO(locals.logger);
                }
            }
        }
    }

    PUBLIC_PROCEDURE_WITH_LOCALS(closeGate)
    {
        locals.invReward = qpi.invocationReward();
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
            if (locals.invReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invReward);
            }
            output.status = QUGATE_INVALID_GATE_ID;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_GATE;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.gate = state.get()._gates.get(locals.slotIdx);

        // Authorization: owner OR admin gate approval
        locals.adminApprovalUsed = 0;
        if (locals.gate.owner != qpi.invocator())
        {
            uint8 adminAuth = 0;
            if (locals.gate.hasAdminGate)
            {
                locals.adminCheckSlot = locals.gate.adminGateId & QUGATE_GATE_ID_SLOT_MASK;
                if (locals.adminCheckSlot < state.get()._gateCount)
                {
                    locals.adminCheckGate = state.get()._gates.get(locals.adminCheckSlot);
                    if (locals.adminCheckGate.active && locals.adminCheckGate.mode == QUGATE_MODE_MULTISIG)
                    {
                        locals.adminCheckApproval = state.get()._adminApprovalStates.get(locals.adminCheckSlot);
                        if (locals.adminCheckApproval.active == 1)
                        {
                            if ((uint32)qpi.epoch() <= locals.adminCheckApproval.validUntilEpoch)
                            {
                                adminAuth = 1;
                                locals.adminApprovalUsed = 1;
                            }
                            else
                            {
                                locals.adminCheckApproval.active = 0;
                                locals.adminCheckApproval.validUntilEpoch = 0;
                                state.mut()._adminApprovalStates.set(locals.adminCheckSlot, locals.adminCheckApproval);
                            }
                        }
                    }
                }
            }
            if (adminAuth == 0)
            {
                if (locals.invReward > 0)
                {
                    qpi.transfer(qpi.invocator(), locals.invReward);
                }
                output.status = QUGATE_UNAUTHORIZED;
                locals.logger._type = QUGATE_LOG_FAIL_UNAUTHORIZED;
                LOG_WARNING(locals.logger);
                return;
            }
        }

        if (locals.gate.active == 0)
        {
            if (locals.invReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invReward);
            }
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Lazy expiry check: expire gate if inactive too long
        if (state.get()._expiryEpochs > 0
            && qpi.epoch() - locals.gate.lastActivityEpoch >= state.get()._expiryEpochs
            && locals.gate.active == 1)
        {
            if (locals.gate.currentBalance > 0)
            {
                if (qpi.transfer(locals.gate.owner, locals.gate.currentBalance) >= 0) // [QG-03]
                {
                    locals.gate.currentBalance = 0;
                }
            }
            if (locals.gate.mode == QUGATE_MODE_ORACLE && locals.gate.oracleReserve > 0)
            {
                if (qpi.transfer(locals.gate.owner, locals.gate.oracleReserve) >= 0) // [QG-03]
                {
                    locals.gate.oracleReserve = 0;
                }
            }
            if (locals.gate.chainReserve > 0)
            {
                if (qpi.transfer(locals.gate.owner, locals.gate.chainReserve) >= 0) // [QG-03]
                {
                    locals.gate.chainReserve = 0;
                }
            }
            locals.gate.active = 0;
            state.mut()._gates.set(locals.slotIdx, locals.gate);
            state.mut()._activeGates -= 1;
            state.mut()._freeSlots.set(state.get()._freeCount, locals.slotIdx);
            state.mut()._freeCount += 1;
            state.mut()._gateGenerations.set(locals.slotIdx, state.get()._gateGenerations.get(locals.slotIdx) + 1);
            if (locals.invReward > 0) qpi.transfer(qpi.invocator(), locals.invReward);
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_GATE_EXPIRED;
            LOG_INFO(locals.logger);
            return;
        }

        // Refund any held balance (THRESHOLD / ORACLE mode)
        if (locals.gate.currentBalance > 0)
        {
            if (qpi.transfer(locals.gate.owner, locals.gate.currentBalance) >= 0) // [QG-06]
            {
                locals.gate.currentBalance = 0;
            }
        }

        // Refund oracle reserve and unsubscribe
        if (locals.gate.mode == QUGATE_MODE_ORACLE)
        {
            if (locals.gate.oracleReserve > 0)
            {
                if (qpi.transfer(locals.gate.owner, locals.gate.oracleReserve) >= 0) // [QG-06]
                {
                    locals.gate.oracleReserve = 0;
                }
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
            if (qpi.transfer(locals.gate.owner, locals.gate.chainReserve) >= 0) // [QG-06]
            {
                locals.gate.chainReserve = 0;
            }
        }

        // Clear heartbeat config on close
        if (locals.gate.mode == QUGATE_MODE_HEARTBEAT)
        {
            locals.hbZeroCfg.thresholdEpochs = 0;
            locals.hbZeroCfg.lastHeartbeatEpoch = 0;
            locals.hbZeroCfg.payoutPercentPerEpoch = 0;
            locals.hbZeroCfg.minimumBalance = 0;
            locals.hbZeroCfg.active = 0;
            locals.hbZeroCfg.triggered = 0;
            locals.hbZeroCfg.triggerEpoch = 0;
            locals.hbZeroCfg.beneficiaryCount = 0;
            for (uint8 _bi = 0; _bi < 8; _bi++)
            {
                locals.hbZeroCfg.beneficiaryAddresses.set(_bi, id::zero());
                locals.hbZeroCfg.beneficiaryShares.set(_bi, 0);
            }
            state.mut()._heartbeatConfigs.set(locals.slotIdx, locals.hbZeroCfg);
        }

        // Clear multisig config on close
        if (locals.gate.mode == QUGATE_MODE_MULTISIG)
        {
            for (uint8 _ci = 0; _ci < 8; _ci++)
            {
                locals.msigZeroCfg.guardians.set(_ci, id::zero());
            }
            locals.msigZeroCfg.guardianCount = 0;
            locals.msigZeroCfg.required = 0;
            locals.msigZeroCfg.proposalExpiryEpochs = 0;
            locals.msigZeroCfg.adminApprovalWindowEpochs = 0;
            locals.msigZeroCfg.approvalBitmap = 0;
            locals.msigZeroCfg.approvalCount = 0;
            locals.msigZeroCfg.proposalEpoch = 0;
            locals.msigZeroCfg.proposalActive = 0;
            state.mut()._multisigConfigs.set(locals.slotIdx, locals.msigZeroCfg);
            locals.adminApprovalZero.active = 0;
            locals.adminApprovalZero.validUntilEpoch = 0;
            state.mut()._adminApprovalStates.set(locals.slotIdx, locals.adminApprovalZero);
        }

        // Clear time lock config on close
        if (locals.gate.mode == QUGATE_MODE_TIME_LOCK)
        {
            locals.tlZeroCfg.unlockEpoch = 0;
            locals.tlZeroCfg.delayEpochs = 0;
            locals.tlZeroCfg.lockMode = QUGATE_TIME_LOCK_ABSOLUTE_EPOCH;
            locals.tlZeroCfg.cancellable = 0;
            locals.tlZeroCfg.fired = 0;
            locals.tlZeroCfg.cancelled = 0;
            locals.tlZeroCfg.active = 0;
            state.mut()._timeLockConfigs.set(locals.slotIdx, locals.tlZeroCfg);
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
        if (locals.invReward > 0)
        {
            qpi.transfer(qpi.invocator(), locals.invReward);
        }

        if (locals.adminApprovalUsed == 1)
        {
            locals.adminCheckApproval = state.get()._adminApprovalStates.get(locals.adminCheckSlot);
            locals.adminCheckApproval.active = 0;
            locals.adminCheckApproval.validUntilEpoch = 0;
            state.mut()._adminApprovalStates.set(locals.adminCheckSlot, locals.adminCheckApproval);
            locals.logger._type = QUGATE_LOG_ADMIN_APPROVAL_USED;
            locals.logger.amount = locals.gate.adminGateId;
            LOG_INFO(locals.logger);
        }

        // Log success
        locals.logger._type = QUGATE_LOG_GATE_CLOSED;
        LOG_INFO(locals.logger);
    }

    PUBLIC_PROCEDURE_WITH_LOCALS(updateGate)
    {
        locals.invReward = qpi.invocationReward();
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
            if (locals.invReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invReward);
            }
            output.status = QUGATE_INVALID_GATE_ID;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_GATE;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.gate = state.get()._gates.get(locals.slotIdx);

        // Authorization: owner OR admin gate approval
        locals.adminApprovalUsed = 0;
        if (locals.gate.owner != qpi.invocator())
        {
            uint8 adminAuth = 0;
            if (locals.gate.hasAdminGate)
            {
                locals.adminCheckSlot = locals.gate.adminGateId & QUGATE_GATE_ID_SLOT_MASK;
                if (locals.adminCheckSlot < state.get()._gateCount)
                {
                    locals.adminCheckGate = state.get()._gates.get(locals.adminCheckSlot);
                    if (locals.adminCheckGate.active && locals.adminCheckGate.mode == QUGATE_MODE_MULTISIG)
                    {
                        locals.adminCheckApproval = state.get()._adminApprovalStates.get(locals.adminCheckSlot);
                        if (locals.adminCheckApproval.active == 1)
                        {
                            if ((uint32)qpi.epoch() <= locals.adminCheckApproval.validUntilEpoch)
                            {
                                adminAuth = 1;
                                locals.adminApprovalUsed = 1;
                            }
                            else
                            {
                                locals.adminCheckApproval.active = 0;
                                locals.adminCheckApproval.validUntilEpoch = 0;
                                state.mut()._adminApprovalStates.set(locals.adminCheckSlot, locals.adminCheckApproval);
                            }
                        }
                    }
                }
            }
            if (adminAuth == 0)
            {
                if (locals.invReward > 0)
                {
                    qpi.transfer(qpi.invocator(), locals.invReward);
                }
                output.status = QUGATE_UNAUTHORIZED;
                locals.logger._type = QUGATE_LOG_FAIL_UNAUTHORIZED;
                LOG_WARNING(locals.logger);
                return;
            }
        }

        if (locals.gate.active == 0)
        {
            if (locals.invReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invReward);
            }
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Lazy expiry check: expire gate if inactive too long
        if (state.get()._expiryEpochs > 0
            && qpi.epoch() - locals.gate.lastActivityEpoch >= state.get()._expiryEpochs
            && locals.gate.active == 1)
        {
            if (locals.gate.currentBalance > 0)
            {
                qpi.transfer(locals.gate.owner, locals.gate.currentBalance);
                locals.gate.currentBalance = 0;
            }
            if (locals.gate.mode == QUGATE_MODE_ORACLE && locals.gate.oracleReserve > 0)
            {
                qpi.transfer(locals.gate.owner, locals.gate.oracleReserve);
                locals.gate.oracleReserve = 0;
            }
            if (locals.gate.chainReserve > 0)
            {
                qpi.transfer(locals.gate.owner, locals.gate.chainReserve);
                locals.gate.chainReserve = 0;
            }
            locals.gate.active = 0;
            state.mut()._gates.set(locals.slotIdx, locals.gate);
            state.mut()._activeGates -= 1;
            state.mut()._freeSlots.set(state.get()._freeCount, locals.slotIdx);
            state.mut()._freeCount += 1;
            state.mut()._gateGenerations.set(locals.slotIdx, state.get()._gateGenerations.get(locals.slotIdx) + 1);
            if (locals.invReward > 0) qpi.transfer(qpi.invocator(), locals.invReward);
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_GATE_EXPIRED;
            LOG_INFO(locals.logger);
            return;
        }

        // Oracle gates with pending balance cannot change recipients — prevents rug-pull of accumulated funds
        if (locals.gate.mode == QUGATE_MODE_ORACLE && locals.gate.currentBalance > 0)
        {
            if (locals.invReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invReward);
            }
            output.status = QUGATE_UNAUTHORIZED;
            locals.logger._type = QUGATE_LOG_FAIL_UNAUTHORIZED;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate new recipient count
        if (input.recipientCount > QUGATE_MAX_RECIPIENTS)
        {
            if (locals.invReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invReward);
            }
            output.status = QUGATE_INVALID_RECIPIENT_COUNT;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }
        if (input.recipientCount == 0 && state.get()._gates.get(locals.slotIdx).chainNextGateId == -1)
        {
            if (locals.invReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invReward);
            }
            output.status = QUGATE_INVALID_RECIPIENT_COUNT;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate allowedSenderCount bounds
        if (input.allowedSenderCount > QUGATE_MAX_RECIPIENTS)
        {
            if (locals.invReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invReward);
            }
            output.status = QUGATE_INVALID_SENDER_COUNT;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate SPLIT ratios if gate is SPLIT mode
        if (locals.gate.mode == QUGATE_MODE_SPLIT && input.recipientCount > 0)
        {
            locals.totalRatio = 0;
            for (locals.i = 0; locals.i < input.recipientCount; locals.i++)
            {
                if (input.ratios.get(locals.i) > QUGATE_MAX_RATIO)
                {
                    if (locals.invReward > 0)
                    {
                        qpi.transfer(qpi.invocator(), locals.invReward);
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
                if (locals.invReward > 0)
                {
                    qpi.transfer(qpi.invocator(), locals.invReward);
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
            if (locals.invReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invReward);
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
        // Reset round robin index if it would be out of bounds after recipient count change
        if (locals.gate.mode == QUGATE_MODE_ROUND_ROBIN && locals.gate.roundRobinIndex >= input.recipientCount)
        {
            locals.gate.roundRobinIndex = 0;
        }
        locals.gate.threshold = input.threshold;
        locals.gate.allowedSenderCount = input.allowedSenderCount;

        // Use locals.i, zero stale slots
        for (locals.i = 0; locals.i < QUGATE_MAX_RECIPIENTS; locals.i++)
        {
            if (locals.i < input.recipientCount)
            {
                locals.gate.recipients.set(locals.i, input.recipients.get(locals.i));
                locals.gate.ratios.set(locals.i, input.ratios.get(locals.i));
                locals.gate.recipientGateIds.set(locals.i, input.recipientGateIds.get(locals.i));
            }
            else
            {
                locals.gate.recipients.set(locals.i, id::zero());
                locals.gate.ratios.set(locals.i, 0);
                locals.gate.recipientGateIds.set(locals.i, -1);
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

        // Validate gate-as-recipient IDs
        for (locals.i = 0; locals.i < input.recipientCount; locals.i++)
        {
            if (input.recipientGateIds.get(locals.i) >= 0)
            {
                uint64 rgSlot = (uint64)(input.recipientGateIds.get(locals.i)) & QUGATE_GATE_ID_SLOT_MASK;
                uint64 rgGen = (uint64)(input.recipientGateIds.get(locals.i)) >> QUGATE_GATE_ID_SLOT_BITS;
                if (rgSlot >= state.get()._gateCount
                    || rgGen == 0
                    || state.get()._gateGenerations.get(rgSlot) != (uint16)(rgGen - 1)
                    || state.get()._gates.get(rgSlot).active == 0)
                {
                    if (locals.invReward > 0) qpi.transfer(qpi.invocator(), locals.invReward);
                    output.status = QUGATE_INVALID_GATE_RECIPIENT;
                    locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                    LOG_WARNING(locals.logger);
                    return;
                }
            }
        }

        state.mut()._gates.set(locals.slotIdx, locals.gate);

        if (locals.invReward > 0)
        {
            qpi.transfer(qpi.invocator(), locals.invReward);
        }

        if (locals.adminApprovalUsed == 1)
        {
            locals.adminCheckApproval = state.get()._adminApprovalStates.get(locals.adminCheckSlot);
            locals.adminCheckApproval.active = 0;
            locals.adminCheckApproval.validUntilEpoch = 0;
            state.mut()._adminApprovalStates.set(locals.adminCheckSlot, locals.adminCheckApproval);
            locals.logger._type = QUGATE_LOG_ADMIN_APPROVAL_USED;
            locals.logger.amount = locals.gate.adminGateId;
            LOG_INFO(locals.logger);
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
        locals.invReward = qpi.invocationReward();
        output.result = QUGATE_SUCCESS;

        locals.logger._contractIndex = CONTRACT_INDEX;
        locals.logger.sender = qpi.invocator();
        locals.logger.gateId = input.gateId;
        locals.logger.amount = locals.invReward;

        // Decode versioned gateId: lower 20 bits = slotIndex, upper bits = generation
        locals.slotIdx = input.gateId & QUGATE_GATE_ID_SLOT_MASK;
        locals.encodedGen = (input.gateId >> QUGATE_GATE_ID_SLOT_BITS);
        if (input.gateId == 0
            || locals.slotIdx >= state.get()._gateCount
            || locals.encodedGen == 0
            || state.get()._gateGenerations.get(locals.slotIdx) != (uint16)(locals.encodedGen - 1))
        {
            if (locals.invReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invReward);
            }
            output.result = QUGATE_INVALID_GATE_ID;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_GATE;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.gate = state.get()._gates.get(locals.slotIdx);

        if (locals.gate.active == 0)
        {
            if (locals.invReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invReward);
            }
            output.result = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Lazy expiry check: expire gate if inactive too long
        if (state.get()._expiryEpochs > 0
            && qpi.epoch() - locals.gate.lastActivityEpoch >= state.get()._expiryEpochs
            && locals.gate.active == 1)
        {
            if (locals.gate.currentBalance > 0)
            {
                if (qpi.transfer(locals.gate.owner, locals.gate.currentBalance) >= 0) // [QG-04]
                {
                    locals.gate.currentBalance = 0;
                }
            }
            if (locals.gate.mode == QUGATE_MODE_ORACLE && locals.gate.oracleReserve > 0)
            {
                if (qpi.transfer(locals.gate.owner, locals.gate.oracleReserve) >= 0) // [QG-04]
                {
                    locals.gate.oracleReserve = 0;
                }
            }
            if (locals.gate.chainReserve > 0)
            {
                if (qpi.transfer(locals.gate.owner, locals.gate.chainReserve) >= 0) // [QG-04]
                {
                    locals.gate.chainReserve = 0;
                }
            }
            locals.gate.active = 0;
            state.mut()._gates.set(locals.slotIdx, locals.gate);
            state.mut()._activeGates -= 1;
            state.mut()._freeSlots.set(state.get()._freeCount, locals.slotIdx);
            state.mut()._freeCount += 1;
            state.mut()._gateGenerations.set(locals.slotIdx, state.get()._gateGenerations.get(locals.slotIdx) + 1);
            if (locals.invReward > 0) qpi.transfer(qpi.invocator(), locals.invReward);
            output.result = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_GATE_EXPIRED;
            LOG_INFO(locals.logger);
            return;
        }

        if (locals.invReward <= 0)
        {
            output.result = QUGATE_DUST_AMOUNT;
            return;
        }

        if (input.reserveTarget == 0)
        {
            // Oracle reserve
            if (locals.gate.mode != QUGATE_MODE_ORACLE)
            {
                if (locals.invReward > 0)
                {
                    qpi.transfer(qpi.invocator(), locals.invReward);
                }
                output.result = QUGATE_INVALID_ORACLE_CONFIG;
                locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                LOG_WARNING(locals.logger);
                return;
            }
            locals.gate.oracleReserve += locals.invReward;
        }
        else if (input.reserveTarget == 1)
        {
            // Chain reserve — gate must have a chain link
            if (locals.gate.chainNextGateId == -1)
            {
                if (locals.invReward > 0)
                {
                    qpi.transfer(qpi.invocator(), locals.invReward);
                }
                output.result = QUGATE_INVALID_CHAIN;
                locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                LOG_WARNING(locals.logger);
                return;
            }
            locals.gate.chainReserve += locals.invReward;
        }
        else
        {
            // Invalid reserveTarget
            if (locals.invReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invReward);
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
                locals.priceScaled = (sint64)QPI::div((uint64)(input.reply.numerator * 1000000), (uint64)input.reply.denominator);
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
                locals.priceScaled = (sint64)QPI::div((uint64)(input.reply.numerator * 1000000), (uint64)input.reply.denominator);
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

            // Route deferred gate-recipients from split
            for (uint8 _dg = 0; _dg < locals.splitOut.deferredCount; _dg++)
            {
                locals.chainIn.slotIdx = locals.splitOut.deferredGateSlots.get(_dg);
                locals.chainIn.amount = locals.splitOut.deferredGateAmounts.get(_dg);
                locals.chainIn.hopCount = 0;
                routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                // Dispatch deferred gate-as-recipient routing from chain hop
                locals.savedDeferredCount = locals.chainOut.deferredCount;
                locals.savedDeferredHopCount = locals.chainOut.deferredHopCount;
                for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                {
                    locals.savedDeferredSlots.set(locals.deferredDispatchIdx, locals.chainOut.deferredGateSlots.get(locals.deferredDispatchIdx));
                    locals.savedDeferredAmounts.set(locals.deferredDispatchIdx, locals.chainOut.deferredGateAmounts.get(locals.deferredDispatchIdx));
                }
                for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                {
                    locals.chainIn.slotIdx = locals.savedDeferredSlots.get(locals.deferredDispatchIdx);
                    locals.chainIn.amount = locals.savedDeferredAmounts.get(locals.deferredDispatchIdx);
                    locals.chainIn.hopCount = locals.savedDeferredHopCount;
                    routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                }
            }

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
                    // Dispatch deferred gate-as-recipient routing from chain hop
                    locals.savedDeferredCount = locals.chainOut.deferredCount;
                    locals.savedDeferredHopCount = locals.chainOut.deferredHopCount;
                    for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                    {
                        locals.savedDeferredSlots.set(locals.deferredDispatchIdx, locals.chainOut.deferredGateSlots.get(locals.deferredDispatchIdx));
                        locals.savedDeferredAmounts.set(locals.deferredDispatchIdx, locals.chainOut.deferredGateAmounts.get(locals.deferredDispatchIdx));
                    }
                    for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                    {
                        locals.chainIn.slotIdx = locals.savedDeferredSlots.get(locals.deferredDispatchIdx);
                        locals.chainIn.amount = locals.savedDeferredAmounts.get(locals.deferredDispatchIdx);
                        locals.chainIn.hopCount = locals.savedDeferredHopCount;
                        routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                    }
                    // Read updated gate to get its chainNextGateId
                    locals.nextChainGate = state.get()._gates.get(nextSlot);
                    currentChainGateId = locals.nextChainGate.chainNextGateId;
                    hop++;
                }
                // Failsafe: if chain forwarding couldn't deliver, revert to currentBalance
                if (chainAmount > 0)
                {
                    locals.gate = state.get()._gates.get(locals.slotIdx);
                    locals.gate.currentBalance += (uint64)chainAmount;
                    locals.gate.totalForwarded -= (uint64)chainAmount; // undo the premature forward count
                    state.mut()._gates.set(locals.slotIdx, locals.gate);
                    locals.logger._type = QUGATE_LOG_CHAIN_HOP_INSUFFICIENT;
                    locals.logger.gateId = ((uint64)(state.get()._gateGenerations.get(locals.slotIdx) + 1) << QUGATE_GATE_ID_SLOT_BITS) | locals.slotIdx;
                    locals.logger.amount = chainAmount;
                    LOG_INFO(locals.logger);
                }
            }

            // If ONCE mode, close gate after trigger (guard against double-close underflow)
            if (locals.gate.oracleTriggerMode == QUGATE_ORACLE_TRIGGER_ONCE && locals.gate.active == 1)
            {
                if (locals.gate.oracleReserve > 0)
                {
                    if (qpi.transfer(locals.gate.owner, locals.gate.oracleReserve) >= 0) // [QG-13]
                    {
                        locals.gate.oracleReserve = 0;
                    }
                }
                if (locals.gate.oracleSubscriptionId >= 0)
                {
                    state.mut()._subscriptionToSlot.removeByKey(locals.gate.oracleSubscriptionId);
                    qpi.unsubscribeOracle(locals.gate.oracleSubscriptionId);
                    locals.gate.oracleSubscriptionId = -1;
                }
                // Defensive cleanup of auxiliary configs for recycled slot
                {
                    QUGATE_HeartbeatConfig zeroCfg;
                    zeroCfg.active = 0; zeroCfg.triggered = 0; zeroCfg.thresholdEpochs = 0;
                    zeroCfg.lastHeartbeatEpoch = 0; zeroCfg.payoutPercentPerEpoch = 0;
                    zeroCfg.minimumBalance = 0; zeroCfg.triggerEpoch = 0; zeroCfg.beneficiaryCount = 0;
                    for (uint8 _zi = 0; _zi < 8; _zi++)
                    {
                        zeroCfg.beneficiaryAddresses.set(_zi, id::zero());
                        zeroCfg.beneficiaryShares.set(_zi, 0);
                    }
                    state.mut()._heartbeatConfigs.set(locals.slotIdx, zeroCfg);
                    QUGATE_MultisigConfig zeroMs;
                    for (uint8 _zi = 0; _zi < 8; _zi++)
                    {
                        zeroMs.guardians.set(_zi, id::zero());
                    }
                    zeroMs.guardianCount = 0; zeroMs.required = 0; zeroMs.proposalExpiryEpochs = 0; zeroMs.adminApprovalWindowEpochs = 0;
                    zeroMs.approvalBitmap = 0; zeroMs.approvalCount = 0; zeroMs.proposalEpoch = 0; zeroMs.proposalActive = 0;
                    state.mut()._multisigConfigs.set(locals.slotIdx, zeroMs);
                    QUGATE_AdminApprovalState zeroAdminApproval;
                    zeroAdminApproval.active = 0; zeroAdminApproval.validUntilEpoch = 0;
                    state.mut()._adminApprovalStates.set(locals.slotIdx, zeroAdminApproval);
                    QUGATE_TimeLockConfig zeroTl;
                    zeroTl.unlockEpoch = 0; zeroTl.delayEpochs = 0; zeroTl.lockMode = QUGATE_TIME_LOCK_ABSOLUTE_EPOCH; zeroTl.cancellable = 0; zeroTl.fired = 0; zeroTl.cancelled = 0; zeroTl.active = 0;
                    state.mut()._timeLockConfigs.set(locals.slotIdx, zeroTl);
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
        output.adminGateId = locals.gate.adminGateId;
        output.hasAdminGate = locals.gate.hasAdminGate;

        // Gate-as-recipient
        for (locals.i = 0; locals.i < 8; locals.i++)
        {
            output.recipientGateIds.set(locals.i, locals.gate.recipientGateIds.get(locals.i));
        }

        // Report as inactive if expired (function can't mutate state)
        if (state.get()._expiryEpochs > 0
            && qpi.epoch() - locals.gate.lastActivityEpoch >= state.get()._expiryEpochs)
        {
            output.active = 0;
        }
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
                locals.entry.allowedSenderCount = 0;
                locals.entry.adminGateId = -1;
                locals.entry.hasAdminGate = 0;
                for (locals.j = 0; locals.j < QUGATE_MAX_RECIPIENTS; locals.j++)
                {
                    locals.entry.recipients.set(locals.j, id::zero());
                    locals.entry.ratios.set(locals.j, 0);
                    locals.entry.recipientGateIds.set(locals.j, -1);
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
                locals.entry.allowedSenderCount = locals.gate.allowedSenderCount;
                locals.entry.adminGateId = locals.gate.adminGateId;
                locals.entry.hasAdminGate = locals.gate.hasAdminGate;

                for (locals.j = 0; locals.j < QUGATE_MAX_RECIPIENTS; locals.j++)
                {
                    locals.entry.recipients.set(locals.j, locals.gate.recipients.get(locals.j));
                    locals.entry.ratios.set(locals.j, locals.gate.ratios.get(locals.j));
                    locals.entry.allowedSenders.set(locals.j, locals.gate.allowedSenders.get(locals.j));
                    locals.entry.recipientGateIds.set(locals.j, locals.gate.recipientGateIds.get(locals.j));
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
        locals.invReward = qpi.invocationReward();
        output.result = QUGATE_SUCCESS;

        locals.logger._contractIndex = CONTRACT_INDEX;
        locals.logger.sender = qpi.invocator();
        locals.logger.gateId = input.gateId;
        locals.logger.amount = 0;

        // Decode versioned gateId: lower 20 bits = slotIndex, upper bits = generation
        // Decode source gate
        locals.slotIdx = input.gateId & QUGATE_GATE_ID_SLOT_MASK;
        locals.encodedGen = input.gateId >> QUGATE_GATE_ID_SLOT_BITS;
        if (input.gateId == 0
            || locals.slotIdx >= state.get()._gateCount
            || locals.encodedGen == 0
            || state.get()._gateGenerations.get(locals.slotIdx) != (uint16)(locals.encodedGen - 1))
        {
            if (locals.invReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invReward);
            }
            output.result = QUGATE_INVALID_GATE_ID;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_GATE;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.gate = state.get()._gates.get(locals.slotIdx);

        // Authorization: owner OR admin gate approval
        locals.adminApprovalUsed = 0;
        if (locals.gate.owner != qpi.invocator())
        {
            uint8 adminAuth = 0;
            if (locals.gate.hasAdminGate)
            {
                locals.adminCheckSlot = locals.gate.adminGateId & QUGATE_GATE_ID_SLOT_MASK;
                if (locals.adminCheckSlot < state.get()._gateCount)
                {
                    locals.adminCheckGate = state.get()._gates.get(locals.adminCheckSlot);
                    if (locals.adminCheckGate.active && locals.adminCheckGate.mode == QUGATE_MODE_MULTISIG)
                    {
                        locals.adminCheckApproval = state.get()._adminApprovalStates.get(locals.adminCheckSlot);
                        if (locals.adminCheckApproval.active == 1)
                        {
                            if ((uint32)qpi.epoch() <= locals.adminCheckApproval.validUntilEpoch)
                            {
                                adminAuth = 1;
                                locals.adminApprovalUsed = 1;
                            }
                            else
                            {
                                locals.adminCheckApproval.active = 0;
                                locals.adminCheckApproval.validUntilEpoch = 0;
                                state.mut()._adminApprovalStates.set(locals.adminCheckSlot, locals.adminCheckApproval);
                            }
                        }
                    }
                }
            }
            if (adminAuth == 0)
            {
                if (locals.invReward > 0)
                {
                    qpi.transfer(qpi.invocator(), locals.invReward);
                }
                output.result = QUGATE_UNAUTHORIZED;
                locals.logger._type = QUGATE_LOG_FAIL_UNAUTHORIZED;
                LOG_WARNING(locals.logger);
                return;
            }
        }

        if (locals.gate.active == 0)
        {
            if (locals.invReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invReward);
            }
            output.result = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Lazy expiry check: expire gate if inactive too long
        if (state.get()._expiryEpochs > 0
            && qpi.epoch() - locals.gate.lastActivityEpoch >= state.get()._expiryEpochs
            && locals.gate.active == 1)
        {
            if (locals.gate.currentBalance > 0)
            {
                if (qpi.transfer(locals.gate.owner, locals.gate.currentBalance) >= 0) // [QG-05]
                {
                    locals.gate.currentBalance = 0;
                }
            }
            if (locals.gate.mode == QUGATE_MODE_ORACLE && locals.gate.oracleReserve > 0)
            {
                if (qpi.transfer(locals.gate.owner, locals.gate.oracleReserve) >= 0) // [QG-05]
                {
                    locals.gate.oracleReserve = 0;
                }
            }
            if (locals.gate.chainReserve > 0)
            {
                if (qpi.transfer(locals.gate.owner, locals.gate.chainReserve) >= 0) // [QG-05]
                {
                    locals.gate.chainReserve = 0;
                }
            }
            locals.gate.active = 0;
            state.mut()._gates.set(locals.slotIdx, locals.gate);
            state.mut()._activeGates -= 1;
            state.mut()._freeSlots.set(state.get()._freeCount, locals.slotIdx);
            state.mut()._freeCount += 1;
            state.mut()._gateGenerations.set(locals.slotIdx, state.get()._gateGenerations.get(locals.slotIdx) + 1);
            if (locals.invReward > 0) qpi.transfer(qpi.invocator(), locals.invReward);
            output.result = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_GATE_EXPIRED;
            LOG_INFO(locals.logger);
            return;
        }

        // Require hop fee as update cost
        if (locals.invReward < QUGATE_CHAIN_HOP_FEE)
        {
            if (locals.invReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invReward);
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
            if (locals.invReward > QUGATE_CHAIN_HOP_FEE)
            {
                qpi.transfer(qpi.invocator(), locals.invReward - QUGATE_CHAIN_HOP_FEE);
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
            if (locals.invReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invReward);
            }
            output.result = QUGATE_INVALID_CHAIN;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.targetGate = state.get()._gates.get(locals.targetSlot);
        if (locals.targetGate.active == 0)
        {
            if (locals.invReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invReward);
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
            if (locals.invReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invReward);
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
                if (locals.invReward > 0)
                {
                    qpi.transfer(qpi.invocator(), locals.invReward);
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
            if (locals.invReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invReward);
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
        if (locals.invReward > QUGATE_CHAIN_HOP_FEE)
        {
            qpi.transfer(qpi.invocator(), locals.invReward - QUGATE_CHAIN_HOP_FEE);
        }

        if (locals.adminApprovalUsed == 1)
        {
            locals.adminCheckApproval = state.get()._adminApprovalStates.get(locals.adminCheckSlot);
            locals.adminCheckApproval.active = 0;
            locals.adminCheckApproval.validUntilEpoch = 0;
            state.mut()._adminApprovalStates.set(locals.adminCheckSlot, locals.adminCheckApproval);
            locals.logger._type = QUGATE_LOG_ADMIN_APPROVAL_USED;
            locals.logger.amount = locals.gate.adminGateId;
            LOG_INFO(locals.logger);
        }

        output.result = QUGATE_SUCCESS;
    }

    // =============================================
    // configureHeartbeat — configure HEARTBEAT gate — epoch-based trigger
    // =============================================

    PUBLIC_PROCEDURE_WITH_LOCALS(configureHeartbeat)
    {
        locals.invReward = qpi.invocationReward();
        output.status = QUGATE_SUCCESS;

        locals.logger._contractIndex = CONTRACT_INDEX;
        locals.logger.sender = qpi.invocator();
        locals.logger.gateId = input.gateId;
        locals.logger.amount = 0;

        // Refund any attached QU
        if (locals.invReward > 0)
        {
            qpi.transfer(qpi.invocator(), locals.invReward);
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

        // Authorization: owner OR admin gate approval
        locals.adminApprovalUsed = 0;
        if (locals.gate.owner != qpi.invocator())
        {
            uint8 adminAuth = 0;
            if (locals.gate.hasAdminGate)
            {
                locals.adminCheckSlot = locals.gate.adminGateId & QUGATE_GATE_ID_SLOT_MASK;
                if (locals.adminCheckSlot < state.get()._gateCount)
                {
                    locals.adminCheckGate = state.get()._gates.get(locals.adminCheckSlot);
                    if (locals.adminCheckGate.active && locals.adminCheckGate.mode == QUGATE_MODE_MULTISIG)
                    {
                        locals.adminCheckApproval = state.get()._adminApprovalStates.get(locals.adminCheckSlot);
                        if (locals.adminCheckApproval.active == 1)
                        {
                            if ((uint32)qpi.epoch() <= locals.adminCheckApproval.validUntilEpoch)
                            {
                                adminAuth = 1;
                                locals.adminApprovalUsed = 1;
                            }
                            else
                            {
                                locals.adminCheckApproval.active = 0;
                                locals.adminCheckApproval.validUntilEpoch = 0;
                                state.mut()._adminApprovalStates.set(locals.adminCheckSlot, locals.adminCheckApproval);
                            }
                        }
                    }
                }
            }
            if (adminAuth == 0)
            {
                output.status = QUGATE_UNAUTHORIZED;
                locals.logger._type = QUGATE_LOG_FAIL_UNAUTHORIZED;
                LOG_WARNING(locals.logger);
                return;
            }
        }

        // Gate must be active
        if (locals.gate.active == 0)
        {
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Gate must be HEARTBEAT mode
        if (locals.gate.mode != QUGATE_MODE_HEARTBEAT)
        {
            output.status = QUGATE_HEARTBEAT_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate thresholdEpochs >= 1
        if (input.thresholdEpochs == 0)
        {
            output.status = QUGATE_HEARTBEAT_INVALID;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate payoutPercentPerEpoch 1-100
        if (input.payoutPercentPerEpoch == 0 || input.payoutPercentPerEpoch > 100)
        {
            output.status = QUGATE_HEARTBEAT_INVALID;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate beneficiary count
        if (input.beneficiaryCount == 0 || input.beneficiaryCount > 8)
        {
            output.status = QUGATE_HEARTBEAT_INVALID;
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
            output.status = QUGATE_HEARTBEAT_INVALID;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Store config and beneficiaries inline in cfg
        locals.cfg.thresholdEpochs = input.thresholdEpochs;
        locals.cfg.lastHeartbeatEpoch = (uint32)qpi.epoch();
        locals.cfg.payoutPercentPerEpoch = input.payoutPercentPerEpoch;
        locals.cfg.minimumBalance = input.minimumBalance;
        locals.cfg.active = 1;
        locals.cfg.triggered = 0;
        locals.cfg.triggerEpoch = 0;

        // Store beneficiaries inline in cfg
        locals.cfg.beneficiaryCount = input.beneficiaryCount;
        for (locals.i = 0; locals.i < 8; locals.i++)
        {
            if (locals.i < input.beneficiaryCount)
            {
                locals.cfg.beneficiaryAddresses.set(locals.i, input.beneficiaryAddresses.get(locals.i));
                locals.cfg.beneficiaryShares.set(locals.i, input.beneficiaryShares.get(locals.i));
            }
            else
            {
                locals.cfg.beneficiaryAddresses.set(locals.i, id::zero());
                locals.cfg.beneficiaryShares.set(locals.i, 0);
            }
        }
        state.mut()._heartbeatConfigs.set(locals.slotIdx, locals.cfg);

        // Update gate activity
        locals.gate.lastActivityEpoch = qpi.epoch();
        state.mut()._gates.set(locals.slotIdx, locals.gate);

        if (locals.adminApprovalUsed == 1)
        {
            locals.adminCheckApproval = state.get()._adminApprovalStates.get(locals.adminCheckSlot);
            locals.adminCheckApproval.active = 0;
            locals.adminCheckApproval.validUntilEpoch = 0;
            state.mut()._adminApprovalStates.set(locals.adminCheckSlot, locals.adminCheckApproval);
            locals.logger._type = QUGATE_LOG_ADMIN_APPROVAL_USED;
            locals.logger.amount = locals.gate.adminGateId;
            LOG_INFO(locals.logger);
        }

        locals.logger._type = QUGATE_LOG_HEARTBEAT_CONFIGURED;
        LOG_INFO(locals.logger);
    }

    // =============================================
    // heartbeat — reset the epoch counter (owner only, before trigger)
    // =============================================

    PUBLIC_PROCEDURE_WITH_LOCALS(heartbeat)
    {
        locals.invReward = qpi.invocationReward();
        output.status = QUGATE_SUCCESS;
        output.epochRecorded = 0;

        locals.logger._contractIndex = CONTRACT_INDEX;
        locals.logger.sender = qpi.invocator();
        locals.logger.gateId = input.gateId;
        locals.logger.amount = 0;

        // Refund any attached QU
        if (locals.invReward > 0)
        {
            qpi.transfer(qpi.invocator(), locals.invReward);
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

        // Gate must be HEARTBEAT mode
        if (locals.gate.mode != QUGATE_MODE_HEARTBEAT)
        {
            output.status = QUGATE_HEARTBEAT_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.cfg = state.get()._heartbeatConfigs.get(locals.slotIdx);

        // Heartbeat must be configured (active)
        if (locals.cfg.active == 0)
        {
            output.status = QUGATE_HEARTBEAT_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Cannot heartbeat after trigger
        if (locals.cfg.triggered == 1)
        {
            output.status = QUGATE_HEARTBEAT_TRIGGERED;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Reset the epoch counter
        locals.cfg.lastHeartbeatEpoch = (uint32)qpi.epoch();
        state.mut()._heartbeatConfigs.set(locals.slotIdx, locals.cfg);

        // Update gate activity
        locals.gate.lastActivityEpoch = qpi.epoch();
        state.mut()._gates.set(locals.slotIdx, locals.gate);

        output.epochRecorded = locals.cfg.lastHeartbeatEpoch;

        locals.logger._type = QUGATE_LOG_HEARTBEAT_PULSE;
        locals.logger.amount = locals.cfg.lastHeartbeatEpoch;
        LOG_INFO(locals.logger);
    }

    // =============================================
    // getHeartbeat — read-only query for heartbeat config
    // =============================================

    PUBLIC_FUNCTION_WITH_LOCALS(getHeartbeat)
    {
        output.active = 0;
        output.triggered = 0;
        output.thresholdEpochs = 0;
        output.lastHeartbeatEpoch = 0;
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
        if (locals.gate.mode != QUGATE_MODE_HEARTBEAT)
        {
            return;
        }

        locals.cfg = state.get()._heartbeatConfigs.get(locals.slotIdx);
        output.active = locals.cfg.active;
        output.triggered = locals.cfg.triggered;
        output.thresholdEpochs = locals.cfg.thresholdEpochs;
        output.lastHeartbeatEpoch = locals.cfg.lastHeartbeatEpoch;
        output.triggerEpoch = locals.cfg.triggerEpoch;
        output.payoutPercentPerEpoch = locals.cfg.payoutPercentPerEpoch;
        output.minimumBalance = locals.cfg.minimumBalance;

        output.beneficiaryCount = locals.cfg.beneficiaryCount;
        for (locals.i = 0; locals.i < 8; locals.i++)
        {
            output.beneficiaryAddresses.set(locals.i, locals.cfg.beneficiaryAddresses.get(locals.i));
            output.beneficiaryShares.set(locals.i, locals.cfg.beneficiaryShares.get(locals.i));
        }
    }

    // =============================================
    // configureMultisig — set up M-of-N guardians on a MULTISIG gate
    // =============================================

    PUBLIC_PROCEDURE_WITH_LOCALS(configureMultisig)
    {
        locals.invReward = qpi.invocationReward();
        output.status = QUGATE_SUCCESS;

        locals.logger._contractIndex = CONTRACT_INDEX;
        locals.logger.sender = qpi.invocator();
        locals.logger.gateId = input.gateId;
        locals.logger.amount = 0;

        // Refund any attached QU
        if (locals.invReward > 0)
        {
            qpi.transfer(qpi.invocator(), locals.invReward);
        }

        // Decode versioned gateId
        locals.slotIdx = input.gateId & QUGATE_GATE_ID_SLOT_MASK;
        locals.encodedGen = input.gateId >> QUGATE_GATE_ID_SLOT_BITS;
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

        // Authorization: owner OR admin gate approval
        locals.adminApprovalUsed = 0;
        if (locals.gate.owner != qpi.invocator())
        {
            uint8 adminAuth = 0;
            if (locals.gate.hasAdminGate)
            {
                locals.adminCheckSlot = locals.gate.adminGateId & QUGATE_GATE_ID_SLOT_MASK;
                if (locals.adminCheckSlot < state.get()._gateCount)
                {
                    locals.adminCheckGate = state.get()._gates.get(locals.adminCheckSlot);
                    if (locals.adminCheckGate.active && locals.adminCheckGate.mode == QUGATE_MODE_MULTISIG)
                    {
                        locals.adminCheckApproval = state.get()._adminApprovalStates.get(locals.adminCheckSlot);
                        if (locals.adminCheckApproval.active == 1)
                        {
                            if ((uint32)qpi.epoch() <= locals.adminCheckApproval.validUntilEpoch)
                            {
                                adminAuth = 1;
                                locals.adminApprovalUsed = 1;
                            }
                            else
                            {
                                locals.adminCheckApproval.active = 0;
                                locals.adminCheckApproval.validUntilEpoch = 0;
                                state.mut()._adminApprovalStates.set(locals.adminCheckSlot, locals.adminCheckApproval);
                            }
                        }
                    }
                }
            }
            if (adminAuth == 0)
            {
                output.status = QUGATE_UNAUTHORIZED;
                locals.logger._type = QUGATE_LOG_FAIL_UNAUTHORIZED;
                LOG_WARNING(locals.logger);
                return;
            }
        }

        // Gate must be active
        if (locals.gate.active == 0)
        {
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Gate must be MULTISIG mode
        if (locals.gate.mode != QUGATE_MODE_MULTISIG)
        {
            output.status = QUGATE_MULTISIG_INVALID_CONFIG;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Prevent reconfiguration while a proposal is active (#79)
        locals.cfg = state.get()._multisigConfigs.get(locals.slotIdx);
        if (locals.cfg.proposalActive == 1)
        {
            // Allow if proposal has expired
            if (locals.cfg.proposalExpiryEpochs > 0
                && (uint32)qpi.epoch() - locals.cfg.proposalEpoch > locals.cfg.proposalExpiryEpochs)
            {
                // Proposal expired — allow reconfig
            }
            else
            {
                output.status = QUGATE_MULTISIG_PROPOSAL_ACTIVE;
                locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                LOG_WARNING(locals.logger);
                return;
            }
        }

        // Validate guardianCount 1-8
        if (input.guardianCount == 0 || input.guardianCount > 8)
        {
            output.status = QUGATE_MULTISIG_INVALID_CONFIG;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate required 1-guardianCount
        if (input.required == 0 || input.required > input.guardianCount)
        {
            output.status = QUGATE_MULTISIG_INVALID_CONFIG;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate proposalExpiryEpochs >= 1
        if (input.proposalExpiryEpochs == 0)
        {
            output.status = QUGATE_MULTISIG_INVALID_CONFIG;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate adminApprovalWindowEpochs >= 1
        if (input.adminApprovalWindowEpochs == 0)
        {
            output.status = QUGATE_MULTISIG_INVALID_CONFIG;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Check for duplicate guardians
        for (locals.i = 0; locals.i < input.guardianCount; locals.i++)
        {
            for (locals.j = locals.i + 1; locals.j < input.guardianCount; locals.j++)
            {
                if (input.guardians.get(locals.i) == input.guardians.get(locals.j))
                {
                    output.status = QUGATE_MULTISIG_INVALID_CONFIG;
                    locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                    LOG_WARNING(locals.logger);
                    return;
                }
            }
        }

        // Store config, reset any active proposal
        for (uint8 _gi = 0; _gi < 8; _gi++)
        {
            if (_gi < input.guardianCount)
            {
                locals.cfg.guardians.set(_gi, input.guardians.get(_gi));
            }
            else
            {
                locals.cfg.guardians.set(_gi, id::zero());
            }
        }
        locals.cfg.guardianCount = input.guardianCount;
        locals.cfg.required = input.required;
        locals.cfg.proposalExpiryEpochs = input.proposalExpiryEpochs;
        locals.cfg.adminApprovalWindowEpochs = input.adminApprovalWindowEpochs;
        locals.cfg.approvalBitmap = 0;
        locals.cfg.approvalCount = 0;
        locals.cfg.proposalEpoch = 0;
        locals.cfg.proposalActive = 0;
        state.mut()._multisigConfigs.set(locals.slotIdx, locals.cfg);
        locals.adminApprovalZero.active = 0;
        locals.adminApprovalZero.validUntilEpoch = 0;
        state.mut()._adminApprovalStates.set(locals.slotIdx, locals.adminApprovalZero);

        // Update gate activity
        locals.gate.lastActivityEpoch = qpi.epoch();
        state.mut()._gates.set(locals.slotIdx, locals.gate);

        if (locals.adminApprovalUsed == 1)
        {
            locals.adminCheckApproval = state.get()._adminApprovalStates.get(locals.adminCheckSlot);
            locals.adminCheckApproval.active = 0;
            locals.adminCheckApproval.validUntilEpoch = 0;
            state.mut()._adminApprovalStates.set(locals.adminCheckSlot, locals.adminCheckApproval);
            locals.logger._type = QUGATE_LOG_ADMIN_APPROVAL_USED;
            locals.logger.amount = locals.gate.adminGateId;
            LOG_INFO(locals.logger);
        }

        locals.logger._type = QUGATE_LOG_MULTISIG_CONFIGURED;
        LOG_INFO(locals.logger);
    }

    // =============================================
    // getMultisigState — read-only query for multisig proposal state
    // =============================================

    PUBLIC_FUNCTION_WITH_LOCALS(getMultisigState)
    {
        output.status = QUGATE_MULTISIG_NO_ACTIVE_PROP;
        output.approvalBitmap = 0;
        output.approvalCount = 0;
        output.required = 0;
        output.guardianCount = 0;
        output.proposalEpoch = 0;
        output.proposalActive = 0;

        // Decode versioned gateId
        locals.slotIdx = input.gateId & QUGATE_GATE_ID_SLOT_MASK;
        locals.encodedGen = input.gateId >> QUGATE_GATE_ID_SLOT_BITS;
        if (input.gateId == 0
            || locals.slotIdx >= state.get()._gateCount
            || locals.encodedGen == 0
            || state.get()._gateGenerations.get(locals.slotIdx) != (uint16)(locals.encodedGen - 1))
        {
            output.status = QUGATE_INVALID_GATE_ID;
            return;
        }

        locals.gate = state.get()._gates.get(locals.slotIdx);
        if (locals.gate.mode != QUGATE_MODE_MULTISIG)
        {
            output.status = QUGATE_MULTISIG_INVALID_CONFIG;
            return;
        }

        locals.cfg = state.get()._multisigConfigs.get(locals.slotIdx);
        output.status = QUGATE_SUCCESS;
        output.approvalBitmap = locals.cfg.approvalBitmap;
        output.approvalCount = locals.cfg.approvalCount;
        output.required = locals.cfg.required;
        output.guardianCount = locals.cfg.guardianCount;
        output.proposalEpoch = locals.cfg.proposalEpoch;
        output.proposalActive = locals.cfg.proposalActive;
        for (locals.i = 0; locals.i < locals.cfg.guardianCount; locals.i++)
        {
            output.guardians.set(locals.i, locals.cfg.guardians.get(locals.i));
        }
    }

    // =============================================
    // configureTimeLock — set unlock epoch on a TIME_LOCK gate
    // =============================================

    PUBLIC_PROCEDURE_WITH_LOCALS(configureTimeLock)
    {
        locals.invReward = qpi.invocationReward();
        output.status = QUGATE_SUCCESS;

        locals.logger._contractIndex = CONTRACT_INDEX;
        locals.logger.sender = qpi.invocator();
        locals.logger.gateId = input.gateId;
        locals.logger.amount = 0;

        // Refund any attached QU
        if (locals.invReward > 0)
        {
            qpi.transfer(qpi.invocator(), locals.invReward);
        }

        // Decode versioned gateId
        locals.slotIdx = input.gateId & QUGATE_GATE_ID_SLOT_MASK;
        locals.encodedGen = input.gateId >> QUGATE_GATE_ID_SLOT_BITS;
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

        // Authorization: owner OR admin gate approval
        locals.adminApprovalUsed = 0;
        if (locals.gate.owner != qpi.invocator())
        {
            uint8 adminAuth = 0;
            if (locals.gate.hasAdminGate)
            {
                locals.adminCheckSlot = locals.gate.adminGateId & QUGATE_GATE_ID_SLOT_MASK;
                if (locals.adminCheckSlot < state.get()._gateCount)
                {
                    locals.adminCheckGate = state.get()._gates.get(locals.adminCheckSlot);
                    if (locals.adminCheckGate.active && locals.adminCheckGate.mode == QUGATE_MODE_MULTISIG)
                    {
                        locals.adminCheckApproval = state.get()._adminApprovalStates.get(locals.adminCheckSlot);
                        if (locals.adminCheckApproval.active == 1)
                        {
                            if ((uint32)qpi.epoch() <= locals.adminCheckApproval.validUntilEpoch)
                            {
                                adminAuth = 1;
                                locals.adminApprovalUsed = 1;
                            }
                            else
                            {
                                locals.adminCheckApproval.active = 0;
                                locals.adminCheckApproval.validUntilEpoch = 0;
                                state.mut()._adminApprovalStates.set(locals.adminCheckSlot, locals.adminCheckApproval);
                            }
                        }
                    }
                }
            }
            if (adminAuth == 0)
            {
                output.status = QUGATE_UNAUTHORIZED;
                locals.logger._type = QUGATE_LOG_FAIL_UNAUTHORIZED;
                LOG_WARNING(locals.logger);
                return;
            }
        }

        // Gate must be active
        if (locals.gate.active == 0)
        {
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Gate must be TIME_LOCK mode
        if (locals.gate.mode != QUGATE_MODE_TIME_LOCK)
        {
            output.status = QUGATE_INVALID_MODE;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // unlockEpoch must be in the future
        if (input.lockMode == QUGATE_TIME_LOCK_ABSOLUTE_EPOCH)
        {
            if (input.unlockEpoch <= (uint32)qpi.epoch())
            {
                output.status = QUGATE_TIME_LOCK_EPOCH_PAST;
                locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                LOG_WARNING(locals.logger);
                return;
            }
        }
        else if (input.lockMode == QUGATE_TIME_LOCK_RELATIVE_EPOCHS)
        {
            if (input.delayEpochs == 0)
            {
                output.status = QUGATE_INVALID_PARAMS;
                locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                LOG_WARNING(locals.logger);
                return;
            }
        }
        else
        {
            output.status = QUGATE_INVALID_PARAMS;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Store config
        if (input.lockMode == QUGATE_TIME_LOCK_ABSOLUTE_EPOCH)
        {
            locals.cfg.unlockEpoch = input.unlockEpoch;
        }
        else
        {
            locals.cfg.unlockEpoch = 0;
        }
        locals.cfg.delayEpochs = input.delayEpochs;
        locals.cfg.lockMode = input.lockMode;
        locals.cfg.cancellable = input.cancellable;
        locals.cfg.fired = 0;
        locals.cfg.cancelled = 0;
        locals.cfg.active = 1;
        state.mut()._timeLockConfigs.set(locals.slotIdx, locals.cfg);

        // Update gate activity
        locals.gate.lastActivityEpoch = qpi.epoch();
        state.mut()._gates.set(locals.slotIdx, locals.gate);

        if (locals.adminApprovalUsed == 1)
        {
            locals.adminCheckApproval = state.get()._adminApprovalStates.get(locals.adminCheckSlot);
            locals.adminCheckApproval.active = 0;
            locals.adminCheckApproval.validUntilEpoch = 0;
            state.mut()._adminApprovalStates.set(locals.adminCheckSlot, locals.adminCheckApproval);
            locals.logger._type = QUGATE_LOG_ADMIN_APPROVAL_USED;
            locals.logger.amount = locals.gate.adminGateId;
            LOG_INFO(locals.logger);
        }

        locals.logger._type = QUGATE_LOG_TIME_LOCK_CONFIGURED;
        LOG_INFO(locals.logger);
    }

    // =============================================
    // cancelTimeLock — cancel a TIME_LOCK gate (owner-only, cancellable=1 only)
    // =============================================

    PUBLIC_PROCEDURE_WITH_LOCALS(cancelTimeLock)
    {
        locals.invReward = qpi.invocationReward();
        output.status = QUGATE_SUCCESS;

        locals.logger._contractIndex = CONTRACT_INDEX;
        locals.logger.sender = qpi.invocator();
        locals.logger.gateId = input.gateId;
        locals.logger.amount = 0;

        // Refund any attached QU
        if (locals.invReward > 0)
        {
            qpi.transfer(qpi.invocator(), locals.invReward);
        }

        // Decode versioned gateId
        locals.slotIdx = input.gateId & QUGATE_GATE_ID_SLOT_MASK;
        locals.encodedGen = input.gateId >> QUGATE_GATE_ID_SLOT_BITS;
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

        // Authorization: owner OR admin gate approval
        locals.adminApprovalUsed = 0;
        if (locals.gate.owner != qpi.invocator())
        {
            uint8 adminAuth = 0;
            if (locals.gate.hasAdminGate)
            {
                locals.adminCheckSlot = locals.gate.adminGateId & QUGATE_GATE_ID_SLOT_MASK;
                if (locals.adminCheckSlot < state.get()._gateCount)
                {
                    locals.adminCheckGate = state.get()._gates.get(locals.adminCheckSlot);
                    if (locals.adminCheckGate.active && locals.adminCheckGate.mode == QUGATE_MODE_MULTISIG)
                    {
                        locals.adminCheckApproval = state.get()._adminApprovalStates.get(locals.adminCheckSlot);
                        if (locals.adminCheckApproval.active == 1)
                        {
                            if ((uint32)qpi.epoch() <= locals.adminCheckApproval.validUntilEpoch)
                            {
                                adminAuth = 1;
                                locals.adminApprovalUsed = 1;
                            }
                            else
                            {
                                locals.adminCheckApproval.active = 0;
                                locals.adminCheckApproval.validUntilEpoch = 0;
                                state.mut()._adminApprovalStates.set(locals.adminCheckSlot, locals.adminCheckApproval);
                            }
                        }
                    }
                }
            }
            if (adminAuth == 0)
            {
                output.status = QUGATE_UNAUTHORIZED;
                locals.logger._type = QUGATE_LOG_FAIL_UNAUTHORIZED;
                LOG_WARNING(locals.logger);
                return;
            }
        }

        // Gate must be active
        if (locals.gate.active == 0)
        {
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Gate must be TIME_LOCK mode
        if (locals.gate.mode != QUGATE_MODE_TIME_LOCK)
        {
            output.status = QUGATE_INVALID_MODE;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.cfg = state.get()._timeLockConfigs.get(locals.slotIdx);

        // Config must be active
        if (locals.cfg.active == 0)
        {
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Cannot cancel if already fired
        if (locals.cfg.fired == 1)
        {
            output.status = QUGATE_TIME_LOCK_ALREADY_FIRED;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Cannot cancel if already cancelled
        if (locals.cfg.cancelled == 1)
        {
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Must be cancellable
        if (locals.cfg.cancellable == 0)
        {
            output.status = QUGATE_TIME_LOCK_NOT_CANCELLABLE;
            locals.logger._type = QUGATE_LOG_FAIL_UNAUTHORIZED;
            LOG_WARNING(locals.logger);
            return;
        }

        // Refund held balance to owner
        if (locals.gate.currentBalance > 0)
        {
            if (qpi.transfer(locals.gate.owner, locals.gate.currentBalance) >= 0) // [QG-18]
            {
                locals.gate.currentBalance = 0;
            }
        }

        // Mark cancelled
        locals.cfg.cancelled = 1;
        state.mut()._timeLockConfigs.set(locals.slotIdx, locals.cfg);

        // Close the gate
        locals.gate.active = 0;
        state.mut()._gates.set(locals.slotIdx, locals.gate);
        state.mut()._activeGates -= 1;

        state.mut()._freeSlots.set(state.get()._freeCount, locals.slotIdx);
        state.mut()._freeCount += 1;
        state.mut()._gateGenerations.set(locals.slotIdx, state.get()._gateGenerations.get(locals.slotIdx) + 1);

        if (locals.adminApprovalUsed == 1)
        {
            locals.adminCheckApproval = state.get()._adminApprovalStates.get(locals.adminCheckSlot);
            locals.adminCheckApproval.active = 0;
            locals.adminCheckApproval.validUntilEpoch = 0;
            state.mut()._adminApprovalStates.set(locals.adminCheckSlot, locals.adminCheckApproval);
            locals.logger._type = QUGATE_LOG_ADMIN_APPROVAL_USED;
            locals.logger.amount = locals.gate.adminGateId;
            LOG_INFO(locals.logger);
        }

        locals.logger._type = QUGATE_LOG_TIME_LOCK_CANCELLED;
        LOG_INFO(locals.logger);
    }

    // =============================================
    // getTimeLockState — read-only query for time lock state
    // =============================================

    PUBLIC_FUNCTION_WITH_LOCALS(getTimeLockState)
    {
        output.status = QUGATE_INVALID_GATE_ID;
        output.unlockEpoch = 0;
        output.delayEpochs = 0;
        output.lockMode = QUGATE_TIME_LOCK_ABSOLUTE_EPOCH;
        output.cancellable = 0;
        output.fired = 0;
        output.cancelled = 0;
        output.active = 0;
        output.currentBalance = 0;
        output.currentEpoch = 0;
        output.epochsRemaining = 0;

        // Decode versioned gateId
        locals.slotIdx = input.gateId & QUGATE_GATE_ID_SLOT_MASK;
        locals.encodedGen = input.gateId >> QUGATE_GATE_ID_SLOT_BITS;
        if (input.gateId == 0
            || locals.slotIdx >= state.get()._gateCount
            || locals.encodedGen == 0
            || state.get()._gateGenerations.get(locals.slotIdx) != (uint16)(locals.encodedGen - 1))
        {
            return;
        }

        locals.gate = state.get()._gates.get(locals.slotIdx);
        if (locals.gate.mode != QUGATE_MODE_TIME_LOCK)
        {
            output.status = QUGATE_INVALID_MODE;
            return;
        }

        locals.cfg = state.get()._timeLockConfigs.get(locals.slotIdx);
        output.status = QUGATE_SUCCESS;
        output.unlockEpoch = locals.cfg.unlockEpoch;
        output.delayEpochs = locals.cfg.delayEpochs;
        output.lockMode = locals.cfg.lockMode;
        output.cancellable = locals.cfg.cancellable;
        output.fired = locals.cfg.fired;
        output.cancelled = locals.cfg.cancelled;
        output.active = locals.cfg.active;
        output.currentBalance = (sint64)locals.gate.currentBalance;
        output.currentEpoch = (uint32)qpi.epoch();
        if (locals.cfg.fired == 1 || locals.cfg.unlockEpoch == 0 || (uint32)qpi.epoch() >= locals.cfg.unlockEpoch)
        {
            output.epochsRemaining = 0;
        }
        else
        {
            output.epochsRemaining = locals.cfg.unlockEpoch - (uint32)qpi.epoch();
        }
    }

    // =============================================
    // setAdminGate procedure
    // =============================================

    PUBLIC_PROCEDURE_WITH_LOCALS(setAdminGate)
    {
        locals.invReward = qpi.invocationReward();
        output.status = QUGATE_SUCCESS;

        locals.logger._contractIndex = CONTRACT_INDEX;
        locals.logger.sender = qpi.invocator();
        locals.logger.gateId = input.gateId;
        locals.logger.amount = 0;

        // Refund any attached QU
        if (locals.invReward > 0)
        {
            qpi.transfer(qpi.invocator(), locals.invReward);
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

        // Gate must be active
        if (locals.gate.active == 0)
        {
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Authorization
        locals.adminApprovalUsed = 0;
        if (qpi.invocator() != locals.gate.owner)
        {
            // Non-owner: always needs admin gate approval
            if (!locals.gate.hasAdminGate)
            {
                output.status = QUGATE_UNAUTHORIZED;
                locals.logger._type = QUGATE_LOG_FAIL_UNAUTHORIZED;
                LOG_WARNING(locals.logger);
                return;
            }
            // Check admin gate approval
            locals.adminSlot = locals.gate.adminGateId & QUGATE_GATE_ID_SLOT_MASK;
            locals.adminApprovalSlot = locals.adminSlot;
            locals.adminApproved = 0;
            locals.adminApprovalUsed = 0;
            if (locals.adminSlot < state.get()._gateCount)
            {
                locals.adminGate = state.get()._gates.get(locals.adminSlot);
                if (locals.adminGate.active && locals.adminGate.mode == QUGATE_MODE_MULTISIG)
                {
                    locals.adminApproval = state.get()._adminApprovalStates.get(locals.adminSlot);
                    if (locals.adminApproval.active == 1)
                    {
                        if ((uint32)qpi.epoch() <= locals.adminApproval.validUntilEpoch)
                        {
                            locals.adminApproved = 1;
                            locals.adminApprovalUsed = 1;
                        }
                        else
                        {
                            locals.adminApproval.active = 0;
                            locals.adminApproval.validUntilEpoch = 0;
                            state.mut()._adminApprovalStates.set(locals.adminSlot, locals.adminApproval);
                        }
                    }
                }
            }
            if (locals.adminApproved == 0)
            {
                output.status = QUGATE_ADMIN_GATE_REQUIRED;
                locals.logger._type = QUGATE_LOG_FAIL_UNAUTHORIZED;
                LOG_WARNING(locals.logger);
                return;
            }
        }
        // Owner always authorized (can set or clear admin gate)

        // Clear admin gate
        if (input.adminGateId == -1)
        {
            locals.gate.adminGateId = -1;
            locals.gate.hasAdminGate = 0;
            state.mut()._gates.set(locals.slotIdx, locals.gate);
            if (locals.adminApprovalUsed == 1)
            {
                locals.adminApproval = state.get()._adminApprovalStates.get(locals.adminApprovalSlot);
                locals.adminApproval.active = 0;
                locals.adminApproval.validUntilEpoch = 0;
                state.mut()._adminApprovalStates.set(locals.adminApprovalSlot, locals.adminApproval);
                locals.logger._type = QUGATE_LOG_ADMIN_APPROVAL_USED;
                locals.logger.amount = locals.gate.adminGateId;
                LOG_INFO(locals.logger);
            }
            output.status = QUGATE_SUCCESS;
            locals.logger._type = QUGATE_LOG_ADMIN_GATE_CLEARED;
            LOG_INFO(locals.logger);
            return;
        }

        // Validate adminGateId points to an active MULTISIG gate
        locals.adminSlot = (uint64)input.adminGateId & QUGATE_GATE_ID_SLOT_MASK;
        locals.adminEncodedGen = (uint64)input.adminGateId >> QUGATE_GATE_ID_SLOT_BITS;
        if (input.adminGateId <= 0
            || locals.adminSlot >= state.get()._gateCount
            || locals.adminEncodedGen == 0
            || state.get()._gateGenerations.get(locals.adminSlot) != (uint16)(locals.adminEncodedGen - 1))
        {
            output.status = QUGATE_INVALID_ADMIN_GATE;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.adminGate = state.get()._gates.get(locals.adminSlot);
        if (locals.adminGate.active == 0 || locals.adminGate.mode != QUGATE_MODE_MULTISIG)
        {
            output.status = QUGATE_INVALID_ADMIN_GATE;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Prevent self-referential admin gate
        if (locals.adminSlot == locals.slotIdx)
        {
            output.status = QUGATE_INVALID_ADMIN_CYCLE;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Walk admin gate chain to detect cycles (max QUGATE_MAX_CHAIN_DEPTH steps)
        locals.walkSlot = locals.adminSlot;
        for (locals.walkStep = 0; locals.walkStep < QUGATE_MAX_CHAIN_DEPTH; locals.walkStep++)
        {
            locals.walkGate = state.get()._gates.get(locals.walkSlot);
            if (locals.walkGate.adminGateId == -1 || locals.walkGate.hasAdminGate == 0)
            {
                break;
            }
            uint64 nextAdminSlot = (uint64)(locals.walkGate.adminGateId) & QUGATE_GATE_ID_SLOT_MASK;
            if (nextAdminSlot == locals.slotIdx)
            {
                output.status = QUGATE_INVALID_ADMIN_CYCLE;
                locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                LOG_WARNING(locals.logger);
                return;
            }
            if (nextAdminSlot >= state.get()._gateCount)
            {
                break;
            }
            locals.walkSlot = nextAdminSlot;
        }

        // Set admin gate
        locals.gate.adminGateId = input.adminGateId;
        locals.gate.hasAdminGate = 1;
        state.mut()._gates.set(locals.slotIdx, locals.gate);

        if (locals.adminApprovalUsed == 1)
        {
            locals.adminApproval = state.get()._adminApprovalStates.get(locals.adminApprovalSlot);
            locals.adminApproval.active = 0;
            locals.adminApproval.validUntilEpoch = 0;
            state.mut()._adminApprovalStates.set(locals.adminApprovalSlot, locals.adminApproval);
            locals.logger._type = QUGATE_LOG_ADMIN_APPROVAL_USED;
            locals.logger.amount = locals.gate.adminGateId;
            LOG_INFO(locals.logger);
        }

        output.status = QUGATE_SUCCESS;
        locals.logger._type = QUGATE_LOG_ADMIN_GATE_SET;
        LOG_INFO(locals.logger);
    }

    // =============================================
    // getAdminGate function
    // =============================================

    PUBLIC_FUNCTION_WITH_LOCALS(getAdminGate)
    {
        output.hasAdminGate = 0;
        output.adminGateId = -1;
        output.adminGateMode = 0;
        output.guardianCount = 0;
        output.required = 0;
        output.adminApprovalWindowEpochs = 0;
        output.adminApprovalActive = 0;
        output.adminApprovalValidUntilEpoch = 0;

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
        output.hasAdminGate = locals.gate.hasAdminGate;
        output.adminGateId = locals.gate.adminGateId;

        // If admin gate is set, look up its mode
        if (locals.gate.hasAdminGate)
        {
            locals.adminSlot = locals.gate.adminGateId & QUGATE_GATE_ID_SLOT_MASK;
            if (locals.adminSlot < state.get()._gateCount)
            {
                locals.adminGate = state.get()._gates.get(locals.adminSlot);
                output.adminGateMode = locals.adminGate.mode;
                locals.adminCfg = state.get()._multisigConfigs.get(locals.adminSlot);
                locals.adminApproval = state.get()._adminApprovalStates.get(locals.adminSlot);
                output.guardianCount = locals.adminCfg.guardianCount;
                output.required = locals.adminCfg.required;
                output.adminApprovalWindowEpochs = locals.adminCfg.adminApprovalWindowEpochs;
                if (locals.adminApproval.active == 1
                    && (uint32)qpi.epoch() <= locals.adminApproval.validUntilEpoch)
                {
                    output.adminApprovalActive = 1;
                    output.adminApprovalValidUntilEpoch = locals.adminApproval.validUntilEpoch;
                }
                for (locals.i = 0; locals.i < locals.adminCfg.guardianCount; locals.i++)
                {
                    output.guardians.set(locals.i, locals.adminCfg.guardians.get(locals.i));
                }
            }
        }
    }

    // =============================================
    // withdrawReserve — withdraw from oracleReserve or chainReserve
    // =============================================

    PUBLIC_PROCEDURE_WITH_LOCALS(withdrawReserve)
    {
        locals.invReward = qpi.invocationReward();
        output.status = QUGATE_SUCCESS;
        output.withdrawn = 0;

        locals.logger._contractIndex = CONTRACT_INDEX;
        locals.logger.sender = qpi.invocator();
        locals.logger.gateId = input.gateId;
        locals.logger.amount = 0;

        // Refund any attached QU
        if (locals.invReward > 0)
        {
            qpi.transfer(qpi.invocator(), locals.invReward);
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

        // Authorization: owner OR admin gate approval
        locals.adminApprovalUsed = 0;
        if (locals.gate.owner != qpi.invocator())
        {
            uint8 adminAuth = 0;
            if (locals.gate.hasAdminGate)
            {
                locals.adminCheckSlot = locals.gate.adminGateId & QUGATE_GATE_ID_SLOT_MASK;
                if (locals.adminCheckSlot < state.get()._gateCount)
                {
                    locals.adminCheckGate = state.get()._gates.get(locals.adminCheckSlot);
                    if (locals.adminCheckGate.active && locals.adminCheckGate.mode == QUGATE_MODE_MULTISIG)
                    {
                        locals.adminCheckApproval = state.get()._adminApprovalStates.get(locals.adminCheckSlot);
                        if (locals.adminCheckApproval.active == 1)
                        {
                            if ((uint32)qpi.epoch() <= locals.adminCheckApproval.validUntilEpoch)
                            {
                                adminAuth = 1;
                                locals.adminApprovalUsed = 1;
                            }
                            else
                            {
                                locals.adminCheckApproval.active = 0;
                                locals.adminCheckApproval.validUntilEpoch = 0;
                                state.mut()._adminApprovalStates.set(locals.adminCheckSlot, locals.adminCheckApproval);
                            }
                        }
                    }
                }
            }
            if (adminAuth == 0)
            {
                output.status = QUGATE_UNAUTHORIZED;
                locals.logger._type = QUGATE_LOG_FAIL_UNAUTHORIZED;
                LOG_WARNING(locals.logger);
                return;
            }
        }

        // Gate must be active
        if (locals.gate.active == 0)
        {
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        if (input.reserveTarget == 0)
        {
            // oracleReserve — gate must be ORACLE mode
            if (locals.gate.mode != QUGATE_MODE_ORACLE)
            {
                output.status = QUGATE_INVALID_MODE;
                locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                LOG_WARNING(locals.logger);
                return;
            }

            sint64 available = locals.gate.oracleReserve;
            if (available <= 0)
            {
                output.status = QUGATE_SUCCESS;
                output.withdrawn = 0;
                return;
            }

            uint64 toWithdraw = (uint64)available;
            if (input.amount != 0 && input.amount < toWithdraw)
            {
                toWithdraw = input.amount;
            }

            if (qpi.transfer(qpi.invocator(), toWithdraw) >= 0) // [QG-19]
            {
                locals.gate.oracleReserve -= (sint64)toWithdraw;
                state.mut()._gates.set(locals.slotIdx, locals.gate);
                output.withdrawn = toWithdraw;
            }
        }
        else if (input.reserveTarget == 1)
        {
            // chainReserve — gate must have a chain link
            if (locals.gate.chainNextGateId == -1)
            {
                output.status = QUGATE_INVALID_CHAIN;
                locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                LOG_WARNING(locals.logger);
                return;
            }

            sint64 available = locals.gate.chainReserve;
            if (available <= 0)
            {
                output.status = QUGATE_SUCCESS;
                output.withdrawn = 0;
                return;
            }

            uint64 toWithdraw = (uint64)available;
            if (input.amount != 0 && input.amount < toWithdraw)
            {
                toWithdraw = input.amount;
            }

            if (qpi.transfer(qpi.invocator(), toWithdraw) >= 0) // [QG-20]
            {
                locals.gate.chainReserve -= (sint64)toWithdraw;
                state.mut()._gates.set(locals.slotIdx, locals.gate);
                output.withdrawn = toWithdraw;
            }
        }
        else
        {
            output.status = QUGATE_INVALID_MODE;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        if (locals.adminApprovalUsed == 1)
        {
            locals.adminCheckApproval = state.get()._adminApprovalStates.get(locals.adminCheckSlot);
            locals.adminCheckApproval.active = 0;
            locals.adminCheckApproval.validUntilEpoch = 0;
            state.mut()._adminApprovalStates.set(locals.adminCheckSlot, locals.adminCheckApproval);
            locals.logger._type = QUGATE_LOG_ADMIN_APPROVAL_USED;
            locals.logger.amount = locals.gate.adminGateId;
            LOG_INFO(locals.logger);
        }

        locals.logger._type = QUGATE_LOG_PAYMENT_FORWARDED;
        locals.logger.amount = (sint64)output.withdrawn;
        LOG_INFO(locals.logger);
    }

    // =============================================
    // getGatesByMode — query active gates by mode
    // =============================================

    PUBLIC_FUNCTION_WITH_LOCALS(getGatesByMode)
    {
        output.count = 0;
        for (locals.i = 0; locals.i < state.get()._gateCount && output.count < QUGATE_MAX_OWNER_GATES; locals.i++)
        {
            if (state.get()._gates.get(locals.i).active == 1
                && state.get()._gates.get(locals.i).mode == input.mode)
            {
                output.gateIds.set(output.count,
                    ((uint64)(state.get()._gateGenerations.get(locals.i) + 1) << QUGATE_GATE_ID_SLOT_BITS) | locals.i);
                output.count += 1;
            }
        }
    }

    // getGateBySlot — read gate data by raw slot index, no generation check
    PUBLIC_FUNCTION_WITH_LOCALS(getGateBySlot)
    {
        output.valid = 0;
        if (input.slotIndex >= state.get()._gateCount)
        {
            return;
        }

        output.valid = 1;
        output.generation = state.get()._gateGenerations.get(input.slotIndex);
        output.gateId = ((uint64)(output.generation + 1) << QUGATE_GATE_ID_SLOT_BITS) | input.slotIndex;

        locals.gate = state.get()._gates.get(input.slotIndex);
        output.mode = locals.gate.mode;
        output.recipientCount = locals.gate.recipientCount;
        output.active = locals.gate.active;
        output.owner = locals.gate.owner;
        output.totalReceived = locals.gate.totalReceived;
        output.totalForwarded = locals.gate.totalForwarded;
        output.currentBalance = locals.gate.currentBalance;
        output.threshold = locals.gate.threshold;
        output.createdEpoch = locals.gate.createdEpoch;
        output.lastActivityEpoch = locals.gate.lastActivityEpoch;
        output.allowedSenderCount = locals.gate.allowedSenderCount;
        output.oracleReserve = locals.gate.oracleReserve;
        output.oracleSubscriptionId = locals.gate.oracleSubscriptionId;
        output.chainNextGateId = locals.gate.chainNextGateId;
        output.chainReserve = locals.gate.chainReserve;
        output.chainDepth = locals.gate.chainDepth;
        output.adminGateId = locals.gate.adminGateId;
        output.hasAdminGate = locals.gate.hasAdminGate;

        for (locals.i = 0; locals.i < 8; locals.i++)
        {
            output.recipients.set(locals.i, locals.gate.recipients.get(locals.i));
            output.ratios.set(locals.i, locals.gate.ratios.get(locals.i));
            output.allowedSenders.set(locals.i, locals.gate.allowedSenders.get(locals.i));
            output.recipientGateIds.set(locals.i, locals.gate.recipientGateIds.get(locals.i));
        }
    }

    PUBLIC_FUNCTION_WITH_LOCALS(getLatestExecution)
    {
        output.valid = 0;
        output.mode = 0;
        output.outcomeType = QUGATE_EXEC_NONE;
        output.selectedRecipientIndex = 255;
        output.selectedDownstreamGateId = -1;
        output.forwardedAmount = 0;
        output.observedTick = 0;

        locals.slotIdx = input.gateId & QUGATE_GATE_ID_SLOT_MASK;
        locals.encodedGen = (input.gateId >> QUGATE_GATE_ID_SLOT_BITS);
        if (input.gateId == 0
            || locals.slotIdx >= state.get()._gateCount
            || locals.encodedGen == 0
            || state.get()._gateGenerations.get(locals.slotIdx) != (uint16)(locals.encodedGen - 1))
        {
            return;
        }

        locals.latestExecution = state.get()._latestExecutions.get(locals.slotIdx);
        output.valid = locals.latestExecution.valid;
        output.mode = locals.latestExecution.mode;
        output.outcomeType = locals.latestExecution.outcomeType;
        output.selectedRecipientIndex = locals.latestExecution.selectedRecipientIndex;
        output.selectedDownstreamGateId = locals.latestExecution.selectedDownstreamGateId;
        output.forwardedAmount = locals.latestExecution.forwardedAmount;
        output.observedTick = locals.latestExecution.observedTick;
    }

    // =============================================
    // Registration
    // =============================================

    REGISTER_USER_FUNCTIONS_AND_PROCEDURES()
    {
        // Index assignments: 1=createGate 2=sendToGate 3=closeGate 4=updateGate 5=getGate 6=getGateCount 7=getGatesByOwner 8=getGateBatch 9=getFees 10=fundGate 11=setChain 12=sendToGateVerified 13=configureHeartbeat 14=heartbeat 15=getHeartbeat 16=configureMultisig 17=getMultisigState 18=configureTimeLock 19=cancelTimeLock 20=getTimeLockState 21=setAdminGate 22=getAdminGate 23=withdrawReserve 24=getGatesByMode 25=getGateBySlot 26=getLatestExecution
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
        REGISTER_USER_PROCEDURE(configureHeartbeat, 13);
        REGISTER_USER_PROCEDURE(heartbeat, 14);
        REGISTER_USER_FUNCTION(getHeartbeat, 15);
        REGISTER_USER_PROCEDURE(configureMultisig, 16);
        REGISTER_USER_FUNCTION(getMultisigState, 17);
        REGISTER_USER_PROCEDURE(configureTimeLock, 18);
        REGISTER_USER_PROCEDURE(cancelTimeLock, 19);
        REGISTER_USER_FUNCTION(getTimeLockState, 20);
        REGISTER_USER_PROCEDURE(setAdminGate, 21);
        REGISTER_USER_FUNCTION(getAdminGate, 22);
        REGISTER_USER_PROCEDURE(withdrawReserve, 23);
        REGISTER_USER_FUNCTION(getGatesByMode, 24);
        REGISTER_USER_FUNCTION(getGateBySlot, 25);
        REGISTER_USER_FUNCTION(getLatestExecution, 26);
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
                // Exempt long-duration modes from inactivity expiry (#77)
                if (locals.gate.mode == QUGATE_MODE_TIME_LOCK)
                {
                    locals.tlCfg = state.get()._timeLockConfigs.get(locals.i);
                    if (locals.tlCfg.active == 1 && locals.tlCfg.fired == 0 && locals.tlCfg.cancelled == 0)
                    {
                        continue;
                    }
                }
                if (locals.gate.mode == QUGATE_MODE_HEARTBEAT)
                {
                    locals.inhCfg = state.get()._heartbeatConfigs.get(locals.i);
                    if (locals.inhCfg.active == 1 && locals.inhCfg.triggered == 0)
                    {
                        continue;
                    }
                }
                if (locals.gate.mode == QUGATE_MODE_MULTISIG && locals.gate.currentBalance > 0)
                {
                    continue;
                }

                if (qpi.epoch() - locals.gate.lastActivityEpoch >= state.get()._expiryEpochs)
                {
                    // Refund any held balance (THRESHOLD / ORACLE mode)
                    if (locals.gate.currentBalance > 0)
                    {
                        if (qpi.transfer(locals.gate.owner, locals.gate.currentBalance) >= 0) // [QG-14]
                        {
                            locals.gate.currentBalance = 0;
                        }
                    }

                    // Refund oracle reserve on expiry
                    if (locals.gate.mode == QUGATE_MODE_ORACLE)
                    {
                        if (locals.gate.oracleReserve > 0)
                        {
                            if (qpi.transfer(locals.gate.owner, locals.gate.oracleReserve) >= 0) // [QG-14]
                            {
                                locals.gate.oracleReserve = 0;
                            }
                        }
                        if (locals.gate.oracleSubscriptionId >= 0)
                        {
                            state.mut()._subscriptionToSlot.removeByKey(locals.gate.oracleSubscriptionId);
                        }
                    }

                    // Refund chain reserve on expiry
                    if (locals.gate.chainReserve > 0)
                    {
                        if (qpi.transfer(locals.gate.owner, locals.gate.chainReserve) >= 0) // [QG-14]
                        {
                            locals.gate.chainReserve = 0;
                        }
                    }

                    // Clear mode-specific configs on expiry (prevents ghost state in recycled slots)
                    if (locals.gate.mode == QUGATE_MODE_HEARTBEAT)
                    {
                        QUGATE_HeartbeatConfig zeroCfg;
                        zeroCfg.active = 0; zeroCfg.triggered = 0; zeroCfg.thresholdEpochs = 0;
                        zeroCfg.lastHeartbeatEpoch = 0; zeroCfg.payoutPercentPerEpoch = 0;
                        zeroCfg.minimumBalance = 0; zeroCfg.triggerEpoch = 0; zeroCfg.beneficiaryCount = 0;
                        for (uint8 _zi = 0; _zi < 8; _zi++)
                        {
                            zeroCfg.beneficiaryAddresses.set(_zi, id::zero());
                            zeroCfg.beneficiaryShares.set(_zi, 0);
                        }
                        state.mut()._heartbeatConfigs.set(locals.i, zeroCfg);
                    }
                    else if (locals.gate.mode == QUGATE_MODE_MULTISIG)
                    {
                        QUGATE_MultisigConfig zeroCfg;
                        for (uint8 _zi = 0; _zi < 8; _zi++)
                        {
                            zeroCfg.guardians.set(_zi, id::zero());
                        }
                        zeroCfg.guardianCount = 0; zeroCfg.required = 0; zeroCfg.proposalExpiryEpochs = 0; zeroCfg.adminApprovalWindowEpochs = 0;
                        zeroCfg.approvalBitmap = 0; zeroCfg.approvalCount = 0; zeroCfg.proposalEpoch = 0; zeroCfg.proposalActive = 0;
                        state.mut()._multisigConfigs.set(locals.i, zeroCfg);
                        QUGATE_AdminApprovalState zeroAdminApproval;
                        zeroAdminApproval.active = 0; zeroAdminApproval.validUntilEpoch = 0;
                        state.mut()._adminApprovalStates.set(locals.i, zeroAdminApproval);
                    }
                    else if (locals.gate.mode == QUGATE_MODE_TIME_LOCK)
                    {
                        QUGATE_TimeLockConfig zeroCfg;
                        zeroCfg.unlockEpoch = 0; zeroCfg.delayEpochs = 0; zeroCfg.lockMode = QUGATE_TIME_LOCK_ABSOLUTE_EPOCH; zeroCfg.cancellable = 0; zeroCfg.fired = 0; zeroCfg.cancelled = 0; zeroCfg.active = 0;
                        state.mut()._timeLockConfigs.set(locals.i, zeroCfg);
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
        // Heartbeat gate processing — epoch-based trigger
        // =============================================
        for (locals.i = 0; locals.i < state.get()._gateCount; locals.i++)
        {
            locals.gate = state.get()._gates.get(locals.i);

            // Skip non-HEARTBEAT gates and inactive gates
            if (locals.gate.active == 0 || locals.gate.mode != QUGATE_MODE_HEARTBEAT)
            {
                continue;
            }

            locals.inhCfg = state.get()._heartbeatConfigs.get(locals.i);

            // Skip if heartbeat not yet configured
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
                    locals.inhPayoutTotal = (sint64)QPI::div((uint64)(locals.inhBalance * (sint64)locals.inhCfg.payoutPercentPerEpoch), (uint64)100);

                    // Zeno fix: if payout rounds to 0 but balance remains, sweep remaining balance
                    if (locals.inhPayoutTotal == 0 && locals.inhBalance > 0)
                    {
                        locals.inhPayoutTotal = locals.inhBalance;
                    }

                    if (locals.inhPayoutTotal > 0)
                    {
                        locals.inhBeneCount = locals.inhCfg.beneficiaryCount;

                        for (locals.inhJ = 0; locals.inhJ < locals.inhBeneCount; locals.inhJ++)
                        {
                            if (locals.inhJ == locals.inhBeneCount - 1)
                            {
                                // Last beneficiary gets remainder to avoid dust from rounding
                                locals.inhPriorSum = 0;
                                for (locals.inhK = 0; locals.inhK < locals.inhJ; locals.inhK++)
                                {
                                    locals.inhPriorSum += (sint64)QPI::div((uint64)(locals.inhPayoutTotal * (sint64)locals.inhCfg.beneficiaryShares.get(locals.inhK)), (uint64)100);
                                }
                                locals.inhShare = locals.inhPayoutTotal - locals.inhPriorSum;
                            }
                            else
                            {
                                locals.inhShare = (sint64)QPI::div((uint64)(locals.inhPayoutTotal * (sint64)locals.inhCfg.beneficiaryShares.get(locals.inhJ)), (uint64)100);
                            }

                            if (locals.inhShare > 0)
                            {
                                if (qpi.transfer(locals.inhCfg.beneficiaryAddresses.get(locals.inhJ), locals.inhShare) >= 0) // [QG-15]
                                {
                                    locals.gate.totalForwarded += (uint64)locals.inhShare;
                                    locals.gate.currentBalance -= (uint64)locals.inhShare;
                                }
                            }
                        }

                        state.mut()._gates.set(locals.i, locals.gate);

                        // Log payout
                        locals.logger._contractIndex = CONTRACT_INDEX;
                        locals.logger._type = QUGATE_LOG_HEARTBEAT_PAYOUT;
                        locals.logger.gateId = ((uint64)(state.get()._gateGenerations.get(locals.i) + 1) << QUGATE_GATE_ID_SLOT_BITS) | locals.i;
                        locals.logger.sender = locals.gate.owner;
                        locals.logger.amount = locals.inhPayoutTotal;
                        LOG_INFO(locals.logger);
                    }

                    // Chain forwarding after heartbeat payout
                    locals.gate = state.get()._gates.get(locals.i);
                    if (locals.gate.chainNextGateId != -1 && locals.inhPayoutTotal > 0)
                    {
                        sint64 chainAmount = locals.inhPayoutTotal;
                        sint64 currentChainGateId = locals.gate.chainNextGateId;
                        uint8 hop = 0;
                        while (hop < QUGATE_MAX_CHAIN_DEPTH && currentChainGateId != -1 && chainAmount > 0)
                        {
                            uint64 nextSlot = (uint64)currentChainGateId & QUGATE_GATE_ID_SLOT_MASK;
                            uint64 nextGen = (uint64)currentChainGateId >> QUGATE_GATE_ID_SLOT_BITS;
                            if (nextSlot >= state.get()._gateCount || nextGen == 0
                                || state.get()._gateGenerations.get(nextSlot) != (uint16)(nextGen - 1))
                                break;
                            locals.rtIn.slotIdx = nextSlot; locals.rtIn.amount = chainAmount; locals.rtIn.hopCount = hop;
                            routeToGate(qpi, state, locals.rtIn, locals.rtOut, locals.rtLocals);
                            chainAmount = locals.rtOut.forwarded;
                            // Dispatch deferred gate-as-recipient routing from chain hop
                            locals.savedDeferredCount = locals.rtOut.deferredCount;
                            locals.savedDeferredHopCount = locals.rtOut.deferredHopCount;
                            for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                            {
                                locals.savedDeferredSlots.set(locals.deferredDispatchIdx, locals.rtOut.deferredGateSlots.get(locals.deferredDispatchIdx));
                                locals.savedDeferredAmounts.set(locals.deferredDispatchIdx, locals.rtOut.deferredGateAmounts.get(locals.deferredDispatchIdx));
                            }
                            for (locals.deferredDispatchIdx = 0; locals.deferredDispatchIdx < locals.savedDeferredCount; locals.deferredDispatchIdx++)
                            {
                                locals.rtIn.slotIdx = locals.savedDeferredSlots.get(locals.deferredDispatchIdx);
                                locals.rtIn.amount = locals.savedDeferredAmounts.get(locals.deferredDispatchIdx);
                                locals.rtIn.hopCount = locals.savedDeferredHopCount;
                                routeToGate(qpi, state, locals.rtIn, locals.rtOut, locals.rtLocals);
                            }
                            GateConfig nextGate = state.get()._gates.get(nextSlot);
                            currentChainGateId = nextGate.chainNextGateId;
                            hop++;
                        }
                        // Failsafe: if chain forwarding couldn't deliver, revert to currentBalance
                        if (chainAmount > 0)
                        {
                            locals.gate = state.get()._gates.get(locals.i);
                            locals.gate.currentBalance += (uint64)chainAmount;
                            locals.gate.totalForwarded -= (uint64)chainAmount; // undo the premature forward count
                            state.mut()._gates.set(locals.i, locals.gate);
                            locals.logger._type = QUGATE_LOG_CHAIN_HOP_INSUFFICIENT;
                            locals.logger.gateId = ((uint64)(state.get()._gateGenerations.get(locals.i) + 1) << QUGATE_GATE_ID_SLOT_BITS) | locals.i;
                            locals.logger.amount = chainAmount;
                            LOG_INFO(locals.logger);
                        }
                    }

                    // Auto-close gate if balance dropped to or below minimum
                    locals.gate = state.get()._gates.get(locals.i);
                    if ((sint64)locals.gate.currentBalance <= locals.inhCfg.minimumBalance)
                    {
                        // Distribute remaining balance to beneficiaries (owner may be deceased in inheritance use case)
                        if (locals.gate.currentBalance > 0 && locals.inhCfg.beneficiaryCount > 0)
                        {
                            sint64 dustTotal = (sint64)locals.gate.currentBalance;
                            locals.inhPriorSum = 0;
                            for (locals.inhJ = 0; locals.inhJ < locals.inhCfg.beneficiaryCount; locals.inhJ++)
                            {
                                if (locals.inhJ == locals.inhCfg.beneficiaryCount - 1)
                                {
                                    locals.inhShare = dustTotal - locals.inhPriorSum;
                                }
                                else
                                {
                                    locals.inhShare = (sint64)QPI::div((uint64)(dustTotal * (sint64)locals.inhCfg.beneficiaryShares.get(locals.inhJ)), (uint64)100);
                                    locals.inhPriorSum += locals.inhShare;
                                }
                                if (locals.inhShare > 0)
                                {
                                    if (qpi.transfer(locals.inhCfg.beneficiaryAddresses.get(locals.inhJ), locals.inhShare) >= 0) // [QG-16]
                                    {
                                        locals.gate.totalForwarded += (uint64)locals.inhShare;
                                        locals.gate.currentBalance -= (uint64)locals.inhShare;
                                    }
                                }
                            }
                        }
                        else if (locals.gate.currentBalance > 0)
                        {
                            // No beneficiaries configured — refund to owner as fallback
                            if (qpi.transfer(locals.gate.owner, locals.gate.currentBalance) >= 0)
                            {
                                locals.gate.currentBalance = 0;
                            }
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
                locals.inhEpochsInactive = (uint32)qpi.epoch() - locals.inhCfg.lastHeartbeatEpoch;
                if (locals.inhEpochsInactive > locals.inhCfg.thresholdEpochs)
                {
                    // Heartbeat triggered!
                    locals.inhCfg.triggered = 1;
                    locals.inhCfg.triggerEpoch = (uint32)qpi.epoch();
                    state.mut()._heartbeatConfigs.set(locals.i, locals.inhCfg);

                    locals.logger._contractIndex = CONTRACT_INDEX;
                    locals.logger._type = QUGATE_LOG_HEARTBEAT_TRIGGERED;
                    locals.logger.gateId = ((uint64)(state.get()._gateGenerations.get(locals.i) + 1) << QUGATE_GATE_ID_SLOT_BITS) | locals.i;
                    locals.logger.sender = locals.gate.owner;
                    locals.logger.amount = (sint64)locals.gate.currentBalance;
                    LOG_INFO(locals.logger);
                }
            }
        }

        // =============================================
        // Multisig gate processing — expire stale proposals
        // =============================================
        for (locals.i = 0; locals.i < state.get()._gateCount; locals.i++)
        {
            locals.gate = state.get()._gates.get(locals.i);

            if (locals.gate.active == 0 || locals.gate.mode != QUGATE_MODE_MULTISIG)
            {
                continue;
            }

            locals.msigCfg = state.get()._multisigConfigs.get(locals.i);

            if (locals.msigCfg.proposalActive == 1
                && (uint32)qpi.epoch() - locals.msigCfg.proposalEpoch > locals.msigCfg.proposalExpiryEpochs)
            {
                locals.msigCfg.approvalBitmap = 0;
                locals.msigCfg.approvalCount = 0;
                locals.msigCfg.proposalActive = 0;
                state.mut()._multisigConfigs.set(locals.i, locals.msigCfg);

                locals.logger._contractIndex = CONTRACT_INDEX;
                locals.logger._type = QUGATE_LOG_MULTISIG_EXPIRED;
                locals.logger.gateId = ((uint64)(state.get()._gateGenerations.get(locals.i) + 1) << QUGATE_GATE_ID_SLOT_BITS) | locals.i;
                locals.logger.sender = locals.gate.owner;
                locals.logger.amount = 0;
                LOG_INFO(locals.logger);
            }
        }

        // =============================================
        // TIME_LOCK gate processing — epoch-based fund release
        // =============================================
        for (locals.i = 0; locals.i < state.get()._gateCount; locals.i++)
        {
            locals.gate = state.get()._gates.get(locals.i);

            if (locals.gate.active == 0 || locals.gate.mode != QUGATE_MODE_TIME_LOCK)
            {
                continue;
            }

            locals.tlCfg = state.get()._timeLockConfigs.get(locals.i);

            // Only process if active and not yet fired or cancelled
            if (locals.tlCfg.active == 0 || locals.tlCfg.fired == 1 || locals.tlCfg.cancelled == 1)
            {
                continue;
            }

            // Relative mode waits for first funding to anchor unlockEpoch.
            if (locals.tlCfg.unlockEpoch == 0)
            {
                continue;
            }

            if ((uint32)qpi.epoch() >= locals.tlCfg.unlockEpoch)
            {
                // Release funds to target (recipients.get(0))
                if (locals.gate.currentBalance > 0)
                {
                    locals.tlReleaseAmount = (sint64)locals.gate.currentBalance;
                    uint8 tlTransferred = 0;
                    if (locals.gate.recipientCount > 0)
                    {
                        if (locals.gate.recipientGateIds.get(0) >= 0)
                        {
                            // Gate-as-recipient: route via routeToGate
                            uint64 targetSlot = (uint64)(locals.gate.recipientGateIds.get(0)) & QUGATE_GATE_ID_SLOT_MASK;
                            uint64 targetGen = (uint64)(locals.gate.recipientGateIds.get(0)) >> QUGATE_GATE_ID_SLOT_BITS;
                            if (targetSlot < state.get()._gateCount && targetGen > 0
                                && state.get()._gateGenerations.get(targetSlot) == (uint16)(targetGen - 1)
                                && state.get()._gates.get(targetSlot).active == 1)
                            {
                                locals.rtIn.slotIdx = targetSlot;
                                locals.rtIn.amount = locals.tlReleaseAmount;
                                locals.rtIn.hopCount = 0;
                                routeToGate(qpi, state, locals.rtIn, locals.rtOut, locals.rtLocals);
                                tlTransferred = 1;
                            }
                        }
                        else
                        {
                            if (qpi.transfer(locals.gate.recipients.get(0), locals.tlReleaseAmount) >= 0) // [QG-17]
                            {
                                tlTransferred = 1;
                            }
                        }
                    }
                    else if (locals.gate.chainNextGateId != -1)
                    {
                        tlTransferred = 1; // No recipient, chain will handle
                    }
                    // else: no recipients AND no chain — funds stay in currentBalance
                    if (tlTransferred)
                    {
                        locals.gate.totalForwarded += (uint64)locals.tlReleaseAmount;
                        locals.gate.currentBalance = 0;
                        state.mut()._gates.set(locals.i, locals.gate);
                    }
                }

                locals.tlCfg.fired = 1;
                state.mut()._timeLockConfigs.set(locals.i, locals.tlCfg);

                // Log fired
                locals.logger._contractIndex = CONTRACT_INDEX;
                locals.logger._type = QUGATE_LOG_TIME_LOCK_FIRED;
                locals.logger.gateId = ((uint64)(state.get()._gateGenerations.get(locals.i) + 1) << QUGATE_GATE_ID_SLOT_BITS) | locals.i;
                locals.logger.sender = locals.gate.owner;
                locals.logger.amount = (sint64)locals.gate.totalForwarded;
                LOG_INFO(locals.logger);

                // Close the gate
                locals.gate = state.get()._gates.get(locals.i);
                locals.gate.active = 0;
                state.mut()._gates.set(locals.i, locals.gate);
                state.mut()._activeGates -= 1;

                state.mut()._freeSlots.set(state.get()._freeCount, locals.i);
                state.mut()._freeCount += 1;
                state.mut()._gateGenerations.set(locals.i, state.get()._gateGenerations.get(locals.i) + 1);
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
