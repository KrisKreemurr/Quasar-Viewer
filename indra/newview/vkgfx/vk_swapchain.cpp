#include "vk_swapchain.h"
#include "ql_queue_lock.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <new>
#include <thread>

bool VkSwapchain::init(VkDeviceContext& ctx, VkSurfaceKHR surface,
                       uint32_t width, uint32_t height, uint32_t framesInFlight) {
    mSurface = surface;
    mFramesInFlight = framesInFlight ? framesInFlight : 2;

    // Sanity: confirm the graphics queue family can present to this surface.
    VkBool32 presentSupported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(ctx.physicalDevice(), ctx.queueFamily(),
                                         mSurface, &presentSupported);
    if (!presentSupported) {
        std::fprintf(stderr,
            "[vkgfx] queue family %u does not support present to surface\n",
            ctx.queueFamily());
        return false;
    }

    // createSwapchainObjects() also (re)creates the per-IMAGE render-finished
    // semaphores, since the image count is only known there and can change on
    // recreate().
    if (!createSwapchainObjects(ctx, width, height)) return false;

    // Per-frame-in-flight sync objects: image-available semaphore + in-flight
    // fence. (render-finished is per-image; owned by createSwapchainObjects.)
    mImageAvailable.resize(mFramesInFlight);
    mInFlight.resize(mFramesInFlight);
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT; // so the first acquire doesn't hang
    for (uint32_t i = 0; i < mFramesInFlight; ++i) {
        VK_CHECK(vkCreateSemaphore(ctx.device(), &sci, nullptr, &mImageAvailable[i]));
        VK_CHECK(vkCreateFence(ctx.device(), &fci, nullptr, &mInFlight[i]));
    }
    // Quasar frame generation: the generated image's own acquire semaphore and fence per
    // frame-in-flight slot (always made: a handful of objects).
    mImageAvailableGen.resize(mFramesInFlight);
    mGenFence.resize(mFramesInFlight);
    mGenSlotBusy = std::vector<std::atomic<bool>>(mFramesInFlight);
    for (uint32_t i = 0; i < mFramesInFlight; ++i) {
        VK_CHECK(vkCreateSemaphore(ctx.device(), &sci, nullptr, &mImageAvailableGen[i]));
        VK_CHECK(vkCreateFence(ctx.device(), &fci, nullptr, &mGenFence[i]));
        mGenSlotBusy[i].store(false);
    }
    return true;
}

bool VkSwapchain::sVSync = true;
uint32_t VkSwapchain::sNoVsyncMode = 0;
bool VkSwapchain::sFrameGen = false;

