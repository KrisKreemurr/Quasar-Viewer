/**
 * @file llcleanvisqueuevk.cpp
 * @brief THE VISIBILITY QUEUE (#1039): the host side. The shader side and the data contract are in
 *        indra/vkclean/shaders/clean_vis_queue.glsl.
 *
 * WHAT IT DOES: the RT hit shading's rays leave the lighting kernels. The hit-shading splits of #1036 run, per
 * consumer, material stage -> lighting stage, and the lighting stage walked the hit's rays (the sun / moon ray with
 * its translucent and foliage tails, the hit shadow-ray budget's projector / point-light rays, the RT reflections'
 * occlusion ray) with the whole lit surface live around it: its register peak. With the queue (M2):
 *   MAT (the material only: the RT LIT RECORD) ->
 *   REQ (the producer: reads the record back, runs the local-light walk with every share unshadowed -- the sum and
 *        the budget's picks into result slots -- and queues the sun ray, the budget's K light rays and, for the RT
 *        reflections, the occlusion ray as 48 B requests, or a constant result) ->
 *   WALK (one lane per request, nothing but the walk: DeferVisWalk; each light ray leaves its pick's blocked part)
 *        [+ the RT reflections' LAYERS beside it: the blend-layer gather with its per-layer sun rays] ->
 *   LIGHT (the resolve: the sun, the lights' sum minus the blocked parts, the occlusion, the layers' composite
 *        from the results -- the kernel holds no ray query).
 * The walk runs the same functions on the same rays and cones, so the image is bit-identical to the fused
 * kernels'. MAT and REQ are separate stages (one barrier between): fused, the two working sets cost 165 / 149
 * registers (12 warps / SM); split, each stage is 128 (16).
 *
 * CONSUMERS:
 *   * THE GI HITS (recordGI, with RenderVKGIShadeSplit): always through the queue -- HIT-MAT (the fused stage's
 *     kernel: the material is the same), HIT-REQ (DeferGiSplitHitReqVq2), the walk, HIT-LIGHT
 *     (DeferGiSplitHitLightVq2). RenderVKVisQueue bit 64 (A/B, same image) keeps them on M1 instead: HIT-MAT
 *     (DeferGiSplitHitMatVq) queues only the sun ray, HIT-LIGHT (DeferGiSplitHitLightVq) keeps the light walk
 *     and the budget's rays.
 *   * THE RT REFLECTIONS (recordRtReflect, with RenderVKRTReflectionsShadeSplit): through the queue only with
 *     RenderVKVisQueue bit 2 (A/B, same image) -- MAT (DeferRtSplitMatVq2), REQ, the walk + LAYERS, LIGHT;
 *     else the fused split.
 *
 * THE FRAME WALK below: the lighting chain records every queued consumer's producers first, in shared levels,
 * then ONE walk wave over all their requests (two barriers in all instead of two per consumer, the region headers
 * in the light prep), then the resolves. A GI recorded outside the chain (the one-frame-latency GI, the early
 * trace's own segment) runs its walk and HIT-LIGHT itself.
 *
 * FRAME (every step on the consumer's own queue segment -- q_defer_light / q_gi_lag -- no new semaphore):
 *   1. visQueueDecide (recordWorld, after giDecide and deferSplitFrame's rtSplitDecide): the setting, the
 *      regions' entries (each consumer's own: the RT records' capacity, the GI texels; within the cap),
 *      the buffer (allocated first, the old one retired -- no device wait), the stats harvest;
 *   2. the region headers (counters zeroed, the walk's indirect dispatch (0, 0, 1), the capacities, the RT
 *      records' address): written by THE LIGHT PREP with the chain's other list headers (visQueuePrepHeaders, no
 *      drain in the chain); visQueueBegin (each consumer, before its producer) takes it -- or, where the prep did
 *      not write it, writes it between a barrier pair;
 *   3. the producer appends (subgroup-aggregated: one atomic per subgroup) and raises the dispatch;
 *   4. visQueueWalk: the walk, indirect over the region's requests;
 *   5. the resolve -- and, for the entries past the region's capacity, the consumer's fused lighting stage
 *      as a TAIL (push rtr3.y = the first such entry): the queue never drops a ray.
 * VRAM: 256 B per region + per entry R x 48 B requests and S x 16 B result slots -- RT (2 + K) x 48 + (4 + K) x 16
 * (K = RenderVKRTReflectionsHitShadowRays; 224 B at K 1), GI (1 + K) x 48 + (2 + K) x 16 (K =
 * RenderVKGIHitShadowRays; 144 B at K 1; M1: 64 B) -- capped by RenderVKVisQueueMB (0 = the tier of the
 * device-local heap's size: 896 MB above 24 GB, 192 MB above 12.5 GB, 64 MB above 8.5 GB, else 32 MB), the RT and
 * GI hits sharing it by demand. At 1440p (GI at half extent, the RT records at the default ray budget: ~0.92 M
 * entries each): ~133 MB GI, ~206 MB RT. Fixed by the extent / the settings: no growth.
 * THE FALLBACK (soft, never a crash): the entries past a region's capacity (the cap, shared by demand) take the
 * consumer's fused lighting stage as the TAIL; a frame with no region -- no buffer under texture pressure, a
 * refused allocation (#697: logged, retried in 30 s), the queue's pipelines still compiling -- runs the fused
 * stages for that consumer (the GI: the fused HIT-MAT + HIT-LIGHT).
 * GPU timers: c_/g_defer_rtrefl_walk and c_/g_gi_walk (the walks; + c_/g_defer_rtrefl_layers), the requests
 * under c_/g_defer_rtrefl_req and c_/g_gi_hitreq, the material / lighting marks keep their names (the lighting:
 * the resolve and its tail); c_/g_vq_begin (headers the light prep did not write -- normally none), c_/g_vq_prod
 * (the last producer up to the walk wave). Counters (GPU timers on): per region requests, constants, entries past
 * the cap -- logged every 10 s ("VK vis queue: ...").
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 * $/LicenseInfo$
 */
#include "linden_common.h"

#include "llcleanrenderervk.h"
#include "llcleandenoisevk.h"
#include "llcleanrtvk.h"

#include "llviewercontrol.h"
#include "vk_common.h"
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>

namespace
{
double vqClock()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
VkShaderModule vqModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    vkNoteShaderModule(m, code, bytes, name);
    return m;
}
// The lighting kernels' debug specialization OFF (as llcleangivk.cpp giLitSpec).
const VkSpecializationInfo* vqLitSpec()
{
    static const VkBool32 kOff = VK_FALSE;
    static const VkSpecializationMapEntry kEntry{1u, 0u, sizeof(VkBool32)};
    static const VkSpecializationInfo kSiOff{1u, &kEntry, sizeof(VkBool32), &kOff};
    return &kSiOff;
}
constexpr VkDeviceSize kVqAlign = 256u;
VkDeviceSize vqAlignUp(VkDeviceSize v) { return (v + kVqAlign - 1u) & ~(kVqAlign - 1u); }
constexpr uint32_t kVqStatBytes = 5u * 16u;   // the region header's words 0-4 (clean_vis_queue.glsl)
bool s_vqHeld = false;   // (visQueueDecide: no buffer under pressure / before a refusal's retry -- logged once per episode)
} // namespace

