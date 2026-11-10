// Optional current-renderer final display transfer. No ownership of world/UI images.
#pragma once
#include "vk_display_hdr.h"
#include "vk_device.h"
#include <array>

class VkDisplayEncode {
public:
    enum class Content : uint8_t { CompleteDisplay, WorldBeforeUi };
    VkDisplayEncode() = default;
    VkDisplayEncode(const VkDisplayEncode&) = delete;
    VkDisplayEncode& operator=(const VkDisplayEncode&) = delete;
    // All prior uses retired before prepare/destroy. Never called per frame.
    // Disabled preparation creates no Vulkan objects. Shader binaries are supplied
    // by the current renderer's existing embedded-shader build, not render2.
    bool prepare(VkDeviceContext& ctx, bool enabled, VkSurfaceFormatKHR target,
                 std::span<const uint32_t> vertex, std::span<const uint32_t> fragment);
    bool validForTarget(VkSurfaceFormatKHR target) const;
    void swap(VkDisplayEncode& other) noexcept;
    void destroy();
    // Caller has retired THIS descriptor slot (same frame-slot fence as UI replay).
    // Input is opaque Rec.709 LINEAR NITS in SHADER_READ_ONLY, not SDR/PQ bytes.
    // SDR output requires WorldBeforeUi: tone-map the world, then replay UI/HUD
    // into SDR. A composed HDR UI frame must not be tone-mapped as scene colour.
    // Caller owns barriers; all image bytes, including fenced copies and other HDR
    // intermediates, must fit declaredBudget. False records no commands.
    bool record(VkCommandBuffer cmd, uint32_t slot, VkImageView linearNits,
                VkImageView target, VkExtent2D extent, vkdisplay::Request request, Content content,
                uint64_t allHdrImageBytes, uint64_t declaredBudget, bool dither = true);
private:
    VkDeviceContext* mContext = nullptr;
    VkPipeline mPipeline = VK_NULL_HANDLE;
    VkPipelineLayout mLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool mPool = VK_NULL_HANDLE;
    VkSampler mSampler = VK_NULL_HANDLE;
    std::array<VkDescriptorSet,3> mSets{};
    std::array<VkImageView,3> mInputs{};
    VkSurfaceFormatKHR mTarget{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    vkdisplay::Encoding mEncoding = vkdisplay::Encoding::Sdr;
};
