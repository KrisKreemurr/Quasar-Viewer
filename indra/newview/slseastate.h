/**
 * @file slseastate.h
 * @brief THE SEA STATE (#1020): one shared description of the water surface, derived on the CPU
 *        once per frame from the weather (wind, storm, rain) and the region's water settings.
 *        The water surface (waves), the foam / interaction and the surf sounds all read it.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * (LGPL v2.1, see slweather.h)
 * $/LicenseInfo$
 *
 * INPUTS (update(), once per frame; repeated calls in one frame are no-ops)
 *   SLWeather::state().windMean   the slow mean wind (no gusts): the sea builds from it
 *   SLWeather::state().wind       the gusty wind: only the short chop (detail normals) follows it
 *   SLWeather::target()           storm (lightning amount), rain, active
 *   LLSettingsWater               wave1Dir: the calm-day breeze and its direction when no weather
 *                                 source is active (a gentle sea, as LL's scrolling normal maps)
 *   region terrain (LLSurface)    the SEA FIELD below
 * Everything is eased (seas build over ~25 s, turn over ~40 s, storms over ~20 s), so a weather
 * change never pops the surface. Wave phases advance with the synced UTC clock
 * (SLWeather::syncedSeconds) and are ANCHORED AT THE AGENT REGION'S ORIGIN (agent xy 0), carried
 * across a region crossing so the pattern is continuous there. Never at the global origin: a
 * phase k . O_global (|O| ~ 3e5 m on the mainland) turned every tiny change of the wave direction
 * -- the easing, a float ulp -- into tens of radians per frame: the whole sea jittered (#1020).
 *
 * THE WAVE SET: kWaves Gerstner components on a FIXED wavelength ladder (kLambda: 80 .. 4 m).
 * Only their amplitudes (a Pierson-Moskowitz spectrum of the eased wind, the visible height
 * compressed to kHsCap) and their directions (spread around the eased wind) change with the
 * weather -- never a wavenumber, so a change never stretches the pattern far from the camera.
 * Steepness Q per wave: sum(Q k A) = the chop (0.35 calm .. 0.85 storm), loop free.
 *
 * THE SEA FIELD: a kFieldRes x kFieldRes window (kFieldTexel m texels, ~512 m) of what the water
 * knows about the place under it, around the camera, rebuilt a slice per frame when the camera
 * leaves its centre, the wind turns or every kFieldMaxAge s (fixed cost: kFieldRowsPerFrame rows a
 * frame, ~0.1 ms). Per texel, four halves:
 *   depth   water height - ground height (m; < 0 on land). Terrain only (prims are the
 *           water FS's per-pixel depth's business).
 *   shore   signed distance to the shoreline (m; + offshore, - inland; chamfer distance)
 *   fetch   open water upwind (m; kFetchOpen = open sea): enclosed ponds keep only the
 *           short waves, a lagoon behind a reef the middle ones
 *   dh      this texel's region water height - the agent region's (m; 0 almost always)
 * Outside the window: open, deep sea at the agent region's height.
 * THE SEA FLOOR (#1020, 2026-11-06): the depth is not the terrain alone -- SL terrain is often left
 * at its minimum with mesh terrain on top. Each build asks the renderer for a top-down depth pass
 * over the same window (floorRequest(); LLCleanRendererVK::recordSeaFloor: an orthographic view
 * from 0.1 m under the water down 100 m, the static opaque / alpha-mask world through the GPU cull
 * -- no avatars, attachments, moving or physical objects, alpha-blend or water surfaces -- at 1 m,
 * read back asynchronously and reduced to the field's 4 m by slsea::reduceFloor) and waits up to
 * kFloorWait s for it (takeFloor()). Where it found a floor above the terrain, that is the floor;
 * elsewhere (nothing found, the pass unavailable, a region without terrain) the terrain as before.
 * A static object changing under the water in the window (noteObjectChange, from the ingest's
 * change events -- no per-frame tracker) makes the field due again.
 *
 * EVENTS: when the swash clock wraps (a breaker reaches the waterline) update() posts
 * SLWaterEvents::BREAK at the shore point nearest the camera (waves on, a shore within 150 m):
 * the sounds' breaker crashes follow the surface's.
 *
 * GPU INTERFACE (clean_sea_state.glsl is the reader; LLCleanRendererVK fills it):
 *   frame info ssr.sea[7], ssr.sea_wave[2 * kWaves] (clean_frame_info.glsl; floats below)
 *   draw set 0 binding 67: the field, std430 uvec2 per texel (x = half2(depth, shore),
 *   y = half2(fetch, dh)), row-major from the field origin, at texel offset ssr.sea[4].z.
 */