// The pipeline sets a RenderVKVisQueue mask asks for: bit s = set s. Set 0 always (the walk + the GI hits' M2 REQ /
// LIGHT: the default GI path); set 1 = bit 2 (the RT reflections' M2 kernels); set 2 = bit 64 (the GI hits' M1
// kernels).
static uint32_t vqSetsOf(U32 mask)
{
    return 1u | ((mask & 2u) != 0u ? 2u : 0u) | ((mask & 64u) != 0u ? 4u : 0u);
}
constexpr uint32_t kVqSets = 3u;
// After the RT layout exists (initDeferSplit's RT block, inside the world batch): the walk + the producer /
// resolve variants of the GI shade split and the RT split (distinct module names: RenderVKPipelineStats logs each
// one's registers). setMask 0: the setting's sets, at init (set 0 always); else the sets the background build of
// visQueueDecide asks for, inside its own batch.
void LLCleanRendererVK::visQueueInit(VkDevice dev, uint32_t setMask)
{
    if (!mRtReflLayout) return;
    if (setMask == 0u)
    {
        U32 mask = 0;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<U32> s_vq(gSavedSettings, "RenderVKVisQueue", 0U);
        mask = s_vq;
#endif
        setMask = vqSetsOf(mask);
    }
    struct Stage { uint32_t set; const uint32_t* code; size_t bytes; const char* mod; const char* name; VkPipeline* out; };
    const Stage st[] = {
        {7u, kWorldDeferVisWalkSpv, kWorldDeferVisWalkSpv_bytes, "DeferVisWalk", "mVisWalkPipe (DeferVisWalk)", &mVisWalkPipe},
        // set 0: the GI hits, M2 (HIT-MAT is the fused stage's kernel)
        {1u, kWorldDeferGiSplitHitReqVq2Spv, kWorldDeferGiSplitHitReqVq2Spv_bytes, "DeferGiSplitHitReqVq2",
         "mGiVqPipe (DeferGiSplitHitReqVq2)", &mGiVqPipe[1][VQS_REQ]},
        {1u, kWorldDeferGiSplitHitLightVq2Spv, kWorldDeferGiSplitHitLightVq2Spv_bytes, "DeferGiSplitHitLightVq2",
         "mGiVqPipe (DeferGiSplitHitLightVq2)", &mGiVqPipe[1][VQS_LIGHT]},
        // set 1 (bit 2): the RT reflections, M2
        {2u, kWorldDeferRtSplitMatVq2Spv, kWorldDeferRtSplitMatVq2Spv_bytes, "DeferRtSplitMatVq2",
         "mRtVqPipe (DeferRtSplitMatVq2)", &mRtVqPipe[VQS_MAT]},
        {2u, kWorldDeferRtSplitReqVq2Spv, kWorldDeferRtSplitReqVq2Spv_bytes, "DeferRtSplitReqVq2",
         "mRtVqPipe (DeferRtSplitReqVq2)", &mRtVqPipe[VQS_REQ]},
        {2u, kWorldDeferRtSplitLayersVq2Spv, kWorldDeferRtSplitLayersVq2Spv_bytes, "DeferRtSplitLayersVq2",
         "mRtVqPipe (DeferRtSplitLayersVq2)", &mRtVqPipe[VQS_LAYERS]},
        {2u, kWorldDeferRtSplitLightVq2Spv, kWorldDeferRtSplitLightVq2Spv_bytes, "DeferRtSplitLightVq2",
         "mRtVqPipe (DeferRtSplitLightVq2)", &mRtVqPipe[VQS_LIGHT]},
        // set 2 (bit 64): the GI hits, M1
        {4u, kWorldDeferGiSplitHitMatVqSpv, kWorldDeferGiSplitHitMatVqSpv_bytes, "DeferGiSplitHitMatVq",
         "mGiVqPipe (DeferGiSplitHitMatVq)", &mGiVqPipe[0][VQS_MAT]},
        {4u, kWorldDeferGiSplitHitLightVqSpv, kWorldDeferGiSplitHitLightVqSpv_bytes, "DeferGiSplitHitLightVq",
         "mGiVqPipe (DeferGiSplitHitLightVq)", &mGiVqPipe[0][VQS_LIGHT]}};
    for (uint32_t s = 0; s < kVqSets; ++s)
        if ((setMask & (1u << s)) != 0u) mVqPipesQueued[s] = true;
    for (const Stage& s : st)
    {
        if ((s.set & setMask) == 0u || *s.out) continue;
        VkShaderModule m = vqModule(dev, s.code, s.bytes, s.mod);
        createComputePipelineInto(dev, m, mRtReflLayout, 0u, vqLitSpec(), s.out, s.name);
        destroyShaderModuleDeferred(dev, m, nullptr);
    }
}

void LLCleanRendererVK::visQueueDestroy()
{
    if (!mCtx) return;
    mVqJob.wait();   // (a background build writes these handles)
    VkDevice dev = mCtx->device();
    if (mVisWalkPipe) { vkDestroyPipeline(dev, mVisWalkPipe, nullptr); mVisWalkPipe = VK_NULL_HANDLE; }
    for (uint32_t k = 0; k < VQS_COUNT; ++k)
        for (VkPipeline* q : {&mRtVqPipe[k], &mGiVqPipe[0][k], &mGiVqPipe[1][k]})
            if (*q) { vkDestroyPipeline(dev, *q, nullptr); *q = VK_NULL_HANDLE; }
    if (mVq.buffer) { mCtx->destroyBuffer(mVq); mVq = GpuBuffer{}; }
    if (mVqStatRb.buffer) { mCtx->destroyBuffer(mVqStatRb); mVqStatRb = GpuBuffer{}; }
    for (auto& s : mVqStatUsed)
        for (bool& u : s) u = false;
    for (VisQueueRegion& r : mVqRegion) r = VisQueueRegion{};
    mVqRtNow = mVqGiNow = false;
    mVqM6Now = false;
    visQueueM6End();
    mVqHdrDone = 0u;
    mVqSetGi = 1u;
    for (bool& q : mVqPipesQueued) q = false;
}

