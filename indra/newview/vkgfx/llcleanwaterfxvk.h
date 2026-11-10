/**
 * @file llcleanwaterfxvk.h
 * @brief WATER INTERACTION (#1019) for the Vulkan clean renderer: the ripple height field and
 *        the foam pattern the water surface FS samples (clean_water_fx.glsl).
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
 *
 * WHAT IT OWNS (all fixed size; nothing grows):
 *   ripple field  N x N x 2 layers RGBA32F (h', h, dh/dx, dh/dy), ping-ponged: the damped wave
 *                 equation around the camera, world-anchored toroidal (clean_water_ripple.comp).
 *                 Tier MEDIUM 256^2 at 12.5 cm (2 MB), HIGH 512^2 at 6.25 cm (8 MB); both 32 m.
 *   foam pattern  256^2 RG8 + mips (~90 KB), generated once on the CPU: R a lacy cellular foam
 *                 pattern, G a soft tileable noise; repeat-sampled.
 *   impulses      kMaxImpulses contacts per frame per frame slot (host-visible, 3 KB).
 * Bound at the water layout's set 2 bindings 3 / 4 (LLCleanRendererVK mDslWaterDepth,
 * PARTIALLY_BOUND: the FS reads them only while ssr.water_fx says they are live).
 *
 * FLOW, per live recording:
 *   prepare()        the target-ensure step (before anything binds draw set 0 / the water set):
 *                    the quality tier (RenderVKWaterInteraction + the VRAM tier), lifetime
 *                    (create on demand behind a device wait -> bindingDirty; free after kFreeGrace
 *                    seconds off), the window, the sub-steps (CFL <= 0.5) and the impulses.
 *   record()         compute, beside the weather simulation: a clear when the window jumped
 *                    (teleport / region crossing / first use), then the sub-steps, then a barrier
 *                    to the water FS's sampled reads in pass 2b.
 *   fillFrameInfo()  ssr.water_fx (clean_frame_info.glsl): flags, ripple frame. (The sea state
 *                    -- wind, storm, surf -- is ssr.sea: slseastate.h, clean_sea_state.glsl.)
 * Offscreen (snapshot) recordings never step the field: they read it as the live view left it.
 * Stereo: the field is world space and view independent -- one simulation for every view.
 *
 * ALLOCATION FAILURE (soft): logged, retried after kRetrySecs, dropped (module stays at the
 * foam-only tier) after kMaxRetries; never a crash.
 */
#ifndef LL_LLCLEANWATERFXVK_H
#define LL_LLCLEANWATERFXVK_H

#include "vk_device.h"
#include "vk_descpool.h"

#include <cstdint>

class LLCleanWaterFxVK
{
public:
    // Quality tiers (RenderVKWaterInteraction): 0 off, 1 LOW (foam band only: one breakup
    // pattern, no ripples / surf lines), 2 MEDIUM (+ ripples 256^2, surf lines, whitecaps),
    // 3 HIGH (ripples 512^2).
    enum ETier : uint32_t { TIER_OFF = 0, TIER_LOW = 1, TIER_MEDIUM = 2, TIER_HIGH = 3 };
    static constexpr uint32_t kMaxImpulses = 64;
    static constexpr uint32_t kSlots = 3;   // = LLCleanRendererVK::kTexDescSets (frame slots)

    struct Frame
    {
        VkDeviceContext* ctx = nullptr;
        uint32_t frameSlot = 0;
        double   now = 0.0;                 // s (frame clock)
        float    dt = 0.f;                  // s this frame (0: frozen world / no step)
        float    camPos[3] = {};            // main eye (world = agent frame)
        bool     offscreen = false;         // snapshot recording: no step
        uint64_t vramCapacity = 0;          // device budget (bytes, 0 unknown): the small-card cap
        // This frame's contacts (SLWaterInteraction): xy world, z radius (m), w amplitude (m).
        const float (*impulses)[4] = nullptr;
        uint32_t impulseCount = 0;
    };