#ifndef SL_SEASTATE_H
#define SL_SEASTATE_H

#include "stdtypes.h"

#include <vector>

class SLSeaState
{
public:
    static constexpr U32 kWaves = 8;                 // Gerstner components (= clean_sea_state.glsl SEA_WAVES)
    static constexpr U32 kFieldRes = 128;            // sea field texels per side
    static constexpr F32 kFieldTexel = 4.f;          // m per texel
    static constexpr F32 kFetchOpen = 4000.f;        // fetch of the open sea (m)
    static constexpr F32 kHsCap = 2.5f;              // the visible significant height saturates here (m)
    static constexpr U32 kFrameFloats = 7 * 4;       // ssr.sea
    static constexpr U32 kWaveFloats = 2 * kWaves * 4;   // ssr.sea_wave

    // Quality: 0 = flat water (the old surface exactly), 1 = waves (low: coarser grid),
    // 2 = waves (high). From RenderVKWaterWaves (Persist 1; the featuretable sets it per preset).
    enum EMode : U32 { MODE_FLAT = 0, MODE_LOW = 1, MODE_HIGH = 2 };

    // One Gerstner component (region axes, agent space; theta = k . xy + phase).
    struct Wave
    {
        F32 kvec[2] = {0.f, 0.f};    // wave vector (rad / m), along the travel direction
        F32 k = 0.f;                 // |kvec|
        F32 amp = 0.f;               // amplitude (m) on deep, open water
        F32 steep = 0.f;             // Gerstner Q (0..1)
        F32 omega = 0.f;             // rad / s (deep water dispersion)
        F32 phase = 0.f;             // rad, wrapped: global origin + time folded in (double on the CPU)
    };

    // What everyone reads. Eased; zeros in flat mode except the wind / storm (sounds still want them).
    struct State
    {
        U32 mode = MODE_FLAT;
        F32 windSpeed = 0.f;         // eased mean wind (m/s) the sea is built from
        F32 windDir[2] = {1.f, 0.f}; // unit, the direction the waves TRAVEL (region axes)
        F32 gust = 0.f;              // gusty wind speed now (m/s), lightly eased (chop, sounds)
        F32 storm = 0.f;             // 0..1: lightning, rain and gales combined
        F32 beaufort = 0.f;          // 0..12, continuous, of windSpeed
        F32 hs = 0.f;                // visible significant wave height of the open sea (m)
        F32 peakLambda = 0.f;        // the spectrum's peak wavelength (m)
        F32 chop = 0.f;              // 0.35..0.85: sum(Q k A), crest sharpness
        F32 detail = 1.f;            // detail-normal (LL normal map) strength, 1 = LL's look
        F32 surf = 0.f;              // 0..1 breaker strength at a beach (lapping .. crashing)
        F32 runup = 0.f;             // m: how high the swash climbs above the water line
        F32 swashPeriod = 8.f;       // s between breakers at the shore
        F32 swashPhase = 0.f;        // 0..1, advances 1 per swashPeriod
        F32 swashLen = 20.f;         // m: breaker spacing toward the shore
        Wave waves[kWaves];
    };

    // One field texel (see the header comment).
    struct FieldTexel
    {
        F32 depth = 100.f, shore = 1000.f, fetch = kFetchOpen, dh = 0.f;
    };

    static SLSeaState& instance();

    // Once per frame, before the renderer's frame is filled (llappviewer.cpp).
    void update();
    const State& state() const { return mState; }

    // The water surface's height above the region's water plane (m) at agent xy now: the
    // same waves, shoaling and swash as the GPU (clean_sea_state.glsl seaSurface), without the
    // horizontal displacement (the crest a fixed point sees, close enough for the camera's
    // under-water test, a bob or a sound). 0 in flat mode.
    F32 surfaceHeightAt(F32 x, F32 y) const;
    // The field at agent xy (bilinear; outside the window: open deep sea).
    FieldTexel fieldAt(F32 x, F32 y) const;
    // Breaking / crest / swash 0..1 at agent xy (CPU twin of seaSurface's masks, for the sounds).
    void surfAt(F32 x, F32 y, F32& breaking, F32& swash) const;
    // The shore breakers' strength 0..1 at agent xy (slsea::boreStrength: the surf x the local,
    // fetch-limited wave energy x how squarely the shore faces the waves).
    F32 boreStrengthAt(F32 x, F32 y) const;

