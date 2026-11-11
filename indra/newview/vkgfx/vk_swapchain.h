// vkgfx — swapchain + present, the one foundation piece the offscreen spike did
// not need. Owns a VkSwapchainKHR over a caller-supplied VkSurfaceKHR, per-image
// views, and per-frame-in-flight sync objects (acquire semaphore, render-finished
// semaphore, in-flight fence). Presentation uses classic binary semaphores
// (timeline semaphores can't gate vkQueuePresentKHR).
//
// Usage per frame:
//   uint32_t idx;
//   if (!sc.acquire(&idx)) { sc.recreate(...); return; }   // OUT_OF_DATE -> recreate
//   ... record into a command buffer, targeting image(idx), using
//       imageAvailableSemaphore() as a wait and renderFinishedSemaphore() as a
//       signal on the graphics submit, and inFlightFence() on the submit ...
//   sc.present(idx);
//
// This file depends only on vk_device (which owns instance/device/queue) plus the
// Vulkan headers; it does NOT depend on SDL or the viewer, so it stays testable
// in isolation. The surface is created by the windowing layer (SDL) and handed in.
#pragma once

#include "vk_device.h"
#include "vk_display_hdr.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

class VkSwapchain {
public:
    // surface must already be created from ctx.instance() by the window layer.
    // Chooses an SRGB/UNORM B8G8R8A8 format, FIFO present mode (vsync, always
    // available), and a clamped image count. Returns false if the surface has no
    // usable format (caller should fall back / report).
    bool init(VkDeviceContext& ctx, VkSurfaceKHR surface,
              uint32_t width, uint32_t height, uint32_t framesInFlight = 2);
    void destroy(VkDeviceContext& ctx);

    // Tear down and rebuild the swapchain (window resize / OUT_OF_DATE). Keeps
    // the surface. Waits device-idle first.
    bool recreate(VkDeviceContext& ctx, uint32_t width, uint32_t height);
    // CPU wall time spent waiting for prior GPU work during the last recreate.
    double lastDeviceIdleWaitMs() const { return mLastDeviceIdleWaitMs; }

    // RenderVSyncEnable. true: FIFO (vsync). false: MAILBOX (uncapped, no tearing)
    // when the surface offers it, else IMMEDIATE, else FIFO. Read at (re)creation.
    static bool sVSync;
    // RenderVKNoVsyncPresentMode (only with sVSync off): 0 = MAILBOX first (no tearing;
    // then IMMEDIATE, then FIFO), 1 = IMMEDIATE first (then MAILBOX, then FIFO). A
    // diagnostic: some Windows compositors pace MAILBOX unevenly. Read at (re)creation.
    static uint32_t sNoVsyncMode;

    // QuasarFrameGen (read at startup): each frame presents a generated in-between image,
    // then the real one half a frame later, from a pacing thread. Read at (re)creation for
    // the image count and the present mode.
    static bool sFrameGen;
    // Frame generation, per frame: after acquire() (the real image), acquireGen() takes the
    // generated image (waits this slot's generated-frame fence, acquires with its own
    // semaphore, resets the fence). The frame's LAST submission waits
    // imageAvailableGenSemaphore(), signals renderFinishedSemaphore(genIndex) and genFence();
    // then presentPaced() hands both images to the pacing thread and advances the frame slot
    // (in place of present()).
    bool acquireGen(VkDeviceContext& ctx, uint32_t* outImageIndex, bool* outNeedsRecreate);
    // The live chain was created for frame generation (enough images, its present mode):
    // until a rebuild after QuasarFrameGen turns on, frames present normally.
    bool builtForFrameGen() const { return mBuiltForFrameGen; }
    VkSemaphore imageAvailableGenSemaphore() const { return mImageAvailableGen[mFrame]; }
    VkFence     genFence() const { return mGenFence[mFrame]; }
    void presentPaced(VkDeviceContext& ctx, uint32_t genIndex, uint32_t realIndex);
    // A paced present asked for a rebuild (OUT_OF_DATE / SUBOPTIMAL); cleared by the call.
    bool takePacerRecreate() { return mPacerNeedsRecreate.exchange(false); }
    // Presents made by the pacing thread so far (generated + real).
    uint64_t pacedPresents() const { return mPacedPresents.load(std::memory_order_relaxed); }
    // Waits until the pacing thread has shown everything handed to it.
    void pacerFlush();

