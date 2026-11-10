/** Prepared transport owner storage. SPDX-License-Identifier: LGPL-2.1-only */
#include "linden_common.h"
#include "lltransportownergraph.h"
#include <limits>
#include <tuple>
#include <utility>

// Every object that borrows the ring/socket lives in this resettable
// holder. It is destroyed only after owner join and authoritative drains.
struct LLTransportOwnerGraph::Runtime
{
    LLPacketRing& ring;
    LLTransportReceivePump receive;
    LLTransportOwnerLoop loop;
    LLTransportOwnerWorker worker;
    Runtime(LLTransportOwnerGraph& g,LLPacketRing& r) noexcept
        : ring(r), receive(r,g.mState,g.mArena,g.mIngress,g.mPoisoned,g.mConfig.schemas,g.mConfig.schemaCount),
          loop(g.mState,receive,g.mChannels,g.mPoisoned,g.mConfig.mainWake),
          worker(loop,g.mDirectory,r,g.mRingStats,g.mConfig,g.mWait) {}
};

LLTransportOwnerGraph::LLTransportOwnerGraph(const LLTransportOwnerConfig& config,
    std::shared_ptr<LLTransportOwnerWait> wait) noexcept
    : mConfig(config),mAdmission(mChannels,config.schemas,config.schemaCount),mWait(std::move(wait)) {}
LLTransportOwnerGraph::~LLTransportOwnerGraph() = default;
bool LLTransportOwnerGraph::failedOnMain() const noexcept
{ return mRuntime ? mRuntime->worker.failed() : mFinalFailed; }

LLTransportOwnerGraph::Prepare LLTransportOwnerGraph::prepareOnMain(const LLTransportOwnerConfig& config,
    LLPacketRing& ring, std::shared_ptr<LLTransportOwnerGraph>& out) noexcept
{
    if (config.socket < 0 || !config.mainWake || !config.schemaLease || !config.schemas ||
        !config.schemaCount || !config.loginGeneration || !config.controlTruncationCallbacksAbsent || !config.totalBudgetBytes ||
        !config.allocatorOverheadBytes || config.initialAckCollectUsec > 2000000 ||
        config.schemaLease->entries() != config.schemas || config.schemaLease->count() != config.schemaCount ||
        config.schemaLease->retainedBytes() != config.schemaBytes || !config.schemaBytes ||
        config.arenaCapacityBytes > LLTransportDecodedArena::kMaximumCapacityBytes ||
        config.maxArenaRecordBytes > LLTransportDecodedArena::kMaximumRecordBytes ||
        config.maxArenaRecordBytes > config.arenaCapacityBytes / 2)
        return Prepare::Invalid;

    // The schema's conservative bound includes protocol zero-fill and all
    // 255 variable repeats. Reject before ownership handoff if even one
    // supported packet might exceed scratch or the packed arena record.
    for (uint32_t i=0; i<config.schemaCount; ++i)
    {
        const auto& entry=config.schemas[i];
        LLTransportCapacity cap;
        if (!entry.schema || !entry.schema->isFrozen() || !entry.schema->generation() ||
            entry.messageNumber != entry.schema->messageNumber() ||
            (i && config.schemas[i-1].messageNumber >= entry.messageNumber) ||
            !entry.schema->capacityForWireBytes(NET_BUFFER_SIZE,cap) ||
            cap.bytes > std::tuple_size_v<decltype(LLTransportIngressScratch::data)> ||
            cap.blocks > std::tuple_size_v<decltype(LLTransportIngressScratch::blocks)> ||
            cap.fields > std::tuple_size_v<decltype(LLTransportIngressScratch::fields)> ||
            LLTransportDecodedArena::requiredRecordBytes(static_cast<uint32_t>(cap.bytes),
                static_cast<uint32_t>(cap.blocks),static_cast<uint32_t>(cap.fields)) > config.maxArenaRecordBytes)
            return Prepare::Capacity;
    }

    uint64_t stateBytes=0, ringBytes=0;
    if (!LLTransportCircuitState::estimateBytes(config.stateLimits,stateBytes) ||
        !ring.ownerRetainedBytesOnMain(ringBytes)) return Prepare::Invalid;
    uint64_t total=sizeof(LLTransportOwnerGraph);
    const auto maximum=(std::numeric_limits<uint64_t>::max)();
    auto add=[&](uint64_t value) noexcept {
        if (value > maximum-total) return false;
        total+=value; return true;
    };
    // Requested bytes of all graph arrays, its retained schema and the existing
    // ring. The wait's capped allocation payload is reserved separately; the
    // caller allowance covers other shared control blocks and allocator metadata.
    // OS resource bytes and the worker thread stack are separate from this ledger.
    if (!add(sizeof(Runtime)) || !add(stateBytes) || !add(ringBytes) || !add(config.schemaBytes) ||
        !add(config.arenaCapacityBytes) || !add(LLTransportOwnerLoop::pendingChargeBytes()) ||
        !add(LLTransportOwnerLoop::watchdogChargeBytes(config.stateLimits.circuits)) ||
        !add(sizeof(LLRandLagFib2281)) || !add(LLTransportOwnerWaitHeapBudgetBytes) ||
        !add(config.allocatorOverheadBytes) ||
        total > config.totalBudgetBytes) return Prepare::Capacity;

    auto wait=llCreateTransportOwnerWait();
    if (!wait) return Prepare::Unavailable;
    try
    {
        auto graph=std::shared_ptr<LLTransportOwnerGraph>(new LLTransportOwnerGraph(config,std::move(wait)));
        graph->mRuntime=std::make_unique<Runtime>(*graph,ring);
        uint64_t charged=0;
        if (!graph->mAdmission.schemaTableValidOnMain()) return Prepare::Invalid;
        if (!graph->mState.init(config.stateLimits,charged) || charged != stateBytes ||
            !graph->mArena.initialize(static_cast<std::size_t>(config.arenaCapacityBytes),
                                      static_cast<std::size_t>(config.maxArenaRecordBytes)) ||
            !graph->mRuntime->loop.initializePending() || !graph->mRuntime->loop.initializeWatchdogs(config.stateLimits.circuits))
            return Prepare::AllocationFailed;
        out=std::move(graph);
        return Prepare::Prepared; // no socket handoff
    }
    catch (...) { return Prepare::AllocationFailed; }
}

