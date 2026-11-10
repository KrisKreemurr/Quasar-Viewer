/** WIP sole-socket worker; not wired into viewer startup. SPDX-License-Identifier: LGPL-2.1-only */
#ifndef LL_LLTRANSPORTOWNERWORKER_H
#define LL_LLTRANSPORTOWNERWORKER_H
#include "lltransportownerloop.h"
#include "lltransportownerwait.h"
#include "lltransportringstats.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

// The containing backend owns the initialized graph and must outlive join and
// all main-side drains. It reserves every terminal credit BEFORE publishing a
// command, closes admission BEFORE requesting stop, and retains ACKed inbound
// storage after join. This class never calls a viewer handler or closes socket.
class LLTransportOwnerWorker
{
public:
    LLTransportOwnerWorker(LLTransportOwnerLoop& loop, LLTransportOwnerLoop::Directory& directory,
                           LLPacketRing& ring, LLTransportRingPublication& ringStats, const LLTransportOwnerConfig& config,
                           std::shared_ptr<LLTransportOwnerWait> wait) noexcept;
    ~LLTransportOwnerWorker();
    LLTransportOwnerWorker(const LLTransportOwnerWorker&)=delete;
    LLTransportOwnerWorker& operator=(const LLTransportOwnerWorker&)=delete;
    // Main only. Success means bindSocketOnOwner succeeded on the worker.
    // Graph/config validation and exclusive socket handoff are backend duties.
    bool startOnMain() noexcept;
    void signalOnMain() noexcept;
    void requestStopOnMain() noexcept;
    void joinOnMain() noexcept;
    bool failed() const noexcept { return mFailed.load(); }
    // After failed start and join, prove the worker never entered an I/O turn.
    // If harvested is true the factory must restore the exact interval bytes;
    // if false those bytes were never removed. Failure preserves all outputs.
    bool startupRollbackStateAfterJoinOnMain(bool& harvested, uint64_t& in, uint64_t& out) const noexcept;
private:
    void run() noexcept;
    void settleForStop() noexcept;
    bool harvestRingOnOwner(bool startup=false) noexcept;
    LLTransportOwnerLoop& mLoop;
    LLTransportOwnerLoop::Directory& mDirectory;
    LLPacketRing& mRing;
    LLTransportRingPublication& mRingStats;
    LLTransportOwnerConfig mConfig;
    std::shared_ptr<LLTransportOwnerWait> mWait;
    std::mt19937 mWatchdogRandom;
    std::atomic<bool> mStop{false}, mFailed{false};
    std::atomic<bool> mOwnerIoStarted{false};
    std::thread mThread;
    std::mutex mStartupMutex;
    std::condition_variable mStartupCondition;
    bool mStartupDone=false, mBound=false;
    bool mStartupHarvested=false;
    uint64_t mStartupHarvestIn=0, mStartupHarvestOut=0;
};
#endif
