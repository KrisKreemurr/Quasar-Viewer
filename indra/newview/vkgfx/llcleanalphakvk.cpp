/**
 * @file llcleanalphakvk.cpp
 * @brief Alpha-blend cover lists (#1147): see llcleanalphakvk.h and clean_alpha_k.glsl.
 */
#include "linden_common.h"

#include "llcleanalphakvk.h"
#include "llcleanrenderervk.h"     // vkWorldStaticVI / vkWorldRiggedVI (the world vertex streams)
#include "vk_descpool.h"           // vkdCreatePipelineLayout
#include "vk_world_shaders_spv.h"

#include "llerror.h"

#include <algorithm>
#include <sstream>

namespace
{
VkShaderModule akModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    vkNoteShaderModule(m, code, bytes, name);
    return m;
}

void bufferBarrier(VkCommandBuffer cmd, VkBuffer buf, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
    b.buffer = buf; b.size = VK_WHOLE_SIZE;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.bufferMemoryBarrierCount = 1; d.pBufferMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

// clean_alpha_k_clear.comp Push
struct ClearPush { uint32_t entries[2]; uint32_t width, height, k, pad[3]; };
static_assert(sizeof(ClearPush) == 32, "clean_alpha_k_clear.comp Push");

constexpr VkAccessFlags2 kStorageRW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
constexpr uint32_t kIdleFreeFrames = 600;   // ~10 s unused: free the lists
constexpr uint32_t kRetryFrames = 600;      // after a failed / refused allocation
constexpr uint32_t kMaxFailures = 3;        // then give up for this render extent
} // namespace

