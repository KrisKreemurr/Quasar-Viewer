// Quasar frame generation, simple mode. See qlframegenvk.h.
#include "qlframegenvk.h"
#include "qlframegen_spv.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace {
bool sWanted = false;
QLFrameGenTuning sTuning;

void imageBarrier2(VkCommandBuffer cmd, VkImage img,
                   VkImageLayout from, VkImageLayout to,
                   VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

void copyWhole(VkCommandBuffer cmd, VkImage src, VkImage dst, uint32_t w, uint32_t h)
{
    VkImageCopy r{};
    r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.extent = {w, h, 1};
    vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
}

VkShaderModule makeModule(VkDevice dev, const uint32_t* code, size_t bytes)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

struct WarpPush
{
    float invTarget[2];
    float t;
    float motionValid;
    float motionScale;
    float flipY;
    float blendPrev;
    float pad;
};
static_assert(sizeof(WarpPush) == 32, "matches ql_fg_warp.frag");
} // namespace

namespace { std::atomic<unsigned> sGenerated{0}; }
void qlFrameGenNoteGenerated(unsigned n) { sGenerated.fetch_add(n, std::memory_order_relaxed); }
unsigned qlFrameGenTakeGenerated() { return sGenerated.exchange(0, std::memory_order_relaxed); }

void qlSetFrameGenWanted(bool on) { sWanted = on; }
bool qlFrameGenWanted() { return sWanted; }
void qlSetFrameGenTuning(const QLFrameGenTuning& t) { sTuning = t; }
const QLFrameGenTuning& qlFrameGenTuning() { return sTuning; }

bool QLFrameGenVK::ensurePipeline(VkDeviceContext& ctx, VkFormat format)
{
    if (mPipe && mPipeFormat == format) return true;
    VkDevice dev = ctx.device();
    if (mPipe) { vkDestroyPipeline(dev, mPipe, nullptr); mPipe = VK_NULL_HANDLE; }

    if (!mLinear)
    {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0.f;
        if (vkCreateSampler(dev, &si, nullptr, &mLinear) != VK_SUCCESS) return false;
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        if (vkCreateSampler(dev, &si, nullptr, &mNearest) != VK_SUCCESS) return false;
    }
    if (!mDsl)
    {
        VkDescriptorSetLayoutBinding b[3]{};
        for (uint32_t i = 0; i < 3; ++i)
        {
            b[i].binding = i;
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 3;
        li.pBindings = b;
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &mDsl) != VK_SUCCESS) return false;

        VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(WarpPush)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &mDsl;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pr;
        if (vkCreatePipelineLayout(dev, &pli, nullptr, &mLayout) != VK_SUCCESS) return false;

        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * kMaxSlots};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = kMaxSlots;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(dev, &pi, nullptr, &mPool) != VK_SUCCESS) return false;
        VkDescriptorSetLayout layouts[kMaxSlots];
        for (uint32_t i = 0; i < kMaxSlots; ++i) layouts[i] = mDsl;
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = mPool;
        ai.descriptorSetCount = kMaxSlots;
        ai.pSetLayouts = layouts;
        if (vkAllocateDescriptorSets(dev, &ai, mSet) != VK_SUCCESS) return false;
    }

    VkShaderModule vs = makeModule(dev, kQLFgFullscreenVert, sizeof(kQLFgFullscreenVert));
    VkShaderModule fs = makeModule(dev, kQLFgWarpFrag, sizeof(kQLFgWarpFrag));
    if (!vs || !fs)
    {
        if (vs) vkDestroyShaderModule(dev, vs, nullptr);
        if (fs) vkDestroyShaderModule(dev, fs, nullptr);
        return false;
    }
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &format;

    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri;
    gi.stageCount = 2;
    gi.pStages = st;
    gi.pVertexInputState = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &dy;
    gi.layout = mLayout;
    const VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gi, nullptr, &mPipe);
    vkDestroyShaderModule(dev, vs, nullptr);
    vkDestroyShaderModule(dev, fs, nullptr);
    if (r != VK_SUCCESS) { mPipe = VK_NULL_HANDLE; return false; }
    mPipeFormat = format;
    return true;
}

void QLFrameGenVK::destroyImages(VkDeviceContext& ctx)
{
    for (GpuImage& i : mHud) if (i.image) ctx.destroyImage(i);
    if (mGen.image) ctx.destroyImage(mGen);
    mHud[0] = mHud[1] = mGen = GpuImage{};
    mW = mH = 0;
    mHistory = false;
}

