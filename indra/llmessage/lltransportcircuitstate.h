/** Owner-only, bounded transport circuit state; no sockets or viewer callbacks. */
#ifndef LL_LLTRANSPORTCIRCUITSTATE_H
#define LL_LLTRANSPORTCIRCUITSTATE_H

#include "lltransportcircuittypes.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

class LLTransportCircuitState
{
public:
    static constexpr uint32_t MaxPacketBytes = 8192;
    // LLCircuit::sendAcks sends when ++acks_this_packet > 250.
    static constexpr uint32_t MaxAckBatch = 251;
    // Bounds are allocated once at init; no later operation allocates.
    struct Limits
    {
        uint32_t circuits = 0, receives = 0, remembered = 0, acks = 0, reliable = 0, losses = 0;
        // Remaining actor graph budget reserved for this state, never unlimited.
        uint64_t byteBudget = 0;
    };
    enum class Status : uint8_t { Ok, Full, Invalid, Absent, Stale, Busy, NotDue, Partial };
    struct WatchdogConfig
    {
        uint64_t heartbeatUsec = 0, timeoutUsec = 0;
        float initialOffsetDraw = 0.f, initialJitterDraw = 0.f;
    };
    struct EnableResult { Status status = Status::Invalid; uint64_t incarnation = 0; };
    enum class ReceiveStatus : uint8_t { New, Duplicate, Full, DuplicateFull, Invalid };
    struct ReceiveResult { ReceiveStatus status = ReceiveStatus::Invalid; uint64_t reservation = 0; };
    enum class DuplicateClass : uint8_t { NotDuplicate, Duplicate, Busy, Invalid };
    enum class ArrivalStatus : uint8_t { Observed, PartialLoss, Duplicate, Busy, Unavailable, Invalid };
    struct ArrivalResult { ArrivalStatus status = ArrivalStatus::Invalid; uint64_t observation = 0; };
    struct RetryPolicy
    {
        uint32_t retries = 0;
        int64_t firstIntervalUsec = 0, retryIntervalUsec = 0;
        // Negative legacy timeout's successful-ACK callback is still timeout.
        bool successIsTimeout = false;
        // Legacy ping-based retries recompute the interval from the selected
        // circuit's current averaged ping after each resend attempt.
        bool pingBasedRetry = false;
        // Resolve legacy ping-based initial timeout on the owner at admission.
        // Independent from retries: semireliable sends have no ping retry.
        bool pingBasedInitial = false;
    };
    enum class ActionKind : uint8_t { None, InitialSend, Resend, Ack, Ping };
    struct Action
    {
        ActionKind kind = ActionKind::None;
        uint64_t token = 0, incarnation = 0, reliableToken = 0;
        LLTransportEndpoint endpoint{};
        uint32_t packetId = 0, size = 0, ackCount = 0, oldestUnacked = 0;
        uint8_t pingId = 0;
        bool packetAssigned = false;
        std::array<uint8_t, MaxPacketBytes> bytes{};
        std::array<uint32_t, MaxAckBatch> ackIds{};
    };
    // At most one entry per circuit per advance call. The owner brackets the
    // call in the snapshot directory transaction, republishes these values,
    // and fails the directory closed if overflow is ever observed.
    struct DirtyIncarnations
    {
        struct Value { LLTransportEndpoint endpoint{}; uint64_t incarnation=0; };
        std::array<Value,1024> values{};
        uint32_t count=0;
        bool overflow=false;
    };
    struct Piggyback
    {
        uint64_t token = 0, incarnation = 0;
        LLTransportEndpoint endpoint{};
        uint32_t count = 0;
        std::array<uint32_t, 250> ackIds{};
    };
    struct TimeoutEvent
    {
        LLTransportEndpoint endpoint{};
        uint64_t loginGeneration = 0, incarnation = 0, eventId = 0, lastPublishedAppSequence = 0;
    };
    enum class WatchdogStatus : uint8_t { NotDue, PingReady, Timeout, Full, Busy, Invalid, Stale };
    struct WatchdogPoll { WatchdogStatus status = WatchdogStatus::Invalid; TimeoutEvent event{}; };
    struct Completion
    {
        LLTransportEndpoint endpoint{};
        uint64_t loginGeneration = 0, incarnation = 0, token = 0;
        int32_t result = 0;
    };