// FIRST USE OFF THE RENDER THREAD (as LLCleanGodRaysVK): the clear layout is made here, the
// three pipelines go to an async batch and land on a later call -- no lists until then. A failed
// build is latched for this draw layout.
bool LLCleanAlphaKVK::ensurePipelines(const Frame& f)
{
    VkDevice dev = f.ctx->device();
    if (mJob.pending())
    {
        if (!mJob.poll()) return false;   // still compiling
        mVisPipe[0] = mStage[0]; mVisPipe[1] = mStage[1]; mClearPipe = mStage[2];
        mCoverPipe[0] = mStage[3]; mCoverPipe[1] = mStage[4];
        for (VkPipeline& p : mStage) p = VK_NULL_HANDLE;
        if (!mCoverPipe[0] || !mCoverPipe[1] || (mBuiltLists && (!mVisPipe[0] || !mVisPipe[1] || !mClearPipe)))
        {
            LL_WARNS("VKAlphaK") << "alpha cover list pipelines failed to build; every blended layer is lit (as before)"
                                 << LL_ENDL;
            const VkPipelineLayout built = mBuiltLayout;
            const VkFormat builtDepth = mBuiltDepth;
            destroy(dev);
            mBuiltLayout = built;
            mBuiltDepth = builtDepth;
            mPipeFailed = true;
            return false;
        }
        LL_INFOS("VKAlphaK") << "alpha cover list pipelines built (background)" << LL_ENDL;
    }
    const bool same = mBuiltLayout == f.drawLayout && mBuiltDepth == f.depthFormat;
    if (mCoverPipe[0] && same) return true;
    if (mPipeFailed && same) return false;
    if (mVisPipe[0] || mCoverPipe[0] || mClearPipe || mClearLayout)
    {
        // The world draw layout was re-created (rare): rebuild against it.
        vkDeviceWaitIdle(dev);
        destroy(dev);
    }
    mCtx = f.ctx;
    mPipeFailed = false;
    mBuiltLayout = f.drawLayout;
    mBuiltDepth = f.depthFormat;
    mBuiltLists = f.lists;

    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ClearPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
    if (vkdCreatePipelineLayout(dev, &pli, &mClearLayout, "mAlphaKClearLayout") != VK_SUCCESS)
    {
        mClearLayout = VK_NULL_HANDLE;
        mPipeFailed = true;
        return false;
    }

    PipelineBatchScope batchScope(dev);   // (the pipelines below: queued, flushed async at the end)
    // The depth-mask VS twins (RenderVKDepthOnlyVS): the position chain and the alpha test's
    // inputs (+ v_akdraw) from the full VS's own source lines -- invariant gl_Position and uv,
    // so each fragment, its coverage and its texel are pass 2's.
    VkShaderModule vs  = akModule(dev, kWorldVertDepthMaskSpv, kWorldVertDepthMaskSpv_bytes, "kWorldVertDepthMaskSpv");
    VkShaderModule rvs = akModule(dev, kWorldRiggedVertDepthMaskSpv, kWorldRiggedVertDepthMaskSpv_bytes,
                                  "kWorldRiggedVertDepthMaskSpv");
    // (The list FS needs 64-bit buffer atomics: not even its module without them.)
    VkShaderModule fs  = f.lists ? akModule(dev, kWorldAlphaKVisFragSpv, kWorldAlphaKVisFragSpv_bytes, "kWorldAlphaKVisFragSpv")
                                 : VK_NULL_HANDLE;
    VkShaderModule cs  = f.lists ? akModule(dev, kWorldAlphaKClearSpv, kWorldAlphaKClearSpv_bytes, "kWorldAlphaKClearSpv")
                                 : VK_NULL_HANDLE;
    VkShaderModule cfs = akModule(dev, kWorldAlphaCoverFragSpv, kWorldAlphaCoverFragSpv_bytes, "kWorldAlphaCoverFragSpv");
    if (vs && rvs && cfs)
    {
        // NEAR-OPAQUE COVERS: one R32_UINT target, no blending (the ROP keeps the last writer
        // in rasterization order), the main depth read-only with a strict early test. Cull mode
        // dynamic: the recorder replays drawListBlend's face passes (back faces, then front).
        GraphicsPipelineDesc c;
        c.frag = cfs; c.layout = f.drawLayout;
        c.colorFormat = VK_FORMAT_R32_UINT; c.glowFormat = VK_FORMAT_UNDEFINED;
        c.blend = false; c.colorWrite = true;
        c.depthFormat = f.depthFormat; c.depthTest = true; c.depthWrite = false;
        c.depthCompare = VK_COMPARE_OP_LESS; c.reverseZ = f.reverseZ;
        c.cullMode = VK_CULL_MODE_NONE;
        c.dynamicDepthCull = true;
        c.vert = vs;  vkWorldStaticVI(c); createGraphicsPipelineInto(dev, c, &mStage[3], "alpha near-opaque covers (static)");
        c.vert = rvs; vkWorldRiggedVI(c); createGraphicsPipelineInto(dev, c, &mStage[4], "alpha near-opaque covers (rigged)");
    }
    if (vs && rvs && fs && cs)
    {
        // No colour: the FS only stores. The main depth read-only with a strict early test
        // (reversed-Z: in front), so layers wholly behind the opaque surface -- hair behind the
        // head -- never launch the FS (it still tests each covered pixel itself). Both faces (the FS
        // drops single-sided back faces, as the lit FS does).
        GraphicsPipelineDesc d;
        d.frag = fs; d.layout = f.drawLayout;
        d.noColor = true;
        d.depthFormat = f.depthFormat; d.depthTest = true; d.depthWrite = false;
        d.depthCompare = VK_COMPARE_OP_LESS; d.reverseZ = f.reverseZ;
        d.cullMode = VK_CULL_MODE_NONE;
        d.vert = vs;  vkWorldStaticVI(d); createGraphicsPipelineInto(dev, d, &mStage[0], "alpha cover lists (static)");
        d.vert = rvs; vkWorldRiggedVI(d); createGraphicsPipelineInto(dev, d, &mStage[1], "alpha cover lists (rigged)");
        createComputePipelineInto(dev, cs, mClearLayout, 0, nullptr, &mStage[2], "alpha cover lists clear");
    }
    for (VkShaderModule m : {vs, rvs, fs, cs, cfs})
        if (m) destroyShaderModuleDeferred(dev, m, nullptr);
    PipelineJobOpts o;
    o.what = "alpha cover lists";
    o.gate = false;   // (optional: pass 2 lights every layer until they land)
    mJob.start(pipelineBatchFlushAsync(dev, o));
    if (!mJob.pending())
    {
        // Nothing was queued (a module failed): latch it.
        LL_WARNS("VKAlphaK") << "alpha cover list shader modules failed; every blended layer is lit (as before)" << LL_ENDL;
        mPipeFailed = true;
        return false;
    }
    LL_INFOS("VKAlphaK") << "alpha cover list pipelines compiling in the background" << LL_ENDL;
    return false;
}

