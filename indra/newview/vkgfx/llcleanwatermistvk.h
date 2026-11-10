/** @file llcleanwatermistvk.h
 * WATER MIST (#963): fog collecting over lakes, sea and rivers, as a density term of the ONE camera
 * medium (#1266; the shader side is clean_medium.glsl cmWaterMistTau). This module owns the CPU side:
 *
 *  - the WATER PROXIMITY FIELD: a fixed 128 x 128 grid of 8 m cells (1 km square) centred on the
 *    camera (re-centred on a 64 m lattice), each cell the water surface height (its region's) and
 *    the signed distance to the shore (+ inland, - offshore; a chamfer distance transform over the
 *    terrain-below-water test). Built from LLWorld's regions and terrain a few rows per frame (fixed
 *    cost), rebuilt when the camera crosses a lattice cell and every kRefreshSecs (terrain edits,
 *    regions arriving); packed half2 per cell into one of two host-visible buffers the shaders read
 *    by device address (ssr.mist_field): the other buffer is only rewritten frames after it was
 *    last published, so no frame in flight reads a cell being written. Mesh water is not in it.
 *    WHY NOT THE SEA FIELD (#1020, SLSeaState: 128 x 128 x 4 m = 512 m, depth / shore / fetch / dh):
 *    the same kind of data, a different window. The mist is seen along rays out to the haze's
 *    reach, so it needs the 1 km square; the waves need 4 m texels for their shoaling and breakers.
 *    One field serving both would either halve the mist's reach (mist over land past 256 m: the
 *    sea field's outside is "open deep sea") or halve the waves' resolution, or cost 4x the
 *    terrain samples of either (256 x 256 x 4 m). Sharing only the overlapping height samples
 *    (a quarter of the mist's, when both windows happen to rebuild together) is not worth
 *    coupling their schedules. Both builds are fixed cost and idle while the camera stays put.
 *  - the WEATHER / TIME-OF-DAY DRIVE: the mist's extinction, scale height, inland reach, breakup,
 *    roll and the storm spray from the windlight sun (cool low sun: thick; high clear sun burns it
 *    off), the weather's humidity (rain, snow, fog, cloud), the wind (thins it, rolls it into banks
 *    and moves them) and the storm (spray over the surf). The wind / storm are THE SEA STATE's (#1020,
 *    slseastate.h; mistSeaState reads SLSeaState).
 *
 * Off (all zeros: the shaders skip it at one uniform test) while RenderVKVolumetricFog is 0 or
 * RenderVKWaterMist is 0; the buffers are freed after kIdleFreeFrames off. The camera medium's own
 * guardrails (start distance, max opacity; Photo Studio's override) bound it like the haze.
 */
#ifndef LL_LLCLEANWATERMISTVK_H
#define LL_LLCLEANWATERMISTVK_H

#include "vk_device.h"

#include <cstdint>
#include <vector>

class LLCleanWaterMistVK
{
public:
    // What the mist takes from THE SEA STATE (#1020, SLSeaState; see mistSeaState).
    struct SeaState
    {
        float wind[2] = {0.f, 0.f};   // mean wind velocity (m/s, region axes: x east, y north; where it blows TO)
        float speed = 0.f;            // |wind| (m/s)
        float storm = 0.f;            // 0 calm .. 1 storm (Beaufort ~7 .. 10 and the weather's storm amount)
    };
    static SeaState mistSeaState();

    LLCleanWaterMistVK() = default;
    ~LLCleanWaterMistVK() = default;   // destroy() is explicit (needs the device)
    LLCleanWaterMistVK(const LLCleanWaterMistVK&) = delete;
    LLCleanWaterMistVK& operator=(const LLCleanWaterMistVK&) = delete;

    // Once per frame (repeated calls in one frame are no-ops): the settings, the drive, a few rows of
    // the field. camAgent: the camera (agent m); waterHeight: the camera region's water (m);
    // fogOn: the volumetric fog runs this frame.
    void update(VkDeviceContext* ctx, const float camAgent[3], float waterHeight, bool fogOn);
    // The frame-info block (VKSSRInfo mist_field / water_mist = clean_frame_info.glsl): zeros when off.
    void fillFrameInfo(uint32_t field[4], float mist[4][4]) const;
    void destroy(VkDeviceContext* ctx);

    static constexpr uint32_t kCells = 128;            // per side
    static constexpr float    kCellSize = 8.f;         // m
    static constexpr float    kSnap = 64.f;            // the re-centring lattice (m)
    static constexpr uint32_t kRowsPerFrame = 8;       // the build's fixed per-frame cost (1024 terrain samples)
    static constexpr double   kRefreshSecs = 20.0;
    static constexpr uint32_t kIdleFreeFrames = 600;   // off this long: the buffers are freed
    static constexpr uint32_t kRetryFrames = 300;      // after an allocation failure

private:
    bool ensureBuffers(VkDeviceContext* ctx);
    void stepBuild(const float camAgent[3], float waterHeight);
    void finishBuild();
    void drive(float dt);

    GpuBuffer mBuf[2];
    uint64_t  mAddr[2] = {0, 0};
    int       mPublished = -1;          // which buffer the shaders read (-1 none)
    uint32_t  mPublishFrame = 0;
    double    mOrigin[2] = {0.0, 0.0};  // this frame's agent-frame origin (global m)
    double    mCorner[2] = {0.0, 0.0};  // the published field's corner (global m)
    float     mTopWater = 0.f;          // its highest water surface (m)
    // The build in progress (rows mRow .. kCells-1 left; mRow == kCells: idle).
    uint32_t  mRow = kCells;
    double    mBuildCorner[2] = {0.0, 0.0};
    double    mBuildStarted = -1e9;
    std::vector<float> mLand, mWater, mDist;
    std::vector<uint8_t> mIsWater;

    uint32_t  mFrame = 0;
    uint32_t  mLastFrameCount = ~0u;
    uint32_t  mOffFrames = 0;
    uint32_t  mRetryAt = 0;
    bool      mOn = false;
    bool      mWisps = true;
    // The drive (eased) and the noise drift (m, wrapped to the noise tile).
    float     mSigma = 0.f, mHeight = 3.f, mReach = 30.f, mBreakup = 0.25f, mRoll = 2.f;
    float     mSpray = 0.f, mSprayHeight = 2.f;
    bool      mEased = false;
    double    mDrift[2][2] = {{0.0, 0.0}, {0.0, 0.0}};   // banks, wisps
};

#endif