    LLTransportCircuitState() noexcept;
    ~LLTransportCircuitState();
    LLTransportCircuitState(const LLTransportCircuitState&) = delete;
    LLTransportCircuitState& operator=(const LLTransportCircuitState&) = delete;
    // Counts requested object/array bytes, including all control/completion
    // storage. Actor reserves this charge in its graph ledger exactly once;
    // allocator overhead and separately owned arenas are budgeted separately.
    static bool estimateBytes(const Limits& limits, uint64_t& bytes) noexcept;
    bool init(const Limits& limits, uint64_t& chargedBytes) noexcept;
#if defined(LL_TRANSPORT_STATE_TESTING)
    bool testInit(const Limits& limits, uint64_t& chargedBytes, uint32_t failBeforeAllocation) noexcept;
    uint32_t testAllocationCalls() const noexcept { return mAllocationCalls; }
    static bool testCheckedAddProduct(uint64_t& total, uint64_t count, uint64_t size) noexcept;
    // Deterministically exercise wire packet-ID wrap/reuse in source tests.
    bool testSetLastOut(LLTransportEndpoint endpoint, uint64_t incarnation, uint32_t value) noexcept;
    bool testSetSequence(uint64_t value) noexcept;
#endif
    // Every newly enabled circuit gets an explicit watchdog; no never-expire default.
    EnableResult enable(LLTransportEndpoint endpoint, uint64_t loginGeneration, bool trusted,
                        uint64_t nowUsec, WatchdogConfig watchdog) noexcept;
    LLTransportCircuitSnapshot lookup(LLTransportEndpoint endpoint, uint64_t nowUsec) const noexcept;
    // Owner-only coherent value pair for Hume's snapshot quality publication;
    // Absent/Invalid maps to Unknown, Ok+true to Complete, Ok+false to Partial.
    Status lookupWithLossTracking(LLTransportEndpoint endpoint, uint64_t nowUsec,
                                  LLTransportCircuitSnapshot& snapshot, bool& complete) const noexcept;
    // Read-only committed-resent classification, before template validation;
    // no ACK, counters, timestamps or dedup state change. The actor must use
    // acknowledgeCommittedDuplicate separately for a reliable duplicate.
    DuplicateClass classifyResentDuplicate(LLTransportEndpoint endpoint, uint64_t incarnation, uint64_t loginGeneration,
                                           uint32_t packetId, bool resent) const noexcept;
    // After trailer ACK processing, duplicate classification, and template /
    // trust validation, record circuit activity even when the APP queue is full.
    // Duplicate means an already-committed resent ID: no observation or ACK is
    // produced here. PartialLoss explicitly means loss statistics are no longer
    // complete for this incarnation, while liveness and byte counters advance.
    ArrivalResult observeValidatedArrival(LLTransportEndpoint endpoint, uint64_t incarnation, uint64_t loginGeneration,
                                          uint32_t packetId, bool resent, uint32_t wireBytes, uint64_t nowUsec) noexcept;
    Status lossTrackingComplete(LLTransportEndpoint endpoint, uint64_t incarnation, bool& complete) const noexcept;
    // Actor first reserves bounded payload capacity, then reserves ACK/dedup
    // here before publishing the payload. Use the exact observation token before
    // observing another packet on this circuit. Commit only once publication is
    // guaranteed; it does not count the observed packet twice.
    ReceiveResult reserveObservedReceive(LLTransportEndpoint endpoint, uint64_t incarnation, uint64_t loginGeneration,
                                         uint32_t packetId, bool resent, bool reliable, uint64_t nowUsec,
                                         uint64_t observation) noexcept;
    // Queue an ACK only for a previously committed resent ID on the exact
    // circuit/login incarnation; Full leaves the ACK queue unchanged.
    ReceiveStatus acknowledgeCommittedDuplicate(LLTransportEndpoint endpoint, uint64_t incarnation,
                                                 uint64_t loginGeneration, uint32_t packetId, uint64_t nowUsec) noexcept;
    // Allocation-free admission preflight for a reliable internal control.
    // Full means no trailer/body effect may be applied; Duplicate means an
    // already committed resent packet with ACK capacity, not a new delivery.
    // DuplicateFull separately reports a committed duplicate lacking ACK room.
    // The sole owner must not process another receive before reservation.
    ReceiveStatus preflightReliableControl(LLTransportEndpoint endpoint, uint64_t incarnation,
                                           uint64_t loginGeneration, uint32_t packetId, bool resent) const noexcept;
    // Legacy combined path retained for existing source tests; new actor uses
    // the explicit observation/retention path above.
    ReceiveResult reserveReceive(LLTransportEndpoint endpoint, uint32_t packetId, bool resent,
                                 bool reliable, uint64_t nowUsec) noexcept;
    // Commit only after owned decode-queue publication is guaranteed; actor
    // must not interleave close/timeout with publication and this commit.
    Status commitReceive(uint64_t reservation, uint32_t bytes, uint64_t nowUsec) noexcept;
    void cancelReceive(uint64_t reservation) noexcept;
    // Called on StartPingCheck's OldestUnacked, using the legacy wrap cleanup.
    Status forgetReceivedBefore(LLTransportEndpoint endpoint, uint64_t incarnation, uint32_t oldestId, uint64_t nowUsec) noexcept;
    // ownerAppSequence is the current owner-global published APP sequence at
    // this exact transition; it is carried to main as a processing barrier.
    LLTransportCircuitCompletion conditionalClose(const LLTransportCircuitCommand& command, uint64_t ownerAppSequence) noexcept;
    LLTransportCircuitCompletion disable(const LLTransportCircuitCommand& command, uint64_t ownerAppSequence) noexcept;
    LLTransportCircuitCompletion disable(LLTransportEndpoint endpoint, uint64_t incarnation, uint64_t ownerAppSequence) noexcept;
    // Shutdown closes all circuits; accepted completions remain owned until
    // drained. Unaccepted reservations are cancelled without completions.
    void shutdown() noexcept;
    // Reserve storage and its completion before accepting the main command.
    // After acceptance allocate the packet ID exactly once before compression;
    // failures after allocation do not roll the ID back.
    Status reserveReliableSlot(LLTransportEndpoint endpoint, uint64_t incarnation, uint64_t completionToken) noexcept;
    // Actor admission must match the live circuit's login as well as its
    // incarnation. A stale generation never reserves a slot or terminal.
    Status reserveReliableSlot(LLTransportEndpoint endpoint, uint64_t incarnation,
                               uint64_t loginGeneration, uint64_t completionToken) noexcept;
    // The actor calls accept only when command publication is guaranteed;
    // unaccepted reservations cancel without generating a completion.
    Status acceptReliableSlot(uint64_t completionToken) noexcept;
    // Cancel only before command acceptance and before assigning an ID.
    Status cancelReliableSlot(uint64_t completionToken) noexcept;
    Status allocatePacketId(LLTransportEndpoint endpoint, uint64_t incarnation, uint32_t& packetId) noexcept;
    Status allocateReliablePacketId(uint64_t completionToken, uint32_t& packetId) noexcept;
    Status prepareReliable(uint64_t completionToken, uint32_t packetId, const uint8_t* bytes,
                           uint32_t size, RetryPolicy policy, uint64_t nowUsec) noexcept;
    Status failReliable(uint64_t completionToken, int32_t originalStatus) noexcept;
    // Caller reserves bounded output capacity before polling. At most one action
    // is outstanding: on WouldBlock, defer immediately before polling again.
    // Defer changes no packet, deadline, retry count or ACK order; it allows
    // other ACK/ping/control work to run before this data is reacquired.
    // Bind an ACK/ping action's ID once before encoding; a deferred control
    // action is reacquired with the same ID and reserved ACKs. Resends reuse
    // their reliable packet ID and allocate none.
    Action advance(uint64_t nowUsec, uint64_t ackCollectUsec) noexcept;
    Action advance(uint64_t nowUsec, uint64_t ackCollectUsec, DirtyIncarnations& dirty) noexcept;
    // For a selected resend, sample its own circuit's ping policy before the
    // first socket attempt; retain this value across would-block retries.
    // Failure preserves out. Fixed/negative policies return their own interval.
    Status selectedRetryInterval(uint64_t actionToken, uint64_t nowUsec, int64_t& out) const noexcept;
    // Resolve a completed syscall attempt (success OR permanent failure). A
    // WouldBlock is instead deferred without consuming retry/deadline state.
    Status assignActionPacketId(uint64_t actionToken, uint32_t& packetId) noexcept;
    Status commitAttempt(uint64_t actionToken, uint64_t nowUsec,
                         int64_t nextRetryIntervalUsec, bool sent, uint32_t wireBytes) noexcept;
    Status defer(uint64_t actionToken) noexcept;
    // Reserve up to min(capacity, 250) ACKs for an ordinary datagram. The
    // actor owns wire encoding and resolves this before action resolution:
    // attempted=true consumes them even on a permanent send failure (legacy),
    // false releases without consuming on WouldBlock/cancel.
    Status reservePiggyback(LLTransportEndpoint endpoint, uint64_t incarnation,
                            uint32_t capacity, Piggyback& value) noexcept;
    Status finishPiggyback(uint64_t token, bool attempted) noexcept;
    Status consumeAck(LLTransportEndpoint endpoint, uint64_t incarnation, uint32_t packetId) noexcept;
    // Inject legacy heartbeat timing and random draws, each in [0,1). The
    // actor wakes at nextWatchdogUsec and polls with a reserved event slot.
    Status configureWatchdog(LLTransportEndpoint endpoint, uint64_t incarnation, uint64_t heartbeatUsec,
                             uint64_t timeoutUsec, uint64_t nowUsec, float offsetDraw, float jitterDraw) noexcept;
    Status nextWatchdogUsec(LLTransportEndpoint endpoint, uint64_t incarnation, uint64_t& dueUsec) const noexcept;
    WatchdogPoll pollWatchdog(LLTransportEndpoint endpoint, uint64_t incarnation, uint64_t nowUsec,
                              bool eventSlotReserved, uint64_t ownerAppSequence, float jitterDraw) noexcept;
    // Timeout marks the circuit dead; finish waits for main's callback/revive
    // decision, then closes only if the same circuit was not revived.
    Status beginWatchdogTimeout(LLTransportEndpoint endpoint, uint64_t incarnation, uint64_t nowUsec,
                                uint64_t timeoutUsec, bool eventSlotReserved, uint64_t ownerAppSequence, TimeoutEvent& event) noexcept;
    Status finishWatchdogTimeout(const TimeoutEvent& event, float jitterDraw) noexcept;
    // Ordinary (non-action) datagrams report their final wire length here;
    // failed sends report zero. Resends do not increment legacy bytesOut.
    Status noteOrdinaryAttempt(LLTransportEndpoint endpoint, uint64_t incarnation, bool sent, uint32_t wireBytes) noexcept;
    Status successfulBytesOut(LLTransportEndpoint endpoint, uint64_t incarnation, uint64_t& bytes) const noexcept;
    Status notePingReply(LLTransportEndpoint endpoint, uint64_t incarnation, uint8_t pingId,
                         uint64_t heartbeatUsec, uint64_t nowUsec) noexcept;
    // Effective average includes outstanding ping time and is capped at 2000 ms,
    // matching LLCircuitData::getPingDelayAveraged.
    Status effectivePingMilliseconds(LLTransportEndpoint endpoint, uint64_t incarnation,
                                     uint64_t heartbeatUsec, uint64_t nowUsec, float& value) const noexcept;
    // Called after ping send, with the legacy min(5 seconds, averaged-ping * 16)
    // timeout computed by the owner from its current reliable ping state.
    Status expireLosses(LLTransportEndpoint endpoint, uint64_t incarnation,
                        uint64_t nowUsec, uint64_t timeoutUsec) noexcept;
    // Actor must reserve main-event capacity before taking a completion.
    // Owner only. A nonmutating probe permits shutdown to distinguish an
    // empty state from a full terminal lane without consuming a completion.
    bool hasCompletion() const noexcept;
    bool takeCompletion(Completion& completion) noexcept;
    uint32_t oldestUnacked(LLTransportEndpoint endpoint, uint64_t incarnation) const noexcept;
private:
    Action advanceImpl(uint64_t nowUsec, uint64_t ackCollectUsec, DirtyIncarnations* dirty) noexcept;
    struct Impl;
    ReceiveResult reserveReceiveImpl(LLTransportEndpoint endpoint, uint32_t packetId, bool resent,
                                     bool reliable, uint64_t nowUsec, bool alreadyObserved) noexcept;
    bool initWithFault(const Limits& limits, uint64_t& chargedBytes, uint32_t failBeforeAllocation) noexcept;
    std::unique_ptr<Impl> mImpl;
    uint32_t mAllocationCalls = 0;
};
#endif
