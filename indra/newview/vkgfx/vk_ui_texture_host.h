// #703 UI texture index host: the bindless slots the RmlUi backend samples from.
// In the viewer the world renderer provides them (one index namespace shared with the
// world's descriptor-indexed textures, see LLCleanRendererVK::reserveUITexture); the
// offscreen harness (indra/rmluilab) provides its own. Fixed capacity and byte budget.
#pragma once

#include <vulkan/vulkan.h>
#include <cstddef>
#include <cstdint>

class VkUITextureHost
{
public:
    struct Lease
    {
        uint32_t slot = 0, generation = 0;
    };
    virtual ~VkUITextureHost() = default;
    virtual Lease                 reserve(VkImageView view, VkSampler sampler, uint64_t bytes) noexcept = 0;
    virtual bool                  pin(Lease lease, uint64_t serial) noexcept                           = 0;
    virtual bool                  release(Lease lease, uint64_t completedSerial) noexcept              = 0;
    virtual bool                  descriptor(Lease lease, VkDescriptorImageInfo& info) const noexcept   = 0;
    virtual VkDescriptorSetLayout layout() const noexcept                                              = 0;
    virtual uint32_t              capacity() const noexcept                                            = 0;
    virtual uint64_t              bytes() const noexcept                                               = 0;
    virtual uint64_t              budget() const noexcept                                              = 0;
};

class LLCleanRendererVK;
// The viewer's host: the world renderer's reserved UI slots (llrmluibindlessvk.cpp). One
// adapter per world renderer instance; valid while that renderer lives.
VkUITextureHost& slWorldUITextureHost(LLCleanRendererVK& world) noexcept;

// #703: the main window's UI batch stream (LLRenderVK) for the RmlUi layer's z slot:
// close the current batch and mark the layer before the next one / count the batches.
size_t slUIMarkExternalLayer() noexcept;
size_t slUIBatchCount() noexcept;
// #703: the main window's facade image for a viewer texture (LLTextureData), touching its
// cache entry like a bind. False: not uploaded yet / gone (the presenter falls back).
class LLTextureData;
bool slUIExternalImage(LLTextureData* data, VkImage& image, VkImageView& view, VkExtent2D& extent, uint32_t& texId) noexcept;
