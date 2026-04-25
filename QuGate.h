// QuGate.h - QUGATE payment routing contract

using namespace QPI;

// Contract index is assigned by core.

// Capacity scales with network via X_MULTIPLIER
constexpr uint64 QUGATE_INITIAL_MAX_GATES = 2048;
constexpr uint64 QUGATE_MAX_GATES = QUGATE_INITIAL_MAX_GATES * X_MULTIPLIER;
constexpr uint64 QUGATE_MAX_RECIPIENTS = 8;
constexpr uint64 QUGATE_MAX_RATIO = 10000;       // Max ratio per recipient (prevents overflow)

// Default fees — initial values, changeable via shareholder vote
constexpr uint64 QUGATE_DEFAULT_CREATION_FEE = 100000;
constexpr uint64 QUGATE_DEFAULT_MIN_SEND = 1000;
constexpr uint64 QUGATE_DEFAULT_MAINTENANCE_FEE = 25000;
constexpr uint64 QUGATE_DEFAULT_MAINTENANCE_INTERVAL_EPOCHS = 4;
constexpr uint64 QUGATE_DEFAULT_MAINTENANCE_GRACE_EPOCHS = 4;
// Fee split defaults: burn vs shareholder dividends (governed via _feeBurnBps state variable)
constexpr uint64 QUGATE_DEFAULT_FEE_BURN_BPS = 5000;      // 50% burned (default)
constexpr uint64 QUGATE_MIN_FEE_BURN_BPS = 3000;          // floor: 30% burn minimum
constexpr uint64 QUGATE_MAX_FEE_BURN_BPS = 7000;          // ceiling: 70% burn maximum

// Complexity-based idle maintenance multipliers (basis points, 10000 = 1x)
constexpr uint64 QUGATE_IDLE_BASE_MULTIPLIER_BPS = 10000;            // 1x for simple gates
constexpr uint64 QUGATE_IDLE_MULTI_RECIPIENT_THRESHOLD = 3;          // 3+ recipients triggers multiplier
constexpr uint64 QUGATE_IDLE_MULTI_RECIPIENT_MULTIPLIER_BPS = 15000; // 1.5x for 3+ recipients
constexpr uint64 QUGATE_IDLE_MAX_RECIPIENT_MULTIPLIER_BPS = 20000;   // 2x for 8 recipients
constexpr uint64 QUGATE_IDLE_HEARTBEAT_MULTIPLIER_BPS = 15000;       // 1.5x for HEARTBEAT
constexpr uint64 QUGATE_IDLE_MULTISIG_MULTIPLIER_BPS = 15000;        // 1.5x for MULTISIG
constexpr uint64 QUGATE_IDLE_CHAIN_EXTRA_BPS = 5000;                 // +0.5x if chained
constexpr uint64 QUGATE_IDLE_SHIELD_PER_TARGET_BPS = 5000;          // +0.5x surcharge per downstream target shielded

// Escalating fee: fee = baseFee * (1 + QPI::div(activeGates, FEE_ESCALATION_STEP))
constexpr uint64 QUGATE_FEE_ESCALATION_STEP = 1024;

// Gate expiry: gates with no activity for this many epochs auto-close
constexpr uint64 QUGATE_DEFAULT_EXPIRY_EPOCHS = 50;

// Versioned gate ID encoding: gateId = ((generation+1) << SLOT_BITS) | slotIndex
constexpr uint64 QUGATE_GATE_ID_SLOT_BITS = 20;
constexpr uint64 QUGATE_GATE_ID_SLOT_MASK = (1ULL << QUGATE_GATE_ID_SLOT_BITS) - 1; // 0xFFFFF

// Query limits
constexpr uint64 QUGATE_MAX_OWNER_GATES = 32;    // max gates returned by getGatesByOwner
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

// Governance policy for gates with an admin multisig attached
constexpr uint8 QUGATE_GOVERNANCE_STRICT_ADMIN = 0;    // owner must wait for admin approval window
constexpr uint8 QUGATE_GOVERNANCE_OWNER_OR_ADMIN = 1;  // owner can mutate directly; admin window is a backup path

// Minimal execution observability outcome types
constexpr uint8 QUGATE_EXEC_NONE      = 0;
constexpr uint8 QUGATE_EXEC_FORWARDED = 1;
constexpr uint8 QUGATE_EXEC_HELD      = 2;
constexpr uint8 QUGATE_EXEC_REFUNDED  = 3;
constexpr uint8 QUGATE_EXEC_BURNED    = 4;
constexpr uint8 QUGATE_EXEC_REJECTED  = 5;

// Chain gate constants
constexpr uint8  QUGATE_MAX_CHAIN_DEPTH            = 3;
constexpr sint64 QUGATE_CHAIN_HOP_FEE              = 1000;
constexpr sint64 QUGATE_HEARTBEAT_PING_FEE         = 1000;  // Minimum heartbeat() ping fee floor; actual cost is pro-rated maintenance
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
constexpr sint64 QUGATE_INSUFFICIENT_FEE       = -7;  // invocationReward < required fee for this operation
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
constexpr sint64 QUGATE_INVALID_PARAMS            = -31; // Generic invalid parameter (e.g. bad lockMode or zero delayEpochs)

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
constexpr uint32 QUGATE_LOG_MAINTENANCE_CHARGED  = 29; // inactivity maintenance fee collected
constexpr uint32 QUGATE_LOG_MAINTENANCE_DELINQUENT = 30; // idle gate could not pay inactivity maintenance
constexpr uint32 QUGATE_LOG_MAINTENANCE_CURED    = 31; // delinquent gate became active again or paid maintenance later

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

// Per-gate allowed-senders configuration.
struct QUGATE_AllowedSendersConfig
{
    Array<id, 8> senders;  // Whitelist addresses
    uint8 count;           // Number of active entries (0-8)
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
        uint16 createdEpoch;        // Network epoch when this gate was created
        uint16 lastActivityEpoch;   // Network epoch of last sendToGate or updateGate call; used for expiry
        uint64 totalReceived;       // Cumulative QU received by this gate across all sendToGate calls
        uint64 totalForwarded;      // Cumulative QU forwarded to recipients (excludes dust burns)
        uint64 currentBalance;      // Held balance awaiting release: THRESHOLD mode accumulates here
        uint64 threshold;           // Release threshold for THRESHOLD mode; 0 for other modes
        uint64 roundRobinIndex;     // Next recipient index for ROUND_ROBIN mode; wraps at recipientCount
        Array<id, 8> recipients;     // Recipient public keys; indices beyond recipientCount are zeroed
        Array<uint64, 8> ratios;         // Ratio per recipient for SPLIT mode; others may zero these

        // Chain gate fields — only active when chainNextGateId != -1
        sint64 chainNextGateId;   // Versioned gate ID of next gate in chain; -1 if this is a terminal gate
        uint8  chainDepth;        // This gate's position in its chain (0 = root/trigger, increments toward leaf)

        // Unified reserve — covers chain hop fees and idle maintenance
        sint64 reserve;            // QU reserve for hop fees and inactivity maintenance; auto-seeded from excess creation fee
        uint16 nextIdleChargeEpoch; // Gate-specific due epoch for the next inactivity charge

        // Admin gate fields — governance via MULTISIG quorum
        sint64 adminGateId;       // -1 = no admin gate (owner-only). Set to a MULTISIG gate ID for quorum-controlled config.
        uint8  governancePolicy;  // QUGATE_GOVERNANCE_*; only relevant when adminGateId >= 0

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

    // Adds invocationReward to a gate's unified reserve.
    struct fundGate_input
    {
        uint64 gateId;
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
        uint8  beneficiaryCount;           // 0–8; 0 allowed only when chainNextGateId is set
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
        uint64 feePaid;  // actual maintenance cost charged
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
        uint8  governancePolicy; // QUGATE_GOVERNANCE_*; ignored when clearing
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
        uint8  governancePolicy;
        uint8  adminGateMode;  // mode of the admin gate (should be QUGATE_MODE_MULTISIG)
        uint8  guardianCount;      // Number of guardians on the admin gate
        uint8  required;           // M-of-N threshold
        uint32 adminApprovalWindowEpochs;
        uint8  adminApprovalActive;
        uint32 adminApprovalValidUntilEpoch;
        Array<id, 8> guardians;    // Guardian public keys from the admin gate's QUGATE_MultisigConfig
    };

    // Withdraw from a gate's unified reserve without closing. Owner only (or admin gate).
    struct withdrawReserve_input
    {
        uint64 gateId;
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
        sint64 chainNextGateId;
        uint8  chainDepth;
        sint64 reserve;
        uint16 nextIdleChargeEpoch;
        sint64 adminGateId;
        uint8  governancePolicy; // QUGATE_GOVERNANCE_*
        uint8  hasAdminGate;
        uint8  idleDelinquent;
        uint16 idleGraceRemainingEpochs;
        uint8  idleExpiryOverdue;
        Array<sint64, 8> recipientGateIds;
    };
    struct getGateBySlot_locals
    {
        GateConfig gate;
        QUGATE_AllowedSendersConfig asCfg;
        uint64 i;
        uint16 delinquentEpoch;
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

        // Chain gate fields
        sint64 chainNextGateId;
        uint8  chainDepth;
        sint64 reserve;            // unified reserve (hop fees + idle maintenance)
        uint16 nextIdleChargeEpoch;
        sint64 adminGateId;    // -1 if no admin gate
        uint8  governancePolicy; // QUGATE_GOVERNANCE_*
        uint8  hasAdminGate;   // 1 if governed by admin gate
        uint8  idleDelinquent; // 1 if gate has missed idle-period funding
        uint16 idleGraceRemainingEpochs; // epochs left before delinquent gate should expire
        uint8  idleExpiryOverdue; // 1 if idle grace has already been exhausted

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
        uint64 totalMaintenanceCharged;
        uint64 totalMaintenanceBurned;
        uint64 totalMaintenanceDividends;
        uint64 distributedMaintenanceDividends;
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
        uint64 feeBurnBps;          // burn portion in basis points (3000-7000)
        uint64 idleFee;             // base idle maintenance fee (before complexity scaling)
        uint64 idleWindowEpochs;
        uint64 idleGraceEpochs;
        uint64 minSendAmount;
        uint64 expiryEpochs;
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
        uint64 _feeBurnBps;         // burn portion of fees in basis points (3000-7000)
        uint64 _idleFee;     // base inactivity maintenance charge (scaled by gate complexity)
        uint64 _idleWindowEpochs; // idle-window / recurring inactivity cadence
        uint64 _idleGraceEpochs;
        uint64 _minSendAmount;
        uint64 _expiryEpochs;      // epochs of inactivity before auto-close
        uint64 _totalMaintenanceCharged;
        uint64 _totalMaintenanceBurned;
        uint64 _totalMaintenanceDividends;
        uint64 _earnedMaintenanceDividends;
        uint64 _distributedMaintenanceDividends;
        Array<uint16, QUGATE_MAX_GATES> _idleDelinquentEpochs;

        // Heartbeat mode state — indexed by gate slot (same index as _gates)
        // Beneficiaries are stored inline in QUGATE_HeartbeatConfig
        Array<QUGATE_HeartbeatConfig, QUGATE_MAX_GATES> _heartbeatConfigs;

        // Multisig mode state — indexed by gate slot (same index as _gates)
        Array<QUGATE_MultisigConfig, QUGATE_MAX_GATES> _multisigConfigs;

        // Admin approval window state — indexed by gate slot (same index as _gates)
        Array<QUGATE_AdminApprovalState, QUGATE_MAX_GATES> _adminApprovalStates;

        // TIME_LOCK mode state — indexed by gate slot (same index as _gates)
        Array<QUGATE_TimeLockConfig, QUGATE_MAX_GATES> _timeLockConfigs;

        // Allowed-senders config — indexed by gate slot (same index as _gates)
        Array<QUGATE_AllowedSendersConfig, QUGATE_MAX_GATES> _allowedSendersConfigs;

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
        QUGATE_AllowedSendersConfig allowedSendersZero;
        uint64 totalRatio;
        uint64 i;
        uint64 slotIdx;
        uint64 currentFee;     // escalated fee
        uint64 creationBurnAmount;
        uint64 creationDividendAmount;
        // Recipient gate validation
        uint64 rgSlot;
        uint64 rgGen;
        // Chain validation
        uint64 chainTargetSlot;
        uint64 chainTargetGen;
        GateConfig chainTarget;
        uint8 newDepth;
        uint64 walkSlot;
        uint8 walkStep;
        GateConfig walkGate;
        uint64 nextWalkSlot;
        uint64 nextWalkGen;
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
        QUGATE_TimeLockConfig targetTlCfg;
        uint64 totalRatio;
        uint64 share;
        uint64 distributed;
        uint64 i;
        // Gate-as-recipient
        uint64 targetSlot;
        uint64 targetGen;
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
        QUGATE_TimeLockConfig targetTlCfg;
        QUGATE_LatestExecution latestExec;
        // Gate-as-recipient
        uint64 targetSlot;
        uint64 targetGen;
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
        QUGATE_TimeLockConfig targetTlCfg;
        // Gate-as-recipient
        uint64 targetSlot;
        uint64 targetGen;
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
        QUGATE_TimeLockConfig targetTlCfg;
        uint64 recipientIdx;
        QUGATE_LatestExecution latestExec;
        // Gate-as-recipient
        uint64 targetSlot;
        uint64 targetGen;
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
        QUGATE_AllowedSendersConfig asCfg;
        uint64 i;
        uint8 senderAllowed;
        // Gate-as-recipient
        uint64 targetSlot;
        uint64 targetGen;
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
        uint8 accepted; // 1 if the target gate accepted custody or delivery of the amount
        // Deferred gate-as-recipient routing — caller must dispatch via routeToGate
        uint8 deferredCount;
        Array<uint64, 8> deferredGateSlots;
        Array<sint64, 8> deferredGateAmounts;
        uint8 deferredHopCount;
    };
    struct routeToGate_locals
    {
        GateConfig gate;
        QUGATE_TimeLockConfig tlCfg;
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
        uint8 deferredIdx;
    };

