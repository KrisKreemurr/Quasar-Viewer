/**
 * @file llcleanalphakvk.h
 * @brief Alpha-blend cover lists (#1147) for the Vulkan clean renderer: the per-pixel lists of
 *        the last-composited blended fragments that let pass 2 skip lighting the layers they
 *        cover (clean_alpha_k.glsl has the shader contract).
 *
 * Every alpha-blend layer was fully lit however much of it the layers composited over it hide:
 * stacked hair cards, the "alpha blending performance grinder" shells. Per frame:
 *
 *   prepare()        before the frame info is filled: the pipelines (built in the background
 *                    on first use), the list buffer sized for the render extent -- K entries
 *                    per pixel and water side, K tiered by the VRAM budget (kMaxK..2; off when
 *                    even 2 does not fit), allocated soft, skipped under VRAM pressure, a failure
 *                    retried later and given up after a few (logged) -- and whether the lists are
 *                    built this frame. fillFrameInfo() then writes ssr.alpha_k (zeros = none).
 *   recordClear()    outside any rendering scope, before pass 2: the tiles the previous lists
 *                    touched are zeroed (the whole buffer once after an allocation).
 *   (the renderer)   one scope without attachments over pass 2's four alpha lists with
 *                    visPipe(): the same VS, pad0 words and shading rate (recordAlphaKLists).
 *   recordListsDone() the list stores -> pass 2's alpha FS reads.
 *
 * AUTO SWITCH (RenderVKAlphaKCull 2, the default): the list pass and pass 2 count, one atomic per
 * subgroup, the list pass's fragments (its cost) and the pass-2 fragments it dropped (the lighting
 * saved); read back 3 frames late, over 30-frame windows. Fewer than kAutoKeep dropped per listed
 * fragment -- below that the list pass costs more than it saves (the owner's grinder timings: a
 * listed fragment ~0.35 of a lit one) -- switches the lists off for kAutoOffFrames, then they
 * are tried again for one window. A scene without stacked blended layers pays only those probes.
 * 1 = always on, 0 = off. A "VKAlphaK" line every ~10 s (on / auto decisions at once) gives the
 * counts: the owner's in-world numbers.
 *
 * NEAR-OPAQUE COVERS (RenderVKAlphaCoverCull, clean_alpha_k.glsl NEAR-OPAQUE COVERS): the cheap
 * case, no atomics -- an R32_UINT target per water side that the ROP leaves holding the LAST
 * fragment of alpha >= 0.98 in pass 2's order (the grinder: one draw of 252/255 planes, last
 * plane wins), copied into a buffer pass 2 reads. beginCover / endCover around the renderer's
 * draws (coverPipe), recordCoverDone after both sides. Independent of the lists (no 64-bit
 * atomics needed); the lists' auto switch credits only the drops the covers alone do not make.
 *
 * Fixed cost per frame: one cheap raster of the blended draws (VS + base alpha), at most one
 * 64-bit atomic per recorded fragment and covered pixel (its bucket's slot), a clear proportional to the blended area. Memory: rw x rh x 2 x K x 8 B +
 * one word per 8 x 8 tile, freed after ~10 s unused. Main view only: not in stereo, offscreen
 * snapshots, the transparent-avatar layer or a lit debug view (pass 2 shades every layer there,
 * as before).
 */
#ifndef LL_LLCLEANALPHAKVK_H
#define LL_LLCLEANALPHAKVK_H

#include "vk_device.h"
#include "vk_pipeline.h"

#include <cstdint>

class LLCleanAlphaKVK
{
public:
    struct Frame
    {
        VkDeviceContext* ctx = nullptr;
        VkPipelineLayout drawLayout = VK_NULL_HANDLE;   // the world draw layout (sets 0 / 1, the 16 B push)
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;     // the main depth (the list scope reads it, early test)
        bool reverseZ = true;                           // the main view's depth convention
        uint32_t rw = 0, rh = 0;                        // render extent
        bool want = false;                              // RenderVKAlphaKCull != 0 and this frame qualifies
        uint32_t mode = 2;                              // RenderVKAlphaKCull: 1 always, 2 auto
        bool lists = false;                             // the device has 64-bit buffer atomics (the list pass)
        bool wantCover = false;                         // RenderVKAlphaCoverCull and this frame qualifies
        uint32_t coverDebug = 0;                        // RenderVKAlphaCoverDebug (the bisect bits, clean_alpha_k.glsl AKD_*)
    };
    static constexpr uint32_t kMaxK = 4;   // = clean_alpha_k.glsl AK_KMAX

    LLCleanAlphaKVK() = default;
    ~LLCleanAlphaKVK() = default;   // destroy() is explicit (needs the device)
    LLCleanAlphaKVK(const LLCleanAlphaKVK&) = delete;
    LLCleanAlphaKVK& operator=(const LLCleanAlphaKVK&) = delete;

