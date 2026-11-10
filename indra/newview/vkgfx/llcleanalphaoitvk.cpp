/**
 * @file llcleanalphaoitvk.cpp
 * @brief Weighted blended order-independent transparency (#1410, RenderVKAlphaOIT): see
 *        llcleanalphaoitvk.h, clean_oit.glsl and clean_oit_resolve.frag.
 */
#include "linden_common.h"

#include "llcleanalphaoitvk.h"
#include "vk_world_shaders_spv.h"

#include "llerror.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace
{
double oitClock()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

VkShaderModule oitModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    vkNoteShaderModule(m, code, bytes, name);
    return m;
}

// Attachments of a mode's accumulation scope, in FS output order (target indices).
uint32_t oitAttachments(uint32_t mode, uint32_t (&idx)[5])
{
    if (mode == 1u) { idx[0] = 0; idx[1] = 1; idx[2] = 4; return 3u; }
    for (uint32_t i = 0; i < 5u; ++i) idx[i] = i;
    return 5u;
}
} // namespace

bool LLCleanAlphaOITVK::ensurePipelines(const Build& b)
{
    if (!b.ctx || !b.rigDesc[0].layout) return false;
    VkDevice dev = b.ctx->device();
    // A finished background job: install what it built (replacing the previous set, which may
    // be in flight: one device wait, a setting change, never per frame).
    if (mJob.poll())
    {
        const bool ok = mAccumStage[0][1][0] && mAccumStage[1][1][0] && mResolveStage[0] && mResolveStage[1];
        if (ok)
        {
            if (mReady) destroyPipelines(dev);
            std::memcpy(mAccum, mAccumStage, sizeof(mAccum));
            std::memcpy(mResolve, mResolveStage, sizeof(mResolve));
            mResolveRt = mResolveRtStage;
            mResolveRtStage = VK_NULL_HANDLE;
            std::memset(mAccumStage, 0, sizeof(mAccumStage));
            std::memset(mResolveStage, 0, sizeof(mResolveStage));
            mReady = true;
        }
        else destroyStage(dev);
        LL_INFOS("VKAlphaOIT") << "WBOIT pipelines " << (ok ? "built" : (mReady ? "FAILED (the previous set stays)" : "FAILED (the ordered path stays)"))
                               << " in " << (S32)((oitClock() - mJobSince) * 1000.0) << " ms (background)" << LL_ENDL;
    }
    if (mJob.pending()) return mReady;
    const bool fmtChanged = mBuiltFormats[0] != b.hdrFormat || mBuiltFormats[1] != b.glowFormat
                         || mBuiltFormats[2] != b.reactFormat || mBuiltFormats[3] != b.depthFormat;
    if (mBuiltKey == b.specKey && !fmtChanged) return mReady;
    // The generic key with a set built: kept (no setting constants to match -- the generic lit
    // pipelines read the live values; a specialised set's folded ones are the same values).
    if (mReady && !fmtChanged && b.generic) { mBuiltKey = b.specKey; return true; }
    // A pass format change invalidates the set at once; a key change keeps it until the new lands.
    if (fmtChanged && mReady) destroyPipelines(dev);
    mCtx = b.ctx;
    if (!mDsl)
    {
        VkDescriptorSetLayoutBinding sb[6]{};
        for (uint32_t i = 0; i < 6; ++i)
        {
            sb[i].binding = i;
            sb[i].descriptorType = i < 5 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            sb[i].descriptorCount = 1;
            sb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 6; lci.pBindings = sb;
        VK_CHECK(vkdCreateSetLayout(dev, &lci, &mDsl, "mDslAlphaOIT"));
        VK_CHECK(mPool.create(dev, "alphaoit", {{mDsl, 1}}));
        VKD_ALLOC_SOFT(mPool.allocate(mDsl, &mDset), { destroy(dev); return false; });
    }
    if (!mStats.buffer)
    {
        mStats = mCtx->createBuffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                    | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        mStatsRb = mCtx->createBuffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mStatsRb.mapped) std::memset(mStatsRb.mapped, 0, 16);
        mStatsLast = 0;
        mStatsZeroed = false;
    }
    if (!mResolveLayout)
    {
        const VkDescriptorSetLayout sets[3] = {b.dslDraw0, b.dslDraw1, mDsl};
        VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, b.pushBytes};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 3; pli.pSetLayouts = sets;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
        VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mResolveLayout, "mAlphaOITResolveLayout"));
        if (b.dslRt)
        {
            const VkDescriptorSetLayout setsRt[4] = {b.dslDraw0, b.dslDraw1, mDsl, b.dslRt};
            pli.setLayoutCount = 4; pli.pSetLayouts = setsRt;
            VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mResolveLayoutRt, "mAlphaOITResolveLayoutRt"));
        }
    }
    writeSet();

    {
        PipelineBatchScope batchScope(dev);   // queued here, compiled by the async job below
        VkShaderModule vs   = oitModule(dev, kWorldVertSpv, kWorldVertSpv_bytes, "Vert");
        VkShaderModule rvs  = oitModule(dev, kWorldRiggedVertSpv, kWorldRiggedVertSpv_bytes, "RiggedVert");
        VkShaderModule acc  = oitModule(dev, kWorldFragOitAccumSpv, kWorldFragOitAccumSpv_bytes, "FragOitAccum");
        VkShaderModule accr = b.rt ? oitModule(dev, kWorldFragOitAccumRtSpv, kWorldFragOitAccumRtSpv_bytes, "FragOitAccumRt")
                                   : VK_NULL_HANDLE;
        VkShaderModule gb   = oitModule(dev, kWorldFragOitGbufSpv, kWorldFragOitGbufSpv_bytes, "FragOitGbuf");
        VkShaderModule fsv  = oitModule(dev, kWorldGlowCombineVertSpv, kWorldGlowCombineVertSpv_bytes, "GlowCombineVert");
        VkShaderModule res1 = oitModule(dev, kWorldOitResolveFragSpv, kWorldOitResolveFragSpv_bytes, "OitResolveFrag");
        VkShaderModule res2 = oitModule(dev, kWorldOitResolveLitFragSpv, kWorldOitResolveLitFragSpv_bytes, "OitResolveLitFrag");
        VkShaderModule res2r = mResolveLayoutRt ? oitModule(dev, kWorldOitResolveLitRtFragSpv, kWorldOitResolveLitRtFragSpv_bytes,
                                                            "OitResolveLitRtFrag") : VK_NULL_HANDLE;
        // Accumulation: the ordered pipeline's state (vertex input, depth test, VRS), the OIT
        // FS, the accumulation targets with the WBOIT blend (vk_pipeline.h oitMrt). One view.
        for (uint32_t m = 1; m <= 2u; ++m)
        {
            uint32_t idx[5];
            const uint32_t n = oitAttachments(m, idx);
            for (uint32_t rig = 0; rig < 2u; ++rig)
                for (uint32_t rt = 0; rt < 2u; ++rt)
                {
                    if (rt && (m == 2u || !b.rt)) continue;   // (mode 2 never traces)
                    GraphicsPipelineDesc d = rig ? b.rigDesc[rt] : b.statDesc[rt];
                    if (!d.layout) continue;
                    d.vert = rig ? rvs : vs;
                    d.frag = m == 1u ? (rt ? accr : acc) : gb;
                    d.fragSpec = b.spec;
                    d.views = 1;
                    d.mrtCount = n;
                    for (uint32_t i = 0; i < n; ++i) d.mrtFormats[i] = idx[i] == 4u ? kRevealFormat : kSumFormat;
                    d.oitMrt = true;
                    createGraphicsPipelineInto(dev, d, &mAccumStage[m - 1][rig][rt], "WBOIT accumulation");
                }
        }
        // Resolve: a fullscreen triangle in pass 2 -- dst * revealage + value (ONE / SRC_ALPHA,
        // the HDR alpha kept), glow likewise, coverage MAX into the reactive mask (R).
        for (uint32_t m = 1; m <= 2u; ++m)
        {
            GraphicsPipelineDesc d;
            d.vert = fsv; d.frag = m == 1u ? res1 : res2; d.layout = mResolveLayout;
            d.fragSpec = m == 2u ? b.spec : nullptr;   // (mode 2: the lighting's setting constants)
            d.colorFormat = b.hdrFormat;
            d.glowFormat = b.glowFormat;
            d.reactFormat = b.reactFormat; d.reactWriteMask = VK_COLOR_COMPONENT_R_BIT;
            d.depthFormat = b.depthFormat; d.depthTest = false; d.depthWrite = false;
            d.customBlend = true; d.srcColor = VK_BLEND_FACTOR_ONE; d.dstColor = VK_BLEND_FACTOR_SRC_ALPHA;
            d.cullMode = VK_CULL_MODE_NONE;
            d.fsrAttachment = b.rigDesc[0].fsrAttachment;
            createGraphicsPipelineInto(dev, d, &mResolveStage[m - 1], "WBOIT resolve");
            if (m == 2u && res2r)
            {
                d.frag = res2r; d.layout = mResolveLayoutRt;
                createGraphicsPipelineInto(dev, d, &mResolveRtStage, "WBOIT resolve (rayquery)");
            }
        }
        for (VkShaderModule mod : {vs, rvs, acc, accr, gb, fsv, res1, res2, res2r})
            if (mod) destroyShaderModuleDeferred(dev, mod, nullptr);
        PipelineJobOpts o;
        o.prio = VKPIPE_PRIO_USER;   // the user switched it on and looks at the ordered path meanwhile
        o.gate = false;
        o.what = "WBOIT";
        mJob.start(pipelineBatchFlushAsync(dev, o));
    }
    mBuiltKey = b.specKey;
    mBuiltFormats[0] = b.hdrFormat; mBuiltFormats[1] = b.glowFormat;
    mBuiltFormats[2] = b.reactFormat; mBuiltFormats[3] = b.depthFormat;
    mJobSince = oitClock();
    LL_INFOS("VKAlphaOIT") << "WBOIT pipelines compiling in the background (spec key " << std::hex << b.specKey << std::dec
                           << (b.rt ? ", with the ray-query mode-1 twins" : "")
                           << (mReady ? "); the previous set until they land" : "); the ordered path until they land") << LL_ENDL;
    return mReady;
}