    // FRAME PACING: waits the current frame-in-flight slot's fence only (no reset, no
    // acquire), so the main loop can block at the TOP of its frame -- before input and
    // simulation -- instead of mid-frame inside acquire(). acquire() keeps its own wait
    // (then free). Safe at any point between frames: mFrame only advances in present(),
    // and a skipped frame (OUT_OF_DATE, minimized) leaves the fence signaled. No-op on a
    // lost device or without sync objects.
    void waitFrameSlot(VkDeviceContext& ctx);

    // Waits the current frame's in-flight fence, then acquires the next image.
    // Returns false and sets *outNeedsRecreate on OUT_OF_DATE/SUBOPTIMAL; the
    // caller should recreate() and skip the frame.
    bool acquire(VkDeviceContext& ctx, uint32_t* outImageIndex,
                 bool* outNeedsRecreate);

    // Presents the given image index. Returns false + sets *outNeedsRecreate on
    // OUT_OF_DATE/SUBOPTIMAL.
    bool present(VkDeviceContext& ctx, uint32_t imageIndex,
                 bool* outNeedsRecreate);

    // ---- per-frame accessors (indexed by the current frame-in-flight slot) ----
    // imageAvailable + the in-flight fence are per-frame-in-flight: acquire signals
    // the semaphore and the fence guards reuse of that slot's command buffer/VBO.
    VkSemaphore imageAvailableSemaphore() const { return mImageAvailable[mFrame]; }
    VkFence     inFlightFence()          const { return mInFlight[mFrame]; }
    // render-finished is per-IMAGE: it is signaled by the submit that renders
    // image `i` and waited by the present of image `i`. Indexing it by the
    // frame-in-flight slot (2) instead of the image (3 here) let a submit re-signal
    // a semaphore a pending present had not yet consumed — a WSI sync hazard.
    VkSemaphore renderFinishedSemaphore(uint32_t imageIndex) const { return mRenderFinished[imageIndex]; }
    // Current frame-in-flight slot (0..framesInFlight-1); the caller indexes its
    // per-slot command buffers / vertex buffers with this so reuse is guarded by
    // the same in-flight fence that acquire() waits on.
    uint32_t    frameSlot()      const { return mFrame; }
    uint32_t    framesInFlight() const { return mFramesInFlight; }
    // The last submission of frame-in-flight slot `slot` has completed (its fence is signaled;
    // non-blocking). Its fence is reset at that slot's next acquire.
    bool        slotDone(VkDevice dev, uint32_t slot) const;

    // ---- swapchain image accessors (indexed by acquired image index) ----
    VkImage     image(uint32_t i)     const { return mImages[i]; }
    VkImageView imageView(uint32_t i) const { return mImageViews[i]; }
    VkFormat    format()              const { return mFormat.format; }
    VkExtent2D  extent()              const { return mExtent; }
    uint32_t    imageCount()          const { return (uint32_t)mImages.size(); }
    bool        valid()               const { return mSwapchain != VK_NULL_HANDLE; }
    VkColorSpaceKHR  colorSpace()     const { return mFormat.colorSpace; }
    // Set at startup / explicit output or monitor changes, before recreation. Readiness
    // requires the complete float-world + linear UI + SDR capture path, not just shaders.
    void setDisplayPolicy(vkdisplay::Request request, vkdisplay::Capability capability,
                          bool linearOutputReady) {
        mDisplayRequest = vkdisplay::sanitized(request);
        mDisplayCapability = capability;
        mLinearOutputReady = linearOutputReady;
    }
    const vkdisplay::Status& displayStatus() const { return mDisplayStatus; }
    // Bounded, allocation-local query used to stage a renderer output bank before
    // retiring the currently visible swapchain. Does not mutate the live chain.
    bool previewDisplayStatus(VkDeviceContext& ctx, vkdisplay::Status& out) const;
    // Predict the exact extent selected by the next createSwapchainObjects call.
    // Used to admit output targets before the current WSI chain is retired.
    bool previewExtent(VkDeviceContext& ctx, uint32_t width, uint32_t height,
                       VkExtent2D& out) const;
    VkPresentModeKHR presentMode()    const { return mPresentMode; }
    // VRAM accounting (#407): the device memory the swapchain images take -- MEASURED
    // (vkGetDeviceImageMemoryRequirements on the swapchain's own image create info, so
    // tiling / alignment / compression metadata are included) x the image count the
    // driver actually returned. The WSI allocates these, not VkDeviceContext, so they
    // are not in its tracker. 0 = unknown (no swapchain).
    uint64_t    memoryBytes()         const { return mMemBytes; }
    VkSwapchainKHR handle()           const { return mSwapchain; }