// One readback entry (a frame's list-pass and pass-2 counts) into the current window.
void LLCleanAlphaKVK::autoSample(uint32_t listed, uint32_t dropped)
{
    mWinListed += listed;
    mWinDropped += dropped;
    if (++mWinFrames < kAutoWindow) return;
    const double ratio = mWinListed ? (double)mWinDropped / (double)mWinListed : 0.0;
    const bool off = mMode == 2 && ratio < kAutoKeep;
    if (off || (++mWindows % 10u) == 0u)
        LL_INFOS("VKAlphaK") << "alpha cover lists: " << (mWinListed / kAutoWindow) << " list fragments / frame, "
                             << (mWinDropped / kAutoWindow) << " pass-2 fragments not lit (" << (S32)(ratio * 100.0)
                             << " %)" << (off ? "; auto: off for a while (below the break-even)" : "") << LL_ENDL;
    if (off) mAutoOffFrames = kAutoOffFrames;
    mWinListed = mWinDropped = 0;
    mWinFrames = 0;
}

// One readback entry of the cover self-check (a frame's pass-2 counts, clean_alpha_k.glsl
// akCoverCounters) into the current window.
void LLCleanAlphaKVK::coverSample(const uint32_t c[8])
{
    mCovWinDropped += c[0];
    mCovWinLit += c[1];
    for (int i = 0; i < 4; ++i) mCovWinCls[i] += c[2 + i];
    if (++mCovWinFrames < kAutoWindow) return;
    // The bisect line (every ~10 s, and the first window): the fragments by class -- the stored
    // key against their own: equal (the last cover itself), later (dropped), none, earlier
    // (a non-cover layer drawn after the last cover: normal, never dropped) -- and, at the screen
    // centre, the last fragment's own key and the stored one, decoded (list 2 bits, face-pass 1,
    // draw 13, triangle 16).
    auto key = [](uint32_t k) {
        std::ostringstream o;
        o << "list " << (k >> 30) << " face " << ((k >> 29) & 1u) << " draw " << ((k >> 16) & 0x1FFFu)
          << " tri " << (k & 0xFFFFu) << " (0x" << std::hex << k << std::dec << ")";
        return o.str();
    };
    const bool noDrop = (mCoverDebug & (8u | 16u)) != 0u;   // (AKD_COUNT_ONLY | AKD_COLOUR: would-be drops)
    if (mCovWindows == 0u || (mCovWindows % 10u) == 0u)
        LL_INFOS("VKAlphaK") << "near-opaque covers (debug " << mCoverDebug << "): per frame " << (mCovWinDropped / kAutoWindow)
                             << (noDrop ? " would drop, " : " dropped, ") << (mCovWinLit / kAutoWindow) << " lit; classes equal "
                             << (mCovWinCls[0] / kAutoWindow) << " later " << (mCovWinCls[1] / kAutoWindow) << " none "
                             << (mCovWinCls[2] / kAutoWindow) << " earlier " << (mCovWinCls[3] / kAutoWindow)
                             << "; centre: own " << key(c[6]) << ", stored " << key(c[7]) << LL_ENDL;
    ++mCovWindows;
    if (!noDrop && mCovWinDropped >= 1000u && mCovWinLit == 0u)
    {
        mCoverBroken = true;
        LL_WARNS("VKAlphaK") << "near-opaque covers dropped " << (mCovWinDropped / kAutoWindow)
                             << " fragments / frame and lit none: their keys disagree with pass 2's -- covers off for"
                                " this session (RenderVKAlphaCoverCull)" << LL_ENDL;
    }
    mCovWinDropped = mCovWinLit = 0;
    for (uint64_t& v : mCovWinCls) v = 0;
    mCovWinFrames = 0;
}