LLTransportOwnerGraph::Start LLTransportOwnerGraph::startOnMain() noexcept
{
    if (!mConfig.experimentalEnabled) return Start::Unavailable;
    // The opt-in is nonpersistent and defaults off. Startup reports failure
    // without falling back if the socket handoff cannot be completed safely.
    return startImplOnMain();
}

LLTransportOwnerGraph::Start LLTransportOwnerGraph::startImplOnMain() noexcept
{
    if (!mRuntime || mStartAttempted || mActiveOnMain || mRingTransferred) return Start::Invalid;
    mStartAttempted=true;
    auto next=LLPacketRing::makeOwnerDropRandom(mConfig.dropSeed);
    if (!next) return Start::FailedRestored; // ring and socket untouched
    mPriorDropRandom=mRuntime->ring.exchangeOwnerDropRandomOnMain(std::move(next));
    mRingTransferred=true;
    if (mRuntime->worker.startOnMain())
    {
        mActiveOnMain=true;
        return Start::Started;
    }
    // startOnMain joined on failure. The worker proves it never entered an I/O
    // turn, including thread creation/bind failures and initial harvest failure.
    // Restore the harvested bytes only if they were actually removed. In all other
    // cases we cannot prove rollback is safe; main must retain the graph and
    // must not resume legacy access to the ring/socket.
    bool harvested=false;
    uint64_t in=0,out=0;
    if (mRuntime->worker.startupRollbackStateAfterJoinOnMain(harvested,in,out) &&
        (!harvested || mRuntime->ring.restoreInitialActualBytesOnMain(in,out)))
    {
        mRuntime->ring.exchangeOwnerDropRandomOnMain(std::move(mPriorDropRandom));
        mRingTransferred=false;
        return Start::FailedRestored;
    }
    mActiveOnMain=true;
    return Start::FailedClosed;
}

bool LLTransportOwnerGraph::requestStopOnMain() noexcept
{
    if (mDetachedOnMain) return true;
    if (!mActiveOnMain || mChannels.commands.offerActiveOnMain()) return false;
    if (!mStopRequestedOnMain)
    {
        mStopRequestedOnMain=true;
        mRuntime->worker.requestStopOnMain();
    }
    return true;
}

bool LLTransportOwnerGraph::joinOnMain() noexcept
{
    if (mDetachedOnMain) return true;
    if (!mActiveOnMain || !mStopRequestedOnMain || mChannels.commands.offerActiveOnMain()) return false;
    if (!mJoinedOnMain) { mRuntime->worker.joinOnMain(); mJoinedOnMain=true; }
    return true;
}

