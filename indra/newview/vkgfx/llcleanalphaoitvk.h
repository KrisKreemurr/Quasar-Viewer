/**
 * @file llcleanalphaoitvk.h
 * @brief Weighted blended order-independent transparency (#1410, RenderVKAlphaOIT) for the
 *        Vulkan clean renderer's expensive alpha-blended content (rigged avatar alpha: hair).
 *
 * McGuire & Bavoil 2013 ("Weighted Blended Order-Independent Transparency", JCGT 2(2)): the OIT
 * group's fragments add w * a * value into additive targets and multiply a revealage target by
 * (1 - a); a fullscreen resolve divides and composites value * (1 - revealage) over the scene at
 * the group's place in the pass-2 blend order. Weight and targets: clean_oit.glsl.
 *
 *   mode 1  the accumulation FS is the ordered pass's lit blend FS, weighted (per-fragment
 *           lighting; isolates the OIT composite): RGBA16F colour + RGBA16F glow + R16F
 *           revealage = 18 B / px.
 *   mode 2  the accumulation FS writes the decoded surface UNLIT (albedo, normal + view depth,
 *           emission + glow, material) and the resolve lights the weighted mean surface ONCE per
 *           pixel (clean_oit_resolve.frag -DOIT_LIT): 4 x RGBA16F + R16F = 34 B / px.
 *
 *   ensurePipelines()  queues the accumulation (rigged / static, plain / ray-query) and resolve
 *                      pipelines as ONE background job on first use and whenever the live lit
 *                      specialization (spec key) or the pass formats change; false until the first
 *                      set lands. A key change keeps the built set live until its successor lands
 *                      (the generic key keeps it outright: it reads the live values).
 *   ensureTargets()    (re)creates the mode's targets at render size (soft allocation: a failure
 *                      returns false -- the ordered path that frame -- logged, retried in 5 s).
 *   beginAccum()       a rendering scope on the targets (sums cleared to 0, revealage to 1), the
 *                      scene depth read-only; the renderer draws the OIT group with accumPipe().
 *   endAccum()         ends it and releases the targets to the resolve's reads.
 *   recordResolve()    inside a pass-2 scope: binds the resolve pipeline + set 2 and draws.
 *
 * Used once per water side per frame (2a pre-water, 2b post-water; the targets are reused). Not
 * in stereo, offscreen snapshots or debug views (the renderer keeps the ordered path there).
 */
#ifndef LL_LLCLEANALPHAOITVK_H
#define LL_LLCLEANALPHAOITVK_H

#include "vk_device.h"
#include "vk_descpool.h"
#include "vk_pipeline.h"

#include <cstdint>

class LLCleanAlphaOITVK
{
public:
    // Pipeline inputs: the live ordered pipelines' state (LIT_RIG_ALPHA / LIT_ALPHA, recorded at
    // init with layout, vertex input, depth and VRS state; modules and spec are replaced here).
    struct Build
    {
        VkDeviceContext* ctx = nullptr;
        GraphicsPipelineDesc rigDesc[2];    // [0] plain (mDrawLayout), [1] ray-query (mDrawLayoutRt)
        GraphicsPipelineDesc statDesc[2];
        bool rt = false;                    // the [1] descs are valid
        VkDescriptorSetLayout dslDraw0 = VK_NULL_HANDLE, dslDraw1 = VK_NULL_HANDLE;
        VkDescriptorSetLayout dslRt = VK_NULL_HANDLE;   // the RT consumer set (set 3 of the mode-2 ray-query resolve)
        uint32_t pushBytes = 16;            // the draw family's VERTEX | FRAGMENT push range
        const VkSpecializationInfo* spec = nullptr;   // the lit FS's live specialization
        uint64_t specKey = 0;
        bool generic = false;               // no setting-specialization set is bound (spec = the generic one)
        VkFormat hdrFormat = VK_FORMAT_UNDEFINED, glowFormat = VK_FORMAT_UNDEFINED;
        VkFormat reactFormat = VK_FORMAT_UNDEFINED, depthFormat = VK_FORMAT_UNDEFINED;
    };