void LLCleanAlphaKVK::freeBuffer()
{
    if (mCtx && mBuf.buffer) mCtx->destroyBuffer(mBuf);
    mBuf = GpuBuffer{};
    mAddr = 0;
    mW = mH = mK = 0;
    mFresh = false;
}

bool LLCleanAlphaKVK::prepare(const Frame& f)
{
    mNow = false;
    mCoverNow = false;
    if (!f.ctx) return false;
    if (f.coverDebug != mCoverDebug)
    {
        // A new bisect setting: a fresh window, and a session-latched self-check failure is retried.
        mCoverDebug = f.coverDebug;
        mCoverBroken = false;
        mCovWinDropped = mCovWinLit = 0;
        for (uint64_t& v : mCovWinCls) v = 0;
        mCovWinFrames = 0; mCovWindows = 0;
    }
    mCtx = f.ctx;
    // AUTO SWITCH: harvest the readback entry this frame's clear reuses (written 3 frames ago:
    // two frames in flight), then honour an "off for a while" decision.
    ++mRbFrame;
    {
        const uint32_t slot = mRbFrame % 3u;
        if (mRbValid[slot] && mRb.mapped)
        {
            const uint32_t* c = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mRb.mapped) + slot * 48u);
            autoSample(c[0], c[1]);
        }
        if (mCoverRbValid[slot] && mRb.mapped)
        {
            const uint32_t* c = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mRb.mapped) + slot * 48u + 16u);
            coverSample(c);
        }
        mRbValid[slot] = false;
        mCoverRbValid[slot] = false;
    }
    if ((f.want || f.wantCover) && !mRb.buffer)
    {
        VkMemScope memcat(VKMEM_READBACK);
        VkDeviceContext::SoftAllocScope soft(f.ctx);
        mRb = f.ctx->createBuffer(144, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                      | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (!mRb.mapped && mRb.buffer) { f.ctx->destroyBuffer(mRb); mRb = GpuBuffer{}; }   // (no readback: no auto / check)
    }
    if (f.mode != mMode) { mMode = f.mode; mAutoOffFrames = 0; mWinListed = mWinDropped = 0; mWinFrames = 0; }
    bool want = f.want && f.lists;
    if (mMode == 2 && mAutoOffFrames > 0) { --mAutoOffFrames; want = false; }
    const bool extentOk = f.rw > 0 && f.rh > 0 && f.rw <= 0xFFFFu && f.rh <= 0xFFFFu;
    const bool wantCover = f.wantCover && !mCoverBroken;
    const bool pipes = (want || wantCover) && extentOk && ensurePipelines(f);
    mCoverNow = pipes && wantCover && prepareCovers(f);
    if (!mCoverNow && (mCoverBuf.buffer || mCoverImg[0].image) && ++mCoverIdle > kIdleFreeFrames)
    {
        vkDeviceWaitIdle(f.ctx->device());   // (frames in flight may still use them)
        LL_INFOS("VKAlphaK") << "near-opaque covers unused: freed" << LL_ENDL;
        freeCovers();
    }
    mNow = prepareLists(f, want && pipes && mBuiltLists);
    return mNow;
}

