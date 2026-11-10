/** Disabled owner-thread scheduler, under construction. SPDX-License-Identifier: LGPL-2.1-only */
#ifndef LL_LLTRANSPORTOWNERLOOP_H
#define LL_LLTRANSPORTOWNERLOOP_H
#include "lltransportownerapi.h"
#include "lltransportreceivepump.h"
#include "lltransportcircuitdirectory.h"
#include <atomic>
#include <memory>
#include <utility>

// One owner thread only. This is a bounded service slice, not yet a runnable
// backend: admission/credits, send/control scheduling, lifecycle, snapshots,
// off-circuit handshake and pending-send invalidation remain required.
class LLTransportOwnerLoop
{
public:
    enum class Step : uint8_t { Idle, Progress, Blocked, SendPending, StopPending, Poisoned };
    enum class PreparationFailure : uint8_t { None, PacketIdUnavailable, EncodeFailed, StatePrepareFailed };
    using Directory=LLTransportCircuitDirectory<2048>;
    LLTransportOwnerLoop(LLTransportCircuitState& state, LLTransportReceivePump& receive,
                         LLTransportOwnerChannels& channels, std::atomic<bool>& poisoned,
                         std::shared_ptr<LLTransportMainWake> wake = {}) noexcept
        : mState(state), mReceive(receive), mChannels(channels), mPoisoned(poisoned), mWake(std::move(wake)) {}

