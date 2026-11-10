/**
 * @file llcleanwaterfxvk.cpp
 * @brief WATER INTERACTION (#1019): ripple height field + foam pattern (see llcleanwaterfxvk.h).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
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
#include "linden_common.h"

#include "llcleanwaterfxvk.h"
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include "llerror.h"
#include "llviewercontrol.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace
{
// Ripple physics (both tiers: 32 m window).
constexpr float kRippleSpeed = 1.1f;      // m/s: the rings' spread (gravity-capillary ripples ~0.3-1.5 m/s)
constexpr float kRippleDecay = 2.2f;      // s: amplitude time constant
constexpr float kCfl = 0.5f;              // c dt / dx per sub-step (stable below 1 / sqrt 2)
constexpr uint32_t kMaxSubSteps = 4;      // past this the ripples run slow (< ~9 fps on HIGH)
constexpr float kSpongeCells = 14.f;      // the absorbing strip along the window edge
constexpr float kSpongeEdge = 0.86f;      // its damping per step at the very edge
constexpr float kRestPull = 0.25f;        // per s: the weak pull of the level back to rest
constexpr double kFreeGrace = 30.0;       // s off before the maps are freed
constexpr double kRetrySecs = 10.0;       // allocation failure: retry after
constexpr uint32_t kMaxRetries = 3;       // ... then stay without the failed part
constexpr uint64_t kSmallVram = 4608ull << 20;   // the weather's small-card line (4.5 GB)
constexpr uint32_t kFoamSize = 256;

VkShaderModule fxModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    vkNoteShaderModule(m, code, bytes, name);
    return m;
}

void memBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = srcStage; mb.srcAccessMask = srcAccess;
    mb.dstStageMask = dstStage; mb.dstAccessMask = dstAccess;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &d);
}

// ---- the foam pattern (CPU, once) ----
uint32_t hashU(uint32_t v)
{
    uint32_t s = v * 747796405u + 2891336453u;
    const uint32_t w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}
float hash01(int x, int y, uint32_t salt)
{
    return (float)(hashU((uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ salt) & 0xFFFFFFu) * (1.f / 16777216.f);
}

// Tileable Worley on a g x g cell grid at uv in [0, 1): F2 - F1 (0 on the cell borders).
float worleyEdge(float u, float v, int g, uint32_t salt)
{
    const float px = u * (float)g, py = v * (float)g;
    const int cx = (int)std::floor(px), cy = (int)std::floor(py);
    float f1 = 1e9f, f2 = 1e9f;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
        {
            const int x = cx + dx, y = cy + dy;
            const int wx = ((x % g) + g) % g, wy = ((y % g) + g) % g;
            const float fx = (float)x + 0.1f + 0.8f * hash01(wx, wy, salt);
            const float fy = (float)y + 0.1f + 0.8f * hash01(wx, wy, salt ^ 0x9E3779B9u);
            const float d = std::sqrt((fx - px) * (fx - px) + (fy - py) * (fy - py));
            if (d < f1) { f2 = f1; f1 = d; }
            else if (d < f2) f2 = d;
        }
    return f2 - f1;
}

// Tileable value noise on a g x g lattice (smoothstep interpolation), 0..1.
float valueNoise(float u, float v, int g, uint32_t salt)
{
    const float px = u * (float)g, py = v * (float)g;
    const int x0 = (int)std::floor(px), y0 = (int)std::floor(py);
    const float fx = px - (float)x0, fy = py - (float)y0;
    const float sx = fx * fx * (3.f - 2.f * fx), sy = fy * fy * (3.f - 2.f * fy);
    auto at = [&](int x, int y) { return hash01(((x % g) + g) % g, ((y % g) + g) % g, salt); };
    const float a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}

float smooth01(float e0, float e1, float x)
{
    const float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// RG8, level 0 then every mip down to 1 x 1 (box filtered), concatenated.
std::vector<uint8_t> makeFoamPattern(uint32_t& levels)
{
    const uint32_t n = kFoamSize;
    std::vector<float> r(n * n), g(n * n);
    for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x)
        {
            const float u = ((float)x + 0.5f) / (float)n, v = ((float)y + 0.5f) / (float)n;
            // G: soft fBm (4 octaves), stretched to use the range.
            float fbm = 0.f, amp = 0.5f, sum = 0.f;
            for (int o = 0; o < 4; ++o)
            {
                fbm += amp * valueNoise(u, v, 4 << o, 0x51u + (uint32_t)o);
                sum += amp; amp *= 0.5f;
            }
            fbm = std::clamp((fbm / sum - 0.5f) * 1.8f + 0.5f, 0.f, 1.f);
            // R: lace -- bright foam strands on the cell borders of two cell sizes, with
            // fine bubbles, thinned where the soft noise is low.
            const float lace1 = 1.f - smooth01(0.f, 0.22f, worleyEdge(u, v, 9, 0xA1u));
            const float lace2 = 1.f - smooth01(0.f, 0.16f, worleyEdge(u, v, 21, 0xB7u));
            const float bub = 1.f - smooth01(0.f, 0.3f, worleyEdge(u, v, 47, 0xC3u));
            float lace = std::max(lace1 * 0.9f, lace2 * 0.75f);
            lace = std::max(lace, bub * 0.45f);
            lace = lace * (0.55f + 0.45f * fbm) + 0.25f * fbm;
            r[y * n + x] = std::clamp(lace, 0.f, 1.f);
            g[y * n + x] = fbm;
        }
    std::vector<uint8_t> out;
    levels = 0;
    for (uint32_t s = n; ; s >>= 1)
    {
        ++levels;
        for (uint32_t i = 0; i < s * s; ++i)
        {
            out.push_back((uint8_t)std::lround(r[i] * 255.f));
            out.push_back((uint8_t)std::lround(g[i] * 255.f));
        }
        if (s == 1) break;
        const uint32_t h = s >> 1;
        std::vector<float> r2(h * h), g2(h * h);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < h; ++x)
            {
                const uint32_t a = (2 * y) * s + 2 * x, b = a + s;
                r2[y * h + x] = 0.25f * (r[a] + r[a + 1] + r[b] + r[b + 1]);
                g2[y * h + x] = 0.25f * (g[a] + g[a + 1] + g[b] + g[b + 1]);
            }
        r.swap(r2); g.swap(g2);
    }
    return out;
}
} // namespace

uint32_t LLCleanWaterFxVK::wantTier(const Frame& f) const
{
    static LLCachedControl<U32> s_quality(gSavedSettings, "RenderVKWaterInteraction", 2);
    uint32_t t = std::min((uint32_t)s_quality, (uint32_t)TIER_HIGH);
    // (the weather's small-card line: a < 4.5 GB card runs at most MEDIUM)
    if (f.vramCapacity > 0 && f.vramCapacity < kSmallVram) t = std::min(t, (uint32_t)TIER_MEDIUM);
    // A part that failed to allocate kMaxRetries times stays off (the foam stays at LOW).
    if (mFails >= kMaxRetries) t = std::min(t, (uint32_t)TIER_LOW);
    return t;
}

bool LLCleanWaterFxVK::ensurePipeline(VkDeviceContext* ctx)
{
    if (mPipe) return true;
    VkDevice dev = ctx->device();
    if (!mSampler)
    {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.maxLod = VK_LOD_CLAMP_NONE;
        VK_CHECK(vkCreateSampler(dev, &sci, nullptr, &mSampler));
    }
    if (!mDsl)
    {
        VkDescriptorSetLayoutBinding b[2]{};
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 2; lci.pBindings = b;
        VK_CHECK(vkdCreateSetLayout(dev, &lci, &mDsl, "mDslWaterFx"));
        VK_CHECK(mPool.create(dev, "waterfx", {{mDsl, kSlots}}));
        bool ok = true;
        for (uint32_t s = 0; s < kSlots && ok; ++s)
            VKD_ALLOC_SOFT(mPool.allocate(mDsl, &mSet[s]), { ok = false; });
        if (!ok) return false;
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1; pli.pSetLayouts = &mDsl;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
        VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mLayout, "mWaterFxLayout"));
    }
    VkShaderModule cs = fxModule(dev, kWorldWaterRippleSpv, kWorldWaterRippleSpv_bytes, "kWorldWaterRippleSpv");
    mPipe = createComputePipeline(dev, cs, mLayout);
    vkDestroyShaderModule(dev, cs, nullptr);
    if (!mPipe)
    {
        LL_WARNS("VKWaterFx") << "ripple pipeline failed to build; ripples off" << LL_ENDL;
        return false;
    }
    return true;
}

bool LLCleanWaterFxVK::ensureFoam(VkDeviceContext* ctx)
{
    if (mFoam.image) return true;
    uint32_t levels = 0;
    const std::vector<uint8_t> data = makeFoamPattern(levels);
    GpuBuffer staging;
    {
        VkMemScope memcat(VKMEM_TARGET);
        VkDeviceContext::SoftAllocScope soft(ctx);
        mFoam = ctx->createImage2D(kFoamSize, kFoamSize, VK_FORMAT_R8G8_UNORM,
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT, levels);
        if (mFoam.image)
            staging = ctx->createBuffer(data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    if (!mFoam.image || !staging.mapped)
    {
        if (staging.buffer) ctx->destroyBuffer(staging);
        if (mFoam.image) ctx->destroyImage(mFoam);
        mFoam = GpuImage{};
        return false;
    }
    std::memcpy(staging.mapped, data.data(), data.size());
    ctx->submitImmediate([&](VkCommandBuffer cmd)
    {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.image = mFoam.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &d);
        std::vector<VkBufferImageCopy> bc(levels);
        VkDeviceSize off = 0;
        for (uint32_t l = 0; l < levels; ++l)
        {
            const uint32_t s = std::max(kFoamSize >> l, 1u);
            bc[l] = VkBufferImageCopy{};
            bc[l].bufferOffset = off;
            bc[l].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
            bc[l].imageExtent = {s, s, 1};
            off += (VkDeviceSize)s * s * 2;
        }
        vkCmdCopyBufferToImage(cmd, staging.buffer, mFoam.image, VK_IMAGE_LAYOUT_GENERAL, levels, bc.data());
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier2(cmd, &d);
    });
    ctx->destroyBuffer(staging);
    ctx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mFoam.image, "waterFoamPattern");
    return true;
}

bool LLCleanWaterFxVK::ensureField(VkDeviceContext* ctx, uint32_t n)
{
    if (mField.image && mN == n) return true;
    freeField();
    {
        VkMemScope memcat(VKMEM_TARGET);
        VkDeviceContext::SoftAllocScope soft(ctx);
        mField = ctx->createImageLayers(n, n, 2, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                        VK_IMAGE_ASPECT_COLOR_BIT, false, VK_IMAGE_VIEW_TYPE_2D_ARRAY);
        if (mField.image && !mImpulses.buffer)
            mImpulses = ctx->createBuffer((VkDeviceSize)kSlots * kMaxImpulses * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    if (!mField.image || !mImpulses.mapped)
    {
        freeField();
        return false;
    }
    ctx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mField.image, "waterRippleField");
    // Starts UNDEFINED: record() moves it to GENERAL and clears it before the first step, and
    // the frame info names it only after that ran (mFieldValid).
    mN = n;
    mFieldValid = false;
    mClear = true;
    writeSets();
    return true;
}

void LLCleanWaterFxVK::writeSets()
{
    if (!mCtx || !mField.view || !mImpulses.buffer) return;
    for (uint32_t s = 0; s < kSlots; ++s)
    {
        if (!mSet[s]) continue;
        const VkDescriptorImageInfo ii{VK_NULL_HANDLE, mField.view, VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorBufferInfo bi{mImpulses.buffer, (VkDeviceSize)s * kMaxImpulses * 16, (VkDeviceSize)kMaxImpulses * 16};
        VkWriteDescriptorSet w[2]{};
        for (uint32_t i = 0; i < 2; ++i)
        {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = mSet[s];
            w[i].dstBinding = i; w[i].descriptorCount = 1;
        }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[0].pImageInfo = &ii;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &bi;
        vkUpdateDescriptorSets(mCtx->device(), 2, w, 0, nullptr);
    }
}

void LLCleanWaterFxVK::freeField()
{
    if (mCtx)
    {
        if (mField.image) mCtx->destroyImage(mField);
        if (mImpulses.buffer) mCtx->destroyBuffer(mImpulses);
    }
    mField = GpuImage{};
    mImpulses = GpuBuffer{};
    mN = 0;
    mFieldValid = false;
}

void LLCleanWaterFxVK::freeAll(VkDevice dev)
{
    freeField();
    if (mCtx && mFoam.image) mCtx->destroyImage(mFoam);
    mFoam = GpuImage{};
    if (dev)
    {
        if (mPipe) vkDestroyPipeline(dev, mPipe, nullptr);
        if (mLayout) vkDestroyPipelineLayout(dev, mLayout, nullptr);
        mPool.destroy();
        if (mDsl) vkdDestroySetLayout(dev, mDsl);
        if (mSampler) vkDestroySampler(dev, mSampler, nullptr);
    }
    mPipe = VK_NULL_HANDLE; mLayout = VK_NULL_HANDLE; mDsl = VK_NULL_HANDLE; mSampler = VK_NULL_HANDLE;
    for (VkDescriptorSet& s : mSet) s = VK_NULL_HANDLE;
}

bool LLCleanWaterFxVK::prepare(VkCommandBuffer /*cmd*/, const Frame& f)
{
    mCtx = f.ctx;
    mSteps = 0;
    mLive = false;
    std::memset(mInfo, 0, sizeof(mInfo));
    if (!f.ctx) return false;
    VkDevice dev = f.ctx->device();

    const uint32_t want = wantTier(f);
    if (want == TIER_OFF)
    {
        // Free on liveness: the maps go after a grace period off (a quick toggle keeps them).
        if (mOffSince < 0.0) mOffSince = f.now;
        if (!idle() && f.now - mOffSince > kFreeGrace)
        {
            vkDeviceWaitIdle(dev);
            freeAll(dev);
            mBindingDirty = true;
            LL_INFOS("VKWaterFx") << "water interaction off for " << kFreeGrace << " s: ripple field and foam pattern freed" << LL_ENDL;
        }
        mTier = TIER_OFF;
        return false;
    }
    mOffSince = -1.0;

    // Lifetime: (re)create behind a device wait (nothing in this recording has bound the
    // water set yet; frames in flight may still read the old maps).
    const uint32_t n = want >= TIER_HIGH ? 512u : want >= TIER_MEDIUM ? 256u : 0u;
    const bool needFoam = !mFoam.image;
    const bool needField = n != mN;
    // (past kMaxRetries a refused foam pattern is not asked for again: the module stays off)
    if ((needFoam || needField) && f.now >= mRetryAt && !(needFoam && mFails >= kMaxRetries))
    {
        vkDeviceWaitIdle(dev);
        bool ok = true;
        if (needFoam && !ensureFoam(f.ctx)) ok = false;
        if (n == 0) { if (mField.image) freeField(); }
        else if (needField && !(ensurePipeline(f.ctx) && ensureField(f.ctx, n))) ok = false;
        mBindingDirty = true;
        if (!ok)
        {
            ++mFails;
            mRetryAt = f.now + kRetrySecs;
            LL_WARNS("VKWaterFx") << "water interaction: allocation refused (attempt " << mFails << " of " << kMaxRetries
                                  << ")" << (mFails >= kMaxRetries ? "; ripples stay off (foam band only)" : "; retried")
                                  << LL_ENDL;
        }
        else
        {
            mFails = 0;
            LL_INFOS("VKWaterFx") << "water interaction tier " << want << ": foam pattern " << kFoamSize << "^2"
                                  << (n ? ", ripple field " : "") << (n ? std::to_string(n) + "^2" : std::string()) << LL_ENDL;
        }
    }
    if (!mFoam.image) return false;   // (nothing to sample: the FS adds nothing)
    mTier = want;
    mLive = true;
    mTexel = mN ? 32.f / (float)mN : 0.f;

    // ---- the ripple step plan ----
    const bool field = mField.image && mPipe && mN && mTier >= TIER_MEDIUM;
    if (field)
    {
        const int32_t half = (int32_t)mN / 2;
        const int32_t ox = (int32_t)std::floor(f.camPos[0] / mTexel) - half;
        const int32_t oy = (int32_t)std::floor(f.camPos[1] / mTexel) - half;
        // A jump of more than half the window (teleport, region crossing re-base, first use)
        // starts over at rest instead of sliding.
        // (a snapshot's camera never moves the live field: it reads it as it is)
        if (!f.offscreen && (!mFieldValid || std::abs(ox - mOrigin[0]) > half || std::abs(oy - mOrigin[1]) > half))
            mClear = true;
        if (!f.offscreen)
        {
            mPrevOrigin[0] = mClear ? ox : mOrigin[0];
            mPrevOrigin[1] = mClear ? oy : mOrigin[1];
            mOrigin[0] = ox; mOrigin[1] = oy;
            const float dtMax = kCfl * mTexel / kRippleSpeed;
            const float dt = std::clamp(f.dt, 0.f, dtMax * (float)kMaxSubSteps);
            mSteps = dt > 0.f ? std::min(kMaxSubSteps, (uint32_t)std::ceil(dt / dtMax - 1e-4f)) : 0u;
            mSteps = std::max(mSteps, dt > 0.f ? 1u : 0u);
            mSubDt = mSteps ? dt / (float)mSteps : 0.f;
            // Impulses (this frame slot's part of the ring; read by the first sub-step).
            mSlot = f.frameSlot % kSlots;
            mImpulseCount = 0;
            if (mImpulses.mapped && f.impulses && mSteps)
            {
                float* dst = (float*)mImpulses.mapped + (size_t)mSlot * kMaxImpulses * 4;
                const float lim = (float)half * mTexel;
                for (uint32_t i = 0; i < f.impulseCount && mImpulseCount < kMaxImpulses; ++i)
                {
                    const float* q = f.impulses[i];
                    if (std::fabs(q[0] - f.camPos[0]) > lim || std::fabs(q[1] - f.camPos[1]) > lim) continue;
                    std::memcpy(dst + mImpulseCount * 4, q, 16);
                    ++mImpulseCount;
                }
            }
        }
        if (mFieldValid || mClear)
        {
            const uint32_t layerAfter = (mLayer + mSteps) & 1u;   // (a clear keeps the layer)
            mInfo[0][3] = (float)layerAfter;
            mInfo[1][0] = 1.f / ((float)mN * mTexel);
            mInfo[1][1] = ((float)mOrigin[0] + (float)half) * mTexel;
            mInfo[1][2] = ((float)mOrigin[1] + (float)half) * mTexel;
            mInfo[1][3] = (float)half * mTexel;
        }
    }

    static LLCachedControl<F32> s_foam(gSavedSettings, "RenderVKWaterFoam", 1.f);
    static LLCachedControl<U32> s_dbg(gSavedSettings, "RenderVKWaterFXDebug", 0);
    uint32_t flags = 0;
    const float foam = std::clamp((float)s_foam, 0.f, 2.f);
    if (foam > 0.f) flags |= 2u;
    if (mTier >= TIER_MEDIUM) flags |= 4u;
    // (the field is named only once a recording has cleared it: never an UNDEFINED read)
    if (field && mFieldValid) flags |= 1u;
    if ((U32)s_dbg == 1u) flags |= 8u;
    if ((U32)s_dbg == 2u) flags |= 16u;
    mInfo[0][0] = (float)flags;
    mInfo[0][1] = foam;
    mInfo[0][2] = mTier >= TIER_HIGH ? 1.1f : 1.f;
    return true;
}