bool LLTransportOwnerGraph::finalWarningSnapshotOnMain(uint32_t kind,
    LLTransportControlWarningStats& out,bool& present) const noexcept
{
    if (!mJoinedOnMain || mDetachedOnMain) return false;
    const uint8_t bit=kind==1 ? 1 : kind==2 ? 2 : kind==0xfffffffbu ? 4 : 0;
    if (!bit || !mChannels.controlWarnings.readFinalOnMain(kind,out,present)) return false;
    mFinalWarningSnapshotMask|=bit;
    return true;
}
bool LLTransportOwnerGraph::acknowledgeFinalWarningsOnMain() noexcept
{
    if (!mJoinedOnMain || mDetachedOnMain || mFinalWarningSnapshotMask!=7) return false;
    mFinalWarningsAcknowledged=true;
    return true;
}
bool LLTransportOwnerGraph::detachDrainedRuntimeOnMain() noexcept
{
    if (mDetachedOnMain) return true;
    if (!mActiveOnMain || !mJoinedOnMain || !mFinalWarningsAcknowledged || !mRuntime ||
        !mAdmission.emptyOnMain() || !mChannels.commands.emptyAfterJoinOnMain() ||
        !mChannels.circuitTerminals.emptyAfterJoinOnMain() || !mChannels.sendTerminals.emptyAfterJoinOnMain() ||
        !mChannels.unreliableTerminals.emptyAfterJoinOnMain() || !mChannels.controls.emptyAfterJoinOnMain() ||
        !mChannels.fatal.emptyAfterJoinOnMain() || !mIngress.emptyAfterJoinOnMain() || !mArena.emptyAfterJoinOnMain())
        return false;
    mFinalFailed=mRuntime->worker.failed();
    mActiveOnMain=false; // all externally retained facades become inert first
    mRuntime.reset(); // destroys joined worker, loop and pump, ending ring borrow
    mWait.reset();
    mConfig.socket=-1;
    mRingTransferred=false;
    mDetachedOnMain=true;
    return true;
}

bool LLTransportOwnerFacade::getCircuitSnapshot(LLTransportEndpoint endpoint,
    LLTransportCircuitSnapshot& out) const noexcept
{
    const auto graph=mGraph;
    if (!graph || !graph->mActiveOnMain) return false;
    LLTransportCircuitSnapshot value;
    if (!graph->mDirectory.get(endpoint,value)) return false;
    if (value.state==LLTransportSnapshotState::Absent && graph->mAdmission.hasPendingEnableOnMain(endpoint))
        return false;
    out=value;
    return true;
}

bool LLTransportOwnerFacade::ingressPending(bool& out) const noexcept
{
    const auto graph=mGraph;
    if (!graph || !graph->mActiveOnMain) return false;
    return graph->mIngress.pendingOnConsumer(out);
}

bool LLTransportOwnerFacade::backlog(LLTransportBacklog& out) const noexcept
{
    const auto graph=mGraph;
    LLTransportRingStats stats;
    if (!graph || !graph->mActiveOnMain || !graph->mRingStats.readOnMain(stats)) return false;
    LLTransportBacklog value;
    value.pendingPackets=stats.bufferedPackets;
    value.pendingBytes=stats.bufferedBytes;
    value.loadRate=static_cast<float>(stats.bufferedPackets)/256.f;
    value.loginGeneration=stats.loginGeneration;
    out=value;
    return true;
}

bool LLTransportOwnerFacade::ringStats(LLTransportRingStats& out) const noexcept
{
    const auto graph=mGraph;
    LLTransportRingStats value;
    if (!graph || !graph->mActiveOnMain || !graph->mRingStats.readOnMain(value)) return false;
    value.reliableControlBackpressure=graph->mRuntime->receive.controlBackpressureOnOwner();
    value.duplicateControlAckBackpressure=graph->mRuntime->receive.controlDuplicateAckBackpressureOnOwner();
    out=value;
    return true;
}

LLTransportCommandOffer LLTransportOwnerFacade::submitCircuitCommand(const LLTransportCircuitCommand& value) noexcept
{
    LLTransportOwnerCommand command;
    switch (value.kind)
    {
    case LLTransportCircuitCommandKind::Enable: command.kind=LLTransportOwnerCommand::Kind::Enable; break;
    case LLTransportCircuitCommandKind::Disable: command.kind=LLTransportOwnerCommand::Kind::Disable; break;
    case LLTransportCircuitCommandKind::DisableIfUnchanged: command.kind=LLTransportOwnerCommand::Kind::DisableIfUnchanged; break;
    default: return LLTransportCommandOffer::Invalid;
    }
    command.circuit=value;
    return submitOrdered(command);
}