// recordWorld, after giDecide (the GI shade split, its extent) and deferSplitFrame (the RT split, its
// records): which consumers queue their hit sun rays this recording, the regions, the buffer.
void LLCleanRendererVK::visQueueDecide()
{
    mVqRtNow = mVqGiNow = false;
    mVqM6Now = false;
    visQueueM6End();   // (nothing collected may cross into this recording)
    mVqHdrDone = 0u;   // (no header of this recording written yet: the light prep follows)
    mVqStatChain = false;   // (never expected set here: a chain that did not reach its flush drops its copies)
    mVqStatPend = 0u;
    if (!mCtx || mOffscreen) return;   // (offscreen recordings run neither split: the buffer stays as it is)
    const double now = vqClock();
    U32 mask = 0, capMB = 0;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_vq(gSavedSettings, "RenderVKVisQueue", 0U);
    static LLCachedControl<U32> s_mb(gSavedSettings, "RenderVKVisQueueMB", 0U);
    mask = s_vq;
    capMB = s_mb;
#endif
    // ---- the counters of the recording 3 slots ago (frames in flight <= 2) ----
    mVqStatSlot = (mVqStatSlot + 1u) % 3u;
    for (uint32_t r = 0; r < VQR_COUNT; ++r)
    {
        if (!mVqStatUsed[mVqStatSlot][r]) continue;
        mVqStatUsed[mVqStatSlot][r] = false;
        if (!mVqStatRb.mapped) continue;
        const uint32_t* h = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mVqStatRb.mapped)
                                                               + ((size_t)mVqStatSlot * VQR_COUNT + r) * kVqStatBytes);
        mVqStatAcc.frames[r] += 1.0;
        mVqStatAcc.req[r] += h[0];
        mVqStatAcc.peak[r] = std::max(mVqStatAcc.peak[r], h[0]);
        mVqStatAcc.wanted[r] += h[12];
        mVqStatAcc.consts[r] += h[13];
        mVqStatAcc.past[r] += h[14];
        mVqStatAcc.refused[r] += h[15];
        mVqStatAcc.occ[r] += h[16];        // (M2: header word 4)
        mVqStatAcc.lightRays[r] += h[17];
        mVqStatAcc.litHits[r] += h[18];
    }
    if (now - mVqStatAcc.lastLog >= 10.0)
    {
        bool any = false;
        for (uint32_t r = 0; r < VQR_COUNT; ++r) any = any || mVqStatAcc.frames[r] > 0.0;
        if (any)
        {
            static const char* kName[VQR_COUNT] = {"RT reflections", "GI"};
            std::ostringstream os;
            os << "VK vis queue (#1039): buffer " << ((mVq.size + (1u << 19)) >> 20) << " MB of cap " << (mVqCapBytes >> 20) << " MB";
            for (uint32_t r = 0; r < VQR_COUNT; ++r)
            {
                const double f = mVqStatAcc.frames[r];
                if (!(f > 0.0)) continue;
                os << " | " << kName[r] << ": " << mVqRegion[r].entries << " entries, per frame "
                   << (uint64_t)(mVqStatAcc.req[r] / f + 0.5) << " sun requests (peak " << mVqStatAcc.peak[r] << "), "
                   << (uint64_t)(mVqStatAcc.consts[r] / f + 0.5) << " of " << (uint64_t)(mVqStatAcc.wanted[r] / f + 0.5)
                   << " sun-facing hits without a ray, " << (uint64_t)(mVqStatAcc.past[r] / f + 0.5)
                   << " past the cap (fused tail)";
                if (mVqStatAcc.litHits[r] > 0.0 || mVqStatAcc.occ[r] > 0.0)
                    os << "; " << (uint64_t)(mVqStatAcc.occ[r] / f + 0.5) << " occlusion requests, "
                       << (uint64_t)(mVqStatAcc.lightRays[r] / f + 0.5) << " light-ray requests (budget K "
                       << mVqRegion[r].budgetK << ") from " << (uint64_t)(mVqStatAcc.litHits[r] / f + 0.5)
                       << " light walks";
                if (mVqStatAcc.refused[r] > 0.0) os << ", " << (uint64_t)mVqStatAcc.refused[r] << " refused";
            }
            LL_INFOS("Vulkan") << os.str() << LL_ENDL;
        }
        const double t = now;
        mVqStatAcc = VqStatAcc{};
        mVqStatAcc.lastLog = t;
    }

    // ---- the pipelines: switched on after init, they compile in the background (the frame never waits on a
    //      cold compile); the consumers stay fused until every one landed ----
    if (mVqJob.pending()) mVqJob.poll();
    const uint32_t sets = vqSetsOf(mask);
    uint32_t queued = 0u;
    for (uint32_t s = 0; s < kVqSets; ++s) queued |= mVqPipesQueued[s] ? (1u << s) : 0u;
    const uint32_t missing = sets & ~queued;
    if (missing != 0u && !mVqJob.pending() && mRtReflLayout)
    {
        VkDevice dev = mCtx->device();
        {
            PipelineBatchScope scope(dev);
            visQueueInit(dev, missing);
            PipelineJobOpts o;
            o.what = "visibility queue";
            o.gate = false;   // (optional: never holds the login screen)
            mVqJob.start(pipelineBatchFlushAsync(dev, o));
        }
        LL_INFOS("Vulkan") << "VK vis queue (#1039): pipelines (set mask " << missing
                           << ") compiling in the background (the queue starts when they land)" << LL_ENDL;
    }
    // ---- this recording's consumers (a background build owns the handles until it lands: no queue meanwhile) ----
    const bool jobFree = !mVqJob.pending();
    // the GI hits: M2 (set 0), or with bit 64 M1 (set 2) once built
    const bool giM1 = (sets & 4u) != 0u && jobFree && vqSetReady(2u);
    const uint32_t vsGi = giM1 ? 0u : 1u;
    const bool wantGi = jobFree && vqSetReady(vsGi == 0u ? 2u : 0u) && mGiNow && mGiShadeSplitNow && mGiDn;
    // the RT reflections: M2 with bit 2 (set 1), else their fused split
    const bool wantRt = (sets & 2u) != 0u && jobFree && vqSetReady(1u) && mRtSplitNow && mRtRec.buffer && mRtRecCap != 0u;
    const uint64_t want[VQR_COUNT] = {wantRt ? (uint64_t)mRtRecCap : 0u,
                                      wantGi ? (uint64_t)mGiDn->width() * mGiDn->height() : 0u};
    // per entry: R requests (the most it can ask: never refused) + S result slots. M2's hit shadow-ray budget K = the
    // one the consumer's push carries this recording (RenderVKRTReflectionsHitShadowRays, RenderVKGIHitShadowRays;
    // 4 = the shaders' RT_SHADE_PROJ_MAXK). M1 (the GI with bit 64): the sun request and its slot.
    uint32_t reqPer[VQR_COUNT] = {1u, 1u}, slots[VQR_COUNT] = {1u, 1u}, budgetK[VQR_COUNT] = {0u, 0u};
    budgetK[VQR_RT] = std::clamp<uint32_t>(mRtReflHitShadows, 1u, 4u);   // (the push's clamp)
    reqPer[VQR_RT] = 2u + budgetK[VQR_RT];   // sun, occlusion, K lights
    slots[VQR_RT] = 4u + budgetK[VQR_RT];    // sun, light sum, K, occlusion, blend layers
    if (vsGi == 1u)
    {
        U32 giK = 1;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<U32> s_gik(gSavedSettings, "RenderVKGIHitShadowRays", 1U);
        giK = s_gik;
#endif
        budgetK[VQR_GI] = std::clamp<uint32_t>(giK, 1u, 4u);   // (llcleangivk.cpp's clamp: the push's K)
        reqPer[VQR_GI] = 1u + budgetK[VQR_GI];   // sun, K lights
        slots[VQR_GI] = 2u + budgetK[VQR_GI];    // sun, light sum, K
    }
    uint64_t entryBytes[VQR_COUNT];
    for (uint32_t r = 0; r < VQR_COUNT; ++r) entryBytes[r] = (uint64_t)reqPer[r] * kVqReqBytes + (uint64_t)slots[r] * kVqSlotBytes;
    bool wantAny = false;
    for (uint32_t r = 0; r < VQR_COUNT; ++r) wantAny = wantAny || want[r] != 0u;
    if (!wantAny)
    {
        // (off / nothing to queue: freed -- a transient stand-down re-allocates in the next queued frame)
        if (mVq.buffer)
        {
            retireBuffer(mVq);
            mVq = GpuBuffer{};
            LL_INFOS("Vulkan") << "VK vis queue (#1039): freed (no consumer queues its rays)" << LL_ENDL;
        }
        for (VisQueueRegion& r : mVqRegion) r = VisQueueRegion{};
        return;
    }
    // ---- the cap (RenderVKVisQueueMB; 0 = the VRAM tier's) and the regions within it ----
    uint64_t cap = (uint64_t)capMB << 20;
    if (cap == 0u)
    {
        // the tier from the GPU's device-local heap SIZE -- not mVramStatus.capacity, which is min(user cap, the
        // OS budget): the budget is what is free for this process right now (a 32 GB RTX 5090 with a desktop read
        // below 24 GB and got the 192 MB tier). VRAM pressure is handled by the allocation rules below (no new
        // buffer under texture pressure, #697 retry), not by the tier.
        if (mVqHeapBytes == 0u)
        {
            VkPhysicalDeviceMemoryProperties mp{};
            vkGetPhysicalDeviceMemoryProperties(mCtx->physicalDevice(), &mp);
            for (uint32_t h = 0; h < mp.memoryHeapCount; ++h)
                if ((mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0u)
                    mVqHeapBytes = std::max<uint64_t>(mVqHeapBytes, mp.memoryHeaps[h].size);
        }
        const uint64_t vram = mVqHeapBytes;
        const uint64_t gib = 1ull << 30;
        // (24 GB+: room for both consumers at 1440p -- ~340 MB at K 1; allocated by demand)
        cap = vram > 24u * gib ? (896ull << 20)
            : (vram > 25u * gib / 2u ? (192ull << 20) : (vram > 17u * gib / 2u ? (64ull << 20) : (32ull << 20)));
    }
    cap = std::clamp<uint64_t>(cap, 4ull << 20, 1024ull << 20);
    mVqCapBytes = (uint32_t)std::min<uint64_t>(cap, 0xFFFFFFFFull);
    uint64_t room = cap - VQR_COUNT * (kVqHdrBytes + kVqAlign);
    uint32_t entries[VQR_COUNT] = {};
    // A partial region: e of want entries within room (shared by demand when demand > room, then whole walk
    // workgroups); a sliver is not worth the extra dispatches -- below 4 K entries the consumer stays fused (and the
    // request / slot indices stay well inside 32 bits: <= 2^26 entries). A region that fits takes every entry
    // EXACTLY: rounded down to 64 it left the last < 64 entries of an extent that is no multiple of 64 to the fused
    // tail -- whose dispatch (the GI's HIT-LIGHT, the RT's LIGHT: the whole list's grid, every lane but those few
    // leaving at once) then ran every frame. (Nothing needs whole workgroups: the walk's dispatch follows the
    // requests, every stage tests its entry against the capacity.)
    auto partial = [](uint64_t want, uint64_t demand, uint64_t room) -> uint32_t
    {
        uint64_t e = want;
        if (demand > room)
        {
            e = (uint64_t)((double)e * ((double)room / (double)demand));   // (the cap: shared by demand)
            e &= ~(uint64_t)63u;
        }
        return (want != 0u && e >= 4096u) ? (uint32_t)std::min<uint64_t>(e, 1ull << 26) : 0u;
    };
    // the hit shading's regions next (RT reflections + GI hits, shared by demand): their entries past the region run
    // the fused lighting stage, at the full cost of a lighting kernel with its walks inline
    {
        const uint64_t demand = want[VQR_RT] * entryBytes[VQR_RT] + want[VQR_GI] * entryBytes[VQR_GI];
        for (uint32_t r : {(uint32_t)VQR_RT, (uint32_t)VQR_GI})
        {
            entries[r] = partial(want[r], demand, room);
            room -= std::min<uint64_t>(room, (uint64_t)entries[r] * entryBytes[r]);
        }
    }
    VkDeviceSize off = 0;
    VisQueueRegion reg[VQR_COUNT];
    for (uint32_t r = 0; r < VQR_COUNT; ++r)
    {
        reg[r].wanted = (uint32_t)want[r];
        reg[r].entries = entries[r];
        reg[r].reqPerEntry = reqPer[r];
        reg[r].slots = slots[r];
        reg[r].budgetK = budgetK[r];
        if (!entries[r]) continue;
        reg[r].offset = off;
        off = vqAlignUp(off + kVqHdrBytes + (VkDeviceSize)entries[r] * entryBytes[r]);
    }
    const VkDeviceSize bytes = off;
    if (bytes == 0u)
    {
        if (mVq.buffer) { retireBuffer(mVq); mVq = GpuBuffer{}; }
        for (VisQueueRegion& r : mVqRegion) r = VisQueueRegion{};
        return;
    }
    // ---- the buffer: a new size allocates first and retires the old one (no descriptor names it) ----
    if (!mVq.buffer || mVq.size != bytes)
    {
        if (mTexPressure || now < mVqRetryAt)
        {
            // (soft: under texture pressure / after a refusal no new allocation -- a buffer at least this large
            //  keeps serving the new regions, else the consumers stay fused until it may allocate again)
            if (mVq.buffer && mVq.size < bytes) { retireBuffer(mVq); mVq = GpuBuffer{}; }
            // (logged on the transition: every consumer runs its fused kernels while this holds)
            if (!mVq.buffer && !s_vqHeld)
                LL_INFOS("Vulkan") << "VK vis queue (#1039): no " << ((bytes + (1u << 19)) >> 20) << " MB buffer "
                                   << (mTexPressure ? "under texture pressure" : "until the refusal's retry")
                                   << ": the fused kernels until it may allocate" << LL_ENDL;
            s_vqHeld = !mVq.buffer;
            if (!mVq.buffer) { for (VisQueueRegion& r : mVqRegion) r = VisQueueRegion{}; return; }
        }
        else
        {
            GpuBuffer nb{};
            {
                VkMemScope memcat(VKMEM_TARGET);
                VkDeviceContext::SoftAllocScope soft(mCtx);
                nb = mCtx->createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                                               | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
            }
            if (nb.buffer && nb.address)
            {
                if (mVq.buffer) retireBuffer(mVq);
                mVq = nb;
                s_vqHeld = false;
                mCtx->setObjectName(VK_OBJECT_TYPE_BUFFER, (uint64_t)mVq.buffer, "visQueue");
                LL_INFOS("Vulkan") << "VK vis queue (#1039): " << ((bytes + (1u << 19)) >> 20) << " MB (cap "
                                   << (cap >> 20) << " MB; GI hits M" << (vsGi == 1u ? 2 : 1) << "; RT reflections " << entries[VQR_RT] << " of "
                                   << want[VQR_RT] << " entries x " << entryBytes[VQR_RT] << " B, GI " << entries[VQR_GI]
                                   << " of " << want[VQR_GI] << " x " << entryBytes[VQR_GI] << " B)" << LL_ENDL;
            }
            else
            {
                if (nb.buffer) mCtx->destroyBuffer(nb);
                mVqRetryAt = now + 30.0;
                LL_WARNS("Vulkan") << "VK vis queue (#1039): the " << ((bytes + (1u << 19)) >> 20)
                                   << " MB buffer was refused (VRAM); the fused hit shading instead, retry in 30 s" << LL_ENDL;
                if (mVq.buffer) { retireBuffer(mVq); mVq = GpuBuffer{}; }
                for (VisQueueRegion& r : mVqRegion) r = VisQueueRegion{};
                return;
            }
        }
    }
    for (uint32_t r = 0; r < VQR_COUNT; ++r) mVqRegion[r] = reg[r];
    mVqSetGi = vsGi;
    // (the GI region's PRODUCER POLICY, header word 5: which rays its hits ask for -- image-changing, off by default)
    {
        bool sunOpaque = false;
        F32 minShare = 0.f;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<bool> s_so(gSavedSettings, "RenderVKVisQueueGISunOpaque", false);
        static LLCachedControl<F32> s_ms(gSavedSettings, "RenderVKVisQueueGIMinShare", 0.f);
        sunOpaque = s_so;
        minShare = s_ms;
#endif
        const float ms = std::isfinite(minShare) ? std::max(minShare, 0.f) : 0.f;
        std::memcpy(&mVqGiPolicy[0], &ms, sizeof(ms));
        mVqGiPolicy[1] = sunOpaque ? 1u : 0u;
    }
    mVqRtNow = mVqRegion[VQR_RT].entries != 0u;
    mVqGiNow = mVqRegion[VQR_GI].entries != 0u;
    // the frame-level walk, when any consumer queues this recording
    mVqM6Now = mVqRtNow || mVqGiNow;
}