bool VkSwapchain::createSwapchainObjects(VkDeviceContext& ctx, uint32_t width,
                                         uint32_t height) {
    VkSurfaceCapabilitiesKHR caps{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        ctx.physicalDevice(), mSurface, &caps));

    // Choose a format: prefer BGRA8 SRGB, else the first available.
    uint32_t fmtCount = 0;
    const VkResult formatCountResult = vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physicalDevice(), mSurface,
                                                                          &fmtCount, nullptr);
    if (formatCountResult != VK_SUCCESS || fmtCount == 0 || fmtCount > 256) {
        std::fprintf(stderr, "[vkgfx] surface has no formats\n");
        return false;
    }
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    const VkResult formatResult = vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physicalDevice(), mSurface,
                                                                     &fmtCount, fmts.data());
    if (formatResult != VK_SUCCESS || fmtCount == 0 || fmtCount > fmts.size()) return false;
    fmts.resize(fmtCount);
    if (fmtCount == 1 && fmts[0].format == VK_FORMAT_UNDEFINED &&
        fmts[0].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        fmts[0].format = VK_FORMAT_B8G8R8A8_UNORM;
    // An HDR-only first entry cannot be used as an SDR fallback: SDR shader bytes
    // on that target would be interpreted as PQ or absolute linear luminance.
    mFormat = {VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    for (const auto& f : fmts) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            mFormat = f; break;
        }
    }
    if (mFormat.format == VK_FORMAT_UNDEFINED) {
        for (const auto& f : fmts) {
            if ((f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_SRGB ||
                 f.format == VK_FORMAT_B8G8R8A8_SRGB) && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                mFormat = f; break;
            }
        }
    }
    if (mFormat.format == VK_FORMAT_UNDEFINED) {
        std::fprintf(stderr, "[vkgfx] surface offers no supported SDR fallback\n");
        return false;
    }
    const VkSurfaceFormatKHR sdrFormat = mFormat;
    mDisplayStatus = vkdisplay::choose(mDisplayRequest, mDisplayCapability, fmts,
                                      mFormat, mLinearOutputReady);
    mFormat = mDisplayStatus.actual;
    if (mDisplayRequest.enabled)
        std::fprintf(stderr, "[vkgfx] HDR display: %s (format %u, colour space %u)\n",
                     vkdisplay::reasonText(mDisplayStatus.reason), unsigned(mFormat.format), unsigned(mFormat.colorSpace));

    // Present mode: FIFO is always available (vsync). With vsync off take MAILBOX
    // (latest frame wins, no tearing), else IMMEDIATE (may tear), else FIFO; with
    // sNoVsyncMode 1 the order of the first two flips (IMMEDIATE bypasses a compositor
    // that releases MAILBOX images on its own cadence).
    mPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!sVSync)
    {
        std::vector<VkPresentModeKHR> pms;
        // #919: with a full-screen-exclusive chain, the modes valid for it.
        if (!(mCreateChain && mPresentModeQuery && mPresentModeQuery(pms)))
        {
            pms.clear();
            uint32_t pmCount = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.physicalDevice(), mSurface, &pmCount, nullptr);
            pms.resize(pmCount);
            if (pmCount)
                vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.physicalDevice(), mSurface, &pmCount, pms.data());
            pms.resize(pmCount);
        }
        bool mailbox = false, immediate = false;
        for (VkPresentModeKHR m : pms)
        {
            mailbox   |= (m == VK_PRESENT_MODE_MAILBOX_KHR);
            immediate |= (m == VK_PRESENT_MODE_IMMEDIATE_KHR);
        }
        // Quasar frame generation: MAILBOX may replace the generated frame with the real one
        // before it is shown, so without vsync it presents IMMEDIATE when it can.
        if (sNoVsyncMode == 1 || sFrameGen)
        {
            if (immediate)    mPresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            else if (mailbox) mPresentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        }
        else if (mailbox)   mPresentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (immediate) mPresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }

    // Extent.
    if (caps.currentExtent.width != 0xFFFFFFFFu) {
        mExtent = caps.currentExtent;
    } else {
        mExtent.width  = std::clamp(width,  caps.minImageExtent.width,
                                    caps.maxImageExtent.width);
        mExtent.height = std::clamp(height, caps.minImageExtent.height,
                                    caps.maxImageExtent.height);
    }
    if (mExtent.width == 0 || mExtent.height == 0) {
        // Minimized; caller should skip. Report benign.
        return false;
    }

    // Quasar frame generation: two images a frame (the generated one and the real one) for
    // every frame in flight, plus the ones the presentation engine holds.
    uint32_t imageCount = caps.minImageCount + (sFrameGen ? 3u : 1u);
    mBuiltForFrameGen = sFrameGen;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.pNext = mCreateChain;   // #919: full-screen exclusive info (Windows), else nullptr
    ci.surface = mSurface;
    ci.minImageCount = imageCount;
    ci.imageFormat = mFormat.format;
    ci.imageColorSpace = mFormat.colorSpace;
    ci.imageExtent = mExtent;
    ci.imageArrayLayers = 1;
    // COLOR_ATTACHMENT for rendering; TRANSFER_DST so a compute/blit path can
    // also copy into the swapchain image (the clean pipeline blits its resolved
    // color target). Both are near-universally supported.
    // TRANSFER_SRC: the world renderer copies the lit scene out for next frame's
    // screen-space reflections.
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; // single queue family
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mPresentMode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = VK_NULL_HANDLE;

    VkResult cr = vkCreateSwapchainKHR(ctx.device(), &ci, nullptr, &mSwapchain);
    mChainRejected = false;
    if (cr != VK_SUCCESS && ci.pNext && cr != VK_ERROR_DEVICE_LOST) {
        // #919: a driver refusing the full-screen exclusive swapchain must not take the
        // viewer down: fall back to a plain (composited) swapchain and report it to the owner.
        std::fprintf(stderr, "[vkgfx] vkCreateSwapchainKHR with the full-screen exclusive chain = %s; "
                     "retrying without it (FIFO)\n", vkResultString(cr));
        mChainRejected = true;
        mSwapchain = VK_NULL_HANDLE;
        ci.pNext = nullptr;
        mPresentMode = VK_PRESENT_MODE_FIFO_KHR;   // (the mode came from the exclusive query)
        ci.presentMode = mPresentMode;
        cr = vkCreateSwapchainKHR(ctx.device(), &ci, nullptr, &mSwapchain);
    }
    if (cr != VK_SUCCESS && cr != VK_ERROR_DEVICE_LOST && mDisplayStatus.encoding != vkdisplay::Encoding::Sdr)
    {
        // Optional output failure is not fatal. Rebuild a real SDR target, never
        // relabel the failed HDR format and feed it SDR pixels.
        mFormat = sdrFormat;
        mDisplayStatus.actual = sdrFormat;
        mDisplayStatus.encoding = vkdisplay::Encoding::Sdr;
        mDisplayStatus.reason = vkdisplay::Reason::ResourcesUnavailable;
        ci.imageFormat = sdrFormat.format;
        ci.imageColorSpace = sdrFormat.colorSpace;
        mSwapchain = VK_NULL_HANDLE;
        std::fprintf(stderr, "[vkgfx] HDR display swapchain refused; retrying SDR\n");
        cr = vkCreateSwapchainKHR(ctx.device(), &ci, nullptr, &mSwapchain);
    }
    VK_CHECK(cr);
    if (mDisplayStatus.encoding == vkdisplay::Encoding::Hdr10 &&
        ctx.optionalExtensionEnabled(VK_EXT_HDR_METADATA_EXTENSION_NAME))
    {
        const auto setMetadata = reinterpret_cast<PFN_vkSetHdrMetadataEXT>(
            vkGetDeviceProcAddr(ctx.device(), "vkSetHdrMetadataEXT"));
        VkHdrMetadataEXT metadata{VK_STRUCTURE_TYPE_HDR_METADATA_EXT};
        metadata.displayPrimaryRed = {.708f, .292f};
        metadata.displayPrimaryGreen = {.170f, .797f};
        metadata.displayPrimaryBlue = {.131f, .046f};
        metadata.whitePoint = {.3127f, .3290f};
        metadata.maxLuminance = mDisplayStatus.peakNits;
        metadata.minLuminance = 0.f;
        metadata.maxContentLightLevel = mDisplayStatus.peakNits;
        metadata.maxFrameAverageLightLevel = mDisplayStatus.peakNits; // conservative bound, not a measured frame statistic
        if (setMetadata) setMetadata(ctx.device(), 1, &mSwapchain, &metadata);
    }

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(ctx.device(), mSwapchain, &n, nullptr);
    mImages.resize(n);
    vkGetSwapchainImagesKHR(ctx.device(), mSwapchain, &n, mImages.data());

    {   // #407: what these images take in device memory. The same create info the WSI
        // uses for them (no VkImageSwapchainCreateInfoKHR: not allowed in this query).
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = ci.imageFormat;
        ici.extent = {mExtent.width, mExtent.height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = ci.imageArrayLayers;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = ci.imageUsage;
        ici.sharingMode = ci.imageSharingMode;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkDeviceImageMemoryRequirements dr{VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS};
        dr.pCreateInfo = &ici;
        VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        vkGetDeviceImageMemoryRequirements(ctx.device(), &dr, &req);
        mMemBytes = (uint64_t)req.memoryRequirements.size * n;
    }

    mImageViews.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = mImages[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = mFormat.format;
        vci.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                          VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(ctx.device(), &vci, nullptr, &mImageViews[i]));
    }

    // Per-IMAGE render-finished semaphores (one per swapchain image). Must be
    // (re)created here because the image count is only known now and can change
    // across recreate(); present() waits mRenderFinished[imageIndex].
    mRenderFinished.resize(n);
    VkSemaphoreCreateInfo rsci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < n; ++i) {
        VK_CHECK(vkCreateSemaphore(ctx.device(), &rsci, nullptr, &mRenderFinished[i]));
    }
    return true;
}

