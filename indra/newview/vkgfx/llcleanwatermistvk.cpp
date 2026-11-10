/** @file llcleanwatermistvk.cpp
 * WATER MIST (#963): the proximity field and the weather / time-of-day drive (llcleanwatermistvk.h).
 */

#include "linden_common.h"

#include "llcleanwatermistvk.h"
#include "llagent.h"   // the agent frame's global origin (the field and the noise are global)

#include <deque>   // (llenvironment.h needs it first, as in llcleanvolfogvk.cpp)
#include "llenvironment.h"
#include "llerror.h"
#include "llframetimer.h"
#include "llhaze.h"
#include "llsettingssky.h"
#include "llsurface.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "slseastate.h"   // the one sea state (#1020)
#include "slweather.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
// The mist's extinction at the water (/m) at strength 1 in its thickest conditions: a calm, cool
// dawn over a lake gives ~150 m visibility within a metre or two of the surface (3.9 / 0.025).
constexpr float kMistSigma = 0.025f;
// The storm spray's extinction over the surf (/m) at storm 1.
constexpr float kSpraySigma = 0.05f;
// The noise's bank size (m) and tile (cells; clean_medium.glsl cmMistNoise period 64): the drift
// offsets wrap at kTileCells x kBankSize so the shader sees small numbers without seams.
constexpr float  kBankSize = 120.f;
constexpr double kTile = 64.0 * kBankSize;
// The calm drift when the weather has no wind (m/s toward 68 degrees: the haze mist's default).
constexpr float kCalmDrift = 0.4f;
constexpr float kCalmDir = 68.f;
// The drive's easing time (s): weather / sun changes roll in, never snap.
constexpr float kEase = 8.f;

float smooth01(float e0, float e1, float x)
{
    const float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

double wrapTile(double v)
{
    const double r = std::fmod(v, kTile);
    return r < 0.0 ? r + kTile : r;
}

uint16_t toHalf(float v)
{
    // (finite, |v| < 65504 by construction: heights and clamped distances)
    uint32_t b;
    std::memcpy(&b, &v, 4);
    const uint32_t s = (b >> 16) & 0x8000u;
    const int32_t  e = (int32_t)((b >> 23) & 0xFFu) - 127 + 15;
    uint32_t m = b & 0x7FFFFFu;
    if (e <= 0) return (uint16_t)s;                       // (tiny: 0)
    if (e >= 31) return (uint16_t)(s | 0x7BFFu);          // (clamped to the largest finite)
    m += 0x1000u;                                         // round to nearest
    if (m & 0x800000u) { m = 0; if (e + 1 >= 31) return (uint16_t)(s | 0x7BFFu); return (uint16_t)(s | ((uint32_t)(e + 1) << 10)); }
    return (uint16_t)(s | ((uint32_t)e << 10) | (m >> 13));
}

// Two-pass 8-neighbour chamfer distance (m) to the nearest cell where seed != 0.
void chamfer(std::vector<float>& d, const std::vector<uint8_t>& seed, uint8_t want, uint32_t n, float cell)
{
    const float inf = 1e9f, a = cell, b = cell * 1.41421356f;
    for (uint32_t i = 0; i < n * n; ++i) d[i] = seed[i] == want ? 0.f : inf;
    for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x)
        {
            float& v = d[y * n + x];
            if (x > 0) v = std::min(v, d[y * n + x - 1] + a);
            if (y > 0)
            {
                v = std::min(v, d[(y - 1) * n + x] + a);
                if (x > 0) v = std::min(v, d[(y - 1) * n + x - 1] + b);
                if (x + 1 < n) v = std::min(v, d[(y - 1) * n + x + 1] + b);
            }
        }
    for (uint32_t y = n; y-- > 0;)
        for (uint32_t x = n; x-- > 0;)
        {
            float& v = d[y * n + x];
            if (x + 1 < n) v = std::min(v, d[y * n + x + 1] + a);
            if (y + 1 < n)
            {
                v = std::min(v, d[(y + 1) * n + x] + a);
                if (x + 1 < n) v = std::min(v, d[(y + 1) * n + x + 1] + b);
                if (x > 0) v = std::min(v, d[(y + 1) * n + x - 1] + b);
            }
        }
}
} // namespace