VkPipeline LLCleanAlphaOITVK::accumPipe(uint32_t mode, bool rigged, bool rt) const
{
    if (!mReady || mode < 1u || mode > 2u) return VK_NULL_HANDLE;
    const VkPipeline p = mAccum[mode - 1][rigged ? 1 : 0][(rt && mode == 1u) ? 1 : 0];
    return p ? p : mAccum[mode - 1][rigged ? 1 : 0][0];   // (a ray-query twin that failed: the plain one)
}

bool LLCleanAlphaOITVK::ensureTargets(VkDeviceContext* ctx, uint32_t mode, uint32_t rw, uint32_t rh)
{
    if (!ctx || !mDset || mode < 1u || mode > 2u || rw < 1u || rh < 1u) return false;
    if (mMode == mode && mW == rw && mH == rh && mTarget[0].image) return true;
    const double now = oitClock();
    if (now < mRetryAt) return false;
    mCtx = ctx;
    vkDeviceWaitIdle(ctx->device());   // the set / images may be in use by frames in flight
    freeTargets();
    {
        VkMemScope memcat(VKMEM_TARGET);
        VkDeviceContext::SoftAllocScope soft(ctx);   // out of memory: the ordered path, not a crash
        const VkImageUsageFlags u = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        static const char* const kNames[5] = {"alphaOitSum0", "alphaOitSum1", "alphaOitSum2", "alphaOitSum3", "alphaOitReveal"};
        bool ok = true;
        for (uint32_t i = 0; i < 5u && ok; ++i)
        {
            if (mode == 1u && (i == 2u || i == 3u)) continue;
            mTarget[i] = ctx->createImage2D(rw, rh, i == 4u ? kRevealFormat : kSumFormat, u, VK_IMAGE_ASPECT_COLOR_BIT);
            ok = mTarget[i].image != VK_NULL_HANDLE;
            if (ok) ctx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mTarget[i].image, kNames[i]);
        }
        if (!ok)
        {
            freeTargets();
            mRetryAt = now + 5.0;
            LL_WARNS("VKAlphaOIT") << "WBOIT: target allocation failed (" << rw << "x" << rh << ", mode " << mode
                                   << "); the ordered path, retry in 5 s" << LL_ENDL;
            return false;
        }
    }
    mMode = mode; mW = rw; mH = rh;
    // No initial transition: every round enters from UNDEFINED (beginAccum clears) and leaves
    // SHADER_READ_ONLY_OPTIMAL (endAccum) before the resolve samples them.
    writeSet();
    LL_INFOS("VKAlphaOIT") << "WBOIT targets: mode " << mode << ", " << rw << "x" << rh << ", "
                           << (mode == 1u ? 18 : 34) << " B/px = " << (targetBytes() >> 20) << " MB" << LL_ENDL;
    return true;
}

