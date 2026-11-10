// One optional live float display surface, reused in graphics submission order.
// No duplicate UI layers: world, depth overlays, HUD and main UI share this image.
#pragma once
#include "vk_device.h"

class VkDisplayFrame {
public:
    VkDisplayFrame() = default;
    ~VkDisplayFrame() { destroy(); }
    VkDisplayFrame(const VkDisplayFrame&) = delete;
    VkDisplayFrame& operator=(const VkDisplayFrame&) = delete;
    // Caller drained prior graphics/compute uses (existing swapchain recreation
    // boundary). callerOwnedHdrBytes includes EVERY other extra HDR image/copy,
    // including renderer AA/FX surfaces and not-yet-retired allocations.
    bool prepare(VkDeviceContext& context,bool enabled,VkExtent2D extent,
                 uint64_t callerOwnedHdrBytes,uint64_t declaredBudget);
    bool validForExtent(VkExtent2D extent) const;
    void swap(VkDisplayFrame& other) noexcept;
    // Snapshot-only SDR scratch. Allocation refusal leaves live HDR intact; caller
    // falls back to the independent SDR offscreen snapshot route.
    bool prepareSdrCapture(uint64_t callerOwnedHdrBytes,uint64_t declaredBudget);
    void destroy(); // prior uses retired, context still alive
    uint64_t residentBytes() const;
    const GpuImage& linear() const { return mLinear; }
    const GpuImage& sdrCapture() const { return mSdrCapture; }
    void beginLinear(VkCommandBuffer cmd,const VkClearColorValue& clear) const;
    void sampleLinear(VkCommandBuffer cmd) const;
    void resumeLinear(VkCommandBuffer cmd) const;
    void beginSdrCapture(VkCommandBuffer cmd) const;
private:
    VkDeviceContext* mContext = nullptr;
    GpuImage mLinear{},mSdrCapture{};
};