// THE SEA STATE (#1020, slseastate.h): the mist reads the one sea state the waves, the foam and the
// sounds read. windDir is the waves' travel direction = where the wind blows TO (the mist's banks
// drift that way); windSpeed the eased mean wind (no gusts: banks do not jitter with gusts) -- with
// no weather source, the region's calm-day breeze (LL's wave1Dir); storm 0..1 (lightning, rain and
// gales combined, eased). The waves' flat-water mode still fills the wind and the storm.
LLCleanWaterMistVK::SeaState LLCleanWaterMistVK::mistSeaState()
{
    SeaState s;
    SLSeaState& sea = SLSeaState::instance();
    sea.update();   // (a no-op once per frame: llappviewer's world frame already stepped it)
    const SLSeaState::State& st = sea.state();
    const float u = std::isfinite(st.windSpeed) ? std::max(st.windSpeed, 0.f) : 0.f;
    s.wind[0] = std::isfinite(st.windDir[0]) ? st.windDir[0] * u : 0.f;
    s.wind[1] = std::isfinite(st.windDir[1]) ? st.windDir[1] * u : 0.f;
    s.speed = u;
    s.storm = std::isfinite(st.storm) ? std::clamp(st.storm, 0.f, 1.f) : 0.f;
    return s;
}

bool LLCleanWaterMistVK::ensureBuffers(VkDeviceContext* ctx)
{
    if (mBuf[0].buffer && mBuf[1].buffer) return true;
    if (mFrame < mRetryAt) return false;
    const VkDeviceSize bytes = (VkDeviceSize)kCells * kCells * 4u;
    {
        VkMemScope memcat(VKMEM_TARGET);
        VkDeviceContext::SoftAllocScope soft(ctx);
        for (GpuBuffer& b : mBuf)
            if (!b.buffer)
                b = ctx->createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
    }
    if (!mBuf[0].buffer || !mBuf[1].buffer || !mBuf[0].mapped || !mBuf[1].mapped)
    {
        LL_WARNS("VKWaterMist") << "water mist field buffers (2 x " << bytes / 1024u
                                << " KB) failed to allocate; mist off, retrying later" << LL_ENDL;
        for (GpuBuffer& b : mBuf) { if (b.buffer) ctx->destroyBuffer(b); b = GpuBuffer{}; }
        mRetryAt = mFrame + kRetryFrames;
        return false;
    }
    for (int i = 0; i < 2; ++i)
    {
        VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        ai.buffer = mBuf[i].buffer;
        mAddr[i] = vkGetBufferDeviceAddress(ctx->device(), &ai);
    }
    mLand.assign(kCells * kCells, 0.f);
    mWater.assign(kCells * kCells, 0.f);
    mDist.assign(kCells * kCells, 0.f);
    mIsWater.assign(kCells * kCells, 0u);
    mPublished = -1;
    mRow = kCells;
    mBuildStarted = -1e9;
    LL_INFOS("VKWaterMist") << "water mist on: proximity field " << kCells << "x" << kCells << " x " << kCellSize
                            << " m, 2 x " << bytes / 1024u << " KB" << LL_ENDL;
    return true;
}

void LLCleanWaterMistVK::destroy(VkDeviceContext* ctx)
{
    if (ctx)
        for (GpuBuffer& b : mBuf)
            if (b.buffer) ctx->destroyBuffer(b);
    mBuf[0] = mBuf[1] = GpuBuffer{};
    mAddr[0] = mAddr[1] = 0;
    mPublished = -1;
    mRow = kCells;
    mOn = false;
    std::vector<float>().swap(mLand);
    std::vector<float>().swap(mWater);
    std::vector<float>().swap(mDist);
    std::vector<uint8_t>().swap(mIsWater);
}

