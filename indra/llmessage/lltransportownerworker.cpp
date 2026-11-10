/** WIP sole-socket worker; not wired into viewer startup. SPDX-License-Identifier: LGPL-2.1-only */
#include "linden_common.h"
#include "lltransportownerworker.h"
#include <algorithm>
#include <limits>
#include <utility>

LLTransportOwnerWorker::LLTransportOwnerWorker(LLTransportOwnerLoop& loop,
    LLTransportOwnerLoop::Directory& directory, LLPacketRing& ring, LLTransportRingPublication& ringStats,
    const LLTransportOwnerConfig& config, std::shared_ptr<LLTransportOwnerWait> wait) noexcept
    : mLoop(loop), mDirectory(directory), mRing(ring), mRingStats(ringStats), mConfig(config),
      mWait(std::move(wait)), mWatchdogRandom(config.watchdogSeed) {}

LLTransportOwnerWorker::~LLTransportOwnerWorker() { requestStopOnMain(); joinOnMain(); }

bool LLTransportOwnerWorker::startOnMain() noexcept
{
    if (mThread.joinable() || mStartupDone || !mWait || mConfig.socket<0 ||
        !mConfig.loginGeneration || !mConfig.schemas || !mConfig.schemaCount ||
        !mLoop.initializeAckCollectTime(mConfig.initialAckCollectUsec)) return false;
    try
    {
        mThread=std::thread(&LLTransportOwnerWorker::run,this);
        std::unique_lock<std::mutex> lock(mStartupMutex);
        mStartupCondition.wait(lock,[this] { return mStartupDone; });
        const bool bound=mBound;
        lock.unlock();
        if (!bound) joinOnMain();
        return bound;
    }
    catch (...) { mFailed.store(true); requestStopOnMain(); joinOnMain(); return false; }
}

void LLTransportOwnerWorker::signalOnMain() noexcept { if (mWait) mWait->signal(); }
void LLTransportOwnerWorker::requestStopOnMain() noexcept { mStop.store(true); signalOnMain(); }
void LLTransportOwnerWorker::joinOnMain() noexcept { if (mThread.joinable()) mThread.join(); }

bool LLTransportOwnerWorker::startupRollbackStateAfterJoinOnMain(bool& harvested, uint64_t& in, uint64_t& out) const noexcept
{
    if (mThread.joinable() || mOwnerIoStarted.load()) return false;
    harvested=mStartupHarvested;
    in=mStartupHarvestIn; out=mStartupHarvestOut;
    return true;
}

bool LLTransportOwnerWorker::harvestRingOnOwner(bool startup) noexcept
{
    const auto bufferedPackets=mRing.getNumBufferedPackets();
    const auto bufferedBytes=mRing.getNumBufferedBytes();
    if (bufferedPackets<0 || bufferedBytes<0) { mRingStats.failOnOwner(); return false; }
    U64 in=0,out=0;
    if (!mRing.takeActualBytesOnOwner(in,out)) { mRingStats.failOnOwner(); return false; }
    if (startup) { mStartupHarvested=true; mStartupHarvestIn=in; mStartupHarvestOut=out; }
    if (!mRingStats.publishOnOwner(mConfig.loginGeneration,in,out,mRing.getSessionBytesIn(),mRing.getSessionBytesOut(),
                                  mRing.getSessionPacketsIn(),mRing.getSessionPacketsOut(),
                                  static_cast<uint32_t>(bufferedPackets),static_cast<uint32_t>(bufferedBytes)))
    { mRingStats.failOnOwner(); return false; }
    return true;
}