// Region r's header for this recording: (0, 0, 0, 1) -- no request, the walk's dispatch empty --, the
// capacities, the RT records' address / capacity, the counters zeroed. WAR: last frame's walk / resolve
// reads (storage + indirect) and the counters' copy, then the producer's appends see it.
void LLCleanRendererVK::visQueueBegin(VkCommandBuffer cmd, uint32_t r, VkDeviceAddress recAddr, uint32_t recCap)
{
    if (!mVq.buffer || r >= VQR_COUNT || !mVqRegion[r].entries) return;
    if ((mVqHdrDone & (1u << r)) != 0u)
    {
        // (the light prep -- or the M6 frame begin -- wrote it this recording: taken, no drain pair here; a second
        //  use of the region in the same recording writes it again below)
        mVqHdrDone &= ~(1u << r);
        return;
    }
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; m.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &d);
    const uint32_t w2[4] = {(uint32_t)(recAddr & 0xFFFFFFFFull), (uint32_t)(recAddr >> 32), recCap, 0u};
    visQueueHdrWrite(cmd, r, w2);
    m.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; m.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                    | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &d);
}

// The header write alone (the caller orders it: visQueueBegin, visQueueBeginFrame).
void LLCleanRendererVK::visQueueHdrWrite(VkCommandBuffer cmd, uint32_t r, const uint32_t w2[4])
{
    const uint32_t e = mVqRegion[r].entries;
    uint32_t hdr[24] = {0u, 0u, 0u, 1u,
                        e * mVqRegion[r].reqPerEntry, e, mVqRegion[r].slots, mVqRegion[r].budgetK,
                        w2[0], w2[1], w2[2], w2[3],
                        0u, 0u, 0u, 0u,
                        0u, 0u, 0u, 0u};
    VkDeviceSize updateBytes = 20u * sizeof(uint32_t);
    if (r == VQR_GI)
    {
        // word 5: the GI region's producer policy
        hdr[20] = mVqGiPolicy[0]; hdr[21] = mVqGiPolicy[1];
        updateBytes = 24u * sizeof(uint32_t);
    }
    vkCmdUpdateBuffer(cmd, mVq.buffer, mVqRegion[r].offset, updateBytes, hdr);
}
// Header word 2 as each consumer's own begin writes it: the RT records' address / capacity (visQueueBegin's caller in
// recordRtReflect), else 0.
void LLCleanRendererVK::visQueueHdrW2(uint32_t r, uint32_t w2[4]) const
{
    w2[0] = w2[1] = w2[2] = w2[3] = 0u;
    if (r == VQR_RT)
    {
        w2[0] = (uint32_t)(mRtRec.address & 0xFFFFFFFFull);
        w2[1] = (uint32_t)(mRtRec.address >> 32);
        w2[2] = mRtRecCap;
    }
}