// A few rows of terrain + water samples (fixed cost); a new build starts when the camera's lattice
// cell moved off the published field's centre, on the refresh timer, or with nothing published.
void LLCleanWaterMistVK::stepBuild(const float camAgent[3], float waterHeight)
{
    const double* origin = mOrigin;
    const double half = 0.5 * kCells * kCellSize;
    const double cx = std::floor(((double)camAgent[0] + origin[0]) / kSnap) * kSnap + 0.5 * kSnap;
    const double cy = std::floor(((double)camAgent[1] + origin[1]) / kSnap) * kSnap + 0.5 * kSnap;
    const double now = LLFrameTimer::getElapsedSeconds();
    if (mRow >= kCells)
    {
        const bool moved = mPublished < 0 || std::abs(mCorner[0] + half - cx) > 1.0 || std::abs(mCorner[1] + half - cy) > 1.0;
        if (!moved && now - mBuildStarted < kRefreshSecs) return;
        mBuildCorner[0] = cx - half; mBuildCorner[1] = cy - half;
        mBuildStarted = now;
        mRow = 0;
    }
    LLWorld* world = LLWorld::getInstance();
    // (cells outside every region: SL's void water at the camera region's height)
    const float voidWater = std::isfinite(waterHeight) ? waterHeight : 20.f;
    const uint32_t end = std::min(mRow + kRowsPerFrame, kCells);
    for (uint32_t y = mRow; y < end; ++y)
    {
        LLViewerRegion* region = nullptr;
        for (uint32_t x = 0; x < kCells; ++x)
        {
            const LLVector3 p((F32)(mBuildCorner[0] + (x + 0.5) * kCellSize - origin[0]),
                              (F32)(mBuildCorner[1] + (y + 0.5) * kCellSize - origin[1]), 0.f);
            if (!region || !region->pointInRegionGlobal(LLVector3d(p) + LLVector3d(origin[0], origin[1], 0.0)))
                region = world->getRegionFromPosAgent(p);
            const uint32_t i = y * kCells + x;
            if (region)
            {
                const LLVector3 local = p - region->getOriginAgent();
                mLand[i] = region->getLand().resolveHeightRegion(local);
                mWater[i] = region->getWaterHeight();
            }
            else
            {
                mLand[i] = -1e3f;
                mWater[i] = voidWater;
            }
            if (!std::isfinite(mLand[i])) mLand[i] = -1e3f;
            if (!std::isfinite(mWater[i])) mWater[i] = voidWater;
            mIsWater[i] = mLand[i] < mWater[i] ? 1u : 0u;
        }
    }
    mRow = end;
    if (mRow >= kCells) finishBuild();
}

// The distance transforms, the pack, the publish (into the buffer no frame in flight reads).
void LLCleanWaterMistVK::finishBuild()
{
    const int target = mPublished < 0 ? 0 : 1 - mPublished;
    if (mPublished >= 0 && mFrame - mPublishFrame < 4u) { mRow = kCells - 1u; return; }   // (retried next frame)
    const uint32_t n = kCells, cells = n * n;
    std::vector<float> toLand(cells);
    chamfer(mDist, mIsWater, 1u, n, kCellSize);   // land cells: distance to water
    chamfer(toLand, mIsWater, 0u, n, kCellSize);  // water cells: distance to land
    uint32_t* dst = static_cast<uint32_t*>(mBuf[target].mapped);
    float top = -1e9f;
    bool anyWater = false;
    for (uint32_t i = 0; i < cells; ++i)
    {
        // (cell centre to cell centre: half a cell puts the shore line between them)
        float d = mIsWater[i] ? -std::max(toLand[i] - 0.5f * kCellSize, 0.f) : std::max(mDist[i] - 0.5f * kCellSize, 0.f);
        d = std::clamp(d, -2000.f, 2000.f);
        dst[i] = (uint32_t)toHalf(mWater[i]) | ((uint32_t)toHalf(d) << 16);
        top = std::max(top, mWater[i]);
        anyWater = anyWater || mIsWater[i];
    }
    mTopWater = top;
    mCorner[0] = mBuildCorner[0]; mCorner[1] = mBuildCorner[1];
    mPublished = target;
    mPublishFrame = mFrame;
    LL_DEBUGS("VKWaterMist") << "proximity field rebuilt at " << mCorner[0] << ", " << mCorner[1]
                             << (anyWater ? "" : " (no water in reach)") << LL_ENDL;
}

