#include "vk_display_frame.h"
#include <limits>
#include <utility>

namespace {
constexpr VkImageUsageFlags displayUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
bool admitted(uint64_t owned,uint64_t others,uint64_t budget) {
    return owned && others<=budget && owned<=budget-others;
}
void transition(VkCommandBuffer cmd,VkImage image,VkImageLayout before,VkImageLayout after,
                VkPipelineStageFlags2 source,VkAccessFlags2 sourceAccess,
                VkPipelineStageFlags2 destination,VkAccessFlags2 destinationAccess) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask=source;barrier.srcAccessMask=sourceAccess;
    barrier.dstStageMask=destination;barrier.dstAccessMask=destinationAccess;
    barrier.oldLayout=before;barrier.newLayout=after;barrier.image=image;
    barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount=1;dependency.pImageMemoryBarriers=&barrier;
    vkCmdPipelineBarrier2(cmd,&dependency);
}
}
void VkDisplayFrame::destroy() {
    if (!mContext) return;
    if (mLinear.image) mContext->destroyImage(mLinear);
    if (mSdrCapture.image) mContext->destroyImage(mSdrCapture);
    mLinear={};mSdrCapture={};mContext=nullptr;
}
uint64_t VkDisplayFrame::residentBytes() const {
    if (!mContext) return 0;
    const uint64_t linearBytes=mContext->allocBytes(mLinear),captureBytes=mContext->allocBytes(mSdrCapture);
    return linearBytes>(std::numeric_limits<uint64_t>::max)()-captureBytes ?
        (std::numeric_limits<uint64_t>::max)() : linearBytes+captureBytes;
}
bool VkDisplayFrame::validForExtent(VkExtent2D extent) const {
    return mContext && mLinear.image && mLinear.view && extent.width && extent.height &&
        mLinear.extent.width==extent.width && mLinear.extent.height==extent.height;
}
void VkDisplayFrame::swap(VkDisplayFrame& other) noexcept {
    using std::swap;
    swap(mContext,other.mContext);swap(mLinear,other.mLinear);swap(mSdrCapture,other.mSdrCapture);
}
bool VkDisplayFrame::prepare(VkDeviceContext& context,bool enabled,VkExtent2D extent,
                             uint64_t others,uint64_t budget) {
    if (!enabled) { destroy();return true; }
    if (!extent.width || !extent.height || extent.width>context.limits().maxImageDimension2D ||
        extent.height>context.limits().maxImageDimension2D || !budget || budget>(512ull<<20)) {
        destroy();return false;
    }
    VkFormatProperties format{};
    vkGetPhysicalDeviceFormatProperties(context.physicalDevice(),VK_FORMAT_R16G16B16A16_SFLOAT,&format);
    constexpr VkFormatFeatureFlags needed=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((format.optimalTilingFeatures&needed)!=needed) { destroy();return false; }
    const uint64_t required=context.imageMemoryBytes(extent.width,extent.height,1,1,
        VK_FORMAT_R16G16B16A16_SFLOAT,displayUsage,false);
    if (!admitted(required,others,budget)) { destroy();return false; }
    if (mContext==&context && mLinear.image && mLinear.extent.width==extent.width &&
        mLinear.extent.height==extent.height && admitted(residentBytes(),others,budget)) return true;
    destroy();mContext=&context;
    VkMemScope category(VKMEM_TARGET);
    VkDeviceContext::SoftAllocScope soft(&context);
    mLinear=context.createImage2D(extent.width,extent.height,VK_FORMAT_R16G16B16A16_SFLOAT,
                                  displayUsage,VK_IMAGE_ASPECT_COLOR_BIT);
    if (!mLinear.image || !admitted(residentBytes(),others,budget)) { destroy();return false; }
    return true;
}
bool VkDisplayFrame::prepareSdrCapture(uint64_t others,uint64_t budget) {
    if (!mContext || !mLinear.image) return false;
    if (mSdrCapture.image) return admitted(residentBytes(),others,budget);
    const auto size=mLinear.extent;
    const uint64_t required=mContext->imageMemoryBytes(size.width,size.height,1,1,
        VK_FORMAT_B8G8R8A8_UNORM,displayUsage,false);
    const uint64_t live=residentBytes();
    if (others>budget || live>budget-others || !admitted(required,others+live,budget)) return false;
    VkMemScope category(VKMEM_TARGET);
    VkDeviceContext::SoftAllocScope soft(mContext);
    mSdrCapture=mContext->createImage2D(size.width,size.height,VK_FORMAT_B8G8R8A8_UNORM,
                                      displayUsage,VK_IMAGE_ASPECT_COLOR_BIT);
    if (!mSdrCapture.image) return false;
    if (!admitted(residentBytes(),others,budget)) { mContext->destroyImage(mSdrCapture);mSdrCapture={};return false; }
    return true;
}
void VkDisplayFrame::beginLinear(VkCommandBuffer cmd,const VkClearColorValue& clear) const {
    // UNDEFINED discards contents, but ALL_COMMANDS memory dependencies still order
    // the preceding frame's sampling/transfer reads before this same-queue overwrite.
    transition(cmd,mLinear.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdClearColorImage(cmd,mLinear.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&range);
    transition(cmd,mLinear.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}
void VkDisplayFrame::sampleLinear(VkCommandBuffer cmd) const {
    transition(cmd,mLinear.image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}
void VkDisplayFrame::resumeLinear(VkCommandBuffer cmd) const {
    transition(cmd,mLinear.image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}
void VkDisplayFrame::beginSdrCapture(VkCommandBuffer cmd) const {
    transition(cmd,mSdrCapture.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}
