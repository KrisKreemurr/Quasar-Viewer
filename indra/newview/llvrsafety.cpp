/**
 * @file llvrsafety.cpp
 * @brief VR milestone M3 (#1076): the frame-rate safety governor. See llvrsafety.h.
 */
#include "llviewerprecompiledheaders.h"

#include "llvrsafety.h"

#include "llagent.h"
#include "llformat.h"
#include "llstartup.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llvkautotune.h"
#include "vkgfx/llvulkancontext.h"
#include "vkgfx/llxrsession.h"

#include <chrono>

namespace
{
using Clock = std::chrono::steady_clock;

constexpr U32 kMaxLevel = 5;

// Judging (half-second windows of xrWaitFrame pacing). "Missed" = display slots missed against
// the headset's NATIVE period (#1137: the fastest period the runtime predicted this session; a
// throttled period -- SteamVR halving to 60 / 45 Hz, or a third -- is not the target, so a
// frame at half rate misses one native slot). Our own frame cost (LLXrSession::frameWorkSec,
// the GPU frame time where known) tells a frame that is too slow from a runtime that holds us
// back although the frame fits: the latter holds the level, it never steps down.
constexpr F32 kSevere = 0.5f;       // a window below half the headset's rate: act at once, two levels
constexpr F32 kBad = 0.10f;         // a window missing more than this is "bad" ...
constexpr U32 kBadOf3 = 2;          // ... and 2 bad windows of the last 3 step down one level
constexpr F32 kBad2s = 0.08f;       // ... or the last 2 s missing more than this
constexpr F32 kClean = 0.03f;       // a window at or under this is clean (one stray miss is not a failure)
constexpr F64 kSettleSecs = 3.0;    // after any level change only a severe window counts (the change's own
                                    // churn -- LOD, streaming, render-scale targets -- costs a few frames)
constexpr F64 kStartGraceSecs = 5.0;   // entering the world / arriving: nothing counts (the scene streams in)
constexpr F32 kThrottled = 1.4f;    // the runtime's period above this x native: it throttles us
constexpr F32 kFits = 0.9f;         // our frame cost under this x native period: it fits the native rate
constexpr F32 kHeadroom = 0.8f;     // ... under this: room to give a level back
constexpr F32 kHeadroomThrottled = 0.7f;   // ... and while throttled (no clean native pacing to judge by)
constexpr U32 kNativeConfirm = 8;   // a shorter period counts as native after this many frames in a row
// Restoring (one level at a time, after this long of clean windows):
constexpr F64 kDwellBase = 10.0;
constexpr F64 kDwellRenderScale = 30.0;   // a restore that raises the render scale (visible, re-allocates targets)
constexpr F64 kDwellMax = 600.0;
constexpr F64 kUndoneSecs = 30.0;         // a restore stepped back down within this was premature:
                                          // that level's dwell doubles
constexpr F64 kHeldSecs = 180.0;          // a restore that held this long: its level's dwell back to the base

// What each level caps: draw distance first (it costs the most and loses least in a headset),
// then mirrors / probe counts, far and projector shadows, probe resolution, LOD -- the render
// scale changes only twice (85 % at 3, 70 % at the top level 5) so the frequent 3 <-> 4 moves
// never toggle it. Cumulative.
LLVKPerfCaps capsFor(U32 level)
{
    LLVKPerfCaps c;
    if (level >= 1)
    {
        c.drawDistScale = 0.75f;
        c.mirrorsOff = true;
        c.probeCountShift = 1;
    }
    if (level >= 2)
    {
        c.drawDistScale = 0.5f;
        c.farShadowsOff = true;
        c.projShadowsOff = true;
        c.probeResShift = 1;
    }
    if (level >= 3)
    {
        c.drawDistScale = 0.35f;
        c.renderScaleMax = 0.85f;
        c.meshLodScale = 0.75f;
        c.avatarLodScale = 0.75f;
    }
    if (level >= 4)
    {
        c.drawDistScale = 0.25f;
        c.meshLodScale = 0.5f;
        c.avatarLodScale = 0.5f;
        c.probeCountShift = 2;
    }
    if (level >= 5)
    {
        c.renderScaleMax = 0.7f;
    }
    return c;
}

const char* levelText(U32 level)
{
    switch (level)
    {
    case 0: return "none";
    case 1: return "draw distance 75 %, mirrors off, half the reflection probes";
    case 2: return "draw distance 50 %, far + projector shadows off, probe resolution halved";
    case 3: return "draw distance 35 %, render scale 85 %, mesh / avatar detail 75 %";
    case 4: return "draw distance 25 %, render scale 85 %, mesh / avatar detail 50 %, a quarter of the probes";
    default: return "draw distance 25 %, render scale 70 %, mesh / avatar detail 50 %, a quarter of the probes";
    }
}

struct SafetyState
{
    U32 level = 0;
    // The current half-second window.
    Clock::time_point winStart{};
    U32 frames = 0, missed = 0;
    // The last 4 complete windows (2 s), newest first.
    U32 histFrames[4] = {}, histMissed[4] = {};
    U32 histCount = 0;
    Clock::time_point settleUntil{};   // after a level change: only severe windows count
    Clock::time_point cleanSince{};    // clean windows since (restore clock)
    // Per level L: seconds of clean pacing before L is given back (L -> L - 1), and when it
    // last was.
    F64 dwell[kMaxLevel + 1] = {};
    Clock::time_point restoredAt[kMaxLevel + 1] = {};
    bool restoreOpen[kMaxLevel + 1] = {};   // L was restored and has not yet held kHeldSecs / been undone
    bool measuring = false;
    bool startGrace = false;           // settleUntil is the start grace: even severe windows wait
    // #1137: the headset's native display period (the fastest the runtime predicted this
    // session, confirmed over kNativeConfirm frames) and a shorter candidate being confirmed.
    F64 nativePeriod = 0.0;
    F64 candPeriod = 0.0;
    U32 candCount = 0;
    // The current window's runtime-throttled frames and our frame cost (CPU span sum / max).
    U32 throttledFrames = 0;
    F64 workSum = 0.0, workMax = 0.0;
    U32 workFrames = 0;
    bool heldThrottled = false;        // the last window was "throttled, but our frame fits" (logged on the edge)
    // 10 s summary (tag VR): what the governor saw.
    Clock::time_point sumStart{};
    U32 sumFrames = 0, sumMissed = 0, sumThrottled = 0;
    F64 sumWork = 0.0, sumWorkMax = 0.0, sumPeriod = 0.0;
};
SafetyState sS;

F64 secs(Clock::duration d)
{
    return std::chrono::duration<F64>(d).count();
}

F64 baseDwell(U32 level)
{
    // Restoring L -> L - 1 raises the render scale for L = 5 (70 -> 85 %) and L = 3 (85 -> 100 %).
    return (level == 5 || level == 3) ? kDwellRenderScale : kDwellBase;
}

// The VR baseline (2026-10-27, owner): while a session runs the effective draw distance is at
// most kVrMaxDrawDist, whatever the governor does (or with it off). A cap like the governor's:
// the setting itself is never changed.
constexpr F32 kVrMaxDrawDist = 256.f;

F32 baselineDrawScale()
{
    static LLCachedControl<F32> s_far(gSavedSettings, "RenderFarClip", 128.f);
    const F32 dd = (F32)s_far;
    return dd > kVrMaxDrawDist ? kVrMaxDrawDist / dd : 1.f;
}

F32 sAppliedBaseline = -1.f;   // the baseline scale last applied (-1: none, no session)

void applyCaps(bool session)
{
    LLVKPerfCaps c = capsFor(sS.level);
    sAppliedBaseline = -1.f;
    if (session)
    {
        LLVKPerfCaps base;
        base.drawDistScale = baselineDrawScale();
        c = LLVKPerfCaps::merged(c, base);
        sAppliedBaseline = base.drawDistScale;
    }
    LLVulkanContext& vk = LLVulkanContext::instance();
    if (vk.isInitialized()) vk.setVrSafetyPerfCaps(c);
}

void setLevel(U32 level, const std::string& why, bool session = true)
{
    if (level == sS.level) return;
    LL_INFOS("VR") << "safety governor: level " << sS.level << " -> " << level << " (" << levelText(level) << "): " << why
                   << LL_ENDL;
    sS.level = level;
    applyCaps(session);
}

void resetWindows(Clock::time_point now)
{
    sS.winStart = now;
    sS.frames = sS.missed = 0;
    sS.throttledFrames = 0;
    sS.workSum = sS.workMax = 0.0;
    sS.workFrames = 0;
    sS.histCount = 0;
    sS.cleanSince = now;
    sS.heldThrottled = false;
}

// #1137: the native period -- the fastest the runtime predicts, once it held kNativeConfirm
// frames in a row (one odd short prediction is not a refresh rate). Logged when it changes
// (the user switched the headset's rate: SteamVR then reports the new one).
void logNative(const char* how)
{
    LL_INFOS("VR") << llformat("safety governor: the headset's native rate is %.1f Hz (%.2f ms; %s)", 1.0 / sS.nativePeriod,
                               sS.nativePeriod * 1000.0, how)
                   << LL_ENDL;
}

void updateNative(F64 period)
{
    if (!(period > 0.0)) return;
    if (sS.nativePeriod <= 0.0)
    {
        sS.nativePeriod = period;   // the first prediction (the session starts at the native rate)
        logNative("the runtime's first display period");
        return;
    }
    if (period < sS.nativePeriod * 0.97)
    {
        if (sS.candPeriod > 0.0 && std::fabs(period - sS.candPeriod) < sS.candPeriod * 0.02) ++sS.candCount;
        else
        {
            sS.candPeriod = period;
            sS.candCount = 1;
        }
        if (sS.candCount < kNativeConfirm) return;
        sS.nativePeriod = sS.candPeriod;
        logNative("a shorter display period the runtime held");
    }
    sS.candPeriod = 0.0;
    sS.candCount = 0;
}

// Our frame cost (seconds): the frame's CPU span and the GPU frame time (where known), the larger.
F64 frameCost(F64 cpuSec, bool& gpuKnown)
{
    const F32 gpuMs = LLVKAutoTune::instance().gpuMs();
    gpuKnown = gpuMs > 0.f;
    return llmax(cpuSec, gpuKnown ? (F64)gpuMs * 0.001 : 0.0);
}

void initDwell()
{
    for (U32 l = 0; l <= kMaxLevel; ++l)
    {
        if (sS.dwell[l] <= 0.0) sS.dwell[l] = baseDwell(l);
    }
}
} // namespace