// The K lists' buffer for this extent (want: lists this frame, pipelines in).
bool LLCleanAlphaKVK::prepareLists(const Frame& f, bool want)
{
    if (!want)
    {
        // Free on liveness: unused ~10 s (the setting off, stereo, snapshots ...).
        if (mBuf.buffer && ++mIdleFrames > kIdleFreeFrames)
        {
            vkDeviceWaitIdle(f.ctx->device());   // (frames in flight may still read it)
            LL_INFOS("VKAlphaK") << "alpha cover lists unused: freed " << (mBuf.size >> 20) << " MB" << LL_ENDL;
            freeBuffer();
        }
        return false;
    }
    mIdleFrames = 0;
    if (!mBuf.buffer || mW != f.rw || mH != f.rh)
    {
        if (mFailures >= kMaxFailures && mFailW == f.rw && mFailH == f.rh) return false;   // given up here
        if (mFailW != f.rw || mFailH != f.rh) { mFailures = 0; mRetryIn = 0; }
        if (mRetryIn > 0) { --mRetryIn; return false; }
        // K TIER: the most entries per pixel and side (kMaxK .. 2) whose lists fit 1/48 of the
        // device-local budget, at most 512 MB (8 GB: 2 at 4K, 4 at 1440p; 16 GB+: 3-4 at 4K).
        const uint64_t px = (uint64_t)f.rw * f.rh;
        // (+ the tile words, 16-byte aligned, + the auto switch's 16 B of counters)
        const uint64_t tiles = (((uint64_t)((f.rw + 7u) / 8u) * ((f.rh + 7u) / 8u) * 4u + 15u) & ~15ull) + 16u;
        const uint64_t budget = std::min<uint64_t>(f.ctx->deviceLocalBudgetBytes() / 48u, 512ull << 20);
        uint32_t k = 0;
        uint64_t need = 0;
        for (uint32_t c = kMaxK; c >= 2u; --c)
        {
            need = px * 2u * c * 8u + tiles;
            if (need <= budget) { k = c; break; }
        }
        mFailW = f.rw; mFailH = f.rh;
        if (!k)
        {
            LL_INFOS("VKAlphaK") << "alpha cover lists off at " << f.rw << "x" << f.rh << ": 2 entries need "
                                 << ((px * 32u + tiles) >> 20) << " MB, over the " << (budget >> 20) << " MB budget"
                                 << LL_ENDL;
            mFailures = kMaxFailures;
            return false;
        }
        // VRAM PRESSURE: not while the heap is within 10 % of its budget (retried later).
        uint64_t use = 0, bud = 0;
        if (f.ctx->liveDeviceBudget(use, bud) && bud > 0 && use + need > bud - bud / 10u)
        {
            LL_INFOS("VKAlphaK") << "alpha cover lists deferred: VRAM " << (use >> 20) << " / " << (bud >> 20)
                                 << " MB, they need " << (need >> 20) << " MB" << LL_ENDL;
            mRetryIn = kRetryFrames;
            return false;
        }
        if (mBuf.buffer)
        {
            vkDeviceWaitIdle(f.ctx->device());   // a resize (rare): frames in flight may still read it
            freeBuffer();
        }
        {
            VkMemScope memcat(VKMEM_TARGET);
            VkDeviceContext::SoftAllocScope soft(f.ctx);
            VkDeviceContext::NoReliefScope noRelief;   // optional: never pushes the VRAM ladder
            mBuf = f.ctx->createBuffer(need, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
        }
        if (!mBuf.buffer)
        {
            ++mFailures;
            mRetryIn = kRetryFrames;
            LL_WARNS("VKAlphaK") << "alpha cover lists: " << (need >> 20) << " MB allocation failed (" << mFailures
                                 << "/" << kMaxFailures << ")" << (mFailures >= kMaxFailures ? "; off at this resolution" : "; retrying later")
                                 << LL_ENDL;
            mBuf = GpuBuffer{};
            return false;
        }
        mFailures = 0;
        VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        ai.buffer = mBuf.buffer;
        mAddr = vkGetBufferDeviceAddress(f.ctx->device(), &ai);
        mW = f.rw; mH = f.rh; mK = k;
        mFresh = true;
        LL_INFOS("VKAlphaK") << "alpha cover lists: " << f.rw << "x" << f.rh << ", K " << k << ", "
                             << (need >> 20) << " MB" << LL_ENDL;
    }
    return mAddr != 0;
}

void LLCleanAlphaKVK::freeCovers()
{
    if (mCtx)
    {
        for (GpuImage& im : mCoverImg)
            if (im.image) mCtx->destroyImage(im);
        if (mCoverBuf.buffer) mCtx->destroyBuffer(mCoverBuf);
    }
    for (GpuImage& im : mCoverImg) im = GpuImage{};
    mCoverBuf = GpuBuffer{};
    mCoverAddr = 0;
    mCW = mCH = 0;
    mCoverCountsValid = false;   // (a new buffer: its counters are garbage until the first fill)
}

// The covers' buffer: the two sides' keys, then (16-byte aligned) the self-check's counters
// (clean_alpha_k.glsl akCoverCounters).
static VkDeviceSize coverCounterOffset(uint32_t w, uint32_t h)
{
    return ((VkDeviceSize)w * h * 2u * 4u + 15u) & ~(VkDeviceSize)15u;
}

// The covers' targets + buffer for this extent: 2 x 4 B per pixel each (the same budget, VRAM
// pressure, retry and give-up rules as the lists; freed after ~10 s unused).
bool LLCleanAlphaKVK::prepareCovers(const Frame& f)
{
    mCoverIdle = 0;
    if (mCoverImg[0].image && mCoverImg[1].image && mCoverBuf.buffer && mCW == f.rw && mCH == f.rh) return true;
    if (mCoverFailures >= kMaxFailures && mCoverFailW == f.rw && mCoverFailH == f.rh) return false;
    if (mCoverFailW != f.rw || mCoverFailH != f.rh) { mCoverFailures = 0; mCoverRetryIn = 0; }
    if (mCoverRetryIn > 0) { --mCoverRetryIn; return false; }
    mCoverFailW = f.rw; mCoverFailH = f.rh;
    const uint64_t need = (uint64_t)f.rw * f.rh * 4u * 4u;   // 2 targets + their buffer copy
    const uint64_t budget = std::min<uint64_t>(f.ctx->deviceLocalBudgetBytes() / 48u, 512ull << 20);
    if (need > budget)
    {
        LL_INFOS("VKAlphaK") << "near-opaque covers off at " << f.rw << "x" << f.rh << ": " << (need >> 20)
                             << " MB over the " << (budget >> 20) << " MB budget" << LL_ENDL;
        mCoverFailures = kMaxFailures;
        return false;
    }
    uint64_t use = 0, bud = 0;
    if (f.ctx->liveDeviceBudget(use, bud) && bud > 0 && use + need > bud - bud / 10u)
    {
        mCoverRetryIn = kRetryFrames;
        return false;
    }
    if (mCoverBuf.buffer || mCoverImg[0].image || mCoverImg[1].image)
    {
        vkDeviceWaitIdle(f.ctx->device());   // a resize (rare): frames in flight may still use them
        freeCovers();
    }
    {
        VkMemScope memcat(VKMEM_TARGET);
        VkDeviceContext::SoftAllocScope soft(f.ctx);
        VkDeviceContext::NoReliefScope noRelief;
        for (GpuImage& im : mCoverImg)
            im = f.ctx->createImage2D(f.rw, f.rh, VK_FORMAT_R32_UINT,
                                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                      VK_IMAGE_ASPECT_COLOR_BIT);
        mCoverBuf = f.ctx->createBuffer(coverCounterOffset(f.rw, f.rh) + 32u,   // (+ the self-check counters)
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
    }
    if (!mCoverImg[0].image || !mCoverImg[1].image || !mCoverBuf.buffer || !mCoverImg[0].view || !mCoverImg[1].view)
    {
        freeCovers();
        ++mCoverFailures;
        mCoverRetryIn = kRetryFrames;
        LL_WARNS("VKAlphaK") << "near-opaque covers: " << (need >> 20) << " MB allocation failed (" << mCoverFailures
                             << "/" << kMaxFailures << ")" << LL_ENDL;
        return false;
    }
    mCoverFailures = 0;
    VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    ai.buffer = mCoverBuf.buffer;
    mCoverAddr = vkGetBufferDeviceAddress(f.ctx->device(), &ai);
    mCW = f.rw; mCH = f.rh;
    LL_INFOS("VKAlphaK") << "near-opaque covers: " << f.rw << "x" << f.rh << ", " << (need >> 20) << " MB" << LL_ENDL;
    return mCoverAddr != 0;
}

void LLCleanAlphaKVK::fillCoverInfo(uint32_t out[4]) const
{
    out[0] = mCoverNow ? (uint32_t)(mCoverAddr & 0xFFFFFFFFull) : 0u;
    out[1] = mCoverNow ? (uint32_t)(mCoverAddr >> 32) : 0u;
    out[2] = mCoverNow ? (mCW | (mCH << 16)) : 0u;
    out[3] = mCoverNow ? mCoverDebug : 0u;   // RenderVKAlphaCoverDebug (clean_alpha_k.glsl AKD_*)
}

void LLCleanAlphaKVK::beginCover(VkCommandBuffer cmd, uint32_t side, VkImageView depthView)
{
    GpuImage& im = mCoverImg[side ? 1 : 0];
    // Last frame's copy (transfer read) -> this frame's clear + writes; the contents are dropped.
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; b.srcAccessMask = 0;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.image = im.image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = im.view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color.uint32[0] = 0u;
    // The main depth read-only, also sampled elsewhere in the frame: STORE_OP_NONE (as pass 2).
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    da.imageView = depthView; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {mCW, mCH}}; ri.layerCount = 1;
    ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca; ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);
}