    // #919 full-screen exclusive (Windows, VK_EXT_full_screen_exclusive). The owner keeps
    // these platform structs; this file stays platform-free:
    //  - createChain: chained into VkSwapchainCreateInfoKHR::pNext at the next
    //    (re)creation (nullptr = none). Must stay valid until then.
    //  - presentModeQuery: replaces vkGetPhysicalDeviceSurfacePresentModesKHR while set (the
    //    modes valid WITH the chain: vkGetPhysicalDeviceSurfacePresentModes2EXT). Returns
    //    false to fall back to the plain query.
    void setCreateChain(const void* createChain) { mCreateChain = createChain; }
    const void* createChain() const { return mCreateChain; }
    void setPresentModeQuery(std::function<bool(std::vector<VkPresentModeKHR>&)> q) { mPresentModeQuery = std::move(q); }
    // An acquire or present returned VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT since the
    // last call (cleared by the call). Those calls return false WITHOUT needsRecreate (the
    // owner re-acquires the mode, or rebuilds without the chain); the frame is skipped
    // (acquire) or was still queued (present). Never a device loss.
    bool takeExclusiveLost() { const bool l = mExclusiveLost; mExclusiveLost = false; return l; }
    // The last (re)creation was refused WITH the chain and fell back to a plain swapchain.
    bool chainRejected() const { return mChainRejected; }

private:
    bool createSwapchainObjects(VkDeviceContext& ctx, uint32_t width,
                                uint32_t height);
    void destroySwapchainObjects(VkDeviceContext& ctx);

    VkSurfaceKHR       mSurface   = VK_NULL_HANDLE; // owned by window layer
    VkSwapchainKHR     mSwapchain = VK_NULL_HANDLE;
    VkSurfaceFormatKHR mFormat{};
    VkPresentModeKHR   mPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D         mExtent{0, 0};
    uint64_t           mMemBytes = 0;   // #407: measured image memory, all images
    double             mLastDeviceIdleWaitMs = 0.0;
    vkdisplay::Request mDisplayRequest;
    vkdisplay::Capability mDisplayCapability;
    vkdisplay::Status mDisplayStatus;
    bool mLinearOutputReady = false;
    const void*        mCreateChain = nullptr;   // #919
    std::function<bool(std::vector<VkPresentModeKHR>&)> mPresentModeQuery;   // #919
    bool               mExclusiveLost = false;   // #919
    bool               mChainRejected = false;   // #919

    std::vector<VkImage>     mImages;
    std::vector<VkImageView> mImageViews;

    uint32_t mFramesInFlight = 2;
    uint32_t mFrame = 0;
    // Quasar frame generation (see acquireGen / presentPaced).
    struct PacedJob {
        VkDeviceContext* ctx = nullptr;
        uint32_t slot = 0;
        VkFence  fence = VK_NULL_HANDLE;
        uint32_t genIndex = 0, realIndex = 0;
    };
    VkResult lockedAcquire(VkDeviceContext& ctx, VkSemaphore sem, uint32_t* outImageIndex);
    void pacerLoop();
    void pacerStop();
    bool                     mBuiltForFrameGen = false;
    std::vector<VkSemaphore> mImageAvailableGen;
    std::vector<VkFence>     mGenFence;
    std::vector<std::atomic<bool>> mGenSlotBusy;   // the pacing thread still owns the slot's fence
    std::thread              mPacer;
    std::mutex               mPacerMutex;
    std::condition_variable  mPacerCv;
    std::deque<PacedJob>     mPacerJobs;
    bool                     mPacerQuit = false;
    bool                     mPacerBusy = false;
    std::atomic<bool>        mPacerNeedsRecreate{false};
    std::atomic<uint64_t>    mPacedPresents{0};

    std::vector<VkSemaphore> mImageAvailable; // size == framesInFlight
    std::vector<VkSemaphore> mRenderFinished;
    std::vector<VkFence>     mInFlight;
};
