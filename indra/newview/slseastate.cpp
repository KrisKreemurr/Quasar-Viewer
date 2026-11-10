/**
 * @file slseastate.cpp
 * @brief The sea state (#1020): slseastate.h has the model and the interface.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * (LGPL v2.1, see slweather.h)
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "slseastate.h"
#include "slseamath.h"   // the CPU twin of the GPU surface, the floor reduction (#1020)

#include "slwaterevents.h"   // BREAK: the breakers for the sounds (#1019)
#include "slweather.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llenvironment.h"
#include "llframetimer.h"
#include "llsurface.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llworld.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
constexpr F32 kG = 9.81f;
constexpr F32 kTwoPi = 6.2831853f;
// The fixed wavelength ladder (m) and each component's direction offset from the wind (deg):
// the long swell close to the wind, the short waves spread wider (a cos^2s-like fan).
constexpr F32 kLambda[SLSeaState::kWaves] = {80.f, 52.f, 34.f, 22.f, 14.5f, 9.5f, 6.2f, 4.f};
constexpr F32 kAngle[SLSeaState::kWaves] = {0.f, 14.f, -20.f, 27.f, -34.f, 41.f, -50.f, 58.f};
constexpr F32 kPhase0[SLSeaState::kWaves] = {0.f, 2.4f, 4.8f, 1.1f, 3.6f, 5.9f, 0.7f, 3.1f};
// Easing (s): seas build slower than the wind, turn slower still; storms ease in.
constexpr F32 kTauWind = 25.f, kTauDir = 40.f, kTauStorm = 20.f, kTauGust = 0.6f;
constexpr F32 kDirDeadCos = 0.99999f;   // ~0.26 degrees: the direction's settle band
// The calm-day breeze with no weather source: LL's normal-map scroll speed -> wind (m/s).
constexpr F32 kCalmBase = 2.5f, kCalmPerScroll = 1.2f, kCalmMin = 1.5f, kCalmMax = 6.f;
constexpr F32 kCalmRef = 3.86f;   // LL's default wave1Dir's breeze: the detail normals at 1
// The field: rebuild slices, triggers.
constexpr U32 kFieldRowsPerFrame = 16;
constexpr F32 kFieldRecentre = 64.f;      // m from the window's centre
constexpr F32 kFieldMaxAge = 15.f;        // s
constexpr F32 kFieldUnknownRetry = 2.f;   // s: a field built over terrain not loaded yet
constexpr F32 kFloorChangeAge = 3.f;      // s: a changed static object re-runs the floor pass at most this often
constexpr F32 kFieldTurnCos = 0.978f;     // ~12 degrees of wind turn: the fetch is stale
constexpr U32 kFetchSteps = 48;           // x 2 texels = 384 m upwind
constexpr F32 kShoreCap = 1000.f;

F32 ease(F32 cur, F32 target, F32 dt, F32 tau)
{
    return cur + (target - cur) * (dt <= 0.f ? 0.f : 1.f - std::exp(-dt / tau));
}
F32 smooth01(F32 e0, F32 e1, F32 x)
{
    const F32 t = llclamp((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}
F32 fract1(F32 x) { return x - std::floor(x); }

// IEEE half (round to nearest, finite range clamped): the field's packing (GLSL unpackHalf2x16).
U16 toHalf(F32 f)
{
    f = llclamp(f, -65000.f, 65000.f);
    U32 x; std::memcpy(&x, &f, 4);
    const U32 sign = (x >> 16) & 0x8000u;
    const S32 e = (S32)((x >> 23) & 0xFF) - 127 + 15;
    U32 m = x & 0x7FFFFFu;
    if (e <= 0)
    {
        if (e < -10) return (U16)sign;
        m |= 0x800000u;
        const U32 sh = (U32)(14 - e);
        return (U16)(sign | ((m + (1u << (sh - 1))) >> sh));
    }
    const U32 h = sign | ((U32)e << 10) | (m >> 13);
    return (U16)(h + ((m >> 12) & 1u));
}
U32 packHalf2(F32 a, F32 b) { return (U32)toHalf(a) | ((U32)toHalf(b) << 16); }
} // namespace

SLSeaState& SLSeaState::instance()
{
    static SLSeaState s;
    return s;
}

void SLSeaState::update()
{
    const U64 frame = LLFrameTimer::getFrameCount();
    if (frame == mLastFrame) return;
    mLastFrame = frame;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const F32 dt = mSeeded ? llclamp((F32)(now - mLastTime), 0.f, 0.25f) : 0.f;
    mLastTime = now;

    static LLCachedControl<U32> s_mode(gSavedSettings, "RenderVKWaterWaves", 2);
    mState.mode = llmin((U32)s_mode, (U32)MODE_HIGH);

    const F32 phase0 = mState.swashPhase;
    stepState(dt);
    buildWaves(SLWeather::syncedSeconds());
    if (mState.mode != MODE_FLAT && gAgent.getRegion()) stepField();
    // The swash clock wrapped: a bore front reaches the waterline everywhere now
    // (clean_sea_state.glsl: psi = fract(phase + shore / L) is 0 at shore 0).
    if (mSeeded && mState.swashPhase < phase0) postBreak();
    mSeeded = true;
}

// BREAK (slwaterevents.h) at the shore point nearest the camera, from the sea field: its signed
// shore distance there and the field's gradient (the offshore direction). Waves on only (flat
// water draws no breakers, so none is heard in step), a shore within kBreakRange, some surf.
void SLSeaState::postBreak() const
{
    constexpr F32 kBreakRange = 150.f;   // m from the camera
    constexpr F32 kStep = 2.f;           // m: the gradient's half step
    LLViewerRegion* rg = gAgent.getRegion();
    if (mState.mode == MODE_FLAT || !mFieldValid || !rg || mState.surf < 0.05f) return;
    const LLVector3 cam = gAgentCamera.getCameraPositionAgent();
    const FieldTexel f = fieldAt(cam.mV[VX], cam.mV[VY]);
    if (std::fabs(f.shore) > kBreakRange) return;
    const F32 gx = fieldAt(cam.mV[VX] + kStep, cam.mV[VY]).shore - fieldAt(cam.mV[VX] - kStep, cam.mV[VY]).shore;
    const F32 gy = fieldAt(cam.mV[VX], cam.mV[VY] + kStep).shore - fieldAt(cam.mV[VX], cam.mV[VY] - kStep).shore;
    const F32 gl = std::sqrt(gx * gx + gy * gy);
    if (gl < 1e-3f) return;
    // (shore > 0 offshore: step against the gradient; < 0 inland: along it -- one formula)
    const LLVector3 p(cam.mV[VX] - gx / gl * f.shore, cam.mV[VY] - gy / gl * f.shore,
                      rg->getWaterHeight() + fieldAt(cam.mV[VX] - gx / gl * f.shore, cam.mV[VY] - gy / gl * f.shore).dh);
    SLWaterEvents::Event e;
    e.kind = SLWaterEvents::BREAK;
    // (#1020: the breakers' LOCAL strength there -- a lee shore or a narrow channel is heard as
    // the lapping it draws, not the open sea's surf)
    const F32 kb = boreStrengthAt(p.mV[VX] + gx / gl * 2.f, p.mV[VY] + gy / gl * 2.f);   // (2 m offshore of the line)
    if (kb < 0.05f) return;
    e.energy = llclamp(kb * (0.6f + 0.4f * mState.storm), 0.f, 1.f);
    e.posAgent = p;
    e.posGlobal = gAgent.getPosGlobalFromAgent(p);
    SLWaterEvents::post(e);
}

void SLSeaState::stepState(F32 dt)
{
    SLWeather& wx = SLWeather::instance();
    wx.update();   // (no-op when the renderer already stepped it this frame)
    const SLWeather::State& st = wx.state();
    const SLWeather::Target& tg = wx.target();

    // The calm-day sea: LL's water settings scroll their normal maps against wave1Dir; the
    // waves travel the other way, as fast as that scroll suggests.
    F32 calm = kCalmRef;
    if (LLSettingsWater::ptr_t water = LLEnvironment::instance().getCurrentWater())
    {
        const LLVector2 d1 = water->getWave1Dir();
        const F32 len = d1.length();
        calm = llclamp(kCalmBase + kCalmPerScroll * len, kCalmMin, kCalmMax);
        if (len > 1e-3f) { mCalmDir[0] = -d1.mV[0] / len; mCalmDir[1] = -d1.mV[1] / len; }
    }

    const bool weather = tg.active && SLWeather::masterEnabled();
    const F32 mx = st.windMean[0], my = st.windMean[1];
    const F32 mean = std::sqrt(mx * mx + my * my);
    // A weather source drives the sea from its mean wind (a calm source: a glassy sea); without
    // one, the region's calm-day breeze. The easing carries every switch.
    const F32 windT = weather ? mean : calm;
    F32 dirT[2] = {mCalmDir[0], mCalmDir[1]};
    if (weather && mean > 0.3f) { dirT[0] = mx / mean; dirT[1] = my / mean; }
    else if (weather) { dirT[0] = mState.windDir[0]; dirT[1] = mState.windDir[1]; }

    if (!mSeeded)
    {
        mState.windSpeed = windT;
        mState.windDir[0] = dirT[0]; mState.windDir[1] = dirT[1];
    }
    mState.windSpeed = ease(mState.windSpeed, windT, dt, kTauWind);
    // The direction only moves while it is more than kDirDeadCos off its target: settled, it stays
    // bit-identical frame to frame (the ease + renormalise otherwise dithers the last float bits,
    // and every wave vector with it).
    if (mState.windDir[0] * dirT[0] + mState.windDir[1] * dirT[1] < kDirDeadCos)
    {
        F32 dx = ease(mState.windDir[0], dirT[0], dt, kTauDir);
        F32 dy = ease(mState.windDir[1], dirT[1], dt, kTauDir);
        const F32 l = std::sqrt(dx * dx + dy * dy);
        if (l > 1e-4f) { dx /= l; dy /= l; } else { dx = dirT[0]; dy = dirT[1]; }
        mState.windDir[0] = dx; mState.windDir[1] = dy;
    }
    const F32 gustT = weather ? std::sqrt(st.wind[0] * st.wind[0] + st.wind[1] * st.wind[1]) : mState.windSpeed;
    mState.gust = mSeeded ? ease(mState.gust, gustT, dt, kTauGust) : gustT;

    const F32 U = mState.windSpeed;
    const F32 stormT = llclamp(llmax(weather ? tg.storm : 0.f, (U - 9.f) / 12.f) + 0.3f * st.rain, 0.f, 1.f);
    mState.storm = mSeeded ? ease(mState.storm, stormT, dt, kTauStorm) : stormT;
    mState.beaufort = llclamp(std::pow(U / 0.836f, 2.f / 3.f), 0.f, 12.f);

    // Pierson-Moskowitz fully developed sea, its height compressed into what reads at SL scale.
    const F32 hsPM = 0.21f * U * U / kG;
    mState.hs = kHsCap * std::tanh(hsPM / kHsCap);
    mState.peakLambda = llmax(0.833f * U * U, 1.f);
    mState.chop = 0.35f + 0.4f * mState.storm + 0.1f * smooth01(5.f, 15.f, U);
    mState.detail = llclamp(std::sqrt(llmax(mState.gust, 0.5f) / kCalmRef), 0.35f, 2.f);

    // Surf: lapping on a calm day, crashing in a storm; one breaker per (stretched) peak period.
    mState.surf = llclamp(0.1f + mState.hs / 1.6f, 0.f, 1.f) * (0.7f + 0.3f * mState.storm);
    mState.runup = llmin(0.12f + 0.45f * mState.hs, 1.5f);
    const F32 tp = 0.73f * U;   // 2 pi / omega_p
    mState.swashPeriod = llclamp(1.3f * tp, 5.f, 12.f);
    mState.swashLen = mState.swashPeriod * std::sqrt(kG * 1.5f);
    mState.swashPhase = fract1(mState.swashPhase + dt / mState.swashPeriod);
}

void SLSeaState::buildWaves(F64 syncNow)
{
    const bool on = mState.mode != MODE_FLAT;
    const F32 U = llmax(mState.windSpeed, 0.3f);
    const F32 wp = 0.877f * kG / U;
    // PM amplitudes on the ladder (dw ~ 0.21 w: the ladder's spacing), scaled to the visible Hs.
    F32 amp[kWaves];
    F32 m0 = 0.f;
    for (U32 i = 0; i < kWaves; ++i)
    {
        const F32 k = kTwoPi / kLambda[i];
        const F32 w = std::sqrt(kG * k);
        const F32 r = wp / w;
        const F32 S = 0.0081f * kG * kG * std::pow(w, -5.f) * std::exp(-1.25f * r * r * r * r);
        amp[i] = std::sqrt(2.f * S * 0.21f * w);
        m0 += 0.5f * amp[i] * amp[i];
    }
    const F32 hsSet = 4.f * std::sqrt(m0);
    const F32 scale = hsSet > 1e-5f ? mState.hs / hsSet : 0.f;

    // PHASE ANCHOR = the agent region's origin (agent xy 0, where the shader's k . xy is measured
    // from). A crossing moves it: each wave carries k . (O_new - O_old) with its CURRENT wave vector,
    // so the pattern is the same surface before and after (a teleport just lands somewhere).
    if (LLViewerRegion* rg = gAgent.getRegion())
    {
        const F64 ox = rg->getOriginGlobal().mdV[VX], oy = rg->getOriginGlobal().mdV[VY];
        if (mAnchorSet && (ox != mAnchor[0] || oy != mAnchor[1]))
            for (U32 i = 0; i < kWaves; ++i)
            {
                const Wave& wv = mState.waves[i];
                F64 c = mPhaseCarry[i] + (F64)wv.kvec[0] * (ox - mAnchor[0]) + (F64)wv.kvec[1] * (oy - mAnchor[1]);
                mPhaseCarry[i] = c - std::floor(c / (F64)kTwoPi) * (F64)kTwoPi;
            }
        mAnchor[0] = ox; mAnchor[1] = oy;
        mAnchorSet = true;
    }
    const F32 base = std::atan2(mState.windDir[1], mState.windDir[0]);
    F32 qsum = 0.f;
    for (U32 i = 0; i < kWaves; ++i)
    {
        Wave& wv = mState.waves[i];
        wv.k = kTwoPi / kLambda[i];
        wv.omega = std::sqrt(kG * wv.k);
        wv.amp = on ? llmin(amp[i] * scale, 0.12f / wv.k) : 0.f;
        const F32 a = base + kAngle[i] * DEG_TO_RAD;
        wv.kvec[0] = std::cos(a) * wv.k;
        wv.kvec[1] = std::sin(a) * wv.k;
        // theta = k . xy_agent + carry - w t + phase0, folded in double. (No k . O_global term: its
        // lever arm made the phase hypersensitive to the direction -- see the header.)
        const F64 th = mPhaseCarry[i] - (F64)wv.omega * syncNow + kPhase0[i];
        wv.phase = (F32)(th - std::floor(th / (F64)kTwoPi) * (F64)kTwoPi);
        qsum += wv.k * wv.amp;
    }
    // Q per wave so that sum(Q k A) = chop, each Q <= 1 (loop free).
    for (U32 i = 0; i < kWaves; ++i)
    {
        Wave& wv = mState.waves[i];
        const F32 ka = wv.k * wv.amp;
        wv.steep = ka > 1e-6f ? llmin(mState.chop / (ka * (F32)kWaves), 1.f) : 0.f;
    }
    (void)qsum;
}

// ---- the field ----

void SLSeaState::stepField()
{
    LLViewerRegion* agentRg = gAgent.getRegion();
    const LLVector3 cam = gAgentCamera.getCameraPositionAgent();
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const F64 ogx = agentRg->getOriginGlobal().mdV[VX], ogy = agentRg->getOriginGlobal().mdV[VY];
    const F32 extent = (F32)kFieldRes * kFieldTexel;

    if (mBuildStage == 0)
    {
        // The published field's centre in today's agent frame (a region crossing re-bases it).
        bool due = !mFieldValid || now - mFieldTime > kFieldMaxAge;
        if (!due)
        {
            const F32 ox = mFieldOrigin[0] + (F32)(mFieldRegionOrigin[0] - ogx);
            const F32 oy = mFieldOrigin[1] + (F32)(mFieldRegionOrigin[1] - ogy);
            const F32 dx = cam.mV[VX] - (ox + 0.5f * extent), dy = cam.mV[VY] - (oy + 0.5f * extent);
            due = std::fabs(dx) > kFieldRecentre || std::fabs(dy) > kFieldRecentre
               || mFieldWindDir[0] * mState.windDir[0] + mFieldWindDir[1] * mState.windDir[1] < kFieldTurnCos
               // (a static object changed under the water in the window: the floor pass again,
               // at most every kFloorChangeAge s)
               || (mFloorDirty && now - mFieldTime > kFloorChangeAge);
        }
        if (!due) return;
        mBuild.resize((size_t)kFieldRes * kFieldRes);
        mBuildOrigin[0] = std::floor((cam.mV[VX] - 0.5f * extent) / 16.f) * 16.f;
        mBuildOrigin[1] = std::floor((cam.mV[VY] - 0.5f * extent) / 16.f) * 16.f;
        mBuildRegionOrigin[0] = ogx; mBuildRegionOrigin[1] = ogy;
        mBuildWindDir[0] = mState.windDir[0]; mBuildWindDir[1] = mState.windDir[1];
        mFieldTime = now;
        mBuildStage = 1;
        mBuildRow = 0;
        mBuildUnknown = false;
        mBuildKnown.assign((size_t)kFieldRes * kFieldRes, 0);
        // THE SEA FLOOR: the renderer's top-down pass over this window, answered while the
        // heights are sampled (takeFloor).
        mFloorDirty = false;
        mFloorReq.serial = ++mFloorSerial;
        if (!mFloorReq.serial) mFloorReq.serial = ++mFloorSerial;   // (0 = no request)
        mFloorReq.origin[0] = mBuildOrigin[0]; mFloorReq.origin[1] = mBuildOrigin[1];
        mFloorReq.extent = extent;
        mFloorReq.top = agentRg->getWaterHeight() - kFloorSkip;
        mFloorReq.span = kFloorSpan;
        mFloorWanted = true;
        mFloorHave = false;
        mFloorAsked = now;
    }
    // A crossing mid-build: start over in the new frame.
    if (mBuildRegionOrigin[0] != ogx || mBuildRegionOrigin[1] != ogy) { mBuildStage = 0; mFloorWanted = false; return; }

    const S32 R = (S32)kFieldRes;
    if (mBuildStage == 1)
    {
        // HEIGHTS: the region under each texel centre (cached along the row), its water height.
        const F32 agentWater = agentRg->getWaterHeight();
        LLWorld* world = LLWorld::getInstance();
        const U32 end = llmin(mBuildRow + kFieldRowsPerFrame, kFieldRes);
        for (U32 j = mBuildRow; j < end; ++j)
        {
            LLViewerRegion* rg = nullptr;
            LLVector3 rgo;
            F32 rw = 0.f;
            for (U32 i = 0; i < kFieldRes; ++i)
            {
                const F32 x = mBuildOrigin[0] + ((F32)i + 0.5f) * kFieldTexel;
                const F32 y = mBuildOrigin[1] + ((F32)j + 0.5f) * kFieldTexel;
                if (!rg || x < rgo.mV[VX] || y < rgo.mV[VY] || x >= rgo.mV[VX] + rw || y >= rgo.mV[VY] + rw)
                {
                    rg = world->getRegionFromPosAgent(LLVector3(x, y, 0.f));
                    if (rg) { rgo = rg->getOriginAgent(); rw = rg->getWidth(); }
                }
                FieldTexel& t = mBuild[(size_t)j * kFieldRes + i];
                t = FieldTexel();
                // A region whose terrain has not arrived (login, teleport) would read as water at
                // 0 m: unknown -- left as the open sea, and the field rebuilt soon (mBuildUnknown).
                if (rg && !rg->getLand().hasZData()) { mBuildUnknown = true; continue; }
                if (rg)
                {
                    const F32 wh = rg->getWaterHeight();
                    t.depth = wh - rg->getLand().resolveHeightRegion(x - rgo.mV[VX], y - rgo.mV[VY]);
                    t.dh = wh - agentWater;
                    mBuildKnown[(size_t)j * kFieldRes + i] = 1;
                }
            }
        }
        mBuildRow = end;
        if (mBuildRow < kFieldRes) return;

        // THE SEA FLOOR: wait for the pass (at most kFloorWait s), then the higher of the terrain
        // and what it found -- only over regions with terrain (unknown ones stay open sea, void
        // water keeps its depth: there is no floor to find off-sim).
        if (mFloorWanted && !mFloorHave && now - mFloorAsked < (F64)kFloorWait) return;
        if (mFloorHave && mFloor.size() == mBuild.size())
        {
            U32 found = 0;
            for (size_t k = 0; k < mBuild.size(); ++k)
            {
                if (!mBuildKnown[k] || !(mFloor[k] > slsea::kFloorNone * 0.5f)) continue;
                FieldTexel& t = mBuild[k];
                const F32 wh = agentWater + t.dh;
                const F32 d = wh - mFloor[k];
                if (d < t.depth) { t.depth = d; ++found; }
            }
            LL_DEBUGS("SeaState") << "sea floor: " << found << " field texels above the terrain" << LL_ENDL;
        }
        else if (mFloorWanted)
        {
            LL_INFOS_ONCE("SeaState") << "sea floor pass not answered within " << kFloorWait
                                      << " s: the terrain is the floor for this build" << LL_ENDL;
        }
        mFloorWanted = false;
        mFloorHave = false;

        // SHORE DISTANCE: chamfer (1, sqrt 2) distance to the other side of the shoreline, both
        // ways, in one frame (two passes over 16 K texels each).
        std::vector<F32> dl((size_t)R * R), dw((size_t)R * R);
        const F32 big = 1e9f;
        for (S32 k = 0; k < R * R; ++k)
        {
            const bool land = mBuild[k].depth <= 0.f;
            dl[k] = land ? 0.f : big;
            dw[k] = land ? big : 0.f;
        }
        auto chamfer = [&](std::vector<F32>& d)
        {
            const F32 s2 = 1.41421356f;
            for (S32 j = 0; j < R; ++j)
                for (S32 i = 0; i < R; ++i)
                {
                    F32& v = d[(size_t)j * R + i];
                    if (i > 0) v = llmin(v, d[(size_t)j * R + i - 1] + 1.f);
                    if (j > 0)
                    {
                        v = llmin(v, d[(size_t)(j - 1) * R + i] + 1.f);
                        if (i > 0) v = llmin(v, d[(size_t)(j - 1) * R + i - 1] + s2);
                        if (i < R - 1) v = llmin(v, d[(size_t)(j - 1) * R + i + 1] + s2);
                    }
                }
            for (S32 j = R - 1; j >= 0; --j)
                for (S32 i = R - 1; i >= 0; --i)
                {
                    F32& v = d[(size_t)j * R + i];
                    if (i < R - 1) v = llmin(v, d[(size_t)j * R + i + 1] + 1.f);
                    if (j < R - 1)
                    {
                        v = llmin(v, d[(size_t)(j + 1) * R + i] + 1.f);
                        if (i < R - 1) v = llmin(v, d[(size_t)(j + 1) * R + i + 1] + s2);
                        if (i > 0) v = llmin(v, d[(size_t)(j + 1) * R + i - 1] + s2);
                    }
                }
        };
        chamfer(dl);
        chamfer(dw);
        for (S32 k = 0; k < R * R; ++k)
        {
            FieldTexel& t = mBuild[k];
            t.shore = t.depth > 0.f ? llmin((dl[k] - 0.5f) * kFieldTexel, kShoreCap)
                                    : -llmin((dw[k] - 0.5f) * kFieldTexel, kShoreCap);
        }
        mBuildStage = 2;
        mBuildRow = 0;
        return;
    }
    if (mBuildStage == 2)
    {
        // FETCH: march upwind (against the waves' travel) to the first land; out of the window
        // without land = the open sea.
        const F32 ux = -mBuildWindDir[0] * 2.f, uy = -mBuildWindDir[1] * 2.f;   // 2 texels a step
        const U32 end = llmin(mBuildRow + kFieldRowsPerFrame, kFieldRes);
        for (U32 j = mBuildRow; j < end; ++j)
            for (U32 i = 0; i < kFieldRes; ++i)
            {
                FieldTexel& t = mBuild[(size_t)j * kFieldRes + i];
                if (t.depth <= 0.f) { t.fetch = 0.f; continue; }
                t.fetch = kFetchOpen;
                F32 px = (F32)i + 0.5f, py = (F32)j + 0.5f;
                for (U32 s = 1; s <= kFetchSteps; ++s)
                {
                    px += ux; py += uy;
                    if (px < 0.f || py < 0.f || px >= (F32)R || py >= (F32)R) break;
                    if (mBuild[(size_t)(S32)py * kFieldRes + (S32)px].depth <= 0.f)
                    {
                        t.fetch = (F32)s * 2.f * kFieldTexel;
                        break;
                    }
                }
            }
        mBuildRow = end;
        if (mBuildRow < kFieldRes) return;
        publishField();
        mBuildStage = 0;
    }
}

void SLSeaState::publishField()
{
    mField.swap(mBuild);
    mFieldOrigin[0] = mBuildOrigin[0]; mFieldOrigin[1] = mBuildOrigin[1];
    mFieldRegionOrigin[0] = mBuildRegionOrigin[0]; mFieldRegionOrigin[1] = mBuildRegionOrigin[1];
    mFieldWindDir[0] = mBuildWindDir[0]; mFieldWindDir[1] = mBuildWindDir[1];
    mFieldPacked.resize((size_t)kFieldRes * kFieldRes * 2);
    for (size_t k = 0; k < mField.size(); ++k)
    {
        const FieldTexel& t = mField[k];
        mFieldPacked[2 * k]     = packHalf2(t.depth, t.shore);
        mFieldPacked[2 * k + 1] = packHalf2(t.fetch, t.dh);
    }
    mFieldValid = true;
    ++mFieldVersion;
    // (terrain still arriving: due again in kFieldUnknownRetry s instead of kFieldMaxAge)
    if (mBuildUnknown) mFieldTime -= (F64)(kFieldMaxAge - kFieldUnknownRetry);
}

void SLSeaState::takeFloor(U32 serial, const F32* floor)
{
    if (!floor || !mFloorWanted || serial != mFloorReq.serial) return;   // (a stale answer: dropped)
    mFloor.assign(floor, floor + (size_t)kFieldRes * kFieldRes);
    mFloorHave = true;
}

void SLSeaState::noteObjectChange(F32 x, F32 y, F32 z, F32 r)
{
    if (mState.mode == MODE_FLAT || mFloorDirty) return;
    LLViewerRegion* rg = gAgent.getRegion();
    if (!rg || z - r > rg->getWaterHeight()) return;   // (wholly above the water: not the floor)
    if (floorWindowContains(x, y, r)) mFloorDirty = true;
}

bool SLSeaState::floorWindowContains(F32 x, F32 y, F32 r) const
{
    LLViewerRegion* rg = gAgent.getRegion();
    if (!mFieldValid || !rg) return false;
    const F32 ox = mFieldOrigin[0] + (F32)(mFieldRegionOrigin[0] - rg->getOriginGlobal().mdV[VX]);
    const F32 oy = mFieldOrigin[1] + (F32)(mFieldRegionOrigin[1] - rg->getOriginGlobal().mdV[VY]);
    const F32 e = (F32)kFieldRes * kFieldTexel;
    return x + r >= ox && y + r >= oy && x - r <= ox + e && y - r <= oy + e;
}

SLSeaState::FieldTexel SLSeaState::fieldAt(F32 x, F32 y) const
{
    FieldTexel open;
    LLViewerRegion* rg = gAgent.getRegion();
    if (!mFieldValid || !rg) return open;
    const F32 ox = mFieldOrigin[0] + (F32)(mFieldRegionOrigin[0] - rg->getOriginGlobal().mdV[VX]);
    const F32 oy = mFieldOrigin[1] + (F32)(mFieldRegionOrigin[1] - rg->getOriginGlobal().mdV[VY]);
    const F32 gx = (x - ox) / kFieldTexel - 0.5f, gy = (y - oy) / kFieldTexel - 0.5f;
    const F32 fx0 = std::floor(gx), fy0 = std::floor(gy);
    const S32 cx = (S32)fx0, cy = (S32)fy0;
    const S32 R = (S32)kFieldRes;
    if (cx < 0 || cy < 0 || cx + 1 >= R || cy + 1 >= R) return open;
    const F32 fx = gx - fx0, fy = gy - fy0;
    auto at = [&](S32 i, S32 j) -> const FieldTexel& { return mField[(size_t)j * R + i]; };
    const FieldTexel &a = at(cx, cy), &b = at(cx + 1, cy), &c = at(cx, cy + 1), &d = at(cx + 1, cy + 1);
    auto bl = [&](F32 va, F32 vb, F32 vc, F32 vd) { return (va + (vb - va) * fx) + ((vc + (vd - vc) * fx) - (va + (vb - va) * fx)) * fy; };
    FieldTexel t;
    t.depth = bl(a.depth, b.depth, c.depth, d.depth);
    t.shore = bl(a.shore, b.shore, c.shore, d.shore);
    t.fetch = bl(a.fetch, b.fetch, c.fetch, d.fetch);
    t.dh = bl(a.dh, b.dh, c.dh, d.dh);
    const F32 e = llmin(llmin(gx, gy), (F32)(R - 2) - llmax(gx, gy));
    const F32 w = llclamp(e / 8.f, 0.f, 1.f);
    t.depth = open.depth + (t.depth - open.depth) * w;
    t.shore = open.shore + (t.shore - open.shore) * w;
    t.fetch = open.fetch + (t.fetch - open.fetch) * w;
    t.dh *= w;
    return t;
}

// CPU twin of clean_sea_state.glsl seaSurface (slseamath.h slsea::eval): the state, the field
// texel and its shore gradient (central differences of the bilinear field, +-2 m).
static void seaInputs(const SLSeaState& sea, F32 x, F32 y, slsea::Waves& w, slsea::Params& p, slsea::Texel& t)
{
    const SLSeaState::State& s = sea.state();
    for (U32 i = 0; i < SLSeaState::kWaves; ++i)
    {
        w.kvec[i][0] = s.waves[i].kvec[0]; w.kvec[i][1] = s.waves[i].kvec[1];
        w.k[i] = s.waves[i].k; w.amp[i] = s.waves[i].amp; w.phase[i] = s.waves[i].phase;
    }
    p.hs = s.hs; p.surf = s.surf; p.runup = s.runup; p.swashPhase = s.swashPhase; p.swashLen = s.swashLen;
    p.dir[0] = s.windDir[0]; p.dir[1] = s.windDir[1];
    const SLSeaState::FieldTexel f = sea.fieldAt(x, y);
    t.depth = f.depth; t.shore = f.shore; t.fetch = f.fetch; t.dh = f.dh;
    constexpr F32 kStep = 2.f;
    t.shoreN[0] = (sea.fieldAt(x + kStep, y).shore - sea.fieldAt(x - kStep, y).shore) / (2.f * kStep);
    t.shoreN[1] = (sea.fieldAt(x, y + kStep).shore - sea.fieldAt(x, y - kStep).shore) / (2.f * kStep);
}

static void seaEval(const SLSeaState& sea, F32 x, F32 y, F32& height, F32& breaking, F32& swash)
{
    height = 0.f; breaking = 0.f; swash = 0.f;
    if (sea.state().mode == SLSeaState::MODE_FLAT) return;
    slsea::Waves w; slsea::Params p; slsea::Texel t;
    seaInputs(sea, x, y, w, p, t);
    slsea::eval(w, p, t, x, y, height, breaking, swash);
}

F32 SLSeaState::boreStrengthAt(F32 x, F32 y) const
{
    if (mState.mode == MODE_FLAT) return 0.f;
    slsea::Waves w; slsea::Params p; slsea::Texel t;
    seaInputs(*this, x, y, w, p, t);
    return slsea::boreStrength(w, p, t);
}

F32 SLSeaState::surfaceHeightAt(F32 x, F32 y) const
{
    F32 h, b, sw;
    seaEval(*this, x, y, h, b, sw);
    return h;
}

void SLSeaState::surfAt(F32 x, F32 y, F32& breaking, F32& swash) const
{
    F32 h;
    seaEval(*this, x, y, h, breaking, swash);
}

void SLSeaState::fillFrameInfo(F32 sea[kFrameFloats], F32 seaWave[kWaveFloats]) const
{
    std::memset(sea, 0, sizeof(F32) * kFrameFloats);
    std::memset(seaWave, 0, sizeof(F32) * kWaveFloats);
    const State& s = mState;
    sea[0] = (F32)s.mode; sea[1] = s.storm; sea[2] = s.beaufort; sea[3] = s.mode ? s.detail : 1.f;
    sea[4] = s.windDir[0]; sea[5] = s.windDir[1]; sea[6] = s.windSpeed; sea[7] = s.mode ? s.hs : 0.f;
    sea[8] = s.mode ? s.surf : 0.f; sea[9] = s.runup; sea[10] = s.swashPhase; sea[11] = s.swashLen;
    LLViewerRegion* rg = gAgent.getRegion();
    if (rg && mFieldValid)
    {
        sea[12] = mFieldOrigin[0] + (F32)(mFieldRegionOrigin[0] - rg->getOriginGlobal().mdV[VX]);
        sea[13] = mFieldOrigin[1] + (F32)(mFieldRegionOrigin[1] - rg->getOriginGlobal().mdV[VY]);
    }
    sea[14] = 1.f / kFieldTexel;
    const LLVector3 cam = gAgentCamera.getCameraPositionAgent();
    sea[15] = s.mode ? surfaceHeightAt(cam.mV[VX], cam.mV[VY]) : 0.f;
    sea[16] = (F32)kFieldRes;   // [17] valid, [18] offset: the renderer's
    sea[19] = s.chop;
    // [20..23] the grid: the renderer's. [24..25] the main camera's horizontal forward (the
    // water VS draws its grid far to near along it), [26..27] 0.
    {
        LLVector3 at = LLViewerCamera::getInstance()->getAtAxis();
        at.mV[VZ] = 0.f;
        const F32 l = at.normVec();
        sea[24] = l > 1e-4f ? at.mV[VX] : 1.f;
        sea[25] = l > 1e-4f ? at.mV[VY] : 0.f;
    }
    for (U32 i = 0; i < kWaves; ++i)
    {
        const Wave& w = s.waves[i];
        F32* o = seaWave + i * 8;
        o[0] = w.kvec[0]; o[1] = w.kvec[1]; o[2] = s.mode ? w.amp : 0.f; o[3] = w.phase;
        o[4] = w.steep; o[5] = w.k; o[6] = w.omega; o[7] = 0.f;
    }
}