// ---- THE HEADERS IN THE LIGHT PREP: a region's header was a COMPUTE -> TRANSFER -> COMPUTE drain pair inside the
//      lighting chain (visQueueBegin; under M6 one pair for all of them, visQueueBeginFrame). recordLightPrep writes
//      them with the chain's other list headers instead (the pdn_vis job list, the tile list, the GI HIT / MISS lists),
//      between its opening barrier (WAR: last frame's producers / walks / resolves / counter copies -- src compute +
//      indirect + transfer, on this queue or behind the fork's semaphore) and its closing barrier (-> this frame's
//      appends and the walks' indirect reads -- dst compute + indirect); nothing between there and the chain touches
//      the queue. visQueueDecide runs first (the regions, the buffer); the consumers' visQueueBegin then take the
//      header (mVqHdrDone). A region the prep did not write (the prep recorded late, or not at the early point) gets
//      the pair as before. q_gi_lag's GI (after the combine) takes its regions the same way: the prep precedes it.
uint32_t LLCleanRendererVK::visQueuePrepMask() const
{
    if (!mVq.buffer) return 0u;
    uint32_t m = 0u;
    if (mVqRtNow && mRtRec.buffer) m |= 1u << VQR_RT;
    if (mVqGiNow) m |= 1u << VQR_GI;
    for (uint32_t r = 0; r < VQR_COUNT; ++r)
        if (!mVqRegion[r].entries) m &= ~(1u << r);
    return m;
}
void LLCleanRendererVK::visQueuePrepHeaders(VkCommandBuffer cmd, uint32_t mask)
{
    mask &= visQueuePrepMask();   // (the same recording's decision: nothing changed since the plan)
    for (uint32_t r = 0; r < VQR_COUNT; ++r)
    {
        if ((mask & (1u << r)) == 0u) continue;
        uint32_t w2[4];
        visQueueHdrW2(r, w2);
        visQueueHdrWrite(cmd, r, w2);
    }
    mVqHdrDone |= mask;
}

// The walk over region r's requests: one lane per request, the producer's indirect dispatch (header words
// 1-3). The caller bound the RT layout's sets and ordered the producer's writes before it (+ indirect).
void LLCleanRendererVK::visQueueWalk(VkCommandBuffer cmd, uint32_t r, const VKRtReflPush& base)
{
    if (!mVq.buffer || !mVisWalkPipe || r >= VQR_COUNT || !mVqRegion[r].entries) return;
    VKRtReflPush wp = base;
    const VkDeviceAddress a = visQueueAddr(r);
    wp.rtr3[0] = 0u;
    wp.rtr3[1] = 0u;
    wp.rtr3[2] = (uint32_t)(a & 0xFFFFFFFFull);
    wp.rtr3[3] = (uint32_t)(a >> 32);
    VK_PUSH_BUDGET_CHECK(wp);
    vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(wp), &wp);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rtBcPipe(mVisWalkPipe));   // (#1128 beacon twin when on)
    vkCmdDispatchIndirect(cmd, mVq.buffer, mVqRegion[r].offset + 4u);
}