bool VkSwapchain::previewDisplayStatus(VkDeviceContext& ctx, vkdisplay::Status& out) const
{
    if (!mSurface) return false;
    uint32_t count = 0;
    const VkResult countResult = vkGetPhysicalDeviceSurfaceFormatsKHR(
        ctx.physicalDevice(), mSurface, &count, nullptr);
    if (countResult != VK_SUCCESS || count == 0 || count > 256) return false;
    std::vector<VkSurfaceFormatKHR> formats;
    try { formats.resize(count); }
    catch (const std::bad_alloc&) { return false; }
    const VkResult formatsResult = vkGetPhysicalDeviceSurfaceFormatsKHR(
        ctx.physicalDevice(), mSurface, &count, formats.data());
    if (formatsResult != VK_SUCCESS || count == 0 || count > formats.size()) return false;
    formats.resize(count);
    if (count == 1 && formats[0].format == VK_FORMAT_UNDEFINED &&
        formats[0].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        formats[0].format = VK_FORMAT_B8G8R8A8_UNORM;

    VkSurfaceFormatKHR sdr{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    for (const auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sdr = f; break; }
    if (sdr.format == VK_FORMAT_UNDEFINED)
        for (const auto& f : formats)
            if ((f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_SRGB ||
                 f.format == VK_FORMAT_B8G8R8A8_SRGB) &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sdr = f; break; }
    if (sdr.format == VK_FORMAT_UNDEFINED) return false;
    out = vkdisplay::choose(mDisplayRequest, mDisplayCapability, formats, sdr,
                            mLinearOutputReady);
    return true;
}

bool VkSwapchain::previewExtent(VkDeviceContext& ctx, uint32_t width, uint32_t height,
                                VkExtent2D& out) const
{
    if (!mSurface) return false;
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.physicalDevice(),mSurface,&caps)!=VK_SUCCESS)
        return false;
    if (caps.currentExtent.width!=0xffffffffu)
        out=caps.currentExtent;
    else
    {
        out.width=std::clamp(width,caps.minImageExtent.width,caps.maxImageExtent.width);
        out.height=std::clamp(height,caps.minImageExtent.height,caps.maxImageExtent.height);
    }
    return out.width!=0 && out.height!=0;
}

