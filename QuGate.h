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

// Status codes — used in procedure outputs and logger _type
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
constexpr sint64 QUGATE_INVALID_ORACLE_CONFIG = -13;

// Log type constants (positive = success events, high numbers = actions)
constexpr uint32 QUGATE_LOG_GATE_CREATED = 1;
constexpr uint32 QUGATE_LOG_GATE_CLOSED = 2;
constexpr uint32 QUGATE_LOG_GATE_UPDATED = 3;
constexpr uint32 QUGATE_LOG_PAYMENT_FORWARDED = 4;
constexpr uint32 QUGATE_LOG_PAYMENT_BOUNCED = 5;
constexpr uint32 QUGATE_LOG_DUST_BURNED = 6;
constexpr uint32 QUGATE_LOG_FEE_CHANGED = 7;
constexpr uint32 QUGATE_LOG_GATE_EXPIRED = 8;
constexpr sint64 QUGATE_LOG_ORACLE_TRIGGERED  = 9;
constexpr sint64 QUGATE_LOG_ORACLE_EXHAUSTED  = 10;
constexpr sint64 QUGATE_LOG_ORACLE_SUBSCRIBED = 11;

// Failure log types use high range
constexpr uint32 QUGATE_LOG_FAIL_INVALID_GATE = 100;
constexpr uint32 QUGATE_LOG_FAIL_NOT_ACTIVE = 101;
constexpr uint32 QUGATE_LOG_FAIL_UNAUTHORIZED = 102;
constexpr uint32 QUGATE_LOG_FAIL_INVALID_PARAMS = 103;
constexpr uint32 QUGATE_LOG_FAIL_INSUFFICIENT_FEE = 104;
constexpr uint32 QUGATE_LOG_FAIL_NO_SLOTS = 105;

// Asset name for shareholder proposals
constexpr uint64 QUGATE_CONTRACT_ASSET_NAME = 76228174763345ULL; // "QUGATE" as uint64 little-endian

// Future extension struct (Qubic convention)
struct QUGATE2
{
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
        id owner;
        uint8 mode;
        uint8 recipientCount;
        uint8 active;
        uint8 allowedSenderCount;
        uint16 createdEpoch;                            // epoch() returns uint16
        uint16 lastActivityEpoch;                       // updated on create/send/update
        uint64 totalReceived;
        uint64 totalForwarded;
        uint64 currentBalance;
        uint64 threshold;
        uint64 roundRobinIndex;
        Array<id, 8> recipients;
        Array<uint64, 8> ratios;
        Array<id, 8> allowedSenders;