// Region r's counters (header words 0-4) into this recording's readback slot (GPU timers on): after the
// consumer's resolve. visQueueDecide harvests the slot three recordings later.
// Inside the lighting chain (mVqStatChain: recordDeferredLighting's split paths) the copy waits for
// visQueueStatsFlush at the chain's end -- ONE compute -> transfer barrier for every region instead of one each (with
// timers on these were up to 5 drains between the chain's dispatches, in exactly the frames the timers measure).
void LLCleanRendererVK::visQueueStats(VkCommandBuffer cmd, uint32_t r)
{
    if (!mTsOn || !mVq.buffer || r >= VQR_COUNT || !mVqRegion[r].entries) return;
    if (mVqStatChain)
    {
        mVqStatPend |= 1u << r;
        return;
    }
    visQueueStatsCopy(cmd, 1u << r);
}
void LLCleanRendererVK::visQueueStatsFlush(VkCommandBuffer cmd)
{
    const uint32_t pend = mVqStatPend;
    mVqStatPend = 0u;
    mVqStatChain = false;
    if (pend != 0u && mTsOn && mVq.buffer) visQueueStatsCopy(cmd, pend);
}
void LLCleanRendererVK::visQueueStatsCopy(VkCommandBuffer cmd, uint32_t mask)
{
    if (!mVqStatRb.buffer)
    {
        VkMemScope memcat(VKMEM_READBACK);   // #407
        mVqStatRb = mCtx->createBuffer(3u * VQR_COUNT * kVqStatBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!mVqStatRb.buffer) return;   // (stats only: none this time, retried next frame)
    }
    VkBufferCopy c[VQR_COUNT]{};
    uint32_t n = 0u;
    for (uint32_t r = 0; r < VQR_COUNT; ++r)
    {
        if ((mask & (1u << r)) == 0u || !mVqRegion[r].entries) continue;
        c[n].srcOffset = mVqRegion[r].offset;
        c[n].dstOffset = ((VkDeviceSize)mVqStatSlot * VQR_COUNT + r) * kVqStatBytes;
        c[n].size = kVqStatBytes;
        ++n;
        mVqStatUsed[mVqStatSlot][r] = true;
    }
    if (n == 0u) return;
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; m.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &d);
    vkCmdCopyBuffer(cmd, mVq.buffer, mVqStatRb.buffer, n, c);
}