void VkSwapchain::destroySwapchainObjects(VkDeviceContext& ctx) {
    for (VkSemaphore s : mRenderFinished) {
        if (s) vkDestroySemaphore(ctx.device(), s, nullptr);
    }
    mRenderFinished.clear();
    for (VkImageView v : mImageViews) {
        if (v) vkDestroyImageView(ctx.device(), v, nullptr);
    }
    mImageViews.clear();
    mImages.clear();
    mMemBytes = 0;
    if (mSwapchain) {
        vkDestroySwapchainKHR(ctx.device(), mSwapchain, nullptr);
        mSwapchain = VK_NULL_HANDLE;
    }
}

bool VkSwapchain::recreate(VkDeviceContext& ctx, uint32_t width, uint32_t height) {
    if (vkDeviceIsLost()) return false;   // #135
    pacerFlush();   // Quasar: no paced present may reach the retired chain
    const auto idleStart = std::chrono::steady_clock::now();
    vkDeviceWaitIdle(ctx.device());
    mLastDeviceIdleWaitMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - idleStart).count();
    destroySwapchainObjects(ctx);
    return createSwapchainObjects(ctx, width, height);
}

void VkSwapchain::destroy(VkDeviceContext& ctx) {
    pacerStop();    // Quasar: the pacing thread ends before anything it presents goes
    if (vkDeviceIsLost()) return;   // #135: leak to process exit, never touch the lost device
    if (ctx.device()) vkDeviceWaitIdle(ctx.device());
    for (VkSemaphore s : mImageAvailableGen) if (s) vkDestroySemaphore(ctx.device(), s, nullptr);
    for (VkFence f : mGenFence) if (f) vkDestroyFence(ctx.device(), f, nullptr);
    mImageAvailableGen.clear();
    mGenFence.clear();
    // Per-frame-in-flight objects (image-available semaphores + fences).
    for (VkSemaphore s : mImageAvailable) if (s) vkDestroySemaphore(ctx.device(), s, nullptr);
    for (VkFence f : mInFlight) if (f) vkDestroyFence(ctx.device(), f, nullptr);
    mImageAvailable.clear();
    mInFlight.clear();
    // Per-image render-finished semaphores are destroyed by destroySwapchainObjects.
    destroySwapchainObjects(ctx);
    // Surface is owned by the window layer; not destroyed here.
    mSurface = VK_NULL_HANDLE;
}

