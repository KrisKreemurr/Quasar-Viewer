// Quasar frame generation (QuasarFrameGen).
//
// Every real frame gets one generated frame shown halfway between it and the frame before.
// The main frame's command buffer, right after the world (before the UI):
//   recordCaptureAndGenerate(): copies the swapchain image (world + HUD attachments + name
//   tags, no 2D UI) into the HUD-less history, then renders the in-between image from the
//   previous and the current HUD-less frames and the renderer's motion vectors.
// A second, small submission after it (it waits the generated swapchain image's acquire):
//   recordToSwap(): copies the in-between image into the generated swapchain image, which
//   then gets the frame's 2D UI drawn over it like the real one.
// VkSwapchain's pacing thread presents the generated image, then the real one half a frame
// later.
//
// Simple mode (this file): the in-between image is the current and previous frames warped
// halfway along the motion vectors and blended where they agree (shaders_quasar/
// ql_fg_warp.frag). The FSR 3 frame interpolation replaces it later behind the same calls.
#pragma once

#include "vk_device.h"

#include <cstdint>

#include "qlframegen_settings.h"

class QLFrameGenVK
{
public:
    struct Inputs
    {
        VkImageView motionView = VK_NULL_HANDLE;   // render size, SHADER_READ_ONLY_OPTIMAL; null = no motion
        uint32_t    motionW = 0, motionH = 0;
    };

    QLFrameGenVK() = default;
    QLFrameGenVK(const QLFrameGenVK&) = delete;
    QLFrameGenVK& operator=(const QLFrameGenVK&) = delete;

    // (Re)creates the images and the pipeline for this display size and swapchain format.
    // Waits the device idle when something has to be re-created. false = frame generation
    // can't run (this frame presents normally).
    bool ensure(VkDeviceContext& ctx, uint32_t w, uint32_t h, VkFormat format);
    void destroy(VkDeviceContext& ctx);
    // Drops the history (a teleport, a resize): the next generated frame is a copy.
    void resetHistory() { mHistory = false; }

    // Main frame command buffer. swapImage: COLOR_ATTACHMENT_OPTIMAL, the world written
    // (and left so). slot: the frame-in-flight slot (its previous use has retired).
    void recordCaptureAndGenerate(VkCommandBuffer cmd, uint32_t slot, VkImage swapImage, const Inputs& in);
    // The generated swapchain image's command buffer: genSwapImage from UNDEFINED to
    // COLOR_ATTACHMENT_OPTIMAL holding the in-between frame (the UI then draws over it).
    void recordToSwap(VkCommandBuffer cmd, VkImage genSwapImage);

    bool ready() const { return mPipe != VK_NULL_HANDLE && mGen.image != VK_NULL_HANDLE; }

private:
    static constexpr uint32_t kMaxSlots = 4;
    bool ensurePipeline(VkDeviceContext& ctx, VkFormat format);
    void destroyImages(VkDeviceContext& ctx);

    VkDeviceContext* mCtx = nullptr;
    uint32_t mW = 0, mH = 0;
    VkFormat mFormat = VK_FORMAT_UNDEFINED;     // the images' (= the swapchain's) format
    VkFormat mPipeFormat = VK_FORMAT_UNDEFINED; // the pipeline's colour attachment format
    GpuImage mHud[2];                           // HUD-less history (display size)
    GpuImage mGen;                              // the in-between frame
    uint32_t mCur = 0;                          // mHud index the next capture writes
    bool     mHistory = false;                  // mHud[1 - mCur] holds the previous frame

    VkSampler mLinear = VK_NULL_HANDLE, mNearest = VK_NULL_HANDLE;
    VkDescriptorSetLayout mDsl = VK_NULL_HANDLE;
    VkPipelineLayout mLayout = VK_NULL_HANDLE;
    VkPipeline mPipe = VK_NULL_HANDLE;
    VkDescriptorPool mPool = VK_NULL_HANDLE;
    VkDescriptorSet mSet[kMaxSlots] = {};
};