    LLCleanWaterFxVK() = default;
    ~LLCleanWaterFxVK() = default;   // destroy() is explicit (needs the device)
    LLCleanWaterFxVK(const LLCleanWaterFxVK&) = delete;
    LLCleanWaterFxVK& operator=(const LLCleanWaterFxVK&) = delete;

    // The ensure step. True = something to draw this frame (foam and / or ripples).
    bool prepare(VkCommandBuffer cmd, const Frame& f);
    // Compute: the ripple steps (no-op without the field or with no step due).
    void record(VkCommandBuffer cmd);
    // ssr.water_fx (zeros while not live).
    void fillFrameInfo(float out[2][4]) const;
    // Set 2 bindings 3 / 4 changed (re-created / freed) since the last call: the renderer
    // rewrites them (the device is idle then: prepare() waited).
    bool takeBindingDirty() { const bool d = mBindingDirty; mBindingDirty = false; return d; }
    // The descriptors (view null: not allocated -- leave the binding as it is, unread).
    VkDescriptorImageInfo rippleDescriptor() const { return {mSampler, mField.view, VK_IMAGE_LAYOUT_GENERAL}; }
    VkDescriptorImageInfo foamDescriptor() const { return {mSampler, mFoam.view, VK_IMAGE_LAYOUT_GENERAL}; }
    bool live() const { return mLive; }
    // Nothing allocated (the renderer may delete the module).
    bool idle() const { return !mField.image && !mFoam.image && !mPipe; }
    void destroy(VkDevice dev);
    uint32_t tier() const { return mTier; }

private:
    bool ensurePipeline(VkDeviceContext* ctx);
    bool ensureFoam(VkDeviceContext* ctx);
    bool ensureField(VkDeviceContext* ctx, uint32_t n);
    void freeField();
    void freeAll(VkDevice dev);
    void writeSets();
    uint32_t wantTier(const Frame& f) const;

    VkDeviceContext*      mCtx = nullptr;
    VkSampler             mSampler = VK_NULL_HANDLE;   // linear / repeat (the FS's reads)
    VkDescriptorSetLayout mDsl = VK_NULL_HANDLE;       // 0 field (storage image), 1 impulses
    VkDescPool            mPool;
    VkDescriptorSet       mSet[kSlots] = {};
    VkPipelineLayout      mLayout = VK_NULL_HANDLE;
    VkPipeline            mPipe = VK_NULL_HANDLE;

    GpuImage  mField, mFoam;
    GpuBuffer mImpulses;            // kSlots x kMaxImpulses x 16 B, host-visible
    uint32_t  mN = 0;               // field size (0 = none)
    float     mTexel = 0.f;         // m per cell

    uint32_t mTier = TIER_OFF;      // in effect this frame
    bool     mLive = false;
    bool     mBindingDirty = false;
    bool     mFieldValid = false;   // the field holds a cleared / simulated state
    double   mOffSince = -1.0;      // when the tier went to off (free after kFreeGrace)
    double   mRetryAt = 0.0;        // allocation failure: next attempt
    uint32_t mFails = 0;

    // The step plan of this recording (prepare -> record).
    struct Push { int32_t win[4]; uint32_t ctl[4]; float step[4]; float sponge[4]; };
    uint32_t mSteps = 0;            // sub-steps this frame
    bool     mClear = false;        // clear first (window jump / new field)
    Push     mPush{};               // the first step's (impulses); the rest derive from it
    float    mSubDt = 0.f, mPrevDt = 1.f / 60.f;
    int32_t  mOrigin[2] = {0, 0}, mPrevOrigin[2] = {0, 0};
    uint32_t mLayer = 0;            // the layer holding the latest state (after this frame's steps)
    uint32_t mSlot = 0;
    uint32_t mImpulseCount = 0;
    float    mCentre[2] = {0.f, 0.f};
    float    mInfo[2][4] = {};      // the frame info of this recording
};

#endif