uint64_t LLCleanAlphaOITVK::targetBytes() const
{
    if (!mTarget[0].image) return 0;
    const uint64_t px = (uint64_t)mW * mH;
    return px * (mMode == 1u ? 18u : 34u);
}

void LLCleanAlphaOITVK::writeSet()
{
    if (!mCtx || !mDset) return;
    VkDescriptorImageInfo ii[5]{};
    VkWriteDescriptorSet wr[6]{};
    uint32_t n = 0;
    if (mTarget[0].image)
    {
        for (uint32_t i = 0; i < 5u; ++i)
        {
            // (mode 1 has no targets 2 / 3: target 0 stands in -- the mode-1 resolve never reads them)
            const GpuImage& t = mTarget[i].image ? mTarget[i] : mTarget[0];
            ii[i] = {VK_NULL_HANDLE, t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            wr[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[n].dstSet = mDset; wr[n].dstBinding = i;
            wr[n].descriptorCount = 1; wr[n].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; wr[n].pImageInfo = &ii[i];
            ++n;
        }
    }
    VkDescriptorBufferInfo bi{mStats.buffer, 0, VK_WHOLE_SIZE};
    if (mStats.buffer)
    {
        wr[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[n].dstSet = mDset; wr[n].dstBinding = 5;
        wr[n].descriptorCount = 1; wr[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[n].pBufferInfo = &bi;
        ++n;
    }
    if (n) vkUpdateDescriptorSets(mCtx->device(), n, wr, 0, nullptr);
}

void LLCleanAlphaOITVK::beginAccum(VkCommandBuffer cmd, VkImageView depthView)
{
    uint32_t idx[5];
    const uint32_t n = oitAttachments(mMode, idx);
    // The previous resolve's reads (this frame's other water side, or last frame) -> this
    // round's cleared writes (contents discarded).
    VkImageMemoryBarrier2 b[5]{};
    for (uint32_t i = 0; i < n; ++i)
    {
        b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b[i].srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; b[i].srcAccessMask = 0;
        b[i].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        b[i].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
        b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b[i].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b[i].image = mTarget[idx[i]].image; b[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.imageMemoryBarrierCount = n; d.pImageMemoryBarriers = b;
    vkCmdPipelineBarrier2(cmd, &d);

    VkRenderingAttachmentInfo ca[5]{};
    for (uint32_t i = 0; i < n; ++i)
    {
        ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        ca[i].imageView = mTarget[idx[i]].view; ca[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        const float c = idx[i] == 4u ? 1.f : 0.f;   // sums 0, revealage 1
        ca[i].clearValue.color = {{c, c, c, c}};
    }
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    da.imageView = depthView; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_NONE;   // read-only (also sampled)
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {mW, mH}}; ri.layerCount = 1;
    ri.colorAttachmentCount = n; ri.pColorAttachments = ca; ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);
}

void LLCleanAlphaOITVK::endAccum(VkCommandBuffer cmd)
{
    vkCmdEndRendering(cmd);
    uint32_t idx[5];
    const uint32_t n = oitAttachments(mMode, idx);
    VkImageMemoryBarrier2 b[5]{};
    for (uint32_t i = 0; i < n; ++i)
    {
        b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b[i].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        b[i].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        b[i].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b[i].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b[i].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; b[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b[i].image = mTarget[idx[i]].image; b[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.imageMemoryBarrierCount = n; d.pImageMemoryBarriers = b;
    vkCmdPipelineBarrier2(cmd, &d);
}

void LLCleanAlphaOITVK::recordResolve(VkCommandBuffer cmd, VkDescriptorSet rtSet)
{
    VkPipeline p = (mReady && mMode >= 1u && mMode <= 2u) ? mResolve[mMode - 1] : VK_NULL_HANDLE;
    if (!p || !mDset) return;
    // Mode 2 in a tracing frame: the ray-query twin (sets 2 + 3). Plain bind (no dynamic cull /
    // depth state); viewport + scissor come from the pass.
    if (mMode == 2u && rtSet && mResolveRt && mResolveLayoutRt)
    {
        const VkDescriptorSet s2[2] = {mDset, rtSet};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mResolveRt);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mResolveLayoutRt, 2, 2, s2, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        return;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mResolveLayout, 2, 1, &mDset, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void LLCleanAlphaOITVK::recordStatsCopy(VkCommandBuffer cmd)
{
    if (!mStats.buffer || !mStatsRb.buffer) return;
    if (!mStatsZeroed)
    {   // (device-local memory starts undefined: zero the running total once)
        vkCmdFillBuffer(cmd, mStats.buffer, 0, 16, 0);
        mStatsZeroed = true;
    }
    // Earlier resolves' atomics (last frame) -> the copy; the copy -> this frame's atomics.
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &d);
    const VkBufferCopy c{0, 0, 4};
    vkCmdCopyBuffer(cmd, mStats.buffer, mStatsRb.buffer, 1, &c);
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; mb.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &d);
}

uint64_t LLCleanAlphaOITVK::takeCoveredPixels()
{
    if (!mStatsRb.mapped) return 0;
    // (A running uint32 the resolves add to, copied back a frame or more late; unsigned
    // wrap-around keeps the difference right.)
    const uint32_t now = *static_cast<volatile const uint32_t*>(mStatsRb.mapped);
    const uint32_t d = now - mStatsLast;
    mStatsLast = now;
    return d;
}

void LLCleanAlphaOITVK::freeTargets()
{
    if (!mCtx) return;
    for (GpuImage& t : mTarget)
        if (t.image) { mCtx->destroyImage(t); t = GpuImage{}; }
    mMode = 0; mW = mH = 0;
}

void LLCleanAlphaOITVK::destroyStage(VkDevice dev)
{
    for (auto& a : mAccumStage) for (auto& b : a) for (VkPipeline& p : b) { if (p) { vkForgetPipeline(p); vkDestroyPipeline(dev, p, nullptr); } p = VK_NULL_HANDLE; }
    for (VkPipeline& p : mResolveStage) { if (p) vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    if (mResolveRtStage) vkDestroyPipeline(dev, mResolveRtStage, nullptr);
    mResolveRtStage = VK_NULL_HANDLE;
}

// The live set (the device idle first: frames in flight may use it).
void LLCleanAlphaOITVK::destroyPipelines(VkDevice dev)
{
    vkDeviceWaitIdle(dev);
    for (auto& a : mAccum) for (auto& b : a) for (VkPipeline& p : b) { if (p) { vkForgetPipeline(p); vkDestroyPipeline(dev, p, nullptr); } p = VK_NULL_HANDLE; }
    for (VkPipeline& p : mResolve) { if (p) vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    if (mResolveRt) vkDestroyPipeline(dev, mResolveRt, nullptr);
    mResolveRt = VK_NULL_HANDLE;
    mReady = false;
}

void LLCleanAlphaOITVK::destroy(VkDevice dev)
{
    if (!dev) return;
    mJob.wait();
    destroyStage(dev);
    destroyPipelines(dev);   // (waits for the device)
    if (mResolveLayout) { vkDestroyPipelineLayout(dev, mResolveLayout, nullptr); mResolveLayout = VK_NULL_HANDLE; }
    if (mResolveLayoutRt) { vkDestroyPipelineLayout(dev, mResolveLayoutRt, nullptr); mResolveLayoutRt = VK_NULL_HANDLE; }
    mBuiltKey = ~0ull;
    for (VkFormat& f : mBuiltFormats) f = VK_FORMAT_UNDEFINED;
    freeTargets();
    if (mStats.buffer && mCtx) { mCtx->destroyBuffer(mStats); mStats = GpuBuffer{}; }
    if (mStatsRb.buffer && mCtx) { mCtx->destroyBuffer(mStatsRb); mStatsRb = GpuBuffer{}; }
    mPool.destroy();
    if (mDsl) vkdDestroySetLayout(dev, mDsl);
    mDsl = VK_NULL_HANDLE; mDset = VK_NULL_HANDLE;
}