void LLVRSafety::reset()
{
    sS.measuring = false;
    sS.nativePeriod = sS.candPeriod = 0.0;   // (the next session may run at another rate)
    sS.candCount = 0;
    sS.sumStart = Clock::time_point{};
    sS.sumFrames = sS.sumMissed = sS.sumThrottled = 0;
    sS.sumWork = sS.sumWorkMax = sS.sumPeriod = 0.0;
    setLevel(0, "the VR session stopped", false);
    if (sAppliedBaseline >= 0.f) applyCaps(false);   // (level already 0: still drop the baseline)
}

void LLVRSafety::tick(bool inWorld)
{
    static LLCachedControl<bool> s_on(gSavedSettings, "SlipstreamVRSafetyGovernor", true);
    const Clock::time_point now = Clock::now();
    // The baseline follows the draw-distance setting live (and is first applied here).
    if (baselineDrawScale() != sAppliedBaseline)
    {
        applyCaps(true);
        LL_INFOS("VR") << "VR baseline: draw distance at most " << kVrMaxDrawDist << " m (x" << sAppliedBaseline
                       << " of the setting)" << LL_ENDL;
    }
    if (!s_on)
    {
        sS.measuring = false;
        setLevel(0, "turned off (Preferences > Safety)");
        return;
    }
    initDwell();
    // Only judge real world frames: not while loading, teleporting or on the progress screen
    // (a load hitch is not a frame rate). The levels hold meanwhile.
    double period = 0.0;
    U32 missed = 0;
    const bool judge = inWorld && LLStartUp::getStartupState() == STATE_STARTED &&
                       gAgent.getTeleportState() == LLAgent::TELEPORT_NONE && gViewerWindow &&
                       !gViewerWindow->getShowProgress() && LLXrSession::instance().frameTiming(period, missed);
    if (!judge)
    {
        sS.measuring = false;
        return;
    }
    if (!sS.measuring)
    {
        // Entering the world / back from a teleport: the scene streams in for a while (#1137:
        // the governor jumped to level 5 at session start); nothing counts during the grace.
        sS.measuring = true;
        resetWindows(now);
        sS.settleUntil = now + std::chrono::milliseconds((S64)(kStartGraceSecs * 1000.0));
        sS.startGrace = true;
    }
    // #1137: judged against the native period. The slots this frame took (the runtime's
    // period x (missed + 1)) in native slots; one long stall counts as a few, not a collapse.
    updateNative(period);
    const F64 native = sS.nativePeriod > 0.0 ? sS.nativePeriod : period;
    const F64 interval = period * (F64)(missed + 1u);
    const U32 nativeMissed = (U32)llmax(0.0, std::floor(interval / native + 0.5) - 1.0);
    ++sS.frames;
    sS.missed += llmin(nativeMissed, 3u);
    if (period > native * kThrottled) ++sS.throttledFrames;
    const F64 work = LLXrSession::instance().frameWorkSec();
    if (work > 0.0 && work < 1.0)
    {
        sS.workSum += work;
        sS.workMax = llmax(sS.workMax, work);
        ++sS.workFrames;
    }
    // The 10 s summary.
    if (sS.sumStart == Clock::time_point{}) sS.sumStart = now;
    ++sS.sumFrames;
    sS.sumMissed += llmin(nativeMissed, 3u);
    sS.sumThrottled += period > native * kThrottled ? 1u : 0u;
    sS.sumPeriod += period;
    if (work > 0.0 && work < 1.0)
    {
        sS.sumWork += work;
        sS.sumWorkMax = llmax(sS.sumWorkMax, work);
    }
    if (secs(now - sS.sumStart) >= 10.0)
    {
        const F32 gpuMs = LLVKAutoTune::instance().gpuMs();
        LL_INFOS("VR") << llformat("safety governor, last 10 s: level %u; native %.0f Hz, the runtime at %.1f Hz on average "
                                   "(throttled %.0f %% of frames); %.1f %% of native slots missed; our frame %.2f ms CPU span "
                                   "(max %.2f), GPU %s of a %.2f ms budget",
                                   sS.level, 1.0 / native, (F64)sS.sumFrames / llmax(1e-6, sS.sumPeriod),
                                   100.0 * sS.sumThrottled / llmax(1u, sS.sumFrames),
                                   100.0 * sS.sumMissed / llmax(1u, sS.sumFrames + sS.sumMissed),
                                   1000.0 * sS.sumWork / llmax(1u, sS.sumFrames), 1000.0 * sS.sumWorkMax,
                                   gpuMs > 0.f ? llformat("%.2f ms", gpuMs).c_str() : "unknown", 1000.0 * native)
                       << LL_ENDL;
        sS.sumStart = now;
        sS.sumFrames = sS.sumMissed = sS.sumThrottled = 0;
        sS.sumWork = sS.sumWorkMax = sS.sumPeriod = 0.0;
    }
    if (now - sS.winStart < std::chrono::milliseconds(500)) return;

    // The window's verdict on our own cost: does the frame fit the native rate?
    bool gpuKnown = false;
    const F64 cpuAvg = sS.workFrames ? sS.workSum / (F64)sS.workFrames : 0.0;
    const F64 cost = frameCost(cpuAvg, gpuKnown);
    const bool costKnown = sS.workFrames > 0;
    const bool throttledWin = sS.throttledFrames * 2u > sS.frames;
    // "Fits" needs the GPU time: a GPU-bound frame that the throttle lets finish inside the
    // throttled period never waits on the GPU in-frame, so its CPU span alone looks cheap (the
    // #1137 case: ~9 ms GPU at 120 Hz, halved to 60). Unknown GPU: judged by native slots only.
    const bool fits = costKnown && gpuKnown && cost < native * kFits;

    // A half-second window is complete: into the history (newest first).
    for (U32 i = 3; i > 0; --i)
    {
        sS.histFrames[i] = sS.histFrames[i - 1];
        sS.histMissed[i] = sS.histMissed[i - 1];
    }
    sS.histFrames[0] = sS.frames;
    sS.histMissed[0] = sS.missed;
    sS.histCount = llmin(sS.histCount + 1, 4u);
    sS.winStart = now;
    sS.frames = sS.missed = 0;
    sS.throttledFrames = 0;
    sS.workSum = sS.workMax = 0.0;
    sS.workFrames = 0;
    auto share = [](U32 i) {
        return (F32)sS.histMissed[i] / (F32)llmax(1u, sS.histFrames[i] + sS.histMissed[i]);
    };
    const F32 half = share(0);
    U32 bad3 = 0;
    for (U32 i = 0; i < llmin(sS.histCount, 3u); ++i) bad3 += share(i) > kBad ? 1u : 0u;
    U32 f2 = 0, m2 = 0;
    for (U32 i = 0; i < sS.histCount; ++i)
    {
        f2 += sS.histFrames[i];
        m2 += sS.histMissed[i];
    }
    const F32 two = (F32)m2 / (F32)llmax(1u, f2 + m2);
    const F32 hz = (F32)(1.0 / native);
    const bool settling = now < sS.settleUntil;
    if (sS.startGrace && !settling)
    {
        sS.startGrace = false;
        sS.histCount = 0;   // the windows recorded while the scene streamed in do not count
        sS.cleanSince = now;
        return;
    }
    if (sS.startGrace) return;

    // #1137: the runtime throttles us (its period well above native) although our frame fits
    // the native rate: stepping down would not help (the throttle, not our cost, sets the rate;
    // SteamVR releases it on its own once frames fit). Hold the level; log the edge.
    if (throttledWin && fits)
    {
        if (!sS.heldThrottled)
        {
            LL_INFOS("VR") << llformat("safety governor: the runtime throttles the headset to %.1f Hz although our frame "
                                       "fits the native %.0f Hz (%.2f ms of %.2f ms, GPU %s): holding level %u",
                                       1.0 / period, hz, cost * 1000.0, native * 1000.0, gpuKnown ? "measured" : "unknown",
                                       sS.level)
                           << LL_ENDL;
        }
        sS.heldThrottled = true;
        // Give a level back only on clear headroom (no clean native pacing to judge by).
        if (sS.level > 0 && !settling && cost < native * kHeadroomThrottled && secs(now - sS.cleanSince) >= sS.dwell[sS.level])
        {
            const U32 l = sS.level;
            setLevel(l - 1, llformat("%.0f s throttled by the runtime with headroom (our frame %.2f ms of %.2f ms)",
                                     sS.dwell[l], cost * 1000.0, native * 1000.0));
            sS.restoredAt[l] = now;
            sS.restoreOpen[l] = true;
            sS.cleanSince = now;
            sS.settleUntil = now + std::chrono::milliseconds((S64)(kSettleSecs * 1000.0));
        }
        else if (cost >= native * kHeadroomThrottled)
        {
            sS.cleanSince = now;
        }
        return;
    }
    if (sS.heldThrottled)
    {
        LL_INFOS("VR") << "safety governor: no longer held by a runtime throttle (" << (throttledWin ? "our frame is over budget"
                                                                                                  : "back at the native rate")
                       << ")" << LL_ENDL;
        sS.heldThrottled = false;
    }

    U32 up = 0;
    if (half > kSevere) up = 2;   // below half the headset's rate: act hard, even while settling
    else if (!settling && sS.histCount >= 2 && (bad3 >= kBadOf3 || (sS.histCount >= 4 && two > kBad2s))) up = 1;
    if (up && sS.level < kMaxLevel)
    {
        const U32 to = llmin(kMaxLevel, sS.level + up);
        // A restore of a level we are stepping back into within kUndoneSecs was premature: that
        // level waits twice as long (up to kDwellMax) before it is given back again.
        for (U32 l = sS.level + 1; l <= to; ++l)
        {
            if (sS.restoreOpen[l] && secs(now - sS.restoredAt[l]) < kUndoneSecs)
            {
                sS.dwell[l] = llmin(sS.dwell[l] * 2.0, kDwellMax);
                LL_INFOS("VR") << "safety governor: giving level " << l << " back was premature ("
                               << secs(now - sS.restoredAt[l]) << " s): it now waits " << sS.dwell[l]
                               << " s of clean pacing" << LL_ENDL;
            }
            sS.restoreOpen[l] = false;
        }
        setLevel(to, llformat("%.0f %% of the headset's native %.0f Hz slots missed in 0.5 s (%.0f %% over 2 s, %u of the "
                              "last 3 windows over %.0f %%); runtime at %.1f Hz, our frame %s of %.2f ms",
                              half * 100.f, hz, two * 100.f, bad3, kBad * 100.f, 1.0 / period,
                              costKnown ? llformat("%.2f ms", cost * 1000.0).c_str() : "unknown", native * 1000.0));
        sS.settleUntil = now + std::chrono::milliseconds((S64)(kSettleSecs * 1000.0));
        sS.cleanSince = now;
        return;
    }
    // A restore that held: its level is back to quick restores next time.
    for (U32 l = 1; l <= kMaxLevel; ++l)
    {
        if (sS.restoreOpen[l] && secs(now - sS.restoredAt[l]) >= kHeldSecs)
        {
            sS.restoreOpen[l] = false;
            sS.dwell[l] = baseDwell(l);
        }
    }
    // Clean = the native rate held AND (#1137) real headroom in our own frame where we know it:
    // "clean at a throttled rate" restored quality the frame could not afford.
    if (half > kClean || (costKnown && cost >= native * kHeadroom))
    {
        sS.cleanSince = now;
        return;
    }
    if (settling) return;
    // Clean pacing: give one level back after that level's dwell of it.
    const U32 l = sS.level;
    if (l > 0 && secs(now - sS.cleanSince) >= sS.dwell[l])
    {
        setLevel(l - 1, llformat("%.0f s of clean pacing at the native %.0f Hz (our frame %s of %.2f ms)", sS.dwell[l], hz,
                                 costKnown ? llformat("%.2f ms", cost * 1000.0).c_str() : "unknown", native * 1000.0));
        sS.restoredAt[l] = now;
        sS.restoreOpen[l] = true;
        sS.cleanSince = now;
        sS.settleUntil = now + std::chrono::milliseconds((S64)(kSettleSecs * 1000.0));
    }
}