// The weather / time-of-day drive (eased).
void LLCleanWaterMistVK::drive(float dt)
{
    static LLCachedControl<F32>  s_strength(gSavedSettings, "RenderVKWaterMist", 1.f);
    static LLCachedControl<U32>  s_quality(gSavedSettings, "RenderVKVolumetricFogQuality", 1);
    static LLCachedControl<bool> s_ocol(gSavedSettings, "RenderVKMist", false);
    static LLCachedControl<F32>  s_ocolWind(gSavedSettings, "RenderVKMistWindSpeed", 2.f);
    static LLCachedControl<F32>  s_ocolWindDir(gSavedSettings, "RenderVKMistWindDirection", 68.f);
    const float strength = (float)LLHaze::bounded(s_strength, 1., 0., 4.);
    mWisps = s_quality > 0;   // (the low fog tier: one noise octave)

    const SeaState sea = mistSeaState();
    const SLWeather::State& wx = SLWeather::instance().state();
    float sunZ = 0.f;
    if (const auto sky = LLEnvironment::instance().getCurrentSky())
    {
        const LLVector3 d = sky->getSunDirection();
        if (std::isfinite(d.mV[VZ])) sunZ = d.mV[VZ];
    }
    const float cloud = std::clamp(wx.cloud, 0.f, 1.f);
    // Cool air over warmer water (night, dawn, dusk: the sun low or down) makes the mist; a high,
    // clear sun burns it off; humid weather feeds it.
    const float cool = 1.f - smooth01(0.02f, 0.5f, sunZ);
    const float sunny = smooth01(0.1f, 0.6f, sunZ) * (1.f - 0.8f * cloud);
    const float humid = std::clamp(0.6f * wx.rain + 0.3f * wx.snow + 0.8f * wx.fog + 0.25f * cloud, 0.f, 1.f);
    const float amount = std::clamp((0.2f + 0.8f * cool) * (1.f - 0.85f * sunny) + 0.5f * humid, 0.f, 1.25f);
    // Wind mixes it up and away (thinner, taller, torn into banks, pushed further inland).
    const float windy = smooth01(1.f, 10.f, sea.speed);
    const float calm = 1.f - 0.8f * smooth01(3.f, 12.f, sea.speed);
    const float sigma = kMistSigma * strength * amount * calm;
    const float height = 2.5f + 3.5f * windy;
    const float reach = std::clamp(30.f + 3.f * sea.speed, 30.f, 80.f);
    const float breakup = 0.25f + 0.75f * smooth01(1.5f, 9.f, sea.speed);
    const float roll = height * (0.6f + 1.4f * windy);
    // Storms whip spray off the surf: a thin dense layer over the breaking band.
    const float spray = kSpraySigma * strength * sea.storm;
    const float sprayHeight = 2.f + 2.f * sea.storm;

    const float k = mEased ? 1.f - std::exp(-std::max(dt, 0.f) / kEase) : 1.f;
    mEased = true;
    mSigma += (sigma - mSigma) * k;
    mHeight += (height - mHeight) * k;
    mReach += (reach - mReach) * k;
    mBreakup += (breakup - mBreakup) * k;
    mRoll += (roll - mRoll) * k;
    mSpray += (spray - mSpray) * k;
    mSprayHeight += (sprayHeight - mSprayHeight) * k;

    // The banks move with the wind (the haze mist's own wind while that mode is on: the user's
    // choice), the wisps faster (shear); a calm day still drifts.
    float vx = sea.wind[0], vy = sea.wind[1];
    if (s_ocol)
    {
        const double rad = LLHaze::bounded(s_ocolWindDir, 68., -1.e6, 1.e6) * DEG_TO_RAD;
        const double sp = LLHaze::bounded(s_ocolWind, 2., -1000., 1000.);
        vx = (float)(std::sin(rad) * sp); vy = (float)(std::cos(rad) * sp);
    }
    else if (sea.speed < kCalmDrift)
    {
        vx = std::sin(kCalmDir * DEG_TO_RAD) * kCalmDrift; vy = std::cos(kCalmDir * DEG_TO_RAD) * kCalmDrift;
    }
    const double t = std::clamp(dt, 0.f, 0.25f);
    mDrift[0][0] = wrapTile(mDrift[0][0] + vx * t);
    mDrift[0][1] = wrapTile(mDrift[0][1] + vy * t);
    mDrift[1][0] = wrapTile(mDrift[1][0] + vx * t * 1.7);
    mDrift[1][1] = wrapTile(mDrift[1][1] + vy * t * 1.7);
}

