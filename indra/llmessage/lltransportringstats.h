/** Coherent owner-to-main packet-ring counters. SPDX-License-Identifier: LGPL-2.1-only */
#ifndef LL_LLTRANSPORTRINGSTATS_H
#define LL_LLTRANSPORTRINGSTATS_H
#include <atomic>
#include <cstdint>
#include <limits>

struct LLTransportRingStats
{
    // Nonzero graph identity from immutable owner config. Startup must assign
    // a fresh monotonic generation to every replacement owner, even if it
    // reuses the same LLMessageSystem address.
    uint64_t loginGeneration=0;
    // Cumulative bytes harvested since handoff, including previously unreset
    // legacy interval bytes. Main computes its own deltas; reads never reset.
    uint64_t actualIn=0, actualOut=0;
    uint64_t sessionBytesIn=0, sessionBytesOut=0, sessionPacketsIn=0, sessionPacketsOut=0;
    // Legacy packet-ring buffered values, sampled only by its owner. They
    // exclude already-published application ingress and unread kernel data.
    uint32_t bufferedPackets=0, bufferedBytes=0;
    // Independently sampled owner counters; monotonic and saturating within
    // loginGeneration, but not part of the coherent packet-counter snapshot.
    uint64_t reliableControlBackpressure=0, duplicateControlAckBackpressure=0;
};

// One owner writer and main reader(s). Every shared field is atomic; the
// sequence supplies coherent values and bounded retries without a main mutex.
// Unavailable/failure never fabricates zeros or changes the caller's output.
class LLTransportRingPublication
{
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
public:
    bool publishOnOwner(uint64_t loginGeneration, uint64_t inDelta, uint64_t outDelta, uint64_t sessionIn,
                        uint64_t sessionOut, uint64_t packetsIn, uint64_t packetsOut,
                        uint32_t bufferedPackets, uint32_t bufferedBytes) noexcept
    {
        const auto maximum=(std::numeric_limits<uint64_t>::max)();
        if (!loginGeneration || (mNext && loginGeneration!=mLastGeneration) ||
            mFailed.load() || mNext>maximum-2 || inDelta>maximum-mTotalIn || outDelta>maximum-mTotalOut ||
            (mNext && (sessionIn<mLastSessionIn || sessionOut<mLastSessionOut ||
                       packetsIn<mLastPacketsIn || packetsOut<mLastPacketsOut)))
        { mFailed.store(true); return false; }
        mTotalIn+=inDelta; mTotalOut+=outDelta;
        mLastGeneration=loginGeneration;
        mLastSessionIn=sessionIn; mLastSessionOut=sessionOut; mLastPacketsIn=packetsIn; mLastPacketsOut=packetsOut;
        mSequence.store(mNext+1);
        mGeneration.store(loginGeneration);
        mActualIn.store(mTotalIn); mActualOut.store(mTotalOut);
        mSessionIn.store(sessionIn); mSessionOut.store(sessionOut);
        mPacketsIn.store(packetsIn); mPacketsOut.store(packetsOut);
        mBufferedPackets.store(bufferedPackets); mBufferedBytes.store(bufferedBytes);
        mNext+=2; mSequence.store(mNext); mReady.store(true);
        return true;
    }
    bool readOnMain(LLTransportRingStats& out) const noexcept
    {
        if (!mReady.load() || mFailed.load()) return false;
        for (unsigned attempt=0; attempt<3; ++attempt)
        {
            const auto before=mSequence.load();
            if (before&1u) continue;
            LLTransportRingStats value;
            value.loginGeneration=mGeneration.load();
            value.actualIn=mActualIn.load(); value.actualOut=mActualOut.load();
            value.sessionBytesIn=mSessionIn.load(); value.sessionBytesOut=mSessionOut.load();
            value.sessionPacketsIn=mPacketsIn.load(); value.sessionPacketsOut=mPacketsOut.load();
            value.bufferedPackets=static_cast<uint32_t>(mBufferedPackets.load());
            value.bufferedBytes=static_cast<uint32_t>(mBufferedBytes.load());
            if (before==mSequence.load() && mReady.load() && !mFailed.load()) { out=value; return true; }
        }
        return false;
    }
    void failOnOwner() noexcept { mFailed.store(true); }
private:
    std::atomic<uint64_t> mSequence{0},mGeneration{0},mActualIn{0},mActualOut{0},mSessionIn{0},mSessionOut{0},mPacketsIn{0},mPacketsOut{0};
    std::atomic<uint64_t> mBufferedPackets{0},mBufferedBytes{0};
    std::atomic<bool> mReady{false},mFailed{false};
    uint64_t mNext=0,mLastGeneration=0,mTotalIn=0,mTotalOut=0; // owner only
    uint64_t mLastSessionIn=0,mLastSessionOut=0,mLastPacketsIn=0,mLastPacketsOut=0;
};
#endif
