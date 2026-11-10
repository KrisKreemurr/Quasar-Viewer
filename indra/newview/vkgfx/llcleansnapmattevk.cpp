/**
 * @file llcleansnapmattevk.cpp
 * @brief The transparent avatar layer for the Vulkan clean renderer: the warm-up's world
 *        occluder, the holdout of each avatar-only recording, its straight-alpha tonemap.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Copyright (C) 2026, squeedledorf (GunMetal-DX); SoapStorm (Soap Frenzy).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

// Ported from GunMetal-DX (squeedledorf, LGPL 2.1), v0.3.0 cf5f1f4 (gmdxsnapmattevk.cpp); #1328
// moved the layer's alpha into the colour chain.
// THE TRANSPARENT AVATAR LAYER: the renderer half of llsnapshotavatars.h (SoapStorm's GL
// transparent-avatar / holdout-matte capture, re-done on the Vulkan path). Defines the
// LLCleanRendererVK members of its "AVATAR LAYER SNAPSHOTS" block; renderOffscreen calls them
// around each recording of an avatar layer's offscreen sequence:
//  - recording 0 (the warm-up every offscreen sequence has) renders the normal view -- world and
//    every avatar; no alpha-blend depth pre-pass in the whole sequence, so its depth is the
//    OPAQUE scene's. snapMatteRecord keeps that depth on the GPU (clean_snap_holdout.comp
//    SNAP_WARM);
//  - recordings 1..N have mSnapMatte: the main view keeps the layer's avatars only, the world --
//    the avatars it leaves out included (computeAvatarVis files them off the main view, the
//    cluster selection hides them there; you: mHideSelf) -- still shadows and lights them
//    (shadows, probes, the mirror and the TLAS keep it). The HDR scene's alpha is their COVERAGE:
//    recordSnapHoldout, after the opaque scene and the composites over it, finds the WORLD
//    OCCLUDER of each pixel -- the warm-up's surface, unless this recording's avatar-only depth
//    shows one of the layer's avatars there (a 3x3 match: the recordings are jittered) --, clears
//    what is no avatar surface or is behind it and sets A = 1 on the rest, then writes the
//    occluder into the depth so each transparent fragment the world is in front of fails its own
//    depth test (a hair fringe stays soft in front of a wall). No avatar ids: the cluster path's
//    avatars count like the legacy ones, and an avatar a wall hides entirely is simply cut. The
//    alpha pipelines composite A "over"; the supersampling (clean_snap_accum.comp) averages RGBA
//    over the N jittered recordings -- antialiased coverage; and mTonemapAlphaPipe unpremultiplies
//    in linear HDR and writes straight colour + A, read back as RGBA (renderOffscreen).
//  - GLOW, BLOOM, LENS EFFECTS, GOD RAYS and the precipitation in front are not in the layer:
//    nothing carries coverage for them (additive draws leave A alone, so their light shows only
//    over the avatar's own pixels). The fog and haze ON an avatar stay: their composites run over
//    the opaque scene before the holdout clears the background they also painted. Translucent
//    (alpha-blended) world surfaces -- glass, blended foliage -- do not cut the avatars: only
//    opaque and alpha-masked ones are in the occluder.
// Nothing here per object or per pixel on the CPU: a compute after the warm-up, a compute and a
// fullscreen depth draw per recording.

#include "linden_common.h"

#include "llcleanrenderervk.h"
#include "llsnapshotbufferlimits.h"
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include <cstring>

namespace
{
// Two depths are one surface within this much of the distance (0.5 %); the occluder is pushed
// back by it, for the jitter between a recording's samples and the warm-up's pixel centres where
// an avatar meets the world (feet on the floor, a hand on a table).
constexpr float kOccluderTol = 0.005f;

// clean_snap_holdout.comp Push; clean_snap_holdout_depth.frag takes `params` alone.
struct HoldPush
{
    uint32_t params[4];   // width, height, 0, tolerance (float bits)
    float    lin[4];      // inverse projection [10], [14], [11], [15]: depth -> view z
    float    proj[4];     // projection [10], [14], [11], [15]: view z -> depth
};
static_assert(sizeof(HoldPush) == 48, "HoldPush");

VkShaderModule snapModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    vkNoteShaderModule(m, code, bytes, name);
    return m;
}

void depthBarrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
    b.oldLayout = from; b.newLayout = to;
    b.image = img; b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

constexpr VkPipelineStageFlags2 kDepthTests =
    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
} // namespace

bool LLCleanRendererVK::initSnapHoldout()
{
    if (mSnapWarmPipe && mSnapHoldoutPipe && mSnapHoldoutDepthPipe && mTonemapAlphaPipe) return true;
    if (mSnapHoldInitFailed || !mCtx || !mTonemapLayout || !mDepthSampler) return false;
    mSnapHoldInitFailed = true;   // until everything below exists
    // The holdout writes the HDR scene as an rgba16f storage image: ensureHdr gives it the usage
    // on exactly this condition.
    if (!cameraMediumStorageSupported())
    {
        LL_WARNS("Snapshot") << "transparent avatar layer: the HDR scene format is not storage-capable; the layer is not available"
                             << LL_ENDL;
        return false;
    }
    VkDevice dev = mCtx->device();
    // b0 the recording's depth, b1 the warm-up's depth (buffer), b2 the recording's occluder,
    // b3 the HDR scene (storage). The depth draw reads only b2; the warm-up's copy b0 / b1.
    const VkDescriptorType types[4] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    VkDescriptorSetLayoutBinding b[4]{};
    for (uint32_t i = 0; i < 4; ++i)
    {
        b[i].binding = i; b[i].descriptorType = types[i]; b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | (i == 2 ? VK_SHADER_STAGE_FRAGMENT_BIT : 0u);
    }
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 4; dl.pBindings = b;
    if (vkdCreateSetLayout(dev, &dl, &mDslSnapHold, "mDslSnapHold") != VK_SUCCESS) mDslSnapHold = VK_NULL_HANDLE;
    if (mDslSnapHold && mSnapHoldPool.create(dev, "snapHoldout", {{mDslSnapHold, 1}}) == VK_SUCCESS
        && mSnapHoldPool.allocate(mDslSnapHold, &mDsetSnapHold) != VK_SUCCESS)
        mDsetSnapHold = VK_NULL_HANDLE;
    const VkPushConstantRange cpr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(HoldPush)};
    const VkPushConstantRange fpr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(HoldPush::params)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1; li.pSetLayouts = &mDslSnapHold;
    li.pushConstantRangeCount = 1;
    if (mDslSnapHold)
    {
        li.pPushConstantRanges = &cpr;
        if (vkdCreatePipelineLayout(dev, &li, &mSnapHoldLayout, "mSnapHoldLayout") != VK_SUCCESS) mSnapHoldLayout = VK_NULL_HANDLE;
        li.pPushConstantRanges = &fpr;
        if (vkdCreatePipelineLayout(dev, &li, &mSnapHoldDepthLayout, "mSnapHoldDepthLayout") != VK_SUCCESS)
            mSnapHoldDepthLayout = VK_NULL_HANDLE;
    }
    VkShaderModule warm = snapModule(dev, kWorldSnapWarmSpv, kWorldSnapWarmSpv_bytes, "SnapWarm");
    VkShaderModule hold = snapModule(dev, kWorldSnapHoldoutSpv, kWorldSnapHoldoutSpv_bytes, "SnapHoldout");
    VkShaderModule fvs = snapModule(dev, kWorldGlowCombineVertSpv, kWorldGlowCombineVertSpv_bytes, "GlowCombineVert");
    VkShaderModule dfs = snapModule(dev, kWorldSnapHoldoutDepthFragSpv, kWorldSnapHoldoutDepthFragSpv_bytes, "SnapHoldoutDepthFrag");
    VkShaderModule tfs = snapModule(dev, kWorldTonemapAlphaFragSpv, kWorldTonemapAlphaFragSpv_bytes, "TonemapAlphaFrag");
    if (mSnapHoldLayout && mSnapHoldDepthLayout && mDsetSnapHold && warm && hold && fvs && dfs && tfs)
    {
        mSnapWarmPipe = createComputePipeline(dev, warm, mSnapHoldLayout);
        mSnapHoldoutPipe = createComputePipeline(dev, hold, mSnapHoldLayout);
        // The occluder into the depth: fullscreen, depth only, nearer wins (reversed-Z flips the
        // conventional LESS to GREATER), written.
        GraphicsPipelineDesc dd;
        dd.vert = fvs; dd.frag = dfs; dd.layout = mSnapHoldDepthLayout;
        dd.noColor = true;
        dd.depthFormat = mDepthFormat; dd.depthTest = true; dd.depthWrite = true;
        dd.depthCompare = VK_COMPARE_OP_LESS; dd.reverseZ = true;
        dd.cullMode = VK_CULL_MODE_NONE;
        mSnapHoldoutDepthPipe = createGraphicsPipeline(dev, dd);
        // The straight-alpha tonemap: mTonemapPipe's state (one view: offscreen renders are mono).
        GraphicsPipelineDesc td;
        td.vert = fvs; td.frag = tfs; td.layout = mTonemapLayout;
        td.colorFormat = mColorFormat; td.depthFormat = VK_FORMAT_UNDEFINED;
        td.depthTest = false; td.depthWrite = false; td.blend = false;
        td.cullMode = VK_CULL_MODE_NONE;
        registerDisplayPipeline(td, &mTonemapAlphaPipe, "snapshot alpha tonemap",
            kWorldGlowCombineVertSpv, kWorldGlowCombineVertSpv_bytes,
            kWorldTonemapAlphaFragSpv, kWorldTonemapAlphaFragSpv_bytes,
            false, UINT32_MAX, true);
        mTonemapAlphaPipe = createGraphicsPipeline(dev, td);
        const VkBool32 enabled = VK_TRUE;
        const VkSpecializationMapEntry entry{0u, 0u, sizeof(enabled)};
        const VkSpecializationInfo spec{1u, &entry, sizeof(enabled), &enabled};
        td.fragSpec = &spec;
        // Register this recipe even if the first avatar snapshot is SDR. A later
        // HDR switch must prepare its matching-format companion pipeline too.
        registerDisplayPipeline(td, &mTonemapAlphaHdrPipe, "snapshot HDR alpha tonemap",
            kWorldGlowCombineVertSpv, kWorldGlowCombineVertSpv_bytes,
            kWorldTonemapAlphaFragSpv, kWorldTonemapAlphaFragSpv_bytes,
            true, 0u, true);
        if (mDisplayLinear)
        {
            mTonemapAlphaHdrPipe = createGraphicsPipeline(dev, td);
        }
    }
    for (VkShaderModule m : {warm, hold, fvs, dfs, tfs})
        if (m) vkDestroyShaderModule(dev, m, nullptr);
    mSnapHoldInitFailed = !mSnapWarmPipe || !mSnapHoldoutPipe || !mSnapHoldoutDepthPipe || !mTonemapAlphaPipe;
    if (mSnapHoldInitFailed)
        LL_WARNS("Snapshot") << "transparent avatar layer: its pipelines could not be created; the layer is not available"
                             << LL_ENDL;
    return !mSnapHoldInitFailed;
}

void LLCleanRendererVK::destroySnapHoldout()
{
    if (!mCtx) return;
    VkDevice dev = mCtx->device();
    freeSnapHoldBuffers();
    for (VkPipeline* p : {&mSnapWarmPipe, &mSnapHoldoutPipe, &mSnapHoldoutDepthPipe, &mTonemapAlphaPipe, &mTonemapAlphaHdrPipe})
        if (*p) { vkDestroyPipeline(dev, *p, nullptr); *p = VK_NULL_HANDLE; }
    for (VkPipelineLayout* l : {&mSnapHoldLayout, &mSnapHoldDepthLayout})
        if (*l) { vkDestroyPipelineLayout(dev, *l, nullptr); *l = VK_NULL_HANDLE; }
    mSnapHoldPool.destroy();
    mDsetSnapHold = VK_NULL_HANDLE;
    if (mDslSnapHold) { vkdDestroySetLayout(dev, mDslSnapHold); mDslSnapHold = VK_NULL_HANDLE; }
    mSnapHoldInitFailed = false;
}

void LLCleanRendererVK::freeSnapHoldBuffers()
{
    if (!mSnapWarmBuf.buffer && !mSnapOccBuf.buffer) return;
    vkDeviceWaitIdle(mCtx->device());
    if (mSnapWarmBuf.buffer) mCtx->destroyBuffer(mSnapWarmBuf);
    if (mSnapOccBuf.buffer) mCtx->destroyBuffer(mSnapOccBuf);
    mSnapWarmBuf = GpuBuffer{};
    mSnapOccBuf = GpuBuffer{};
    mSnapWarmValid = false;
}

bool LLCleanRendererVK::snapMatteInit()
{
    return initSnapHoldout();
}

bool LLCleanRendererVK::snapMatteBegin(uint32_t w, uint32_t h)
{
    mSnapMatte = false;
    mSnapMatteSeq = false;
    mSnapMatteFailed = false;
    mSnapHoldRan = false;
    mSnapMatteFrameNo = 0;
    freeSnapHoldBuffers();
    uint64_t bytes = 0;
    if (!mCtx || !LLSnapshotBufferLimits::imageBytes(w, h, sizeof(float), mCtx->limits().maxStorageBufferRange, bytes))
    {
        LL_WARNS("Snapshot") << "transparent avatar layer " << w << "x" << h
                             << ": world occluder exceeds the device storage-buffer range; the shot is not taken" << LL_ENDL;
        return false;
    }
    if (!initSnapHoldout()) return false;
    {
        // The warm-up's depth and the recordings' occluder, 4 B / px each (budgeted by
        // vkSnapshotFitAvatarLayer; allocated after the supersampling's accumulators).
        VkMemScope memcat(VKMEM_TARGET);   // #407: snapshot-sized intermediates
        VkDeviceContext::SoftAllocScope soft(mCtx);
        mSnapWarmBuf = mCtx->createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mSnapWarmBuf.buffer)
            mSnapOccBuf = mCtx->createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    if (!mSnapWarmBuf.buffer || !mSnapOccBuf.buffer)
    {
        freeSnapHoldBuffers();
        LL_WARNS("Snapshot") << "transparent avatar layer " << w << "x" << h << ": no video memory for the world occluder ("
                             << ((bytes * 2) >> 20) << " MB); the shot is not taken" << LL_ENDL;
        return false;
    }
    mSnapMatteW = w;
    mSnapMatteH = h;
    mSnapMatteSeq = true;
    return true;
}

void LLCleanRendererVK::snapMatteFrame(uint32_t frame)
{
    mSnapMatteFrameNo = frame;
    mSnapMatte = mSnapMatteSeq && frame > 0;   // recording 0: the normal view
    mSnapHoldRan = false;
}

void LLCleanRendererVK::snapMatteRecord(VkCommandBuffer cmd, uint32_t w, uint32_t h)
{
    if (!mSnapMatteSeq || mSnapMatteFrameNo != 0 || w != mSnapMatteW || h != mSnapMatteH) return;
    // The warm-up's depth, in the attachment layout recordWorld ends in.
    if (!mDepth.image || mDepthW != w || mDepthH != h)
    {
        LL_WARNS("Snapshot") << "transparent avatar layer: the warm-up's depth is not " << w << "x" << h
                             << "; the shot is not taken" << LL_ENDL;
        mSnapMatteFailed = true;
        return;
    }
    {
        const VkDescriptorImageInfo di{mDepthSampler, mDepth.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
        const VkDescriptorBufferInfo bi{mSnapWarmBuf.buffer, 0, mSnapWarmBuf.size};
        VkWriteDescriptorSet wr[2]{};
        for (auto& x : wr) { x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; x.dstSet = mDsetSnapHold; x.descriptorCount = 1; }
        wr[0].dstBinding = 0; wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[0].pImageInfo = &di;
        wr[1].dstBinding = 1; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[1].pBufferInfo = &bi;
        vkUpdateDescriptorSets(mCtx->device(), 2, wr, 0, nullptr);
    }
    depthBarrier(cmd, mDepth.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                 kDepthTests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    HoldPush pc{};
    pc.params[0] = w; pc.params[1] = h;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mSnapWarmPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mSnapHoldLayout, 0, 1, &mDsetSnapHold, 0, nullptr);
    VK_PUSH_BUDGET_CHECK(pc);
    vkCmdPushConstants(cmd, mSnapHoldLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
    depthBarrier(cmd, mDepth.image, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_NONE,
                 kDepthTests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    // The warm-up's depth -> every later recording's holdout (separate submissions, same queue).
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &d);
    mSnapWarmValid = true;
}

bool LLCleanRendererVK::snapMatteEnd()
{
    mSnapMatte = false;
    if (!mSnapMatteSeq) return true;
    mSnapMatteSeq = false;
    freeSnapHoldBuffers();
    return !mSnapMatteFailed;
}

void LLCleanRendererVK::recordSnapHoldout(VkCommandBuffer cmd, uint32_t rw, uint32_t rh)
{
    if (!mSnapMatte) return;
    // (offscreen renders are never scaled or stereo: the scene, its depth and the shot share a
    // size, and the warm-up kept its depth -- otherwise the recording keeps its opaque background,
    // tonemapped as usual, and the shot fails: snapMatteEnd)
    if (!mSnapHoldoutPipe || !mDsetSnapHold || !mSnapWarmValid || mScaled || rw != mSnapMatteW || rh != mSnapMatteH
        || !mHdr.view || mHdrW != rw || mHdrH != rh || !mDepth.view || mDepthW != rw || mDepthH != rh)
    {
        if (!mSnapMatteFailed)   // (once per shot)
            LL_WARNS("Snapshot") << "transparent avatar layer: no holdout at " << rw << "x" << rh
                                 << " (the scene targets or the warm-up's depth do not match the shot); the shot is not taken"
                                 << LL_ENDL;
        mSnapMatteFailed = true;
        return;
    }
    {
        const VkDescriptorImageInfo di{mDepthSampler, mDepth.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo hi{VK_NULL_HANDLE, mHdr.view, VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorBufferInfo bi[2] = {{mSnapWarmBuf.buffer, 0, mSnapWarmBuf.size}, {mSnapOccBuf.buffer, 0, mSnapOccBuf.size}};
        VkWriteDescriptorSet wr[4]{};
        for (uint32_t i = 0; i < 4; ++i)
        {
            wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[i].dstSet = mDsetSnapHold; wr[i].dstBinding = i;
            wr[i].descriptorCount = 1;
        }
        wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[0].pImageInfo = &di;
        wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[1].pBufferInfo = &bi[0];
        wr[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[2].pBufferInfo = &bi[1];
        wr[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wr[3].pImageInfo = &hi;
        vkUpdateDescriptorSets(mCtx->device(), 4, wr, 0, nullptr);
    }
    // In: the opaque scene and its composites (pass 1 / 2a draws, the deferred lighting and the
    // camera medium's compute writes) -> this compute's read-modify-write; the depth (already
    // DEPTH_READ_ONLY for pass 2) -> its sampled read.
    {
        VkImageMemoryBarrier2 hb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        hb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        hb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        hb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        hb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        hb.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; hb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        hb.image = mHdr.image; hb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkMemoryBarrier2 db{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        db.srcStageMask = kDepthTests; db.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        db.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; db.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &db;
        d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &hb;
        vkCmdPipelineBarrier2(cmd, &d);
    }
    // The projection's z terms (this recording's: the jitter is in x / y only, so the warm-up's
    // depth reads the same way).
    HoldPush pc{};
    pc.params[0] = rw; pc.params[1] = rh;
    std::memcpy(&pc.params[3], &kOccluderTol, sizeof(float));
    pc.lin[0] = mCurInvProj[10]; pc.lin[1] = mCurInvProj[14]; pc.lin[2] = mCurInvProj[11]; pc.lin[3] = mCurInvProj[15];
    pc.proj[0] = mCurProj[10]; pc.proj[1] = mCurProj[14]; pc.proj[2] = mCurProj[11]; pc.proj[3] = mCurProj[15];
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mSnapHoldoutPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mSnapHoldLayout, 0, 1, &mDsetSnapHold, 0, nullptr);
    VK_PUSH_BUDGET_CHECK(pc);
    vkCmdPushConstants(cmd, mSnapHoldLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (rw + 7) / 8, (rh + 7) / 8, 1);
    // Out: the scene back to its attachment layout for pass 2's transparent layers; the occluder
    // to the depth draw's FS.
    {
        VkImageMemoryBarrier2 hb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        hb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; hb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        hb.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        hb.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        hb.oldLayout = VK_IMAGE_LAYOUT_GENERAL; hb.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        hb.image = mHdr.image; hb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkMemoryBarrier2 ob{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        ob.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; ob.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        ob.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; ob.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &ob;
        d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &hb;
        vkCmdPipelineBarrier2(cmd, &d);
    }
    // The transparent layers' holdout: the occluder into the depth, where it is nearer. Depth
    // READ_ONLY -> ATTACHMENT (after this compute's and pass 2a's sampled reads), the depth-only
    // draw, then READ_ONLY again for pass 2.
    depthBarrier(cmd, mDepth.image, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | kDepthTests, VK_ACCESS_2_NONE,
                 kDepthTests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    da.imageView = mDepth.view; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {rw, rh}}; ri.layerCount = 1;
    ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp{0.f, 0.f, (float)rw, (float)rh, 0.f, 1.f};
    const VkRect2D sc{{0, 0}, {rw, rh}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    bindGraphicsPipeline(cmd, mSnapHoldoutDepthPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mSnapHoldDepthLayout, 0, 1, &mDsetSnapHold, 0, nullptr);
    vkCmdPushConstants(cmd, mSnapHoldDepthLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc.params), pc.params);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    depthBarrier(cmd, mDepth.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                 kDepthTests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | kDepthTests,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    mSnapHoldRan = true;   // (the straight-alpha tonemap reads coverage now)
}