void LLCleanAlphaKVK::recordCoverDone(VkCommandBuffer cmd)
{
    VkImageMemoryBarrier2 ib[2]{};
    for (int i = 0; i < 2; ++i)
    {
        ib[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        ib[i].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        ib[i].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        ib[i].dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; ib[i].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        ib[i].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ib[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        ib[i].image = mCoverImg[i].image; ib[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    // Last frame's pass-2 reads (and counter atomics) of the buffer -> these copies.
    VkBufferMemoryBarrier2 bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    bb.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; bb.srcAccessMask = kStorageRW;
    bb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    bb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    bb.buffer = mCoverBuf.buffer; bb.size = VK_WHOLE_SIZE;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 2; d.pImageMemoryBarriers = ib;
    d.bufferMemoryBarrierCount = 1; d.pBufferMemoryBarriers = &bb;
    vkCmdPipelineBarrier2(cmd, &d);
    for (uint32_t i = 0; i < 2; ++i)
    {
        VkBufferImageCopy c{};
        c.bufferOffset = (VkDeviceSize)i * mCW * mCH * 4u;   // covers[(side * H + y) * W + x]
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {mCW, mCH, 1};
        vkCmdCopyImageToBuffer(cmd, mCoverImg[i].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mCoverBuf.buffer, 1, &c);
    }
    // The previous frame's self-check counters -> the readback ring, then zeroed.
    const VkDeviceSize cntOff = coverCounterOffset(mCW, mCH);
    if (mRb.buffer)
    {
        const uint32_t slot = mRbFrame % 3u;
        const VkBufferCopy bc{cntOff, slot * 48u + 16u, 32u};
        vkCmdCopyBuffer(cmd, mCoverBuf.buffer, mRb.buffer, 1, &bc);
        VkBufferMemoryBarrier2 hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        hb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; hb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        hb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; hb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        hb.buffer = mRb.buffer; hb.size = VK_WHOLE_SIZE;
        VkDependencyInfo hd{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; hd.bufferMemoryBarrierCount = 1; hd.pBufferMemoryBarriers = &hb;
        vkCmdPipelineBarrier2(cmd, &hd);
        mCoverRbValid[slot] = mCoverCountsValid;
    }
    // (copy -> fill: both transfer writes/reads of the same range, ordered)
    bufferBarrier(cmd, mCoverBuf.buffer, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(cmd, mCoverBuf.buffer, cntOff, 32u, 0u);
    mCoverCountsValid = true;   // (from now on the counters hold one frame's counts)
    bufferBarrier(cmd, mCoverBuf.buffer, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, kStorageRW);
}

void LLCleanAlphaKVK::fillFrameInfo(uint32_t out[4]) const
{
    out[0] = mNow ? (uint32_t)(mAddr & 0xFFFFFFFFull) : 0u;
    out[1] = mNow ? (uint32_t)(mAddr >> 32) : 0u;
    out[2] = mNow ? (mW | (mH << 16)) : 0u;
    out[3] = mNow ? mK : 0u;
}

void LLCleanAlphaKVK::recordClear(VkCommandBuffer cmd)
{
    if (!mNow) return;
    if (mFresh)
    {
        // A new buffer: zero it whole (entries + tile words) once.
        vkCmdFillBuffer(cmd, mBuf.buffer, 0, VK_WHOLE_SIZE, 0u);
        bufferBarrier(cmd, mBuf.buffer, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, kStorageRW);
        mFresh = false;
        return;
    }
    // The previous frame's counters (its list pass + pass 2) -> the readback ring, then zeroed.
    const VkDeviceSize cntOff = mBuf.size - 16u;
    bufferBarrier(cmd, mBuf.buffer, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, kStorageRW,
                  VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (mRb.buffer)
    {
        const uint32_t slot = mRbFrame % 3u;
        const VkBufferCopy bc{cntOff, slot * 48u, 16u};
        vkCmdCopyBuffer(cmd, mBuf.buffer, mRb.buffer, 1, &bc);
        VkBufferMemoryBarrier2 hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        hb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; hb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        hb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; hb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        hb.buffer = mRb.buffer; hb.size = VK_WHOLE_SIZE;
        VkDependencyInfo hd{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; hd.bufferMemoryBarrierCount = 1; hd.pBufferMemoryBarriers = &hb;
        vkCmdPipelineBarrier2(cmd, &hd);
        mRbValid[slot] = true;
    }
    vkCmdFillBuffer(cmd, mBuf.buffer, cntOff, 16u, 0u);
    // The previous lists' stores and reads (fragment) and the counters' reset -> the clear and
    // this frame's list pass.
    bufferBarrier(cmd, mBuf.buffer, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                  kStorageRW | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, kStorageRW);
    ClearPush p{};
    p.entries[0] = (uint32_t)(mAddr & 0xFFFFFFFFull);
    p.entries[1] = (uint32_t)(mAddr >> 32);
    p.width = mW; p.height = mH; p.k = mK;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mClearPipe);
    vkCmdPushConstants(cmd, mClearLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cmd, (mW + 7u) / 8u, (mH + 7u) / 8u, 1u);
    bufferBarrier(cmd, mBuf.buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kStorageRW,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, kStorageRW);
}

void LLCleanAlphaKVK::recordListsDone(VkCommandBuffer cmd)
{
    if (!mNow) return;
    // The list pass's atomics -> pass 2's alpha FS reads.
    bufferBarrier(cmd, mBuf.buffer, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, kStorageRW,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

void LLCleanAlphaKVK::destroy(VkDevice dev)
{
    mJob.wait();
    for (VkPipeline& p : mStage)
        if (p) { vkForgetPipeline(p); vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    for (VkPipeline& p : mVisPipe)
        if (p) { vkForgetPipeline(p); vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    if (mClearPipe) { vkDestroyPipeline(dev, mClearPipe, nullptr); mClearPipe = VK_NULL_HANDLE; }
    if (mClearLayout) { vkDestroyPipelineLayout(dev, mClearLayout, nullptr); mClearLayout = VK_NULL_HANDLE; }
    mBuiltLayout = VK_NULL_HANDLE;
    mBuiltDepth = VK_FORMAT_UNDEFINED;
    mPipeFailed = false;
    freeBuffer();
    freeCovers();
    mCoverNow = false;
    for (VkPipeline& p : mCoverPipe)
        if (p) { vkForgetPipeline(p); vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    mBuiltLists = false;
    if (mRb.buffer && mCtx) mCtx->destroyBuffer(mRb);
    mRb = GpuBuffer{};
    for (bool& v : mRbValid) v = false;
    mNow = false;
}
