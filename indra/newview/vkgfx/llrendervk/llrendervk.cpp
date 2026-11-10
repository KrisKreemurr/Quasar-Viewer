#include "llrendervk.h"
#include "vk_pipeline_ui.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

LLRenderVK gVK;

using namespace llrvk;

namespace {

// gGL blend factors -> Vulkan. Same 1:1 mapping GL uses (see llrender.cpp
// sGLBlendFactor[]), just spelled in Vulkan tokens.
VkBlendFactor toVk(eBlendFactor f) {
    switch (f) {
        case BF_ONE:                    return VK_BLEND_FACTOR_ONE;
        case BF_ZERO:                   return VK_BLEND_FACTOR_ZERO;
        case BF_DEST_COLOR:             return VK_BLEND_FACTOR_DST_COLOR;
        case BF_SOURCE_COLOR:           return VK_BLEND_FACTOR_SRC_COLOR;
        case BF_ONE_MINUS_DEST_COLOR:   return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case BF_ONE_MINUS_SOURCE_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case BF_DEST_ALPHA:             return VK_BLEND_FACTOR_DST_ALPHA;
        case BF_SOURCE_ALPHA:           return VK_BLEND_FACTOR_SRC_ALPHA;
        case BF_ONE_MINUS_DEST_ALPHA:   return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case BF_ONE_MINUS_SOURCE_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}

VkPrimitiveTopology topoFor(eGeomMode m) {
    switch (m) {
        case TRIANGLES:      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case TRIANGLE_FAN:   return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        case LINES:          return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case LINE_STRIP:     return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case LINE_LOOP:      return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; // emulated: +closing vert
        case eGeomMode::POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;   // qualified: wingdi.h has a POINTS struct
        case QUADS:          return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; // emulated
        default:             return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

bool isLineTopo(VkPrimitiveTopology t) {
    return t == VK_PRIMITIVE_TOPOLOGY_LINE_LIST ||
           t == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
}

VkCompareOp toVkCompare(eCompareFunc f) {
    switch (f) {
        case CF_NEVER:         return VK_COMPARE_OP_NEVER;
        case CF_ALWAYS:        return VK_COMPARE_OP_ALWAYS;
        case CF_LESS:          return VK_COMPARE_OP_LESS;
        case CF_LESS_EQUAL:    return VK_COMPARE_OP_LESS_OR_EQUAL;
        case CF_EQUAL:         return VK_COMPARE_OP_EQUAL;
        case CF_NOT_EQUAL:     return VK_COMPARE_OP_NOT_EQUAL;
        case CF_GREATER_EQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case CF_GREATER:       return VK_COMPARE_OP_GREATER;
    }
    return VK_COMPARE_OP_LESS_OR_EQUAL;
}

} // namespace

// ---------------------------------------------------------------------------
// init / shutdown
// ---------------------------------------------------------------------------
bool LLRenderVK::init(VkDeviceContext& ctx, const std::string& shaderDir) {
    mCtx = &ctx;
    mVert = loadUiShaderModule(ctx.device(), shaderDir + "/ui.vert.spv");
    mFrag = loadUiShaderModule(ctx.device(), shaderDir + "/ui.frag.spv");
    return initCommon(ctx);
}

bool LLRenderVK::initFromSpv(VkDeviceContext& ctx,
                             const uint32_t* vertSpv, size_t vertBytes,
                             const uint32_t* fragSpv, size_t fragBytes) {
    mCtx = &ctx;
    auto mk = [&](const uint32_t* spv, size_t bytes) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = bytes;
        ci.pCode = spv;
        VkShaderModule mod;
        VK_CHECK(vkCreateShaderModule(ctx.device(), &ci, nullptr, &mod));
        return mod;
    };
    mVert = mk(vertSpv, vertBytes);
    mFrag = mk(fragSpv, fragBytes);
    return initCommon(ctx);
}

void LLRenderVK::initAlphaTestFragSpv(const uint32_t* fragSpv, size_t fragBytes) {
    if (!mCtx || mFragATest || !fragSpv || !fragBytes) return;
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = fragBytes;
    ci.pCode = fragSpv;
    VK_CHECK(vkCreateShaderModule(mCtx->device(), &ci, nullptr, &mFragATest));
}

void LLRenderVK::initOcclSpv(const uint32_t* vertSpv, size_t vertBytes,
                             const uint32_t* fragSpv, size_t fragBytes) {
    // Both or neither: the occlusion pipelines (mVertOccl set) pair the two.
    if (!mCtx || mVertOccl || !vertSpv || !vertBytes || !fragSpv || !fragBytes) return;
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = fragBytes;
    ci.pCode = fragSpv;
    VK_CHECK(vkCreateShaderModule(mCtx->device(), &ci, nullptr, &mFragOccl));
    ci.codeSize = vertBytes;
    ci.pCode = vertSpv;
    VK_CHECK(vkCreateShaderModule(mCtx->device(), &ci, nullptr, &mVertOccl));
}

// Shared init body (everything after the two shader modules are created).
bool LLRenderVK::initCommon(VkDeviceContext& ctx) {
    mDisplayUiFrameComplete = true;
    // One combined image sampler at binding 0. Set 0: the batch texture (fragment).
    // Set 1, same layout: the scene depth an in-world label's pixels test against
    // (#487, #1126, ui_occl.frag). Vertex stage visible too (layout shared with set 0).
    VkDescriptorSetLayoutBinding b0{};
    b0.binding = 0;
    b0.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b0.descriptorCount = 1;
    b0.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dslci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 1;
    dslci.pBindings = &b0;
    VK_CHECK(vkdCreateSetLayout(ctx.device(), &dslci, &mSetLayout, "ui.mSetLayout"));

    // Push constants: mat4 mvp (projection*modelview) + an in-world label's anchor
    // (vec4, #487) for the vertex shader, and the alpha-test threshold for the
    // alpha-tested fragment variant (offscreen).
    VkPushConstantRange pcr[2] = {
        {VK_SHADER_STAGE_VERTEX_BIT, 0, kPushAnchorOffset + 16},
        {VK_SHADER_STAGE_FRAGMENT_BIT, kPushMinAlphaOffset, 16}};
    const VkDescriptorSetLayout setLayouts[2] = {mSetLayout, mSetLayout};
    VkPipelineLayoutCreateInfo lci{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 2;
    lci.pSetLayouts = setLayouts;
    lci.pushConstantRangeCount = 2;
    lci.pPushConstantRanges = pcr;
    VK_CHECK(vkdCreatePipelineLayout(ctx.device(), &lci, &mLayout, "ui.mLayout"));   // push-limit checked (#238)

    // Descriptor pool: one set per (texture, sampler) combo actually used. mDescCache
    // never evicts, so this is a hard ceiling on unique UI texture/sampler combos over
    // the whole session -- 512 was exhausted mid-login (vkAllocateDescriptorSets ->
    // VK_ERROR_OUT_OF_POOL_MEMORY -> abort). Raise to 16384 (trivial on-device); a proper
    // LRU eviction of mDescCache is the real fix (follow-up).
    // Sets are freed individually when their texture is released (deferred), so the
    // pool is FREE_DESCRIPTOR_SET_BIT and the accounting credits freed sets back.
    VK_CHECK(mDescPool.create(ctx.device(), "ui", {{mSetLayout, 16384}},
                              VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT));

    // Samplers: filter(nearest/linear) x address(wrap/mirror/clamp) = 6.
    // Index layout matches samplerIndex(): filter*3 + address.
    auto mkSampler = [&](VkFilter filt, VkSamplerAddressMode addr) {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = filt;
        sci.minFilter = filt;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = addr;
        sci.addressModeV = addr;
        sci.addressModeW = addr;
        VkSampler s;
        VK_CHECK(vkCreateSampler(ctx.device(), &sci, nullptr, &s));
        return s;
    };
    const VkFilter filters[2] = {VK_FILTER_NEAREST, VK_FILTER_LINEAR};
    const VkSamplerAddressMode addrs[3] = {
        VK_SAMPLER_ADDRESS_MODE_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    for (int fi = 0; fi < 2; ++fi)
        for (int ai = 0; ai < 3; ++ai)
            mSamplers[fi * 3 + ai] = mkSampler(filters[fi], addrs[ai]);

    // Texture 0 = 1x1 white so untextured batches sample 1.0 (color-only).
    const uint8_t white[4] = {255, 255, 255, 255};
    uint32_t whiteId = createTexture(1, 1, white, /*nearest=*/true);
    (void)whiteId; // == 0

    // Reserve some CPU vertex storage.
    mVerts.reserve(1 << 16);
    return true;
}

void LLRenderVK::shutdown() {
    if (!mCtx) return;
    VkDevice dev = mCtx->device();
    for (auto& kv : mPipelines) vkDestroyPipeline(dev, kv.second, nullptr);
    mPipelines.clear();
    vkDeviceWaitIdle(dev);
    wtRelease(/*all=*/true);   // #1133 VR world text (nothing exists unless VR ran)
    for (PendingUpload& u : mPendingUploads) if (u.owned.buffer) mCtx->destroyBuffer(u.owned);
    mPendingUploads.clear();
    mPreparedQueuedCount = 0;
    mPreparedRecordedCount = 0;
    mUploadOpen = false;
    processDeferred(/*all=*/true);
    for (GpuBuffer& b : mStageRetired) if (b.buffer) mCtx->destroyBuffer(b);
    mStageRetired.clear();
    if (mStageRing.buf.buffer) mCtx->destroyBuffer(mStageRing.buf);
    mStageRing = VkStageRing{};
    mStageMarks.clear();
    mFreeTexIds.clear();
    for (auto& kv : mRenderTargets) if (kv.second.depth.image) mCtx->destroyImage(kv.second.depth);
    mRenderTargets.clear();
    for (auto& t : mTextures) if (t.image.image) mCtx->destroyImage(t.image);
    mTextures.clear();
    mDescCache.clear();
    for (GpuBuffer& b : mVBO) if (b.buffer) mCtx->destroyBuffer(b);
    mVBO.clear();
    mVBOCapacityBytes.clear();
    for (VkSampler s : mSamplers)
        if (s) vkDestroySampler(dev, s, nullptr);
    mDescPool.destroy();
    if (mLayout) vkDestroyPipelineLayout(dev, mLayout, nullptr);
    if (mSetLayout) vkdDestroySetLayout(dev, mSetLayout);
    if (mVert) vkDestroyShaderModule(dev, mVert, nullptr);
    if (mFrag) vkDestroyShaderModule(dev, mFrag, nullptr);
    if (mFragATest) vkDestroyShaderModule(dev, mFragATest, nullptr);
    mFragATest = VK_NULL_HANDLE;
    if (mVertOccl) vkDestroyShaderModule(dev, mVertOccl, nullptr);
    mVertOccl = VK_NULL_HANDLE;
    if (mFragOccl) vkDestroyShaderModule(dev, mFragOccl, nullptr);
    mFragOccl = VK_NULL_HANDLE;
    mLabelDepthSets.clear();   // freed with the pool
    mCtx = nullptr;
}

// ---------------------------------------------------------------------------
// textures
// ---------------------------------------------------------------------------
void LLRenderVK::recordUploadCmds(VkCommandBuffer cmd, const PendingUpload& u)
{
    // Earlier sampling of this image (previous frames on the queue, or earlier in this
    // command buffer) -> copy, then copy -> fragment sampling. A full overwrite discards
    // the old contents (UNDEFINED); a sub-rectangle keeps them (the image is in
    // SHADER_READ_ONLY_OPTIMAL after its first upload, #458).
    if (u.prepared.valid())
    {
        VkBufferMemoryBarrier2 hostWrite{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        hostWrite.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        hostWrite.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
        hostWrite.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        hostWrite.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        hostWrite.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostWrite.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostWrite.buffer = u.buf;
        hostWrite.offset = u.off;
        hostWrite.size = u.preparedBytes;
        VkDependencyInfo hostDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        hostDependency.bufferMemoryBarrierCount = 1;
        hostDependency.pBufferMemoryBarriers = &hostWrite;
        vkCmdPipelineBarrier2(cmd, &hostDependency);
    }

    VkImageMemoryBarrier2 toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toDst.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    toDst.srcAccessMask = 0;
    toDst.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toDst.oldLayout = u.preserve ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.image = u.image;
    toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo d1{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d1.imageMemoryBarrierCount = 1;
    d1.pImageMemoryBarriers = &toDst;
    vkCmdPipelineBarrier2(cmd, &d1);

    VkBufferImageCopy region{};
    region.bufferOffset = u.off;   // rows tightly packed (bufferRowLength 0 = imageExtent.width)
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {(int32_t)u.x, (int32_t)u.y, 0};
    region.imageExtent = {u.w, u.h, 1};
    vkCmdCopyBufferToImage(cmd, u.buf, u.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier2 toRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toRead.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toRead.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    toRead.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.image = u.image;
    toRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo d2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d2.imageMemoryBarrierCount = 1;
    d2.pImageMemoryBarriers = &toRead;
    vkCmdPipelineBarrier2(cmd, &d2);
}

void LLRenderVK::recordUploads(VkCommandBuffer cmd)
{
    mRecordingFrame = true;   // until recordFrame(): later uploads go immediate
    // The context drains the exact recorded-ticket list immediately after this method.
    // If that caller contract was missed, don't record another service-buffer read with
    // an ambiguous fence association.
    if (mPreparedRecordedCount) return;
    // Ring bytes staged since the last recorded frame belong to this one: mark the head
    // (freed kDeferFrames later, processDeferred). Replaced ring buffers still hold the
    // uploads recorded below: retire them with this frame.
    if (!mStageMarks.empty() ? mStageMarks.back().head != mStageRing.head_v
                             : mStageRing.head_v != mStageRing.tail_v)
        mStageMarks.push_back({mFrameCounter, mStageRing.head_v});
    mStageFrameBytes = mStageFrameAccum;
    mStageFrameAccum = 0;
    for (GpuBuffer& b : mStageRetired)
    {
        DeferredFree d;
        d.frame = mFrameCounter;
        d.staging = b;
        mDeferred.push_back(std::move(d));
    }
    mStageRetired.clear();
    // Idle release: nothing in flight and no ring use for a while.
    if (mStageRing.buf.buffer && mStageMarks.empty() && mStageRing.used() == 0
        && mFrameCounter > mStageLastUse + kStageRingIdleFrames)
    {
        DeferredFree d;
        d.frame = mFrameCounter;
        d.staging = mStageRing.buf;
        mDeferred.push_back(std::move(d));
        mStageRing = VkStageRing{};
    }
    if (mPendingUploads.empty()) return;
    for (PendingUpload& u : mPendingUploads)
    {
        if (u.prepared.valid())
        {
            if (mPreparedRecordedCount >= kMaxPreparedUploads) continue;
            mPreparedRecorded[mPreparedRecordedCount++] = u.prepared;
            if (mPreparedQueuedCount) --mPreparedQueuedCount;
        }
        if (u.image && u.buf) recordUploadCmds(cmd, u);
        if (u.prepared.valid()) u.prepared = PreparedUploadToken{};
        // A one-off staging buffer lives until this frame (and any still in flight) has
        // executed (ring ranges: the mark above).
        if (!u.owned.buffer) continue;
        DeferredFree d;
        d.frame = mFrameCounter;
        d.staging = u.owned;
        mDeferred.push_back(std::move(d));
    }
    // A malformed/empty entry is discarded; valid prepared records were copied above.
    mPendingUploads.erase(std::remove_if(mPendingUploads.begin(), mPendingUploads.end(),
        [](const PendingUpload& u) { return !u.prepared.valid() || u.image == VK_NULL_HANDLE || u.buf == VK_NULL_HANDLE; }),
        mPendingUploads.end());
}

void LLRenderVK::setStageRingMax(uint64_t bytes)
{
    mStageRingMax = std::min(std::max<uint64_t>(bytes, 32ull << 20), kStageRingMax);
}

bool LLRenderVK::growStageRing(uint64_t need)
{
    // need: the request; the ring must hold 4 of them (VkStageRing::alloc's cap / 4).
    uint64_t cap = std::max<uint64_t>(mStageRing.cap * 2, kStageRingMin);
    while (cap < need * 4 && cap < mStageRingMax) cap *= 2;
    cap = std::min(cap, mStageRingMax);
    if (cap <= mStageRing.cap || need > cap / 4) return false;
    GpuBuffer nb;
    {
        VkMemScope memcat(VKMEM_STAGING);   // #407 (UI upload staging ring)
        nb = mCtx->createBuffer((VkDeviceSize)cap, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    if (!nb.buffer || !nb.mapped)
    {
        if (nb.buffer) mCtx->destroyBuffer(nb);
        return false;
    }
    // Queued uploads still reference the old buffer: it is retired by the next
    // recordUploads (which records them). Its marks go with it.
    if (mStageRing.buf.buffer) mStageRetired.push_back(mStageRing.buf);
    mStageRing = VkStageRing{};
    mStageRing.buf = nb;
    mStageRing.cap = cap;
    mStageMarks.clear();
    return true;
}

uint8_t* LLRenderVK::allocStaging(VkDeviceSize bytes, PendingUpload& u, bool mayDefer)
{
    u.owned = GpuBuffer{};
    u.buf = VK_NULL_HANDLE;
    u.off = 0;
    if (!bytes) return nullptr;
    ++mStageUploads;
    // Ring: live frames only (submitImmediate uploads complete out of the ring's order).
    if (mFrameToSwapchain && !mRecordingFrame && bytes <= mStageRingMax / 4)
    {
        uint64_t off = 0;
        bool ok = mStageRing.alloc(bytes, off);
        if (!ok && growStageRing(bytes)) ok = mStageRing.alloc(bytes, off);
        if (ok)
        {
            u.buf = mStageRing.buf.buffer;
            u.off = off;
            mStageLastUse = mFrameCounter;
            mStageFrameAccum += bytes;
            mStagePeak = std::max(mStagePeak, mStageRing.used());
            return (uint8_t*)mStageRing.buf.mapped + off;
        }
        // The ring is at its cap and full: frames are not being recorded (the UI keeps
        // resolving, nothing retires), or the traffic exceeds the ring. A one-off buffer
        // per upload would grow without bound -- the caller retries later instead.
        if (mayDefer && mStageRing.cap >= mStageRingMax)
        {
            --mStageUploads;
            ++mStageDeferred;
            return nullptr;
        }
    }
    // One-off (too big for the ring, ring full at its cap, or an immediate upload).
    {
        VkMemScope memcat(VKMEM_STAGING);   // #407
        u.owned = mCtx->createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    if (!u.owned.buffer || !u.owned.mapped)
    {
        if (u.owned.buffer) mCtx->destroyBuffer(u.owned);
        return nullptr;
    }
    if (mFrameToSwapchain && !mRecordingFrame) ++mStageFallbacks;   // a live upload the ring missed
    u.buf = u.owned.buffer;
    return (uint8_t*)u.owned.mapped;
}

void LLRenderVK::submitUpload(PendingUpload& u)
{
    if (mFrameToSwapchain && !mRecordingFrame)
    {
        mPendingUploads.push_back(u);   // recorded next frame
        return;
    }
    mCtx->submitImmediate([&](VkCommandBuffer cmd) { recordUploadCmds(cmd, u); });
    if (u.owned.buffer) mCtx->destroyBuffer(u.owned);
}

GpuImage LLRenderVK::createSampledImage(uint32_t w, uint32_t h, eTexelFormat fmt)
{
    const VkFormat vkfmt = fmt == TXF_RGBA8 ? VK_FORMAT_R8G8B8A8_UNORM
                         : fmt == TXF_LA8   ? VK_FORMAT_R8G8_UNORM
                                            : VK_FORMAT_R8_UNORM;
    GpuImage img = mCtx->createImage2D(
        w, h, vkfmt,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    if (fmt == TXF_RGBA8 || !img.image) return img;
    // Swizzled view: the shaders sample RGBA exactly as the RGBA8 expansion gave it.
    VkComponentMapping cm{};
    switch (fmt)
    {
        case TXF_LA8:
            cm = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G};
            break;
        case TXF_L8:
            cm = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE};
            break;
        case TXF_A8:
            cm = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R};
            break;
        default:   // TXF_COVERAGE8
            cm = {VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R};
            break;
    }
    mCtx->destroyView(img.view);
    img.view = VK_NULL_HANDLE;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = vkfmt;
    vi.components = cm;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkResult result = vkCreateImageView(mCtx->device(), &vi, nullptr, &img.view);
    if (result != VK_SUCCESS)
    {
        if (result == VK_ERROR_DEVICE_LOST || vkDeviceIsLost())
            vkDeviceLostRaise("vkCreateImageView(UI texture)", result, nullptr, __FILE__, __LINE__);
        mCtx->destroyImage(img);
        return {};
    }
    return img;
}

uint32_t LLRenderVK::createTexture(uint32_t w, uint32_t h, const uint8_t* pixels,
                                   bool nearest, eTexelFormat fmt) {
    VkMemScope memcat(VKMEM_UI);   // #407: UI textures + their staging
    VkTexture t;
    t.image = createSampledImage(w, h, fmt);
    if (!t.image.image)
    {
        if (t.image.view) mCtx->destroyImage(t.image);
        return 0;
    }
    t.view = t.image.view;
    t.texFmt = fmt;

    const VkDeviceSize bytes = (VkDeviceSize)w * h * texelBytes(fmt);
    PendingUpload u;
    u.image = t.image.image;
    u.w = w; u.h = h;
    // pixels == nullptr: no upload (the caller follows with a full beginTextureUpdate).
    if (uint8_t* dst = (t.image.image && pixels) ? allocStaging(bytes, u) : nullptr)
    {
        std::memcpy(dst, pixels, bytes);
        submitUpload(u);
    }

    // Descriptor sets are allocated lazily per (texture, sampler) in
    // descriptorFor(). `nearest` records the DEFAULT sampler preference for this
    // texture (mirrors the filter LLTextureData was uploaded with); the texture unit's
    // filter state can still override it at bind time.
    t.dset = VK_NULL_HANDLE;
    t.defaultFilter = nearest ? TF_POINT : TF_BILINEAR;
    if (!mFreeTexIds.empty())
    {
        t.id = mFreeTexIds.back();
        mFreeTexIds.pop_back();
        mTextures[t.id] = t;
    }
    else
    {
        t.id = (uint32_t)mTextures.size();
        try { mTextures.push_back(t); }
        catch (...)
        {
            mCtx->destroyImage(t.image);
            return 0;
        }
    }
    return t.id;
}

uint32_t LLRenderVK::createPreparedTexture(uint32_t w, uint32_t h, llrvk::eTexelFormat fmt,
                                           VkBuffer source,
                                           VkDeviceSize sourceOffset, VkDeviceSize sourceBytes,
                                           PreparedUploadToken token)
{
    if (!mCtx || !w || !h) return 0;
    VkDeviceContext::SoftAllocScope soft(mCtx);
    VkDeviceContext::NoReliefScope noRelief;
    uint32_t id = createTexture(w, h, nullptr, true, fmt);
    if (!id || id >= mTextures.size() || !mTextures[id].image.image)
    {
        if (id) destroyTexture(id);
        return 0;
    }
    if (!queuePreparedTextureUpdate(id, w, h, fmt, source, sourceOffset, sourceBytes, token))
    {
        // No descriptor or command ever referenced this new candidate, so free it
        // immediately without growing the deferred-free vector on an OOM path.
        mCtx->destroyImage(mTextures[id].image);
        // createTexture either appended the last slot or popped this exact ID
        // from the free list. No callback/texture mutation occurs between its
        // return and this queue attempt. Both rollbacks require no allocation.
        if (id + 1u == mTextures.size())
            mTextures.pop_back();
        else
        {
            mTextures[id] = VkTexture{};
            mFreeTexIds.push_back(id); // capacity retained by createTexture's pop
        }
        return 0;
    }
    return id;
}

bool LLRenderVK::queuePreparedTextureUpdate(uint32_t texId, uint32_t w, uint32_t h,
                                            llrvk::eTexelFormat fmt,
                                            VkBuffer source, VkDeviceSize sourceOffset,
                                            VkDeviceSize sourceBytes, PreparedUploadToken token)
{
    const uint32_t bpp = llrvk::texelBytes(fmt);
    if (!mCtx || !w || !h || !source || !token.valid() || !texId || texId >= mTextures.size()
        || !mTextures[texId].image.image
        || fmt > llrvk::TXF_COVERAGE8 || mTextures[texId].texFmt != fmt
        || mTextures[texId].image.format != (fmt == llrvk::TXF_RGBA8 ? VK_FORMAT_R8G8B8A8_UNORM
            : fmt == llrvk::TXF_LA8 ? VK_FORMAT_R8G8_UNORM : VK_FORMAT_R8_UNORM)
        || mTextures[texId].image.extent.width != w || mTextures[texId].image.extent.height != h
        || (uint64_t)w > (std::numeric_limits<uint64_t>::max)() / h / bpp
        || sourceBytes != (VkDeviceSize)w * h * bpp
        || mPreparedQueuedCount >= kMaxPreparedUploads)
        return false;
    PendingUpload upload{};
    upload.image = mTextures[texId].image.image;
    upload.buf = source;
    upload.off = sourceOffset;
    upload.w = w; upload.h = h;
    upload.preparedBytes = sourceBytes;
    upload.prepared = token;
    try
    {
        mPendingUploads.push_back(upload);
    }
    catch (...)
    {
        return false;
    }
    ++mPreparedQueuedCount;
    return true;
}

uint32_t LLRenderVK::takePreparedRecordedUploads(PreparedUploadToken* out, uint32_t capacity) noexcept
{
    if (!out || capacity < mPreparedRecordedCount) return 0;
    const uint32_t count = mPreparedRecordedCount;
    for (uint32_t i = 0; i < count; ++i) out[i] = mPreparedRecorded[i];
    mPreparedRecordedCount = 0;
    return count;
}

bool LLRenderVK::hasPreparedUpload(PreparedUploadToken token) const noexcept
{
    if (!token.valid()) return false;
    for (const PendingUpload& upload : mPendingUploads)
        if (upload.prepared.slot == token.slot && upload.prepared.generation == token.generation)
            return true;
    return false;
}

bool LLRenderVK::cancelPreparedUpload(PreparedUploadToken token) noexcept
{
    if (!token.valid()) return false;
    for (auto it = mPendingUploads.begin(); it != mPendingUploads.end(); ++it)
    {
        if (it->prepared.generation != token.generation || it->prepared.slot != token.slot) continue;
        mPendingUploads.erase(it);
        if (mPreparedQueuedCount) --mPreparedQueuedCount;
        return true;
    }
    return false;
}

void LLRenderVK::takeDescSets(uint32_t texId, std::vector<VkDescriptorSet>& out)
{
    for (int s = 0; s < kNumSamplers; ++s)
    {
        auto it = mDescCache.find(texId * kNumSamplers + (uint32_t)s);
        if (it == mDescCache.end()) continue;
        out.push_back(it->second);
        mDescCache.erase(it);
    }
}

void LLRenderVK::destroyTexture(uint32_t texId)
{
    if (!mCtx || texId == 0 || texId >= mTextures.size() || !mTextures[texId].image.image) return;
    if (mTextures[texId].pendingFree) return;   // already queued
    mTextures[texId].pendingFree = true;
    DeferredFree d;
    d.frame = mFrameCounter;
    d.image = mTextures[texId].image;
    takeDescSets(texId, d.sets);
    d.recycleId = texId;
    // The slot keeps its (valid) image until the deferred point: batches queued this
    // frame may still bind the id. The backend no longer hands the id out.
    mDeferred.push_back(std::move(d));
    auto rt = mRenderTargets.find(texId);
    if (rt != mRenderTargets.end())
    {
        if (rt->second.depth.image)
        {
            DeferredFree dd;
            dd.frame = mFrameCounter;
            dd.image = rt->second.depth;
            mDeferred.push_back(std::move(dd));
        }
        mRenderTargets.erase(rt);
    }
}

void LLRenderVK::processDeferred(bool all)
{
    // Ring ranges of frames that have completed (#458): advance the tail (marks are in
    // frame order).
    size_t done = 0;
    while (done < mStageMarks.size() &&
           (all || (mFrameCounter >= mStageMarks[done].frame + kDeferFrames &&
                    (!mExternalRetirement || mStageMarks[done].frame < mExternalRetiredFrame))))
        mStageRing.tail_v = std::max(mStageRing.tail_v, std::min(mStageMarks[done++].head, mStageRing.head_v));
    if (done) mStageMarks.erase(mStageMarks.begin(), mStageMarks.begin() + (std::ptrdiff_t)done);
    if (mDeferred.empty()) return;
    size_t keep = 0;
    for (size_t i = 0; i < mDeferred.size(); ++i)
    {
        DeferredFree& d = mDeferred[i];
        if (!all && (mFrameCounter < d.frame + kDeferFrames ||
                     (mExternalRetirement && d.frame >= mExternalRetiredFrame)))
        {
            if (keep != i) mDeferred[keep] = std::move(d);
            ++keep;
            continue;
        }
        if (d.staging.buffer) mCtx->destroyBuffer(d.staging);
        if (!d.sets.empty())
            mDescPool.free((uint32_t)d.sets.size(), d.sets.data());
        if (d.image.image) mCtx->destroyImage(d.image);
        if (d.recycleId && d.recycleId < mTextures.size())
        {
            // Only clear the slot if it still holds the image we queued (a resize
            // in updateTexture defers the OLD image while the slot keeps the new one).
            mTextures[d.recycleId].image = GpuImage{};
            mTextures[d.recycleId].view = VK_NULL_HANDLE;
            mTextures[d.recycleId].pendingFree = false;
            mFreeTexIds.push_back(d.recycleId);
        }
    }
    mDeferred.resize(keep);
}

// Lazily allocate + cache a combined-image-sampler descriptor for the given
// texture sampled through the given sampler (filter x address). This is what lets
// one uploaded image be sampled point/linear and clamp/wrap at different draws.
VkDescriptorSet LLRenderVK::descriptorFor(uint32_t texId, int samplerIndex) {
    if (texId >= mTextures.size() || !mTextures[texId].image.image) texId = 0;
    uint32_t key = texId * kNumSamplers + (uint32_t)samplerIndex;
    auto it = mDescCache.find(key);
    if (it != mDescCache.end()) return it->second;

    VkDescriptorSet dset;
    VK_CHECK(mDescPool.allocate(mSetLayout, &dset));

    VkDescriptorImageInfo dii{};
    dii.sampler = mSamplers[samplerIndex];
    dii.imageView = mTextures[texId].view;
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = dset;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &dii;
    vkUpdateDescriptorSets(mCtx->device(), 1, &wr, 0, nullptr);

    mDescCache[key] = dset;
    return dset;
}

// ---------------------------------------------------------------------------
// pipeline cache
// ---------------------------------------------------------------------------
VkPipeline LLRenderVK::pipelineFor(const BatchKey& key) {
    auto it = mPipelines.find(key);
    if (it != mPipelines.end()) return it->second;

    UiPipelineDesc d;
    d.vert = (key.labelOccl && mVertOccl) ? mVertOccl : mVert;
    d.frag = (key.labelOccl && mFragOccl) ? mFragOccl
           : (key.alphaTest && mFragATest) ? mFragATest : mFrag;
    d.layout = mLayout;
    d.colorFormat = key.colorFmt;
    d.glAlphaBlend = key.glAlpha;
    // RTT batches explicitly select RGBA8 and glAlpha. Only the optional live
    // float display is absolute-linear-nits, including its depth-tested overlays.
    d.displayLinearNits = key.colorFmt==VK_FORMAT_R16G16B16A16_SFLOAT && !key.glAlpha;

    d.topology = key.topo;
    d.blendEnable = key.blend;
    d.srcFactor = toVk(key.sf);
    d.dstFactor = toVk(key.df);
    d.colorWriteMask = key.colorMask;
    d.depthTest = key.depthTest;
    d.depthWrite = key.depthWrite;
    d.depthCompareOp = toVkCompare(key.depthFunc);
    d.depthBias = key.depthBias;
    // When the render pass has a depth attachment, EVERY pipeline used within it
    // must declare the matching depthAttachmentFormat (dynamic-rendering rule),
    // regardless of whether it enables the depth test. Carried in the key so the
    // world-overlay segment (world depth attached) and the 2D UI (no depth) of the
    // same swapchain frame get distinct pipelines.
    if (key.depthFmt != VK_FORMAT_UNDEFINED)
        d.depthFormat = key.depthFmt;
    // Line-family pipelines get dynamic lineWidth (real setLineWidth support).
    d.dynamicLineWidth = isLineTopo(key.topo);
    // Vertex layout: pos(vec3) + color(vec4) + uv(vec2), interleaved.
    d.stride = sizeof(Vertex);
    d.posOffset = offsetof(Vertex, pos);
    d.colOffset = offsetof(Vertex, col);
    d.uvOffset = offsetof(Vertex, uv);
    VkPipeline p = createUiPipeline(mCtx->device(), d);
    mPipelines[key] = p;
    return p;
}

// ---------------------------------------------------------------------------
// frame lifecycle. beginFrame opens ONE command buffer + begins dynamic
// rendering with a clear; every batch records into it; endFrame flushes the
// last batch, ends rendering, submits and waits.
// ---------------------------------------------------------------------------
// Reset all deferred immediate-mode + matrix state to the frame defaults. Shared
// by the offscreen (beginFrame) and swapchain (beginFrameToView) entry points.
void LLRenderVK::resetFrameState(uint32_t fbHeight) {
    mFbHeight = fbHeight;
    mVerts.clear();
    // Drop any batches left over from a frame that was buffered but never recorded
    // (e.g. swapchain acquire failed and the frame was skipped).
    mPending.clear();
    mHaveOpen = false;
    mInFrame = true;
    mInOverlay = false;
    mOverlayBegin = mOverlayEnd = 0;
    mOverlayRecorded = false;
    mOverlayStereo = false;   // #1141: on only when the context says so, per frame
    mOpenWorldCam = false;
    mInPreHud = false;
    mPreHudBegin = mPreHudEnd = 0;
    mPreHudRecorded = false;
    mPreHudOccl = false;
    mAnchorOn = false;
    if (mWorldTextViews || !mWtBatches.empty())
    {   // #1133 VR (the desktop never has either)
        mWtBatches.clear();
        mWtBbAnchorOn = false;
        mOpenWtOccl = false;
        mWtHeadValid = false;
        mWtDropped = 0;
    }
    mFrameVbo = VK_NULL_HANDLE;
    // A frame dropped before recording loses its offscreen segments too (the
    // targets keep their previous contents).
    mOffStack.clear();
    mOffRuns.clear();
    mMinAlpha = 0.0f;
    for (int m = 0; m < NUM_MM; ++m) {
        mMatIdx[m] = 0;
        mMat[m][0] = glm::mat4(1.0f);
    }
    mMatrixMode = MM_MODELVIEW;
    mActiveUnit = 0;
    for (auto& u : mTexUnits) u = TexUnit{};
    mBlendEnabled = true;
    mSF = BF_SOURCE_ALPHA;
    mDF = BF_ONE_MINUS_SOURCE_ALPHA;
    mColorMask = 0xF;
    mDepthTest = false;
    mDepthWrite = false;
    mDepthFunc = CF_LESS_EQUAL;
    mScissorEnable = false;
    mLineWidth = 1.0f;
    mFrameVertBase = 0;
    mBeginVertStart = 0;
}

void LLRenderVK::beginFrame(OffscreenTarget& target) {
    mTarget = &target;
    mFrameToSwapchain = false;
    mFrameColorFormat = target.colorFormat();
    mFrameHasDepth = target.hasDepth();
    mFrameDepthFormat = target.hasDepth() ? target.depthFormat()
                                          : VK_FORMAT_UNDEFINED;
    mFbHeight = target.height();
    mVerts.clear();
    mHaveOpen = false;
    mInFrame = true;
    // reset matrix stacks (modelview, projection, texture0..3)
    for (int m = 0; m < NUM_MM; ++m) {
        mMatIdx[m] = 0;
        mMat[m][0] = glm::mat4(1.0f);
    }
    mMatrixMode = MM_MODELVIEW;
    mActiveUnit = 0;
    for (auto& u : mTexUnits) u = TexUnit{};
    mBlendEnabled = true;
    mSF = BF_SOURCE_ALPHA;
    mDF = BF_ONE_MINUS_SOURCE_ALPHA;
    mColorMask = 0xF;
    mDepthTest = false;
    mDepthWrite = false;
    mDepthFunc = CF_LESS_EQUAL;
    mScissorEnable = false;
    mLineWidth = 1.0f;
}

// Build a BatchKey from the current live render state (everything that a distinct
// pipeline depends on).
LLRenderVK::BatchKey LLRenderVK::liveKey(VkPrimitiveTopology topo) const {
    BatchKey k{};
    k.topo = topo;
    k.blend = mBlendEnabled;
    k.sf = mSF;
    k.df = mDF;
    k.colorMask = mColorMask;
    k.depthTest = mDepthTest;
    k.depthWrite = mDepthWrite;
    k.depthFunc = mDepthFunc;
    k.colorFmt = mFrameColorFormat;
    k.alphaTest = false;
    k.glAlpha = false;
    k.labelOccl = false;
    if (!mOffStack.empty())
    {
        // Offscreen render target: R8G8B8A8_UNORM, its own CONVENTIONAL depth (if
        // any) honoured as LL's GL code asks, GL blend semantics, alpha test.
        k.colorFmt = VK_FORMAT_R8G8B8A8_UNORM;
        auto rt = mRenderTargets.find(mOffStack.back().tex);
        const bool hasDepth = rt != mRenderTargets.end() && rt->second.depth.image;
        k.depthFmt = hasDepth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED;
        if (!hasDepth) { k.depthTest = false; k.depthWrite = false; }
        k.depthBias = 0;
        k.alphaTest = mMinAlpha > 0.0f && mFragATest != VK_NULL_HANDLE;
        k.glAlpha = true;
        return k;
    }
    if (mFrameToSwapchain)
    {
        // Swapchain frames: only the world-overlay segment has a depth attachment
        // (the world's), and only it honours the depth state. The 2D UI layer,
        // HUD overlays and name tags never depth-test (painter's order).
        if (mInOverlay)
        {
            // Test against the world depth, never write it (the overlays that
            // matter -- property/ban lines, highlights, beacons -- don't write in
            // LL either; keeps the segment a pure depth READ).
            k.depthFmt = mOverlayDepthFormat;
            k.depthWrite = false;
            // Only for LESS / LEQUAL (the "visible" tests). The GREATER "hidden
            // behind geometry" pass (build mode) must not be biased the same way.
            const bool visible_test = k.depthTest &&
                (k.depthFunc == llrvk::CF_LESS || k.depthFunc == llrvk::CF_LESS_EQUAL);
            k.depthBias = visible_test ? (mOverlayReverseZ ? 2 : 1) : 0;
            if (mOverlayReverseZ && k.depthTest)
            {
                // Reversed-Z world depth: nearer = larger. Mirror LL's ordering tests.
                switch (k.depthFunc)
                {
                case llrvk::CF_LESS:          k.depthFunc = llrvk::CF_GREATER; break;
                case llrvk::CF_LESS_EQUAL:    k.depthFunc = llrvk::CF_GREATER_EQUAL; break;
                case llrvk::CF_GREATER:       k.depthFunc = llrvk::CF_LESS; break;
                case llrvk::CF_GREATER_EQUAL: k.depthFunc = llrvk::CF_LESS_EQUAL; break;
                default: break;
                }
            }
        }
        else
        {
            // In-world labels (#487, #1126): an anchored batch issued with LL's depth
            // test on tests each pixel, at the anchor's depth, against the scene depth
            // in the fragment shader; still no depth attachment (the layer is colour-only).
            k.labelOccl = mInPreHud && mPreHudOccl && mAnchorOn && k.depthTest &&
                          mVertOccl != VK_NULL_HANDLE;
            k.depthFmt = VK_FORMAT_UNDEFINED;
            k.depthTest = false;
            k.depthWrite = false;
        }
    }
    else
        k.depthFmt = mFrameHasDepth ? mFrameDepthFormat : VK_FORMAT_UNDEFINED;
    return k;
}

void LLRenderVK::flush() {
    // Close the currently open batch (if any) into a PendingDraw, replayed as
    // one vkCmdDraw at endFrame. This keeps all recording inside a single
    // begin/endRendering with the VBO uploaded once.
    if (!mHaveOpen) return;
    uint32_t count = (uint32_t)mVerts.size() - mFrameVertBase;
    if (count > 0) {
        PendingDraw pd;
        pd.pipe = pipelineFor(mOpenKey);
        pd.dset = descriptorFor(mOpenTex, mOpenSamplerIndex);
        pd.mvp = mOpenMVP;
        pd.first = mFrameVertBase;
        pd.count = count;
        pd.lineWidth = mOpenLineWidth;
        pd.scissorEnable = mOpenScissorEnable;
        pd.scissor = mOpenScissor;
        pd.isLine = mOpenIsLine;
        pd.offscreen = !mOffStack.empty();
        pd.alphaTest = mOpenKey.alphaTest;
        pd.minAlpha = mMinAlpha;
        pd.labelOccl = mOpenKey.labelOccl;
        pd.pipePlain = pd.pipe;
        pd.worldCam = mOpenWorldCam;
        std::memcpy(pd.anchor, mOpenAnchor, sizeof(pd.anchor));
        if (pd.labelOccl)
        {
            BatchKey plain = mOpenKey;
            plain.labelOccl = false;
            pd.pipePlain = pipelineFor(plain);
        }
        // A transient keyed-retry refusal drops only this HDR frame. It must
        // never permanently block the next allowed retry or clear its budget.
        if (mOpenKey.colorFmt==VK_FORMAT_R16G16B16A16_SFLOAT && !mOpenKey.glAlpha &&
            (!pd.pipe || !pd.pipePlain || !pd.dset)) mDisplayUiFrameComplete=false;
        if (pd.offscreen)
        {
            // GL's bottom row -> image row 0 (the facade's matrices put GL's top at
            // clip y = -1 for the swapchain; undo that flip for render targets).
            glm::mat4 flipY(1.0f);
            flipY[1][1] = -1.0f;
            pd.mvp = flipY * pd.mvp;
        }
        mPending.push_back(pd);
        // #1133 VR: the pre-HUD layer's batches are drawn in the world (recordWorldTextViews).
        if (mWorldTextViews && mInPreHud)
        {
            if (mWtBatches.size() < kWtBatchCap)
            {
                WtBatch b;
                b.pending = (uint32_t)(mPending.size() - 1);
                b.tex = mOpenTex;
                b.samplerIndex = mOpenSamplerIndex;
                b.topo = mOpenKey.topo;
                b.additive = mOpenKey.blend && mOpenKey.df == BF_ONE;
                b.occl = mOpenWtOccl;
                std::memcpy(b.anchor, mOpenWtAnchor, sizeof(b.anchor));
                mWtBatches.push_back(b);
            }
            else
                ++mWtDropped;
        }
    }
    mFrameVertBase = (uint32_t)mVerts.size();
    mHaveOpen = false;
}

void LLRenderVK::endFrame() {
    flush();

    // Offscreen path is single-threaded and submit-immediate (waits), so ring
    // slot 0 is always free; upload this frame's vertices into it.
    VkBuffer vbo = uploadFrameVerts(0);

    OffscreenTarget& t = *mTarget;
    uint32_t w = t.width(), h = t.height();


    const bool haveDepth = t.hasDepth();

    mCtx->submitImmediate([&](VkCommandBuffer cmd) {
        // color UNDEFINED -> COLOR_ATTACHMENT, clear.
        VkImageMemoryBarrier2 barriers[2];
        VkImageMemoryBarrier2& colBar = barriers[0];
        colBar = VkImageMemoryBarrier2{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        colBar.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        colBar.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        colBar.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        colBar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colBar.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colBar.image = t.color().image;
        colBar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        uint32_t barCount = 1;
        if (haveDepth) {
            VkImageMemoryBarrier2& dBar = barriers[1];
            dBar = VkImageMemoryBarrier2{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            dBar.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            dBar.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            dBar.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
            dBar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            dBar.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            dBar.image = t.depth().image;
            dBar.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            barCount = 2;
        }
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = barCount;
        dep.pImageMemoryBarriers = barriers;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkRenderingAttachmentInfo color{
            VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = t.color().view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0.10f, 0.10f, 0.12f, 1.0f}};

        VkRenderingAttachmentInfo depthAtt{
            VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (haveDepth) {
            depthAtt.imageView = t.depth().view;
            depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depthAtt.clearValue.depthStencil = {1.0f, 0};
        }

        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {w, h}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        if (haveDepth) ri.pDepthAttachment = &depthAtt;
        vkCmdBeginRendering(cmd, &ri);

        recordDraws(cmd, w, h, vbo);

        vkCmdEndRendering(cmd);
    });

    mPending.clear();
    mInFrame = false;
    ++mFrameCounter;               // offscreen frames complete inside submitImmediate
    processDeferred(/*all=*/false);
}

// Grow (if needed) and upload the frame's accumulated vertices into VBO ring slot
// `slot`, returning the buffer to bind. One buffer per frame-in-flight slot means
// frame N+1's upload never stomps memory frame N's in-flight draw is still reading.
VkBuffer LLRenderVK::uploadFrameVerts(uint32_t slot) {
    if (slot >= mVBO.size()) {
        mVBO.resize(slot + 1);
        mVBOCapacityBytes.resize(slot + 1, 0);
    }
    GpuBuffer& vbo = mVBO[slot];
    VkDeviceSize& cap = mVBOCapacityBytes[slot];

    VkDeviceSize needed = mVerts.size() * sizeof(Vertex);
    if (needed == 0) needed = sizeof(Vertex);
    if (needed > cap) {
        // Growth is rare (settles after the first frames). Wait device-idle before
        // freeing so no in-flight frame still references the old allocation.
        if (vbo.buffer) {
            vkDeviceWaitIdle(mCtx->device());
            mCtx->destroyBuffer(vbo);
        }
        VkDeviceSize alloc = needed + needed / 2;  // headroom
        VkMemScope memcat(VKMEM_UI);   // #407
        vbo = mCtx->createBuffer(
            alloc, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        cap = alloc;
    }
    if (!vbo.buffer || !vbo.mapped)
    {
        cap = 0; // retry at the next recorded frame; draw a clear-only UI meanwhile
        return VK_NULL_HANDLE;
    }
    if (!mVerts.empty())
        std::memcpy(vbo.mapped, mVerts.data(), mVerts.size() * sizeof(Vertex));
    return vbo.buffer;
}

// Bind the frame VBO and replay every PendingDraw as one vkCmdDraw, tracking
// pipeline/scissor/lineWidth to avoid redundant state sets. Assumes a dynamic-
// rendering scope is already open on `cmd`.
void LLRenderVK::recordDraws(VkCommandBuffer cmd, uint32_t w, uint32_t h,
                            VkBuffer vbo, size_t begin, size_t end,
                            VkDescriptorSet labelDepth, const glm::mat4* eyeFix) {
    if (!vbo) return;
    VkViewport vp{0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
    VkRect2D fullSc{{0, 0}, {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);

    end = std::min(end, mPending.size());
    if (begin >= end) return;

    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vbo, &off);
    // Scene depth for anchored labels (#487): set 1 of the shared layout, bound once
    // (the batch texture rebinds set 0 per draw; set 1 stays).
    if (labelDepth)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                mLayout, 1, 1, &labelDepth, 0, nullptr);

    VkPipeline lastPipe = VK_NULL_HANDLE;
    VkRect2D lastSc{{0xFFFFFFF, 0}, {0, 0}};
    float lastLW = -1.0f;
    for (size_t i = begin; i < end; ++i) {
        const PendingDraw& pd = mPending[i];
        if (pd.offscreen != mRecordingOffscreen) continue;   // other target's batch
        if (eyeFix && !pd.worldCam) continue;   // #1141: screen-space batch: not in an eye
        // An anchored label is depth-tested only where the scene depth is bound
        // (the world-recorded pre-HUD layer); elsewhere it draws depth-free.
        const bool occl = pd.labelOccl && labelDepth != VK_NULL_HANDLE;
        const VkPipeline pipe = (pd.labelOccl && !occl) ? pd.pipePlain : pd.pipe;
        // Creation can fail or be deferred. Never replay through a previous
        // pipeline (or bind a null one) when this batch has no usable state.
        if (!pipe || !pd.dset) continue;
        if (pipe != lastPipe) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            lastPipe = pipe;
            // Binding a pipeline without dynamic line width invalidates the
            // dynamic value (VUID-vkCmdDraw-None-08617): re-set after any bind.
            lastLW = -1.0f;
        }
        VkRect2D sc = (pd.scissorEnable && !eyeFix) ? pd.scissor : fullSc;
        if (sc.offset.x != lastSc.offset.x || sc.offset.y != lastSc.offset.y ||
            sc.extent.width != lastSc.extent.width ||
            sc.extent.height != lastSc.extent.height) {
            vkCmdSetScissor(cmd, 0, 1, &sc);
            lastSc = sc;
        }
        if (pd.isLine && pd.lineWidth != lastLW) {
            // Without wideLines the width must be exactly 1.0 (validation / RADV
            // are strict; NVIDIA ignores it); otherwise clamp to the device range.
            const float lw = std::clamp(pd.lineWidth, 1.0f, mCtx->lineWidthMax());
            vkCmdSetLineWidth(cmd, lw);
            lastLW = pd.lineWidth;
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                mLayout, 0, 1, &pd.dset, 0, nullptr);
        VK_PUSH_BUDGET_CHECK(glm::mat4);
        if (eyeFix)
        {
            const glm::mat4 eyeMvp = *eyeFix * pd.mvp;   // #1141: this eye's projection
            vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(glm::mat4), &eyeMvp);
        }
        else
            vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(glm::mat4), &pd.mvp);
        if (occl)
            vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_VERTEX_BIT, kPushAnchorOffset,
                               sizeof(pd.anchor), pd.anchor);
        if (pd.alphaTest)
            vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_FRAGMENT_BIT, kPushMinAlphaOffset,
                               sizeof(float), &pd.minAlpha);
        if (!pd.offscreen && mFrameColorFormat==VK_FORMAT_R16G16B16A16_SFLOAT)
            vkCmdPushConstants(cmd,mLayout,VK_SHADER_STAGE_FRAGMENT_BIT,kPushMinAlphaOffset+4,
                               sizeof(float),&mDisplayPaperWhiteNits);
        vkCmdDraw(cmd, pd.count, 1, pd.first, 0);
    }
}

// ---------------------------------------------------------------------------
// SWAPCHAIN frame lifecycle (live viewer). The caller owns the command buffer
// and image-layout transitions + submit/present.
// ---------------------------------------------------------------------------
void LLRenderVK::beginFrameToView(VkImageView colorView, VkFormat colorFormat,
                                  uint32_t w, uint32_t h) {
    mTarget = nullptr;
    mFrameToSwapchain = true;
    mFrameColorView = colorView;
    mFrameColorFormat = colorFormat;
    if (colorFormat==VK_FORMAT_R16G16B16A16_SFLOAT) mDisplayUiFrameComplete=true;
    // UI pass has no depth attachment (2D layer); depth-test batches degrade to
    // always-pass, which is correct for the UI ordering (painter's algorithm).
    mFrameHasDepth = false;
    mFrameDepthFormat = VK_FORMAT_UNDEFINED;
    mSwapW = w;
    mSwapH = h;
    resetFrameState(h);
}

bool LLRenderVK::beginWorldOverlay() {
    // One overlay segment per frame, swapchain frames with a world pass only.
    if (!mFrameToSwapchain || mOverlayDepthFormat == VK_FORMAT_UNDEFINED ||
        mInOverlay || mInPreHud || mOverlayEnd > mOverlayBegin ||
        mPreHudEnd > mPreHudBegin)   // the pre-HUD layer follows the overlay segment
        return false;
    flush();
    mInOverlay = true;
    mOverlayBegin = mOverlayEnd = mPending.size();
    return true;
}

void LLRenderVK::endWorldOverlay() {
    if (!mInOverlay) return;
    flush();
    mInOverlay = false;
    mOverlayEnd = mPending.size();
}

void LLRenderVK::setWorldOverlayStereo(bool on, const float* proj) {
    // Before the segment opens (batches already open keep their flag).
    mOverlayStereo = on && proj && mFrameToSwapchain && !mInOverlay;
    if (mOverlayStereo)
        std::memcpy(&mOverlayProj[0][0], proj, sizeof(float) * 16);
}

void LLRenderVK::recordWorldOverlayViews(VkCommandBuffer cmd, uint32_t frameSlot,
                                         const VkImageView* color, const VkImageView* depth,
                                         uint32_t n, uint32_t w, uint32_t h,
                                         const glm::mat4* eyeFix) {
    if (mInOverlay) endWorldOverlay();
    if (mInPreHud) endPreHudLayer();
    if (!mFrameToSwapchain || !mOverlayStereo || mOverlayRecorded || mOverlayEnd <= mOverlayBegin ||
        !color || !depth || !eyeFix || n == 0)
        return;
    flush();
    if (!mFrameVbo) mFrameVbo = uploadFrameVerts(frameSlot);
    if (!mFrameVbo) return;   // (no VBO this frame: the segment is dropped, as recordFrame would)

    // Order after the world's last colour / depth writes (as recordWorldOverlay).
    {
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                          VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                           VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        mb.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &dep);
    }
    for (uint32_t v = 0; v < n; ++v)
    {
        if (color[v] == VK_NULL_HANDLE || depth[v] == VK_NULL_HANDLE) continue;
        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca.imageView = color[v];
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        da.imageView = depth[v];
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_NONE;   // read only, as the desktop segment
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {w, h}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        ri.pDepthAttachment = &da;
        vkCmdBeginRendering(cmd, &ri);
        recordDraws(cmd, w, h, mFrameVbo, mOverlayBegin, mOverlayEnd, VK_NULL_HANDLE, &eyeFix[v]);
        vkCmdEndRendering(cmd);
    }
    mOverlayRecorded = true;
}

bool LLRenderVK::beginPreHudLayer(const float* proj, const float* view) {
    // One layer per frame, after the overlay segment (ranges stay ordered), swapchain
    // frames with a world pass only (the world renderer records it).
    if (!mFrameToSwapchain || mOverlayDepthFormat == VK_FORMAT_UNDEFINED ||
        mInOverlay || mInPreHud || mPreHudEnd > mPreHudBegin || !mOffStack.empty())
        return false;
    flush();
    mInPreHud = true;
    mPreHudBegin = mPreHudEnd = mPending.size();
    // Label occlusion (#487): the world camera the scene depth was drawn with.
    mPreHudOccl = proj && view && mVertOccl != VK_NULL_HANDLE;
    mAnchorOn = false;
    if (mWorldTextViews)
    {
        // #1133 VR: no 2D label occlusion (the layer is drawn per eye in the world); the head
        // camera LL lays the labels out under maps each batch back to world space.
        mPreHudOccl = false;
        mWtBbAnchorOn = false;
        mWtHeadValid = proj && view;
        if (mWtHeadValid)
        {
            glm::mat4 P, V;
            std::memcpy(&P[0][0], proj, sizeof(float) * 16);
            std::memcpy(&V[0][0], view, sizeof(float) * 16);
            mWtHeadVP = P * V;
        }
        return true;
    }
    if (mPreHudOccl)
    {
        glm::mat4 P, V;
        std::memcpy(&P[0][0], proj, sizeof(float) * 16);
        std::memcpy(&V[0][0], view, sizeof(float) * 16);
        mPreHudViewProj = P * V;
        mPreHudCamPos = glm::vec3(glm::inverse(V)[3]);
    }
    return true;
}

void LLRenderVK::endPreHudLayer() {
    if (!mInPreHud) return;
    flush();
    mInPreHud = false;
    mPreHudEnd = mPending.size();
    mAnchorOn = false;
}

void LLRenderVK::setLabelAnchor(bool on, float x, float y, float z, float bias) {
    // Bias / fade (metres along the view ray, toward the camera): geometry within
    // kBias of the anchor never hides the label (the anchor sits on or just above the
    // object it labels); geometry more than kBias + kFade in front of it hides the
    // pixels of the label it covers; in between they fade (ui_occl.frag, per pixel).
    if (mWorldTextViews)
    {
        // #1133 VR: the anchor only travels with the batch (recordWorldTextViews projects it
        // per eye); a change closes the open batch, as below.
        const bool vr_on = on && mInPreHud;
        const float b[4] = {x, y, z, bias};
        if (vr_on == mWtBbAnchorOn && (!vr_on || std::memcmp(b, mWtBbAnchor, sizeof(b)) == 0))
            return;
        flush();
        mWtBbAnchorOn = vr_on;
        std::memcpy(mWtBbAnchor, b, sizeof(b));
        return;
    }
    const float kBias = bias;
    constexpr float kFade = 0.35f;
    constexpr float kMinDist = 0.05f;
    bool valid = false;
    float a[4] = {0, 0, 0, 0};
    if (on && mInPreHud && mPreHudOccl)
    {
        const glm::vec3 p(x, y, z);
        const glm::vec4 c = mPreHudViewProj * glm::vec4(p, 1.0f);
        const glm::vec3 toCam = mPreHudCamPos - p;
        const float dist = glm::length(toCam);
        if (c.w > 0.0f && dist - kBias > kMinDist)
        {
            const float u = (c.x / c.w) * 0.5f + 0.5f;
            const float v = (c.y / c.w) * 0.5f + 0.5f;
            const glm::vec3 dir = toCam / dist;
            const float showDist = kBias;
            const float hideDist = std::min(kBias + kFade, dist - kMinDist);
            const glm::vec4 cs = mPreHudViewProj * glm::vec4(p + dir * showDist, 1.0f);
            const glm::vec4 ch = mPreHudViewProj * glm::vec4(p + dir * hideDist, 1.0f);
            // The anchor itself may be off screen (#1126): each pixel samples its own
            // depth, only the anchor's depth is needed.
            if (cs.w > 0.0f && ch.w > 0.0f)
            {
                // Depths in the world's own convention (reversed-Z or not): the
                // shader's clamp((hide - d) / (hide - show)) needs no sign flag.
                const float show = cs.z / cs.w;
                const float hide = ch.z / ch.w;
                if (std::fabs(hide - show) > 1e-9f)
                {
                    a[0] = u; a[1] = v; a[2] = hide; a[3] = 1.0f / (hide - show);
                    valid = true;
                }
            }
        }
    }
    // Behind the camera / no world camera: never occluded.
    if (valid == mAnchorOn && (!valid || std::memcmp(a, mAnchor, sizeof(a)) == 0))
        return;
    flush();   // the open batch keeps the anchor it was opened with
    mAnchorOn = valid;
    std::memcpy(mAnchor, a, sizeof(a));
}

VkDescriptorSet LLRenderVK::labelDepthSet(uint32_t frameSlot, VkImageView depthView) {
    if (depthView == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    if (frameSlot >= mLabelDepthSets.size()) mLabelDepthSets.resize(frameSlot + 1, VK_NULL_HANDLE);
    VkDescriptorSet& dset = mLabelDepthSets[frameSlot];
    if (!dset && mDescPool.allocate(mSetLayout, &dset) != VK_SUCCESS)
    {
        dset = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    // Rewritten every use (one write per frame with labels): this slot's last frame
    // has completed, and a recreated depth view can reuse the old handle value.
    VkDescriptorImageInfo dii{};
    dii.sampler = mSamplers[samplerIndex(TF_POINT, TAM_CLAMP)];   // texelFetch: unfiltered
    dii.imageView = depthView;
    dii.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = dset;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &dii;
    vkUpdateDescriptorSets(mCtx->device(), 1, &wr, 0, nullptr);
    return dset;
}

void LLRenderVK::recordWorldOverlay(VkCommandBuffer cmd, uint32_t frameSlot,
                                    VkImageView colorView, VkImageView depthView,
                                    VkImage depthImage, uint32_t w, uint32_t h,
                                    bool preHud) {
    if (mInOverlay) endWorldOverlay();
    if (mInPreHud) endPreHudLayer();
    if (!mFrameToSwapchain || colorView == VK_NULL_HANDLE)
        return;
    const bool doOverlay = !mOverlayRecorded && mOverlayEnd > mOverlayBegin &&
                           depthView != VK_NULL_HANDLE;
    const bool doPreHud = preHud && !mPreHudRecorded && mPreHudEnd > mPreHudBegin;
    if (!doOverlay && !doPreHud)
        return;
    // Close any batch still open (the UI buffering finished before recording) and
    // upload the whole frame's vertices once; recordFrame() reuses the buffer.
    flush();
    if (!mFrameVbo) mFrameVbo = uploadFrameVerts(frameSlot);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = colorView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    // The world's depth, as the pipeline left it (DEPTH_ATTACHMENT_OPTIMAL), read
    // only: overlay pipelines never write depth, and STORE_OP_NONE (VK 1.3) keeps
    // the attachment free of store writes.
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = depthView;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_NONE;

    // Order after the world's last color (post / AA / selection) and depth writes.
    {
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                          VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                           VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        mb.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w, h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    if (doOverlay)
    {
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &ri);
        recordDraws(cmd, w, h, mFrameVbo, mOverlayBegin, mOverlayEnd);
        vkCmdEndRendering(cmd);
        mOverlayRecorded = true;
    }
    // PRE-HUD TEXT LAYER (#468): in-world name tags / hover text / bubbles over the
    // world + its overlays, before the caller's HUD pass draws the HUD attachments
    // over them. Color only: these are the UI pass's own (depth-less) pipelines.
    // Anchored labels (#487, #1126) sample the world depth in their fragment shader:
    // it is read-only (DEPTH_READ_ONLY_OPTIMAL) for this scope, then back to the
    // attachment layout the hook was handed.
    if (doPreHud)
    {
        bool anyLabel = false;
        for (size_t i = mPreHudBegin; i < mPreHudEnd && i < mPending.size(); ++i)
            if (mPending[i].labelOccl) { anyLabel = true; break; }
        const VkDescriptorSet labelDepth =
            (anyLabel && depthImage != VK_NULL_HANDLE) ? labelDepthSet(frameSlot, depthView) : VK_NULL_HANDLE;
        VkImageMemoryBarrier2 toRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toRead.srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        toRead.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        toRead.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        toRead.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        toRead.image = depthImage;
        toRead.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (doOverlay || labelDepth)
        {
            // Blends over the overlay segment's color writes (another rendering scope).
            VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            mb.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            mb.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.memoryBarrierCount = doOverlay ? 1 : 0;
            dep.pMemoryBarriers = doOverlay ? &mb : nullptr;
            dep.imageMemoryBarrierCount = labelDepth ? 1 : 0;
            dep.pImageMemoryBarriers = labelDepth ? &toRead : nullptr;
            vkCmdPipelineBarrier2(cmd, &dep);
        }
        ri.pDepthAttachment = nullptr;
        vkCmdBeginRendering(cmd, &ri);
        recordDraws(cmd, w, h, mFrameVbo, mPreHudBegin, mPreHudEnd, labelDepth);
        vkCmdEndRendering(cmd);
        mPreHudRecorded = true;
        if (labelDepth)
        {
            VkImageMemoryBarrier2 back = toRead;
            back.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            back.srcAccessMask = 0;   // reads: execution dependency only
            back.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            back.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            back.oldLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
            back.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &back;
            vkCmdPipelineBarrier2(cmd, &dep);
        }
    }
}

// #1076 VR: the same 2D UI draws recordFrame() lays down, into another colour view of the
// frame's size and format (the UI wall's own image), before recordFrame(): no state is
// consumed, the batches stay for recordFrame. VR only (never called on the desktop).
void LLRenderVK::replayTo(VkCommandBuffer cmd, uint32_t frameSlot, VkImageView colorView) {
    if (colorView == VK_NULL_HANDLE) return;
    if (mInOverlay) endWorldOverlay();
    if (mInPreHud) endPreHudLayer();
    flush();
    if (!mFrameVbo) mFrameVbo = uploadFrameVerts(frameSlot);   // recordFrame() reuses it

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = colorView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {mSwapW, mSwapH}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);
    // The ranges recordFrame() draws (the world overlay segment and a world-recorded pre-HUD
    // layer are in-world, not UI).
    recordDraws(cmd, mSwapW, mSwapH, mFrameVbo, 0, mOverlayBegin);
    if (mPreHudRecorded || mWorldTextViews)   // (#1133 VR: the layer is in the world, never the 2D UI)
    {
        recordDraws(cmd, mSwapW, mSwapH, mFrameVbo, mOverlayEnd, mPreHudBegin);
        recordDraws(cmd, mSwapW, mSwapH, mFrameVbo, mPreHudEnd, (size_t)-1);
    }
    else
        recordDraws(cmd, mSwapW, mSwapH, mFrameVbo, mOverlayEnd, (size_t)-1);
    vkCmdEndRendering(cmd);
}

// ---------------------------------------------------------------------------
// #1133 VR IN-WORLD TEXT. The pre-HUD layer of a VR stereo frame holds world geometry: LL's
// name tags / bubbles / hover text / beacon labels laid out on head-facing billboards
// (LLHUDObject::sWorldBillboards) under the head camera beginPreHudLayer got. Each batch's
// world positions come back through that camera (inverse(headVP) * mvp), and the whole layer
// becomes ONE vertex stream: drawn once, multiview, into both eyes' layers; the textures it
// samples (glyph atlas pages, bubble images) share one sampler array per draw. Fixed cost:
// kWtVertCap vertices, kWtSetsPerSlot draws, nothing grows. VR only: the desktop never sets
// mWorldTextViews, so none of this runs and none of it is allocated.
// ---------------------------------------------------------------------------
void LLRenderVK::initWorldTextSpv(const uint32_t* vertSpv, size_t vertBytes, const uint32_t* fragSpv, size_t fragBytes) {
    // Kept for the first VR stereo frame: the modules are made there (wtEnsure), never on the desktop.
    mWtVertSpv = vertSpv; mWtVertBytes = vertBytes;
    mWtFragSpv = fragSpv; mWtFragBytes = fragBytes;
}

void LLRenderVK::setWorldTextViews(bool on) {
    mWorldTextViews = on && mWtVertSpv && mWtFragSpv && mWtVertBytes && mWtFragBytes;
    if (mWorldTextViews)
    {
        mWtLastUse = mFrameCounter;
        if (mWtBatches.capacity() < kWtBatchCap) mWtBatches.reserve(kWtBatchCap);   // once: fixed
    }
    else if (mWtHasBuffers && mFrameCounter > mWtLastUse + kWtIdleFrames)
        wtRelease(false);   // out of VR a while: give the buffers back (free on liveness)
}

bool LLRenderVK::wtEnsure(uint32_t frameSlot, VkFormat colorFmt) {
    if (!mCtx || frameSlot >= kWtSlots || mFrameCounter < mWtRetryAt) return false;
    const VkDevice dev = mCtx->device();
    bool ok = true;
    auto mk = [&](const uint32_t* spv, size_t bytes, VkShaderModule& out) {
        if (out) return;
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = bytes;
        ci.pCode = spv;
        if (vkCreateShaderModule(dev, &ci, nullptr, &out) != VK_SUCCESS) { out = VK_NULL_HANDLE; ok = false; }
    };
    mk(mWtVertSpv, mWtVertBytes, mWtVert);
    mk(mWtFragSpv, mWtFragBytes, mWtFrag);
    if (ok && !mWtSetLayout)
    {
        VkDescriptorSetLayoutBinding b[2]{};
        b[0].binding = 0;
        b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[0].descriptorCount = kWtTexSlots;
        b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[1].binding = 1;
        b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[1].descriptorCount = 1;
        b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 2;
        ci.pBindings = b;
        if (vkdCreateSetLayout(dev, &ci, &mWtSetLayout, "ui.worldTextSet") != VK_SUCCESS)
        { mWtSetLayout = VK_NULL_HANDLE; ok = false; }
    }
    if (ok && !mWtLayout)
    {
        const VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, 2 * 16 * sizeof(float)};   // mat4 viewProj[2]
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 1;
        ci.pSetLayouts = &mWtSetLayout;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &pcr;
        if (vkdCreatePipelineLayout(dev, &ci, &mWtLayout, "ui.worldTextLayout") != VK_SUCCESS)
        { mWtLayout = VK_NULL_HANDLE; ok = false; }
    }
    if (ok && !mWtPool.handle())
    {
        if (mWtPool.create(dev, "ui.worldtext", {{mWtSetLayout, kWtSlots * kWtSetsPerSlot}}) != VK_SUCCESS)
            ok = false;
        else
        {
            for (uint32_t s = 0; s < kWtSlots && ok; ++s)
                for (uint32_t d = 0; d < kWtSetsPerSlot && ok; ++d)
                    if (mWtPool.allocate(mWtSetLayout, &mWtSets[s][d]) != VK_SUCCESS) ok = false;
            if (!ok) { mWtPool.destroy(); std::memset(mWtSets, 0, sizeof(mWtSets)); }
        }
    }
    if (ok && (!mWtPipe || mWtPipeFmt != colorFmt))
    {
        if (mWtPipe)
        {   // another eye format (a new VR target): rare, the frames in flight may still use it
            vkDeviceWaitIdle(dev);
            vkDestroyPipeline(dev, mWtPipe, nullptr);
            mWtPipe = VK_NULL_HANDLE;
        }
        mWtPipe = createUiWorldTextPipeline(dev, mWtVert, mWtFrag, mWtLayout, colorFmt, 0x3u, sizeof(WtVert));
        mWtPipeFmt = mWtPipe ? colorFmt : VK_FORMAT_UNDEFINED;
        if (!mWtPipe) ok = false;
    }
    if (ok && !mWtVbo[frameSlot].buffer)
    {
        VkMemScope memcat(VKMEM_UI);
        VkDeviceContext::SoftAllocScope soft(mCtx);   // a refusal drops the labels, never the frame
        mWtVbo[frameSlot] = mCtx->createBuffer((VkDeviceSize)kWtVertCap * sizeof(WtVert), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!mWtVbo[frameSlot].buffer || !mWtVbo[frameSlot].mapped)
        {
            if (mWtVbo[frameSlot].buffer) mCtx->destroyBuffer(mWtVbo[frameSlot]);
            mWtVbo[frameSlot] = {};
            ok = false;
        }
        else
            mWtHasBuffers = true;
    }
    if (!ok) mWtRetryAt = mFrameCounter + 300;   // allocation policy: retry later, log (the context), never crash
    return ok;
}

void LLRenderVK::wtRelease(bool all) {
    if (!mCtx) return;
    for (GpuBuffer& b : mWtVbo)
    {
        if (!b.buffer) continue;
        if (all) mCtx->destroyBuffer(b);
        else
        {   // frames in flight may still read it: the deferred-release clock frees it
            DeferredFree d;
            d.frame = mFrameCounter;
            d.staging = b;
            mDeferred.push_back(std::move(d));
        }
        b = {};
    }
    mWtHasBuffers = false;
    std::vector<WtBatch>().swap(mWtBatches);
    if (!all) return;
    const VkDevice dev = mCtx->device();
    if (mWtPipe) vkDestroyPipeline(dev, mWtPipe, nullptr);
    mWtPipe = VK_NULL_HANDLE;
    mWtPipeFmt = VK_FORMAT_UNDEFINED;
    mWtPool.destroy();
    std::memset(mWtSets, 0, sizeof(mWtSets));
    if (mWtLayout) vkDestroyPipelineLayout(dev, mWtLayout, nullptr);
    mWtLayout = VK_NULL_HANDLE;
    if (mWtSetLayout) vkdDestroySetLayout(dev, mWtSetLayout);
    mWtSetLayout = VK_NULL_HANDLE;
    if (mWtVert) vkDestroyShaderModule(dev, mWtVert, nullptr);
    if (mWtFrag) vkDestroyShaderModule(dev, mWtFrag, nullptr);
    mWtVert = mWtFrag = VK_NULL_HANDLE;
}

void LLRenderVK::recordWorldTextViews(VkCommandBuffer cmd, uint32_t frameSlot, VkImageView colorArray, VkFormat colorFmt,
                                      VkImage depthImage, VkImageView depthArray, VkImageLayout depthLayout,
                                      uint32_t w, uint32_t h, const float viewProj[2][16], const float eye[2][3]) {
    if (mInPreHud) endPreHudLayer();
    mWtStats = WorldTextStats{};
    if (!mWorldTextViews || !mFrameToSwapchain) return;
    flush();
    mPreHudRecorded = true;   // (recordFrame / replayTo skip the layer in VR anyway)
    mWtStats.droppedCap = mWtDropped;
    if (mWtBatches.empty() || !mWtHeadValid || !colorArray || !depthImage || !depthArray || !w || !h) return;
    if (!wtEnsure(frameSlot, colorFmt)) { mWtStats.failed = true; return; }

    const glm::mat4 invHead = glm::inverse(mWtHeadVP);
    glm::mat4 VP[2];
    glm::vec3 E[2];
    for (int v = 0; v < 2; ++v)
    {
        std::memcpy(&VP[v][0][0], viewProj[v], sizeof(float) * 16);
        E[v] = glm::vec3(eye[v][0], eye[v][1], eye[v][2]);
    }
    WtVert* const out = static_cast<WtVert*>(mWtVbo[frameSlot].mapped);
    uint32_t n = 0;
    // Draws: one per descriptor set (kWtTexSlots distinct images each), in buffer order -- the
    // painter's order LL issued (bubble, then its lines; far tags first) survives.
    struct Draw { uint32_t first = 0, count = 0, nTex = 0; uint32_t keys[kWtTexSlots] = {}; };
    Draw draws[kWtSetsPerSlot];
    uint32_t nd = 0;
    auto slotFor = [&](uint32_t key) -> int {
        if (nd == 0) { nd = 1; draws[0].first = n; }
        Draw& d = draws[nd - 1];
        for (uint32_t i = 0; i < d.nTex; ++i)
            if (d.keys[i] == key) return (int)i;
        if (d.nTex < kWtTexSlots) { d.keys[d.nTex] = key; return (int)d.nTex++; }
        if (nd >= kWtSetsPerSlot) return -1;
        d.count = n - d.first;
        Draw& e = draws[nd++];
        e.first = n;
        e.keys[0] = key;
        e.nTex = 1;
        return 0;
    };
    // The anchor's per-eye depth band (setLabelAnchor's, per eye): hide = the anchor pulled
    // bias + fade toward that eye, show = pulled bias. An eye where it can't be placed (behind
    // it, inside the bias) gets (2, 1): visible over the whole [0, 1] depth range.
    constexpr float kFade = 0.35f;
    constexpr float kMinDist = 0.05f;
    auto eyeBand = [&](const float* a, int v, float* o) -> bool {
        o[0] = 2.f; o[1] = 1.f;
        const glm::vec3 p(a[0], a[1], a[2]);
        const float bias = a[3];
        const glm::vec3 toEye = E[v] - p;
        const float dist = glm::length(toEye);
        const glm::vec4 c = VP[v] * glm::vec4(p, 1.f);
        if (!(c.w > 0.f) || !(dist - bias > kMinDist)) return false;
        const glm::vec3 dir = toEye / dist;
        const glm::vec4 cs = VP[v] * glm::vec4(p + dir * bias, 1.f);
        const glm::vec4 ch = VP[v] * glm::vec4(p + dir * std::min(bias + kFade, dist - kMinDist), 1.f);
        if (!(cs.w > 0.f) || !(ch.w > 0.f)) return false;
        const float show = cs.z / cs.w, hide = ch.z / ch.w;
        if (!(std::fabs(hide - show) > 1e-9f)) return false;
        o[0] = hide; o[1] = 1.f / (hide - show);
        return true;
    };
    auto pack = [](const float* c) -> uint32_t {
        uint32_t r = 0;
        for (int i = 0; i < 4; ++i)
        {
            const float x = std::clamp(c[i], 0.f, 1.f);
            r |= (uint32_t)(x * 255.f + 0.5f) << (8 * i);
        }
        return r;
    };

    for (const WtBatch& b : mWtBatches)
    {
        if (b.pending >= mPending.size()) continue;
        const PendingDraw& pd = mPending[b.pending];
        if ((size_t)pd.first + pd.count > mVerts.size()) continue;
        uint32_t tris = 0;
        switch (b.topo)
        {
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:  tris = pd.count / 3; break;
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:   tris = pd.count >= 3 ? pd.count - 2 : 0; break;
        default: ++mWtStats.droppedTopo; continue;   // lines / points: no billboard meaning
        }
        if (!tris) continue;
        // A 2D (orthographic) batch has no world position: every label path lays out in the
        // world in this mode, so this is only something LL drew flat into the layer.
        const glm::mat4& M = pd.mvp;
        if (std::fabs(M[0][3]) + std::fabs(M[1][3]) + std::fabs(M[2][3]) < 1e-7f) { ++mWtStats.dropped2d; continue; }
        if (n + tris * 3 > kWtVertCap) { ++mWtStats.droppedCap; continue; }
        const int ts = slotFor(b.tex * kNumSamplers + (uint32_t)b.samplerIndex);
        if (ts < 0) { ++mWtStats.droppedTex; continue; }
        uint32_t flags = (uint32_t)ts | (b.additive ? 0x100u : 0u);
        float occl[4] = {2.f, 1.f, 2.f, 1.f};
        if (b.occl)
        {
            const bool e0 = eyeBand(b.anchor, 0, occl), e1 = eyeBand(b.anchor, 1, occl + 2);
            if (e0 || e1) { flags |= 0x200u; ++mWtStats.occluded; }
        }
        const glm::mat4 W = invHead * M;   // batch space -> world (agent)
        auto emit = [&](uint32_t vi) {
            const Vertex& s = mVerts[pd.first + vi];
            const glm::vec4 q = W * glm::vec4(s.pos[0], s.pos[1], s.pos[2], 1.f);
            const float iw = std::fabs(q.w) > 1e-12f ? 1.f / q.w : 0.f;
            WtVert& o = out[n++];
            o.pos[0] = q.x * iw; o.pos[1] = q.y * iw; o.pos[2] = q.z * iw;
            o.uv[0] = s.uv[0]; o.uv[1] = s.uv[1];
            o.rgba = pack(s.col);
            o.flags = flags;
            std::memcpy(o.occl, occl, sizeof(occl));
        };
        for (uint32_t t = 0; t < tris; ++t)
        {
            if (b.topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) { emit(3 * t); emit(3 * t + 1); emit(3 * t + 2); }
            else if (b.topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP) { emit(t); emit(t + 1); emit(t + 2); }
            else { emit(0); emit(t + 1); emit(t + 2); }
        }
        ++mWtStats.batches;
    }
    if (nd) draws[nd - 1].count = n - draws[nd - 1].first;
    mWtStats.verts = n;
    if (!n) return;

    // This slot's sets (its previous frame has retired: the slot's fence was waited).
    const VkSampler depthSampler = mSamplers[samplerIndex(TF_POINT, TAM_CLAMP)];   // texelFetch
    for (uint32_t d = 0; d < nd; ++d)
    {
        VkDescriptorImageInfo ii[kWtTexSlots + 1];
        for (uint32_t i = 0; i < kWtTexSlots; ++i)
        {
            uint32_t tex = 0;
            int si = 0;
            if (i < draws[d].nTex)
            {
                tex = draws[d].keys[i] / kNumSamplers;
                si = (int)(draws[d].keys[i] % kNumSamplers);
            }
            if (tex >= mTextures.size() || !mTextures[tex].image.image) tex = 0;   // as descriptorFor
            ii[i] = {mSamplers[si], mTextures[tex].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }
        ii[kWtTexSlots] = {depthSampler, depthArray, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wr[2]{};
        wr[0].sType = wr[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[0].dstSet = wr[1].dstSet = mWtSets[frameSlot][d];
        wr[0].dstBinding = 0;
        wr[0].descriptorCount = kWtTexSlots;
        wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[0].pImageInfo = ii;
        wr[1].dstBinding = 1;
        wr[1].descriptorCount = 1;
        wr[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[1].pImageInfo = &ii[kWtTexSlots];
        vkUpdateDescriptorSets(mCtx->device(), 2, wr, 0, nullptr);
        mWtStats.textures += draws[d].nTex;
    }

    // The eyes' colour after the world's last writes; the depth (every layer) sampled.
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                       VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    mb.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    VkImageMemoryBarrier2 toRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toRead.srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    toRead.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    toRead.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    toRead.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    toRead.oldLayout = depthLayout;
    toRead.newLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    toRead.image = depthImage;
    toRead.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
    {
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &toRead;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = colorArray;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w, h}};
    ri.layerCount = 1;
    ri.viewMask = 0x3u;   // both eyes, one draw
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp{0.f, 0.f, (float)w, (float)h, 0.f, 1.f};
    const VkRect2D sc{{0, 0}, {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mWtPipe);
    const VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &mWtVbo[frameSlot].buffer, &off);
    float push[2][16];
    std::memcpy(push, viewProj, sizeof(push));
    VK_PUSH_BUDGET_CHECK(push);
    vkCmdPushConstants(cmd, mWtLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), push);
    for (uint32_t d = 0; d < nd; ++d)
    {
        if (!draws[d].count) continue;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mWtLayout, 0, 1, &mWtSets[frameSlot][d], 0, nullptr);
        vkCmdDraw(cmd, draws[d].count, 1, draws[d].first, 0);
        ++mWtStats.draws;
    }
    vkCmdEndRendering(cmd);

    VkImageMemoryBarrier2 back = toRead;
    back.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    back.srcAccessMask = 0;   // reads: an execution dependency
    back.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    back.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_2_TRANSFER_READ_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    back.newLayout = depthLayout;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &back;
    vkCmdPipelineBarrier2(cmd, &dep);
    mWtStats.recorded = true;
}

void LLRenderVK::recordUIRange(VkCommandBuffer cmd, VkBuffer vbo, size_t first, size_t last)
{
    // The 2D UI batches of [first, last), skipping the in-world segments (see recordFrame).
    auto seg = [&](size_t b, size_t e)
    {
        b = std::max(b, first);
        e = std::min(e, last);
        if (b < e)
            recordDraws(cmd, mSwapW, mSwapH, vbo, b, e);
    };
    seg(0, mOverlayBegin);
    if (mPreHudRecorded || mWorldTextViews)   // (#1133 VR: the layer is in the world, never the 2D UI)
    {
        seg(mOverlayEnd, mPreHudBegin);
        seg(mPreHudEnd, (size_t)-1);
    }
    else
        seg(mOverlayEnd, (size_t)-1);
}

void LLRenderVK::recordFrame(VkCommandBuffer cmd, uint32_t frameSlot, bool advanceFrame) {
    if (mInOverlay) endWorldOverlay();
    if (mInPreHud) endPreHudLayer();
    flush();

    // Upload this frame's vertices into the per-frame-in-flight VBO ring slot. The
    // caller waited this slot's in-flight fence before recording, so the previous
    // frame that used the same slot (and the same swapchain image) is retired and
    // this overwrite cannot stomp an in-flight draw. (Already done if the world
    // renderer recorded the overlay segment this frame.)
    VkBuffer vbo = mFrameVbo ? mFrameVbo : uploadFrameVerts(frameSlot);

    // Dynamic rendering over the swapchain image view. LOAD (not CLEAR): the
    // caller already cleared/blitted the color image, so the UI composites on
    // top. The caller has transitioned the image to COLOR_ATTACHMENT_OPTIMAL.
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = mFrameColorView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {mSwapW, mSwapH}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    // The world-overlay segment [mOverlayBegin, mOverlayEnd) was recorded by the
    // world renderer against the world depth; if it wasn't (no world pass this
    // frame) those batches are dropped -- their pipelines declare a depth format
    // this no-depth pass can't use. Everything else is the 2D UI.
    // The pre-HUD text layer [mPreHudBegin, mPreHudEnd) (after the overlay segment)
    // is skipped when the world recorded it under the HUD pass; otherwise it draws
    // here in buffer order (depth-less pipelines: valid in this pass).
    // #703: the external layer (RmlUi) is recorded at each of its marks (one segment per
    // contiguous run of presented views), between the batches below and above it; with no
    // mark, once under every batch.
    size_t drawn = 0;
    if (mExternalLayer)
    {
        const uint32_t segments = std::max<uint32_t>(1u, mExternalMarkCount);
        for (uint32_t k = 0; k < segments; ++k)
        {
            const size_t at = k < mExternalMarkCount ? std::max(drawn, std::min(mExternalMarks[k], mPending.size())) : drawn;
            if (at > drawn)
            {
                vkCmdBeginRendering(cmd, &ri);
                recordUIRange(cmd, vbo, drawn, at);
                vkCmdEndRendering(cmd);
                drawn = at;
            }
            mExternalLayer(cmd, k);
        }
    }
    vkCmdBeginRendering(cmd, &ri);
    recordUIRange(cmd, vbo, drawn, (size_t)-1);
    vkCmdEndRendering(cmd);
    if (mExternalLayer)
    {
        mExternalLayer     = {};
        mExternalMarkCount = 0;
    }
    else if (advanceFrame)
        mExternalMarkCount = 0; // marks without a layer this frame (no recorder): never carried over

    mPending.clear();
    mInFrame = false;
    mOverlayBegin = mOverlayEnd = 0;
    mOverlayRecorded = false;
    mInPreHud = false;
    mPreHudBegin = mPreHudEnd = 0;
    mPreHudRecorded = false;
    mPreHudOccl = false;
    mAnchorOn = false;
    if (!mWtBatches.empty()) mWtBatches.clear();   // #1133 VR (they index mPending)
    mFrameVbo = VK_NULL_HANDLE;
    mRecordingFrame = false;
    // Multiple windows share textures and their deferred-release clock. Advancing
    // it once per WINDOW could free images still used by the main frame.
    if (advanceFrame)
    {
        ++mFrameCounter;
        processDeferred(/*all=*/false);
    }
}

void LLRenderVK::releaseFrameSlots(uint32_t first, uint32_t count)
{
    for (uint32_t i = first; i < first + count && i < mVBO.size(); ++i)
    {
        if (mVBO[i].buffer) mCtx->destroyBuffer(mVBO[i]);
        mVBO[i] = {};
        mVBOCapacityBytes[i] = 0;
    }
}

bool LLRenderVK::prepareBufferedFrame(uint32_t frameSlot)
{
    // Auxiliary rings have a fixed per-window byte ceiling; an oversized UI
    // returns to its main host rather than growing sixteen independent rings.
    if (mVerts.size() > (16ull << 20) / sizeof(Vertex)) return false;
    while (!mOffStack.empty()) endOffscreen();
    flush();
    mFrameVbo = uploadFrameVerts(frameSlot);
    mDeferred.reserve(mDeferred.size() + mStageRetired.size() + mPendingUploads.size() + 1);
    mStageMarks.reserve(mStageMarks.size() + 1);
    return mFrameVbo != VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// texture update (atlas refresh)
// ---------------------------------------------------------------------------
void LLRenderVK::updateTexture(uint32_t texId, uint32_t w, uint32_t h,
                               const uint8_t* pixels, eTexelFormat fmt) {
    if (!pixels) return;
    uint8_t* dst = beginTextureUpdate(texId, w, h, fmt, 0, 0, w, h);
    if (!dst) return;
    std::memcpy(dst, pixels, (size_t)w * h * texelBytes(fmt));
    commitTextureUpdate();
}

uint8_t* LLRenderVK::beginTextureUpdate(uint32_t texId, uint32_t w, uint32_t h, eTexelFormat fmt,
                                        uint32_t x, uint32_t y, uint32_t rw, uint32_t rh, bool mayDefer)
{
    mUploadOpen = false;
    if (!mCtx || texId >= mTextures.size() || !w || !h || !rw || !rh
        || x >= w || y >= h || rw > w - x || rh > h - y) return nullptr;
    VkMemScope memcat(VKMEM_UI);   // #407
    VkTexture& t = mTextures[texId];
    const bool full = x == 0 && y == 0 && rw == w && rh == h;
    // If dimensions or format changed, recreate the image (atlas can grow). Descriptors
    // referencing the old view are invalidated; drop the cache entries for this
    // texture so descriptorFor() re-points at the new view.
    const bool recreate = t.image.extent.width != w || t.image.extent.height != h || t.texFmt != fmt;
    if (recreate && !full) return nullptr;   // a new image needs every texel
    if (recreate) {
        // In-flight frames (and batches already queued this frame) may still sample
        // the old image through its descriptor sets: defer both, don't destroy now.
        DeferredFree d;
        d.frame = mFrameCounter;
        d.image = t.image;
        takeDescSets(texId, d.sets);
        mDeferred.push_back(std::move(d));
        t.image = createSampledImage(w, h, fmt);
        t.view = t.image.view;
        t.texFmt = fmt;
        mayDefer = false;   // the new image has no contents yet
    }
    if (!t.image.image) return nullptr;

    PendingUpload& u = mOpenUpload;
    u = PendingUpload{};
    u.image = t.image.image;
    u.x = x; u.y = y; u.w = rw; u.h = rh;
    u.preserve = !full;   // keep the texels outside the rectangle
    uint8_t* dst = allocStaging((VkDeviceSize)rw * rh * texelBytes(fmt), u, mayDefer);
    if (!dst) return nullptr;
    if (!full) ++mStagePartial;
    mUploadOpen = true;
    return dst;
}

void LLRenderVK::commitTextureUpdate()
{
    if (!mUploadOpen) return;
    mUploadOpen = false;
    submitUpload(mOpenUpload);
    mOpenUpload = PendingUpload{};
}

// ---------------------------------------------------------------------------
// immediate-mode geometry
// ---------------------------------------------------------------------------
glm::mat4 LLRenderVK::currentMVP() const {
    return mMat[MM_PROJECTION][mMatIdx[MM_PROJECTION]] *
           mMat[MM_MODELVIEW][mMatIdx[MM_MODELVIEW]];
}

void LLRenderVK::begin(eGeomMode mode) {
    // A change in primitive family (or any deferred state) means the previous
    // batch must be closed; mirrors gGL's flush-on-mode-change.
    flush();
    mMode = mode;
    mInBegin = true;
    mBeginVertStart = (uint32_t)mVerts.size();

    // Open a new batch under the *current* live state.
    VkPrimitiveTopology topo = topoFor(mode);
    mOpenKey = liveKey(topo);
    const TexUnit& u0 = mTexUnits[0];
    mOpenTex = u0.boundTex;
    mOpenSamplerIndex = samplerIndex(u0.filter, u0.address);
    mOpenMVP = currentMVP();
    // #1141 VR stereo: a world-space overlay batch (the overlay camera's projection, bit for
    // bit: setVKOverlayCamera / setup3DRender load the same floats) is re-projected per eye.
    // Desktop: mOverlayStereo is false, nothing is compared.
    mOpenWorldCam = mOverlayStereo && mInOverlay &&
                    std::memcmp(&mMat[MM_PROJECTION][mMatIdx[MM_PROJECTION]][0][0], &mOverlayProj[0][0],
                                sizeof(float) * 16) == 0;
    // #487: the anchor of an occlusion-tested label batch (liveKey decided).
    std::memcpy(mOpenAnchor, mAnchor, sizeof(mOpenAnchor));
    if (mWorldTextViews)
    {   // #1133 VR: an anchored label drawn with LL's depth test on is occluded per eye.
        mOpenWtOccl = mInPreHud && mWtBbAnchorOn && mDepthTest;
        std::memcpy(mOpenWtAnchor, mWtBbAnchor, sizeof(mOpenWtAnchor));
    }
    mOpenLineWidth = mLineWidth;
    mOpenIsLine = isLineTopo(topo);
    mOpenScissorEnable = mScissorEnable;
    if (mScissorEnable) {
        // Flip GL bottom-left origin to Vulkan top-left (render targets keep GL's
        // row order, so no flip there).
        int x = mScissorX;
        int y = mOffStack.empty() ? (int)mFbHeight - (mScissorY + mScissorH) : mScissorY;
        int w = mScissorW < 0 ? 0 : mScissorW;
        int h = mScissorH < 0 ? 0 : mScissorH;
        // A NEGATIVE VkRect2D.offset is invalid and the driver voids the draw (5000+
        // validation errors). Clamp offset to >= 0 and shrink the extent by the
        // clipped-off amount so the visible region stays correct.
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (w < 0) w = 0;
        if (h < 0) h = 0;
        mOpenScissor.offset = {x, y};
        mOpenScissor.extent = {(uint32_t)w, (uint32_t)h};
    }
    mHaveOpen = true;
    mFrameVertBase = (uint32_t)mVerts.size();
}

void LLRenderVK::emitVertex(float x, float y, float z) {
    Vertex v;
    v.pos[0] = x; v.pos[1] = y; v.pos[2] = z;
    v.col[0] = mCurColor[0]; v.col[1] = mCurColor[1];
    v.col[2] = mCurColor[2]; v.col[3] = mCurColor[3];
    // Texture matrix (unit 0), as LL's GL UI shader applies texture_matrix0 to the
    // texcoords: scrolling / scaled UI textures and the move tool's snap grid (which
    // scrolls the grid so its lines stay fixed in the world). Only the 2D terms matter
    // for (u, v, 0, 1).
    const glm::mat4& tm = mMat[MM_TEXTURE0][mMatIdx[MM_TEXTURE0]];
    v.uv[0] = tm[0][0] * mCurUV[0] + tm[1][0] * mCurUV[1] + tm[3][0];
    v.uv[1] = tm[0][1] * mCurUV[0] + tm[1][1] * mCurUV[1] + tm[3][1];
    mVerts.push_back(v);
}

void LLRenderVK::end() {
    if (!mInBegin) return;
    mInBegin = false;

    // QUADS emulation: gGL accepts 4-vertex quads; expand each run of 4 into two
    // triangles (0,1,2)+(0,2,3). We appended them raw during begin..end, so
    // rewrite the tail in place.
    if (mMode == QUADS) {
        uint32_t start = mBeginVertStart;
        uint32_t n = (uint32_t)mVerts.size() - start;
        uint32_t quads = n / 4;
        std::vector<Vertex> tri;
        tri.reserve(quads * 6);
        for (uint32_t q = 0; q < quads; ++q) {
            Vertex* base = &mVerts[start + q * 4];
            tri.push_back(base[0]); tri.push_back(base[1]); tri.push_back(base[2]);
            tri.push_back(base[0]); tri.push_back(base[2]); tri.push_back(base[3]);
        }
        mVerts.resize(start);
        for (auto& v : tri) mVerts.push_back(v);
    }
    // LINE_LOOP emulation: rendered as a LINE_STRIP, so append a copy of the
    // first vertex to close the loop (edge Vn -> V0). Matches GL LINE_LOOP.
    else if (mMode == LINE_LOOP) {
        uint32_t start = mBeginVertStart;
        if (mVerts.size() - start >= 2) {
            Vertex first = mVerts[start];
            mVerts.push_back(first);
        }
    }
    // The batch stays open; it is flushed on the next state change / endFrame,
    // so consecutive begin/end pairs with identical state coalesce into one draw
    // (exactly the batching win gGL provides).
}

void LLRenderVK::vertex2f(float x, float y) { emitVertex(x, y, 0.0f); }
void LLRenderVK::vertex3f(float x, float y, float z) { emitVertex(x, y, z); }
void LLRenderVK::texCoord2f(float u, float v) { mCurUV[0] = u; mCurUV[1] = v; }

void LLRenderVK::color4ub(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    mCurColor[0] = r / 255.0f;
    mCurColor[1] = g / 255.0f;
    mCurColor[2] = b / 255.0f;
    mCurColor[3] = a / 255.0f;
}
void LLRenderVK::color4f(float r, float g, float b, float a) {
    mCurColor[0] = r; mCurColor[1] = g; mCurColor[2] = b; mCurColor[3] = a;
}

// Mirrors gGL::vertexBatchPreTransformed: append a run of pre-built vertices
// (position + uv + packed RGBA8 color) directly. This is the exact call
// LLFontGL uses for glyph quads (6 verts/glyph, TRIANGLES). Must be inside
// begin()/end().
void LLRenderVK::vertexBatchPreTransformed(const PreVert* verts, int count) {
    for (int i = 0; i < count; ++i) {
        const PreVert& s = verts[i];
        Vertex v;
        v.pos[0] = s.x; v.pos[1] = s.y; v.pos[2] = s.z;
        v.col[0] = s.r / 255.0f; v.col[1] = s.g / 255.0f;
        v.col[2] = s.b / 255.0f; v.col[3] = s.a / 255.0f;
        v.uv[0] = s.u; v.uv[1] = s.v;
        mVerts.push_back(v);
    }
}

// ---------------------------------------------------------------------------
// matrix stack (glm; identical semantics to gGL's glm-based stack)
// ---------------------------------------------------------------------------
void LLRenderVK::matrixMode(eMatrixMode m) {
    // Resolve the MM_TEXTURE alias to the active unit's concrete texture stack.
    mMatrixMode = (eMatrixMode)resolveMM(m);
}

void LLRenderVK::pushMatrix() {
    flush();
    int& idx = mMatIdx[mMatrixMode];
    if (idx < kStackDepth - 1) {
        mMat[mMatrixMode][idx + 1] = mMat[mMatrixMode][idx];
        ++idx;
    }
}
void LLRenderVK::popMatrix() {
    flush();
    if (mMatIdx[mMatrixMode] > 0) --mMatIdx[mMatrixMode];
}
void LLRenderVK::loadIdentity() {
    flush();
    mMat[mMatrixMode][mMatIdx[mMatrixMode]] = glm::mat4(1.0f);
}
void LLRenderVK::loadMatrix(const float* m16) {
    flush();
    std::memcpy(&mMat[mMatrixMode][mMatIdx[mMatrixMode]][0][0], m16,
                sizeof(float) * 16);
}
void LLRenderVK::multMatrix(const float* m16) {
    flush();
    glm::mat4 m;
    std::memcpy(&m[0][0], m16, sizeof(float) * 16);
    mMat[mMatrixMode][mMatIdx[mMatrixMode]] *= m;
}
void LLRenderVK::translatef(float x, float y, float z) {
    flush();
    glm::mat4& cur = mMat[mMatrixMode][mMatIdx[mMatrixMode]];
    cur = glm::translate(cur, glm::vec3(x, y, z));
}
void LLRenderVK::scalef(float x, float y, float z) {
    flush();
    glm::mat4& cur = mMat[mMatrixMode][mMatIdx[mMatrixMode]];
    cur = glm::scale(cur, glm::vec3(x, y, z));
}
void LLRenderVK::rotatef(float deg, float x, float y, float z) {
    flush();
    glm::mat4& cur = mMat[mMatrixMode][mMatIdx[mMatrixMode]];
    cur = glm::rotate(cur, glm::radians(deg), glm::vec3(x, y, z));
}
void LLRenderVK::ortho(float l, float r, float b, float t, float zn, float zf) {
    flush();
    // GLM_FORCE_DEPTH_ZERO_TO_ONE makes glm::ortho emit Vulkan's [0,1] clip Z.
    // We also negate the Y scale so that, with a top>bottom UI convention
    // (top-left origin like gGL's UI ortho of (0,w,0,h) mapped to a Y-down
    // framebuffer), +Y in caller space goes DOWN the framebuffer — matching how
    // the viewer lays out UI. Here we simply build a standard glm ortho and let
    // the caller pass (0,w,h,0) or flip; to keep parity with gGL (which passes
    // bottom<top and relies on GL's Y-up NDC) we flip Y in the matrix.
    // Swap bottom/top so the whole Y row (scale AND translation) flips
    // consistently for Vulkan's y-down framebuffer; negating only o[1][1] leaves
    // the translation term wrong and pushes geometry off-screen.
    glm::mat4 o = glm::ortho(l, r, t, b, zn, zf);
    mMat[mMatrixMode][mMatIdx[mMatrixMode]] *= o;
}

// ---------------------------------------------------------------------------
// render state
// ---------------------------------------------------------------------------
void LLRenderVK::blendFunc(eBlendFactor s, eBlendFactor d) {
    if (s == mSF && d == mDF && mBlendEnabled) return;
    flush();
    mSF = s;
    mDF = d;
    mBlendEnabled = true;
}

void LLRenderVK::setSceneBlendType(eBlendType type) {
    flush();
    switch (type) {
        case BT_ALPHA:
            mBlendEnabled = true;
            mSF = BF_SOURCE_ALPHA; mDF = BF_ONE_MINUS_SOURCE_ALPHA; break;
        case BT_ADD:
            mBlendEnabled = true; mSF = BF_ONE; mDF = BF_ONE; break;
        case BT_MULT:
            mBlendEnabled = true; mSF = BF_DEST_COLOR; mDF = BF_ZERO; break;
        case BT_REPLACE:
            mBlendEnabled = false; mSF = BF_ONE; mDF = BF_ZERO; break;
    }
}

void LLRenderVK::setLineWidth(float w) {
    if (w == mLineWidth) return;
    flush();               // line width is per-draw dynamic state
    mLineWidth = w;
}

void LLRenderVK::bindTexture(uint32_t texId) {
    TexUnit& u = mTexUnits[mActiveUnit];
    uint32_t resolved = (texId < mTextures.size()) ? texId : 0;
    // Adopt the texture's uploaded filter (mirrors LLTextureData carrying its own
    // filtering). A subsequent setTextureFilter() can still override it.
    eTextureFilter texFilter = mTextures[resolved].defaultFilter;
    // Render targets (avatar composites, previews) resample LL textures: bilinear,
    // as GL's LLTextureData default filtering, not the UI's point default.
    if (!mOffStack.empty()) texFilter = TF_BILINEAR;
    if (resolved == u.boundTex && texFilter == u.filter) return;
    if (mActiveUnit == 0) flush();   // unit 0 is the sampled descriptor
    u.boundTex = resolved;
    u.filter = texFilter;
}

// ---- color mask / depth / scissor -----------------------------------------
void LLRenderVK::setColorMask(bool writeColor, bool writeAlpha) {
    setColorMask(writeColor, writeColor, writeColor, writeAlpha);
}
void LLRenderVK::setColorMask(bool r, bool g, bool b, bool a) {
    uint8_t m = (r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0);
    if (m == mColorMask) return;
    flush();
    mColorMask = m;
}

void LLRenderVK::setDepthTest(bool enabled, bool writeEnabled,
                              eCompareFunc func) {
    // Mirror LLRender::DepthScope: disabling the test forces writes off (GL spec).
    if (!enabled) writeEnabled = false;
    if (enabled == mDepthTest && writeEnabled == mDepthWrite &&
        func == mDepthFunc)
        return;
    // Swapchain 2D layer: depth never reaches its batch keys (liveKey forces it
    // off outside the world-overlay segment), so track the state without breaking
    // the current batch.
    if (!(mFrameToSwapchain && !mInOverlay && mOffStack.empty()))
        flush();
    mDepthTest = enabled;
    mDepthWrite = writeEnabled;
    mDepthFunc = func;
}
void LLRenderVK::enableDepthTest(bool writeEnabled, eCompareFunc func) {
    setDepthTest(true, writeEnabled, func);
}
void LLRenderVK::disableDepthTest() {
    setDepthTest(false, false, mDepthFunc);
}

void LLRenderVK::setScissor(bool enable, int x, int y, int w, int h) {
    if (enable == mScissorEnable && (!enable ||
        (x == mScissorX && y == mScissorY && w == mScissorW &&
         h == mScissorH)))
        return;
    flush();
    mScissorEnable = enable;
    mScissorX = x; mScissorY = y; mScissorW = w; mScissorH = h;
}

// ---- texture units ---------------------------------------------------------
void LLRenderVK::setActiveTextureUnit(uint32_t unit) {
    if (unit >= (uint32_t)kNumTexUnits) unit = kNumTexUnits - 1;
    mActiveUnit = unit;
}
void LLRenderVK::setTextureAddressMode(eTextureAddressMode mode) {
    TexUnit& u = mTexUnits[mActiveUnit];
    if (mode == u.address) return;
    if (mActiveUnit == 0) flush();
    u.address = mode;
}
void LLRenderVK::setTextureFilter(eTextureFilter filter) {
    TexUnit& u = mTexUnits[mActiveUnit];
    if (filter == u.filter) return;
    if (mActiveUnit == 0) flush();
    u.filter = filter;
}

// ---------------------------------------------------------------------------
// offscreen render targets + segments (#83: LLViewerDynamicTexture under VK)
// ---------------------------------------------------------------------------
uint32_t LLRenderVK::createRenderTarget(uint32_t w, uint32_t h, bool withDepth)
{
    if (!mCtx || w == 0 || h == 0) return 0;
    VkMemScope memcat(VKMEM_UI);   // #407
    VkTexture t;
    t.image = mCtx->createImage2D(
        w, h, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    if (!t.image.image) return 0;
    t.view = t.image.view;
    t.dset = VK_NULL_HANDLE;
    t.defaultFilter = TF_BILINEAR;
    RenderTargetInfo info;
    if (withDepth)
    {
        info.depth = mCtx->createImage2D(w, h, VK_FORMAT_D32_SFLOAT,
                                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                         VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    if (!mFreeTexIds.empty())
    {
        t.id = mFreeTexIds.back();
        mFreeTexIds.pop_back();
        mTextures[t.id] = t;
    }
    else
    {
        t.id = (uint32_t)mTextures.size();
        mTextures.push_back(t);
    }
    mRenderTargets[t.id] = info;
    return t.id;
}

bool LLRenderVK::isRenderTargetReady(uint32_t texId) const
{
    auto it = mRenderTargets.find(texId);
    return it != mRenderTargets.end() && it->second.ready
        && texId < mTextures.size() && mTextures[texId].image.image && !mTextures[texId].pendingFree;
}

VkImage LLRenderVK::textureImage(uint32_t texId) const
{
    return texId < mTextures.size() ? mTextures[texId].image.image : VK_NULL_HANDLE;
}

bool LLRenderVK::textureInfo(uint32_t texId, VkImage& image, VkImageView& view, VkExtent2D& extent) const
{
    if (!texId || texId >= mTextures.size() || mTextures[texId].pendingFree || !mTextures[texId].image.image || !mTextures[texId].view)
        return false;
    image  = mTextures[texId].image.image;
    view   = mTextures[texId].view;
    extent = mTextures[texId].image.extent;
    return true;
}

uint64_t LLRenderVK::textureAllocBytes(uint32_t texId) const
{
    if (!mCtx || texId >= mTextures.size() || !mTextures[texId].image.memory) return 0;
    return mCtx->allocBytes(mTextures[texId].image);   // (pooled: its suballocation, #411)
}

bool LLRenderVK::texturePooled(uint32_t texId) const
{
    return mCtx && texId < mTextures.size() && mCtx->imagePooled(mTextures[texId].image.image);
}

void LLRenderVK::memStats(MemStats& o) const
{
    o = MemStats{};
    if (!mCtx) return;
    for (const VkTexture& t : mTextures)
    {
        if (!t.image.memory) continue;
        const uint64_t b = mCtx->allocBytes(t.image);
        if (t.pendingFree) { o.pendingBytes += b; ++o.pendingCount; }
        else if (mRenderTargets.count(t.id)) { o.rtBytes += b; ++o.rtCount; }
        else { o.texBytes += b; ++o.texCount; }
    }
    for (const auto& kv : mRenderTargets)
    {
        if (!kv.second.depth.memory) continue;
        o.rtDepthBytes += mCtx->allocBytes(kv.second.depth);
        ++o.rtDepthCount;
    }
    for (const DeferredFree& d : mDeferred)
    {
        if (d.staging.buffer) o.stagingBytes += d.staging.size;
        // A released texture id keeps its image in its slot until the deferred point
        // (counted above); other deferred images are resize leftovers / target depth.
        if (!d.image.memory) continue;
        // (compare images: a pooled image's memory is a block shared with others, #411)
        if (d.recycleId && d.recycleId < mTextures.size() && mTextures[d.recycleId].image.image == d.image.image)
            continue;
        o.pendingBytes += mCtx->allocBytes(d.image);
        ++o.pendingCount;
    }
    for (const PendingUpload& u : mPendingUploads) o.stagingBytes += u.owned.size;
    for (const GpuBuffer& b : mStageRetired) o.stagingBytes += b.size;
    o.stageRingCap = mStageRing.cap;
    o.stageRingUsed = mStageRing.used();
    o.stageRingPeak = mStagePeak;
    o.stageFrameBytes = mStageFrameBytes;
    o.stageFallbacks = mStageFallbacks;
    o.stageUploads = mStageUploads;
    o.stagePartial = mStagePartial;
    o.stageDeferred = mStageDeferred;
    for (VkDeviceSize c : mVBOCapacityBytes) o.ringBytes += c;
}

void LLRenderVK::saveState(SavedState& st) const
{
    st.blend = mBlendEnabled; st.sf = mSF; st.df = mDF; st.colorMask = mColorMask;
    st.depthTest = mDepthTest; st.depthWrite = mDepthWrite; st.depthFunc = mDepthFunc;
    st.scissor = mScissorEnable; st.sx = mScissorX; st.sy = mScissorY; st.sw = mScissorW; st.sh = mScissorH;
    st.lineWidth = mLineWidth; st.minAlpha = mMinAlpha;
    st.units = mTexUnits; st.activeUnit = mActiveUnit;
    st.mat = mMat; st.matIdx = mMatIdx; st.matrixMode = mMatrixMode;
    std::memcpy(st.color, mCurColor, sizeof(st.color));
    std::memcpy(st.uv, mCurUV, sizeof(st.uv));
}

void LLRenderVK::restoreState(const SavedState& st)
{
    mBlendEnabled = st.blend; mSF = st.sf; mDF = st.df; mColorMask = st.colorMask;
    mDepthTest = st.depthTest; mDepthWrite = st.depthWrite; mDepthFunc = st.depthFunc;
    mScissorEnable = st.scissor; mScissorX = st.sx; mScissorY = st.sy; mScissorW = st.sw; mScissorH = st.sh;
    mLineWidth = st.lineWidth; mMinAlpha = st.minAlpha;
    mTexUnits = st.units; mActiveUnit = st.activeUnit;
    mMat = st.mat; mMatIdx = st.matIdx; mMatrixMode = st.matrixMode;
    std::memcpy(mCurColor, st.color, sizeof(st.color));
    std::memcpy(mCurUV, st.uv, sizeof(st.uv));
}

void LLRenderVK::openOffRun(uint32_t tex, bool clearColor, const float* clear, bool clearDepth)
{
    OffRun r;
    r.tex = tex;
    r.begin = r.end = mPending.size();
    r.clearColor = clearColor;
    for (int i = 0; i < 4; ++i) r.clear[i] = clear ? clear[i] : 0.0f;
    r.clearDepth = clearDepth;
    mOffRuns.push_back(r);
}

void LLRenderVK::closeOffRun()
{
    if (mOffRuns.empty()) return;
    OffRun& r = mOffRuns.back();
    r.end = mPending.size();
    // An empty resumed run (nothing drawn after a nested segment) needs no pass.
    if (r.end == r.begin && !r.clearColor && !r.clearDepth) mOffRuns.pop_back();
}

bool LLRenderVK::beginOffscreen(uint32_t texId, bool clearColor, const float clearRGBA[4], bool clearDepth)
{
    if (!mInFrame || !mFrameToSwapchain || mInBegin) return false;
    if (mRenderTargets.find(texId) == mRenderTargets.end()) return false;
    if (texId >= mTextures.size() || !mTextures[texId].image.image || mTextures[texId].pendingFree) return false;
    if (mInOverlay || mInPreHud) return false;   // never interleave with a world-recorded segment
    flush();
    if (!mOffStack.empty()) closeOffRun();   // nested: the parent resumes afterwards
    OffFrame f;
    f.tex = texId;
    saveState(f.saved);
    mOffStack.push_back(f);
    openOffRun(texId, clearColor, clearRGBA, clearDepth);

    // Fresh GL-default state for the target (LLRender::UIScope-like): alpha blend, all
    // channels, no depth / scissor / alpha test, identity matrices.
    mBlendEnabled = true; mSF = BF_SOURCE_ALPHA; mDF = BF_ONE_MINUS_SOURCE_ALPHA;
    mColorMask = 0xF;
    mDepthTest = false; mDepthWrite = false; mDepthFunc = CF_LESS_EQUAL;
    mScissorEnable = false;
    mLineWidth = 1.0f;
    mMinAlpha = 0.0f;
    for (auto& u : mTexUnits) u = TexUnit{};
    mActiveUnit = 0;
    for (int m = 0; m < NUM_MM; ++m) { mMatIdx[m] = 0; mMat[m][0] = glm::mat4(1.0f); }
    mMatrixMode = MM_MODELVIEW;
    return true;
}

void LLRenderVK::endOffscreen()
{
    if (mOffStack.empty()) return;
    if (mInBegin) end();
    flush();
    closeOffRun();
    OffFrame f = mOffStack.back();
    mOffStack.pop_back();
    restoreState(f.saved);
    // Resume the parent target: a new run that LOADs what it drew so far.
    if (!mOffStack.empty()) openOffRun(mOffStack.back().tex, false, nullptr, false);
}

void LLRenderVK::clearOffscreenDepth()
{
    if (mOffStack.empty()) return;
    if (mInBegin) end();
    flush();
    closeOffRun();
    openOffRun(mOffStack.back().tex, false, nullptr, true);
}

void LLRenderVK::setMinimumAlpha(float a)
{
    if (a == mMinAlpha) return;
    if (!mOffStack.empty()) flush();
    mMinAlpha = a;
}

void LLRenderVK::recordOffscreen(VkCommandBuffer cmd, uint32_t frameSlot)
{
    // Close anything left open (unbalanced begin/end: draw what we have).
    while (!mOffStack.empty()) endOffscreen();
    if (mOffRuns.empty()) return;
    flush();
    if (!mFrameVbo) mFrameVbo = uploadFrameVerts(frameSlot);

    mRecordingOffscreen = true;
    for (const OffRun& r : mOffRuns)
    {
        auto it = mRenderTargets.find(r.tex);
        if (it == mRenderTargets.end() || r.tex >= mTextures.size()) continue;
        VkTexture& t = mTextures[r.tex];
        if (!t.image.image || t.pendingFree) continue;
        RenderTargetInfo& info = it->second;
        const uint32_t w = t.image.extent.width, h = t.image.extent.height;
        const bool hasDepth = info.depth.image != VK_NULL_HANDLE;
        const bool clearDepth = hasDepth && (r.clearDepth || !info.depthInit);

        VkImageMemoryBarrier2 bars[2];
        uint32_t nb = 0;
        {
            // Prior reads: last frame's UI / world sampling, the world's composite copy.
            VkImageMemoryBarrier2& b = bars[nb++];
            b = VkImageMemoryBarrier2{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            // A cleared target discards its old contents; a resumed / LOADed one keeps them.
            b.oldLayout = (info.ready && !r.clearColor) ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                         : VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            b.image = t.image.image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        if (hasDepth)
        {
            VkImageMemoryBarrier2& b = bars[nb++];
            b = VkImageMemoryBarrier2{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            b.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            b.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            b.oldLayout = clearDepth ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            b.image = info.depth.image;
            b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        }
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = nb;
        dep.pImageMemoryBarriers = bars;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = t.view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = (r.clearColor || !info.ready) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        for (int i = 0; i < 4; ++i) color.clearValue.color.float32[i] = r.clearColor ? r.clear[i] : 0.0f;
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (hasDepth)
        {
            depth.imageView = info.depth.view;
            depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depth.loadOp = clearDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depth.clearValue.depthStencil = {1.0f, 0};   // conventional depth
        }
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {w, h}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        if (hasDepth) ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &ri);
        recordDraws(cmd, w, h, mFrameVbo, r.begin, r.end);
        vkCmdEndRendering(cmd);

        VkImageMemoryBarrier2 toRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toRead.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        toRead.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        toRead.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                              VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        toRead.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT |
                               VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.image = t.image.image;
        toRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo d2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d2.imageMemoryBarrierCount = 1;
        d2.pImageMemoryBarriers = &toRead;
        vkCmdPipelineBarrier2(cmd, &d2);

        info.ready = true;
        if (hasDepth) info.depthInit = true;
    }
    mRecordingOffscreen = false;
    mOffRuns.clear();
}