    struct processMultisigVote_input { uint64 slotIdx; uint64 gateId; sint64 amount; };
    struct processMultisigVote_output { sint64 status; uint8 deferredToGate; uint64 deferredGateSlot; sint64 deferredGateAmount; uint8 chainForward; sint64 chainForwardAmount; };
    struct processMultisigVote_locals
    {
        GateConfig gate;
        QUGATE_MultisigConfig msigCfg;
        QUGATE_AdminApprovalState adminApproval;
        QUGATE_TimeLockConfig targetTlCfg;
        uint8 foundGuardian;
        uint8 guardianIdx;
        QuGateLogger logger;
        sint64 releaseAmount;
        uint8 transferred;
        uint64 targetSlot;
        uint64 targetGen;
    };

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
        uint64 adminCheckEncodedGen;
        GateConfig adminCheckGate;
        QUGATE_MultisigConfig adminCheckMs;
        QUGATE_AdminApprovalState adminCheckApproval;
        uint8 adminApprovalUsed;
        uint8 adminAuth;
        uint64 nextWalkSlot;
        uint64 nextWalkGen;
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
        // Chain forwarding inline vars
        sint64 currentChainGateId;
        uint64 nextSlot;
        uint64 nextGen;
        uint8 hop;
        sint64 chainAmount;
        uint8 deferredIdx;
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
        // Chain forwarding inline vars
        sint64 currentChainGateId;
        uint64 nextSlot;
        uint64 nextGen;
        uint8 hop;
        sint64 chainAmount;
        uint8 deferredIdx;
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
        uint64 adminCheckEncodedGen;
        GateConfig adminCheckGate;
        QUGATE_MultisigConfig adminCheckMs;
        QUGATE_AdminApprovalState adminCheckApproval;
        uint8 adminApprovalUsed;
        uint8 adminAuth;
        uint8 loopIdx;
    };

    struct updateGate_locals
    {
        sint64 invReward;
        QuGateLogger logger;
        GateConfig gate;
        QUGATE_AllowedSendersConfig asCfg;
        uint64 totalRatio;
        uint64 i;
        uint64 slotIdx;
        uint64 encodedGen;
        // Admin gate check
        uint64 adminCheckSlot;
        uint64 adminCheckEncodedGen;
        GateConfig adminCheckGate;
        QUGATE_MultisigConfig adminCheckMs;
        QUGATE_AdminApprovalState adminCheckApproval;
        uint8 adminApprovalUsed;
        uint8 adminAuth;
        uint64 rgSlot;
        uint64 rgGen;
    };

    struct getGate_locals
    {
        GateConfig gate;
        QUGATE_AllowedSendersConfig asCfg;
        uint64 i;
        uint64 slotIdx;
        uint64 encodedGen;
        uint16 delinquentEpoch;
    };

    struct getGateBatch_locals                           //
    {
        uint64 i;
        uint64 j;
        GateConfig gate;
        QUGATE_AllowedSendersConfig asCfg;
        getGate_output entry;
        uint64 slotIdx;
        uint64 encodedGen;
        uint16 delinquentEpoch;
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
        uint8  maintenanceEligible;
        uint16 delinquentEpoch;
        uint64 effectiveIdleFee;
        uint64 idleMultiplierBps;
        uint64 maintenanceBurnAmount;
        uint64 maintenanceDividendAmount;
        uint64 maintenanceDistributed;
        uint64 maintenanceAmountPerShare;
        uint8  recentlyActive;
        uint8  activeHold;
        uint8  cycleDue;
        // Downstream reserve drain + admin gate drain
        uint8  downstreamIdx;
        uint64 downstreamSlot;
        uint64 downstreamGen;
        GateConfig downstreamGate;
        uint64 downstreamIdleFee;
        uint64 downstreamMultiplierBps;
        uint8  downstreamCount;
        uint64 downstreamBurnAmount;
        uint64 downstreamDividendAmount;
        uint64 adminDrainSlot;
        uint64 adminDrainGen;
        GateConfig adminDrainGate;
        uint64 adminDrainFee;
        uint64 adminDrainBurn;
        uint64 adminDrainDividend;
        // Admin gate expiry check
        uint8  adminGateGovernsActive;
        uint64 adminExpiryCheckIdx;
        // Heartbeat processing
        QUGATE_HeartbeatConfig inhCfg;
        sint64 inhBalance;
        sint64 inhPayoutTotal;
        sint64 inhDistributedTotal;
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
        // Chain forwarding inline vars
        sint64 chainAmount;
        sint64 currentChainGateId;
        uint8 hop;
        uint64 nextSlot;
        uint64 nextGen;
        GateConfig nextGate;
        sint64 dustTotal;
        uint8 loopIdx;
        uint8 tlTransferred;
        uint64 targetSlot;
        uint64 targetGen;
        // Expiry cleanup zero configs
        QUGATE_HeartbeatConfig hbZeroCfg;
        QUGATE_MultisigConfig msigZeroCfg;
        QUGATE_AdminApprovalState zeroAdminApproval;
        QUGATE_TimeLockConfig tlZeroCfg;
    };

    struct configureTimeLock_locals
    {
        sint64 invReward;
        QuGateLogger logger;
        GateConfig gate;
        uint64 slotIdx;
        uint64 encodedGen;
        QUGATE_TimeLockConfig cfg;
        uint64 configFee;
        uint64 configBurn;
        uint64 configDividend;
        uint64 lockDuration;
        // Admin gate check
        uint64 adminCheckSlot;
        uint64 adminCheckEncodedGen;
        GateConfig adminCheckGate;
        QUGATE_MultisigConfig adminCheckMs;
        QUGATE_AdminApprovalState adminCheckApproval;
        uint8 adminApprovalUsed;
        uint8 adminAuth;
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
        uint64 adminCheckEncodedGen;
        GateConfig adminCheckGate;
        QUGATE_MultisigConfig adminCheckMs;
        QUGATE_AdminApprovalState adminCheckApproval;
        uint8 adminApprovalUsed;
        uint8 adminAuth;
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
        uint64 nextAdminSlot;
        uint64 nextAdminGen;
    };

    struct getAdminGate_locals
    {
        GateConfig gate;
        GateConfig adminGate;
        uint64 slotIdx;
        uint64 encodedGen;
        uint64 adminSlot;
        uint64 adminEncodedGen;
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
        uint64 adminCheckEncodedGen;
        GateConfig adminCheckGate;
        QUGATE_MultisigConfig adminCheckMs;
        QUGATE_AdminApprovalState adminCheckApproval;
        uint8 adminApprovalUsed;
        sint64 available;
        uint64 toWithdraw;
        uint8 adminAuth;
    };

    struct END_TICK_locals
    {
        uint64 availableMaintenanceDividends;
        uint64 maintenanceDividendPerShare;
    };

    struct getGatesByMode_locals
    {
        uint64 i;
    };

    struct INITIALIZE_locals
    {
        uint64 i;
    };

    struct BEGIN_EPOCH_locals
    {
        uint64 i;
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
        uint64 configFee;
        uint64 configBurn;
        uint64 configDividend;
        // Admin gate check
        uint64 adminCheckSlot;
        uint64 adminCheckEncodedGen;
        GateConfig adminCheckGate;
        QUGATE_MultisigConfig adminCheckMs;
        QUGATE_AdminApprovalState adminCheckApproval;
        uint8 adminApprovalUsed;
        uint8 adminAuth;
    };

    struct heartbeat_locals
    {
        sint64 invReward;
        QuGateLogger logger;
        GateConfig gate;
        uint64 slotIdx;
        uint64 encodedGen;
        QUGATE_HeartbeatConfig cfg;
        // Maintenance cost computation
        uint64 maintenanceCost;
        uint64 ownMultiplierBps;
        uint64 ownIdleFee;
        uint64 downstreamCount;
        uint64 downstreamTotalFee;
        uint64 downstreamSlot;
        uint64 downstreamGen;
        GateConfig downstreamGate;
        uint64 downstreamMultBps;
        uint64 surcharge;
        uint64 adminFee;
        uint64 elapsedEpochs;
        uint64 proratedCost;
        uint64 burnAmount;
        uint64 dividendAmount;
        uint8  dsIdx;
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
        uint64 adminCheckEncodedGen;
        GateConfig adminCheckGate;
        QUGATE_MultisigConfig adminCheckMs;
        QUGATE_AdminApprovalState adminCheckApproval;
        uint8 adminApprovalUsed;
        uint8 guardianLoopIdx;
        uint8 adminAuth;
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
        if (input.mode > QUGATE_MODE_TIME_LOCK || input.mode == QUGATE_MODE_ORACLE)
        {
            // Refund all.
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
        // HEARTBEAT, MULTISIG, and TIME_LOCK configure recipients separately.
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

        // Build the gate config
        locals.newGate.owner = qpi.invocator();
        locals.newGate.mode = input.mode;
        locals.newGate.recipientCount = input.recipientCount;
        locals.newGate.active = 1;
        locals.newGate.createdEpoch = qpi.epoch();       // uint16
        locals.newGate.lastActivityEpoch = qpi.epoch();  //
        locals.newGate.totalReceived = 0;
        locals.newGate.totalForwarded = 0;
        locals.newGate.currentBalance = 0;
        locals.newGate.threshold = input.threshold;
        locals.newGate.roundRobinIndex = 0;

        // Chain fields
        locals.newGate.chainNextGateId = -1;
        locals.newGate.chainDepth = 0;
        locals.newGate.reserve = 0; // seeded below from excess creation fee
        if (state.get()._idleWindowEpochs > 0)
        {
            locals.newGate.nextIdleChargeEpoch = qpi.epoch() + (uint16)state.get()._idleWindowEpochs;
        }
        else
        {
            locals.newGate.nextIdleChargeEpoch = 0;
        }

        // Admin gate fields
        locals.newGate.adminGateId = -1;
        locals.newGate.governancePolicy = QUGATE_GOVERNANCE_STRICT_ADMIN;

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

        // Build allowed-senders config (side array)
        locals.allowedSendersZero.count = input.allowedSenderCount;
        for (locals.i = 0; locals.i < QUGATE_MAX_RECIPIENTS; locals.i++)
        {
            if (locals.i < input.allowedSenderCount)
            {
                locals.allowedSendersZero.senders.set(locals.i, input.allowedSenders.get(locals.i));
            }
            else
            {
                locals.allowedSendersZero.senders.set(locals.i, id::zero());
            }
        }

        // Validate gate-as-recipient IDs (must be -1 or valid active gate with correct generation)
        for (locals.i = 0; locals.i < input.recipientCount; locals.i++)
        {
            if (input.recipientGateIds.get(locals.i) >= 0)
            {
                locals.rgSlot = (uint64)(input.recipientGateIds.get(locals.i)) & QUGATE_GATE_ID_SLOT_MASK;
                locals.rgGen = (uint64)(input.recipientGateIds.get(locals.i)) >> QUGATE_GATE_ID_SLOT_BITS;
                if (locals.rgSlot >= state.get()._gateCount
                    || locals.rgGen == 0
                    || state.get()._gateGenerations.get(locals.rgSlot) != (uint16)(locals.rgGen - 1)
                    || state.get()._gates.get(locals.rgSlot).active == 0)
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
            locals.chainTargetSlot = (uint64)(input.chainNextGateId) & QUGATE_GATE_ID_SLOT_MASK;
            locals.chainTargetGen = (uint64)(input.chainNextGateId) >> QUGATE_GATE_ID_SLOT_BITS;
            if (input.chainNextGateId <= 0
                || locals.chainTargetSlot >= state.get()._gateCount
                || locals.chainTargetGen == 0
                || state.get()._gateGenerations.get(locals.chainTargetSlot) != (uint16)(locals.chainTargetGen - 1))
            {
                // Undo slot allocation.
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

            locals.chainTarget = state.get()._gates.get(locals.chainTargetSlot);
            if (locals.chainTarget.active == 0)
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
            locals.newDepth = locals.chainTarget.chainDepth + 1;
            if (locals.newDepth >= QUGATE_MAX_CHAIN_DEPTH)
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
                locals.walkSlot = locals.chainTargetSlot;
                locals.walkStep = 0;
                while (locals.walkStep < QUGATE_MAX_CHAIN_DEPTH)
                {
                    if (locals.walkSlot == locals.slotIdx)
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
                    locals.walkGate = state.get()._gates.get(locals.walkSlot);
                    if (locals.walkGate.chainNextGateId == -1)
                    {
                        break;
                    }
                    locals.nextWalkSlot = (uint64)(locals.walkGate.chainNextGateId) & QUGATE_GATE_ID_SLOT_MASK;
                    locals.nextWalkGen = (uint64)(locals.walkGate.chainNextGateId) >> QUGATE_GATE_ID_SLOT_BITS;
                    if (locals.walkGate.chainNextGateId <= 0
                        || locals.nextWalkSlot >= state.get()._gateCount
                        || locals.nextWalkGen == 0
                        || state.get()._gateGenerations.get(locals.nextWalkSlot) != (uint16)(locals.nextWalkGen - 1))
                    {
                        break;
                    }
                    locals.walkSlot = locals.nextWalkSlot;
                    locals.walkStep++;
                }
                if (locals.walkSlot == locals.slotIdx)
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
            locals.newGate.chainDepth = locals.newDepth;
        }

        state.mut()._gates.set(locals.slotIdx, locals.newGate);
        state.mut()._allowedSendersConfigs.set(locals.slotIdx, locals.allowedSendersZero);
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

        // Split creation fee: burn + shareholder dividends
        locals.creationBurnAmount = QPI::div(locals.currentFee * state.get()._feeBurnBps, 10000ULL);
        locals.creationDividendAmount = locals.currentFee - locals.creationBurnAmount;
        qpi.burn(locals.creationBurnAmount);
        state.mut()._totalBurned += locals.creationBurnAmount;
        state.mut()._earnedMaintenanceDividends += locals.creationDividendAmount;
        state.mut()._totalMaintenanceDividends += locals.creationDividendAmount;
        output.feePaid = locals.currentFee;

        // Excess creation fee seeds the unified reserve (no separate fundGate needed)
        if (locals.invReward > (sint64)locals.currentFee)
        {
            locals.newGate.reserve = locals.invReward - (sint64)locals.currentFee;
            state.mut()._gates.set(locals.slotIdx, locals.newGate);
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
                    locals.targetSlot = (uint64)(locals.gate.recipientGateIds.get(locals.i)) & QUGATE_GATE_ID_SLOT_MASK;
                    locals.targetGen = (uint64)(locals.gate.recipientGateIds.get(locals.i)) >> QUGATE_GATE_ID_SLOT_BITS;
                    if (locals.targetSlot < state.get()._gateCount && locals.targetGen > 0
                        && state.get()._gateGenerations.get(locals.targetSlot) == (uint16)(locals.targetGen - 1)
                        && state.get()._gates.get(locals.targetSlot).active == 1)
                    {
                        locals.targetTlCfg.active = 1;
                        locals.targetTlCfg.cancelled = 0;
                        locals.targetTlCfg.fired = 0;
                        if (state.get()._gates.get(locals.targetSlot).mode == QUGATE_MODE_TIME_LOCK)
                        {
                            locals.targetTlCfg = state.get()._timeLockConfigs.get(locals.targetSlot);
                        }
                        if (state.get()._gates.get(locals.targetSlot).mode != QUGATE_MODE_TIME_LOCK
                            || (locals.targetTlCfg.active == 1 && locals.targetTlCfg.cancelled == 0 && locals.targetTlCfg.fired == 0))
                        {
                            output.deferredGateSlots.set(output.deferredCount, locals.targetSlot);
                            output.deferredGateAmounts.set(output.deferredCount, (sint64)locals.share);
                            output.deferredCount++;
                            locals.distributed += locals.share;
                        }
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
            locals.targetSlot = (uint64)(locals.gate.recipientGateIds.get(locals.gate.roundRobinIndex)) & QUGATE_GATE_ID_SLOT_MASK;
            locals.targetGen = (uint64)(locals.gate.recipientGateIds.get(locals.gate.roundRobinIndex)) >> QUGATE_GATE_ID_SLOT_BITS;
            if (locals.targetSlot < state.get()._gateCount && locals.targetGen > 0
                && state.get()._gateGenerations.get(locals.targetSlot) == (uint16)(locals.targetGen - 1)
                && state.get()._gates.get(locals.targetSlot).active == 1)
            {
                locals.targetTlCfg.active = 1;
                locals.targetTlCfg.cancelled = 0;
                locals.targetTlCfg.fired = 0;
                if (state.get()._gates.get(locals.targetSlot).mode == QUGATE_MODE_TIME_LOCK)
                {
                    locals.targetTlCfg = state.get()._timeLockConfigs.get(locals.targetSlot);
                }
                if (state.get()._gates.get(locals.targetSlot).mode != QUGATE_MODE_TIME_LOCK
                    || (locals.targetTlCfg.active == 1 && locals.targetTlCfg.cancelled == 0 && locals.targetTlCfg.fired == 0))
                {
                    output.deferredToGate = 1;
                    output.deferredGateSlot = locals.targetSlot;
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
                    locals.targetSlot = (uint64)(locals.gate.recipientGateIds.get(0)) & QUGATE_GATE_ID_SLOT_MASK;
                    locals.targetGen = (uint64)(locals.gate.recipientGateIds.get(0)) >> QUGATE_GATE_ID_SLOT_BITS;
                    if (locals.targetSlot < state.get()._gateCount && locals.targetGen > 0
                        && state.get()._gateGenerations.get(locals.targetSlot) == (uint16)(locals.targetGen - 1)
                        && state.get()._gates.get(locals.targetSlot).active == 1)
                    {
                        locals.targetTlCfg.active = 1;
                        locals.targetTlCfg.cancelled = 0;
                        locals.targetTlCfg.fired = 0;
                        if (state.get()._gates.get(locals.targetSlot).mode == QUGATE_MODE_TIME_LOCK)
                        {
                            locals.targetTlCfg = state.get()._timeLockConfigs.get(locals.targetSlot);
                        }
                        if (state.get()._gates.get(locals.targetSlot).mode != QUGATE_MODE_TIME_LOCK
                            || (locals.targetTlCfg.active == 1 && locals.targetTlCfg.cancelled == 0 && locals.targetTlCfg.fired == 0))
                        {
                            output.deferredToGate = 1;
                            output.deferredGateSlot = locals.targetSlot;
                            output.deferredGateAmount = (sint64)locals.gate.currentBalance;
                            output.forwarded = locals.gate.currentBalance;
                            locals.gate.totalForwarded += locals.gate.currentBalance;
                            locals.gate.currentBalance = 0;
                        }
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

        locals.recipientIdx = QPI::mod(locals.gate.totalReceived + qpi.tick(), (uint64)locals.gate.recipientCount);
        if (locals.gate.recipientGateIds.get(locals.recipientIdx) >= 0)
        {
            // Gate-as-recipient: defer routing to caller
            locals.targetSlot = (uint64)(locals.gate.recipientGateIds.get(locals.recipientIdx)) & QUGATE_GATE_ID_SLOT_MASK;
            locals.targetGen = (uint64)(locals.gate.recipientGateIds.get(locals.recipientIdx)) >> QUGATE_GATE_ID_SLOT_BITS;
            if (locals.targetSlot < state.get()._gateCount && locals.targetGen > 0
                && state.get()._gateGenerations.get(locals.targetSlot) == (uint16)(locals.targetGen - 1)
                && state.get()._gates.get(locals.targetSlot).active == 1)
            {
                locals.targetTlCfg.active = 1;
                locals.targetTlCfg.cancelled = 0;
                locals.targetTlCfg.fired = 0;
                if (state.get()._gates.get(locals.targetSlot).mode == QUGATE_MODE_TIME_LOCK)
                {
                    locals.targetTlCfg = state.get()._timeLockConfigs.get(locals.targetSlot);
                }
                if (state.get()._gates.get(locals.targetSlot).mode != QUGATE_MODE_TIME_LOCK
                    || (locals.targetTlCfg.active == 1 && locals.targetTlCfg.cancelled == 0 && locals.targetTlCfg.fired == 0))
                {
                    output.deferredToGate = 1;
                    output.deferredGateSlot = locals.targetSlot;
                    output.deferredGateAmount = input.amount;
                    locals.gate.totalForwarded += input.amount;
                    locals.latestExec.outcomeType = QUGATE_EXEC_FORWARDED;
                    locals.latestExec.selectedRecipientIndex = (uint8)locals.recipientIdx;
                    locals.latestExec.selectedDownstreamGateId = locals.gate.recipientGateIds.get(locals.recipientIdx);
                    locals.latestExec.forwardedAmount = input.amount;
                    output.forwarded = input.amount;
                }
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
        locals.asCfg = state.get()._allowedSendersConfigs.get(input.gateIdx);
        for (locals.i = 0; locals.i < locals.asCfg.count; locals.i++)
        {
            if (locals.senderAllowed == 0 && locals.asCfg.senders.get(locals.i) == qpi.invocator())
            {
                locals.senderAllowed = 1;
            }
        }

        if (locals.senderAllowed)
        {
            if (locals.gate.recipientCount == 0)
            {
                locals.gate.totalForwarded += input.amount;
                output.forwarded = input.amount;
            }
            else
            {
                if (locals.gate.recipientGateIds.get(0) >= 0)
                {
                    // Gate-as-recipient: defer routing to caller
                    locals.targetSlot = (uint64)(locals.gate.recipientGateIds.get(0)) & QUGATE_GATE_ID_SLOT_MASK;
                    locals.targetGen = (uint64)(locals.gate.recipientGateIds.get(0)) >> QUGATE_GATE_ID_SLOT_BITS;
                    if (locals.targetSlot < state.get()._gateCount && locals.targetGen > 0
                        && state.get()._gateGenerations.get(locals.targetSlot) == (uint16)(locals.targetGen - 1)
                        && state.get()._gates.get(locals.targetSlot).active == 1)
                    {
                        output.deferredToGate = 1;
                        output.deferredGateSlot = locals.targetSlot;
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
            if (qpi.transfer(qpi.invocator(), input.amount) >= 0) // [QG-17]
            {
                output.status = QUGATE_CONDITIONAL_REJECTED;
            }
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
        for (locals.guardianIdx = 0; locals.guardianIdx < locals.msigCfg.guardianCount; locals.guardianIdx++)
        {
            if (locals.foundGuardian == 0 && locals.msigCfg.guardians.get(locals.guardianIdx) == qpi.invocator())
            {
                locals.foundGuardian = 1;
                break;
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

            locals.gate = state.get()._gates.get(input.slotIdx);
            locals.gate.lastActivityEpoch = qpi.epoch();
            if (state.get()._idleWindowEpochs > 0)
            {
                locals.gate.nextIdleChargeEpoch = qpi.epoch() + (uint16)state.get()._idleWindowEpochs;
            }
            state.mut()._gates.set(input.slotIdx, locals.gate);

            locals.logger._contractIndex = CONTRACT_INDEX;
            locals.logger._type = QUGATE_LOG_MULTISIG_VOTE;
            locals.logger.gateId = input.gateId;
            locals.logger.sender = qpi.invocator();
            locals.logger.amount = input.amount;
            LOG_INFO(locals.logger);

            // Check threshold
            locals.releaseAmount = input.amount;
            if (locals.gate.recipientCount == 0
                && locals.gate.chainNextGateId == -1
                && locals.msigCfg.adminApprovalWindowEpochs > 0
                && input.amount > 0
                && locals.gate.currentBalance >= (uint64)input.amount)
            {
                // Admin-only multisig: burn the vote QU (governance gates don't accumulate)
                locals.gate.currentBalance -= (uint64)input.amount;
                state.mut()._gates.set(input.slotIdx, locals.gate);
                qpi.burn(input.amount);
                state.mut()._totalBurned += input.amount;
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

                    locals.msigCfg.approvalBitmap = 0;
                    locals.msigCfg.approvalCount = 0;
                    locals.msigCfg.proposalActive = 0;
                    state.mut()._multisigConfigs.set(input.slotIdx, locals.msigCfg);
                }
                else if (locals.gate.currentBalance > 0)
                {
                    // Transfer balance to target (recipients.get(0) is the target address)
                    locals.releaseAmount = (sint64)locals.gate.currentBalance;
                    locals.transferred = 0;
                    if (locals.gate.recipientCount > 0)
                    {
                        if (locals.gate.recipientGateIds.get(0) >= 0)
                        {
                            // Gate-as-recipient: defer routing to caller
                            locals.targetSlot = (uint64)(locals.gate.recipientGateIds.get(0)) & QUGATE_GATE_ID_SLOT_MASK;
                            locals.targetGen = (uint64)(locals.gate.recipientGateIds.get(0)) >> QUGATE_GATE_ID_SLOT_BITS;
                            if (locals.targetSlot < state.get()._gateCount && locals.targetGen > 0
                                && state.get()._gateGenerations.get(locals.targetSlot) == (uint16)(locals.targetGen - 1)
                                && state.get()._gates.get(locals.targetSlot).active == 1)
                            {
                                locals.targetTlCfg.active = 1;
                                locals.targetTlCfg.cancelled = 0;
                                locals.targetTlCfg.fired = 0;
                                if (state.get()._gates.get(locals.targetSlot).mode == QUGATE_MODE_TIME_LOCK)
                                {
                                    locals.targetTlCfg = state.get()._timeLockConfigs.get(locals.targetSlot);
                                }
                                if (state.get()._gates.get(locals.targetSlot).mode != QUGATE_MODE_TIME_LOCK
                                    || (locals.targetTlCfg.active == 1 && locals.targetTlCfg.cancelled == 0 && locals.targetTlCfg.fired == 0))
                                {
                                    output.deferredToGate = 1;
                                    output.deferredGateSlot = locals.targetSlot;
                                    output.deferredGateAmount = locals.releaseAmount;
                                    locals.transferred = 1;
                                }
                            }
                        }
                        else
                        {
                            if (qpi.transfer(locals.gate.recipients.get(0), locals.releaseAmount) >= 0) // [QG-12]
                            {
                                locals.transferred = 1;
                            }
                        }
                    }
                    else if (locals.gate.chainNextGateId != -1)
                    {
                        locals.transferred = 1; // No recipient, chain will handle
                        output.chainForward = 1;
                        output.chainForwardAmount = locals.releaseAmount;
                    }
                    locals.msigCfg.approvalBitmap = 0;
                    locals.msigCfg.approvalCount = 0;
                    locals.msigCfg.proposalActive = 0;
                    state.mut()._multisigConfigs.set(input.slotIdx, locals.msigCfg);

                    if (locals.transferred)
                    {
                        locals.gate.totalForwarded += (uint64)locals.releaseAmount;
                        locals.gate.currentBalance = 0;
                        state.mut()._gates.set(input.slotIdx, locals.gate);
                    }
                }

                locals.logger._contractIndex = CONTRACT_INDEX;
                locals.logger._type = QUGATE_LOG_MULTISIG_EXECUTED;
                locals.logger.gateId = input.gateId;
                locals.logger.sender = qpi.invocator();
                locals.logger.amount = locals.releaseAmount;
                LOG_INFO(locals.logger);
            }
        }
    }

    PRIVATE_PROCEDURE_WITH_LOCALS(routeToGate)
    {
        output.forwarded = 0;
        output.accepted = 0;

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
            if (locals.gate.reserve >= QUGATE_CHAIN_HOP_FEE)
            {
                locals.gate.reserve -= QUGATE_CHAIN_HOP_FEE;
                state.mut()._gates.set(input.slotIdx, locals.gate);
                qpi.burn(QUGATE_CHAIN_HOP_FEE);
                state.mut()._totalBurned += QUGATE_CHAIN_HOP_FEE;
                // amountAfterFee stays as input.amount (reserve paid the fee)
            }
            else
            {
                // Strand — accumulate in currentBalance
                locals.gate.currentBalance += input.amount;
                state.mut()._gates.set(input.slotIdx, locals.gate);
                output.accepted = 1;
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
            state.mut()._totalBurned += QUGATE_CHAIN_HOP_FEE;
            locals.amountAfterFee = input.amount - QUGATE_CHAIN_HOP_FEE;
        }

        // Update totalReceived and activity for the chained gate
        locals.gate.totalReceived += locals.amountAfterFee;
        locals.gate.lastActivityEpoch = qpi.epoch();
        if (state.get()._idleWindowEpochs > 0)
        {
            locals.gate.nextIdleChargeEpoch = qpi.epoch() + (uint16)state.get()._idleWindowEpochs;
        }
        state.mut()._gates.set(input.slotIdx, locals.gate);

        // Route through this gate's mode
        if (locals.gate.mode == QUGATE_MODE_SPLIT)
        {
            locals.splitIn.gateIdx = input.slotIdx;
            locals.splitIn.amount = locals.amountAfterFee;
            processSplit(qpi, state, locals.splitIn, locals.splitOut, locals.splitLocals);
            output.forwarded = locals.splitOut.forwarded;
            if (locals.splitOut.forwarded > 0 || locals.splitOut.deferredCount > 0)
            {
                output.accepted = 1;
            }
            // Bubble deferred gate-recipients to caller (no self-recursion)
            output.deferredCount = locals.splitOut.deferredCount;
            output.deferredHopCount = input.hopCount + 1;
            for (locals.deferredIdx = 0; locals.deferredIdx < locals.splitOut.deferredCount; locals.deferredIdx++)
            {
                output.deferredGateSlots.set(locals.deferredIdx, locals.splitOut.deferredGateSlots.get(locals.deferredIdx));
                output.deferredGateAmounts.set(locals.deferredIdx, locals.splitOut.deferredGateAmounts.get(locals.deferredIdx));
            }
        }
        else if (locals.gate.mode == QUGATE_MODE_ROUND_ROBIN)
        {
            locals.rrIn.gateIdx = input.slotIdx;
            locals.rrIn.amount = locals.amountAfterFee;
            processRoundRobin(qpi, state, locals.rrIn, locals.rrOut, locals.rrLocals);
            output.forwarded = locals.rrOut.forwarded;
            if (locals.rrOut.forwarded > 0 || locals.rrOut.deferredToGate == 1)
            {
                output.accepted = 1;
            }
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
            output.accepted = 1;
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
            if (locals.randOut.forwarded > 0 || locals.randOut.deferredToGate == 1)
            {
                output.accepted = 1;
            }
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
            // ORACLE mode (reserved) in chain: accumulate into currentBalance
            locals.gate = state.get()._gates.get(input.slotIdx);
            locals.gate.currentBalance += locals.amountAfterFee;
            state.mut()._gates.set(input.slotIdx, locals.gate);
            output.forwarded = locals.amountAfterFee;
            output.accepted = 1;
        }
        else if (locals.gate.mode == QUGATE_MODE_HEARTBEAT
                 || locals.gate.mode == QUGATE_MODE_MULTISIG)
        {
            // Accumulate-on-arrival modes: funds held in currentBalance until mode-specific release
            // HEARTBEAT: released by epoch trigger after inactivity threshold
            // MULTISIG: released when guardians reach quorum
            locals.gate = state.get()._gates.get(input.slotIdx);
            locals.gate.currentBalance += locals.amountAfterFee;
            state.mut()._gates.set(input.slotIdx, locals.gate);
            output.forwarded = locals.amountAfterFee;
            output.accepted = 1;
        }
        else if (locals.gate.mode == QUGATE_MODE_TIME_LOCK)
        {
            // TIME_LOCK in chain: align with direct send semantics
            locals.gate = state.get()._gates.get(input.slotIdx);
            locals.tlCfg = state.get()._timeLockConfigs.get(input.slotIdx);
            if (locals.tlCfg.active == 0 || locals.tlCfg.cancelled == 1 || locals.tlCfg.fired == 1)
            {
                output.forwarded = 0;
            }
            else
            {
                if (locals.tlCfg.lockMode == QUGATE_TIME_LOCK_RELATIVE_EPOCHS && locals.tlCfg.unlockEpoch == 0)
                {
                    locals.tlCfg.unlockEpoch = (uint32)qpi.epoch() + locals.tlCfg.delayEpochs;
                    state.mut()._timeLockConfigs.set(input.slotIdx, locals.tlCfg);
                }
                locals.gate.currentBalance += locals.amountAfterFee;
                state.mut()._gates.set(input.slotIdx, locals.gate);
                output.forwarded = locals.amountAfterFee;
                output.accepted = 1;
            }
        }
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
            if (locals.gate.reserve > 0)
            {
                if (qpi.transfer(locals.gate.owner, locals.gate.reserve) >= 0) // [QG-01]
                {
                    locals.gate.reserve = 0;
                }
            }
            state.mut()._gates.set(locals.slotIdx, locals.gate);
            if (locals.gate.currentBalance > 0 || locals.gate.reserve > 0)
            {
                if (locals.invReward > 0) qpi.transfer(qpi.invocator(), locals.invReward);
                output.status = QUGATE_GATE_NOT_ACTIVE;
                locals.logger._type = QUGATE_LOG_GATE_EXPIRED;
                LOG_INFO(locals.logger);
                return;
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

        if (locals.gate.mode == QUGATE_MODE_TIME_LOCK)
        {
            locals.tlCfg = state.get()._timeLockConfigs.get(locals.slotIdx);
            if (locals.tlCfg.active == 0 || locals.tlCfg.cancelled == 1 || locals.tlCfg.fired == 1)
            {
                if (qpi.transfer(qpi.invocator(), locals.amount) >= 0)
                {
                    output.status = QUGATE_GATE_NOT_ACTIVE;
                }
                else
                {
                    output.status = QUGATE_INVALID_PARAMS;
                    locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                    LOG_WARNING(locals.logger);
                }
                return;
            }
        }

        // Update activity and track received
        locals.gate.lastActivityEpoch = qpi.epoch();
        locals.gate.totalReceived += locals.amount;
        if (state.get()._idleWindowEpochs > 0)
        {
            locals.gate.nextIdleChargeEpoch = qpi.epoch() + (uint16)state.get()._idleWindowEpochs;
        }
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
            for (locals.deferredIdx = 0; locals.deferredIdx < locals.splitOut.deferredCount; locals.deferredIdx++)
            {
                locals.chainIn.slotIdx = locals.splitOut.deferredGateSlots.get(locals.deferredIdx);
                locals.chainIn.amount = locals.splitOut.deferredGateAmounts.get(locals.deferredIdx);
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
            // ORACLE mode (reserved): accumulate into currentBalance
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
                locals.currentChainGateId = locals.gate.chainNextGateId;
                if (locals.currentChainGateId != -1)
                {
                    locals.nextSlot = (uint64)locals.currentChainGateId & QUGATE_GATE_ID_SLOT_MASK;
                    locals.nextGen = (uint64)locals.currentChainGateId >> QUGATE_GATE_ID_SLOT_BITS;
                    if (locals.nextSlot < state.get()._gateCount && locals.nextGen > 0
                        && state.get()._gateGenerations.get(locals.nextSlot) == (uint16)(locals.nextGen - 1))
                    {
                        locals.chainIn.slotIdx = locals.nextSlot;
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
            locals.chainAmount = 0;
            if (locals.gate.mode == QUGATE_MODE_SPLIT)
            {
                locals.chainAmount = locals.splitOut.forwarded;
            }
            else if (locals.gate.mode == QUGATE_MODE_ROUND_ROBIN)
            {
                locals.chainAmount = locals.rrOut.forwarded;
            }
            else if (locals.gate.mode == QUGATE_MODE_THRESHOLD)
            {
                locals.chainAmount = locals.threshOut.forwarded;
            }
            else if (locals.gate.mode == QUGATE_MODE_RANDOM)
            {
                locals.chainAmount = locals.randOut.forwarded;
            }
            else if (locals.gate.mode == QUGATE_MODE_CONDITIONAL && locals.condOut.status == QUGATE_SUCCESS)
            {
                locals.chainAmount = locals.condOut.forwarded;
            }

            // Accumulate-on-arrival modes: chainAmount is 0 at send time
            // (funds accumulate, chain forwarding happens at release time via
            // processMultisigVote / END_EPOCH heartbeat / END_EPOCH time-lock)

            if (locals.chainAmount > 0)
            {
                locals.currentChainGateId = locals.gate.chainNextGateId;
                locals.hop = 0;
                while (locals.hop < QUGATE_MAX_CHAIN_DEPTH && locals.currentChainGateId != -1 && locals.chainAmount > 0)
                {
                    locals.nextSlot = (uint64)locals.currentChainGateId & QUGATE_GATE_ID_SLOT_MASK;
                    locals.nextGen = (uint64)locals.currentChainGateId >> QUGATE_GATE_ID_SLOT_BITS;
                    if (locals.nextSlot >= state.get()._gateCount || locals.nextGen == 0
                        || state.get()._gateGenerations.get(locals.nextSlot) != (uint16)(locals.nextGen - 1))
                    {
                        break; // dead link
                    }
                    locals.chainIn.slotIdx = locals.nextSlot;
                    locals.chainIn.amount = locals.chainAmount;
                    locals.chainIn.hopCount = locals.hop;
                    routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                    locals.chainAmount = locals.chainOut.forwarded;
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
                    locals.nextChainGate = state.get()._gates.get(locals.nextSlot);
                    locals.currentChainGateId = locals.nextChainGate.chainNextGateId;
                    locals.hop += 1;
                }
                // Failsafe: if chain forwarding couldn't deliver, revert to currentBalance.
                if (locals.chainAmount > 0 && locals.currentChainGateId != -1)
                {
                    locals.gate = state.get()._gates.get(locals.slotIdx);
                    locals.gate.currentBalance += (uint64)locals.chainAmount;
                    locals.gate.totalForwarded -= (uint64)locals.chainAmount; // undo the premature forward count
                    state.mut()._gates.set(locals.slotIdx, locals.gate);
                    locals.logger._type = QUGATE_LOG_CHAIN_HOP_INSUFFICIENT;
                    locals.logger.gateId = input.gateId;
                    locals.logger.amount = locals.chainAmount;
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
            if (locals.gate.reserve > 0)
            {
                if (qpi.transfer(locals.gate.owner, locals.gate.reserve) >= 0) // [QG-02]
                {
                    locals.gate.reserve = 0;
                }
            }
            state.mut()._gates.set(locals.slotIdx, locals.gate);
            if (locals.gate.currentBalance > 0 || locals.gate.reserve > 0)
            {
                if (locals.invReward > 0) qpi.transfer(qpi.invocator(), locals.invReward);
                output.status = QUGATE_GATE_NOT_ACTIVE;
                locals.logger._type = QUGATE_LOG_GATE_EXPIRED;
                LOG_INFO(locals.logger);
                return;
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

        if (locals.gate.mode == QUGATE_MODE_TIME_LOCK)
        {
            locals.tlCfg = state.get()._timeLockConfigs.get(locals.slotIdx);
            if (locals.tlCfg.active == 0 || locals.tlCfg.cancelled == 1 || locals.tlCfg.fired == 1)
            {
                if (qpi.transfer(qpi.invocator(), locals.amount) >= 0)
                {
                    output.status = QUGATE_GATE_NOT_ACTIVE;
                }
                else
                {
                    output.status = QUGATE_INVALID_PARAMS;
                    locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                    LOG_WARNING(locals.logger);
                }
                return;
            }
        }

        // Update activity and track received
        locals.gate.lastActivityEpoch = qpi.epoch();
        locals.gate.totalReceived += locals.amount;
        if (state.get()._idleWindowEpochs > 0)
        {
            locals.gate.nextIdleChargeEpoch = qpi.epoch() + (uint16)state.get()._idleWindowEpochs;
        }
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
            for (locals.deferredIdx = 0; locals.deferredIdx < locals.splitOut.deferredCount; locals.deferredIdx++)
            {
                locals.chainIn.slotIdx = locals.splitOut.deferredGateSlots.get(locals.deferredIdx);
                locals.chainIn.amount = locals.splitOut.deferredGateAmounts.get(locals.deferredIdx);
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
            // ORACLE mode (reserved): accumulate into currentBalance
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
                locals.currentChainGateId = locals.gate.chainNextGateId;
                if (locals.currentChainGateId != -1)
                {
                    locals.nextSlot = (uint64)locals.currentChainGateId & QUGATE_GATE_ID_SLOT_MASK;
                    locals.nextGen = (uint64)locals.currentChainGateId >> QUGATE_GATE_ID_SLOT_BITS;
                    if (locals.nextSlot < state.get()._gateCount && locals.nextGen > 0
                        && state.get()._gateGenerations.get(locals.nextSlot) == (uint16)(locals.nextGen - 1))
                    {
                        locals.chainIn.slotIdx = locals.nextSlot;
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
            locals.chainAmount = 0;
            if (locals.gate.mode == QUGATE_MODE_SPLIT)
            {
                locals.chainAmount = locals.splitOut.forwarded;
            }
            else if (locals.gate.mode == QUGATE_MODE_ROUND_ROBIN)
            {
                locals.chainAmount = locals.rrOut.forwarded;
            }
            else if (locals.gate.mode == QUGATE_MODE_THRESHOLD)
            {
                locals.chainAmount = locals.threshOut.forwarded;
            }
            else if (locals.gate.mode == QUGATE_MODE_RANDOM)
            {
                locals.chainAmount = locals.randOut.forwarded;
            }
            else if (locals.gate.mode == QUGATE_MODE_CONDITIONAL && locals.condOut.status == QUGATE_SUCCESS)
            {
                locals.chainAmount = locals.condOut.forwarded;
            }

            // Accumulate-on-arrival modes: chainAmount is 0 at send time
            // (funds accumulate, chain forwarding happens at release time via
            // processMultisigVote / END_EPOCH heartbeat / END_EPOCH time-lock)

            if (locals.chainAmount > 0)
            {
                locals.currentChainGateId = locals.gate.chainNextGateId;
                locals.hop = 0;
                while (locals.hop < QUGATE_MAX_CHAIN_DEPTH && locals.currentChainGateId != -1 && locals.chainAmount > 0)
                {
                    locals.nextSlot = (uint64)locals.currentChainGateId & QUGATE_GATE_ID_SLOT_MASK;
                    locals.nextGen = (uint64)locals.currentChainGateId >> QUGATE_GATE_ID_SLOT_BITS;
                    if (locals.nextSlot >= state.get()._gateCount || locals.nextGen == 0
                        || state.get()._gateGenerations.get(locals.nextSlot) != (uint16)(locals.nextGen - 1))
                    {
                        break; // dead link
                    }
                    locals.chainIn.slotIdx = locals.nextSlot;
                    locals.chainIn.amount = locals.chainAmount;
                    locals.chainIn.hopCount = locals.hop;
                    routeToGate(qpi, state, locals.chainIn, locals.chainOut, locals.chainLocals);
                    locals.chainAmount = locals.chainOut.forwarded;
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
                    locals.nextChainGate = state.get()._gates.get(locals.nextSlot);
                    locals.currentChainGateId = locals.nextChainGate.chainNextGateId;
                    locals.hop += 1;
                }
                // Failsafe: if chain forwarding couldn't deliver, revert to currentBalance.
                if (locals.chainAmount > 0 && locals.currentChainGateId != -1)
                {
                    locals.gate = state.get()._gates.get(locals.slotIdx);
                    locals.gate.currentBalance += (uint64)locals.chainAmount;
                    locals.gate.totalForwarded -= (uint64)locals.chainAmount; // undo the premature forward count
                    state.mut()._gates.set(locals.slotIdx, locals.gate);
                    locals.logger._type = QUGATE_LOG_CHAIN_HOP_INSUFFICIENT;
                    locals.logger.gateId = input.gateId;
                    locals.logger.amount = locals.chainAmount;
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
        if (locals.gate.owner != qpi.invocator()
            || (locals.gate.adminGateId >= 0
                && locals.gate.governancePolicy == QUGATE_GOVERNANCE_STRICT_ADMIN))
        {
            locals.adminAuth = 0;
            if (locals.gate.adminGateId >= 0)
            {
                locals.adminCheckSlot = locals.gate.adminGateId & QUGATE_GATE_ID_SLOT_MASK;
                locals.adminCheckEncodedGen = (uint64)(locals.gate.adminGateId) >> QUGATE_GATE_ID_SLOT_BITS;
                if (locals.gate.adminGateId > 0
                    && locals.adminCheckSlot < state.get()._gateCount
                    && locals.adminCheckEncodedGen > 0
                    && state.get()._gateGenerations.get(locals.adminCheckSlot) == (uint16)(locals.adminCheckEncodedGen - 1))
                {
                    locals.adminCheckGate = state.get()._gates.get(locals.adminCheckSlot);
                    if (locals.adminCheckGate.active && locals.adminCheckGate.mode == QUGATE_MODE_MULTISIG)
                    {
                        locals.adminCheckApproval = state.get()._adminApprovalStates.get(locals.adminCheckSlot);
                        if (locals.adminCheckApproval.active == 1)
                        {
                            if ((uint32)qpi.epoch() <= locals.adminCheckApproval.validUntilEpoch)
                            {
                                locals.adminAuth = 1;
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
            if (locals.adminAuth == 0)
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
            if (locals.gate.reserve > 0)
            {
                if (qpi.transfer(locals.gate.owner, locals.gate.reserve) >= 0) // [QG-03]
                {
                    locals.gate.reserve = 0;
                }
            }
            state.mut()._gates.set(locals.slotIdx, locals.gate);
            if (locals.gate.currentBalance > 0 || locals.gate.reserve > 0)
            {
                if (locals.invReward > 0) qpi.transfer(qpi.invocator(), locals.invReward);
                output.status = QUGATE_GATE_NOT_ACTIVE;
                locals.logger._type = QUGATE_LOG_GATE_EXPIRED;
                LOG_INFO(locals.logger);
                return;
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

        // Refund any held balance (THRESHOLD mode)
        if (locals.gate.currentBalance > 0)
        {
            if (qpi.transfer(locals.gate.owner, locals.gate.currentBalance) >= 0) // [QG-06]
            {
                locals.gate.currentBalance = 0;
            }
        }

        // Refund unified reserve
        if (locals.gate.reserve > 0)
        {
            if (qpi.transfer(locals.gate.owner, locals.gate.reserve) >= 0) // [QG-06]
            {
                locals.gate.reserve = 0;
            }
        }

        state.mut()._gates.set(locals.slotIdx, locals.gate);

        if (locals.gate.currentBalance > 0 || locals.gate.reserve > 0)
        {
            if (locals.invReward > 0)
            {
                qpi.transfer(qpi.invocator(), locals.invReward);
            }
            output.status = QUGATE_INVALID_PARAMS;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
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
            for (locals.loopIdx = 0; locals.loopIdx < 8; locals.loopIdx++)
            {
                locals.hbZeroCfg.beneficiaryAddresses.set(locals.loopIdx, id::zero());
                locals.hbZeroCfg.beneficiaryShares.set(locals.loopIdx, 0);
            }
            state.mut()._heartbeatConfigs.set(locals.slotIdx, locals.hbZeroCfg);
        }

        // Clear multisig config on close
        if (locals.gate.mode == QUGATE_MODE_MULTISIG)
        {
            for (locals.loopIdx = 0; locals.loopIdx < 8; locals.loopIdx++)
            {
                locals.msigZeroCfg.guardians.set(locals.loopIdx, id::zero());
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
        if (locals.gate.owner != qpi.invocator()
            || (locals.gate.adminGateId >= 0
                && locals.gate.governancePolicy == QUGATE_GOVERNANCE_STRICT_ADMIN))
        {
            locals.adminAuth = 0;
            if (locals.gate.adminGateId >= 0)
            {
                locals.adminCheckSlot = locals.gate.adminGateId & QUGATE_GATE_ID_SLOT_MASK;
                locals.adminCheckEncodedGen = (uint64)(locals.gate.adminGateId) >> QUGATE_GATE_ID_SLOT_BITS;
                if (locals.gate.adminGateId > 0
                    && locals.adminCheckSlot < state.get()._gateCount
                    && locals.adminCheckEncodedGen > 0
                    && state.get()._gateGenerations.get(locals.adminCheckSlot) == (uint16)(locals.adminCheckEncodedGen - 1))
                {
                    locals.adminCheckGate = state.get()._gates.get(locals.adminCheckSlot);
                    if (locals.adminCheckGate.active && locals.adminCheckGate.mode == QUGATE_MODE_MULTISIG)
                    {
                        locals.adminCheckApproval = state.get()._adminApprovalStates.get(locals.adminCheckSlot);
                        if (locals.adminCheckApproval.active == 1)
                        {
                            if ((uint32)qpi.epoch() <= locals.adminCheckApproval.validUntilEpoch)
                            {
                                locals.adminAuth = 1;
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
            if (locals.adminAuth == 0)
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
                if (qpi.transfer(locals.gate.owner, locals.gate.currentBalance) >= 0)
                {
                    locals.gate.currentBalance = 0;
                }
            }
            if (locals.gate.reserve > 0)
            {
                if (qpi.transfer(locals.gate.owner, locals.gate.reserve) >= 0)
                {
                    locals.gate.reserve = 0;
                }
            }
            state.mut()._gates.set(locals.slotIdx, locals.gate);
            if (locals.gate.currentBalance > 0 || locals.gate.reserve > 0)
            {
                if (locals.invReward > 0) qpi.transfer(qpi.invocator(), locals.invReward);
                output.status = QUGATE_GATE_NOT_ACTIVE;
                locals.logger._type = QUGATE_LOG_GATE_EXPIRED;
                LOG_INFO(locals.logger);
                return;
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
        if (state.get()._idleWindowEpochs > 0)
        {
            locals.gate.nextIdleChargeEpoch = qpi.epoch() + (uint16)state.get()._idleWindowEpochs;
        }

        // Update configuration (mode stays the same)
        locals.gate.recipientCount = input.recipientCount;
        // Reset round robin index if it would be out of bounds after recipient count change
        if (locals.gate.mode == QUGATE_MODE_ROUND_ROBIN && locals.gate.roundRobinIndex >= input.recipientCount)
        {
            locals.gate.roundRobinIndex = 0;
        }
        locals.gate.threshold = input.threshold;

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
        }

        // Build allowed-senders config (side array)
        locals.asCfg.count = input.allowedSenderCount;
        for (locals.i = 0; locals.i < QUGATE_MAX_RECIPIENTS; locals.i++)
        {
            if (locals.i < input.allowedSenderCount)
            {
                locals.asCfg.senders.set(locals.i, input.allowedSenders.get(locals.i));
            }
            else
            {
                locals.asCfg.senders.set(locals.i, id::zero());
            }
        }

        // Validate gate-as-recipient IDs
        for (locals.i = 0; locals.i < input.recipientCount; locals.i++)
        {
            if (input.recipientGateIds.get(locals.i) >= 0)
            {
                locals.rgSlot = (uint64)(input.recipientGateIds.get(locals.i)) & QUGATE_GATE_ID_SLOT_MASK;
                locals.rgGen = (uint64)(input.recipientGateIds.get(locals.i)) >> QUGATE_GATE_ID_SLOT_BITS;
                if (locals.rgSlot >= state.get()._gateCount
                    || locals.rgGen == 0
                    || state.get()._gateGenerations.get(locals.rgSlot) != (uint16)(locals.rgGen - 1)
                    || state.get()._gates.get(locals.rgSlot).active == 0)
                {
                    if (locals.invReward > 0) qpi.transfer(qpi.invocator(), locals.invReward);
                    output.status = QUGATE_INVALID_GATE_RECIPIENT;
                    locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                    LOG_WARNING(locals.logger);
                    return;
                }
            }
        }

        // All validation passed — charge anti-spam fee (1,000 QU burned)
        if (locals.invReward < QUGATE_CHAIN_HOP_FEE)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_INSUFFICIENT_FEE;
            locals.logger._type = QUGATE_LOG_FAIL_INSUFFICIENT_FEE;
            LOG_INFO(locals.logger);
            return;
        }
        qpi.burn(QUGATE_CHAIN_HOP_FEE);
        state.mut()._totalBurned += QUGATE_CHAIN_HOP_FEE;
        if (locals.invReward > QUGATE_CHAIN_HOP_FEE)
        {
            qpi.transfer(qpi.invocator(), locals.invReward - QUGATE_CHAIN_HOP_FEE);
        }

        state.mut()._gates.set(locals.slotIdx, locals.gate);
        state.mut()._allowedSendersConfigs.set(locals.slotIdx, locals.asCfg);

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
    // fundGate — add QU to gate's unified reserve
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
            if (locals.gate.reserve > 0)
            {
                if (qpi.transfer(locals.gate.owner, locals.gate.reserve) >= 0) // [QG-04]
                {
                    locals.gate.reserve = 0;
                }
            }
            state.mut()._gates.set(locals.slotIdx, locals.gate);
            if (locals.gate.currentBalance > 0 || locals.gate.reserve > 0)
            {
                if (locals.invReward > 0) qpi.transfer(qpi.invocator(), locals.invReward);
                output.result = QUGATE_GATE_NOT_ACTIVE;
                locals.logger._type = QUGATE_LOG_GATE_EXPIRED;
                LOG_INFO(locals.logger);
                return;
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

        locals.gate.reserve += locals.invReward;

        state.mut()._gates.set(locals.slotIdx, locals.gate);
        output.result = QUGATE_SUCCESS;
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
        output.lastActivityEpoch = locals.gate.lastActivityEpoch;

        locals.asCfg = state.get()._allowedSendersConfigs.get(locals.slotIdx);
        for (locals.i = 0; locals.i < QUGATE_MAX_RECIPIENTS; locals.i++)
        {
            output.recipients.set(locals.i, locals.gate.recipients.get(locals.i));
            output.ratios.set(locals.i, locals.gate.ratios.get(locals.i));
            output.allowedSenders.set(locals.i, locals.asCfg.senders.get(locals.i));
        }
        output.allowedSenderCount = locals.asCfg.count;
        output.chainNextGateId = locals.gate.chainNextGateId;
        output.chainDepth = locals.gate.chainDepth;
        output.reserve = locals.gate.reserve;
        output.nextIdleChargeEpoch = locals.gate.nextIdleChargeEpoch;
        output.adminGateId = locals.gate.adminGateId;
        output.governancePolicy = locals.gate.governancePolicy;
        output.hasAdminGate = (locals.gate.adminGateId >= 0) ? 1 : 0;
        locals.delinquentEpoch = state.get()._idleDelinquentEpochs.get(locals.slotIdx);
        output.idleDelinquent = (locals.delinquentEpoch > 0 ? 1 : 0);
        output.idleGraceRemainingEpochs = 0;
        output.idleExpiryOverdue = 0;
        if (locals.delinquentEpoch > 0 && state.get()._idleGraceEpochs > 0)
        {
            if (qpi.epoch() - locals.delinquentEpoch >= state.get()._idleGraceEpochs)
            {
                output.idleExpiryOverdue = 1;
            }
            else
            {
                output.idleGraceRemainingEpochs = (uint16)(state.get()._idleGraceEpochs - (qpi.epoch() - locals.delinquentEpoch));
            }
        }

        // Gate-as-recipient
        for (locals.i = 0; locals.i < 8; locals.i++)
        {
            output.recipientGateIds.set(locals.i, locals.gate.recipientGateIds.get(locals.i));
        }

        // Report as inactive if expired (function can't mutate state)
        if ((locals.delinquentEpoch > 0 && state.get()._idleGraceEpochs > 0
             && qpi.epoch() - locals.delinquentEpoch >= state.get()._idleGraceEpochs)
            || (state.get()._expiryEpochs > 0
                && qpi.epoch() - locals.gate.lastActivityEpoch >= state.get()._expiryEpochs))
        {
            output.active = 0;
        }
    }

    PUBLIC_FUNCTION(getGateCount)
    {
        output.totalGates = state.get()._gateCount;
        output.activeGates = state.get()._activeGates;
        output.totalBurned = state.get()._totalBurned;         //
        output.totalMaintenanceCharged = state.get()._totalMaintenanceCharged;
        output.totalMaintenanceBurned = state.get()._totalMaintenanceBurned;
        output.totalMaintenanceDividends = state.get()._totalMaintenanceDividends;
        output.distributedMaintenanceDividends = state.get()._distributedMaintenanceDividends;
    }

    PUBLIC_FUNCTION_WITH_LOCALS(getGatesByOwner)
    {
        output.count = 0;
        for (locals.i = 0; locals.i < state.get()._gateCount && output.count < QUGATE_MAX_OWNER_GATES; locals.i++)
        {
            if (state.get()._gates.get(locals.i).active == 1
                && state.get()._gates.get(locals.i).owner == input.owner)
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
                locals.entry.chainNextGateId = -1;
                locals.entry.reserve = 0;
                locals.entry.chainDepth = 0;
                locals.entry.nextIdleChargeEpoch = 0;
                locals.entry.allowedSenderCount = 0;
                locals.entry.adminGateId = -1;
                locals.entry.governancePolicy = QUGATE_GOVERNANCE_STRICT_ADMIN;
                locals.entry.hasAdminGate = 0;
                locals.entry.idleDelinquent = 0;
                locals.entry.idleGraceRemainingEpochs = 0;
                locals.entry.idleExpiryOverdue = 0;
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
                locals.entry.chainNextGateId = locals.gate.chainNextGateId;
                locals.entry.reserve = locals.gate.reserve;
                locals.entry.chainDepth = locals.gate.chainDepth;
                locals.entry.nextIdleChargeEpoch = locals.gate.nextIdleChargeEpoch;
                locals.asCfg = state.get()._allowedSendersConfigs.get(locals.slotIdx);
                locals.entry.allowedSenderCount = locals.asCfg.count;
                locals.entry.adminGateId = locals.gate.adminGateId;
                locals.entry.governancePolicy = locals.gate.governancePolicy;
                locals.entry.hasAdminGate = (locals.gate.adminGateId >= 0) ? 1 : 0;
                locals.delinquentEpoch = state.get()._idleDelinquentEpochs.get(locals.slotIdx);
                locals.entry.idleDelinquent = (locals.delinquentEpoch > 0 ? 1 : 0);
                locals.entry.idleGraceRemainingEpochs = 0;
                locals.entry.idleExpiryOverdue = 0;
                if (locals.delinquentEpoch > 0 && state.get()._idleGraceEpochs > 0)
                {
                    if (qpi.epoch() - locals.delinquentEpoch >= state.get()._idleGraceEpochs)
                    {
                        locals.entry.idleExpiryOverdue = 1;
                        locals.entry.active = 0;
                    }
                    else
                    {
                        locals.entry.idleGraceRemainingEpochs = (uint16)(state.get()._idleGraceEpochs - (qpi.epoch() - locals.delinquentEpoch));
                    }
                }

                for (locals.j = 0; locals.j < QUGATE_MAX_RECIPIENTS; locals.j++)
                {
                    locals.entry.recipients.set(locals.j, locals.gate.recipients.get(locals.j));
                    locals.entry.ratios.set(locals.j, locals.gate.ratios.get(locals.j));
                    locals.entry.allowedSenders.set(locals.j, locals.asCfg.senders.get(locals.j));
                    locals.entry.recipientGateIds.set(locals.j, locals.gate.recipientGateIds.get(locals.j));
                }

                if (state.get()._expiryEpochs > 0
                    && qpi.epoch() - locals.gate.lastActivityEpoch >= state.get()._expiryEpochs)
                {
                    locals.entry.active = 0;
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
        output.feeBurnBps = state.get()._feeBurnBps;
        output.idleFee = state.get()._idleFee;
        output.idleWindowEpochs = state.get()._idleWindowEpochs;
        output.idleGraceEpochs = state.get()._idleGraceEpochs;
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
        if (locals.gate.owner != qpi.invocator()
            || (locals.gate.adminGateId >= 0
                && locals.gate.governancePolicy == QUGATE_GOVERNANCE_STRICT_ADMIN))
        {
            locals.adminAuth = 0;
            if (locals.gate.adminGateId >= 0)
            {
                locals.adminCheckSlot = locals.gate.adminGateId & QUGATE_GATE_ID_SLOT_MASK;
                locals.adminCheckEncodedGen = (uint64)(locals.gate.adminGateId) >> QUGATE_GATE_ID_SLOT_BITS;
                if (locals.gate.adminGateId > 0
                    && locals.adminCheckSlot < state.get()._gateCount
                    && locals.adminCheckEncodedGen > 0
                    && state.get()._gateGenerations.get(locals.adminCheckSlot) == (uint16)(locals.adminCheckEncodedGen - 1))
                {
                    locals.adminCheckGate = state.get()._gates.get(locals.adminCheckSlot);
                    if (locals.adminCheckGate.active && locals.adminCheckGate.mode == QUGATE_MODE_MULTISIG)
                    {
                        locals.adminCheckApproval = state.get()._adminApprovalStates.get(locals.adminCheckSlot);
                        if (locals.adminCheckApproval.active == 1)
                        {
                            if ((uint32)qpi.epoch() <= locals.adminCheckApproval.validUntilEpoch)
                            {
                                locals.adminAuth = 1;
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
            if (locals.adminAuth == 0)
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
            if (locals.gate.reserve > 0)
            {
                if (qpi.transfer(locals.gate.owner, locals.gate.reserve) >= 0) // [QG-05]
                {
                    locals.gate.reserve = 0;
                }
            }
            state.mut()._gates.set(locals.slotIdx, locals.gate);
            if (locals.gate.currentBalance > 0 || locals.gate.reserve > 0)
            {
                if (locals.invReward > 0) qpi.transfer(qpi.invocator(), locals.invReward);
                output.result = QUGATE_GATE_NOT_ACTIVE;
                locals.logger._type = QUGATE_LOG_GATE_EXPIRED;
                LOG_INFO(locals.logger);
                return;
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
            state.mut()._totalBurned += QUGATE_CHAIN_HOP_FEE;
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
            locals.nextWalkSlot = (uint64)(locals.walkGate.chainNextGateId) & QUGATE_GATE_ID_SLOT_MASK;
            locals.nextWalkGen = (uint64)(locals.walkGate.chainNextGateId) >> QUGATE_GATE_ID_SLOT_BITS;
            if (locals.walkGate.chainNextGateId <= 0
                || locals.nextWalkSlot >= state.get()._gateCount
                || locals.nextWalkGen == 0
                || state.get()._gateGenerations.get(locals.nextWalkSlot) != (uint16)(locals.nextWalkGen - 1))
            {
                break;
            }
            locals.walkSlot = locals.nextWalkSlot;
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
        state.mut()._totalBurned += QUGATE_CHAIN_HOP_FEE;
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

        // All validation happens before fee charging — rejected calls are fully refunded
        locals.slotIdx = input.gateId & QUGATE_GATE_ID_SLOT_MASK;
        locals.encodedGen = (input.gateId >> QUGATE_GATE_ID_SLOT_BITS);
        if (input.gateId == 0
            || locals.slotIdx >= state.get()._gateCount
            || locals.encodedGen == 0
            || state.get()._gateGenerations.get(locals.slotIdx) != (uint16)(locals.encodedGen - 1))
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_INVALID_GATE_ID;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_GATE;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.gate = state.get()._gates.get(locals.slotIdx);

        // Authorization: owner OR admin gate approval
        locals.adminApprovalUsed = 0;
        if (locals.gate.owner != qpi.invocator()
            || (locals.gate.adminGateId >= 0
                && locals.gate.governancePolicy == QUGATE_GOVERNANCE_STRICT_ADMIN))
        {
            locals.adminAuth = 0;
            if (locals.gate.adminGateId >= 0)
            {
                locals.adminCheckSlot = locals.gate.adminGateId & QUGATE_GATE_ID_SLOT_MASK;
                locals.adminCheckEncodedGen = (uint64)(locals.gate.adminGateId) >> QUGATE_GATE_ID_SLOT_BITS;
                if (locals.gate.adminGateId > 0
                    && locals.adminCheckSlot < state.get()._gateCount
                    && locals.adminCheckEncodedGen > 0
                    && state.get()._gateGenerations.get(locals.adminCheckSlot) == (uint16)(locals.adminCheckEncodedGen - 1))
                {
                    locals.adminCheckGate = state.get()._gates.get(locals.adminCheckSlot);
                    if (locals.adminCheckGate.active && locals.adminCheckGate.mode == QUGATE_MODE_MULTISIG)
                    {
                        locals.adminCheckApproval = state.get()._adminApprovalStates.get(locals.adminCheckSlot);
                        if (locals.adminCheckApproval.active == 1)
                        {
                            if ((uint32)qpi.epoch() <= locals.adminCheckApproval.validUntilEpoch)
                            {
                                locals.adminAuth = 1;
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
            if (locals.adminAuth == 0)
            {
                if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
                output.status = QUGATE_UNAUTHORIZED;
                locals.logger._type = QUGATE_LOG_FAIL_UNAUTHORIZED;
                LOG_WARNING(locals.logger);
                return;
            }
        }

        // Gate must be active
        if (locals.gate.active == 0)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Gate must be HEARTBEAT mode
        if (locals.gate.mode != QUGATE_MODE_HEARTBEAT)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_HEARTBEAT_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate thresholdEpochs >= 1
        if (input.thresholdEpochs == 0)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_HEARTBEAT_INVALID;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate payoutPercentPerEpoch 1-100
        if (input.payoutPercentPerEpoch == 0 || input.payoutPercentPerEpoch > 100)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_HEARTBEAT_INVALID;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate beneficiaries: allow chain-only heartbeat config when a chain target exists.
        if (input.beneficiaryCount > 8 || (input.beneficiaryCount == 0 && locals.gate.chainNextGateId == -1))
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_HEARTBEAT_INVALID;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate shares sum to 100 when direct beneficiaries are configured.
        locals.shareSum = 0;
        for (locals.i = 0; locals.i < input.beneficiaryCount; locals.i++)
        {
            locals.shareSum += input.beneficiaryShares.get(locals.i);
        }
        if (input.beneficiaryCount > 0 && locals.shareSum != 100)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_HEARTBEAT_INVALID;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // All validation passed — now charge the threshold-scaled configuration fee.
        // creationFee * (1 + thresholdEpochs / idleWindowEpochs)
        locals.configFee = 0;
        if (input.thresholdEpochs > 0 && state.get()._idleWindowEpochs > 0)
        {
            locals.configFee = state.get()._creationFee
                * (1 + QPI::div((uint64)input.thresholdEpochs, state.get()._idleWindowEpochs));
        }
        if (locals.invReward < (sint64)locals.configFee)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_INSUFFICIENT_FEE;
            locals.logger._type = QUGATE_LOG_FAIL_INSUFFICIENT_FEE;
            LOG_INFO(locals.logger);
            return;
        }
        if (locals.configFee > 0)
        {
            locals.configBurn = QPI::div(locals.configFee * state.get()._feeBurnBps, 10000ULL);
            locals.configDividend = locals.configFee - locals.configBurn;
            qpi.burn(locals.configBurn);
            state.mut()._totalBurned += locals.configBurn;
            state.mut()._earnedMaintenanceDividends += locals.configDividend;
            state.mut()._totalMaintenanceDividends += locals.configDividend;
        }
        if (locals.invReward > (sint64)locals.configFee)
        {
            qpi.transfer(qpi.invocator(), locals.invReward - (sint64)locals.configFee);
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
        if (state.get()._idleWindowEpochs > 0)
        {
            locals.gate.nextIdleChargeEpoch = qpi.epoch() + (uint16)state.get()._idleWindowEpochs;
        }
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
        output.feePaid = 0;

        locals.logger._contractIndex = CONTRACT_INDEX;
        locals.logger.sender = qpi.invocator();
        locals.logger.gateId = input.gateId;
        locals.logger.amount = 0;

        // Decode versioned gateId first — we need the gate data to compute the fee
        locals.slotIdx = input.gateId & QUGATE_GATE_ID_SLOT_MASK;
        locals.encodedGen = (input.gateId >> QUGATE_GATE_ID_SLOT_BITS);
        if (input.gateId == 0
            || locals.slotIdx >= state.get()._gateCount
            || locals.encodedGen == 0
            || state.get()._gateGenerations.get(locals.slotIdx) != (uint16)(locals.encodedGen - 1))
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_INVALID_GATE_ID;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_GATE;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.gate = state.get()._gates.get(locals.slotIdx);

        // Must be gate owner
        if (locals.gate.owner != qpi.invocator())
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_UNAUTHORIZED;
            locals.logger._type = QUGATE_LOG_FAIL_UNAUTHORIZED;
            LOG_WARNING(locals.logger);
            return;
        }

        // Gate must be active
        if (locals.gate.active == 0)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Gate must be HEARTBEAT mode
        if (locals.gate.mode != QUGATE_MODE_HEARTBEAT)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_HEARTBEAT_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.cfg = state.get()._heartbeatConfigs.get(locals.slotIdx);

        // Heartbeat must be configured (active)
        if (locals.cfg.active == 0)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_HEARTBEAT_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Cannot heartbeat after trigger
        if (locals.cfg.triggered == 1)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_HEARTBEAT_TRIGGERED;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Compute maintenance cost: gate's own idle fee + downstream drain + admin drain + surcharge
        // This is the same calculation as END_EPOCH idle charging.
        locals.ownMultiplierBps = QUGATE_IDLE_HEARTBEAT_MULTIPLIER_BPS;
        if (locals.gate.recipientCount >= QUGATE_MAX_RECIPIENTS)
        {
            locals.ownMultiplierBps = QUGATE_IDLE_MAX_RECIPIENT_MULTIPLIER_BPS;
        }
        else if (locals.gate.recipientCount >= QUGATE_IDLE_MULTI_RECIPIENT_THRESHOLD && locals.ownMultiplierBps < QUGATE_IDLE_MULTI_RECIPIENT_MULTIPLIER_BPS)
        {
            locals.ownMultiplierBps = QUGATE_IDLE_MULTI_RECIPIENT_MULTIPLIER_BPS;
        }
        if (locals.gate.chainNextGateId >= 0)
        {
            locals.ownMultiplierBps += QUGATE_IDLE_CHAIN_EXTRA_BPS;
        }
        locals.ownIdleFee = QPI::div(state.get()._idleFee * locals.ownMultiplierBps, 10000ULL);

        // Count and compute downstream drain (chain target + gate-as-recipients)
        locals.downstreamCount = 0;
        locals.downstreamTotalFee = 0;
        if (locals.gate.chainNextGateId >= 0)
        {
            locals.downstreamSlot = (uint64)(locals.gate.chainNextGateId) & QUGATE_GATE_ID_SLOT_MASK;
            locals.downstreamGen = (uint64)(locals.gate.chainNextGateId) >> QUGATE_GATE_ID_SLOT_BITS;
            if (locals.downstreamSlot < state.get()._gateCount
                && locals.downstreamGen > 0
                && state.get()._gateGenerations.get(locals.downstreamSlot) == (uint16)(locals.downstreamGen - 1))
            {
                locals.downstreamGate = state.get()._gates.get(locals.downstreamSlot);
                if (locals.downstreamGate.active == 1)
                {
                    locals.downstreamMultBps = QUGATE_IDLE_BASE_MULTIPLIER_BPS;
                    if (locals.downstreamGate.recipientCount >= QUGATE_MAX_RECIPIENTS) locals.downstreamMultBps = QUGATE_IDLE_MAX_RECIPIENT_MULTIPLIER_BPS;
                    else if (locals.downstreamGate.recipientCount >= QUGATE_IDLE_MULTI_RECIPIENT_THRESHOLD) locals.downstreamMultBps = QUGATE_IDLE_MULTI_RECIPIENT_MULTIPLIER_BPS;
                    if (locals.downstreamGate.mode == QUGATE_MODE_HEARTBEAT && locals.downstreamMultBps < QUGATE_IDLE_HEARTBEAT_MULTIPLIER_BPS) locals.downstreamMultBps = QUGATE_IDLE_HEARTBEAT_MULTIPLIER_BPS;
                    if (locals.downstreamGate.mode == QUGATE_MODE_MULTISIG && locals.downstreamMultBps < QUGATE_IDLE_MULTISIG_MULTIPLIER_BPS) locals.downstreamMultBps = QUGATE_IDLE_MULTISIG_MULTIPLIER_BPS;
                    if (locals.downstreamGate.chainNextGateId >= 0) locals.downstreamMultBps += QUGATE_IDLE_CHAIN_EXTRA_BPS;
                    locals.downstreamTotalFee += QPI::div(state.get()._idleFee * locals.downstreamMultBps, 10000ULL);
                    locals.downstreamCount++;
                }
            }
        }
        for (locals.dsIdx = 0; locals.dsIdx < locals.gate.recipientCount; locals.dsIdx++)
        {
            if (locals.gate.recipientGateIds.get(locals.dsIdx) >= 0)
            {
                locals.downstreamSlot = (uint64)(locals.gate.recipientGateIds.get(locals.dsIdx)) & QUGATE_GATE_ID_SLOT_MASK;
                locals.downstreamGen = (uint64)(locals.gate.recipientGateIds.get(locals.dsIdx)) >> QUGATE_GATE_ID_SLOT_BITS;
                if (locals.downstreamSlot < state.get()._gateCount
                    && locals.downstreamGen > 0
                    && state.get()._gateGenerations.get(locals.downstreamSlot) == (uint16)(locals.downstreamGen - 1))
                {
                    locals.downstreamGate = state.get()._gates.get(locals.downstreamSlot);
                    if (locals.downstreamGate.active == 1)
                    {
                        locals.downstreamMultBps = QUGATE_IDLE_BASE_MULTIPLIER_BPS;
                        if (locals.downstreamGate.recipientCount >= QUGATE_MAX_RECIPIENTS) locals.downstreamMultBps = QUGATE_IDLE_MAX_RECIPIENT_MULTIPLIER_BPS;
                        else if (locals.downstreamGate.recipientCount >= QUGATE_IDLE_MULTI_RECIPIENT_THRESHOLD) locals.downstreamMultBps = QUGATE_IDLE_MULTI_RECIPIENT_MULTIPLIER_BPS;
                        if (locals.downstreamGate.mode == QUGATE_MODE_HEARTBEAT && locals.downstreamMultBps < QUGATE_IDLE_HEARTBEAT_MULTIPLIER_BPS) locals.downstreamMultBps = QUGATE_IDLE_HEARTBEAT_MULTIPLIER_BPS;
                        if (locals.downstreamGate.mode == QUGATE_MODE_MULTISIG && locals.downstreamMultBps < QUGATE_IDLE_MULTISIG_MULTIPLIER_BPS) locals.downstreamMultBps = QUGATE_IDLE_MULTISIG_MULTIPLIER_BPS;
                        if (locals.downstreamGate.chainNextGateId >= 0) locals.downstreamMultBps += QUGATE_IDLE_CHAIN_EXTRA_BPS;
                        locals.downstreamTotalFee += QPI::div(state.get()._idleFee * locals.downstreamMultBps, 10000ULL);
                        locals.downstreamCount++;
                    }
                }
            }
        }

        // Shielding surcharge
        locals.surcharge = 0;
        if (locals.downstreamCount > 0)
        {
            locals.surcharge = QPI::div(state.get()._idleFee * locals.downstreamCount * QUGATE_IDLE_SHIELD_PER_TARGET_BPS, 10000ULL);
        }

        // Admin gate fee
        locals.adminFee = 0;
        if (locals.gate.adminGateId >= 0)
        {
            locals.downstreamSlot = (uint64)(locals.gate.adminGateId) & QUGATE_GATE_ID_SLOT_MASK;
            locals.downstreamGen = (uint64)(locals.gate.adminGateId) >> QUGATE_GATE_ID_SLOT_BITS;
            if (locals.downstreamSlot < state.get()._gateCount
                && locals.downstreamGen > 0
                && state.get()._gateGenerations.get(locals.downstreamSlot) == (uint16)(locals.downstreamGen - 1)
                && state.get()._gates.get(locals.downstreamSlot).active == 1)
            {
                locals.adminFee = QPI::div(state.get()._idleFee * QUGATE_IDLE_MULTISIG_MULTIPLIER_BPS, 10000ULL);
            }
        }

        // Total full-cycle maintenance cost
        locals.maintenanceCost = locals.ownIdleFee + locals.downstreamTotalFee + locals.surcharge + locals.adminFee;

        // Pro-rate by elapsed time since last heartbeat:
        // fee = max(QUGATE_HEARTBEAT_PING_FEE, fullCost * elapsedEpochs / idleWindow)
        // This ensures diligent pingers pay proportionally, not a full cycle per ping.
        locals.elapsedEpochs = (uint64)(qpi.epoch() - locals.cfg.lastHeartbeatEpoch);
        if (locals.elapsedEpochs == 0)
        {
            locals.elapsedEpochs = 1;
        }
        if (state.get()._idleWindowEpochs > 0 && locals.elapsedEpochs < state.get()._idleWindowEpochs)
        {
            locals.proratedCost = QPI::div(locals.maintenanceCost * locals.elapsedEpochs, state.get()._idleWindowEpochs);
        }
        else
        {
            locals.proratedCost = locals.maintenanceCost;
        }
        // Floor at legacy ping fee to prevent spam/zero-fee pings
        if (locals.proratedCost < (uint64)QUGATE_HEARTBEAT_PING_FEE)
        {
            locals.proratedCost = (uint64)QUGATE_HEARTBEAT_PING_FEE;
        }
        locals.maintenanceCost = locals.proratedCost;

        // Charge the pro-rated maintenance cost
        if (locals.invReward < (sint64)locals.maintenanceCost)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_INSUFFICIENT_FEE;
            locals.logger._type = QUGATE_LOG_FAIL_INSUFFICIENT_FEE;
            locals.logger.amount = locals.maintenanceCost;
            LOG_INFO(locals.logger);
            return;
        }

        // Apply burn/dividend split to the maintenance fee
        locals.burnAmount = QPI::div(locals.maintenanceCost * state.get()._feeBurnBps, 10000ULL);
        locals.dividendAmount = locals.maintenanceCost - locals.burnAmount;
        qpi.burn(locals.burnAmount);
        state.mut()._totalBurned += locals.burnAmount;
        state.mut()._totalMaintenanceBurned += locals.burnAmount;
        state.mut()._earnedMaintenanceDividends += locals.dividendAmount;
        state.mut()._totalMaintenanceDividends += locals.dividendAmount;
        state.mut()._totalMaintenanceCharged += locals.maintenanceCost;
        output.feePaid = locals.maintenanceCost;

        // Refund excess
        if (locals.invReward > (sint64)locals.maintenanceCost)
        {
            qpi.transfer(qpi.invocator(), locals.invReward - (sint64)locals.maintenanceCost);
        }

        // Reset the epoch counter
        locals.cfg.lastHeartbeatEpoch = (uint32)qpi.epoch();
        state.mut()._heartbeatConfigs.set(locals.slotIdx, locals.cfg);

        // Update gate activity
        locals.gate.lastActivityEpoch = qpi.epoch();
        if (state.get()._idleWindowEpochs > 0)
        {
            locals.gate.nextIdleChargeEpoch = qpi.epoch() + (uint16)state.get()._idleWindowEpochs;
        }
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

        // Decode versioned gateId
        locals.slotIdx = input.gateId & QUGATE_GATE_ID_SLOT_MASK;
        locals.encodedGen = input.gateId >> QUGATE_GATE_ID_SLOT_BITS;
        if (input.gateId == 0
            || locals.slotIdx >= state.get()._gateCount
            || locals.encodedGen == 0
            || state.get()._gateGenerations.get(locals.slotIdx) != (uint16)(locals.encodedGen - 1))
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_INVALID_GATE_ID;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_GATE;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.gate = state.get()._gates.get(locals.slotIdx);

        // Authorization: owner OR admin gate approval
        locals.adminApprovalUsed = 0;
        if (locals.gate.owner != qpi.invocator()
            || (locals.gate.adminGateId >= 0
                && locals.gate.governancePolicy == QUGATE_GOVERNANCE_STRICT_ADMIN))
        {
            locals.adminAuth = 0;
            if (locals.gate.adminGateId >= 0)
            {
                locals.adminCheckSlot = locals.gate.adminGateId & QUGATE_GATE_ID_SLOT_MASK;
                locals.adminCheckEncodedGen = (uint64)(locals.gate.adminGateId) >> QUGATE_GATE_ID_SLOT_BITS;
                if (locals.gate.adminGateId > 0
                    && locals.adminCheckSlot < state.get()._gateCount
                    && locals.adminCheckEncodedGen > 0
                    && state.get()._gateGenerations.get(locals.adminCheckSlot) == (uint16)(locals.adminCheckEncodedGen - 1))
                {
                    locals.adminCheckGate = state.get()._gates.get(locals.adminCheckSlot);
                    if (locals.adminCheckGate.active && locals.adminCheckGate.mode == QUGATE_MODE_MULTISIG)
                    {
                        locals.adminCheckApproval = state.get()._adminApprovalStates.get(locals.adminCheckSlot);
                        if (locals.adminCheckApproval.active == 1)
                        {
                            if ((uint32)qpi.epoch() <= locals.adminCheckApproval.validUntilEpoch)
                            {
                                locals.adminAuth = 1;
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
            if (locals.adminAuth == 0)
            {
                if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
                output.status = QUGATE_UNAUTHORIZED;
                locals.logger._type = QUGATE_LOG_FAIL_UNAUTHORIZED;
                LOG_WARNING(locals.logger);
                return;
            }
        }

        // Gate must be active
        if (locals.gate.active == 0)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Gate must be MULTISIG mode
        if (locals.gate.mode != QUGATE_MODE_MULTISIG)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_MULTISIG_INVALID_CONFIG;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Prevent reconfiguration while a proposal is active
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
                if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
                output.status = QUGATE_MULTISIG_PROPOSAL_ACTIVE;
                locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                LOG_WARNING(locals.logger);
                return;
            }
        }

        // Validate guardianCount 1-8
        if (input.guardianCount == 0 || input.guardianCount > 8)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_MULTISIG_INVALID_CONFIG;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate required 1-guardianCount
        if (input.required == 0 || input.required > input.guardianCount)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_MULTISIG_INVALID_CONFIG;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate proposalExpiryEpochs >= 1
        if (input.proposalExpiryEpochs == 0)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_MULTISIG_INVALID_CONFIG;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Validate adminApprovalWindowEpochs >= 1
        if (input.adminApprovalWindowEpochs == 0)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
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
                    if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
                    output.status = QUGATE_MULTISIG_INVALID_CONFIG;
                    locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                    LOG_WARNING(locals.logger);
                    return;
                }
            }
        }

        // All validation passed — charge anti-spam fee (1,000 QU burned)
        if (locals.invReward < QUGATE_CHAIN_HOP_FEE)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_INSUFFICIENT_FEE;
            locals.logger._type = QUGATE_LOG_FAIL_INSUFFICIENT_FEE;
            LOG_INFO(locals.logger);
            return;
        }
        qpi.burn(QUGATE_CHAIN_HOP_FEE);
        state.mut()._totalBurned += QUGATE_CHAIN_HOP_FEE;
        if (locals.invReward > QUGATE_CHAIN_HOP_FEE)
        {
            qpi.transfer(qpi.invocator(), locals.invReward - QUGATE_CHAIN_HOP_FEE);
        }

        // Store config, reset any active proposal
        for (locals.guardianLoopIdx = 0; locals.guardianLoopIdx < 8; locals.guardianLoopIdx++)
        {
            if (locals.guardianLoopIdx < input.guardianCount)
            {
                locals.cfg.guardians.set(locals.guardianLoopIdx, input.guardians.get(locals.guardianLoopIdx));
            }
            else
            {
                locals.cfg.guardians.set(locals.guardianLoopIdx, id::zero());
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
        if (state.get()._idleWindowEpochs > 0)
        {
            locals.gate.nextIdleChargeEpoch = qpi.epoch() + (uint16)state.get()._idleWindowEpochs;
        }
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

        // Decode versioned gateId
        locals.slotIdx = input.gateId & QUGATE_GATE_ID_SLOT_MASK;
        locals.encodedGen = input.gateId >> QUGATE_GATE_ID_SLOT_BITS;
        if (input.gateId == 0
            || locals.slotIdx >= state.get()._gateCount
            || locals.encodedGen == 0
            || state.get()._gateGenerations.get(locals.slotIdx) != (uint16)(locals.encodedGen - 1))
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_INVALID_GATE_ID;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_GATE;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.gate = state.get()._gates.get(locals.slotIdx);

        // Authorization: owner OR admin gate approval
        locals.adminApprovalUsed = 0;
        if (locals.gate.owner != qpi.invocator()
            || (locals.gate.adminGateId >= 0
                && locals.gate.governancePolicy == QUGATE_GOVERNANCE_STRICT_ADMIN))
        {
            locals.adminAuth = 0;
            if (locals.gate.adminGateId >= 0)
            {
                locals.adminCheckSlot = locals.gate.adminGateId & QUGATE_GATE_ID_SLOT_MASK;
                locals.adminCheckEncodedGen = (uint64)(locals.gate.adminGateId) >> QUGATE_GATE_ID_SLOT_BITS;
                if (locals.gate.adminGateId > 0
                    && locals.adminCheckSlot < state.get()._gateCount
                    && locals.adminCheckEncodedGen > 0
                    && state.get()._gateGenerations.get(locals.adminCheckSlot) == (uint16)(locals.adminCheckEncodedGen - 1))
                {
                    locals.adminCheckGate = state.get()._gates.get(locals.adminCheckSlot);
                    if (locals.adminCheckGate.active && locals.adminCheckGate.mode == QUGATE_MODE_MULTISIG)
                    {
                        locals.adminCheckApproval = state.get()._adminApprovalStates.get(locals.adminCheckSlot);
                        if (locals.adminCheckApproval.active == 1)
                        {
                            if ((uint32)qpi.epoch() <= locals.adminCheckApproval.validUntilEpoch)
                            {
                                locals.adminAuth = 1;
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
            if (locals.adminAuth == 0)
            {
                if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
                output.status = QUGATE_UNAUTHORIZED;
                locals.logger._type = QUGATE_LOG_FAIL_UNAUTHORIZED;
                LOG_WARNING(locals.logger);
                return;
            }
        }

        // Gate must be active
        if (locals.gate.active == 0)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Gate must be TIME_LOCK mode
        if (locals.gate.mode != QUGATE_MODE_TIME_LOCK)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
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
                if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
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
                if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
                output.status = QUGATE_INVALID_PARAMS;
                locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                LOG_WARNING(locals.logger);
                return;
            }
        }
        else
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_INVALID_PARAMS;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Duration-scaled config fee: creationFee * (1 + lockDuration / idleWindow)
        if (input.lockMode == QUGATE_TIME_LOCK_ABSOLUTE_EPOCH)
        {
            locals.lockDuration = (uint64)(input.unlockEpoch - (uint32)qpi.epoch());
        }
        else
        {
            locals.lockDuration = (uint64)input.delayEpochs;
        }
        locals.configFee = 0;
        if (locals.lockDuration > 0 && state.get()._idleWindowEpochs > 0)
        {
            locals.configFee = state.get()._creationFee
                * (1 + QPI::div(locals.lockDuration, state.get()._idleWindowEpochs));
        }
        if (locals.invReward < (sint64)locals.configFee)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_INSUFFICIENT_FEE;
            locals.logger._type = QUGATE_LOG_FAIL_INSUFFICIENT_FEE;
            LOG_INFO(locals.logger);
            return;
        }
        if (locals.configFee > 0)
        {
            locals.configBurn = QPI::div(locals.configFee * state.get()._feeBurnBps, 10000ULL);
            locals.configDividend = locals.configFee - locals.configBurn;
            qpi.burn(locals.configBurn);
            state.mut()._totalBurned += locals.configBurn;
            state.mut()._earnedMaintenanceDividends += locals.configDividend;
            state.mut()._totalMaintenanceDividends += locals.configDividend;
        }
        if (locals.invReward > (sint64)locals.configFee)
        {
            qpi.transfer(qpi.invocator(), locals.invReward - (sint64)locals.configFee);
        }

        // Store config
        if (input.lockMode == QUGATE_TIME_LOCK_ABSOLUTE_EPOCH)
        {
            locals.cfg.unlockEpoch = input.unlockEpoch;
        }
        else
        {
            // If the gate already holds funds, anchor the relative unlock window immediately
            // from the current epoch rather than waiting for another deposit.
            if (locals.gate.currentBalance > 0)
            {
                locals.cfg.unlockEpoch = (uint32)qpi.epoch() + input.delayEpochs;
            }
            else
            {
                locals.cfg.unlockEpoch = 0;
            }
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
        if (state.get()._idleWindowEpochs > 0)
        {
            locals.gate.nextIdleChargeEpoch = qpi.epoch() + (uint16)state.get()._idleWindowEpochs;
        }
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

        // Decode versioned gateId
        locals.slotIdx = input.gateId & QUGATE_GATE_ID_SLOT_MASK;
        locals.encodedGen = input.gateId >> QUGATE_GATE_ID_SLOT_BITS;
        if (input.gateId == 0
            || locals.slotIdx >= state.get()._gateCount
            || locals.encodedGen == 0
            || state.get()._gateGenerations.get(locals.slotIdx) != (uint16)(locals.encodedGen - 1))
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_INVALID_GATE_ID;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_GATE;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.gate = state.get()._gates.get(locals.slotIdx);

        // Authorization: owner OR admin gate approval
        locals.adminApprovalUsed = 0;
        if (locals.gate.owner != qpi.invocator()
            || (locals.gate.adminGateId >= 0
                && locals.gate.governancePolicy == QUGATE_GOVERNANCE_STRICT_ADMIN))
        {
            locals.adminAuth = 0;
            if (locals.gate.adminGateId >= 0)
            {
                locals.adminCheckSlot = locals.gate.adminGateId & QUGATE_GATE_ID_SLOT_MASK;
                locals.adminCheckEncodedGen = (uint64)(locals.gate.adminGateId) >> QUGATE_GATE_ID_SLOT_BITS;
                if (locals.gate.adminGateId > 0
                    && locals.adminCheckSlot < state.get()._gateCount
                    && locals.adminCheckEncodedGen > 0
                    && state.get()._gateGenerations.get(locals.adminCheckSlot) == (uint16)(locals.adminCheckEncodedGen - 1))
                {
                    locals.adminCheckGate = state.get()._gates.get(locals.adminCheckSlot);
                    if (locals.adminCheckGate.active && locals.adminCheckGate.mode == QUGATE_MODE_MULTISIG)
                    {
                        locals.adminCheckApproval = state.get()._adminApprovalStates.get(locals.adminCheckSlot);
                        if (locals.adminCheckApproval.active == 1)
                        {
                            if ((uint32)qpi.epoch() <= locals.adminCheckApproval.validUntilEpoch)
                            {
                                locals.adminAuth = 1;
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
            if (locals.adminAuth == 0)
            {
                if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
                output.status = QUGATE_UNAUTHORIZED;
                locals.logger._type = QUGATE_LOG_FAIL_UNAUTHORIZED;
                LOG_WARNING(locals.logger);
                return;
            }
        }

        // Gate must be active
        if (locals.gate.active == 0)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Gate must be TIME_LOCK mode
        if (locals.gate.mode != QUGATE_MODE_TIME_LOCK)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_INVALID_MODE;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.cfg = state.get()._timeLockConfigs.get(locals.slotIdx);

        // Config must be active
        if (locals.cfg.active == 0)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Cannot cancel if already fired
        if (locals.cfg.fired == 1)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_TIME_LOCK_ALREADY_FIRED;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Cannot cancel if already cancelled
        if (locals.cfg.cancelled == 1)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Must be cancellable
        if (locals.cfg.cancellable == 0)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
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

        // Refund reserve to owner
        if (locals.gate.reserve > 0)
        {
            if (qpi.transfer(locals.gate.owner, locals.gate.reserve) >= 0) // [QG-18]
            {
                locals.gate.reserve = 0;
            }
        }

        state.mut()._gates.set(locals.slotIdx, locals.gate);

        if (locals.gate.currentBalance > 0 || locals.gate.reserve > 0)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_INVALID_PARAMS;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // All validation and owner refunds passed — charge anti-spam fee (1,000 QU burned)
        if (locals.invReward < QUGATE_CHAIN_HOP_FEE)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_INSUFFICIENT_FEE;
            locals.logger._type = QUGATE_LOG_FAIL_INSUFFICIENT_FEE;
            LOG_INFO(locals.logger);
            return;
        }
        qpi.burn(QUGATE_CHAIN_HOP_FEE);
        state.mut()._totalBurned += QUGATE_CHAIN_HOP_FEE;
        if (locals.invReward > QUGATE_CHAIN_HOP_FEE)
        {
            qpi.transfer(qpi.invocator(), locals.invReward - QUGATE_CHAIN_HOP_FEE);
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

        // Decode versioned gateId
        locals.slotIdx = input.gateId & QUGATE_GATE_ID_SLOT_MASK;
        locals.encodedGen = (input.gateId >> QUGATE_GATE_ID_SLOT_BITS);
        if (input.gateId == 0
            || locals.slotIdx >= state.get()._gateCount
            || locals.encodedGen == 0
            || state.get()._gateGenerations.get(locals.slotIdx) != (uint16)(locals.encodedGen - 1))
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_INVALID_GATE_ID;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_GATE;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.gate = state.get()._gates.get(locals.slotIdx);

        // Gate must be active
        if (locals.gate.active == 0)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_GATE_NOT_ACTIVE;
            locals.logger._type = QUGATE_LOG_FAIL_NOT_ACTIVE;
            LOG_WARNING(locals.logger);
            return;
        }

        // Authorization
        locals.adminApprovalUsed = 0;
        if (qpi.invocator() != locals.gate.owner
            || (locals.gate.adminGateId >= 0
                && locals.gate.governancePolicy == QUGATE_GOVERNANCE_STRICT_ADMIN))
        {
            // Changes require an active approval window from the current admin gate.
            if (locals.gate.adminGateId < 0)
            {
                if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
                output.status = QUGATE_UNAUTHORIZED;
                locals.logger._type = QUGATE_LOG_FAIL_UNAUTHORIZED;
                LOG_WARNING(locals.logger);
                return;
            }
            // The owner may clear a dead admin gate reference.
            if (qpi.invocator() == locals.gate.owner && input.adminGateId == -1)
            {
                locals.adminSlot = (uint64)(locals.gate.adminGateId) & QUGATE_GATE_ID_SLOT_MASK;
                locals.adminEncodedGen = (uint64)(locals.gate.adminGateId) >> QUGATE_GATE_ID_SLOT_BITS;
                locals.adminApproved = 0;
                if (locals.gate.adminGateId <= 0
                    || locals.adminSlot >= state.get()._gateCount
                    || locals.adminEncodedGen == 0
                    || state.get()._gateGenerations.get(locals.adminSlot) != (uint16)(locals.adminEncodedGen - 1))
                {
                    locals.adminApproved = 1;
                }
                else
                {
                    locals.adminGate = state.get()._gates.get(locals.adminSlot);
                    if (locals.adminGate.active == 0 || locals.adminGate.mode != QUGATE_MODE_MULTISIG)
                    {
                        locals.adminApproved = 1;
                    }
                }
                if (locals.adminApproved == 1)
                {
                    if (locals.invReward < QUGATE_CHAIN_HOP_FEE)
                    {
                        if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
                        output.status = QUGATE_INSUFFICIENT_FEE;
                        locals.logger._type = QUGATE_LOG_FAIL_INSUFFICIENT_FEE;
                        LOG_INFO(locals.logger);
                        return;
                    }
                    qpi.burn(QUGATE_CHAIN_HOP_FEE);
                    state.mut()._totalBurned += QUGATE_CHAIN_HOP_FEE;
                    if (locals.invReward > QUGATE_CHAIN_HOP_FEE)
                    {
                        qpi.transfer(qpi.invocator(), locals.invReward - QUGATE_CHAIN_HOP_FEE);
                    }
                    locals.gate.adminGateId = -1;
                    locals.gate.governancePolicy = QUGATE_GOVERNANCE_STRICT_ADMIN;
                    state.mut()._gates.set(locals.slotIdx, locals.gate);
                    output.status = QUGATE_SUCCESS;
                    locals.logger._type = QUGATE_LOG_ADMIN_GATE_CLEARED;
                    LOG_INFO(locals.logger);
                    return;
                }
            }
            // Check admin gate approval
            locals.adminSlot = locals.gate.adminGateId & QUGATE_GATE_ID_SLOT_MASK;
            locals.adminEncodedGen = (uint64)(locals.gate.adminGateId) >> QUGATE_GATE_ID_SLOT_BITS;
            locals.adminApprovalSlot = locals.adminSlot;
            locals.adminApproved = 0;
            locals.adminApprovalUsed = 0;
            if (locals.gate.adminGateId > 0
                && locals.adminSlot < state.get()._gateCount
                && locals.adminEncodedGen > 0
                && state.get()._gateGenerations.get(locals.adminSlot) == (uint16)(locals.adminEncodedGen - 1))
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
                if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
                output.status = QUGATE_ADMIN_GATE_REQUIRED;
                locals.logger._type = QUGATE_LOG_FAIL_UNAUTHORIZED;
                LOG_WARNING(locals.logger);
                return;
            }
        }
        // Clear admin gate
        if (input.adminGateId == -1)
        {
            if (locals.invReward < QUGATE_CHAIN_HOP_FEE)
            {
                if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
                output.status = QUGATE_INSUFFICIENT_FEE;
                locals.logger._type = QUGATE_LOG_FAIL_INSUFFICIENT_FEE;
                LOG_INFO(locals.logger);
                return;
            }
            qpi.burn(QUGATE_CHAIN_HOP_FEE);
            state.mut()._totalBurned += QUGATE_CHAIN_HOP_FEE;
            if (locals.invReward > QUGATE_CHAIN_HOP_FEE)
            {
                qpi.transfer(qpi.invocator(), locals.invReward - QUGATE_CHAIN_HOP_FEE);
            }
            locals.gate.adminGateId = -1;
            locals.gate.governancePolicy = QUGATE_GOVERNANCE_STRICT_ADMIN;
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
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_INVALID_ADMIN_GATE;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        locals.adminGate = state.get()._gates.get(locals.adminSlot);
        if (locals.adminGate.active == 0 || locals.adminGate.mode != QUGATE_MODE_MULTISIG)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_INVALID_ADMIN_GATE;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        if (input.governancePolicy != QUGATE_GOVERNANCE_STRICT_ADMIN
            && input.governancePolicy != QUGATE_GOVERNANCE_OWNER_OR_ADMIN)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_INVALID_PARAMS;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Prevent governing an admin-only multisig — admin gates govern fund-flow gates
        if (locals.gate.mode == QUGATE_MODE_MULTISIG && locals.gate.recipientCount == 0)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_INVALID_ADMIN_GATE;
            locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
            LOG_WARNING(locals.logger);
            return;
        }

        // Prevent self-referential admin gate
        if (locals.adminSlot == locals.slotIdx)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
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
            if (locals.walkGate.adminGateId < 0)
            {
                break;
            }
            locals.nextAdminSlot = (uint64)(locals.walkGate.adminGateId) & QUGATE_GATE_ID_SLOT_MASK;
            locals.nextAdminGen = (uint64)(locals.walkGate.adminGateId) >> QUGATE_GATE_ID_SLOT_BITS;
            if (locals.nextAdminSlot == locals.slotIdx)
            {
                if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
                output.status = QUGATE_INVALID_ADMIN_CYCLE;
                locals.logger._type = QUGATE_LOG_FAIL_INVALID_PARAMS;
                LOG_WARNING(locals.logger);
                return;
            }
            if (locals.walkGate.adminGateId <= 0
                || locals.nextAdminSlot >= state.get()._gateCount
                || locals.nextAdminGen == 0
                || state.get()._gateGenerations.get(locals.nextAdminSlot) != (uint16)(locals.nextAdminGen - 1))
            {
                break;
            }
            locals.walkSlot = locals.nextAdminSlot;
        }

        if (locals.invReward < QUGATE_CHAIN_HOP_FEE)
        {
            if (locals.invReward > 0) { qpi.transfer(qpi.invocator(), locals.invReward); }
            output.status = QUGATE_INSUFFICIENT_FEE;
            locals.logger._type = QUGATE_LOG_FAIL_INSUFFICIENT_FEE;
            LOG_INFO(locals.logger);
            return;
        }
        qpi.burn(QUGATE_CHAIN_HOP_FEE);
        state.mut()._totalBurned += QUGATE_CHAIN_HOP_FEE;
        if (locals.invReward > QUGATE_CHAIN_HOP_FEE)
        {
            qpi.transfer(qpi.invocator(), locals.invReward - QUGATE_CHAIN_HOP_FEE);
        }

        // Set admin gate
        locals.gate.adminGateId = input.adminGateId;
        locals.gate.governancePolicy = input.governancePolicy;
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
        output.governancePolicy = QUGATE_GOVERNANCE_STRICT_ADMIN;
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
        output.hasAdminGate = (locals.gate.adminGateId >= 0) ? 1 : 0;
        output.adminGateId = locals.gate.adminGateId;
        output.governancePolicy = locals.gate.governancePolicy;

        // If admin gate is set, look up its mode
        if (locals.gate.adminGateId >= 0)
        {
            locals.adminSlot = locals.gate.adminGateId & QUGATE_GATE_ID_SLOT_MASK;
            locals.adminEncodedGen = (uint64)(locals.gate.adminGateId) >> QUGATE_GATE_ID_SLOT_BITS;
            if (locals.gate.adminGateId > 0
                && locals.adminSlot < state.get()._gateCount
                && locals.adminEncodedGen > 0
                && state.get()._gateGenerations.get(locals.adminSlot) == (uint16)(locals.adminEncodedGen - 1))
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
    // withdrawReserve — withdraw from gate's unified reserve
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
            locals.adminAuth = 0;
            if (locals.gate.adminGateId >= 0)
            {
                locals.adminCheckSlot = locals.gate.adminGateId & QUGATE_GATE_ID_SLOT_MASK;
                locals.adminCheckEncodedGen = (uint64)(locals.gate.adminGateId) >> QUGATE_GATE_ID_SLOT_BITS;
                if (locals.gate.adminGateId > 0
                    && locals.adminCheckSlot < state.get()._gateCount
                    && locals.adminCheckEncodedGen > 0
                    && state.get()._gateGenerations.get(locals.adminCheckSlot) == (uint16)(locals.adminCheckEncodedGen - 1))
                {
                    locals.adminCheckGate = state.get()._gates.get(locals.adminCheckSlot);
                    if (locals.adminCheckGate.active && locals.adminCheckGate.mode == QUGATE_MODE_MULTISIG)
                    {
                        locals.adminCheckApproval = state.get()._adminApprovalStates.get(locals.adminCheckSlot);
                        if (locals.adminCheckApproval.active == 1)
                        {
                            if ((uint32)qpi.epoch() <= locals.adminCheckApproval.validUntilEpoch)
                            {
                                locals.adminAuth = 1;
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
            if (locals.adminAuth == 0)
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

        locals.available = locals.gate.reserve;
        if (locals.available <= 0)
        {
            output.status = QUGATE_SUCCESS;
            output.withdrawn = 0;
            return;
        }

        locals.toWithdraw = (uint64)locals.available;
        if (input.amount != 0 && input.amount < locals.toWithdraw)
        {
            locals.toWithdraw = input.amount;
        }

        if (qpi.transfer(qpi.invocator(), locals.toWithdraw) >= 0) // [QG-19]
        {
            locals.gate.reserve -= (sint64)locals.toWithdraw;
            state.mut()._gates.set(locals.slotIdx, locals.gate);
            output.withdrawn = locals.toWithdraw;
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
        locals.gate = state.get()._gates.get(input.slotIndex);

        // Active gates: gateId = (generation+1) << SLOT_BITS | slot (current generation).
        // Inactive gates: the generation counter was already bumped at close/recycle,
        // so use the previous generation to match the stale slot data being returned.
        if (locals.gate.active)
        {
            output.gateId = ((uint64)(output.generation + 1) << QUGATE_GATE_ID_SLOT_BITS) | input.slotIndex;
        }
        else
        {
            output.gateId = (output.generation > 0)
                ? (((uint64)output.generation) << QUGATE_GATE_ID_SLOT_BITS) | input.slotIndex
                : 0;
        }
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
        locals.asCfg = state.get()._allowedSendersConfigs.get(input.slotIndex);
        output.allowedSenderCount = locals.asCfg.count;
        output.chainNextGateId = locals.gate.chainNextGateId;
        output.reserve = locals.gate.reserve;
        output.chainDepth = locals.gate.chainDepth;
        output.nextIdleChargeEpoch = locals.gate.nextIdleChargeEpoch;
        output.adminGateId = locals.gate.adminGateId;
        output.governancePolicy = locals.gate.governancePolicy;
        output.hasAdminGate = (locals.gate.adminGateId >= 0) ? 1 : 0;
        locals.delinquentEpoch = state.get()._idleDelinquentEpochs.get(input.slotIndex);
        output.idleDelinquent = (locals.delinquentEpoch > 0 ? 1 : 0);
        output.idleGraceRemainingEpochs = 0;
        output.idleExpiryOverdue = 0;
        if (locals.delinquentEpoch > 0 && state.get()._idleGraceEpochs > 0)
        {
            if (qpi.epoch() - locals.delinquentEpoch >= state.get()._idleGraceEpochs)
            {
                output.idleExpiryOverdue = 1;
                output.active = 0;
            }
            else
            {
                output.idleGraceRemainingEpochs = (uint16)(state.get()._idleGraceEpochs - (qpi.epoch() - locals.delinquentEpoch));
            }
        }

        for (locals.i = 0; locals.i < 8; locals.i++)
        {
            output.recipients.set(locals.i, locals.gate.recipients.get(locals.i));
            output.ratios.set(locals.i, locals.gate.ratios.get(locals.i));
            output.allowedSenders.set(locals.i, locals.asCfg.senders.get(locals.i));
            output.recipientGateIds.set(locals.i, locals.gate.recipientGateIds.get(locals.i));
        }

        if (state.get()._expiryEpochs > 0
            && qpi.epoch() - locals.gate.lastActivityEpoch >= state.get()._expiryEpochs)
        {
            output.active = 0;
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
    }

    // =============================================
    // System procedures
    // =============================================

    INITIALIZE_WITH_LOCALS()
    {
        state.mut()._gateCount = 0;
        state.mut()._activeGates = 0;
        state.mut()._freeCount = 0;
        state.mut()._totalBurned = 0;                           //
        state.mut()._creationFee = QUGATE_DEFAULT_CREATION_FEE;  //
        state.mut()._feeBurnBps = QUGATE_DEFAULT_FEE_BURN_BPS;
        state.mut()._idleFee = QUGATE_DEFAULT_MAINTENANCE_FEE;
        state.mut()._idleWindowEpochs = QUGATE_DEFAULT_MAINTENANCE_INTERVAL_EPOCHS;
        state.mut()._idleGraceEpochs = QUGATE_DEFAULT_MAINTENANCE_GRACE_EPOCHS;
        state.mut()._minSendAmount = QUGATE_DEFAULT_MIN_SEND;    //
        state.mut()._expiryEpochs = QUGATE_DEFAULT_EXPIRY_EPOCHS; //
        state.mut()._totalMaintenanceCharged = 0;
        state.mut()._totalMaintenanceBurned = 0;
        state.mut()._totalMaintenanceDividends = 0;
        state.mut()._earnedMaintenanceDividends = 0;
        state.mut()._distributedMaintenanceDividends = 0;

        // Zero all generation counters
        for (locals.i = 0; locals.i < QUGATE_MAX_GATES; locals.i++)
        {
            state.mut()._gateGenerations.set(locals.i, 0);
            state.mut()._idleDelinquentEpochs.set(locals.i, 0);
        }
    }

    BEGIN_EPOCH_WITH_LOCALS()
    {
    }

    END_EPOCH_WITH_LOCALS()
    {
        // Inactivity maintenance charging and delinquency tracking
        for (locals.i = 0; locals.i < state.get()._gateCount; locals.i++)
        {
            locals.gate = state.get()._gates.get(locals.i);
            if (locals.gate.active == 0)
            {
                continue;
            }

            locals.maintenanceEligible = 1;
            if (locals.gate.mode == QUGATE_MODE_HEARTBEAT)
            {
                locals.inhCfg = state.get()._heartbeatConfigs.get(locals.i);
                if (locals.inhCfg.active == 0)
                {
                    locals.maintenanceEligible = 0;
                }
            }
            else if (locals.gate.mode == QUGATE_MODE_MULTISIG)
            {
                locals.msigCfg = state.get()._multisigConfigs.get(locals.i);
                if (locals.msigCfg.guardianCount == 0 || locals.msigCfg.required == 0)
                {
                    locals.maintenanceEligible = 0;
                }
            }
            else if (locals.gate.mode == QUGATE_MODE_TIME_LOCK)
            {
                locals.tlCfg = state.get()._timeLockConfigs.get(locals.i);
                if (locals.tlCfg.active == 0)
                {
                    locals.maintenanceEligible = 0;
                }
            }

            if (locals.maintenanceEligible == 0 || state.get()._idleFee == 0)
            {
                continue;
            }

            // Admin gate drain: any gate with an admin multisig pays its admin's idle fees.
            // Only fires once per idle window cycle (when nextIdleChargeEpoch is due).
            // Admin gate survival is handled by the expiry exemption in the expiry loop.
            locals.cycleDue = 0;
            if (locals.gate.nextIdleChargeEpoch > 0 && qpi.epoch() >= locals.gate.nextIdleChargeEpoch) locals.cycleDue = 1;
            else if (locals.gate.nextIdleChargeEpoch == 0) locals.cycleDue = 1;
            if (locals.cycleDue == 1 && locals.gate.adminGateId >= 0 && locals.gate.reserve > 0)
            {
                locals.adminDrainSlot = (uint64)(locals.gate.adminGateId) & QUGATE_GATE_ID_SLOT_MASK;
                locals.adminDrainGen = (uint64)(locals.gate.adminGateId) >> QUGATE_GATE_ID_SLOT_BITS;
                if (locals.adminDrainSlot < state.get()._gateCount
                    && locals.adminDrainGen > 0
                    && state.get()._gateGenerations.get(locals.adminDrainSlot) == (uint16)(locals.adminDrainGen - 1))
                {
                    locals.adminDrainGate = state.get()._gates.get(locals.adminDrainSlot);
                    if (locals.adminDrainGate.active == 1)
                    {
                        locals.adminDrainFee = QPI::div(state.get()._idleFee * QUGATE_IDLE_MULTISIG_MULTIPLIER_BPS, 10000ULL);
                        if (locals.gate.reserve >= (sint64)locals.adminDrainFee)
                        {
                            locals.gate.reserve -= locals.adminDrainFee;
                            // Payment refreshes admin gate idle schedule
                            locals.adminDrainGate.lastActivityEpoch = qpi.epoch();
                            if (state.get()._idleWindowEpochs > 0)
                            {
                                locals.adminDrainGate.nextIdleChargeEpoch = qpi.epoch() + (uint16)state.get()._idleWindowEpochs;
                            }
                            state.mut()._gates.set(locals.adminDrainSlot, locals.adminDrainGate);
                            state.mut()._gates.set(locals.i, locals.gate);
                            state.mut()._idleDelinquentEpochs.set(locals.adminDrainSlot, 0);
                            state.mut()._totalMaintenanceCharged += locals.adminDrainFee;

                            locals.adminDrainBurn = QPI::div(locals.adminDrainFee * state.get()._feeBurnBps, 10000ULL);
                            locals.adminDrainDividend = locals.adminDrainFee - locals.adminDrainBurn;
                            qpi.burn(locals.adminDrainBurn);
                            state.mut()._totalBurned += locals.adminDrainBurn;
                            state.mut()._totalMaintenanceBurned += locals.adminDrainBurn;
                            state.mut()._earnedMaintenanceDividends += locals.adminDrainDividend;
                            state.mut()._totalMaintenanceDividends += locals.adminDrainDividend;
                        }
                    }
                }
            }

            locals.delinquentEpoch = state.get()._idleDelinquentEpochs.get(locals.i);
            locals.recentlyActive = 0;
            locals.activeHold = 0;
            if (state.get()._idleWindowEpochs > 0
                && qpi.epoch() - locals.gate.lastActivityEpoch < state.get()._idleWindowEpochs)
            {
                locals.recentlyActive = 1;
            }

            if (locals.gate.mode == QUGATE_MODE_HEARTBEAT)
            {
                if (locals.inhCfg.active == 1 && locals.inhCfg.triggered == 0)
                {
                    locals.activeHold = 1;
                }
            }
            else if (locals.gate.mode == QUGATE_MODE_TIME_LOCK)
            {
                if (locals.tlCfg.active == 1
                    && locals.tlCfg.fired == 0
                    && locals.tlCfg.cancelled == 0
                    && locals.gate.currentBalance > 0)
                {
                    locals.activeHold = 1;
                }
            }
            else if (locals.gate.mode == QUGATE_MODE_THRESHOLD)
            {
                if (locals.gate.currentBalance > 0)
                {
                    locals.activeHold = 1;
                }
            }
            else if (locals.gate.mode == QUGATE_MODE_MULTISIG)
            {
                if (locals.gate.currentBalance > 0 || locals.msigCfg.proposalActive == 1)
                {
                    locals.activeHold = 1;
                }
            }
            else if (locals.gate.mode == QUGATE_MODE_ORACLE)
            {
                if (locals.gate.currentBalance > 0)
                {
                    locals.activeHold = 1;
                }
            }

            if (locals.recentlyActive == 1 || locals.activeHold == 1)
            {
                // Check if this is a cycle boundary (drain fires once per idle window, not every epoch)
                locals.cycleDue = 0;
                if (locals.gate.nextIdleChargeEpoch > 0 && qpi.epoch() >= locals.gate.nextIdleChargeEpoch)
                {
                    locals.cycleDue = 1;
                }
                else if (locals.gate.nextIdleChargeEpoch == 0)
                {
                    locals.cycleDue = 1; // First cycle
                }

                if (state.get()._idleWindowEpochs > 0)
                {
                    locals.gate.nextIdleChargeEpoch = qpi.epoch() + (uint16)state.get()._idleWindowEpochs;
                    state.mut()._gates.set(locals.i, locals.gate);
                }
                if (locals.delinquentEpoch > 0)
                {
                    state.mut()._idleDelinquentEpochs.set(locals.i, 0);
                }

                // Reserve drain: pay downstream gates' idle fees from the upstream gate's reserve.
                // Only fires once per idle window cycle (not every epoch).
                if (locals.cycleDue == 1 && locals.activeHold == 1 && locals.recentlyActive == 0 && locals.gate.reserve > 0)
                {
                    // Re-read the gate in case it was modified above
                    locals.gate = state.get()._gates.get(locals.i);
                    locals.downstreamCount = 0;

                    // Count and pay for chain target
                    if (locals.gate.chainNextGateId >= 0)
                    {
                        locals.downstreamSlot = (uint64)(locals.gate.chainNextGateId) & QUGATE_GATE_ID_SLOT_MASK;
                        locals.downstreamGen = (uint64)(locals.gate.chainNextGateId) >> QUGATE_GATE_ID_SLOT_BITS;
                        if (locals.downstreamSlot < state.get()._gateCount
                            && locals.downstreamGen > 0
                            && state.get()._gateGenerations.get(locals.downstreamSlot) == (uint16)(locals.downstreamGen - 1))
                        {
                            locals.downstreamGate = state.get()._gates.get(locals.downstreamSlot);
                            if (locals.downstreamGate.active == 1)
                            {
                                locals.downstreamCount++;
                                // Compute downstream gate's effective idle fee
                                locals.downstreamMultiplierBps = QUGATE_IDLE_BASE_MULTIPLIER_BPS;
                                if (locals.downstreamGate.recipientCount >= QUGATE_MAX_RECIPIENTS)
                                {
                                    locals.downstreamMultiplierBps = QUGATE_IDLE_MAX_RECIPIENT_MULTIPLIER_BPS;
                                }
                                else if (locals.downstreamGate.recipientCount >= QUGATE_IDLE_MULTI_RECIPIENT_THRESHOLD)
                                {
                                    locals.downstreamMultiplierBps = QUGATE_IDLE_MULTI_RECIPIENT_MULTIPLIER_BPS;
                                }
                                if (locals.downstreamGate.mode == QUGATE_MODE_HEARTBEAT && locals.downstreamMultiplierBps < QUGATE_IDLE_HEARTBEAT_MULTIPLIER_BPS)
                                {
                                    locals.downstreamMultiplierBps = QUGATE_IDLE_HEARTBEAT_MULTIPLIER_BPS;
                                }
                                if (locals.downstreamGate.mode == QUGATE_MODE_MULTISIG && locals.downstreamMultiplierBps < QUGATE_IDLE_MULTISIG_MULTIPLIER_BPS)
                                {
                                    locals.downstreamMultiplierBps = QUGATE_IDLE_MULTISIG_MULTIPLIER_BPS;
                                }
                                if (locals.downstreamGate.chainNextGateId >= 0)
                                {
                                    locals.downstreamMultiplierBps += QUGATE_IDLE_CHAIN_EXTRA_BPS;
                                }
                                locals.downstreamIdleFee = QPI::div(state.get()._idleFee * locals.downstreamMultiplierBps, 10000ULL);

                                if (locals.gate.reserve >= (sint64)locals.downstreamIdleFee)
                                {
                                    locals.gate.reserve -= locals.downstreamIdleFee;
                                    locals.downstreamGate.lastActivityEpoch = qpi.epoch();
                                    if (state.get()._idleWindowEpochs > 0)
                                    {
                                        locals.downstreamGate.nextIdleChargeEpoch = qpi.epoch() + (uint16)state.get()._idleWindowEpochs;
                                    }
                                    state.mut()._gates.set(locals.downstreamSlot, locals.downstreamGate);
                                    state.mut()._idleDelinquentEpochs.set(locals.downstreamSlot, 0);
                                    state.mut()._totalMaintenanceCharged += locals.downstreamIdleFee;

                                    locals.downstreamBurnAmount = QPI::div(locals.downstreamIdleFee * state.get()._feeBurnBps, 10000ULL);
                                    locals.downstreamDividendAmount = locals.downstreamIdleFee - locals.downstreamBurnAmount;
                                    qpi.burn(locals.downstreamBurnAmount);
                                    state.mut()._totalBurned += locals.downstreamBurnAmount;
                                    state.mut()._totalMaintenanceBurned += locals.downstreamBurnAmount;
                                    state.mut()._earnedMaintenanceDividends += locals.downstreamDividendAmount;
                                    state.mut()._totalMaintenanceDividends += locals.downstreamDividendAmount;
                                }
                            }
                        }
                    }

                    // Count and pay for gate-as-recipient targets
                    for (locals.downstreamIdx = 0; locals.downstreamIdx < locals.gate.recipientCount; locals.downstreamIdx++)
                    {
                        if (locals.gate.recipientGateIds.get(locals.downstreamIdx) >= 0)
                        {
                            locals.downstreamSlot = (uint64)(locals.gate.recipientGateIds.get(locals.downstreamIdx)) & QUGATE_GATE_ID_SLOT_MASK;
                            locals.downstreamGen = (uint64)(locals.gate.recipientGateIds.get(locals.downstreamIdx)) >> QUGATE_GATE_ID_SLOT_BITS;
                            if (locals.downstreamSlot < state.get()._gateCount
                                && locals.downstreamGen > 0
                                && state.get()._gateGenerations.get(locals.downstreamSlot) == (uint16)(locals.downstreamGen - 1))
                            {
                                locals.downstreamGate = state.get()._gates.get(locals.downstreamSlot);
                                if (locals.downstreamGate.active == 1)
                                {
                                    locals.downstreamCount++;
                                    locals.downstreamMultiplierBps = QUGATE_IDLE_BASE_MULTIPLIER_BPS;
                                    if (locals.downstreamGate.recipientCount >= QUGATE_MAX_RECIPIENTS)
                                    {
                                        locals.downstreamMultiplierBps = QUGATE_IDLE_MAX_RECIPIENT_MULTIPLIER_BPS;
                                    }
                                    else if (locals.downstreamGate.recipientCount >= QUGATE_IDLE_MULTI_RECIPIENT_THRESHOLD)
                                    {
                                        locals.downstreamMultiplierBps = QUGATE_IDLE_MULTI_RECIPIENT_MULTIPLIER_BPS;
                                    }
                                    if (locals.downstreamGate.mode == QUGATE_MODE_HEARTBEAT && locals.downstreamMultiplierBps < QUGATE_IDLE_HEARTBEAT_MULTIPLIER_BPS)
                                    {
                                        locals.downstreamMultiplierBps = QUGATE_IDLE_HEARTBEAT_MULTIPLIER_BPS;
                                    }
                                    if (locals.downstreamGate.mode == QUGATE_MODE_MULTISIG && locals.downstreamMultiplierBps < QUGATE_IDLE_MULTISIG_MULTIPLIER_BPS)
                                    {
                                        locals.downstreamMultiplierBps = QUGATE_IDLE_MULTISIG_MULTIPLIER_BPS;
                                    }
                                    if (locals.downstreamGate.chainNextGateId >= 0)
                                    {
                                        locals.downstreamMultiplierBps += QUGATE_IDLE_CHAIN_EXTRA_BPS;
                                    }
                                    locals.downstreamIdleFee = QPI::div(state.get()._idleFee * locals.downstreamMultiplierBps, 10000ULL);

                                    if (locals.gate.reserve >= (sint64)locals.downstreamIdleFee)
                                    {
                                        locals.gate.reserve -= locals.downstreamIdleFee;
                                        locals.downstreamGate.lastActivityEpoch = qpi.epoch();
                                        if (state.get()._idleWindowEpochs > 0)
                                        {
                                            locals.downstreamGate.nextIdleChargeEpoch = qpi.epoch() + (uint16)state.get()._idleWindowEpochs;
                                        }
                                        state.mut()._gates.set(locals.downstreamSlot, locals.downstreamGate);
                                        state.mut()._idleDelinquentEpochs.set(locals.downstreamSlot, 0);
                                        state.mut()._totalMaintenanceCharged += locals.downstreamIdleFee;

                                        locals.downstreamBurnAmount = QPI::div(locals.downstreamIdleFee * state.get()._feeBurnBps, 10000ULL);
                                        locals.downstreamDividendAmount = locals.downstreamIdleFee - locals.downstreamBurnAmount;
                                        qpi.burn(locals.downstreamBurnAmount);
                                        state.mut()._totalBurned += locals.downstreamBurnAmount;
                                        state.mut()._totalMaintenanceBurned += locals.downstreamBurnAmount;
                                        state.mut()._earnedMaintenanceDividends += locals.downstreamDividendAmount;
                                        state.mut()._totalMaintenanceDividends += locals.downstreamDividendAmount;
                                    }
                                }
                            }
                        }
                    }

                    // Apply shielding surcharge: upstream gate's own idle fee increases
                    // per downstream target it is paying for
                    if (locals.downstreamCount > 0)
                    {
                        locals.downstreamIdleFee = QPI::div(
                            state.get()._idleFee * (uint64)locals.downstreamCount * QUGATE_IDLE_SHIELD_PER_TARGET_BPS,
                            10000ULL);
                        if (locals.gate.reserve >= (sint64)locals.downstreamIdleFee)
                        {
                            locals.gate.reserve -= locals.downstreamIdleFee;
                            state.mut()._totalMaintenanceCharged += locals.downstreamIdleFee;

                            locals.downstreamBurnAmount = QPI::div(locals.downstreamIdleFee * state.get()._feeBurnBps, 10000ULL);
                            locals.downstreamDividendAmount = locals.downstreamIdleFee - locals.downstreamBurnAmount;
                            qpi.burn(locals.downstreamBurnAmount);
                            state.mut()._totalBurned += locals.downstreamBurnAmount;
                            state.mut()._totalMaintenanceBurned += locals.downstreamBurnAmount;
                            state.mut()._earnedMaintenanceDividends += locals.downstreamDividendAmount;
                            state.mut()._totalMaintenanceDividends += locals.downstreamDividendAmount;
                        }
                    }

                    // Persist updated reserve
                    state.mut()._gates.set(locals.i, locals.gate);
                }

                continue;
            }

            if (locals.gate.nextIdleChargeEpoch == 0 && state.get()._idleWindowEpochs > 0)
            {
                locals.gate.nextIdleChargeEpoch = qpi.epoch() + (uint16)state.get()._idleWindowEpochs;
                state.mut()._gates.set(locals.i, locals.gate);
                continue;
            }

            // Charge inactivity maintenance when an idle gate reaches its own due epoch.
            // Fee scales with gate complexity: mode, recipient count, chain linkage.
            if (state.get()._idleWindowEpochs > 0
                && qpi.epoch() > 0
                && locals.gate.nextIdleChargeEpoch > 0
                && qpi.epoch() >= locals.gate.nextIdleChargeEpoch)
            {
                // Compute complexity multiplier
                locals.idleMultiplierBps = QUGATE_IDLE_BASE_MULTIPLIER_BPS;
                if (locals.gate.recipientCount >= QUGATE_MAX_RECIPIENTS)
                {
                    locals.idleMultiplierBps = QUGATE_IDLE_MAX_RECIPIENT_MULTIPLIER_BPS;
                }
                else if (locals.gate.recipientCount >= QUGATE_IDLE_MULTI_RECIPIENT_THRESHOLD)
                {
                    locals.idleMultiplierBps = QUGATE_IDLE_MULTI_RECIPIENT_MULTIPLIER_BPS;
                }
                if (locals.gate.mode == QUGATE_MODE_HEARTBEAT && locals.idleMultiplierBps < QUGATE_IDLE_HEARTBEAT_MULTIPLIER_BPS)
                {
                    locals.idleMultiplierBps = QUGATE_IDLE_HEARTBEAT_MULTIPLIER_BPS;
                }
                if (locals.gate.mode == QUGATE_MODE_MULTISIG && locals.idleMultiplierBps < QUGATE_IDLE_MULTISIG_MULTIPLIER_BPS)
                {
                    locals.idleMultiplierBps = QUGATE_IDLE_MULTISIG_MULTIPLIER_BPS;
                }
                if (locals.gate.chainNextGateId >= 0)
                {
                    locals.idleMultiplierBps += QUGATE_IDLE_CHAIN_EXTRA_BPS;
                }
                locals.effectiveIdleFee = QPI::div(state.get()._idleFee * locals.idleMultiplierBps, 10000ULL);

                if (locals.gate.reserve >= (sint64)locals.effectiveIdleFee)
                {
                    locals.gate.reserve -= locals.effectiveIdleFee;
                    locals.gate.nextIdleChargeEpoch = qpi.epoch() + (uint16)state.get()._idleWindowEpochs;
                    state.mut()._gates.set(locals.i, locals.gate);
                    state.mut()._idleDelinquentEpochs.set(locals.i, 0);
                    state.mut()._totalMaintenanceCharged += locals.effectiveIdleFee;

                    locals.maintenanceBurnAmount = QPI::div(locals.effectiveIdleFee * state.get()._feeBurnBps, 10000ULL);
                    locals.maintenanceDividendAmount = locals.effectiveIdleFee - locals.maintenanceBurnAmount;
                    qpi.burn(locals.maintenanceBurnAmount);
                    state.mut()._totalBurned += locals.maintenanceBurnAmount;
                    state.mut()._totalMaintenanceBurned += locals.maintenanceBurnAmount;
                    state.mut()._earnedMaintenanceDividends += locals.maintenanceDividendAmount;
                    state.mut()._totalMaintenanceDividends += locals.maintenanceDividendAmount;

                    locals.logger._contractIndex = CONTRACT_INDEX;
                    locals.logger._type = (locals.delinquentEpoch > 0 ? QUGATE_LOG_MAINTENANCE_CURED : QUGATE_LOG_MAINTENANCE_CHARGED);
                    locals.logger.gateId = ((uint64)(state.get()._gateGenerations.get(locals.i) + 1) << QUGATE_GATE_ID_SLOT_BITS) | locals.i;
                    locals.logger.sender = locals.gate.owner;
                    locals.logger.amount = locals.effectiveIdleFee;
                    LOG_INFO(locals.logger);
                }
                else if (locals.delinquentEpoch == 0)
                {
                    state.mut()._idleDelinquentEpochs.set(locals.i, qpi.epoch());
                    locals.logger._contractIndex = CONTRACT_INDEX;
                    locals.logger._type = QUGATE_LOG_MAINTENANCE_DELINQUENT;
                    locals.logger.gateId = ((uint64)(state.get()._gateGenerations.get(locals.i) + 1) << QUGATE_GATE_ID_SLOT_BITS) | locals.i;
                    locals.logger.sender = locals.gate.owner;
                    locals.logger.amount = state.get()._idleFee;
                    LOG_INFO(locals.logger);
                }
            }
        }

        // Expire inactive gates
        for (locals.i = 0; locals.i < state.get()._gateCount; locals.i++)
        {
            locals.gate = state.get()._gates.get(locals.i);
            locals.delinquentEpoch = state.get()._idleDelinquentEpochs.get(locals.i);

            // active==1 guard prevents double-close / activeGates underflow (intentional)
            if (locals.gate.active == 1 && state.get()._expiryEpochs > 0)
            {
                // Exempt long-duration modes from inactivity expiry
                if (locals.gate.mode == QUGATE_MODE_TIME_LOCK)
                {
                    locals.tlCfg = state.get()._timeLockConfigs.get(locals.i);
                    if (locals.delinquentEpoch == 0 && locals.tlCfg.active == 1 && locals.tlCfg.fired == 0 && locals.tlCfg.cancelled == 0)
                    {
                        continue;
                    }
                }
                if (locals.gate.mode == QUGATE_MODE_HEARTBEAT)
                {
                    locals.inhCfg = state.get()._heartbeatConfigs.get(locals.i);
                    if (locals.delinquentEpoch == 0 && locals.inhCfg.active == 1 && locals.inhCfg.triggered == 0)
                    {
                        continue;
                    }
                }
                if (locals.delinquentEpoch == 0 && locals.gate.mode == QUGATE_MODE_MULTISIG && locals.gate.currentBalance > 0)
                {
                    continue;
                }
                // Admin gate expiry exemption: if this multisig gate (recipientCount==0)
                // actively governs at least one active gate, exempt from expiry.
                // The admin gate may still be delinquent (unfunded), but it stays alive
                // as long as it has governance responsibilities.
                if (locals.gate.mode == QUGATE_MODE_MULTISIG && locals.gate.recipientCount == 0)
                {
                    locals.adminGateGovernsActive = 0;
                    for (locals.adminExpiryCheckIdx = 0; locals.adminExpiryCheckIdx < state.get()._gateCount; locals.adminExpiryCheckIdx++)
                    {
                        if (state.get()._gates.get(locals.adminExpiryCheckIdx).active == 1
                            && state.get()._gates.get(locals.adminExpiryCheckIdx).adminGateId >= 0)
                        {
                            locals.adminDrainSlot = (uint64)(state.get()._gates.get(locals.adminExpiryCheckIdx).adminGateId) & QUGATE_GATE_ID_SLOT_MASK;
                            locals.adminDrainGen = (uint64)(state.get()._gates.get(locals.adminExpiryCheckIdx).adminGateId) >> QUGATE_GATE_ID_SLOT_BITS;
                            if (locals.adminDrainSlot == locals.i
                                && locals.adminDrainGen > 0
                                && state.get()._gateGenerations.get(locals.i) == (uint16)(locals.adminDrainGen - 1))
                            {
                                locals.adminGateGovernsActive = 1;
                                break;
                            }
                        }
                    }
                    if (locals.adminGateGovernsActive == 1)
                    {
                        continue;
                    }
                }

                if ((locals.delinquentEpoch > 0 && state.get()._idleGraceEpochs > 0
                     && qpi.epoch() - locals.delinquentEpoch >= state.get()._idleGraceEpochs)
                    || (qpi.epoch() - locals.gate.lastActivityEpoch >= state.get()._expiryEpochs))
                {
                    // Refund any held balance (THRESHOLD mode)
                    if (locals.gate.currentBalance > 0)
                    {
                        if (qpi.transfer(locals.gate.owner, locals.gate.currentBalance) >= 0) // [QG-14]
                        {
                            locals.gate.currentBalance = 0;
                        }
                    }

                    // Refund reserve on expiry
                    if (locals.gate.reserve > 0)
                    {
                        if (qpi.transfer(locals.gate.owner, locals.gate.reserve) >= 0) // [QG-14]
                        {
                            locals.gate.reserve = 0;
                        }
                    }

                    state.mut()._gates.set(locals.i, locals.gate);

                    if (locals.gate.currentBalance > 0 || locals.gate.reserve > 0)
                    {
                        continue;
                    }

                    // Clear mode-specific configs on expiry (prevents ghost state in recycled slots)
                    if (locals.gate.mode == QUGATE_MODE_HEARTBEAT)
                    {
                        locals.hbZeroCfg.active = 0; locals.hbZeroCfg.triggered = 0; locals.hbZeroCfg.thresholdEpochs = 0;
                        locals.hbZeroCfg.lastHeartbeatEpoch = 0; locals.hbZeroCfg.payoutPercentPerEpoch = 0;
                        locals.hbZeroCfg.minimumBalance = 0; locals.hbZeroCfg.triggerEpoch = 0; locals.hbZeroCfg.beneficiaryCount = 0;
                        for (locals.loopIdx = 0; locals.loopIdx < 8; locals.loopIdx++)
                        {
                            locals.hbZeroCfg.beneficiaryAddresses.set(locals.loopIdx, id::zero());
                            locals.hbZeroCfg.beneficiaryShares.set(locals.loopIdx, 0);
                        }
                        state.mut()._heartbeatConfigs.set(locals.i, locals.hbZeroCfg);
                    }
                    else if (locals.gate.mode == QUGATE_MODE_MULTISIG)
                    {
                        for (locals.loopIdx = 0; locals.loopIdx < 8; locals.loopIdx++)
                        {
                            locals.msigZeroCfg.guardians.set(locals.loopIdx, id::zero());
                        }
                        locals.msigZeroCfg.guardianCount = 0; locals.msigZeroCfg.required = 0; locals.msigZeroCfg.proposalExpiryEpochs = 0; locals.msigZeroCfg.adminApprovalWindowEpochs = 0;
                        locals.msigZeroCfg.approvalBitmap = 0; locals.msigZeroCfg.approvalCount = 0; locals.msigZeroCfg.proposalEpoch = 0; locals.msigZeroCfg.proposalActive = 0;
                        state.mut()._multisigConfigs.set(locals.i, locals.msigZeroCfg);
                        locals.zeroAdminApproval.active = 0; locals.zeroAdminApproval.validUntilEpoch = 0;
                        state.mut()._adminApprovalStates.set(locals.i, locals.zeroAdminApproval);
                    }
                    else if (locals.gate.mode == QUGATE_MODE_TIME_LOCK)
                    {
                        locals.tlZeroCfg.unlockEpoch = 0; locals.tlZeroCfg.delayEpochs = 0; locals.tlZeroCfg.lockMode = QUGATE_TIME_LOCK_ABSOLUTE_EPOCH; locals.tlZeroCfg.cancellable = 0; locals.tlZeroCfg.fired = 0; locals.tlZeroCfg.cancelled = 0; locals.tlZeroCfg.active = 0;
                        state.mut()._timeLockConfigs.set(locals.i, locals.tlZeroCfg);
                    }

                    locals.gate.active = 0;
                    state.mut()._gates.set(locals.i, locals.gate);
                    state.mut()._activeGates -= 1;
                    state.mut()._idleDelinquentEpochs.set(locals.i, 0);

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

                    // Sweep the remainder if rounding would otherwise produce a zero payout.
                    if (locals.inhPayoutTotal == 0 && locals.inhBalance > 0)
                    {
                        locals.inhPayoutTotal = locals.inhBalance;
                    }

                    if (locals.inhPayoutTotal > 0)
                    {
                        locals.inhBeneCount = locals.inhCfg.beneficiaryCount;
                        locals.inhDistributedTotal = 0;

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
                                    locals.inhDistributedTotal += locals.inhShare;
                                }
                            }
                        }

                        if (locals.gate.chainNextGateId != -1 && locals.inhPayoutTotal > locals.inhDistributedTotal)
                        {
                            locals.chainAmount = locals.inhPayoutTotal - locals.inhDistributedTotal;
                            locals.gate.totalForwarded += (uint64)locals.chainAmount;
                            locals.gate.currentBalance -= (uint64)locals.chainAmount;
                            state.mut()._gates.set(locals.i, locals.gate);

                            locals.currentChainGateId = locals.gate.chainNextGateId;
                            locals.hop = 0;
                            while (locals.hop < QUGATE_MAX_CHAIN_DEPTH && locals.currentChainGateId != -1 && locals.chainAmount > 0)
                            {
                                locals.nextSlot = (uint64)locals.currentChainGateId & QUGATE_GATE_ID_SLOT_MASK;
                                locals.nextGen = (uint64)locals.currentChainGateId >> QUGATE_GATE_ID_SLOT_BITS;
                                if (locals.nextSlot >= state.get()._gateCount || locals.nextGen == 0
                                    || state.get()._gateGenerations.get(locals.nextSlot) != (uint16)(locals.nextGen - 1))
                                    break;
                                locals.rtIn.slotIdx = locals.nextSlot; locals.rtIn.amount = locals.chainAmount; locals.rtIn.hopCount = locals.hop;
                                routeToGate(qpi, state, locals.rtIn, locals.rtOut, locals.rtLocals);
                                locals.chainAmount = locals.rtOut.forwarded;
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
                                locals.nextGate = state.get()._gates.get(locals.nextSlot);
                                locals.currentChainGateId = locals.nextGate.chainNextGateId;
                                locals.hop++;
                            }
                            if (locals.chainAmount > 0 && locals.currentChainGateId != -1)
                            {
                                locals.gate = state.get()._gates.get(locals.i);
                                locals.gate.currentBalance += (uint64)locals.chainAmount;
                                locals.gate.totalForwarded -= (uint64)locals.chainAmount;
                                state.mut()._gates.set(locals.i, locals.gate);
                                locals.logger._type = QUGATE_LOG_CHAIN_HOP_INSUFFICIENT;
                                locals.logger.gateId = ((uint64)(state.get()._gateGenerations.get(locals.i) + 1) << QUGATE_GATE_ID_SLOT_BITS) | locals.i;
                                locals.logger.amount = locals.chainAmount;
                                LOG_INFO(locals.logger);
                            }
                        }
                        else
                        {
                            state.mut()._gates.set(locals.i, locals.gate);
                        }

                        locals.logger._contractIndex = CONTRACT_INDEX;
                        locals.logger._type = QUGATE_LOG_HEARTBEAT_PAYOUT;
                        locals.logger.gateId = ((uint64)(state.get()._gateGenerations.get(locals.i) + 1) << QUGATE_GATE_ID_SLOT_BITS) | locals.i;
                        locals.logger.sender = locals.gate.owner;
                        locals.logger.amount = locals.inhPayoutTotal;
                        LOG_INFO(locals.logger);
                    }
                    else
                    {
                        state.mut()._gates.set(locals.i, locals.gate);
                    }

                    // Auto-close gate if balance dropped to or below minimum
                    locals.gate = state.get()._gates.get(locals.i);
                    if ((sint64)locals.gate.currentBalance <= locals.inhCfg.minimumBalance)
                    {
                        // Distribute the remaining balance to beneficiaries.
                        if (locals.gate.currentBalance > 0 && locals.inhCfg.beneficiaryCount > 0)
                        {
                            locals.dustTotal = (sint64)locals.gate.currentBalance;
                            locals.inhPriorSum = 0;
                            for (locals.inhJ = 0; locals.inhJ < locals.inhCfg.beneficiaryCount; locals.inhJ++)
                            {
                                if (locals.inhJ == locals.inhCfg.beneficiaryCount - 1)
                                {
                                    locals.inhShare = locals.dustTotal - locals.inhPriorSum;
                                }
                                else
                                {
                                    locals.inhShare = (sint64)QPI::div((uint64)(locals.dustTotal * (sint64)locals.inhCfg.beneficiaryShares.get(locals.inhJ)), (uint64)100);
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

                        if (locals.gate.currentBalance > 0)
                        {
                            state.mut()._gates.set(locals.i, locals.gate);
                            continue;
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
                    locals.tlTransferred = 0;
                    if (locals.gate.recipientCount > 0)
                    {
                        if (locals.gate.recipientGateIds.get(0) >= 0)
                        {
                            // Gate-as-recipient: route via routeToGate
                            locals.targetSlot = (uint64)(locals.gate.recipientGateIds.get(0)) & QUGATE_GATE_ID_SLOT_MASK;
                            locals.targetGen = (uint64)(locals.gate.recipientGateIds.get(0)) >> QUGATE_GATE_ID_SLOT_BITS;
                            if (locals.targetSlot < state.get()._gateCount && locals.targetGen > 0
                                && state.get()._gateGenerations.get(locals.targetSlot) == (uint16)(locals.targetGen - 1)
                                && state.get()._gates.get(locals.targetSlot).active == 1)
                            {
                                locals.rtIn.slotIdx = locals.targetSlot;
                                locals.rtIn.amount = locals.tlReleaseAmount;
                                locals.rtIn.hopCount = 0;
                                routeToGate(qpi, state, locals.rtIn, locals.rtOut, locals.rtLocals);
                                if (locals.rtOut.accepted == 1)
                                {
                                    locals.tlTransferred = 1;
                                }
                            }
                        }
                        else
                        {
                            if (qpi.transfer(locals.gate.recipients.get(0), locals.tlReleaseAmount) >= 0) // [QG-17]
                            {
                                locals.tlTransferred = 1;
                            }
                        }
                    }
                    else if (locals.gate.chainNextGateId != -1)
                    {
                        locals.chainAmount = locals.tlReleaseAmount;
                        locals.gate.totalForwarded += (uint64)locals.chainAmount;
                        locals.gate.currentBalance -= (uint64)locals.chainAmount;
                        state.mut()._gates.set(locals.i, locals.gate);

                        locals.currentChainGateId = locals.gate.chainNextGateId;
                        locals.hop = 0;
                        while (locals.hop < QUGATE_MAX_CHAIN_DEPTH && locals.currentChainGateId != -1 && locals.chainAmount > 0)
                        {
                            locals.nextSlot = (uint64)locals.currentChainGateId & QUGATE_GATE_ID_SLOT_MASK;
                            locals.nextGen = (uint64)locals.currentChainGateId >> QUGATE_GATE_ID_SLOT_BITS;
                            if (locals.nextSlot >= state.get()._gateCount || locals.nextGen == 0
                                || state.get()._gateGenerations.get(locals.nextSlot) != (uint16)(locals.nextGen - 1))
                                break;
                            locals.rtIn.slotIdx = locals.nextSlot;
                            locals.rtIn.amount = locals.chainAmount;
                            locals.rtIn.hopCount = locals.hop;
                            routeToGate(qpi, state, locals.rtIn, locals.rtOut, locals.rtLocals);
                            locals.chainAmount = locals.rtOut.forwarded;
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
                            locals.nextGate = state.get()._gates.get(locals.nextSlot);
                            locals.currentChainGateId = locals.nextGate.chainNextGateId;
                            locals.hop++;
                        }
                        if (locals.chainAmount > 0 && locals.currentChainGateId != -1)
                        {
                            locals.gate = state.get()._gates.get(locals.i);
                            locals.gate.currentBalance += (uint64)locals.chainAmount;
                            locals.gate.totalForwarded -= (uint64)locals.chainAmount;
                            state.mut()._gates.set(locals.i, locals.gate);
                            locals.logger._type = QUGATE_LOG_CHAIN_HOP_INSUFFICIENT;
                            locals.logger.gateId = ((uint64)(state.get()._gateGenerations.get(locals.i) + 1) << QUGATE_GATE_ID_SLOT_BITS) | locals.i;
                            locals.logger.amount = locals.chainAmount;
                            LOG_INFO(locals.logger);
                        }
                        locals.gate = state.get()._gates.get(locals.i);
                        if (locals.gate.currentBalance == 0)
                        {
                            locals.tlTransferred = 1;
                        }
                    }
                    // else: no recipients AND no chain — funds stay in currentBalance
                    if (locals.tlTransferred)
                    {
                        locals.gate.totalForwarded += (uint64)locals.tlReleaseAmount;
                        locals.gate.currentBalance = 0;
                        state.mut()._gates.set(locals.i, locals.gate);
                    }
                }

                locals.gate = state.get()._gates.get(locals.i);
                if (locals.gate.currentBalance > 0)
                {
                    continue;
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
    END_TICK_WITH_LOCALS()
    {
        locals.availableMaintenanceDividends =
            (state.get()._earnedMaintenanceDividends > state.get()._distributedMaintenanceDividends)
                ? (state.get()._earnedMaintenanceDividends - state.get()._distributedMaintenanceDividends)
                : 0;

        locals.maintenanceDividendPerShare = QPI::div(locals.availableMaintenanceDividends, (uint64)NUMBER_OF_COMPUTORS);
        if (locals.maintenanceDividendPerShare > 0)
        {
            if (qpi.distributeDividends(locals.maintenanceDividendPerShare))
            {
                state.mut()._distributedMaintenanceDividends += locals.maintenanceDividendPerShare * NUMBER_OF_COMPUTORS;
            }
        }
    }

};