    // The caller supplies the actual pre-reserved watchdog policy and random
    // draws. Only the head command may execute; Send and Stop remain pending
    // for the corresponding scheduler/shutdown path. A terminal slot must be
    // reserved before any circuit mutation; the owner uses the pump's current
    // global APP sequence as the close barrier at the exact transition.
    // A successful Enable requires a directory mutation bracket so its
    // snapshot can be published before the terminal becomes visible.
    Step processCircuitHead(LLTransportSteadyUsec now,
                            LLTransportCircuitState::WatchdogConfig watchdog,
                            Directory* directory=nullptr) noexcept;
    // Initial reliable send admission for a previously accepted ordered
    // command. Unreliable sends and full backend shutdown remain separate.
    // Frozen schema metadata stays alive until all inbound readers drain.
    Step processReliableSendHead(LLTransportSteadyUsec now, uint64_t loginGeneration,
                                 const LLTransportSchemaEntry* schemas, std::size_t schemaCount) noexcept;
    Step processUnreliableSendHead(LLTransportSteadyUsec now, uint64_t loginGeneration,
                                   const LLTransportSchemaEntry* schemas, std::size_t schemaCount) noexcept;
    Step sendUnreliableOnce(LLPacketRing& ring, int socket, const LLPacketRing::UDPProxySnapshot& proxy,
                            LLTransportSteadyUsec now) noexcept;
    Step cancelOneUnreliableForStop() noexcept;
    // A reliable completion has reserved main-side credit before admission;
    // do not consume the state value until a separate send-terminal slot exists.
    Step publishReliableCompletion() noexcept;
    LLTransportRxStatus pollReceive(int socket, const LLPacketRing::UDPProxySnapshot& proxy,
                                    uint64_t loginGeneration, LLTransportSteadyUsec now) noexcept;
    LLTransportRxStatus resumeBusyAcknowledgements(uint64_t loginGeneration) noexcept;
    // Only call after pollReceive returned ControlPending. Apply and commit
    // CompletePing/PacketAck in the same owner turn. Truncated controls
    // require the exact warning to have been published under the frozen
    // no-callback policy before applying their effects.
    Step servicePendingControl(LLTransportSteadyUsec now, uint64_t heartbeatUsec,
                               bool diagnosticsHandled) noexcept;
    // Retain StartPing response before applying/committing input; it cannot
    // become sendable until that input has committed successfully.
    Step prepareStartPingResponse(LLTransportSteadyUsec now, uint64_t heartbeatUsec,
                                  bool diagnosticsHandled) noexcept;
    Step discardStaleHeldControl(uint64_t currentLoginGeneration, LLTransportSteadyUsec now) noexcept;
    Step discardEmptyUnreliableControl(bool diagnosticsHandled) noexcept;
    Step sendControlResponseOnce(LLPacketRing& ring, int socket,
                                 const LLPacketRing::UDPProxySnapshot& proxy, LLTransportSteadyUsec now) noexcept;
    // Bounded manual iteration of the actual owner methods; does not start a
    // thread, install a backend or bracket snapshot-directory publication.
    // Caller must supply independent RNG draws and the full owner lifecycle.
    Step driveOnce(Directory& directory, LLPacketRing& ring, int socket, const LLPacketRing::UDPProxySnapshot& proxy,
                   LLTransportSteadyUsec now, uint64_t loginGeneration,
                   const LLTransportSchemaEntry* schemas, std::size_t schemaCount,
                   LLTransportCircuitState::WatchdogConfig watchdog, bool diagnosticsHandled) noexcept;
    // Startup only, before worker ownership. Further changes are ordered
    // SetAckCollectTime commands with reserved completion credit.
    bool initializeAckCollectTime(uint64_t usec) noexcept
    { if (usec>2000000) return false; mAckCollectUsec=usec; return true; }
    // True means a bounded receive turn must run before any native wait. Only
    // an explicit WouldBlock clears it; a readiness wake sets it again.
    bool needsReceiveTurn(LLTransportSteadyUsec now) const noexcept
    { return mReadTurnNeeded && !mReceiveHeld && now.value>=mReceiveRetryAt; }
    uint64_t receiveRetryAt() const noexcept { return mReceiveRetryAt; }
    bool wantsReadableWake(LLTransportSteadyUsec now) const noexcept
    { return !mReceiveHeld && (!mReadTurnNeeded || now.value>=mReceiveRetryAt); }
    bool wantsWritableWake() const noexcept;
    void notifySocketReadable() noexcept { mReadTurnNeeded=true; }
    // A wait-layer socket error has already been consumed by SO_ERROR/WSA.
    // Apply the same bounded policy as a receive error; zero requests an
    // immediate read, false means the native error is terminal.
    bool noteSocketError(int nativeError, LLTransportSteadyUsec now) noexcept;
    int lastReceiveNativeError() const noexcept { return mLastReceiveNativeError; }
    // The surrounding sole owner chooses an active circuit fairly and must
    // resolve a held receive before timing that same circuit out. Timeout
    // events reserve independent control capacity before state is changed.
    Step pollWatchdog(LLTransportEndpoint endpoint, uint64_t incarnation,
                      LLTransportSteadyUsec now, float jitterDraw) noexcept;
    // Fixed-capacity registry, allocated/accounted at startup. One call
    // examines one slot so a busy circuit cannot starve other circuits.
    static std::size_t watchdogChargeBytes(uint32_t capacity) noexcept;
    bool initializeWatchdogs(uint32_t capacity) noexcept;
    Step pollNextWatchdog(LLTransportSteadyUsec now, float jitterDraw) noexcept;
    // Startup allocates and accounts these two fixed lanes before a worker is
    // started. Data backpressure cannot consume control retention capacity.
    static constexpr uint32_t PendingPerLane=256;
    static std::size_t pendingChargeBytes() noexcept;
    bool initializePending() noexcept;
    // One real state action, at most one nonblocking socket attempt. The
    // caller interleaves RX, ordered commands and watchdog work between calls.
    // This disabled slice does not yet attach piggyback ACKs and must not be
    // installed as a parity-complete backend.
    Step sendStateActionOnce(LLPacketRing& ring, int socket,
                             const LLPacketRing::UDPProxySnapshot& proxy,
                             LLTransportSteadyUsec now, uint64_t ackCollectUsec) noexcept;
    // Owner consumes these within the same directory mutation bracket. A
    // missing/mismatched snapshot or overflow must fail the directory closed.
    const LLTransportCircuitState::DirtyIncarnations& lastAdvanceDirty() const noexcept { return mLastAdvanceDirty; }
    LLTransportReceivePump::AffectedCircuit lastActionAffected() const noexcept { return mLastActionAffected; }
    // Only after accepted command intake has stopped and the ordered command
    // queue is settled except for a possibly admitted Busy head. Existing
    // terminal credits remain held until main consumes each completion;
    // retained inbound views are not released here.
    void cancelInFlightForStop() noexcept;
    // Owner closes the same publication word used by producer admission;
    // settlement then sees the committed prefix without waiting for main.
    void closeAdmissionForStop() noexcept { mChannels.commands.closeOnConsumer(); }
    // After cancelInFlightForStop and after admission has closed, settle one
    // queued command using the terminal credit reserved at admission. No
    // socket operation or main-thread consumer is required. Idle means the
    // command queue is empty; accepted state completions must still be drained
    // by publishReliableCompletion before the owner can exit.
    Step settleQueuedForStop() noexcept;
    PreparationFailure lastPreparationFailure() const noexcept { return mLastPreparationFailure.load(); }
private:
    struct Pending
    {
        LLTransportCircuitState::ActionKind kind=LLTransportCircuitState::ActionKind::None;
        uint64_t incarnation=0, reliableToken=0, firstAttemptUsec=0;
        int64_t nextRetryIntervalUsec=0;
        uint32_t packetId=0, size=0, attempts=0;
        LLTransportEndpoint endpoint{};
        uint64_t replyObservation=0, replySequence=0;
        bool controlReply=false, replyReady=false;
        bool unreliable=false;
        bool allowPiggyback=false;
        uint64_t loginGeneration=0, unreliableToken=0, unreliableSequence=0;
        std::array<uint8_t,LLTransportCircuitState::MaxPacketBytes> bytes{};
    };
    struct Watch { LLTransportEndpoint endpoint{}; uint64_t incarnation=0; };
    Step finishControlBackpressure() noexcept;
    void discardIncarnation(uint64_t incarnation) noexcept;
    void discardControlIncarnation(uint64_t incarnation) noexcept;
    void discardReliable(uint64_t incarnation, uint64_t token) noexcept;
    LLTransportCircuitState& mState;
    LLTransportReceivePump& mReceive;
    LLTransportOwnerChannels& mChannels;
    std::atomic<bool>& mPoisoned;
    std::shared_ptr<LLTransportMainWake> mWake;
    std::unique_ptr<Pending[]> mControlPending, mDataPending;
    std::unique_ptr<Watch[]> mWatches;
    uint32_t mWatchCapacity=0, mWatchCursor=0;
    std::atomic<PreparationFailure> mLastPreparationFailure{PreparationFailure::None};
    uint64_t mAdmittedHeadToken=0, mAdmittedHeadIncarnation=0; // owner-only Busy head
    uint64_t mReplySequence=0;
    uint64_t mUnreliableSequence=0;
    // One application first attempt retains global legacy wire ordering.
    // It does not block RX, ACK/ping, timers or already-unacked retries.
    uint64_t mFirstAttemptToken=0, mFirstAttemptIncarnation=0;
    bool mFirstAttemptReliable=false;
    uint64_t mAckCollectUsec=0; // owner only after startup
    bool mReceiveHeld=false; // owner only, a control/replay retains pump bytes
    bool mReadTurnNeeded=true;
    uint64_t mReceiveRetryAt=0; // owner-only steady time; zero means no delay
    int mLastReceiveNativeError=0;
    LLTransportCircuitState::DirtyIncarnations mLastAdvanceDirty{};
    LLTransportReceivePump::AffectedCircuit mLastActionAffected{};
    LLTransportReceivePump::AffectedCircuit mLastReplyAffected{};
    LLTransportReceivePump::AffectedCircuit mLastUnreliableAffected{};
};
#endif