    // Renderer hand-off: ssr.sea / ssr.sea_wave floats (sea[4].y / .z and the grid row are the
    // renderer's: field valid / offset), and the published field (kFieldRes^2 x 2 uint, packed
    // halves) with its version (bumped on every publish) and origin.
    void fillFrameInfo(F32 sea[kFrameFloats], F32 seaWave[kWaveFloats]) const;
    const U32* fieldData() const { return mFieldPacked.empty() ? nullptr : mFieldPacked.data(); }
    U32 fieldVersion() const { return mFieldVersion; }
    const F32* fieldOrigin() const { return mFieldOrigin; }

    // THE SEA FLOOR hand-off (see the header comment). serial 0 = no request.
    struct FloorRequest
    {
        U32 serial = 0;
        F32 origin[2] = {0.f, 0.f};   // the window's texel (0, 0) corner (agent m, the agent region's frame)
        F32 extent = 0.f;             // m (kFieldRes x kFieldTexel)
        F32 top = 0.f, span = 0.f;    // the pass's near plane height (m, agent z) and depth range (m)
    };
    static constexpr U32 kFloorRes = 512;            // the pass's texels per side (1 m)
    static constexpr F32 kFloorSkip = 0.1f;          // m under the water surface before anything counts
    static constexpr F32 kFloorSpan = 100.f;         // m of depth the pass looks down
    static constexpr F32 kFloorWait = 1.5f;          // s a build waits for the pass before the terrain alone
    // The pending request while a build waits for it (serial 0 = none wanted now).
    const FloorRequest& floorRequest() const { return mFloorWanted ? mFloorReq : mFloorNone; }
    // The renderer's answer: kFieldRes^2 floor heights (agent z; slsea::kFloorNone = nothing).
    void takeFloor(U32 serial, const F32* floor);
    // A static object changed at agent (x, y, z), radius r (the ingest's change events,
    // llcleaningest.cpp noteChangedSphere): under the water inside the field's window -> due.
    void noteObjectChange(F32 x, F32 y, F32 z, F32 r);
    bool floorWindowContains(F32 x, F32 y, F32 r) const;

private:
    SLSeaState() = default;
    void stepState(F32 dt);
    void buildWaves(F64 syncNow);
    void stepField();
    void publishField();
    void postBreak() const;   // SLWaterEvents::BREAK when the swash clock wraps (the sounds' breakers)

    State mState;
    U64   mLastFrame = ~0ull;
    F64   mLastTime = 0.0;
    bool  mSeeded = false;
    F32   mCalmDir[2] = {1.f, 0.f};
    // The phase anchor (the agent region's global origin) and each wave's phase carried across
    // anchor moves (rad, double): theta = k . xy_agent + carry - w t + phase0.
    F64   mAnchor[2] = {0.0, 0.0};
    F64   mPhaseCarry[kWaves] = {};
    bool  mAnchorSet = false;

    // ---- the field (published + the one being built) ----
    std::vector<FieldTexel> mField;        // published (CPU reads: surfaceHeightAt)
    std::vector<FieldTexel> mBuild;        // being built
    std::vector<U32>        mFieldPacked;  // published, packed for the GPU
    F32  mFieldOrigin[2] = {0.f, 0.f};     // published texel (0, 0)'s corner (agent m)
    F32  mBuildOrigin[2] = {0.f, 0.f};
    F32  mBuildWindDir[2] = {1.f, 0.f};
    F32  mFieldWindDir[2] = {1.f, 0.f};
    F64  mBuildRegionOrigin[2] = {0.0, 0.0};   // agent region's global origin the build was made in
    F64  mFieldRegionOrigin[2] = {0.0, 0.0};
    F64  mFieldTime = -1e9;                // when the published field was started
    U32  mBuildStage = 0;                  // 0 idle, 1 heights (+ the floor), 2 fetch rows
    U32  mBuildRow = 0;
    U32  mFieldVersion = 0;
    bool mFieldValid = false;
    bool mBuildUnknown = false;            // the build met a region with no terrain yet
    // ---- the sea floor ----
    FloorRequest    mFloorReq, mFloorNone;
    bool            mFloorWanted = false;      // the build is waiting for mFloorReq's answer
    bool            mFloorHave = false;        // mFloor holds the answer to mFloorReq
    bool            mFloorDirty = false;       // a static object changed in the window
    U32             mFloorSerial = 0;
    F64             mFloorAsked = 0.0;
    std::vector<F32> mFloor;                   // kFieldRes^2 floor heights of the answer
    std::vector<U8>  mBuildKnown;              // per texel: a region with terrain (the floor applies)
};

#endif // SL_SEASTATE_H
