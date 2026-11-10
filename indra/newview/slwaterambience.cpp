/**
 * @file slwaterambience.cpp
 * @brief Water ambience inputs (#1019 / #1020): see slwaterambience.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * (LGPL v2.1, see slweather.h)
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "slwaterambience.h"

#include "llagent.h"
#include "llsurface.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "slseastate.h"   // the one sea state (#1020)

#include <algorithm>
#include <cmath>

namespace
{
// ---- shore (slwaterambience.h SHORE) ----
constexpr F32 kRadius[SLWaterAmbience::kRadii] = {3.f, 6.f, 10.f, 15.f, 22.f, 32.f, 45.f, 64.f, 90.f, 128.f, 180.f, 256.f};
constexpr U32 kNearRadii = 6;         // the rings within 40 m (waterNear)
constexpr U32 kBisect = 4;            // refinement steps on each direction's first change
constexpr F32 kBayWeight = 10.f;      // m: the directions this much farther than the nearest still pull the direction
constexpr U32 kFetchSpread = 2;       // directions either side of the nearest one (x 22.5 deg) for the fetch
constexpr F32 kExposure0 = 15.f, kExposure1 = 300.f;   // m of fetch: a pond .. open water
constexpr F32 kPlaceCell = 64.f;      // m: one bed variant per stretch of coast this size

struct State
{
    SLWaterAmbience::SeaState sea;
    SLWaterAmbience::Shore    shore;
    LLVector3d lastAt;
    F64        lastT = -1.0e9;
    bool       due = true;
};

State& st()
{
    static State s;
    return s;
}

F32 smooth01(F32 e0, F32 e1, F32 x)
{
    const F32 t = llclamp((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// THE SEA STATE (#1020, slseastate.h): the sounds read the one sea state the waves, the foam and
// the mist read. windSpeed / windDir (the waves' travel = where the wind blows TO), storm and
// beaufort as they are; sea = surf (0..1, lapping .. crashing at an open beach: the breakers the
// surface draws), period = swashPeriod (s between breakers at the shore). Already eased there
// (seas build over ~25 s, storms over ~20 s): no second easing here. Computed with the waves in
// flat-water mode too (only the GPU's copy is zeroed), so the sounds never depend on the waves'
// quality setting. Breaker sync: SLSeaState posts SLWaterEvents::BREAK at the shore nearest the
// camera when its swash clock wraps (waves on); the audio's own breaker clock stands down then.
SLWaterAmbience::SeaState readSeaState()
{
    SLWaterAmbience::SeaState s;
    SLSeaState& sea = SLSeaState::instance();
    sea.update();   // (a no-op once per frame: llappviewer's world frame already stepped it)
    const SLSeaState::State& ss = sea.state();
    s.windSpeed = ss.windSpeed;
    s.windDir[0] = ss.windDir[0];
    s.windDir[1] = ss.windDir[1];
    s.storm = llclamp(ss.storm, 0.f, 1.f);
    s.beaufort = llclamp(ss.beaufort, 0.f, 12.f);
    s.sea = llclamp(ss.surf, 0.f, 1.f);
    s.period = ss.swashPeriod;
    return s;
}

// One sample: below the water there (the region's own water height; outside every region: the
// void water, wet)? A region whose terrain has not arrived yet is UNKNOWN (its zero heights
// would read as water: a false shore at the edge of what is loaded, right after a login or
// a teleport): the direction's ray stops there.
enum ESample : S32 { S_UNKNOWN = -1, S_DRY = 0, S_WET = 1 };
ESample sampleAt(LLWorld& w, const LLVector3d& p)
{
    LLViewerRegion* r = w.getRegionFromPosGlobal(p);
    if (!r) return S_WET;
    if (!r->getLand().hasZData()) return S_UNKNOWN;
    return r->getLand().resolveHeightGlobal(p) < r->getWaterHeight() ? S_WET : S_DRY;
}

void sampleShore(const LLVector3d& ear, SLWaterAmbience::Shore& out)
{
    constexpr U32 kAzimuths = SLWaterAmbience::kAzimuths;
    constexpr U32 kRadii = SLWaterAmbience::kRadii;
    LLWorld& w = LLWorld::instance();
    LLViewerRegion* here = w.getRegionFromPosGlobal(ear);
    if (!here) here = gAgent.getRegion();
    if (!here)
    {
        out.valid = false;
        return;
    }
    const F32 waterH = here->getWaterHeight();
    const ESample centre = sampleAt(w, ear);
    if (centre == S_UNKNOWN)
    {
        out.valid = false;
        return;
    }
    const bool centreWet = centre == S_WET;
    bool wet[kAzimuths][kRadii];
    U32 known[kAzimuths];        // rings sampled before the first unknown one
    F32 change[kAzimuths];       // m: the first land / water change in each direction (< 0: none)
    F32 run[kAzimuths];          // m: the water stretch's length in each direction (kOpenFetch: to the edge)
    U32 nWet = centreWet ? 1 : 0, nNearWet = nWet, nNear = 1, nAll = 1;
    for (U32 a = 0; a < kAzimuths; ++a)
    {
        const F32 ang = 6.2831853f * (F32)a / (F32)kAzimuths;
        const F64 cx = std::cos(ang), cy = std::sin(ang);
        known[a] = kRadii;
        for (U32 i = 0; i < kRadii; ++i)
        {
            const LLVector3d p(ear.mdV[VX] + cx * kRadius[i], ear.mdV[VY] + cy * kRadius[i], ear.mdV[VZ]);
            const ESample sm = sampleAt(w, p);
            if (sm == S_UNKNOWN) { known[a] = i; break; }
            wet[a][i] = sm == S_WET;
            ++nAll;
            nWet += wet[a][i] ? 1 : 0;
            if (i < kNearRadii) { ++nNear; nNearWet += wet[a][i] ? 1 : 0; }
        }
        change[a] = -1.f;
        S32 first = -1;
        for (U32 i = 0; i < known[a] && first < 0; ++i)
            if (wet[a][i] != centreWet) first = (S32)i;
        if (first >= 0)
        {
            // Bisect between the last ring like the ear and the first one not.
            F32 lo = first ? kRadius[first - 1] : 0.f, hi = kRadius[first];
            for (U32 k = 0; k < kBisect; ++k)
            {
                const F32 mid = 0.5f * (lo + hi);
                const LLVector3d p(ear.mdV[VX] + cx * mid, ear.mdV[VY] + cy * mid, ear.mdV[VZ]);
                const ESample sm = sampleAt(w, p);
                ((sm == S_UNKNOWN || (sm == S_WET) == centreWet) ? lo : hi) = mid;
            }
            change[a] = 0.5f * (lo + hi);
        }
        // The water stretch: from the ear (over water) or from the change (on land) to the next dry ring.
        run[a] = 0.f;
        const S32 start = centreWet ? 0 : first;
        if (start >= 0)
        {
            S32 end = -1;
            for (U32 i = (U32)start; i < known[a] && end < 0; ++i)
                if (!wet[a][i]) end = (S32)i;
            const F32 from = centreWet ? 0.f : change[a];
            if (end >= 0)
                run[a] = std::max(0.5f * ((end ? kRadius[end - 1] : 0.f) + kRadius[end]) - from, 0.f);
            else   // (wet to the edge of the grid: open; to the edge of what is loaded: as far as known)
                run[a] = known[a] == kRadii ? SLWaterAmbience::kOpenFetch : std::max((known[a] ? kRadius[known[a] - 1] : 0.f) - from, 0.f);
        }
    }
    out.valid = true;
    out.overWater = centreWet;
    out.waterHeight = waterH;
    out.height = (F32)ear.mdV[VZ] - waterH;
    out.waterAll = (F32)nWet / (F32)nAll;
    out.waterNear = (F32)nNearWet / (F32)nNear;
    // The nearest line, the bay-weighted direction around it.
    S32 best = -1;
    for (U32 a = 0; a < kAzimuths; ++a)
        if (change[a] >= 0.f && (best < 0 || change[a] < change[best])) best = (S32)a;
    out.found = best >= 0;
    if (out.found)
    {
        const F32 dmin = change[best];
        F32 dx = 0.f, dy = 0.f;
        for (U32 a = 0; a < kAzimuths; ++a)
        {
            if (change[a] < 0.f) continue;
            const F32 wgt = std::exp(-(change[a] - dmin) / kBayWeight);
            const F32 ang = 6.2831853f * (F32)a / (F32)kAzimuths;
            dx += wgt * std::cos(ang);
            dy += wgt * std::sin(ang);
        }
        const F32 l = std::sqrt(dx * dx + dy * dy);
        const F32 ab = 6.2831853f * (F32)best / (F32)kAzimuths;
        out.dir = l > 1e-3f ? LLVector3(dx / l, dy / l, 0.f) : LLVector3(std::cos(ab), std::sin(ab), 0.f);
        out.dist = dmin;
        out.pointGlobal = LLVector3d(ear.mdV[VX] + (F64)(out.dir.mV[VX] * dmin), ear.mdV[VY] + (F64)(out.dir.mV[VY] * dmin), waterH);
        // Fetch: the water beyond the line around the nearest direction (on land), or the mean
        // stretch all round (over water: how open the water the listener is on is).
        F32 fetch = 0.f;
        if (centreWet)
        {
            for (U32 a = 0; a < kAzimuths; ++a) fetch += run[a];
            fetch /= (F32)kAzimuths;
        }
        else
        {
            for (S32 k = -(S32)kFetchSpread; k <= (S32)kFetchSpread; ++k)
                fetch = std::max(fetch, run[(best + k + (S32)kAzimuths) % kAzimuths]);
        }
        out.fetch = fetch;
        const S32 px = (S32)std::floor(out.pointGlobal.mdV[VX] / kPlaceCell), py = (S32)std::floor(out.pointGlobal.mdV[VY] / kPlaceCell);
        U32 h = ((U32)px * 73856093u) ^ ((U32)py * 19349663u);
        h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
        out.place = h;
    }
    else
    {
        out.dist = 1.0e6f;
        out.fetch = centreWet ? SLWaterAmbience::kOpenFetch : 0.f;   // (open sea all round, or dry land all round)
        out.place = 0;
    }
    out.exposure = smooth01(kExposure0, kExposure1, out.fetch);
}
} // namespace

void SLWaterAmbience::update(const LLVector3d& earGlobal, F64 now, F32 dt)
{
    State& s = st();
    // Sea state: the shared one (already eased).
    s.sea = readSeaState();
    (void)dt;
    // Shore: only when due (moved, or a while passed).
    const F64 mx = earGlobal.mdV[VX] - s.lastAt.mdV[VX], my = earGlobal.mdV[VY] - s.lastAt.mdV[VY];
    if (s.due || (mx * mx + my * my > (F64)(kResampleMove * kResampleMove) && now - s.lastT > kResampleMinSecs)
        || now - s.lastT > kResampleSecs)
    {
        sampleShore(earGlobal, s.shore);
        s.lastAt = earGlobal;
        s.lastT = now;
        s.due = false;
    }
    else
    {
        // Between samples only the height above the water follows the ear (flying up / down).
        s.shore.height = (F32)earGlobal.mdV[VZ] - s.shore.waterHeight;
    }
}

void SLWaterAmbience::invalidate()
{
    st().due = true;
}

const SLWaterAmbience::SeaState& SLWaterAmbience::seaState()
{
    return st().sea;
}

const SLWaterAmbience::Shore& SLWaterAmbience::shore()
{
    return st().shore;
}
