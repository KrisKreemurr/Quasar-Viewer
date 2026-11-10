/**
 * @file slwaterambience.h
 * @brief Water ambience inputs (#1019 / #1020): the sea state the water sounds follow and a
 *        cheap, cached estimate of the nearest shoreline around the listener.
 *        The audio itself is in slweatheraudio.cpp (WATER).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * (LGPL v2.1, see slweather.h)
 * $/LicenseInfo$
 *
 * SEA STATE (seaState()). THE sea state (#1020, SLSeaState, slseastate.h: one per frame, from
 * the weather's wind and storm and the region's water settings), read through readSeaState() in
 * the .cpp: wind, storm, Beaufort as they are; sea = surf (0..1: the breaker strength at an open
 * beach), period = swashPeriod. Eased there; no second easing here.
 *
 * SHORE (shore()). The terrain against the water height, never per frame on every sample:
 * a polar grid around the ear (kAzimuths = 16 directions x kRadii = 12 rings, 3 .. 256 m,
 * log-spaced, plus the ear itself: 193 height lookups, LLWorld::resolveLandHeightGlobal on
 * the region under each sample against THAT region's water height; outside every region: the
 * void water), then 4 bisection steps on each direction's first land / water change (64
 * more). Re-sampled when the ear has moved kResampleMove horizontally or kResampleSecs have
 * passed (never more often than kResampleMinSecs): ~260 bilinear lookups a resample, at most
 * 4 a second (well under 0.1 ms), nothing otherwise. A region whose terrain has not arrived
 * (no patch data yet) is unknown, not water: the direction's ray stops there.
 * From it:
 *   overWater  the ground under the ear is below the water surface;
 *   dist / dir the nearest land / water line (horizontal metres, unit direction ear -> line,
 *              the directions within ~10 m of the nearest weighted in: a bay surrounds you);
 *   point      that line's point (global, at the water height): the surf's source;
 *   fetch      how far the water reaches beyond the line, over the directions within 45 deg
 *              of it (an ocean is open: kOpenFetch); exposure = smooth(15 .. 300 m) of it
 *              -- a pond or a canal stays calm whatever the wind;
 *   waterNear  the wet share of the samples within 40 m, waterAll of all of them;
 *   height     the ear above the water surface (m; < 0 under it);
 *   place      a hash of the 64 m cell of the shore point: picks a bed variant per stretch of
 *              coast, so two beaches do not sound the same.
 * WHY NOT THE SEA FIELD (SLSeaState::fieldAt().shore, 512 m x 4 m around the camera): it is built
 * only while the waves are on (RenderVKWaterWaves > 0) -- flat water, the low presets, still
 * hear the shore; it is centred on the camera with a 64 m re-centring slack, so its reach from
 * the ear varies (~190..256 m) where this grid always reaches 256 m; and it does not hold what
 * the levels need beyond the distance: the water's extent beyond the line (its fetch is upwind
 * only), the wet shares, and an "unknown" for terrain not loaded yet. ~260 lookups at most 4 a
 * second (only when the ear moved) is cheaper than building that field with the waves off.
 * Terrain only: prim / mesh land and docks are not seen (a mesh island in the sea is heard as
 * open water; a prim pool on land has no shore). Linden water only (#936 mesh water is not
 * water to the sounds).
 */

#ifndef SL_WATERAMBIENCE_H
#define SL_WATERAMBIENCE_H

#include "stdtypes.h"
#include "v3dmath.h"
#include "v3math.h"

class SLWaterAmbience
{
public:
    struct SeaState
    {
        F32  windSpeed = 0.f;              // m/s, the mean wind (no gusts)
        F32  windDir[2] = {1.f, 0.f};      // unit, where the wind blows TOWARD (region axes)
        F32  beaufort = 0.f;               // 0..12
        F32  storm = 0.f;                  // 0..1
        F32  sea = 0.f;                    // 0..1 (see the header): what the levels follow
        F32  period = 6.f;                 // s
    };
    struct Shore
    {
        bool       valid = false;          // sampled at least once in a region
        bool       overWater = false;
        bool       found = false;          // a land / water line within kMaxRange
        F32        dist = 1.0e6f;          // m, horizontal (found only)
        LLVector3  dir;                    // unit, horizontal, agent axes (found only)
        LLVector3d pointGlobal;            // the line's point at the water height (found only)
        F32        fetch = 0.f;            // m
        F32        exposure = 0.f;         // 0..1
        F32        waterNear = 0.f;        // 0..1
        F32        waterAll = 0.f;         // 0..1
        F32        height = 1.0e6f;        // m above the water surface
        F32        waterHeight = 0.f;      // m, the ear's region
        U32        place = 0;
    };

    static constexpr U32 kAzimuths = 16;
    static constexpr U32 kRadii = 12;
    static constexpr F32 kMaxRange = 256.f;        // m
    static constexpr F32 kOpenFetch = 1000.f;      // m: water to the edge of the grid
    static constexpr F32 kResampleMove = 4.f;      // m
    static constexpr F64 kResampleSecs = 1.0;      // s
    static constexpr F64 kResampleMinSecs = 0.25;  // s: never more often (fast flight)

    // Audio update (SLWeatherAudio::update), main thread: ease the sea state, re-sample the
    // shore if it is due. Cheap when not due (a distance test).
    static void update(const LLVector3d& earGlobal, F64 now, F32 dt);
    // Nothing wants the water sounds (off / no engine): the next update re-samples at once.
    static void invalidate();
    static const SeaState& seaState();
    static const Shore& shore();
};

#endif