bool QLFrameGenVK::ensure(VkDeviceContext& ctx, uint32_t w, uint32_t h, VkFormat format)
{
    mCtx = &ctx;
    if (!w || !h || format == VK_FORMAT_UNDEFINED) return false;
    if (mW == w && mH == h && mFormat == format && ready()) return true;
    vkDeviceWaitIdle(ctx.device());   // the images may still be read by frames in flight
    destroyImages(ctx);
    if (!ensurePipeline(ctx, format))
    {
        std::fprintf(stderr, "[vkgfx] Quasar frame generation: pipeline creation failed; presenting normally\n");
        return false;
    }
    for (uint32_t i = 0; i < 2; ++i)
    {
        mHud[i] = ctx.createImage2D(w, h, format, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                    VK_IMAGE_ASPECT_COLOR_BIT);
        if (mHud[i].image) ctx.setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mHud[i].image, "quasarFgHudless");
    }
    mGen = ctx.createImage2D(w, h, format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT);
    if (mGen.image) ctx.setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mGen.image, "quasarFgGenerated");
    if (!mHud[0].image || !mHud[1].image || !mGen.image)
    {
        std::fprintf(stderr, "[vkgfx] Quasar frame generation: out of memory for its images; presenting normally\n");
        destroyImages(ctx);
        return false;
    }
    mW = w; mH = h; mFormat = format;
    mCur = 0;
    mHistory = false;
    return true;
}

void QLFrameGenVK::destroy(VkDeviceContext& ctx)
{
    VkDevice dev = ctx.device();
    if (!dev) return;
    destroyImages(ctx);
    if (mPipe) vkDestroyPipeline(dev, mPipe, nullptr);
    if (mLayout) vkDestroyPipelineLayout(dev, mLayout, nullptr);
    if (mDsl) vkDestroyDescriptorSetLayout(dev, mDsl, nullptr);
    if (mPool) vkDestroyDescriptorPool(dev, mPool, nullptr);
    if (mLinear) vkDestroySampler(dev, mLinear, nullptr);
    if (mNearest) vkDestroySampler(dev, mNearest, nullptr);
    mPipe = VK_NULL_HANDLE; mLayout = VK_NULL_HANDLE; mDsl = VK_NULL_HANDLE; mPool = VK_NULL_HANDLE;
    mLinear = mNearest = VK_NULL_HANDLE;
    std::memset(mSet, 0, sizeof(mSet));
    mPipeFormat = mFormat = VK_FORMAT_UNDEFINED;
}

void QLFrameGenVK::recordCaptureAndGenerate(VkCommandBuffer cmd, uint32_t slot, VkImage swapImage, const Inputs& in)
{
    if (!ready() || slot >= kMaxSlots) return;
    const uint32_t cur = mCur;
    const uint32_t prev = mHistory ? 1u - cur : cur;

    // 1. The world (HUD-less) into the history.
    imageBarrier2(cmd, swapImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    imageBarrier2(cmd, mHud[cur].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    copyWhole(cmd, swapImage, mHud[cur].image, mW, mH);
    imageBarrier2(cmd, swapImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    imageBarrier2(cmd, mHud[cur].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    // The in-between image: last frame's copy out of it (COPY) is done before it is drawn.
    imageBarrier2(cmd, mGen.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    // The motion vectors were written earlier in the frame (and left SHADER_READ_ONLY).
    {
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1;
        d.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &d);
    }

    // 2. The in-between frame.
    const QLFrameGenTuning& tun = qlFrameGenTuning();
    const bool motion = in.motionView != VK_NULL_HANDLE && in.motionW && in.motionH;
    VkDescriptorImageInfo ii[3]{};
    ii[0] = {mLinear, mHud[cur].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    ii[1] = {mLinear, mHud[prev].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    ii[2] = motion ? VkDescriptorImageInfo{mNearest, in.motionView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
                   : VkDescriptorImageInfo{mNearest, mHud[cur].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr[3]{};
    for (uint32_t i = 0; i < 3; ++i)
    {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = mSet[slot];
        wr[i].dstBinding = i;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(mCtx->device(), 3, wr, 0, nullptr);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = mGen.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {mW, mH}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp{0.f, 0.f, (float)mW, (float)mH, 0.f, 1.f};
    VkRect2D sc{{0, 0}, {mW, mH}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLayout, 0, 1, &mSet[slot], 0, nullptr);
    WarpPush p{};
    p.invTarget[0] = 1.f / (float)mW;
    p.invTarget[1] = 1.f / (float)mH;
    p.t = tun.showGenerated ? 0.5f : 1.f;   // 1: the current frame as it is
    p.motionValid = (motion && mHistory && tun.showGenerated) ? 1.f : 0.f;
    p.motionScale = tun.motionScale;
    p.flipY = tun.flipMotionY ? 1.f : 0.f;
    p.blendPrev = (tun.blendPrev && mHistory && tun.showGenerated) ? 1.f : 0.f;
    vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    imageBarrier2(cmd, mGen.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    mCur = 1u - cur;
    mHistory = true;
}

void QLFrameGenVK::recordToSwap(VkCommandBuffer cmd, VkImage genSwapImage)
{
    if (!ready()) return;
    // First scope = the acquire semaphore's wait stage (COPY) of this submission.
    imageBarrier2(cmd, genSwapImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    copyWhole(cmd, mGen.image, genSwapImage, mW, mH);
    imageBarrier2(cmd, genSwapImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}