LLTransportCommandOffer LLTransportOwnerFacade::submitOrdered(const LLTransportOwnerCommand& command) noexcept
{
    if (!beginOfferOnMain()) return LLTransportCommandOffer::Unavailable;
    const auto result=offerReservedOnMain(command);
    endOfferOnMain();
    return result;
}
bool LLTransportOwnerFacade::beginOfferOnMain() noexcept
{
    const auto graph=mGraph;
    if (mOfferGraph || !graph || !graph->mActiveOnMain || graph->mStopRequestedOnMain || graph->mPoisoned.load() ||
        !graph->mChannels.commands.beginOfferOnMain()) return false;
    mOfferGraph=graph;
    return true;
}
LLTransportCommandOffer LLTransportOwnerFacade::offerReservedOnMain(const LLTransportOwnerCommand& command) noexcept
{
    const auto graph=mOfferGraph; // pins the complete outer offer stack
    if (!graph || !graph->mActiveOnMain || graph->mStopRequestedOnMain || graph->mPoisoned.load())
        return LLTransportCommandOffer::Unavailable;
    using Kind=LLTransportOwnerCommand::Kind;
    if (command.kind==Kind::FinishWatchdogTimeout)
    {
        if (command.timeout.loginGeneration!=graph->mConfig.loginGeneration) return LLTransportCommandOffer::Invalid;
    }
    else if (command.kind!=Kind::SetAckCollectTime && command.kind!=Kind::Stop &&
             command.circuit.loginGeneration!=graph->mConfig.loginGeneration)
        return LLTransportCommandOffer::Invalid;
    const auto result=graph->mAdmission.offerOnMain(command);
    if (result==LLTransportCommandOffer::Accepted) graph->mRuntime->worker.signalOnMain();
    return result;
}
void LLTransportOwnerFacade::endOfferOnMain() noexcept
{
    if (!mOfferGraph) return;
    mOfferGraph->mChannels.commands.endOfferOnMain();
    mOfferGraph.reset();
}

bool LLTransportOwnerFacade::popCircuitCompletion(LLTransportCircuitCompletion& out) noexcept
{
    LLTransportCircuitCompletion copy;
    if (!peekCircuitCompletion(copy) || !releaseCircuitCompletion()) return false;
    out=copy;
    return true;
}

bool LLTransportOwnerFacade::peekCircuitCompletion(LLTransportCircuitCompletion& out) noexcept
{
    const auto graph=mGraph;
    if (!graph || !graph->mActiveOnMain) return false;
    const auto* value=graph->mChannels.circuitTerminals.peekConsumer();
    if (!value) return false;
    if (!graph->mAdmission.matchesCircuitOnMain(*value)) { failCompletion(); return false; }
    out=*value;
    return true;
}

bool LLTransportOwnerFacade::releaseCircuitCompletion() noexcept
{
    const auto graph=mGraph;
    if (!graph || !graph->mActiveOnMain) return false;
    const auto* value=graph->mChannels.circuitTerminals.peekConsumer();
    if (!value || !graph->mAdmission.matchesCircuitOnMain(*value)) { failCompletion(); return false; }
    const auto copy=*value;
    if (!graph->mChannels.circuitTerminals.releaseConsumer() ||
        !graph->mAdmission.releaseCircuitCreditOnMain(copy)) { failCompletion(); return false; }
    return true;
}