    // true = build the lists this frame (pipelines in, buffer sized); coverNow() the same for the
    // covers. Call every frame.
    bool prepare(const Frame& f);
    bool coverNow() const { return mCoverNow; }
    void fillFrameInfo(uint32_t out[4]) const;
    void fillCoverInfo(uint32_t out[4]) const;
    // One water side's cover scope (side 0 pre-water, 1 post-water): its target cleared to 0,
    // the main depth read-only (depthView, DEPTH_READ_ONLY_OPTIMAL) for the early test.
    void beginCover(VkCommandBuffer cmd, uint32_t side, VkImageView depthView);
    void endCover(VkCommandBuffer cmd) { vkCmdEndRendering(cmd); }
    // Both sides' targets -> the buffer pass 2 reads (and last frame's reads of it -> the copy).
    void recordCoverDone(VkCommandBuffer cmd);
    VkPipeline coverPipe(int rigged) const { return mCoverPipe[rigged ? 1 : 0]; }
    void recordClear(VkCommandBuffer cmd);
    void recordListsDone(VkCommandBuffer cmd);
    VkPipeline visPipe(int rigged) const { return mVisPipe[rigged ? 1 : 0]; }
    // Nothing allocated or compiling (the owner may delete it).
    bool idle() const { return !mBuf.buffer && !mCoverBuf.buffer && !mJob.pending(); }
    void destroy(VkDevice dev);

private:
    bool ensurePipelines(const Frame& f);
    bool prepareLists(const Frame& f, bool want);
    bool prepareCovers(const Frame& f);
    void freeBuffer();
    void freeCovers();
    void autoSample(uint32_t listed, uint32_t dropped);
    void coverSample(const uint32_t c[8]);

    static constexpr double   kAutoKeep = 0.4;         // dropped / listed fragments to stay on
    static constexpr uint32_t kAutoWindow = 30;        // frames per decision
    static constexpr uint32_t kAutoOffFrames = 300;    // off this long before the next try

    VkDeviceContext* mCtx = nullptr;
    PipelineJobSlot  mJob;
    VkPipeline       mStage[5] = {};                 // the background job's outputs
    VkPipeline       mVisPipe[2] = {};               // the list pass: static, rigged
    VkPipeline       mCoverPipe[2] = {};             // the cover pass: static, rigged
    bool             mBuiltLists = false;            // the list pipelines were built (64-bit atomics)
    VkPipeline       mClearPipe = VK_NULL_HANDLE;
    VkPipelineLayout mClearLayout = VK_NULL_HANDLE;
    VkPipelineLayout mBuiltLayout = VK_NULL_HANDLE;  // the draw layout the list pipelines were built for
    VkFormat         mBuiltDepth = VK_FORMAT_UNDEFINED;   // ... and the depth format
    bool             mPipeFailed = false;

    GpuBuffer        mBuf;
    VkDeviceAddress  mAddr = 0;
    uint32_t         mW = 0, mH = 0, mK = 0;
    bool             mFresh = false;                 // allocated since the last clear: fill it whole
    bool             mNow = false;                   // prepare() said yes this frame
    uint32_t         mIdleFrames = 0;
    uint32_t         mRetryIn = 0;                   // frames until the next allocation attempt
    uint32_t         mFailures = 0;                  // consecutive failed attempts (gives up at 3)
    uint32_t         mFailW = 0, mFailH = 0;         // ... for this extent
    // Auto switch + cover check: the counters' readback ring (3 x 48 B, host-visible: the lists'
    // 16 B, then the covers' 32 B), its frame counter and which entries hold a copy; the current
    // window; frames left switched off; the last mode.
    GpuBuffer        mRb;
    uint32_t         mRbFrame = 0;
    bool             mRbValid[3] = {false, false, false};
    bool             mCoverRbValid[3] = {false, false, false};
    // COVER SELF-CHECK: pass 2 counts the fragments the covers dropped and those it lit while the
    // covers were tested. Every pixel's last cover is itself tested and lit, so "dropped, none
    // lit" over a window means the cover keys disagree with pass 2's: covers off for the session,
    // logged (a broken build must not erase blended objects).
    uint64_t         mCovWinDropped = 0, mCovWinLit = 0, mCovWinCls[4] = {};
    uint32_t         mCoverDebug = 0;                 // RenderVKAlphaCoverDebug this frame
    uint32_t         mCovWinFrames = 0, mCovWindows = 0;
    bool             mCoverBroken = false;
    bool             mCoverCountsValid = false;   // the buffer's counters were zeroed once (not garbage)
    uint64_t         mWinListed = 0, mWinDropped = 0;
    uint32_t         mWinFrames = 0, mWindows = 0;
    uint32_t         mAutoOffFrames = 0;
    uint32_t         mMode = 2;
    // Near-opaque covers: a target per water side (R32_UINT), the buffer they are copied to.
    GpuImage         mCoverImg[2];
    GpuBuffer        mCoverBuf;
    VkDeviceAddress  mCoverAddr = 0;
    uint32_t         mCW = 0, mCH = 0;
    bool             mCoverNow = false;
    uint32_t         mCoverIdle = 0, mCoverRetryIn = 0, mCoverFailures = 0, mCoverFailW = 0, mCoverFailH = 0;
};

#endif