void LLCleanWaterFxVK::record(VkCommandBuffer cmd)
{
    if (!mLive || !mPipe || !mField.image || !mN || (!mClear && mSteps == 0)) return;
    const uint32_t slot = mSlot;
    if (!mSet[slot]) return;
    const uint32_t groups = (mN + 7u) / 8u;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout, 0, 1, &mSet[slot], 0, nullptr);
    Push p{};
    p.ctl[0] = mN;
    p.step[3] = mTexel;
    p.sponge[0] = kSpongeCells; p.sponge[1] = kSpongeEdge;
    p.sponge[2] = std::min(kRestPull * mSubDt, 0.05f);
    if (mClear)
    {
        // UNDEFINED (a new field) or GENERAL -> the clear's writes.
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        b.oldLayout = mFieldValid ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.image = mField.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &d);
        p.win[0] = p.win[2] = mOrigin[0]; p.win[1] = p.win[3] = mOrigin[1];
        p.ctl[1] = mLayer; p.ctl[3] = 1u;
        VK_PUSH_BUDGET_CHECK(p);
        vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cmd, groups, groups, 1);
        mClear = false;
        mFieldValid = true;
        mPrevOrigin[0] = mOrigin[0]; mPrevOrigin[1] = mOrigin[1];
        mPrevDt = mSubDt > 0.f ? mSubDt : mPrevDt;
        memBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    else
    {
        // Last frame's sampled reads (pass 2b) -> this frame's writes.
        memBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }
    const float decay = std::exp(-mSubDt / kRippleDecay);
    for (uint32_t s = 0; s < mSteps; ++s)
    {
        const int32_t* po = s == 0 ? mPrevOrigin : mOrigin;
        p.win[0] = mOrigin[0]; p.win[1] = mOrigin[1]; p.win[2] = po[0]; p.win[3] = po[1];
        p.ctl[1] = mLayer;
        p.ctl[2] = s == 0 ? mImpulseCount : 0u;
        p.ctl[3] = 0u;
        const float dtPrev = std::max(mPrevDt, 1e-4f);
        const float c = kRippleSpeed / mTexel;
        p.step[0] = std::min(c * c * mSubDt * 0.5f * (mSubDt + dtPrev), 0.49f);
        p.step[1] = std::clamp(mSubDt / dtPrev, 0.25f, 4.f);
        p.step[2] = decay;
        VK_PUSH_BUDGET_CHECK(p);
        vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cmd, groups, groups, 1);
        mLayer ^= 1u;
        mPrevDt = mSubDt;
        memBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    mPrevOrigin[0] = mOrigin[0]; mPrevOrigin[1] = mOrigin[1];
    mSteps = 0;
}

void LLCleanWaterFxVK::fillFrameInfo(float out[2][4]) const
{
    static_assert(sizeof(mInfo) == sizeof(float) * 8, "ssr.water_fx[2]");
    if (!mLive) { std::memset(out, 0, sizeof(mInfo)); return; }
    std::memcpy(out, mInfo, sizeof(mInfo));
}

void LLCleanWaterFxVK::destroy(VkDevice dev)
{
    if (!dev) return;
    vkDeviceWaitIdle(dev);
    freeAll(dev);
    mLive = false;
}
