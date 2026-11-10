// #703 external UI slots. No ownership of world textures or mutable world sets.
#include "linden_common.h"
#include "llcleanrenderervk.h"
#include "vk_ui_texture_host.h"
#include "llrendervk/llrendervk.h"
#include "llrendervkbackend.h"
#include "llrender.h"

#include <algorithm>
#include <new>

LLCleanRendererVK::UITextureLease LLCleanRendererVK::reserveUITexture(VkImageView view, VkSampler sampler, uint64_t bytes) noexcept
{
    if (!mReady || !view || !sampler || !bytes || bytes > kUITextureBudget || mUITextureBytes > kUITextureBudget - bytes ||
        mTextures.empty() || mVkTexKey.size() != mTextures.size())
        return {};
    UITextureReservation* entry = nullptr;
    for (auto& item : mUITextures)
        if (!item.lease.generation)
        {
            entry = &item;
            break;
        }
    if (!entry || mUITextureGeneration == UINT32_MAX)
        return {};
    uint32_t slot = 0;
    try
    {
        // Reserve every parallel array before changing any sizes or free list.
        // An empty GpuImage is intentional: every WORLD descriptor is white.
        if (mFreeTexSlots.empty())
        {
            const size_t count = mTextures.size() + 1;
            if (count > mMaxBindless)
                return {};
            mTextures.reserve(count);
            mVkTexKey.reserve(count);
            mVkTexShared.reserve(count);
            mVkTexGen.reserve(count);
            mVkTexW.reserve(count);
            mVkTexH.reserve(count);
            mTexLastUsed.reserve(count);
            mTexVer.reserve(count);
            mTexContentVer.reserve(count);
            // Returning any reservation must not allocate, including when the
            // world has since grown its own free list.
            mFreeTexSlots.reserve(mMaxBindless);
            slot = static_cast<uint32_t>(mTextures.size());
            mTextures.emplace_back();
            mVkTexKey.push_back(0);
            mVkTexShared.push_back(0);
            mVkTexGen.push_back(0);
            mVkTexW.push_back(0);
            mVkTexH.push_back(0);
            mTexLastUsed.push_back(0);
        }
        else
        {
            mTexVer.reserve(mTextures.size());
            mTexContentVer.reserve(mTextures.size());
            mFreeTexSlots.reserve(mMaxBindless);
            slot = mFreeTexSlots.back();
            mFreeTexSlots.pop_back();
        }
        touchTexSlot(slot);
    }
    catch (const std::bad_alloc&)
    {
        return {};
    }
    entry->lease   = { slot, ++mUITextureGeneration };
    entry->info    = { sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    entry->bytes   = bytes;
    entry->lastUse = 0;
    mUITextureBytes += bytes;
    return entry->lease;
}

bool LLCleanRendererVK::pinUITexture(UITextureLease lease, uint64_t serial) noexcept
{
    if (!serial || !lease.generation)
        return false;
    for (auto& item : mUITextures)
        if (item.lease.slot == lease.slot && item.lease.generation == lease.generation)
        {
            item.lastUse = (std::max)(item.lastUse, serial);
            return true;
        }
    return false;
}

bool LLCleanRendererVK::releaseUITexture(UITextureLease lease, uint64_t completedSerial) noexcept
{
    if (!lease.generation)
        return false;
    for (auto& item : mUITextures)
        if (item.lease.slot == lease.slot && item.lease.generation == lease.generation)
        {
            if (item.lastUse > completedSerial)
                return false;
            // The world has only ever named its own white image at this index.
            // Returning it cannot retire that white image or any world asset.
            mFreeTexSlots.push_back(lease.slot);
            mUITextureBytes -= item.bytes;
            item = {};
            return true;
        }
    return false;
}

bool LLCleanRendererVK::uiTextureDescriptor(UITextureLease lease, VkDescriptorImageInfo& info) const noexcept
{
    if (!lease.generation)
        return false;
    for (const auto& item : mUITextures)
        if (item.lease.slot == lease.slot && item.lease.generation == lease.generation)
        {
            info = item.info;
            return true;
        }
    return false;
}

namespace
{
class WorldUITextureHost final : public VkUITextureHost
{
public:
    LLCleanRendererVK* world = nullptr;
    Lease reserve(VkImageView view, VkSampler sampler, uint64_t bytes) noexcept override
    {
        const auto l = world->reserveUITexture(view, sampler, bytes);
        return { l.slot, l.generation };
    }
    bool pin(Lease l, uint64_t serial) noexcept override { return world->pinUITexture({ l.slot, l.generation }, serial); }
    bool release(Lease l, uint64_t completed) noexcept override { return world->releaseUITexture({ l.slot, l.generation }, completed); }
    bool descriptor(Lease l, VkDescriptorImageInfo& info) const noexcept override
    {
        return world->uiTextureDescriptor({ l.slot, l.generation }, info);
    }
    VkDescriptorSetLayout layout() const noexcept override { return world->uiTextureLayout(); }
    uint32_t              capacity() const noexcept override { return world->uiTextureCapacity(); }
    uint64_t              bytes() const noexcept override { return world->uiTextureBytes(); }
    uint64_t              budget() const noexcept override { return LLCleanRendererVK::uiTextureBudget(); }
};
} // namespace

VkUITextureHost& slWorldUITextureHost(LLCleanRendererVK& world) noexcept
{
    static WorldUITextureHost host;
    host.world = &world;
    return host;
}

size_t slUIMarkExternalLayer() noexcept
{
    return gVK.markExternalLayer();
}
size_t slUIBatchCount() noexcept
{
    return gVK.uiBatchCount();
}

bool slUIExternalImage(LLTextureData* data, VkImage& image, VkImageView& view, VkExtent2D& extent, uint32_t& texId) noexcept
{
    auto* backend = dynamic_cast<LLRenderVKBackend*>(LLRender::sBackend);
    if (!data || !backend)
        return false;
    texId = backend->uiTextureId(data);
    return texId && gVK.textureInfo(texId, image, view, extent);
}