void LLCleanWaterMistVK::update(VkDeviceContext* ctx, const float camAgent[3], float waterHeight, bool fogOn)
{
    const U32 fc = LLFrameTimer::getFrameCount();
    if (fc == mLastFrameCount) return;
    mLastFrameCount = fc;
    ++mFrame;
    static LLCachedControl<F32> s_strength(gSavedSettings, "RenderVKWaterMist", 1.f);
    const bool want = fogOn && ctx && camAgent && LLHaze::bounded(s_strength, 1., 0., 4.) > 0.;
    if (!want)
    {
        mOn = false;
        mEased = false;
        // Free on liveness: frames in flight stopped naming the buffers long before this.
        if ((mBuf[0].buffer || mBuf[1].buffer) && ++mOffFrames > kIdleFreeFrames)
        {
            LL_INFOS("VKWaterMist") << "water mist unused: field freed" << LL_ENDL;
            destroy(ctx);
        }
        return;
    }
    mOffFrames = 0;
    if (!ensureBuffers(ctx)) { mOn = false; return; }
    // This frame's agent-frame origin (global m): the field's corner and the noise are global, so a
    // region crossing moves neither.
    const LLVector3d o = gAgent.getPosGlobalFromAgent(LLVector3::zero);
    if (std::isfinite(o.mdV[VX]) && std::isfinite(o.mdV[VY])) { mOrigin[0] = o.mdV[VX]; mOrigin[1] = o.mdV[VY]; }
    drive(LLFrameTimer::getFrameDeltaTimeF32());
    stepBuild(camAgent, waterHeight);
    mOn = mPublished >= 0;
}

void LLCleanWaterMistVK::fillFrameInfo(uint32_t field[4], float mist[4][4]) const
{
    std::memset(field, 0, 4 * sizeof(uint32_t));
    std::memset(mist, 0, 16 * sizeof(float));
    if (!mOn || mPublished < 0 || !mAddr[mPublished] || !(mSigma > 1e-6f || mSpray > 1e-6f)) return;
    const double* origin = mOrigin;
    field[0] = (uint32_t)(mAddr[mPublished] & 0xFFFFFFFFull);
    field[1] = (uint32_t)(mAddr[mPublished] >> 32);
    field[2] = kCells;
    field[3] = mWisps ? 1u : 0u;
    mist[0][0] = (float)(mCorner[0] - origin[0]);
    mist[0][1] = (float)(mCorner[1] - origin[1]);
    mist[0][2] = 1.f / kCellSize;
    mist[0][3] = mSigma;
    mist[1][0] = mHeight;
    mist[1][1] = mReach;
    mist[1][2] = mBreakup;
    mist[1][3] = mRoll;
    // The noise is global (continuous across regions): the agent frame's origin minus the drift.
    mist[2][0] = (float)wrapTile(origin[0] - mDrift[0][0]);
    mist[2][1] = (float)wrapTile(origin[1] - mDrift[0][1]);
    mist[2][2] = (float)wrapTile(origin[0] - mDrift[1][0]);
    mist[2][3] = (float)wrapTile(origin[1] - mDrift[1][1]);
    mist[3][0] = kBankSize;
    // (no mist above this: the highest water + the layer's tail (8 H) + its roll, or the spray's)
    mist[3][1] = mTopWater + std::max(8.f * mHeight + 0.5f * mRoll, 8.f * mSprayHeight);
    mist[3][2] = mSpray;
    mist[3][3] = mSprayHeight;
}