        // Oracle mode fields (only used when mode == QUGATE_MODE_ORACLE)
        id     oracleId;               // e.g. Price::getBinanceOracleId()
        id     oracleCurrency1;        // first currency of pair
        id     oracleCurrency2;        // second currency of pair
        uint8  oracleCondition;        // QUGATE_ORACLE_COND_PRICE_ABOVE/BELOW/TIME_AFTER
        uint8  oracleTriggerMode;      // QUGATE_ORACLE_TRIGGER_ONCE / RECURRING
        sint64 oracleThreshold;        // price ratio * 1e6, or unix timestamp
        sint64 oracleReserve;          // QU reserve to pay subscription fees
        sint32 oracleSubscriptionId;   // -1 if not subscribed
    };

    // =============================================
    // Procedure inputs/outputs
    // =============================================

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
    };
    struct createGate_output
    {
        sint64 status;          //
        uint64 gateId;
        uint64 feePaid;         // actual fee charged (for transparency)
    };

    struct sendToGate_input
    {
        uint64 gateId;
    };
    struct sendToGate_output
    {
        sint64 status;          //
    };

    struct closeGate_input
    {
        uint64 gateId;
    };
    struct closeGate_output
    {
        sint64 status;          //
    };

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

    struct fundGate_input
    {
        sint64 gateId;
    };
    struct fundGate_output
    {
        sint64 result;
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

protected:
    // =============================================
    // Contract state (wrapped in StateData for dirty-tracking)
    // =============================================

    struct StateData
    {
        uint64 _gateCount;
        uint64 _activeGates;
        uint64 _totalBurned;        // cumulative QU burned
        Array<GateConfig, QUGATE_MAX_GATES> _gates;

        // Free-list for slot reuse
        Array<uint64, QUGATE_MAX_GATES> _freeSlots;
        uint64 _freeCount;

        // Shareholder-adjustable parameters
        uint64 _creationFee;        // base creation fee
        uint64 _minSendAmount;
        uint64 _expiryEpochs;      // epochs of inactivity before auto-close
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


    struct sendToGate_locals
    {
        QuGateLogger logger;
        GateConfig gate;
        sint64 amount;
        uint64 idx;
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

    struct closeGate_locals
    {
        QuGateLogger logger;
        GateConfig gate;
    };

    struct updateGate_locals
    {
        QuGateLogger logger;
        GateConfig gate;
        uint64 totalRatio;
        uint64 i;
    };

    struct getGate_locals
    {
        GateConfig gate;
        uint64 i;
    };

    struct getGateBatch_locals                           //
    {
        uint64 i;
        uint64 j;
        GateConfig gate;
        getGate_output entry;
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
    };

    struct fundGate_locals
    {
        QuGateLogger logger;
        GateConfig gate;
    };

    // Oracle notification callback types
    typedef OracleNotificationInput<Price> OraclePriceNotification_input;
    typedef NoData OraclePriceNotification_output;

    struct OraclePriceNotification_locals
    {
        uint64 i;
        GateConfig gate;
        QuGateLogger logger;
        uint8 conditionMet;
        sint64 priceScaled;
        sint64 splitAmount;
        processSplit_input splitIn;
        processSplit_output splitOut;
        processSplit_locals splitLocals;
    };

    struct BEGIN_EPOCH_locals
    {
        uint64 i;
        GateConfig gate;
        QuGateLogger logger;
        sint32 subId;
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
        if (input.mode > QUGATE_MODE_ORACLE)
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

        state.mut()._gates.set(locals.slotIdx, locals.newGate);
        output.gateId = locals.slotIdx + 1;              // 1-indexed for users
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
            qpi.transfer(locals.gate.recipients.get(0), locals.gate.currentBalance);
            output.forwarded = locals.gate.currentBalance;
            locals.gate.totalForwarded += locals.gate.currentBalance;
            locals.gate.currentBalance = 0;
        }

        state.mut()._gates.set(input.gateIdx, locals.gate);
    }

    PRIVATE_PROCEDURE_WITH_LOCALS(processRandom)
    {
        locals.gate = state.get()._gates.get(input.gateIdx);

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
    // Procedures
    // =============================================

    PUBLIC_PROCEDURE_WITH_LOCALS(sendToGate)
    {
        output.status = QUGATE_SUCCESS;

        locals.logger._contractIndex = CONTRACT_INDEX;
        locals.logger.sender = qpi.invocator();
        locals.logger.amount = qpi.invocationReward();
        locals.logger.gateId = input.gateId;

        if (input.gateId == 0 || input.gateId > state.get()._gateCount)
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

        locals.idx = input.gateId - 1;
        locals.gate = state.get()._gates.get(locals.idx);

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
        state.mut()._gates.set(locals.idx, locals.gate);

        // Dispatch to mode-specific handler
        if (locals.gate.mode == QUGATE_MODE_SPLIT)
        {
            locals.splitIn.gateIdx = locals.idx;
            locals.splitIn.amount = locals.amount;
            processSplit(qpi, state, locals.splitIn, locals.splitOut, locals.splitLocals);
            locals.logger._type = QUGATE_LOG_PAYMENT_FORWARDED;
            LOG_INFO(locals.logger);
        }
        else if (locals.gate.mode == QUGATE_MODE_ROUND_ROBIN)
        {
            locals.rrIn.gateIdx = locals.idx;
            locals.rrIn.amount = locals.amount;
            processRoundRobin(qpi, state, locals.rrIn, locals.rrOut, locals.rrLocals);
            locals.logger._type = QUGATE_LOG_PAYMENT_FORWARDED;
            LOG_INFO(locals.logger);
        }
        else if (locals.gate.mode == QUGATE_MODE_THRESHOLD)
        {
            locals.threshIn.gateIdx = locals.idx;
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
            locals.randIn.gateIdx = locals.idx;
            locals.randIn.amount = locals.amount;
            processRandom(qpi, state, locals.randIn, locals.randOut, locals.randLocals);
            locals.logger._type = QUGATE_LOG_PAYMENT_FORWARDED;
            LOG_INFO(locals.logger);
        }
        else if (locals.gate.mode == QUGATE_MODE_CONDITIONAL)
        {
            locals.condIn.gateIdx = locals.idx;
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
            locals.gate = state.get()._gates.get(locals.idx);
            locals.gate.currentBalance += locals.amount;
            state.mut()._gates.set(locals.idx, locals.gate);
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

        if (input.gateId == 0 || input.gateId > state.get()._gateCount)
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

        locals.gate = state.get()._gates.get(input.gateId - 1);

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
                qpi.unsubscribeOracle(locals.gate.oracleSubscriptionId);
                locals.gate.oracleSubscriptionId = -1;
            }
        }

        // Guard against double-close underflow
        if (locals.gate.active == 1)
        {
            locals.gate.active = 0;
            state.mut()._gates.set(input.gateId - 1, locals.gate);
            state.mut()._activeGates -= 1;

            // Push slot onto free-list for reuse
            state.mut()._freeSlots.set(state.get()._freeCount, input.gateId - 1);
            state.mut()._freeCount += 1;
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

        if (input.gateId == 0 || input.gateId > state.get()._gateCount)
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

        locals.gate = state.get()._gates.get(input.gateId - 1);

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
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
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

        state.mut()._gates.set(input.gateId - 1, locals.gate);

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

        if (input.gateId <= 0 || input.gateId > (sint64)state.get()._gateCount)
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

        locals.gate = state.get()._gates.get(input.gateId - 1);

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

        if (qpi.invocationReward() <= 0)
        {
            output.result = QUGATE_DUST_AMOUNT;
            return;
        }

        // Add invocationReward to oracle reserve — anyone can call, burns nothing
        locals.gate.oracleReserve += qpi.invocationReward();
        state.mut()._gates.set(input.gateId - 1, locals.gate);
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

        // Find gate matching this subscriptionId
        for (locals.i = 0; locals.i < state.get()._gateCount; locals.i++)
        {
            locals.gate = state.get()._gates.get(locals.i);
            if (locals.gate.active == 0 || locals.gate.mode != QUGATE_MODE_ORACLE)
            {
                continue;
            }
            if (locals.gate.oracleSubscriptionId != input.subscriptionId)
            {
                continue;
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
                // TIME_AFTER: treat oracle reply numerator as a timestamp-like value;
                // use a time oracle or a price oracle whose numerator encodes unix time
                if (input.reply.numerator > locals.gate.oracleThreshold)
                {
                    locals.conditionMet = 1;
                }
            }

            if (locals.conditionMet && locals.gate.currentBalance > 0)
            {
                // Store amount, zero balance, and write gate back BEFORE calling processSplit
                // (processSplit reads from state so the gate must be persisted first)
                locals.splitAmount = locals.gate.currentBalance;
                locals.gate.currentBalance = 0;
                state.mut()._gates.set(locals.i, locals.gate);

                // Distribute using ratio-aware split
                locals.splitIn.gateIdx = locals.i;
                locals.splitIn.amount = locals.splitAmount;
                processSplit(qpi, state, locals.splitIn, locals.splitOut, locals.splitLocals);

                // Re-read gate after processSplit updated totalForwarded
                locals.gate = state.get()._gates.get(locals.i);

                // Log oracle triggered
                locals.logger._contractIndex = CONTRACT_INDEX;
                locals.logger._type = QUGATE_LOG_ORACLE_TRIGGERED;
                locals.logger.gateId = locals.i + 1;
                locals.logger.sender = id::zero();
                locals.logger.amount = locals.splitOut.forwarded;
                LOG_INFO(locals.logger);

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
                        qpi.unsubscribeOracle(locals.gate.oracleSubscriptionId);
                        locals.gate.oracleSubscriptionId = -1;
                    }
                    locals.gate.active = 0;
                    state.mut()._activeGates -= 1;
                    state.mut()._freeSlots.set(state.get()._freeCount, locals.i);
                    state.mut()._freeCount += 1;
                }
            }

            state.mut()._gates.set(locals.i, locals.gate);
            break; // found the matching gate
        }
    }

    // =============================================
    // Functions (read-only)
    // =============================================

    PUBLIC_FUNCTION_WITH_LOCALS(getGate)
    {
        if (input.gateId == 0 || input.gateId > state.get()._gateCount)
        {
            output.active = 0;
            return;
        }

        locals.gate = state.get()._gates.get(input.gateId - 1);
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
                output.gateIds.set(output.count, locals.i + 1);
                output.count += 1;
            }
        }
    }

    // Batch gate query — fetch up to 32 gates in one call
    PUBLIC_FUNCTION_WITH_LOCALS(getGateBatch)
    {
        for (locals.i = 0; locals.i < QUGATE_MAX_BATCH_GATES; locals.i++)
        {
            if (input.gateIds.get(locals.i) == 0 || input.gateIds.get(locals.i) > state.get()._gateCount)
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
                for (locals.j = 0; locals.j < QUGATE_MAX_RECIPIENTS; locals.j++)
                {
                    locals.entry.recipients.set(locals.j, id::zero());
                    locals.entry.ratios.set(locals.j, 0);
                }
                output.gates.set(locals.i, locals.entry);
            }
            else
            {
                locals.gate = state.get()._gates.get(input.gateIds.get(locals.i) - 1);

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
    // Registration
    // =============================================

    REGISTER_USER_FUNCTIONS_AND_PROCEDURES()
    {
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
                    Price::OracleQuery query;
                    query.oracle = locals.gate.oracleId;
                    query.currency1 = locals.gate.oracleCurrency1;
                    query.currency2 = locals.gate.oracleCurrency2;

                    locals.subId = SUBSCRIBE_ORACLE(Price, query, OraclePriceNotification,
                        QUGATE_ORACLE_NOTIFY_PERIOD_MS, true);

                    if (locals.subId >= 0)
                    {
                        locals.gate.oracleSubscriptionId = locals.subId;
                        locals.gate.oracleReserve -= QUGATE_ORACLE_SUBSCRIPTION_FEE;
                        state.mut()._gates.set(locals.i, locals.gate);

                        locals.logger._contractIndex = CONTRACT_INDEX;
                        locals.logger._type = QUGATE_LOG_ORACLE_SUBSCRIBED;
                        locals.logger.gateId = locals.i + 1;
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
                    locals.logger.gateId = locals.i + 1;
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
                    if (locals.gate.mode == QUGATE_MODE_ORACLE && locals.gate.oracleReserve > 0)
                    {
                        qpi.transfer(locals.gate.owner, locals.gate.oracleReserve);
                        locals.gate.oracleReserve = 0;
                    }

                    locals.gate.active = 0;
                    state.mut()._gates.set(locals.i, locals.gate);
                    state.mut()._activeGates -= 1;

                    // Push slot onto free-list
                    state.mut()._freeSlots.set(state.get()._freeCount, locals.i);
                    state.mut()._freeCount += 1;

                    // Log expiry
                    locals.logger._contractIndex = CONTRACT_INDEX;
                    locals.logger._type = QUGATE_LOG_GATE_EXPIRED;
                    locals.logger.gateId = locals.i + 1;
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
    }

    BEGIN_TICK() {}
    END_TICK() {}

};