// ======================= (#1039 M6, RenderVKVisQueue bit 16) THE FRAME WALK =======================
// recordDeferLight's split chains, with bit 16 and at least one queued consumer this recording:
//   visQueueBeginFrame (the headers the light prep did not write -- normally none: no commands)
//   (the merged shadow pass + its denoiser, recorded whole); REFLECT
//   PRODUCERS: the RT reflections' TRACE + MAT + REQ (+ the fused tail); [the projector-only pass 15];
//              the GI's TRACE + MISS + HIT-MAT + HIT-REQ -- in shared levels (THE LEVELS below)
//   visQueueFrameWalk: ONE barrier, every consumer's walk back to back (the RT walk + its blend layers, the GI walk
//              -- disjoint outputs, so no barrier between them: their traversals overlap and fill each other's tails),
//              ONE barrier
//   RESOLVES:  SHADOW (it reads the denoised sun), the RT LIGHT + forward + resolve, the GI HIT-LIGHT + REUSE (its
//              visibility rays inline) + the rest
// Everything stays on the chain's queue segment;
// the GI early trace (#978) keeps its segment (the chain), the one-frame-latency GI (mGiLagNow) runs after the
// combine with its own walk (it is recorded outside the collecting window).
// Dependencies the reorder relies on (checked against clean_deferred_light.comp's bindings): REFLECT, the RT and the
// GI producers read neither pass 15's outputs (set 3 bindings 13 / 16: read by the combine passes and SHADOW only)
// nor SHADOW / CLASSIFY's (uVisSun / uVisProj / the tile grid: the combine passes; REFLECT already ran beside SHADOW
// without a barrier); the RT and GI hit lighting takes its lights from the clusters and the ReGIR grid, built before
// pass 15.
void LLCleanRendererVK::visQueueBeginFrame(VkCommandBuffer cmd, bool withGi, bool onAsync)
{
    if (!mVq.buffer) return;
    uint32_t want = visQueuePrepMask();
    if (!withGi) want &= ~(1u << VQR_GI);
    want &= ~mVqHdrDone;   // (the light prep wrote them: normally all -- then no barrier here at all)
    if (want == 0u) return;
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; m.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &d);
    for (uint32_t r = 0; r < VQR_COUNT; ++r)
    {
        if ((want & (1u << r)) == 0u) continue;
        uint32_t w2[4];
        visQueueHdrW2(r, w2);
        visQueueHdrWrite(cmd, r, w2);
    }
    m.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; m.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                    | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &d);
    mVqHdrDone |= want;
    gpuSubMark(cmd, onAsync ? "c_vq_begin" : "g_vq_begin");
}
bool LLCleanRendererVK::visQueueM6Defer(uint32_t c, std::function<void(VkCommandBuffer)> walk,
                                        std::function<void(VkCommandBuffer)> post)
{
    if (!mVqM6Collect || c >= VQC_COUNT || mVqM6Walk[c] || mVqM6Post[c]) return false;
    mVqM6Walk[c] = std::move(walk);
    mVqM6Post[c] = std::move(post);
    return true;
}
// ---- THE LEVELS: the consumers' producer chains are independent of each other (the audit above), so their stages
//      need not each drain the GPU for the others: a level is every consumer's next stage, back to back, as soon as
//      possible (stage k of consumer c at level first_c + k), and ONE barrier separates two levels (src compute
//      writes; dst compute + indirect + the #1128 copy -- the union of what the consumers' own barriers named). With
//      the default chain (RT reflections 2 stages -- 4 with their split -- from level 1, GI 3) that is 2 (4) barriers
//      for every consumer's stages, and each level's short kernels run beside the long ones instead of alone between
//      drains. The order inside a level is the consumers' (RT, GI). The RT reflections start at level 1 (first_c 1:
//      their selection reads REFLECT, which the chain records before the schedule), and that level's barrier also
//      carries their forward target's WAR on last frame's FS reads (recordRtReflect's opening barrier).
//      Same commands, same pushes (each stage binds its own sets + push), so the same image.
void LLCleanRendererVK::visQueueM6Prod(uint32_t c, uint32_t first,
                                       std::initializer_list<std::function<void(VkCommandBuffer)>> st)
{
    if (c >= VQC_COUNT) return;
    uint32_t n = 0u;
    for (const std::function<void(VkCommandBuffer)>& f : st)
    {
        if (n >= kVqM6Stages) break;
        if (f) mVqM6Prod[c][n++] = f;
    }
    mVqM6ProdN[c] = n;
    mVqM6ProdFirst[c] = first;
}
// The RT reflections' stages for the schedule (recordRtReflect's commands, in its order, each with its sets + push):
//   ray list, shade split + queue: 1 SELECT, 2 (#1128 copy) TRACE, 3 MAT + the fused TAIL (its rays are its own: no
//     barrier against MAT), 4 REQ; the walk (+ LAYERS) + FORWARD in the wave; LIGHT the head; the rest
//     (barrier, RESOLVE, the stats) after;
//   ray list, fused: 1 SELECT, 2 (#1128 copy) the list TRACE; FORWARD in the wave; RESOLVE the head; the stats after;
//   tile trace: 1 TRACE; likewise.
// FORWARD reads neither the trace's nor the walk's results (recordRtReflectTail records it beside them) and writes only
// its own target. The shade split without the queue keeps recordRtReflect's own order (false).
bool LLCleanRendererVK::recordRtReflectLeveled(VkCommandBuffer cmd, const VKDeferPush& p, uint32_t viewOff, bool onAsync,
                                               VkDescriptorSet rtSet)
{
    const bool list = mRtReflRayList && mRtSelectPipe && mRtTraceListPipe;
    const bool split = list && mRtSplitNow && mRtRec.buffer && mRtRec.address;
    const bool vq = split && mVqRtNow && mVq.buffer && vqSetReady(1u);
    if (split && !vq) return false;
    const VKRtReflPush rp = rtReflPush(p);
    const VkDescriptorSet sets[5] = {mDsetDraw0, mDsetDraw1, mDsetDefer, mDsetDeferSplit, rtSet};
    const VkDeviceSize argOff = kRtTilesHdrBytes + ((VkDeviceSize)mDsRtTileCount * 2u + 2u) * sizeof(uint32_t);
    VKRtReflPush sp = rp;   // (the split: rtr3.x the records' capacity, rtr3.zw their address)
    sp.rtr3[0] = mRtRecCap;
    sp.rtr3[2] = (uint32_t)(mRtRec.address & 0xFFFFFFFFull);
    sp.rtr3[3] = (uint32_t)(mRtRec.address >> 32);
    VKRtReflPush vp = sp;   // (the queue's stages: rtr3.zw the region)
    if (vq)
    {
        visQueueBegin(cmd, VQR_RT, mRtRec.address, mRtRecCap);   // (the light prep wrote it: no commands)
        const VkDeviceAddress qa = visQueueAddr(VQR_RT);
        vp.rtr3[0] = 0u;
        vp.rtr3[1] = 0u;
        vp.rtr3[2] = (uint32_t)(qa & 0xFFFFFFFFull);
        vp.rtr3[3] = (uint32_t)(qa >> 32);
    }
    auto bind = [this, sets, viewOff](VkCommandBuffer c, const VKRtReflPush& q)
    {
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, mRtReflLayout, 0, 5, sets, 1, &viewOff);
        VK_PUSH_BUDGET_CHECK(q);
        vkCmdPushConstants(c, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(q), &q);
    };
    const char* traceMark = onAsync ? "c_defer_rtrefl_trace" : "g_defer_rtrefl_trace";
    std::function<void(VkCommandBuffer)> st0, st1, st2, st3;
    if (list)
    {
        st0 = [this, bind, rp, onAsync](VkCommandBuffer c)
        {
            bind(c, rp);
            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, mRtSelectPipe);
            vkCmdDispatchIndirect(c, mDsRtTiles.buffer, 0);
            gpuSubMark(c, onAsync ? "c_defer_rtrefl_sel" : "g_defer_rtrefl_sel");
        };
        st1 = [this, bind, rp, sp, split, argOff, traceMark](VkCommandBuffer c)
        {
            rtReflArgsCopy(c);   // (the level's barrier carries COPY)
            bind(c, split ? sp : rp);
            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, split ? mRtSplitPipe[RTS_TRACE] : rtBcPipe(mRtTraceListPipe));
            vkCmdDispatchIndirect(c, mDsRtTiles.buffer, argOff);
            gpuSubMark(c, traceMark);
        };
        if (vq)
        {
            st2 = [this, bind, sp, vp, argOff, onAsync](VkCommandBuffer c)
            {
                bind(c, vp);
                vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, mRtVqPipe[VQS_MAT]);
                vkCmdDispatchIndirect(c, mDsRtTiles.buffer, argOff);
                gpuSubMark(c, onAsync ? "c_defer_rtrefl_mat" : "g_defer_rtrefl_mat");
                vkCmdPushConstants(c, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(sp), &sp);
                vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, rtBcPipe(mRtTraceListPipe));   // (#1128 beacon twin when on)
                vkCmdDispatchIndirect(c, mDsRtTiles.buffer, argOff);
                gpuSubMark(c, onAsync ? "c_defer_rtrefl_tail" : "g_defer_rtrefl_tail");
            };
            st3 = [this, bind, vp, argOff, onAsync](VkCommandBuffer c)
            {
                bind(c, vp);
                vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, mRtVqPipe[VQS_REQ]);
                vkCmdDispatchIndirect(c, mDsRtTiles.buffer, argOff);
                gpuSubMark(c, onAsync ? "c_defer_rtrefl_req" : "g_defer_rtrefl_req");
            };
        }
    }
    else
    {
        st0 = [this, bind, rp, traceMark](VkCommandBuffer c)
        {
            bind(c, rp);
            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, mRtTracePipe);
            vkCmdDispatchIndirect(c, mDsRtTiles.buffer, 0);   // one 8x8 workgroup per listed tile
            gpuSubMark(c, traceMark);
        };
    }
    // (level 1: SELECT reads REFLECT's tile list, and the barrier takes the forward target's WAR on last frame's FS)
    visQueueM6Prod(VQC_RT, 1u, {st0, st1, st2, st3});
    mVqM6WaitImg = (mRtFwdNow && mDsRtFwd.image) ? mDsRtFwd.image : VK_NULL_HANDLE;
    mVqM6FwdInWave = mVqM6WaitImg != VK_NULL_HANDLE;
    // the wave: the walk (+ the blend layers) and FORWARD (the frame walk bound the RT layout's sets)
    auto walk = [this, p, rp, sp, vp, vq, argOff, onAsync](VkCommandBuffer c)
    {
        if (vq)
        {
            visQueueWalk(c, VQR_RT, sp);
            gpuSubMark(c, onAsync ? "c_defer_rtrefl_walk" : "g_defer_rtrefl_walk");
            if (mRtReflBlend)
            {
                vkCmdPushConstants(c, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(vp), &vp);
                vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, rtBcPipe(mRtVqPipe[VQS_LAYERS]));   // (#1128)
                vkCmdDispatchIndirect(c, mDsRtTiles.buffer, argOff);
                gpuSubMark(c, onAsync ? "c_defer_rtrefl_layers" : "g_defer_rtrefl_layers");
            }
        }
        vkCmdPushConstants(c, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rp), &rp);
        recordRtReflectFwd(c, p, onAsync);
    };
    if (vq)
    {
        // the head: LIGHT (+ the fused LIGHT over the entries past the region); the rest: barrier, RESOLVE, the stats
        mVqM6Head[VQC_RT] = [this, bind, sp, vp, argOff, onAsync](VkCommandBuffer c)
        {
            bind(c, vp);
            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, mRtVqPipe[VQS_LIGHT]);
            vkCmdDispatchIndirect(c, mDsRtTiles.buffer, argOff);
            if (mVqRegion[VQR_RT].entries < mRtRecCap)
            {
                VKRtReflPush tp = sp;
                tp.rtr3[1] = mVqRegion[VQR_RT].entries;
                vkCmdPushConstants(c, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(tp), &tp);
                vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, mRtSplitPipe[RTS_LIGHT]);
                vkCmdDispatchIndirect(c, mDsRtTiles.buffer, argOff);
            }
            gpuSubMark(c, onAsync ? "c_defer_rtrefl_light" : "g_defer_rtrefl_light");
            visQueueStats(c, VQR_RT);
        };
        visQueueM6Defer(VQC_RT, walk,
            [this, bind, rp, p, viewOff, onAsync](VkCommandBuffer c)
            {
                bind(c, rp);
                recordRtReflectTail(c, p, viewOff, onAsync, false);
            });
    }
    else
    {
        // the head: RESOLVE (the trace's and FORWARD's results: the wave's closing barrier); the rest: the stats
        mVqM6Head[VQC_RT] = [this, p, viewOff, onAsync](VkCommandBuffer c)
        { recordRtReflectResolve(c, p, viewOff, onAsync, false); };
        visQueueM6Defer(VQC_RT, walk, [this](VkCommandBuffer c) { rtReflStatsRecord(c); });
    }
    return true;
}