void LLTransportOwnerWorker::settleForStop() noexcept
{
    mLoop.closeAdmissionForStop();
    // Once state shutdown begins no directory lookup can be authoritative.
    mDirectory.failClosedOnOwner();
    for (;;)
    {
        const auto step=mLoop.cancelOneUnreliableForStop();
        if (step==LLTransportOwnerLoop::Step::Idle) break;
        // Even an invariant failure in this lane must not strand the
        // receive reservation or prevent other accepted sends settling.
        if (step!=LLTransportOwnerLoop::Step::Progress) { mFailed.store(true); break; }
    }
    mLoop.cancelInFlightForStop();
    for (;;)
    {
        const auto step=mLoop.settleQueuedForStop();
        if (step==LLTransportOwnerLoop::Step::Idle) break;
        if (step!=LLTransportOwnerLoop::Step::Progress) { mFailed.store(true); break; }
    }
    for (;;)
    {
        const auto step=mLoop.publishReliableCompletion();
        if (step==LLTransportOwnerLoop::Step::Idle) break;
        // Capacity was reserved at admission; do not wait for main to drain.
        if (step!=LLTransportOwnerLoop::Step::Progress) { mFailed.store(true); return; }
    }
}

void LLTransportOwnerWorker::run() noexcept
{
    const bool bound=mWait->bindSocketOnOwner(mConfig.socket);
    // The factory must supply a freshly initialized empty state/directory. A
    // missing publication is authoritative only after owner activation.
    const bool ready=bound && mDirectory.activateOnOwner() && harvestRingOnOwner(true);
    if (bound && !ready) mWait->detachSocketOnOwner();
    {
        std::lock_guard<std::mutex> lock(mStartupMutex);
        mBound=ready; mStartupDone=true;
    }
    mStartupCondition.notify_one();
    if (!ready) { mFailed.store(true); return; }
    // This worker remains disabled until backend admission, reliable controls,
    // off-circuit handshake and ordered stop/timeout settlement are complete.
    while (!mStop.load())
    {
        const auto now=llTransportSteadyNow();
        LLTransportCircuitState::WatchdogConfig watchdog{mConfig.heartbeatUsec,mConfig.timeoutUsec,
            std::generate_canonical<float,24>(mWatchdogRandom),
            std::generate_canonical<float,24>(mWatchdogRandom)};
        // Conservative boundary: a failed start after this point cannot
        // restore legacy ownership even if this particular turn did no I/O.
        mOwnerIoStarted.store(true);
        const auto step=mLoop.driveOnce(mDirectory,mRing,mConfig.socket,mConfig.proxy,now,
            mConfig.loginGeneration,mConfig.schemas,mConfig.schemaCount,watchdog,false);
        if (!harvestRingOnOwner()) { mFailed.store(true); break; }
        if (step==LLTransportOwnerLoop::Step::Poisoned) { mFailed.store(true); break; }
        if (step==LLTransportOwnerLoop::Step::StopPending) { mStop.store(true); break; }
        if (step==LLTransportOwnerLoop::Step::Progress || mLoop.needsReceiveTurn(now)) continue;
        // Maintenance must run independently of rendering. A delayed read is
        // retried from its latch at the deadline even without a new FD_READ.
        constexpr uint64_t maintenanceInterval=5000;
        const auto maximum=(std::numeric_limits<uint64_t>::max)();
        uint64_t deadline=now.value>maximum-maintenanceInterval ? maximum : now.value+maintenanceInterval;
        const auto readRetry=mLoop.receiveRetryAt();
        if (readRetry>now.value) deadline=std::min(deadline,readRetry);
        const auto result=mWait->waitUntil(mConfig.socket,LLTransportSteadyUsec(deadline),
            mLoop.wantsWritableWake(),mLoop.wantsReadableWake(now));
        if (result.error) { mFailed.store(true); break; }
        if (result.socketErrorPending && !mLoop.noteSocketError(static_cast<int>(result.nativeError),llTransportSteadyNow()))
        { mFailed.store(true); break; }
        if (result.readable) mLoop.notifySocketReadable();
    }
    settleForStop();
    if (!harvestRingOnOwner()) mFailed.store(true);
    mWait->detachSocketOnOwner();
    // Publish exit/failure before waking the main-side owner of this graph.
    if (mConfig.mainWake) mConfig.mainWake->signal();
}