bool LLTransportOwnerFacade::peekOwnerControl(LLTransportOwnerControl& out) noexcept
{
    const auto graph=mGraph;
    if (!graph || !graph->mActiveOnMain) return false;
    const auto* value=graph->mChannels.controls.peekConsumer();
    if (!value) return false;
    out=*value; return true;
}
bool LLTransportOwnerFacade::releaseOwnerControl() noexcept
{
    const auto graph=mGraph;
    return graph && graph->mActiveOnMain && graph->mChannels.controls.releaseConsumer();
}
bool LLTransportOwnerFacade::peekOwnerFatal(LLTransportOwnerFatal& out) noexcept
{
    const auto graph=mGraph;
    if (!graph || !graph->mActiveOnMain) return false;
    const auto* value=graph->mChannels.fatal.peekConsumer();
    if (!value) return false;
    out=*value; return true;
}
bool LLTransportOwnerFacade::releaseOwnerFatal() noexcept
{
    const auto graph=mGraph;
    return graph && graph->mActiveOnMain && graph->mChannels.fatal.releaseConsumer();
}
bool LLTransportOwnerFacade::controlWarningStats(uint32_t number, LLTransportControlWarningStats& out) const noexcept
{
    const auto graph=mGraph;
    return graph && graph->mActiveOnMain && graph->mChannels.controlWarnings.readOnMain(number,out);
}
bool LLTransportOwnerFacade::peekReliableCompletion(LLTransportSendCompletion& out) noexcept
{
    const auto graph=mGraph;
    if (!graph || !graph->mActiveOnMain) return false;
    const auto* value=graph->mChannels.sendTerminals.peekConsumer();
    if (!value) return false;
    if (!graph->mAdmission.matchesReliableOnMain(*value)) { failCompletion(); return false; }
    out=*value; return true;
}
bool LLTransportOwnerFacade::releaseReliableCompletion() noexcept
{
    const auto graph=mGraph;
    if (!graph || !graph->mActiveOnMain) return false;
    const auto* value=graph->mChannels.sendTerminals.peekConsumer();
    if (!value || !graph->mAdmission.matchesReliableOnMain(*value)) { failCompletion(); return false; }
    const auto copy=*value;
    if (!graph->mChannels.sendTerminals.releaseConsumer() ||
        !graph->mAdmission.releaseReliableCreditOnMain(copy)) { failCompletion(); return false; }
    return true;
}
bool LLTransportOwnerFacade::popUnreliableCompletion(LLTransportUnreliableCompletion& out) noexcept
{
    const auto graph=mGraph;
    if (!graph || !graph->mActiveOnMain) return false;
    const auto* value=graph->mChannels.unreliableTerminals.peekConsumer();
    if (!value) return false;
    if (!graph->mAdmission.matchesUnreliableOnMain(*value)) { failCompletion(); return false; }
    const auto copy=*value;
    if (!graph->mChannels.unreliableTerminals.releaseConsumer() ||
        !graph->mAdmission.releaseUnreliableCreditOnMain(copy)) { failCompletion(); return false; }
    out=copy;
    return true;
}
void LLTransportOwnerFacade::failCompletion() noexcept
{
    const auto graph=mGraph;
    if (graph) { graph->mPoisoned.store(true); if (graph->mRuntime) graph->mRuntime->worker.signalOnMain(); }
}
bool LLTransportOwnerFacade::legacyMessageTimeUsec(uint64_t& out) const noexcept
{
    const auto graph=mGraph;
    if (!graph || !graph->mActiveOnMain) return false;
    out=llTransportLegacyAt(graph->mConfig.legacyAnchor,graph->mConfig.steadyAnchor,llTransportSteadyNow()).value;
    return true;
}
bool LLTransportOwnerFacade::peekInbound(LLTransportIngressEvent& out, LLTransportDecodedView& view) noexcept
{
    const auto graph=mGraph;
    if (!graph || !graph->mActiveOnMain) return false;
    const auto* value=graph->mIngress.peekConsumer();
    if (!value) return false;
    LLTransportDecodedView decoded;
    if (!graph->mArena.view(value->arenaRecord,decoded)) { failInbound(); return false; }
    out=*value; view=decoded; return true;
}
bool LLTransportOwnerFacade::releaseInbound() noexcept
{
    const auto graph=mGraph;
    if (!graph || !graph->mActiveOnMain) return false;
    const auto* value=graph->mIngress.peekConsumer();
    if (!value || !graph->mArena.release(value->arenaRecord) || !graph->mIngress.releaseConsumer())
    { failInbound(); return false; }
    return true;
}
bool LLTransportOwnerFacade::inboundEmptyAfterJoinOnMain() const noexcept
{
    const auto graph=mGraph;
    // A running producer's momentarily empty queue cannot authorize teardown.
    // Check both payload and metadata after join, including held reservations.
    return graph && graph->mActiveOnMain && graph->mJoinedOnMain &&
        graph->mIngress.emptyAfterJoinOnMain() && graph->mArena.emptyAfterJoinOnMain();
}
bool LLTransportOwnerFacade::schemaMatches(uint32_t number,uint64_t generation) const noexcept
{
    const auto graph=mGraph;
    if (!graph || !graph->mActiveOnMain || !generation) return false;
    const auto* entries=graph->mConfig.schemas;
    std::size_t lo=0,hi=graph->mConfig.schemaCount;
    while (lo<hi) { const auto mid=lo+(hi-lo)/2; if (entries[mid].messageNumber<number) lo=mid+1; else hi=mid; }
    return lo<graph->mConfig.schemaCount && entries[lo].messageNumber==number &&
        entries[lo].schema && entries[lo].schema->generation()==generation;
}
void LLTransportOwnerFacade::failInbound() noexcept { failCompletion(); }