bool VkSwapchain::slotDone(VkDevice dev, uint32_t slot) const
{
    return dev && slot < mInFlight.size() && mInFlight[slot] && vkGetFenceStatus(dev, mInFlight[slot]) == VK_SUCCESS;
}

void VkSwapchain::waitFrameSlot(VkDeviceContext& ctx) {
    if (vkDeviceIsLost() || !ctx.device()) return;   // #135
    if (mFrame >= mInFlight.size() || !mInFlight[mFrame]) return;   // (not created / destroyed)
    // The same hooked wait as acquire() (vk_common.h: device loss raises with this site,
    // an infinite wait is sliced into the hang watchdog). The fence is NOT reset here:
    // acquire() resets it only once a submit is certain.
    VK_CHECK(vkWaitForFences(ctx.device(), 1, &mInFlight[mFrame], VK_TRUE, UINT64_MAX));
}

bool VkSwapchain::acquire(VkDeviceContext& ctx, uint32_t* outImageIndex,
                          bool* outNeedsRecreate) {
    if (outNeedsRecreate) *outNeedsRecreate = false;
    if (vkDeviceIsLost()) return false;   // #135: no acquire on a lost device
    if (!mSwapchain) { if (outNeedsRecreate) *outNeedsRecreate = true; return false; }

    // Hooked (vk_common.h): DEVICE_LOST raises with this site (the #135 crash came
    // through here), an infinite wait is sliced into the hang watchdog. With frame pacing
    // (waitFrameSlot at the top of the main-loop frame) this fence is already signaled and
    // the wait returns at once; kept as the correctness backstop for slot reuse.
    VK_CHECK(vkWaitForFences(ctx.device(), 1, &mInFlight[mFrame], VK_TRUE, UINT64_MAX));
    if (vkDeviceIsLost()) return false;
    vkAddrFrameTick();

    VkResult r = lockedAcquire(ctx, mImageAvailable[mFrame], outImageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
        // No image delivered and the semaphore was NOT signaled: skip the frame
        // and recreate. Do not reset the fence (no submit this frame).
        // #919: exclusive mode lost (alt-tab, another app took the display): the owner
        // re-acquires it or rebuilds without exclusive mode -- never a device loss. It does
        // not ask for a recreate: re-acquiring the mode is enough, and the owner falls back
        // to a rebuild without exclusive mode when that keeps failing.
        if (r == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) mExclusiveLost = true;
        else if (outNeedsRecreate) *outNeedsRecreate = true;
        return false;
    }
    if (r == VK_SUBOPTIMAL_KHR) {
        // SUBOPTIMAL DID deliver an image and DID signal the semaphore; we must
        // render + present this frame (else the acquire semaphore stays signaled
        // with no matching wait). Flag recreate so the caller rebuilds AFTER
        // present. Fall through to the success path.
        if (outNeedsRecreate) *outNeedsRecreate = true;
    }
    else if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[vkgfx] vkAcquireNextImageKHR = %s\n",
                     vkResultString(r));
        // A lost device, or a lost surface (the window's surface is gone: nothing can be
        // presented again without re-creating the device's WSI objects): the graceful
        // device-lost exit (#135) instead of skipping every frame from now on.
        if (r == VK_ERROR_DEVICE_LOST || r == VK_ERROR_SURFACE_LOST_KHR)
            vkDeviceLostRaise("vkAcquireNextImageKHR", r, "graphics", __FILE__, __LINE__);
        return false;
    }
    // Only reset the fence once we know we'll submit work this frame.
    VK_CHECK(vkResetFences(ctx.device(), 1, &mInFlight[mFrame]));
    return true;
}