void LLCleanRendererVK::visQueueFrameWalk(VkCommandBuffer cmd, uint32_t viewOff, bool onAsync)
{
    mVqM6Collect = false;
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    // ---- the levels ----
    {
        uint32_t start[VQC_COUNT] = {}, end = 0u;
        for (uint32_t c = 0; c < VQC_COUNT; ++c)
        {
            if (mVqM6ProdN[c] == 0u) continue;
            start[c] = mVqM6ProdFirst[c];
            end = std::max(end, start[c] + mVqM6ProdN[c]);
        }
        for (uint32_t lv = 0; lv < end; ++lv)
        {
            bool here = false, img = false;
            for (uint32_t c = 0; c < VQC_COUNT; ++c)
            {
                const bool at = mVqM6ProdN[c] != 0u && lv >= start[c] && lv < start[c] + mVqM6ProdN[c];
                here = here || at;
                img = img || (at && lv == start[c] && c == VQC_RT && mVqM6WaitImg != VK_NULL_HANDLE);
            }
            if (!here) continue;
            if (lv > 0u)
            {
                m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT
                               | VK_PIPELINE_STAGE_2_COPY_BIT;
                m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                                | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
                VkImageMemoryBarrier2 fb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                fb.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                fb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; fb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                fb.oldLayout = VK_IMAGE_LAYOUT_GENERAL; fb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                fb.image = mVqM6WaitImg; fb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                d = VkDependencyInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
                if (img) { d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &fb; }
                vkCmdPipelineBarrier2(cmd, &d);
            }
            for (uint32_t c = 0; c < VQC_COUNT; ++c)
            {
                if (mVqM6ProdN[c] == 0u || lv < start[c] || lv >= start[c] + mVqM6ProdN[c]) continue;
                std::function<void(VkCommandBuffer)> f = std::move(mVqM6Prod[c][lv - start[c]]);
                mVqM6Prod[c][lv - start[c]] = nullptr;
                if (f) f(cmd);
            }
        }
        for (uint32_t& n : mVqM6ProdN) n = 0u;
        mVqM6WaitImg = VK_NULL_HANDLE;
    }
    bool any = false;
    for (uint32_t c = 0; c < VQC_COUNT; ++c) any = any || (bool)mVqM6Walk[c];
    // every producer's appends + raised dispatches -> the walks (storage + indirect)
    d = VkDependencyInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    if (!any || !mRT)
    {
        // (never expected: a scheduled consumer always hands over a walk) the producers -> the heads
        bool head = false;
        for (uint32_t c = 0; c < VQC_COUNT; ++c) head = head || (bool)mVqM6Head[c];
        if (!head) return;
        m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
        vkCmdPipelineBarrier2(cmd, &d);
        for (uint32_t c = 0; c < VQC_COUNT; ++c) visQueueFrameHead(cmd, c);
        return;
    }
    const VkDescriptorSet rtSet = mRT->consumerSet();
    m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                    | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &d);
    gpuSubMark(cmd, onAsync ? "c_vq_prod" : "g_vq_prod");   // (the producers' tail: the wave starts here)
    // (every walk runs on the RT layout with the chain's sets; each pushes its own region)
    const VkDescriptorSet sets[5] = {mDsetDraw0, mDsetDraw1, mDsetDefer, mDsetDeferSplit, rtSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRtReflLayout, 0, 5, sets, 1, &viewOff);
    for (uint32_t c = 0; c < VQC_COUNT; ++c)
    {
        if (!mVqM6Walk[c]) continue;
        std::function<void(VkCommandBuffer)> f = std::move(mVqM6Walk[c]);
        mVqM6Walk[c] = nullptr;
        f(cmd);
    }
    // the walks' results -> the resolves (+ the RT forward target, when it ran in the wave: -> the forward / water FS,
    // as recordRtReflectTail's barrier gave it)
    m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    if (mVqM6FwdInWave)
    {
        m.dstStageMask |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        m.dstAccessMask |= VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    }
    mVqM6FwdInWave = false;
    vkCmdPipelineBarrier2(cmd, &d);
    // (the levels) every consumer's HEAD -- its resolve's first kernel: the RT reflections'
    // LIGHT (their split) or RESOLVE, the GI's HIT-LIGHT -- back to back after that one barrier (disjoint outputs; none
    // reads another's -- the GI cache, which the RT kernels read, is written only by the GI's REUSE and its cache
    // pass, in the GI's rest, behind barriers); the rest of each consumer follows (visQueueFramePost), led by its own
    // barrier as before
    for (uint32_t c = 0; c < VQC_COUNT; ++c) visQueueFrameHead(cmd, c);
}
void LLCleanRendererVK::visQueueFrameHead(VkCommandBuffer cmd, uint32_t c)
{
    if (c >= VQC_COUNT || !mVqM6Head[c]) return;
    std::function<void(VkCommandBuffer)> f = std::move(mVqM6Head[c]);
    mVqM6Head[c] = nullptr;
    f(cmd);
}
void LLCleanRendererVK::visQueueFramePost(VkCommandBuffer cmd, uint32_t c)
{
    if (c >= VQC_COUNT) return;
    visQueueFrameHead(cmd, c);   // (never expected left: the frame walk ran the heads)
    if (!mVqM6Post[c]) return;
    std::function<void(VkCommandBuffer)> f = std::move(mVqM6Post[c]);
    mVqM6Post[c] = nullptr;
    f(cmd);
}
void LLCleanRendererVK::visQueueM6End()
{
    mVqM6Collect = false;
    for (uint32_t c = 0; c < VQC_COUNT; ++c)
    {
        mVqM6Walk[c] = nullptr;
        mVqM6Post[c] = nullptr;
        mVqM6Head[c] = nullptr;
        for (std::function<void(VkCommandBuffer)>& f : mVqM6Prod[c]) f = nullptr;
        mVqM6ProdN[c] = 0u;
    }
    mVqM6WaitImg = VK_NULL_HANDLE;
    mVqM6FwdInWave = false;
}