    static constexpr VkFormat kSumFormat    = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kRevealFormat = VK_FORMAT_R16_SFLOAT;

    LLCleanAlphaOITVK() = default;
    ~LLCleanAlphaOITVK() = default;   // destroy() is explicit (needs the device)
    LLCleanAlphaOITVK(const LLCleanAlphaOITVK&) = delete;
    LLCleanAlphaOITVK& operator=(const LLCleanAlphaOITVK&) = delete;

    bool ensurePipelines(const Build& b);
    bool ensureTargets(VkDeviceContext* ctx, uint32_t mode, uint32_t rw, uint32_t rh);
    // mode 1 / 2; rigged: the rigged VS (else the static one, force mode); rt: the ray-query
    // lit FS (mode 1 in a tracing frame; mode 2 never traces).
    VkPipeline accumPipe(uint32_t mode, bool rigged, bool rt) const;
    void beginAccum(VkCommandBuffer cmd, VkImageView depthView);
    void endAccum(VkCommandBuffer cmd);
    // Binds the resolve pipeline + set 2 (layout compatible with the draw layout for sets 0 / 1
    // and the push range; set 2 of a ray-query draw layout must be rebound after) and draws.
    // rtSet (a tracing frame): mode 2 takes its ray-query twin, the RT set at set 3.
    void recordResolve(VkCommandBuffer cmd, VkDescriptorSet rtSet);
    // Outside any scope, once per frame before the first accumulation: the device-local
    // covered-pixel counter -> the host readback (no host-memory atomics in the timed resolve).
    void recordStatsCopy(VkCommandBuffer cmd);
    // Covered pixels the resolves counted since the last call (both water sides, every frame).
    uint64_t takeCoveredPixels();
    uint64_t targetBytes() const;
    uint32_t mode() const { return mMode; }
    void freeTargets();
    void destroy(VkDevice dev);

private:
    void destroyPipelines(VkDevice dev);
    void destroyStage(VkDevice dev);
    void writeSet();

    VkDeviceContext*      mCtx = nullptr;
    VkDescriptorSetLayout mDsl = VK_NULL_HANDLE;   // 0-4 the targets (sampled), 5 the stats counter
    VkDescPool            mPool;
    VkDescriptorSet       mDset = VK_NULL_HANDLE;
    VkPipelineLayout      mResolveLayout = VK_NULL_HANDLE;
    VkPipelineLayout      mResolveLayoutRt = VK_NULL_HANDLE;   // + the RT consumer set at 3

    // [mode - 1][rigged][rt]: accumulation; [mode - 1]: resolve. The stage arrays are what the
    // background job writes (installed when it is done).
    VkPipeline mAccum[2][2][2] = {};
    VkPipeline mResolve[2] = {};
    VkPipeline mResolveRt = VK_NULL_HANDLE;        // mode 2's ray-query twin (one sun ray per covered pixel)
    VkPipeline mResolveRtStage = VK_NULL_HANDLE;
    VkPipeline mAccumStage[2][2][2] = {};
    VkPipeline mResolveStage[2] = {};
    PipelineJobSlot mJob;
    bool       mReady = false;
    uint64_t   mBuiltKey = ~0ull;
    VkFormat   mBuiltFormats[4] = {};
    double     mJobSince = 0.0;

    // Targets: [0] colour | albedo sum, [1] glow | normal + depth, [2] emission + glow, [3]
    // material (mode 2 only), [4] the revealage.
    GpuImage   mTarget[5];
    uint32_t   mMode = 0, mW = 0, mH = 0;
    double     mRetryAt = 0.0;
    GpuBuffer  mStats;          // device-local: the resolves' covered-pixel counter (running total)
    GpuBuffer  mStatsRb;        // host-visible copy of it (recordStatsCopy)
    uint32_t   mStatsLast = 0;
    bool       mStatsZeroed = false;
};

#endif