bool VkSwapchain::present(VkDeviceContext& ctx, uint32_t imageIndex,
                          bool* outNeedsRecreate) {
    if (outNeedsRecreate) *outNeedsRecreate = false;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    // Wait the render-finished semaphore for THIS image (signaled by its submit).
    pi.pWaitSemaphores = &mRenderFinished[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &mSwapchain;
    pi.pImageIndices = &imageIndex;

    if (vkDeviceIsLost()) return false;   // #135: nothing reaches a lost device
    VkResult r = VK_SUCCESS;
    if (vkSimulateDeviceLostTake(VK_SIM_LOST_PRESENT))
        r = VK_ERROR_DEVICE_LOST;          // RenderVKDebugSimulateDeviceLost 3: not presented
    else
    {
        std::lock_guard<std::mutex> ql(qlQueueMutex());   // Quasar: see ql_queue_lock.h
        r = vkQueuePresentKHR(ctx.queue(), &pi);
    }
    mFrame = (mFrame + 1) % mFramesInFlight;

    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
        // (#919: like OUT_OF_DATE, a MODE_LOST present still counts as enqueued -- its
        // semaphore wait executes -- so nothing is left signaled; no recreate, see acquire.)
        if (r == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) mExclusiveLost = true;
        else if (outNeedsRecreate) *outNeedsRecreate = true;
        return false;
    }
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[vkgfx] vkQueuePresentKHR = %s\n", vkResultString(r));
        if (r == VK_ERROR_DEVICE_LOST || r == VK_ERROR_SURFACE_LOST_KHR)
            vkDeviceLostRaise(vkDeviceLostSimulated() ? "vkQueuePresentKHR (simulated)" : "vkQueuePresentKHR",
                              r, "graphics", __FILE__, __LINE__);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Quasar frame generation: the second acquire and the paced presents
// ---------------------------------------------------------------------------
VkResult VkSwapchain::lockedAcquire(VkDeviceContext& ctx, VkSemaphore sem, uint32_t* outImageIndex)
{
    // While the pacing thread presents, an acquire that blocks under the lock could wait
    // for an image only its next present frees: poll instead, releasing the lock between
    // tries. Without the thread it is the plain blocking acquire (under the lock, which is
    // then uncontended).
    if (!mPacer.joinable())
    {
        std::lock_guard<std::mutex> ql(qlQueueMutex());
        return vkAcquireNextImageKHR(ctx.device(), mSwapchain, UINT64_MAX, sem, VK_NULL_HANDLE, outImageIndex);
    }
    const auto start = std::chrono::steady_clock::now();
    for (;;)
    {
        VkResult r;
        {
            std::lock_guard<std::mutex> ql(qlQueueMutex());
            r = vkAcquireNextImageKHR(ctx.device(), mSwapchain, 0, sem, VK_NULL_HANDLE, outImageIndex);
        }
        if (r != VK_NOT_READY && r != VK_TIMEOUT) return r;
        if (vkDeviceIsLost()) return VK_ERROR_DEVICE_LOST;
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(10))
        {
            std::fprintf(stderr, "[vkgfx] Quasar frame generation: no swapchain image for 10 s\n");
            return VK_ERROR_OUT_OF_DATE_KHR;   // rebuild the chain rather than hang
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

bool VkSwapchain::acquireGen(VkDeviceContext& ctx, uint32_t* outImageIndex, bool* outNeedsRecreate)
{
    if (outNeedsRecreate) *outNeedsRecreate = false;
    if (vkDeviceIsLost() || !mSwapchain || mFrame >= mGenFence.size()) return false;
    // This slot's last paced presents are done with its fence before it is reset.
    while (mGenSlotBusy[mFrame].load(std::memory_order_acquire))
    {
        if (vkDeviceIsLost()) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    VK_CHECK(vkWaitForFences(ctx.device(), 1, &mGenFence[mFrame], VK_TRUE, UINT64_MAX));
    if (vkDeviceIsLost()) return false;
    const VkResult r = lockedAcquire(ctx, mImageAvailableGen[mFrame], outImageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)
    {
        if (r == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) mExclusiveLost = true;
        else if (outNeedsRecreate) *outNeedsRecreate = true;
        return false;
    }
    if (r == VK_SUBOPTIMAL_KHR) { if (outNeedsRecreate) *outNeedsRecreate = true; }
    else if (r != VK_SUCCESS)
    {
        std::fprintf(stderr, "[vkgfx] Quasar frame generation: vkAcquireNextImageKHR = %s\n", vkResultString(r));
        if (r == VK_ERROR_DEVICE_LOST || r == VK_ERROR_SURFACE_LOST_KHR)
            vkDeviceLostRaise("vkAcquireNextImageKHR", r, "graphics", __FILE__, __LINE__);
        return false;
    }
    VK_CHECK(vkResetFences(ctx.device(), 1, &mGenFence[mFrame]));
    return true;
}

void VkSwapchain::presentPaced(VkDeviceContext& ctx, uint32_t genIndex, uint32_t realIndex)
{
    PacedJob job;
    job.ctx = &ctx;
    job.slot = mFrame;
    job.fence = mGenFence[mFrame];
    job.genIndex = genIndex;
    job.realIndex = realIndex;
    mGenSlotBusy[mFrame].store(true, std::memory_order_release);
    {
        std::unique_lock<std::mutex> lk(mPacerMutex);
        if (!mPacer.joinable())
        {
            mPacerQuit = false;
            mPacer = std::thread([this] { pacerLoop(); });
        }
        // At most one frame waiting behind the one being shown: the main thread never
        // runs more than a frame ahead of what is on screen.
        mPacerCv.wait(lk, [this] { return mPacerJobs.size() < 2; });
        mPacerJobs.push_back(job);
    }
    mPacerCv.notify_all();
    mFrame = (mFrame + 1) % mFramesInFlight;
}

void VkSwapchain::pacerLoop()
{
    using clock = std::chrono::steady_clock;
    clock::time_point lastReady{};
    double intervalMs = 16.6;   // smoothed time between real frames
    for (;;)
    {
        PacedJob job;
        {
            std::unique_lock<std::mutex> lk(mPacerMutex);
            mPacerCv.wait(lk, [this] { return mPacerQuit || !mPacerJobs.empty(); });
            if (mPacerJobs.empty()) return;   // quit with nothing left to show
            job = mPacerJobs.front();
            mPacerBusy = true;
        }
        // The frame is ready when its last submission (the generated image's) has finished.
        // (The driver's own wait, not the hooked one: those hooks keep main-thread
        // statistics. In slices, so a lost device never parks this thread for good.)
        while (!vkDeviceIsLost())
        {
            const VkResult wr = (vkWaitForFences)(job.ctx->device(), 1, &job.fence, VK_TRUE, 100000000ull);
            if (wr != VK_TIMEOUT) break;
        }
        const clock::time_point ready = clock::now();
        if (lastReady.time_since_epoch().count() != 0)
        {
            const double dt = std::chrono::duration<double, std::milli>(ready - lastReady).count();
            if (dt > 1.0 && dt < 100.0) intervalMs += (dt - intervalMs) * 0.1;
            else if (dt >= 100.0) intervalMs = 16.6;   // a hitch: don't let it set the pace
        }
        lastReady = ready;

        auto presentOne = [&](uint32_t idx) -> VkResult
        {
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &mRenderFinished[idx];
            pi.swapchainCount = 1;
            pi.pSwapchains = &mSwapchain;
            pi.pImageIndices = &idx;
            if (vkDeviceIsLost()) return VK_ERROR_DEVICE_LOST;
            std::lock_guard<std::mutex> ql(qlQueueMutex());
            return vkQueuePresentKHR(job.ctx->queue(), &pi);
        };
        auto note = [&](VkResult r)
        {
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) mPacerNeedsRecreate.store(true);
            else if (r == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) mExclusiveLost = true;
            else if (r == VK_ERROR_DEVICE_LOST || r == VK_ERROR_SURFACE_LOST_KHR)
                vkDeviceLostRaise("vkQueuePresentKHR (Quasar frame generation)", r, "graphics", __FILE__, __LINE__);
        };
        // The generated frame now, the real one half a frame later.
        note(presentOne(job.genIndex));
        const clock::time_point due = ready + std::chrono::microseconds((int64_t)(intervalMs * 500.0));
        if (due > clock::now()) std::this_thread::sleep_until(due);
        note(presentOne(job.realIndex));
        mPacedPresents.fetch_add(2, std::memory_order_relaxed);

        mGenSlotBusy[job.slot].store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(mPacerMutex);
            mPacerJobs.pop_front();
            mPacerBusy = false;
        }
        mPacerCv.notify_all();
    }
}

void VkSwapchain::pacerFlush()
{
    if (!mPacer.joinable()) return;
    std::unique_lock<std::mutex> lk(mPacerMutex);
    mPacerCv.wait(lk, [this] { return mPacerJobs.empty() && !mPacerBusy; });
}

void VkSwapchain::pacerStop()
{
    if (!mPacer.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(mPacerMutex);
        mPacerQuit = true;
    }
    mPacerCv.notify_all();
    mPacer.join();
}
