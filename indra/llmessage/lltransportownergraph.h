/** Prepared transport owner storage. Not yet connected to viewer startup.
 * SPDX-License-Identifier: LGPL-2.1-only */
#ifndef LL_LLTRANSPORTOWNERGRAPH_H
#define LL_LLTRANSPORTOWNERGRAPH_H

#include "lltransportowneradmission.h"
#include "lltransportownerworker.h"
#include "lltransportringstats.h"
#include <memory>
#include <utility>

// Construct on main, while the legacy socket and ring remain exclusively main
// owned. The lease owns every schema; the ring must outlive this graph. A later
// handoff must retain the graph through worker join and all borrowed-view and
// terminal drains. Preparing storage alone never starts a worker or changes the
// ring. In particular it is not permission to enable the incomplete RX path.
class LLTransportOwnerGraph final
{
public:
    enum class Prepare { Prepared, Invalid, Capacity, AllocationFailed, Unavailable };
    static Prepare prepareOnMain(const LLTransportOwnerConfig& config,
        LLPacketRing& ring, std::shared_ptr<LLTransportOwnerGraph>& out) noexcept;
    enum class Start { Started, FailedRestored, FailedClosed, Invalid, Unavailable };
    // Caller first fences all legacy socket/ring readers and proves protocol
    // and policy readiness. FailedClosed MUST retain/install this graph and
    // abort login; it is never permission to resume legacy ring access.
    Start startOnMain() noexcept;
#if defined(LL_TRANSPORT_OWNER_TESTING)
    Start startForTestOnMain() noexcept { return startImplOnMain(); }
#endif
    // Started acknowledges socket binding, not indefinite worker health.
    // Main must observe failure/wake and run the fail-closed drain path.
    bool failedOnMain() const noexcept;
    // Main only, outside an active offer. Closing main admission precedes
    // stop publication. Join never implies that queues/readers are drained.
    bool requestStopOnMain() noexcept;
    bool joinOnMain() noexcept;
    bool joinedOnMain() const noexcept { return mJoinedOnMain; }
    // Absent warning kinds succeed with present=false and leave out unchanged.
    bool finalWarningSnapshotOnMain(uint32_t kind,LLTransportControlWarningStats& out,bool& present) const noexcept;
    bool acknowledgeFinalWarningsOnMain() noexcept;
    // Caller must already have cleared every main reader/callback. Retained
    // facade/graph references become inert once the borrowed runtime is gone.
    bool detachDrainedRuntimeOnMain() noexcept;
    ~LLTransportOwnerGraph();
    LLTransportOwnerGraph(const LLTransportOwnerGraph&) = delete;
    LLTransportOwnerGraph& operator=(const LLTransportOwnerGraph&) = delete;

private:
    friend class LLTransportOwnerFacade;
    Start startImplOnMain() noexcept;
    LLTransportOwnerGraph(const LLTransportOwnerConfig& config,
                          std::shared_ptr<LLTransportOwnerWait> wait) noexcept;
    struct Runtime;

    LLTransportOwnerConfig mConfig;
    LLTransportOwnerChannels mChannels;
    LLTransportIngressQueue mIngress;
    LLTransportDecodedArena mArena;
    LLTransportCircuitState mState;
    LLTransportOwnerLoop::Directory mDirectory;
    LLTransportRingPublication mRingStats;
    std::atomic<bool> mPoisoned{false};
    LLTransportOwnerAdmission mAdmission;
    std::shared_ptr<LLTransportOwnerWait> mWait;
    std::unique_ptr<Runtime> mRuntime;
    std::unique_ptr<LLRandLagFib2281> mPriorDropRandom;
    bool mRingTransferred=false;
    bool mStartAttempted=false; // failed startup requires a fresh graph
    bool mStopRequestedOnMain=false, mJoinedOnMain=false, mDetachedOnMain=false;
    bool mFinalWarningsAcknowledged=false, mFinalFailed=false;
    mutable uint8_t mFinalWarningSnapshotMask=0;
    // Only the eventual successful handoff may set this on main. Prepared
    // storage must not be treated as an operational owner.
    bool mActiveOnMain=false;
};

// Concrete main-side adapter. No method starts the worker; install only after
// the transactional socket handoff and protocol prerequisites are complete.
// The shared graph pins queues and schemas across every operation, including
// an offer racing owner failure/close. Main destroys it only after join/drain.
class LLTransportOwnerFacade final : public LLTransportMainFacade
{
public:
    explicit LLTransportOwnerFacade(std::shared_ptr<LLTransportOwnerGraph> graph) noexcept
        : mGraph(std::move(graph)) {}
    bool getCircuitSnapshot(LLTransportEndpoint, LLTransportCircuitSnapshot&) const noexcept override;
    bool backlog(LLTransportBacklog&) const noexcept override;
    bool ingressPending(bool&) const noexcept override;
    bool ringStats(LLTransportRingStats&) const noexcept override;
    // Commands and sends use one ordered lane. Operations with callback
    // reservations must use begin/offer/end around reservation AND rollback;
    // these nonreentrant main-only operations must not invoke callbacks/yield.
    bool beginOfferOnMain() noexcept override;
    LLTransportCommandOffer offerReservedOnMain(const LLTransportOwnerCommand&) noexcept override;
    void endOfferOnMain() noexcept override;
    // Convenience only for commands without an external callback reservation.
    LLTransportCommandOffer submitOrdered(const LLTransportOwnerCommand&) noexcept;
    LLTransportCommandOffer submitCircuitCommand(const LLTransportCircuitCommand&) noexcept override;
    bool popCircuitCompletion(LLTransportCircuitCompletion&) noexcept override;
    bool peekCircuitCompletion(LLTransportCircuitCompletion&) noexcept override;
    bool releaseCircuitCompletion() noexcept override;
    bool peekOwnerControl(LLTransportOwnerControl&) noexcept override;
    bool releaseOwnerControl() noexcept override;
    bool peekOwnerFatal(LLTransportOwnerFatal&) noexcept override;
    bool releaseOwnerFatal() noexcept override;
    bool controlWarningStats(uint32_t, LLTransportControlWarningStats&) const noexcept override;
    bool peekReliableCompletion(LLTransportSendCompletion&) noexcept override;
    bool releaseReliableCompletion() noexcept override;
    bool popUnreliableCompletion(LLTransportUnreliableCompletion&) noexcept override;
    void failCompletion() noexcept override;
    bool legacyMessageTimeUsec(uint64_t&) const noexcept override;
    bool peekInbound(LLTransportIngressEvent&, LLTransportDecodedView&) noexcept override;
    bool inboundEmptyAfterJoinOnMain() const noexcept override;
    bool releaseInbound() noexcept override;
    bool schemaMatches(uint32_t, uint64_t) const noexcept override;
    void failInbound() noexcept override;
private:
    std::shared_ptr<LLTransportOwnerGraph> mGraph;
    std::shared_ptr<LLTransportOwnerGraph> mOfferGraph;
};
#endif
