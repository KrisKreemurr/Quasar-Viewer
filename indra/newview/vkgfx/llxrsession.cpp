/**
 * @file llxrsession.cpp
 * @brief VR milestone M0 (#1073): the OpenXR session. See llxrsession.h.
 *
 * Built on the standalone bring-up spike (#1079, contrib/vrspike on branch vr-spike-openxr),
 * which ran this sequence against SteamVR with a Valve Index on the viewer's exact instance /
 * device request: instance + system, xrCreateVulkanInstanceKHR / xrCreateVulkanDeviceKHR,
 * session, B8G8R8A8_SRGB + MUTABLE_FORMAT swapchains with UNORM views, the frame loop.
 */
#include "linden_common.h"

#include "llxrsession.h"

#include "vk_device.h"
#include "llxr.h"

#include "llerror.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#if LL_WINDOWS
#include "llwin32headers.h"
#else
#include <signal.h>
#endif

bool LLXrSession::sActive = false;

namespace
{
using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

#if LL_WINDOWS
// #1122 XR_OCULUS_audio_device_guid. Declared here: openxr_platform.h only declares it under
// XR_USE_PLATFORM_WIN32, which would pull the Windows types into llxr.h.
constexpr const char* kXrOculusAudioDeviceGuidExt = "XR_OCULUS_audio_device_guid";
constexpr uint32_t kXrAudioDeviceStrSize = 128;   // XR_MAX_AUDIO_DEVICE_STR_SIZE_OCULUS
typedef XrResult(XRAPI_PTR* PFN_slGetAudioDeviceGuid)(XrInstance instance, wchar_t buffer[kXrAudioDeviceStrSize]);

std::string wideToUtf8(const wchar_t* w)
{
    if (!w || !*w) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], n, nullptr, nullptr);
    out.resize((size_t)n - 1);
    return out;
}

// The headset's output / microphone endpoint ids (WASAPI, "{0.0.0.00000000}.{...}"); "" = the
// runtime gave none. Once, in the probe.
void fetchAudioDeviceIds(XrInstance inst, std::string& out_id, std::string& in_id)
{
    const LLXrFns& f = LLXrLoader::fn();
    const char* names[2] = {"xrGetAudioOutputDeviceGuidOculus", "xrGetAudioInputDeviceGuidOculus"};
    std::string* dst[2] = {&out_id, &in_id};
    for (int i = 0; i < 2; ++i)
    {
        PFN_xrVoidFunction fn = nullptr;
        if (!f.getInstanceProcAddr || XR_FAILED(f.getInstanceProcAddr(inst, names[i], &fn)) || !fn)
        {
            LL_INFOS("VRAudio") << names[i] << ": not resolved" << LL_ENDL;
            continue;
        }
        wchar_t buf[kXrAudioDeviceStrSize] = {};
        const XrResult r = reinterpret_cast<PFN_slGetAudioDeviceGuid>(fn)(inst, buf);
        buf[kXrAudioDeviceStrSize - 1] = 0;
        if (XR_SUCCEEDED(r)) *dst[i] = wideToUtf8(buf);
        LL_INFOS("VRAudio") << names[i] << " -> " << llxrResultText(inst, r) << " '" << *dst[i] << "'" << LL_ENDL;
    }
}
#endif

const char* stateName(XrSessionState s)
{
    switch (s)
    {
    case XR_SESSION_STATE_IDLE: return "IDLE";
    case XR_SESSION_STATE_READY: return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
    case XR_SESSION_STATE_STOPPING: return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING: return "EXITING";
    default: return "UNKNOWN";
    }
}

const char* formatName(int64_t f)
{
    switch (f)
    {
    case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
    case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM_PACK32";
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return "A2R10G10B10_UNORM_PACK32";
    case VK_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
    case VK_FORMAT_R32G32B32A32_SFLOAT: return "R32G32B32A32_SFLOAT";
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return "B10G11R11_UFLOAT_PACK32";
    case VK_FORMAT_D16_UNORM: return "D16_UNORM";
    case VK_FORMAT_X8_D24_UNORM_PACK32: return "X8_D24_UNORM_PACK32";
    case VK_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
    case VK_FORMAT_D32_SFLOAT: return "D32_SFLOAT";
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return "D32_SFLOAT_S8_UINT";
    default: return "?";
    }
}

// The UNORM view format of an sRGB swapchain format (UNDEFINED: none).
VkFormat unormSibling(VkFormat f)
{
    switch (f)
    {
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}

// The sRGB format with the window format's bytes (UNDEFINED: none): the UI swapchain takes the
// window image's sRGB-encoded bytes by a bit-exact copy and the compositor decodes them.
VkFormat srgbSibling(VkFormat f)
{
    switch (f)
    {
    case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    default: return VK_FORMAT_UNDEFINED;
    }
}

std::string vkVersionText(uint32_t v)
{
    return std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) + "." +
           std::to_string(VK_API_VERSION_PATCH(v));
}

std::string joinList(const std::vector<std::string>& v)
{
    std::string s;
    for (const std::string& e : v) s += (s.empty() ? "" : ", ") + e;
    return s.empty() ? std::string("(none)") : s;
}

bool hasName(const std::vector<std::string>& v, const char* n)
{
    return std::find(v.begin(), v.end(), std::string(n)) != v.end();
}

int chainLength(const void* p)
{
    int n = 0;
    for (const VkBaseInStructure* s = (const VkBaseInStructure*)p; s; s = s->pNext) ++n;
    return n;
}

// ---------------------------------------------------------------------------------------------
// Spy: the runtime calls vkCreateInstance / vkCreateDevice through the pfnGetInstanceProcAddr we
// hand it. Log what it finally passes to the driver: our list plus what it appended (or dropped).
// ---------------------------------------------------------------------------------------------
std::vector<std::string> sSpyInstExts, sSpyDevExts;

void spyDiff(const char* what, const std::vector<std::string>& requested, uint32_t n, const char* const* names)
{
    std::vector<std::string> finalList(names, names + n);
    std::vector<std::string> added, dropped;
    for (const std::string& e : finalList)
        if (!hasName(requested, e.c_str())) added.push_back(e);
    for (const std::string& e : requested)
        if (!hasName(finalList, e.c_str())) dropped.push_back(e);
    LL_INFOS("VR") << "runtime passes " << n << " " << what << " extensions to the driver (we asked for "
                   << requested.size() << "); ADDED: " << joinList(added) << "; DROPPED: " << joinList(dropped) << LL_ENDL;
}

VKAPI_ATTR VkResult VKAPI_CALL spyCreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* alloc,
                                                 VkInstance* out)
{
    LL_INFOS("VR") << "runtime calls vkCreateInstance: apiVersion "
                   << (ci->pApplicationInfo ? vkVersionText(ci->pApplicationInfo->apiVersion) : std::string("(no app info)"))
                   << ", " << ci->enabledLayerCount << " layers" << LL_ENDL;
    spyDiff("instance", sSpyInstExts, ci->enabledExtensionCount, ci->ppEnabledExtensionNames);
    const Clock::time_point t = Clock::now();
    const VkResult r = vkCreateInstance(ci, alloc, out);
    LL_INFOS("VR") << "driver vkCreateInstance -> " << vkResultString(r) << " (" << msSince(t) << " ms)" << LL_ENDL;
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL spyCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
                                               const VkAllocationCallbacks* alloc, VkDevice* out)
{
    std::string queues, chain;
    for (uint32_t i = 0; i < ci->queueCreateInfoCount; ++i)
        queues += (queues.empty() ? "" : ", ") + std::string("family ") + std::to_string(ci->pQueueCreateInfos[i].queueFamilyIndex) +
                  " x" + std::to_string(ci->pQueueCreateInfos[i].queueCount);
    for (const VkBaseInStructure* s = (const VkBaseInStructure*)ci->pNext; s; s = s->pNext)
        chain += (chain.empty() ? "" : " ") + std::to_string((int)s->sType);
    LL_INFOS("VR") << "runtime calls vkCreateDevice: queues " << queues << "; pNext chain of " << chainLength(ci->pNext)
                   << " structs (sTypes " << chain << ")" << (ci->pEnabledFeatures ? "; NOTE: pEnabledFeatures set" : "")
                   << LL_ENDL;
    spyDiff("device", sSpyDevExts, ci->enabledExtensionCount, ci->ppEnabledExtensionNames);
    const Clock::time_point t = Clock::now();
    const VkResult r = vkCreateDevice(pd, ci, alloc, out);
    LL_INFOS("VR") << "driver vkCreateDevice -> " << vkResultString(r) << " (" << msSince(t) << " ms)" << LL_ENDL;
    return r;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL spyGetInstanceProcAddr(VkInstance inst, const char* name)
{
    if (std::strcmp(name, "vkCreateInstance") == 0) return (PFN_vkVoidFunction)&spyCreateInstance;
    if (std::strcmp(name, "vkCreateDevice") == 0) return (PFN_vkVoidFunction)&spyCreateDevice;
    return vkGetInstanceProcAddr(inst, name);
}

// ---------------------------------------------------------------------------------------------
// Crash handler ownership. SteamVR's client installs its own in-process crash handler (breakpad)
// while xrCreateInstance loads it (spike #1079). Ours must keep owning crashes (the viewer's
// crash report + markers): snapshot the handlers before a call into the runtime, put ours back
// if the runtime replaced them, and log it.
// ---------------------------------------------------------------------------------------------
class CrashHandlerGuard
{
public:
    explicit CrashHandlerGuard(const char* step) : mStep(step)
    {
#if LL_WINDOWS
        mFilter = SetUnhandledExceptionFilter(nullptr);
        SetUnhandledExceptionFilter(mFilter);
#else
        for (size_t i = 0; i < kCount; ++i) sigaction(kSignals[i], nullptr, &mActs[i]);
#endif
    }
    ~CrashHandlerGuard()
    {
        std::string changed;
#if LL_WINDOWS
        const LPTOP_LEVEL_EXCEPTION_FILTER now = SetUnhandledExceptionFilter(mFilter);
        if (now != mFilter) changed = "the unhandled-exception filter";
#else
        for (size_t i = 0; i < kCount; ++i)
        {
            struct sigaction now;
            if (sigaction(kSignals[i], nullptr, &now) != 0) continue;
            if (now.sa_handler != mActs[i].sa_handler || now.sa_sigaction != mActs[i].sa_sigaction)
            {
                changed += (changed.empty() ? "" : " ") + std::string(kNames[i]);
                sigaction(kSignals[i], &mActs[i], nullptr);
            }
        }
#endif
        if (changed.empty())
            LL_INFOS("VR") << "crash handler check after " << mStep << ": ours is still installed" << LL_ENDL;
        else
            LL_WARNS("VR") << "crash handler check after " << mStep << ": the OpenXR runtime replaced " << changed
                           << "; the viewer's crash handler is restored (it owns crashes)" << LL_ENDL;
    }

private:
    const char* mStep;
#if LL_WINDOWS
    LPTOP_LEVEL_EXCEPTION_FILTER mFilter = nullptr;
#else
    static constexpr size_t kCount = 7;
    static constexpr int kSignals[kCount] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGSYS, SIGTRAP};
    static constexpr const char* kNames[kCount] = {"SIGSEGV", "SIGBUS", "SIGILL", "SIGFPE", "SIGABRT", "SIGSYS", "SIGTRAP"};
    struct sigaction mActs[kCount];
#endif
};

struct XrSwap
{
    XrSwapchain handle = XR_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    VkFormat imageFormat = VK_FORMAT_UNDEFINED;
    VkFormat viewFormat = VK_FORMAT_UNDEFINED;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;   // per image, per layer: views[image * layers + layer]
    uint32_t acquired = UINT32_MAX;   // image index acquired this frame (UINT32_MAX: none)
    uint32_t layers = 1;              // #1075: array layers (2: the stereo eye array swapchain)
};

void xrBarrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
               VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
               VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
               uint32_t layer = 0, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT)
{
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, 0, 1, layer, 1};   // (#1075: one array layer; 0 for every M1 image)
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}
} // namespace

struct LLXrSession::Impl
{
    Config cfg;
    std::thread worker;
    bool probeOk = false;
    std::string probeWhy;
    bool probeLoaderMissing = false;   // #1401: the loader library itself failed to load

    XrInstance inst = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    std::string runtimeText;      // #1078 "SteamVR/OpenXR 2.x.y" (About / bug reports / crash metadata)
    std::string systemName;       // #1078 the headset system's name
    uint32_t maxSwapW = 0, maxSwapH = 0;
    std::vector<XrViewConfigurationView> views;
    XrEnvironmentBlendMode blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    bool localFloorExt = false;   // #1074 XR_EXT_local_floor enabled on the instance
    bool cylinderExt = false;     // #1076 XR_KHR_composition_layer_cylinder enabled on the instance
    bool depthExt = false;        // #1075 XR_KHR_composition_layer_depth enabled (stereo sessions only)
    bool visMaskExt = false;      // #1075 XR_KHR_visibility_mask enabled (stereo sessions only)
    bool audioGuidExt = false;    // #1122 XR_OCULUS_audio_device_guid enabled (Windows)
    std::string runtimeName;      // #1122 XrInstanceProperties::runtimeName
    // #1122 the headset's audio endpoints as the runtime names them (XR_OCULUS_audio_device_guid:
    // WASAPI endpoint ids, UTF-8; "" = not offered). Written by the probe (worker), read after it.
    std::string audioOutId, audioInId;
    // #1075 the hidden-area mesh per eye (fetchVisibilityMask): a triangle list, x y per vertex,
    // in the eye's tangent space (the view space's z = -1 plane). visMaskDirty: (re)fetch at the
    // next pollEvents (session created, XrEventDataVisibilityMaskChangedKHR); visMaskGen bumps
    // with every fetch.
    std::vector<float> visMask[2];
    bool visMaskDirty = false;
    uint32_t visMaskGen = 0;

    // Vulkan objects (created through the runtime).
    VkInstance vkInst = VK_NULL_HANDLE;
    VkPhysicalDevice vkPhys = VK_NULL_HANDLE;   // xrGetVulkanGraphicsDevice2KHR
    VkDevice vkDev = VK_NULL_HANDLE;
    bool deviceByXr = false;

    // Session.
    XrSession session = XR_NULL_HANDLE;
    // #1074 the app space the eyes are located and submitted in: LOCAL_FLOOR, else STAGE,
    // else LOCAL (M0: LOCAL).
    XrSpace appSpace = XR_NULL_HANDLE;
    XrReferenceSpaceType appSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XrSpace viewSpace = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    std::vector<int64_t> formats;
    XrSwap eyes[2];
    uint32_t eyeCount = 0;
    // #1075 LAYERED STEREO (Config::stereo): stereoOn = the eye swapchains were made for one layer
    // per eye; eyeArray = eyes[0] is ONE array swapchain with a layer per eye (eyes[1] unused),
    // else one swapchain per eye. depth = the depth swapchain (same shape as the eyes) for the
    // depth layer; depthInfo chained into the projection views of a frame that copied depth.
    bool stereoOn = false;
    bool eyeArray = false;
    XrSwap depth[2];
    bool depthRecorded = false;
    bool depthWarned = false;
    XrCompositionLayerDepthInfoKHR depthInfo[2] = {{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR},
                                                   {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR}};
    XrSwap ui;
    bool uiFailed = false;   // no UI quad this session (logged once)
    uint32_t uiPendingW = 0, uiPendingH = 0, uiResizeFrames = 0;   // a window size waiting to settle

    // Frame.
    bool frameBegun = false;
    bool recorded = false;     // eyes rendered: the projection layer goes out
    bool uiRecorded = false;   // UI copied: the quad layer goes out
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    XrCompositionLayerProjectionView projViews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                     {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    uint32_t uiCopyW = 0, uiCopyH = 0;
    bool uiAlpha = false;      // #1076 the UI swapchain holds the UI-only image (premultiplied alpha)
    XrSwap fade;               // #1076 the comfort fade: a tiny image cleared to (0, 0, 0, a) per frame
    bool fadeFailed = false;
    bool fadeRecorded = false;
    // #1078 the comfort vignette: kVigSize^2 premultiplied black with a radial alpha (clear in
    // the middle), head-locked, while the view moves without your head. Swapchain + a
    // device-local staging buffer the gradient is written into (vkCmdUpdateBuffer, only when the
    // strength changes), both made on the first vignette and kept for the session.
    VkDeviceContext* dev = nullptr;
    XrSwap vig;
    GpuBuffer vigBuf;
    bool vigFailed = false;
    bool vigRecorded = false;
    int vigBuiltLevel = -1;    // the quantized strength the buffer holds (-1: none)
    // #1076 the login room: the doorway and mist images (static swapchains, written once from a
    // host-visible staging buffer kept for the session: fixed ~400 KB, VR only).
    XrSwap door, mist;
    GpuBuffer lobbyBuf;
    bool lobbyFailed = false;
    bool lobbyWritten = false;
    bool lobbyRecorded = false;
    XrRect2Di panelRect{};     // #1134 the floating menu panel's rect, clamped to the copied image
    // #1120 the wrist panel's own swapchain (R8G8B8A8_SRGB, else B8G8R8A8_SRGB: LLVRWrist then
    // swaps its colours), the size of LLVRWrist's image; made on the first copy. wristContent:
    // an image with the panel was released into it (the layer may go out from then on).
    XrSwap wrist;
    bool wristFailed = false;
    bool wristContent = false;

    // #1074 views located for this frame (waitFrame) and the pose / frustum derived from them.
    XrView located[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    bool locatedOk = false;         // this frame's views are usable for the layer (orientation valid)
    LLXrHeadPose head;              // held across frames where tracking is lost
    XrFovf renderFov{};             // the rendered symmetric frustum (declared in the layer)
    uint32_t renderW = 0, renderH = 0;   // the eye swapchain size (both eyes)
    uint32_t spaceGeneration = 0;
    bool worldFormatWarned = false;
    bool renderFovLogged = false;

    // #1076 input: one action set, both hands as subaction paths.
    XrActionSet actionSet = XR_NULL_HANDLE;
    XrAction actGrip = XR_NULL_HANDLE, actAim = XR_NULL_HANDLE;
    XrAction actSelect = XR_NULL_HANDLE, actContext = XR_NULL_HANDLE, actMenu = XR_NULL_HANDLE;
    XrAction actFlyCam = XR_NULL_HANDLE, actStick = XR_NULL_HANDLE, actStickClick = XR_NULL_HANDLE;
    XrAction actSelectValue = XR_NULL_HANDLE;   // the trigger's analogue value (select = click || value)
    XrAction actSqueeze = XR_NULL_HANDLE;       // #1143 the grip's squeeze value (grab)
    bool squeezeOn[2] = {false, false};         // per hand: the squeeze's hysteresis state
    bool squeezeLogged[2] = {false, false};     // per hand: the first squeeze edge was logged
    bool selectByValue[2] = {false, false};     // per hand: the value's hysteresis state
    bool selectLogged[2] = {false, false};      // per hand: the first select edge was logged
    bool selectBindLogged[2] = {false, false};  // per hand: which select sources are bound (logged once)
    XrAction actHaptic = XR_NULL_HANDLE;
    XrPath handPath[LLXR_HAND_COUNT] = {XR_NULL_PATH, XR_NULL_PATH};
    XrSpace gripSpace[LLXR_HAND_COUNT] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrSpace aimSpace[LLXR_HAND_COUNT] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    bool actionsAttached = false;
    LLXrInputState input;

    // Stats (logged every 10 s while running).
    Clock::time_point renderStart{};
    Clock::time_point lastWaitEnd{};
    Clock::time_point statsStart{};
    uint64_t frames = 0, rendered = 0, statFrames = 0;
    double waitDeltaSum = 0.0, waitDeltaMin = 1e9, waitDeltaMax = 0.0;
    bool firstWaitLogged = false, posesLogged = false;
    // #1076 pacing for the safety governor (waitFrame).
    XrTime lastPredicted = 0;
    uint32_t missedSlots = 0;
    bool timingValid = false;
    // #1137 PACING (LLXrPacing, the 10 s line). frameWaited: xrWaitFrame returned for this frame
    // and it is owed an xrBeginFrame (late begin: not yet) + xrEndFrame.
    bool frameWaited = false;
    Clock::time_point frameStart{};    // this frame's xrWaitFrame return
    XrDuration nativePeriod = 0;       // the shortest predictedDisplayPeriod seen this session
    uint32_t missedNative = 0;
    bool throttled = false;
    Clock::time_point throttleSince{};
    float blockedMs = 0.f, slotWaitMs = 0.f;     // this frame's
    float lastCpuMs = 0.f, lastSlotMs = 0.f;     // the previous complete frame's
    float gpuMs = -1.f;
    // The 10 s window: fixed rings (no growth; > 10 s at 144 Hz -- older frames of a longer
    // window are overwritten) for the percentiles, sums for the rest.
    static constexpr uint32_t kPaceRing = 1536;
    float ringCpu[kPaceRing] = {}, ringSlot[kPaceRing] = {}, ringBlocked[kPaceRing] = {};
    uint32_t ringN = 0;
    uint64_t statMissedNative = 0, statThrottled = 0, statLate = 0;
    double statGpuSum = 0.0;
    float statGpuMax = 0.f;
    uint32_t statGpuN = 0;
    // #1076 governor: our own frame's CPU span, xrBeginFrame's return to xrEndFrame (it includes
    // any wait on the GPU inside the frame, so a GPU-bound frame shows here too; not the time
    // xrWaitFrame blocked). workSec: the last ended frame's (0: none yet).
    Clock::time_point workStart{};
    double workSec = 0.0;
    int endFailLogs = 0;
    // #1076 diagnostics: the placement / debug mode whose layers were last logged.
    uint32_t layersLoggedSerial = UINT32_MAX;
    uint32_t layersLoggedMode = UINT32_MAX;
};

LLXrSession& LLXrSession::instance()
{
    static LLXrSession s;
    return s;
}

LLXrSession::~LLXrSession()
{
    // A probe still running at process exit (startup aborted before the join).
    if (mImpl && mImpl->worker.joinable()) mImpl->worker.join();
}

void LLXrSession::fail(const std::string& why, bool notice)
{
    sActive = false;
    if (!mFailure.empty()) return;   // the first reason is the one that matters
    mFailure = why;
    LL_WARNS("VR") << "VR off for this session: " << why << ". The desktop continues." << LL_ENDL;
    if (notice && mImpl && mImpl->probeLoaderMissing)
        mNotice = "VR could not start: " + why + ".\n\nSlipstream is running on the desktop.";
    else if (notice)
        mNotice = "VR could not start: " + why + ".\n\nSlipstream is running on the desktop. Check that the "
                  "headset is connected and switched on and that your VR runtime (for example SteamVR) is "
                  "running, then use Start in VR on the login screen again.";
}

bool LLXrSession::sessionRunning() const
{
    return mImpl && mImpl->session != XR_NULL_HANDLE && mImpl->running;
}

namespace
{
const std::string kNoXrString;
}

const std::string& LLXrSession::runtimeName() const
{
    return mImpl ? mImpl->runtimeName : kNoXrString;
}

const std::string& LLXrSession::headsetAudioOutputId() const
{
    return mImpl ? mImpl->audioOutId : kNoXrString;
}

const std::string& LLXrSession::headsetAudioInputId() const
{
    return mImpl ? mImpl->audioInId : kNoXrString;
}

std::string LLXrSession::takeNotice()
{
    std::string s;
    s.swap(mNotice);
    return s;
}

// ---------------------------------------------------------------------------------------------
// 1. Probe (worker): loader, instance, system, requirements.
// ---------------------------------------------------------------------------------------------
void LLXrSession::startProbe(const Config& cfg)
{
    if (mImpl) return;
    mImpl = new Impl();
    mImpl->cfg = cfg;
    sActive = true;
    LL_INFOS("VR") << "VR requested (SlipstreamVREnabled / --vr): probing the OpenXR runtime on a worker" << LL_ENDL;
    mImpl->worker = std::thread([this]() { probe(); });
}

bool LLXrSession::finishProbe()
{
    if (!mImpl) return false;
    if (mImpl->worker.joinable())
    {
        const Clock::time_point t = Clock::now();
        mImpl->worker.join();
        const double waited = msSince(t);
        if (waited > 50.0) LL_INFOS("VR") << "waited " << waited << " ms for the OpenXR probe" << LL_ENDL;
    }
    if (!mImpl->probeOk)
    {
        // Nothing Vulkan exists yet: the context is created exactly as on the desktop.
        if (mImpl->inst != XR_NULL_HANDLE && LLXrLoader::fn().destroyInstance)
            LLXrLoader::fn().destroyInstance(mImpl->inst);
        mImpl->inst = XR_NULL_HANDLE;
        LLXrLoader::forgetInstance();
        fail(mImpl->probeWhy.empty() ? std::string("the OpenXR probe failed") : mImpl->probeWhy);
        return false;
    }
    sActive = true;
    return true;
}

void LLXrSession::probe()
{
    Impl& m = *mImpl;
    const Clock::time_point t0 = Clock::now();
    std::string why;
    if (!LLXrLoader::load(m.cfg.viewerLibDir, why))
    {
        m.probeWhy = why;
        m.probeLoaderMissing = true;
        return;
    }
    const LLXrFns& f = LLXrLoader::fn();
    LL_INFOS("VR") << "OpenXR loader: " << LLXrLoader::libraryPath() << LL_ENDL;
    if (f.enumerateApiLayerProperties)
    {
        uint32_t n = 0;
        f.enumerateApiLayerProperties(0, &n, nullptr);
        std::vector<XrApiLayerProperties> layers(n, {XR_TYPE_API_LAYER_PROPERTIES});
        if (n) f.enumerateApiLayerProperties(n, &n, layers.data());
        LL_INFOS("VR") << "OpenXR API layers available: " << n << LL_ENDL;
        for (uint32_t i = 0; i < n; ++i) LL_INFOS("VR") << "  layer " << layers[i].layerName << " (" << layers[i].description << ")" << LL_ENDL;
    }
    std::vector<std::string> exts;
    {
        const Clock::time_point t = Clock::now();
        uint32_t n = 0;
        XrResult r = f.enumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
        if (XR_FAILED(r))
        {
            LL_WARNS("VR") << "xrEnumerateInstanceExtensionProperties failed: " << llxrResultText(XR_NULL_HANDLE, r)
                           << " (" << (int)r << ")" << LL_ENDL;
            m.probeWhy = "no OpenXR runtime is installed or enabled (" + llxrResultText(XR_NULL_HANDLE, r) +
                         "); install SteamVR, the Oculus / Meta app or Windows Mixed Reality and set it as "
                         "the active OpenXR runtime";
            return;
        }
        std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
        r = f.enumerateInstanceExtensionProperties(nullptr, n, &n, props.data());
        if (XR_FAILED(r))
        {
            m.probeWhy = "the OpenXR runtime could not list its extensions (" + llxrResultText(XR_NULL_HANDLE, r) + ")";
            return;
        }
        LL_INFOS("VR") << "runtime instance extensions: " << n << " (" << msSince(t) << " ms)" << LL_ENDL;
        for (uint32_t i = 0; i < n; ++i)
        {
            exts.push_back(props[i].extensionName);
            LL_INFOS("VR") << "  " << props[i].extensionName << " v" << props[i].extensionVersion << LL_ENDL;
        }
    }
    if (!hasName(exts, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME))
    {
        m.probeWhy = std::string("the OpenXR runtime does not offer ") + XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME +
                     " (Vulkan rendering)";
        return;
    }
    // #1074: XR_EXT_local_floor (the LOCAL_FLOOR reference space: seated/standing origin on
    // the floor) where the runtime offers it; optional.
    // #1076: XR_KHR_composition_layer_cylinder (the curved UI wall; without it the wall is
    // built from flat quads along the same arc); optional.
    // #1075: XR_KHR_composition_layer_depth (the world's depth beside the eye images: positional
    // reprojection) -- stereo sessions only (a mono render declared at the eyes' positions has
    // no depth that matches its declaration); optional.
    // #1075: XR_KHR_visibility_mask (the lens' hidden area per eye: those pixels are never shown,
    // the stereo world rejects them before shading) -- stereo sessions only (the mono render's
    // image is not either eye's projection); optional.
    const char* enable[6] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, nullptr, nullptr, nullptr, nullptr, nullptr};
    uint32_t enableCount = 1;
#if LL_WINDOWS
    // #1122: XR_OCULUS_audio_device_guid (the headset's audio output / microphone endpoints;
    // the Oculus / Meta runtime and some streaming runtimes); optional, Windows only.
    if (hasName(exts, kXrOculusAudioDeviceGuidExt))
    {
        enable[enableCount++] = kXrOculusAudioDeviceGuidExt;
        m.audioGuidExt = true;
    }
#endif
    if (m.cfg.stereo && hasName(exts, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME))
    {
        enable[enableCount++] = XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME;
        m.depthExt = true;
    }
    if (m.cfg.stereo && hasName(exts, XR_KHR_VISIBILITY_MASK_EXTENSION_NAME))
    {
        enable[enableCount++] = XR_KHR_VISIBILITY_MASK_EXTENSION_NAME;
        m.visMaskExt = true;
    }
    if (hasName(exts, XR_EXT_LOCAL_FLOOR_EXTENSION_NAME))
    {
        enable[enableCount++] = XR_EXT_LOCAL_FLOOR_EXTENSION_NAME;
        m.localFloorExt = true;
    }
    if (hasName(exts, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME))
    {
        enable[enableCount++] = XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME;
        m.cylinderExt = true;
    }
    else
    {
        LL_INFOS("VR") << "the runtime offers no " << XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME
                       << ": the UI wall is built from " << kWallFacets << " flat quads along its arc" << LL_ENDL;
    }
    for (uint32_t i = 0; i < enableCount; ++i) LL_INFOS("VR") << "enabling XR extension " << enable[i] << LL_ENDL;

    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    std::snprintf(ici.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "Slipstream");
    ici.applicationInfo.applicationVersion = 1;
    std::snprintf(ici.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "slipstream-vk");
    ici.applicationInfo.engineVersion = 1;
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;   // accepted by every 1.x runtime
    ici.enabledExtensionCount = enableCount;
    ici.enabledExtensionNames = enable;
    {
        LL_INFOS("VR") << "xrCreateInstance ... (application version " << m.cfg.appVersion << ")" << LL_ENDL;
        const Clock::time_point t = Clock::now();
        XrResult r;
        {
            CrashHandlerGuard guard("xrCreateInstance");
            r = f.createInstance(&ici, &m.inst);
        }
        LL_INFOS("VR") << "xrCreateInstance -> " << llxrResultText(XR_NULL_HANDLE, r) << " (" << msSince(t) << " ms)" << LL_ENDL;
        if (XR_FAILED(r) && enableCount > 1)
        {
            // #1074 / #1076: the optional extensions are never worth losing VR over.
            ici.enabledExtensionCount = 1;
            m.localFloorExt = false;
            m.cylinderExt = false;
            m.depthExt = false;
            m.visMaskExt = false;
            m.audioGuidExt = false;
            m.inst = XR_NULL_HANDLE;
            {
                CrashHandlerGuard guard("xrCreateInstance (retry)");
                r = f.createInstance(&ici, &m.inst);
            }
            LL_WARNS("VR") << "xrCreateInstance retry without the optional extensions -> "
                           << llxrResultText(XR_NULL_HANDLE, r) << LL_ENDL;
        }
        if (XR_FAILED(r))
        {
            m.inst = XR_NULL_HANDLE;
            LL_WARNS("VR") << "xrCreateInstance failed: " << llxrResultText(XR_NULL_HANDLE, r) << " (" << (int)r << ")" << LL_ENDL;
            m.probeWhy = r == XR_ERROR_RUNTIME_UNAVAILABLE
                ? "no OpenXR runtime is installed or enabled (xrCreateInstance: " + llxrResultText(XR_NULL_HANDLE, r) +
                      "); install SteamVR, the Oculus / Meta app or Windows Mixed Reality and set it as the active OpenXR runtime"
                : "the OpenXR runtime did not start (xrCreateInstance: " + llxrResultText(XR_NULL_HANDLE, r) + ")";
            return;
        }
    }
    if (!LLXrLoader::loadInstance(m.inst, why))
    {
        m.probeWhy = why;
        return;
    }
    {
        XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
        if (XR_SUCCEEDED(f.getInstanceProperties(m.inst, &ip)))
        {
            LL_INFOS("VR") << "runtime: " << ip.runtimeName << " " << llxrVersionText(ip.runtimeVersion) << LL_ENDL;
            m.runtimeName = ip.runtimeName;
            m.runtimeText = std::string(ip.runtimeName) + " " + llxrVersionText(ip.runtimeVersion);   // #1078 (About)
        }
    }
#if LL_WINDOWS
    if (m.audioGuidExt) fetchAudioDeviceIds(m.inst, m.audioOutId, m.audioInId);   // #1122
#endif

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    {
        const XrResult r = f.getSystem(m.inst, &sgi, &m.system);
        LL_INFOS("VR") << "xrGetSystem(HEAD_MOUNTED_DISPLAY) -> " << llxrResultText(m.inst, r) << LL_ENDL;
        if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE)
        {
            m.probeWhy = "no headset found (the VR runtime reports none connected or switched on)";
            return;
        }
        if (XR_FAILED(r))
        {
            m.probeWhy = "the VR runtime could not find a headset (xrGetSystem: " + llxrResultText(m.inst, r) + ")";
            return;
        }
    }
    {
        XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
        if (XR_SUCCEEDED(f.getSystemProperties(m.inst, m.system, &sp)))
        {
            m.maxSwapW = sp.graphicsProperties.maxSwapchainImageWidth;
            m.maxSwapH = sp.graphicsProperties.maxSwapchainImageHeight;
            m.systemName = sp.systemName;   // #1078 (About)
            char vendor[16];
            std::snprintf(vendor, sizeof(vendor), "0x%x", (unsigned)sp.vendorId);
            LL_INFOS("VR") << "system: \"" << sp.systemName << "\" vendor " << vendor << ", max swapchain " << m.maxSwapW
                           << "x" << m.maxSwapH << ", max layers " << sp.graphicsProperties.maxLayerCount
                           << ", orientation tracking " << (int)sp.trackingProperties.orientationTracking
                           << ", position tracking " << (int)sp.trackingProperties.positionTracking << LL_ENDL;
        }
    }
    const XrViewConfigurationType vcType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    {
        uint32_t n = 0;
        bool stereo = false;
        if (XR_SUCCEEDED(f.enumerateViewConfigurations(m.inst, m.system, 0, &n, nullptr)) && n)
        {
            std::vector<XrViewConfigurationType> vct(n);
            f.enumerateViewConfigurations(m.inst, m.system, n, &n, vct.data());
            for (XrViewConfigurationType v : vct)
            {
                LL_INFOS("VR") << "view configuration offered: " << (int)v
                               << (v == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO ? " (PRIMARY_STEREO)" : "") << LL_ENDL;
                stereo |= v == vcType;
            }
        }
        if (!stereo)
        {
            m.probeWhy = "the headset does not offer a stereo view configuration";
            return;
        }
        XrViewConfigurationProperties vp{XR_TYPE_VIEW_CONFIGURATION_PROPERTIES};
        if (XR_SUCCEEDED(f.getViewConfigurationProperties(m.inst, m.system, vcType, &vp)))
            LL_INFOS("VR") << "PRIMARY_STEREO: fovMutable " << (int)vp.fovMutable << LL_ENDL;
        n = 0;
        f.enumerateViewConfigurationViews(m.inst, m.system, vcType, 0, &n, nullptr);
        if (n < 2)
        {
            m.probeWhy = "the headset reports " + std::to_string(n) + " views for stereo";
            return;
        }
        m.views.assign(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        f.enumerateViewConfigurationViews(m.inst, m.system, vcType, n, &n, m.views.data());
        for (uint32_t i = 0; i < n; ++i)
            LL_INFOS("VR") << "eye " << i << ": recommended " << m.views[i].recommendedImageRectWidth << "x"
                           << m.views[i].recommendedImageRectHeight << " (max " << m.views[i].maxImageRectWidth << "x"
                           << m.views[i].maxImageRectHeight << "), samples recommended "
                           << m.views[i].recommendedSwapchainSampleCount << " max " << m.views[i].maxSwapchainSampleCount
                           << LL_ENDL;
    }
    {
        uint32_t n = 0;
        if (XR_SUCCEEDED(f.enumerateEnvironmentBlendModes(m.inst, m.system, vcType, 0, &n, nullptr)) && n)
        {
            std::vector<XrEnvironmentBlendMode> modes(n);
            f.enumerateEnvironmentBlendModes(m.inst, m.system, vcType, n, &n, modes.data());
            for (XrEnvironmentBlendMode b : modes) LL_INFOS("VR") << "blend mode offered: " << (int)b << LL_ENDL;
            m.blend = modes[0];
        }
    }
    {
        // Must be asked before xrCreateVulkanInstanceKHR (spec).
        XrGraphicsRequirementsVulkan2KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
        const XrResult r = f.getVulkanGraphicsRequirements2(m.inst, m.system, &req);
        if (XR_FAILED(r))
        {
            m.probeWhy = "the VR runtime gave no Vulkan requirements (" + llxrResultText(m.inst, r) + ")";
            return;
        }
        LL_INFOS("VR") << "Vulkan requirements: min API " << llxrVersionText(req.minApiVersionSupported)
                       << ", max known API " << llxrVersionText(req.maxApiVersionSupported)
                       << " (the viewer creates 1.3; above the max KNOWN version is allowed)" << LL_ENDL;
        if (XR_MAKE_VERSION(1, 3, 0) < req.minApiVersionSupported)
        {
            m.probeWhy = "the VR runtime needs Vulkan " + llxrVersionText(req.minApiVersionSupported) + " or newer";
            return;
        }
    }
    LL_INFOS("VR") << "OpenXR probe done in " << msSince(t0) << " ms: the runtime will create the Vulkan instance and device" << LL_ENDL;
    m.probeOk = true;
}

// ---------------------------------------------------------------------------------------------
// 2. Device hooks (main thread, inside VkDeviceContext::initForPresent).
// ---------------------------------------------------------------------------------------------
void LLXrSession::installDeviceHooks(VkDeviceContext& dev)
{
    if (!sActive || !mImpl || !mImpl->probeOk) return;
    VkDeviceContext::XrHooks h;
    h.createInstance = [this](const VkInstanceCreateInfo& ci, VkInstance* out) { return hookCreateInstance(ci, out); };
    h.physicalDevice = [this](VkInstance inst) { return hookPhysicalDevice(inst); };
    h.refuse = [this](const std::string& why) { fail(why); };
    h.createDevice = [this](VkPhysicalDevice pd, const VkDeviceCreateInfo& ci, VkDevice* out) { return hookCreateDevice(pd, ci, out); };
    dev.setXrHooks(std::move(h));
    LL_INFOS("VR") << "Vulkan instance / device creation goes through the OpenXR runtime (XR_KHR_vulkan_enable2)" << LL_ENDL;
}

VkResult LLXrSession::hookCreateInstance(const VkInstanceCreateInfo& ci, VkInstance* out)
{
    Impl& m = *mImpl;
    if (failed() || m.inst == XR_NULL_HANDLE) return vkCreateInstance(&ci, nullptr, out);
    sSpyInstExts.clear();
    for (uint32_t i = 0; i < ci.enabledExtensionCount; ++i) sSpyInstExts.push_back(ci.ppEnabledExtensionNames[i]);
    LL_INFOS("VR") << "viewer instance request: API " << (ci.pApplicationInfo ? vkVersionText(ci.pApplicationInfo->apiVersion) : std::string("?"))
                   << ", " << ci.enabledLayerCount << " layers, extensions: " << joinList(sSpyInstExts) << LL_ENDL;
    XrVulkanInstanceCreateInfoKHR xci{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xci.systemId = m.system;
    xci.pfnGetInstanceProcAddr = &spyGetInstanceProcAddr;
    xci.vulkanCreateInfo = &ci;
    VkResult vr = VK_SUCCESS;
    const Clock::time_point t = Clock::now();
    XrResult r;
    {
        CrashHandlerGuard guard("xrCreateVulkanInstanceKHR");
        r = LLXrLoader::fn().createVulkanInstance(m.inst, &xci, out, &vr);
    }
    LL_INFOS("VR") << "xrCreateVulkanInstanceKHR -> " << llxrResultText(m.inst, r) << ", vulkanResult " << vkResultString(vr)
                   << " (" << msSince(t) << " ms)" << LL_ENDL;
    if (XR_SUCCEEDED(r) && vr == VK_SUCCESS)
    {
        m.vkInst = *out;
        return VK_SUCCESS;
    }
    *out = VK_NULL_HANDLE;
    fail("the VR runtime could not create the Vulkan instance (" + llxrResultText(m.inst, r) + ", " + vkResultString(vr) + ")");
    return vkCreateInstance(&ci, nullptr, out);   // desktop: the plain call with the same request
}

VkPhysicalDevice LLXrSession::hookPhysicalDevice(VkInstance inst)
{
    Impl& m = *mImpl;
    if (failed() || m.inst == XR_NULL_HANDLE || inst != m.vkInst) return VK_NULL_HANDLE;
    XrVulkanGraphicsDeviceGetInfoKHR gi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    gi.systemId = m.system;
    gi.vulkanInstance = inst;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    const XrResult r = LLXrLoader::fn().getVulkanGraphicsDevice2(m.inst, &gi, &pd);
    if (XR_FAILED(r) || pd == VK_NULL_HANDLE)
    {
        LL_WARNS("VR") << "xrGetVulkanGraphicsDevice2KHR -> " << llxrResultText(m.inst, r) << LL_ENDL;
        return VK_NULL_HANDLE;
    }
    VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &idp;
    vkGetPhysicalDeviceProperties2(pd, &p2);
    char uuid[2 * VK_UUID_SIZE + 1] = {};
    for (uint32_t k = 0; k < VK_UUID_SIZE; ++k) std::snprintf(uuid + 2 * k, 3, "%02x", idp.deviceUUID[k]);
    LL_INFOS("VR") << "xrGetVulkanGraphicsDevice2KHR: '" << p2.properties.deviceName << "' (Vulkan "
                   << vkVersionText(p2.properties.apiVersion) << ", uuid " << uuid
                   << "): this GPU is used (it overrides the automatic choice and RenderVKDeviceSelect)" << LL_ENDL;
    m.vkPhys = pd;
    return pd;
}

VkResult LLXrSession::hookCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo& ci, VkDevice* out)
{
    Impl& m = *mImpl;
    if (!failed() && pd != m.vkPhys) fail("the device was not created on the GPU the VR runtime requires");
    if (failed() || m.inst == XR_NULL_HANDLE) return vkCreateDevice(pd, &ci, nullptr, out);
    sSpyDevExts.clear();
    for (uint32_t i = 0; i < ci.enabledExtensionCount; ++i) sSpyDevExts.push_back(ci.ppEnabledExtensionNames[i]);
    int multiview = -1;
    for (const VkBaseInStructure* s = (const VkBaseInStructure*)ci.pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES)
            multiview = (int)reinterpret_cast<const VkPhysicalDeviceVulkan11Features*>(s)->multiview;
    LL_INFOS("VR") << "viewer device request: " << ci.enabledExtensionCount << " extensions (" << joinList(sSpyDevExts)
                   << "), pNext chain of " << chainLength(ci.pNext) << " structs, multiview " << multiview << ", "
                   << ci.queueCreateInfoCount << " queue families" << LL_ENDL;
    XrVulkanDeviceCreateInfoKHR xdi{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xdi.systemId = m.system;
    xdi.pfnGetInstanceProcAddr = &spyGetInstanceProcAddr;
    xdi.vulkanPhysicalDevice = pd;
    xdi.vulkanCreateInfo = &ci;
    VkResult vr = VK_SUCCESS;
    const Clock::time_point t = Clock::now();
    XrResult r;
    {
        CrashHandlerGuard guard("xrCreateVulkanDeviceKHR");
        r = LLXrLoader::fn().createVulkanDevice(m.inst, &xdi, out, &vr);
    }
    LL_INFOS("VR") << "xrCreateVulkanDeviceKHR -> " << llxrResultText(m.inst, r) << ", vulkanResult " << vkResultString(vr)
                   << " (" << msSince(t) << " ms)" << LL_ENDL;
    if (XR_SUCCEEDED(r) && vr == VK_SUCCESS)
    {
        m.vkDev = *out;
        m.deviceByXr = true;
        return VK_SUCCESS;
    }
    // Control: the identical request straight to the driver. If the driver refuses it too, the
    // request is the problem: return that, and the caller's retry (without the optional
    // extensions) comes back here through the runtime. If the driver takes it, the runtime
    // refused our device: VR is off and the desktop continues on this device.
    *out = VK_NULL_HANDLE;
    const VkResult cr = vkCreateDevice(pd, &ci, nullptr, out);
    LL_INFOS("VR") << "CONTROL vkCreateDevice with the identical create info -> " << vkResultString(cr) << LL_ENDL;
    if (cr == VK_SUCCESS)
        fail("the VR runtime refused the Vulkan device (" + llxrResultText(m.inst, r) + ", " + vkResultString(vr) + ")");
    return cr;
}

// ---------------------------------------------------------------------------------------------
// 3. Session (main thread, after the device).
// ---------------------------------------------------------------------------------------------
bool LLXrSession::createSession(VkDeviceContext& dev)
{
    if (!mImpl || failed()) return false;
    Impl& m = *mImpl;
    if (!m.deviceByXr || dev.device() != m.vkDev || dev.instance() != m.vkInst)
    {
        fail("the Vulkan device was not created by the VR runtime");
        return false;
    }
    const LLXrFns& f = LLXrLoader::fn();
    m.dev = &dev;   // #1078 (the vignette's staging buffer)
    XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    binding.instance = dev.instance();
    binding.physicalDevice = dev.physicalDevice();
    binding.device = dev.device();
    binding.queueFamilyIndex = dev.queueFamily();
    binding.queueIndex = 0;   // the viewer's graphics queue: every submit is on the main thread, as are the XR calls
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = m.system;
    {
        const Clock::time_point t = Clock::now();
        XrResult r;
        {
            CrashHandlerGuard guard("xrCreateSession");
            r = f.createSession(m.inst, &sci, &m.session);
        }
        LL_INFOS("VR") << "xrCreateSession (graphics queue family " << binding.queueFamilyIndex << ", index 0) -> "
                       << llxrResultText(m.inst, r) << " (" << msSince(t) << " ms)" << LL_ENDL;
        if (XR_FAILED(r))
        {
            m.session = XR_NULL_HANDLE;
            fail("the VR session could not be created (" + llxrResultText(m.inst, r) + ")");
            return false;
        }
    }
    {
        std::vector<XrReferenceSpaceType> st;
        uint32_t n = 0;
        if (XR_SUCCEEDED(f.enumerateReferenceSpaces(m.session, 0, &n, nullptr)) && n)
        {
            st.resize(n);
            f.enumerateReferenceSpaces(m.session, n, &n, st.data());
            st.resize(n);
            for (XrReferenceSpaceType s : st)
                LL_INFOS("VR") << "reference space offered: " << (int)s
                               << (s == XR_REFERENCE_SPACE_TYPE_VIEW ? " (VIEW)" : s == XR_REFERENCE_SPACE_TYPE_LOCAL ? " (LOCAL)"
                                   : s == XR_REFERENCE_SPACE_TYPE_STAGE ? " (STAGE)"
                                   : s == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR ? " (LOCAL_FLOOR)" : "") << LL_ENDL;
        }
        auto spaceOffered = [&](XrReferenceSpaceType t) { return std::find(st.begin(), st.end(), t) != st.end(); };
        // #1074: the eyes' app space -- LOCAL_FLOOR (the extension), else STAGE (room scale,
        // floor origin), else LOCAL (head height at the start; LLVRCamera adds the configured
        // eye height). The camera works relative to a recentre reference, so all three behave
        // the same apart from where "the floor" is.
        std::vector<XrReferenceSpaceType> order;
        if (m.localFloorExt && spaceOffered(XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR)) order.push_back(XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR);
        if (spaceOffered(XR_REFERENCE_SPACE_TYPE_STAGE)) order.push_back(XR_REFERENCE_SPACE_TYPE_STAGE);
        order.push_back(XR_REFERENCE_SPACE_TYPE_LOCAL);   // required of every runtime
        XrReferenceSpaceCreateInfo rci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        rci.poseInReferenceSpace.orientation.w = 1.0f;
        XrResult r = XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
        for (XrReferenceSpaceType t : order)
        {
            rci.referenceSpaceType = t;
            r = f.createReferenceSpace(m.session, &rci, &m.appSpace);
            if (XR_SUCCEEDED(r))
            {
                m.appSpaceType = t;
                break;
            }
            m.appSpace = XR_NULL_HANDLE;
            LL_WARNS("VR") << "xrCreateReferenceSpace(" << (int)t << ") -> " << llxrResultText(m.inst, r) << LL_ENDL;
        }
        if (XR_SUCCEEDED(r))
        {
            rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;   // the head-locked UI quad
            r = f.createReferenceSpace(m.session, &rci, &m.viewSpace);
        }
        if (XR_FAILED(r))
        {
            destroySessionObjects();
            fail("the VR reference spaces could not be created (" + llxrResultText(m.inst, r) + ")");
            return false;
        }
        m.head.floorSpace = m.appSpaceType != XR_REFERENCE_SPACE_TYPE_LOCAL;
        LL_INFOS("VR") << "using reference space "
                       << (m.appSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR ? "LOCAL_FLOOR"
                           : m.appSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE ? "STAGE" : "LOCAL")
                       << " (eyes, head tracking) and VIEW (UI quad)" << LL_ENDL;
    }
    // #1076: controllers. A failure here leaves VR without controller input (logged), never
    // without VR: the head-tracked world and the desktop keyboard / mouse keep working.
    createActions();
    {
        uint32_t n = 0;
        f.enumerateSwapchainFormats(m.session, 0, &n, nullptr);
        m.formats.resize(n);
        if (n) f.enumerateSwapchainFormats(m.session, n, &n, m.formats.data());
        LL_INFOS("VR") << "runtime swapchain formats (runtime preference order): " << n << LL_ENDL;
        for (int64_t fm : m.formats) LL_INFOS("VR") << "  " << (long long)fm << " " << formatName(fm) << LL_ENDL;
    }
    auto offered = [&](VkFormat fm) { return std::find(m.formats.begin(), m.formats.end(), (int64_t)fm) != m.formats.end(); };
    // B8G8R8A8_SRGB first: its UNORM sibling is the viewer's window format, so the final / UI
    // pipelines compiled for the window can render into the eye views as they are (M1+).
    VkFormat scFormat = VK_FORMAT_UNDEFINED;
    for (VkFormat fm : {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB})
        if (scFormat == VK_FORMAT_UNDEFINED && offered(fm)) scFormat = fm;
    if (scFormat == VK_FORMAT_UNDEFINED)
    {
        destroySessionObjects();
        fail("the VR runtime offers no 8-bit sRGB swapchain format");
        return false;
    }
    m.eyeCount = std::min<uint32_t>(2u, (uint32_t)m.views.size());
    {
        // #1074: one image goes to both eyes, so both swapchains get one size: the larger
        // recommendation x SlipstreamVRRenderScale, within the runtime's maxima (and 8192).
        uint32_t recW = 0, recH = 0, maxW = 8192, maxH = 8192;
        for (uint32_t i = 0; i < m.eyeCount; ++i)
        {
            recW = std::max(recW, m.views[i].recommendedImageRectWidth);
            recH = std::max(recH, m.views[i].recommendedImageRectHeight);
            if (m.views[i].maxImageRectWidth) maxW = std::min(maxW, m.views[i].maxImageRectWidth);
            if (m.views[i].maxImageRectHeight) maxH = std::min(maxH, m.views[i].maxImageRectHeight);
        }
        if (m.maxSwapW) maxW = std::min(maxW, m.maxSwapW);
        if (m.maxSwapH) maxH = std::min(maxH, m.maxSwapH);
        const float scale = std::isfinite(m.cfg.renderScale) ? std::clamp(m.cfg.renderScale, 0.25f, 2.f) : 1.f;
        m.renderW = std::clamp((uint32_t)std::lround(recW * scale), 64u, std::max(64u, maxW));
        m.renderH = std::clamp((uint32_t)std::lround(recH * scale), 64u, std::max(64u, maxH));
        LL_INFOS("VR") << "eye render size " << m.renderW << "x" << m.renderH << " (recommended " << recW << "x" << recH
                       << " x SlipstreamVRRenderScale " << scale << ", max " << maxW << "x" << maxH << ")" << LL_ENDL;
    }
    // #1075 LAYERED STEREO: one 2-layer ARRAY swapchain for both eyes (layer v = eye v, the
    // world's layer v copied in), where the runtime takes it; else one swapchain per eye as M1.
    m.stereoOn = m.cfg.stereo && m.eyeCount == 2u;
    bool tryArray = m.stereoOn;
    for (uint32_t i = 0; i < m.eyeCount; ++i)
    {
        if (m.eyeArray && i > 0) break;   // (the array swapchain holds every eye)
        XrSwap& e = m.eyes[i];
        e.width = m.renderW;
        e.height = m.renderH;
        e.imageFormat = scFormat;
        e.layers = (tryArray && i == 0) ? m.eyeCount : 1u;
        const VkFormat sib = unormSibling(scFormat);
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                        XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
        ci.format = scFormat;
        ci.sampleCount = 1;
        ci.width = e.width;
        ci.height = e.height;
        ci.faceCount = 1;
        ci.arraySize = e.layers;
        ci.mipCount = 1;
        XrResult r = f.createSwapchain(m.session, &ci, &e.handle);
        LL_INFOS("VR") << "xrCreateSwapchain eye " << (e.layers > 1u ? "array (both eyes)" : std::to_string(i)) << " "
                       << e.width << "x" << e.height << " x" << e.layers << " " << formatName(scFormat)
                       << " (MUTABLE_FORMAT, UNORM views) -> " << llxrResultText(m.inst, r) << LL_ENDL;
        if (XR_FAILED(r) && e.layers > 1u)
        {
            // #1075: no array swapchain: one per eye (copies go to layer 0 of eye v's).
            tryArray = false;
            e.handle = XR_NULL_HANDLE;
            --i;
            continue;
        }
        if (e.layers > 1u) m.eyeArray = true;
        bool mutableOk = XR_SUCCEEDED(r);
        if (XR_FAILED(r))
        {
            // Without MUTABLE_FORMAT the views are sRGB: the hardware encodes on write. Fine for
            // the M0 test colours; M1 then needs an sRGB-target variant of the final pass.
            ci.usageFlags &= ~(XrSwapchainUsageFlags)XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
            r = f.createSwapchain(m.session, &ci, &e.handle);
            LL_WARNS("VR") << "  retry WITHOUT MUTABLE_FORMAT -> " << llxrResultText(m.inst, r)
                           << " (the final passes will need an sRGB-target variant for the world, M1)" << LL_ENDL;
        }
        if (XR_FAILED(r))
        {
            e.handle = XR_NULL_HANDLE;
            destroySessionObjects();
            fail("the VR eye swapchains could not be created (" + llxrResultText(m.inst, r) + ")");
            return false;
        }
        e.viewFormat = mutableOk ? sib : scFormat;
        uint32_t n = 0;
        f.enumerateSwapchainImages(e.handle, 0, &n, nullptr);
        std::vector<XrSwapchainImageVulkan2KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
        if (n) f.enumerateSwapchainImages(e.handle, n, &n, (XrSwapchainImageBaseHeader*)imgs.data());
        for (uint32_t k = 0; k < n; ++k)
        {
            e.images.push_back(imgs[k].image);
            for (uint32_t l = 0; l < e.layers; ++l)   // (#1075: one 2D view per array layer)
            {
                VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                vi.image = imgs[k].image;
                vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
                vi.format = e.viewFormat;
                vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, l, 1};
                VkImageView v = VK_NULL_HANDLE;
                const VkResult vr = vkCreateImageView(m.vkDev, &vi, nullptr, &v);
                if (vr != VK_SUCCESS)
                {
                    destroySessionObjects();
                    fail(std::string("the VR eye image views could not be created (") + formatName(e.viewFormat) + " on " +
                         formatName(scFormat) + ": " + vkResultString(vr) + ")");
                    return false;
                }
                e.views.push_back(v);
            }
        }
        LL_INFOS("VR") << "  eye " << i << ": " << n << " images x " << e.layers << " layers, views as "
                       << formatName(e.viewFormat)
                       << (mutableOk ? " (UNORM view of an sRGB image: the viewer's sRGB-encoded bytes pass through)" : "")
                       << LL_ENDL;
    }
    // #1075: the DEPTH swapchain(s) for the depth layer, same shape as the eyes, in a depth format
    // the runtime offers that the world's depth copies into bit for bit (D32_SFLOAT; else none).
    if (m.stereoOn && m.depthExt)
    {
        const bool d32 = std::find(m.formats.begin(), m.formats.end(), (int64_t)VK_FORMAT_D32_SFLOAT) != m.formats.end();
        if (!d32)
            LL_INFOS("VR") << "no depth layer: the runtime offers no D32_SFLOAT swapchain format" << LL_ENDL;
        for (uint32_t i = 0; d32 && i < (m.eyeArray ? 1u : m.eyeCount); ++i)
        {
            XrSwap& d = m.depth[i];
            XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            ci.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            ci.format = VK_FORMAT_D32_SFLOAT;
            ci.sampleCount = 1;
            ci.width = m.renderW;
            ci.height = m.renderH;
            ci.faceCount = 1;
            ci.arraySize = m.eyeArray ? m.eyeCount : 1u;
            ci.mipCount = 1;
            const XrResult r = f.createSwapchain(m.session, &ci, &d.handle);
            LL_INFOS("VR") << "xrCreateSwapchain depth " << i << " " << ci.width << "x" << ci.height << " x" << ci.arraySize
                           << " D32_SFLOAT -> " << llxrResultText(m.inst, r) << LL_ENDL;
            if (XR_FAILED(r))
            {
                d.handle = XR_NULL_HANDLE;
                for (XrSwap& o : m.depth)
                {
                    if (o.handle != XR_NULL_HANDLE) f.destroySwapchain(o.handle);
                    o = XrSwap{};
                }
                break;   // (no depth layer: the eyes go out without it)
            }
            d.width = m.renderW;
            d.height = m.renderH;
            d.imageFormat = d.viewFormat = VK_FORMAT_D32_SFLOAT;
            d.layers = ci.arraySize;
            uint32_t n = 0;
            f.enumerateSwapchainImages(d.handle, 0, &n, nullptr);
            std::vector<XrSwapchainImageVulkan2KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
            if (n) f.enumerateSwapchainImages(d.handle, n, &n, (XrSwapchainImageBaseHeader*)imgs.data());
            for (uint32_t k = 0; k < n; ++k) d.images.push_back(imgs[k].image);
        }
    }
    LL_INFOS("VR") << "eyes: " << (m.stereoOn ? (m.eyeArray ? "STEREO, one array swapchain" : "STEREO, one swapchain per eye")
                                              : "mono (M1: one image into both eyes)")
                   << (m.depth[0].handle != XR_NULL_HANDLE ? ", depth layer on" : "") << LL_ENDL;
    // #1075: the hidden-area meshes are fetched by the first pollEvents (stereo + the extension).
    m.visMaskDirty = m.stereoOn && m.visMaskExt;
    if (m.stereoOn && !m.visMaskExt)
        LL_INFOS("VR") << "the runtime offers no " << XR_KHR_VISIBILITY_MASK_EXTENSION_NAME
                       << ": every eye pixel is shaded (no hidden-area mask)" << LL_ENDL;
    LL_INFOS("VR") << "VR session created; waiting for the runtime to make it READY (SteamVR can take a while the first time)" << LL_ENDL;
    return true;
}

// The UI quad's swapchain: the window format's sRGB sibling at the window size. After a window
// resize the overlapping part is copied until the new size has held for kUiResizeFrames frames
// (no swapchain churn while a window edge is dragged); then the swapchain is made again.
bool LLXrSession::ensureUiSwapchain(VkFormat windowFormat, VkExtent2D windowExtent)
{
    Impl& m = *mImpl;
    if (m.uiFailed) return false;
    uint32_t w = std::max(1u, windowExtent.width), h = std::max(1u, windowExtent.height);
    if (m.maxSwapW) w = std::min(w, m.maxSwapW);
    if (m.maxSwapH) h = std::min(h, m.maxSwapH);
    if (m.ui.handle != XR_NULL_HANDLE)
    {
        if (w == m.ui.width && h == m.ui.height)
        {
            m.uiResizeFrames = 0;
            return true;
        }
        static const uint32_t kUiResizeFrames = 30;
        if (w != m.uiPendingW || h != m.uiPendingH)
        {
            m.uiPendingW = w;
            m.uiPendingH = h;
            m.uiResizeFrames = 0;
        }
        if (++m.uiResizeFrames < kUiResizeFrames) return true;   // copy the overlap meanwhile
        // Rare (a settled window resize): wait for our copies into the old images, then remake it.
        LL_INFOS("VR") << "UI quad: window now " << w << "x" << h << ", recreating the UI swapchain (was "
                       << m.ui.width << "x" << m.ui.height << ")" << LL_ENDL;
        if (m.vkDev) vkDeviceWaitIdle(m.vkDev);
        LLXrLoader::fn().destroySwapchain(m.ui.handle);
        m.ui = XrSwap{};
        m.uiResizeFrames = 0;
    }
    const VkFormat fm = srgbSibling(windowFormat);
    if (fm == VK_FORMAT_UNDEFINED || std::find(m.formats.begin(), m.formats.end(), (int64_t)fm) == m.formats.end())
    {
        m.uiFailed = true;
        LL_WARNS("VR") << "no UI quad: the window format " << formatName(windowFormat) << " (" << (int)windowFormat
                       << ") has no sRGB swapchain format the runtime offers" << LL_ENDL;
        return false;
    }
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = fm;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    const XrResult r = LLXrLoader::fn().createSwapchain(m.session, &ci, &m.ui.handle);
    LL_INFOS("VR") << "xrCreateSwapchain UI quad " << w << "x" << h << " " << formatName(fm) << " -> "
                   << llxrResultText(m.inst, r) << LL_ENDL;
    if (XR_FAILED(r))
    {
        m.ui.handle = XR_NULL_HANDLE;
        m.uiFailed = true;
        return false;
    }
    m.ui.width = w;
    m.ui.height = h;
    m.ui.imageFormat = m.ui.viewFormat = fm;
    uint32_t n = 0;
    LLXrLoader::fn().enumerateSwapchainImages(m.ui.handle, 0, &n, nullptr);
    std::vector<XrSwapchainImageVulkan2KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    if (n) LLXrLoader::fn().enumerateSwapchainImages(m.ui.handle, n, &n, (XrSwapchainImageBaseHeader*)imgs.data());
    for (uint32_t k = 0; k < n; ++k) m.ui.images.push_back(imgs[k].image);
    return !m.ui.images.empty();
}

// ---------------------------------------------------------------------------------------------
// 4. Events + frame loop (main thread).
// ---------------------------------------------------------------------------------------------
void LLXrSession::setRunning(bool on)
{
    Impl& m = *mImpl;
    if (m.running == on) return;
    m.running = on;
    if (on)
    {
        m.renderStart = m.statsStart = Clock::now();
        m.lastWaitEnd = Clock::time_point{};
        m.statFrames = 0;
        m.waitDeltaSum = 0.0;
        m.waitDeltaMin = 1e9;
        m.waitDeltaMax = 0.0;
        m.firstWaitLogged = false;
        m.lastPredicted = 0;   // #1076 (no gap across a session restart)
        // #1137: the native period is relearned per session (the user may switch the panel rate)
        m.nativePeriod = 0;
        m.throttled = false;
        m.ringN = 0;
        m.statMissedNative = m.statThrottled = m.statLate = 0;
        m.statGpuSum = 0.0; m.statGpuMax = 0.f; m.statGpuN = 0;
        m.gpuMs = -1.f;
    }
    if (mRunningCb) mRunningCb(on);
}

void LLXrSession::pollEvents()
{
    Impl& m = *mImpl;
    const LLXrFns& f = LLXrLoader::fn();
    if (m.inst == XR_NULL_HANDLE || !f.pollEvent) return;
    bool destroy = false;
    std::string why;
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (f.pollEvent(m.inst, &ev) == XR_SUCCESS)
    {
        switch (ev.type)
        {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
        {
            const XrEventDataSessionStateChanged& sc = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev);
            if (sc.session != m.session) break;
            LL_INFOS("VR") << "session state " << stateName(m.state) << " -> " << stateName(sc.state) << LL_ENDL;
            m.state = sc.state;
            if (sc.state == XR_SESSION_STATE_READY)
            {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                const XrResult r = f.beginSession(m.session, &bi);
                LL_INFOS("VR") << "xrBeginSession -> " << llxrResultText(m.inst, r) << LL_ENDL;
                if (XR_SUCCEEDED(r)) setRunning(true);
            }
            else if (sc.state == XR_SESSION_STATE_STOPPING)
            {
                const XrResult r = f.endSession(m.session);
                LL_INFOS("VR") << "xrEndSession -> " << llxrResultText(m.inst, r)
                               << " (headset idle: the desktop paces itself again)" << LL_ENDL;
                setRunning(false);
            }
            else if (sc.state == XR_SESSION_STATE_EXITING || sc.state == XR_SESSION_STATE_LOSS_PENDING)
            {
                destroy = true;
                why = sc.state == XR_SESSION_STATE_EXITING ? "the VR runtime closed the session"
                                                           : "the VR runtime lost the session";
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            LL_WARNS("VR") << "event: instance loss pending" << LL_ENDL;
            destroy = true;
            why = "the VR runtime is shutting down";
            break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
        {
            // #1074: the runtime re-centred a space (e.g. the user reset the view in the VR
            // dashboard). Ours: LLVRCamera re-takes its recentre reference.
            const XrEventDataReferenceSpaceChangePending& rc =
                *reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(&ev);
            const bool ours = rc.session == m.session && rc.referenceSpaceType == m.appSpaceType;
            LL_INFOS("VR") << "event: reference space " << (int)rc.referenceSpaceType << " change pending (recenter)"
                           << (ours ? ": the app space, the camera re-centres" : "") << LL_ENDL;
            if (ours) ++m.spaceGeneration;
            break;
        }
        case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
            LL_INFOS("VR") << "event: interaction profile changed" << LL_ENDL;
            logInteractionProfiles();   // #1076
            break;
        case XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR:
        {
            // #1075: the lens' hidden area changed (e.g. the runtime changed the FOV): re-fetch.
            const XrEventDataVisibilityMaskChangedKHR& vc =
                *reinterpret_cast<const XrEventDataVisibilityMaskChangedKHR*>(&ev);
            LL_INFOS("VR") << "event: visibility mask changed (view " << vc.viewIndex << ")" << LL_ENDL;
            if (vc.session == m.session) m.visMaskDirty = m.stereoOn && m.visMaskExt;
            break;
        }
        case XR_TYPE_EVENT_DATA_EVENTS_LOST:
            LL_WARNS("VR") << "event: events lost ("
                           << reinterpret_cast<const XrEventDataEventsLost*>(&ev)->lostEventCount << ")" << LL_ENDL;
            break;
        default:
            LL_INFOS("VR") << "event: type " << (int)ev.type << LL_ENDL;
            break;
        }
        ev = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }
    if (destroy && m.session != XR_NULL_HANDLE)
    {
        LL_WARNS("VR") << why << ": VR session ended; the desktop continues" << LL_ENDL;
        setRunning(false);
        if (m.vkDev) vkDeviceWaitIdle(m.vkDev);   // (between frames: nothing of ours in flight after this)
        destroySessionObjects();
        mNotice = "The VR session ended (" + why + "). Slipstream continues on the desktop; restart it in VR to go back.";
    }
    if (m.visMaskDirty && m.session != XR_NULL_HANDLE) fetchVisibilityMask();
}

// #1075 VISIBILITY MASK (XR_KHR_visibility_mask): each eye's hidden-area triangle mesh, flattened
// to a triangle list of x y pairs. The vertices are in the eye's view space on the z = -1 plane
// (x = tangent of the horizontal angle, y up = tangent of the vertical one), so the renderer maps
// them with the eye's own projection. An eye without a mask (zero vertices: legal) or a failed
// call leaves that eye's list empty: nothing is masked there.
void LLXrSession::fetchVisibilityMask()
{
    Impl& m = *mImpl;
    const LLXrFns& f = LLXrLoader::fn();
    m.visMaskDirty = false;
    for (std::vector<float>& v : m.visMask) v.clear();
    if (f.getVisibilityMask && m.stereoOn)
    {
        for (uint32_t eye = 0; eye < 2u; ++eye)
        {
            XrVisibilityMaskKHR vm{XR_TYPE_VISIBILITY_MASK_KHR};
            XrResult r = f.getVisibilityMask(m.session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eye,
                                             XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &vm);
            std::vector<XrVector2f> verts;
            std::vector<uint32_t> idx;
            if (XR_SUCCEEDED(r) && vm.vertexCountOutput && vm.indexCountOutput)
            {
                verts.resize(vm.vertexCountOutput);
                idx.resize(vm.indexCountOutput);
                vm.vertexCapacityInput = (uint32_t)verts.size();
                vm.vertices = verts.data();
                vm.indexCapacityInput = (uint32_t)idx.size();
                vm.indices = idx.data();
                r = f.getVisibilityMask(m.session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eye,
                                        XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &vm);
            }
            if (XR_FAILED(r))
            {
                LL_WARNS("VR") << "xrGetVisibilityMaskKHR eye " << eye << " -> " << llxrResultText(m.inst, r)
                               << ": no hidden-area mask for that eye" << LL_ENDL;
                continue;
            }
            const uint32_t nv = std::min<uint32_t>(vm.vertexCountOutput, (uint32_t)verts.size());
            const uint32_t ni = std::min<uint32_t>(vm.indexCountOutput, (uint32_t)idx.size()) / 3u * 3u;
            std::vector<float>& out = m.visMask[eye];
            out.reserve((size_t)ni * 2u);
            float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
            for (uint32_t i = 0; i < ni; ++i)
            {
                const uint32_t k = idx[i];
                if (k >= nv || !std::isfinite(verts[k].x) || !std::isfinite(verts[k].y))
                {
                    LL_WARNS("VR") << "hidden-area mesh eye " << eye << ": bad vertex " << k << " (" << nv
                                   << " vertices): no mask for that eye" << LL_ENDL;
                    out.clear();
                    break;
                }
                out.push_back(verts[k].x);
                out.push_back(verts[k].y);
                lo[0] = std::min(lo[0], verts[k].x); hi[0] = std::max(hi[0], verts[k].x);
                lo[1] = std::min(lo[1], verts[k].y); hi[1] = std::max(hi[1], verts[k].y);
            }
            // (x / y spanning about the eye's FOV tangents, not +-1, confirms the tangent-space reading)
            LL_INFOS("VR") << "hidden-area mesh eye " << eye << ": " << nv << " vertices, " << out.size() / 6u
                           << " triangles; x " << (out.empty() ? 0.f : lo[0]) << " .. " << (out.empty() ? 0.f : hi[0])
                           << ", y " << (out.empty() ? 0.f : lo[1]) << " .. " << (out.empty() ? 0.f : hi[1]) << LL_ENDL;
        }
    }
    ++m.visMaskGen;
}

uint32_t LLXrSession::visibilityMaskGeneration() const
{
    return mImpl ? mImpl->visMaskGen : 0u;
}

const std::vector<float>& LLXrSession::visibilityMask(uint32_t eye) const
{
    static const std::vector<float> s_none;
    return (mImpl && eye < 2u) ? mImpl->visMask[eye] : s_none;
}

bool LLXrSession::waitFrame()
{
    mCameraApplied = false;   // #1074: set again by LLVRCamera for this frame
    if (!mImpl || mImpl->session == XR_NULL_HANDLE) return false;
    Impl& m = *mImpl;
    // a frame left open by an early-out: end it first (zero layers; a late-begin frame is begun there)
    if (m.frameBegun || m.frameWaited) endFrameNow(false);
    pollEvents();
    if (!m.running || m.session == XR_NULL_HANDLE) return false;
    const LLXrFns& f = LLXrLoader::fn();
    XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
    m.fs = XrFrameState{XR_TYPE_FRAME_STATE};
    const Clock::time_point w0 = Clock::now();
    XrResult r = f.waitFrame(m.session, &fwi, &m.fs);
    const Clock::time_point w1 = Clock::now();
    if (XR_FAILED(r))
    {
        LL_WARNS("VR") << "xrWaitFrame -> " << llxrResultText(m.inst, r) << LL_ENDL;
        return false;
    }
    // #1137: every waited frame is owed an xrBeginFrame + xrEndFrame (endFrameNow keeps that
    // promise, beginning it there if nothing did before). Late begin (A/B): begun by recordFrame
    // / endFrameNow, right before the frame's GPU work -- xrLocateViews and xrSyncActions below
    // need no begun frame.
    m.frameWaited = true;
    m.frameStart = w1;
    m.workStart = w1;   // #1076 governor: our frame's CPU span starts here (late begin too)
    m.blockedMs = (float)std::chrono::duration<double, std::milli>(w1 - w0).count();
    m.slotWaitMs = 0.f;
    m.recorded = m.uiRecorded = false;
    if (!mLateBegin && !ensureBegun()) return false;
    // #1076: display slots missed since the previous frame (the runtime predicts the next
    // display time it can still show; a gap of n periods = n - 1 missed slots).
    m.missedSlots = 0;
    m.missedNative = 0;
    m.timingValid = m.fs.predictedDisplayPeriod > 0;
    if (m.timingValid)
    {
        // #1137: the native period = the shortest one predicted this session (see LLXrPacing);
        // throttled = the runtime holds the app at over 1.5x of it. Logged on each edge.
        if (!m.nativePeriod || m.fs.predictedDisplayPeriod < m.nativePeriod) m.nativePeriod = m.fs.predictedDisplayPeriod;
        const bool thr = (double)m.fs.predictedDisplayPeriod > 1.5 * (double)m.nativePeriod;
        if (thr != m.throttled)
        {
            m.throttled = thr;
            const double held = std::chrono::duration<double>(w1 - m.throttleSince).count();
            m.throttleSince = w1;
            if (thr)
                LL_INFOS("VR") << "pacing: the runtime THROTTLES the app to " << 1e9 / (double)m.fs.predictedDisplayPeriod
                               << " Hz (display period " << m.fs.predictedDisplayPeriod / 1e6 << " ms, native "
                               << m.nativePeriod / 1e6 << " ms); last frame: GPU " << m.gpuMs << " ms (sampled), CPU "
                               << m.lastCpuMs << " ms (slot wait " << m.lastSlotMs << " ms)" << LL_ENDL;
            else
                LL_INFOS("VR") << "pacing: back at the native " << 1e9 / (double)m.nativePeriod << " Hz after " << held
                               << " s throttled; last frame: GPU " << m.gpuMs << " ms (sampled), CPU " << m.lastCpuMs
                               << " ms" << LL_ENDL;
        }
    }
    if (m.timingValid && m.lastPredicted && m.fs.predictedDisplayTime > m.lastPredicted)
    {
        const double gap = (double)(m.fs.predictedDisplayTime - m.lastPredicted);
        const double n = gap / (double)m.fs.predictedDisplayPeriod;
        m.missedSlots = (uint32_t)std::max(0.0, std::floor(n + 0.5) - 1.0);
        const double nn = gap / (double)m.nativePeriod;
        m.missedNative = (uint32_t)std::max(0.0, std::floor(nn + 0.5) - 1.0);
    }
    m.lastPredicted = m.fs.predictedDisplayTime;
    // #1074: the views for this frame's display time, before idle() moves the camera.
    locateViews();
    // #1076: the controllers for the same display time.
    syncInput();
    if (!m.firstWaitLogged)
    {
        m.firstWaitLogged = true;
        LL_INFOS("VR") << "first xrWaitFrame: blocked " << std::chrono::duration<double, std::milli>(w1 - w0).count()
                       << " ms, predictedDisplayPeriod " << m.fs.predictedDisplayPeriod / 1e6 << " ms ("
                       << (m.fs.predictedDisplayPeriod ? 1e9 / (double)m.fs.predictedDisplayPeriod : 0.0)
                       << " Hz), shouldRender " << (int)m.fs.shouldRender << LL_ENDL;
    }
    if (m.lastWaitEnd != Clock::time_point{})
    {
        const double dt = std::chrono::duration<double, std::milli>(w1 - m.lastWaitEnd).count();
        m.waitDeltaSum += dt;
        m.waitDeltaMin = std::min(m.waitDeltaMin, dt);
        m.waitDeltaMax = std::max(m.waitDeltaMax, dt);
        ++m.statFrames;
    }
    m.lastWaitEnd = w1;
    if (msSince(m.statsStart) > 10000.0 && m.statFrames)
    {
        LL_INFOS("VR") << "frames " << m.frames << " (" << m.rendered << " with layers), display period "
                       << m.fs.predictedDisplayPeriod / 1e6 << " ms; last 10 s: xrWaitFrame-to-xrWaitFrame avg "
                       << m.waitDeltaSum / (double)m.statFrames << " ms, min " << m.waitDeltaMin << ", max "
                       << m.waitDeltaMax << ", state " << stateName(m.state) << LL_ENDL;
        // #1137: slow or throttled, in one line. Percentiles over the frames of the window (the
        // ring's last kPaceRing); the GPU is the renderer's sampled world span.
        const uint32_t n = std::min(m.ringN, Impl::kPaceRing);
        if (n)
        {
            auto pct = [n](const float* ring, float q) {
                float tmp[Impl::kPaceRing];
                std::copy(ring, ring + n, tmp);
                const uint32_t k = std::min(n - 1u, (uint32_t)(q * (float)(n - 1u) + 0.5f));
                std::nth_element(tmp, tmp + k, tmp + n);
                return tmp[k];
            };
            const float cpu50 = pct(m.ringCpu, 0.5f), cpu90 = pct(m.ringCpu, 0.9f);
            const float slot50 = pct(m.ringSlot, 0.5f);
            const float blk50 = pct(m.ringBlocked, 0.5f), blk90 = pct(m.ringBlocked, 0.9f);
            const float gpuAvg = m.statGpuN ? (float)(m.statGpuSum / (double)m.statGpuN) : -1.f;
            const double nat = (double)m.nativePeriod / 1e6;
            const float work50 = std::max(0.f, cpu50 - slot50);   // CPU work: the slot wait is the GPU's
            const char* verdict = "on rate";
            if (gpuAvg > 0.f && gpuAvg > 0.95 * nat) verdict = "GPU-bound (the GPU frame is over the native period)";
            else if (work50 > 0.95 * nat) verdict = "CPU-bound (the main thread's work is over the native period)";
            else if (m.statThrottled * 2u > m.statFrames) verdict = "THROTTLED with headroom (the runtime holds a lower rate)";
            else if (m.statMissedNative * 20u > m.statFrames) verdict = "missing native slots (spikes)";
            LL_INFOS("VR") << "pacing (10 s, " << m.statFrames << " frames): native " << nat << " ms ("
                           << (nat > 0.0 ? 1000.0 / nat : 0.0) << " Hz), throttled " << m.statThrottled
                           << " frames, native slots missed " << m.statMissedNative << ", late xrBeginFrame "
                           << m.statLate << " frames; GPU (sampled) avg " << gpuAvg << " max " << m.statGpuMax
                           << " ms; CPU wait return -> xrEndFrame p50 " << cpu50 << " p90 " << cpu90
                           << " ms (frame-slot wait p50 " << slot50 << ", work p50 " << work50
                           << "); in xrWaitFrame p50 " << blk50 << " p90 " << blk90 << " ms -> " << verdict << LL_ENDL;
        }
        m.ringN = 0;
        m.statMissedNative = m.statThrottled = m.statLate = 0;
        m.statGpuSum = 0.0; m.statGpuMax = 0.f; m.statGpuN = 0;
        m.statsStart = Clock::now();
        m.statFrames = 0;
        m.waitDeltaSum = 0.0;
        m.waitDeltaMin = 1e9;
        m.waitDeltaMax = 0.0;
    }
    return true;
}

// #1074: both views for this frame's predicted display time, in the app space, right after
// xrBeginFrame (before idle() moves the camera): the camera and the projection layer use the
// same located poses. Derives the cyclopean head pose and the rendered symmetric frustum.
void LLXrSession::locateViews()
{
    Impl& m = *mImpl;
    m.locatedOk = false;
    m.head.tracked = false;
    m.head.headTracked = false;
    m.head.shown = m.state == XR_SESSION_STATE_VISIBLE || m.state == XR_SESSION_STATE_FOCUSED;
    m.head.renderWidth = m.renderW;
    m.head.renderHeight = m.renderH;
    m.head.spaceGeneration = m.spaceGeneration;
    if (m.eyeCount < 2 || m.appSpace == XR_NULL_HANDLE || !m.renderW || !m.renderH) return;
    const LLXrFns& f = LLXrLoader::fn();
    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = m.fs.predictedDisplayTime;
    vli.space = m.appSpace;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    m.located[0] = XrView{XR_TYPE_VIEW};
    m.located[1] = XrView{XR_TYPE_VIEW};
    uint32_t nv = 0;
    const XrResult lr = f.locateViews(m.session, &vli, &vs, 2, &nv, m.located);
    if (XR_FAILED(lr) || nv < 2 || !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) return;   // zero layers
    if (!m.posesLogged)
    {
        m.posesLogged = true;
        const double deg = 180.0 / 3.14159265358979;
        for (uint32_t i = 0; i < nv; ++i)
        {
            const XrPosef& p = m.located[i].pose;
            const XrFovf& fv = m.located[i].fov;
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "view %u: pos (%.3f %.3f %.3f) quat (%.3f %.3f %.3f %.3f) fov L%.1f R%.1f U%.1f D%.1f deg, flags 0x%llx",
                          i, p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y,
                          p.orientation.z, p.orientation.w, fv.angleLeft * deg, fv.angleRight * deg, fv.angleUp * deg,
                          fv.angleDown * deg, (unsigned long long)vs.viewStateFlags);
            LL_INFOS("VR") << buf << LL_ENDL;
        }
    }

    // Cyclopean head: the eye midpoint; orientation the eyes' average (the same quaternion on
    // headsets without canted displays). Position held while only orientation is tracked.
    const XrPosef& a = m.located[0].pose;
    const XrPosef& b = m.located[1].pose;
    float q[4] = {a.orientation.x, a.orientation.y, a.orientation.z, a.orientation.w};
    const float qb[4] = {b.orientation.x, b.orientation.y, b.orientation.z, b.orientation.w};
    const float sgn = (q[0] * qb[0] + q[1] * qb[1] + q[2] * qb[2] + q[3] * qb[3]) < 0.f ? -1.f : 1.f;
    for (int k = 0; k < 4; ++k) q[k] += sgn * qb[k];
    const float qlen = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(qlen > 1e-6f) || !std::isfinite(qlen)) return;
    for (int k = 0; k < 4; ++k) m.head.orientation[k] = q[k] / qlen;
    if ((vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) || !m.head.valid)
    {
        m.head.position[0] = 0.5f * (a.position.x + b.position.x);
        m.head.position[1] = 0.5f * (a.position.y + b.position.y);
        m.head.position[2] = 0.5f * (a.position.z + b.position.z);
    }
    // #1075: each eye's own pose and FOV (the stereo camera). While only orientation is tracked
    // the eyes keep their offsets from the (held) head position.
    {
        const bool posOk = (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
        for (uint32_t i = 0; i < 2; ++i)
        {
            const XrView& e = m.located[i];
            const float midX = 0.5f * (a.position.x + b.position.x), midY = 0.5f * (a.position.y + b.position.y),
                        midZ = 0.5f * (a.position.z + b.position.z);
            // (eye - midpoint) + the head position: the located eye itself when the position is valid
            m.head.eyePos[i][0] = m.head.position[0] + (posOk ? e.pose.position.x - midX : 0.f);
            m.head.eyePos[i][1] = m.head.position[1] + (posOk ? e.pose.position.y - midY : 0.f);
            m.head.eyePos[i][2] = m.head.position[2] + (posOk ? e.pose.position.z - midZ : 0.f);
            m.head.eyeOri[i][0] = e.pose.orientation.x;
            m.head.eyeOri[i][1] = e.pose.orientation.y;
            m.head.eyeOri[i][2] = e.pose.orientation.z;
            m.head.eyeOri[i][3] = e.pose.orientation.w;
            m.head.eyeFov[i][0] = e.fov.angleLeft;
            m.head.eyeFov[i][1] = e.fov.angleRight;
            m.head.eyeFov[i][2] = e.fov.angleUp;
            m.head.eyeFov[i][3] = e.fov.angleDown;
        }
        m.head.eyesValid = true;
    }

    // The ONE rendered image: a symmetric frustum that covers both eyes' FOV, at the eye image
    // aspect (wider than either eye alone; the compositor shows each eye its part).
    float tanX = 0.f, tanY = 0.f;
    for (uint32_t i = 0; i < 2; ++i)
    {
        const XrFovf& fv = m.located[i].fov;
        tanX = std::max(tanX, std::max(std::tan(-fv.angleLeft), std::tan(fv.angleRight)));
        tanY = std::max(tanY, std::max(std::tan(fv.angleUp), std::tan(-fv.angleDown)));
    }
    const float aspect = (float)m.renderW / (float)m.renderH;
    tanY = std::max(tanY, tanX / aspect);
    if (!std::isfinite(tanY)) return;
    tanY = std::clamp(tanY, 0.1f, 10.f);
    m.head.tanHalfY = tanY;
    m.head.aspect = aspect;
    const float ax = std::atan(tanY * aspect), ay = std::atan(tanY);
    m.renderFov.angleLeft = -ax;
    m.renderFov.angleRight = ax;
    m.renderFov.angleUp = ay;
    m.renderFov.angleDown = -ay;
    m.head.valid = true;
    m.head.tracked = true;
    const XrViewStateFlags trackedBits = XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
    m.head.headTracked = (vs.viewStateFlags & trackedBits) == trackedBits &&
                         (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
    m.locatedOk = true;
    if (!m.renderFovLogged)
    {
        m.renderFovLogged = true;
        const double deg = 180.0 / 3.14159265358979;
        LL_INFOS("VR") << "mono render frustum (covers both eyes): " << 2.0 * ax * deg << " x " << 2.0 * ay * deg
                       << " deg, aspect " << aspect << ", " << m.renderW << "x" << m.renderH << LL_ENDL;
    }
}

bool LLXrSession::headPose(LLXrHeadPose& out) const
{
    if (!mImpl || mImpl->session == XR_NULL_HANDLE || !mImpl->running || !mImpl->head.valid) return false;
    out = mImpl->head;
    return true;
}

bool LLXrSession::renderExtent(uint32_t& w, uint32_t& h) const
{
    if (!mImpl || mImpl->session == XR_NULL_HANDLE || !mImpl->renderW || !mImpl->renderH) return false;
    w = mImpl->renderW;
    h = mImpl->renderH;
    return true;
}

// ---------------------------------------------------------------------------------------------
// #1076 controllers: one action set, the two hands as subaction paths, suggested bindings for
// the Valve Index, Oculus Touch and the generic simple controller (the runtime picks the
// profile of what is connected; SteamVR also offers its own rebinding UI on top).
//
//   action        Index (valve/index_controller)   Touch (oculus/touch_controller)   simple
//   grip / aim    grip/pose, aim/pose              grip/pose, aim/pose               grip/pose, aim/pose
//   select        trigger/click (*)                -                                 select/click
//   select_value  trigger/value (*)                trigger/value                     -
//   context       a/click                          right a/click, left x/click       -
//   menu          left b/click                     left menu/click                   menu/click
//   flycam        right b/click                    right b/click                     -
//   stick         thumbstick                       thumbstick                        -
//   stick_click   thumbstick/click (left, held 1 s: recentre)  thumbstick/click  -
//   squeeze       squeeze/value (#1143 grab)       squeeze/value                     -
//   haptic        output/haptic                    output/haptic                     output/haptic
// (*) A click is select OR the trigger value past our own threshold (press 0.5, release 0.3).
//     The Index trigger's click component only closes at the very end of the pull (the first
//     headset run: "clicking doesn't work well"). The second run bound the BOOLEAN select to
//     trigger/value, leaving the float -> bool conversion to the runtime, and nothing clicked
//     (the pointer moved). The value now comes in as a FLOAT action with our own hysteresis
//     and the click component stays bound, so neither depends on that conversion; the first
//     select edge per hand is logged with both sources.
// ---------------------------------------------------------------------------------------------
void LLXrSession::createActions()
{
    Impl& m = *mImpl;
    const LLXrFns& f = LLXrLoader::fn();
    auto path = [&](const char* s) {
        XrPath p = XR_NULL_PATH;
        if (XR_FAILED(f.stringToPath(m.inst, s, &p))) p = XR_NULL_PATH;
        return p;
    };
    m.handPath[LLXR_HAND_LEFT] = path("/user/hand/left");
    m.handPath[LLXR_HAND_RIGHT] = path("/user/hand/right");
    if (m.handPath[0] == XR_NULL_PATH || m.handPath[1] == XR_NULL_PATH)
    {
        LL_WARNS("VR") << "no controller input: the hand paths could not be made" << LL_ENDL;
        return;
    }
    XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::snprintf(asci.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "slipstream");
    std::snprintf(asci.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "Slipstream");
    asci.priority = 0;
    XrResult r = f.createActionSet(m.inst, &asci, &m.actionSet);
    if (XR_FAILED(r))
    {
        m.actionSet = XR_NULL_HANDLE;
        LL_WARNS("VR") << "no controller input: xrCreateActionSet -> " << llxrResultText(m.inst, r) << LL_ENDL;
        return;
    }
    auto action = [&](XrAction& out, XrActionType type, const char* name, const char* label) {
        XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
        aci.actionType = type;
        std::snprintf(aci.actionName, XR_MAX_ACTION_NAME_SIZE, "%s", name);
        std::snprintf(aci.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "%s", label);
        aci.countSubactionPaths = LLXR_HAND_COUNT;
        aci.subactionPaths = m.handPath;
        const XrResult ar = f.createAction(m.actionSet, &aci, &out);
        if (XR_FAILED(ar))
        {
            out = XR_NULL_HANDLE;
            LL_WARNS("VR") << "xrCreateAction(" << name << ") -> " << llxrResultText(m.inst, ar) << LL_ENDL;
        }
    };
    action(m.actGrip, XR_ACTION_TYPE_POSE_INPUT, "grip_pose", "Hand pose");
    action(m.actAim, XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Pointer pose");
    action(m.actSelect, XR_ACTION_TYPE_BOOLEAN_INPUT, "select", "Click / select");
    action(m.actSelectValue, XR_ACTION_TYPE_FLOAT_INPUT, "select_value", "Click / select (trigger pull)");
    action(m.actContext, XR_ACTION_TYPE_BOOLEAN_INPUT, "context", "Right click / context menu");
    action(m.actMenu, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Show / hide the UI wall");
    action(m.actFlyCam, XR_ACTION_TYPE_BOOLEAN_INPUT, "flycam", "Fly cam on / off");
    action(m.actStick, XR_ACTION_TYPE_VECTOR2F_INPUT, "stick", "Move / turn / scroll");
    action(m.actStickClick, XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_click", "Recenter (hold the left stick pressed)");
    action(m.actSqueeze, XR_ACTION_TYPE_FLOAT_INPUT, "squeeze", "Grab objects (grip)");
    action(m.actHaptic, XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Vibration");
    if (!m.actGrip || !m.actAim || !m.actSelect)
    {
        LL_WARNS("VR") << "no controller input: the pose / select actions could not be created" << LL_ENDL;
        return;
    }

    struct Bind { XrAction a; const char* p; };
    auto suggest = [&](const char* profile, std::initializer_list<Bind> binds) {
        std::vector<XrActionSuggestedBinding> v;
        for (const Bind& b : binds)
        {
            if (!b.a) continue;
            const XrPath p = path(b.p);
            if (p == XR_NULL_PATH)
            {
                LL_WARNS("VR") << "binding path not accepted: " << b.p << LL_ENDL;
                continue;
            }
            v.push_back({b.a, p});
        }
        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = path(profile);
        sb.countSuggestedBindings = (uint32_t)v.size();
        sb.suggestedBindings = v.data();
        const XrResult br = sb.interactionProfile != XR_NULL_PATH ? f.suggestInteractionProfileBindings(m.inst, &sb)
                                                                   : XR_ERROR_PATH_INVALID;
        LL_INFOS("VR") << "suggested bindings " << profile << " (" << v.size() << ") -> " << llxrResultText(m.inst, br)
                       << LL_ENDL;
    };
    suggest("/interaction_profiles/valve/index_controller",
            {{m.actGrip, "/user/hand/left/input/grip/pose"}, {m.actGrip, "/user/hand/right/input/grip/pose"},
             {m.actAim, "/user/hand/left/input/aim/pose"}, {m.actAim, "/user/hand/right/input/aim/pose"},
             {m.actSelect, "/user/hand/left/input/trigger/click"}, {m.actSelect, "/user/hand/right/input/trigger/click"},
             {m.actSelectValue, "/user/hand/left/input/trigger/value"},
             {m.actSelectValue, "/user/hand/right/input/trigger/value"},
             {m.actContext, "/user/hand/left/input/a/click"}, {m.actContext, "/user/hand/right/input/a/click"},
             {m.actMenu, "/user/hand/left/input/b/click"}, {m.actFlyCam, "/user/hand/right/input/b/click"},
             {m.actStick, "/user/hand/left/input/thumbstick"}, {m.actStick, "/user/hand/right/input/thumbstick"},
             {m.actStickClick, "/user/hand/left/input/thumbstick/click"},
             {m.actStickClick, "/user/hand/right/input/thumbstick/click"},
             {m.actSqueeze, "/user/hand/left/input/squeeze/value"}, {m.actSqueeze, "/user/hand/right/input/squeeze/value"},
             {m.actHaptic, "/user/hand/left/output/haptic"}, {m.actHaptic, "/user/hand/right/output/haptic"}});
    suggest("/interaction_profiles/oculus/touch_controller",
            {{m.actGrip, "/user/hand/left/input/grip/pose"}, {m.actGrip, "/user/hand/right/input/grip/pose"},
             {m.actAim, "/user/hand/left/input/aim/pose"}, {m.actAim, "/user/hand/right/input/aim/pose"},
             {m.actSelectValue, "/user/hand/left/input/trigger/value"},
             {m.actSelectValue, "/user/hand/right/input/trigger/value"},
             {m.actContext, "/user/hand/left/input/x/click"}, {m.actContext, "/user/hand/right/input/a/click"},
             {m.actMenu, "/user/hand/left/input/menu/click"}, {m.actFlyCam, "/user/hand/right/input/b/click"},
             {m.actStick, "/user/hand/left/input/thumbstick"}, {m.actStick, "/user/hand/right/input/thumbstick"},
             {m.actStickClick, "/user/hand/left/input/thumbstick/click"},
             {m.actStickClick, "/user/hand/right/input/thumbstick/click"},
             {m.actSqueeze, "/user/hand/left/input/squeeze/value"}, {m.actSqueeze, "/user/hand/right/input/squeeze/value"},
             {m.actHaptic, "/user/hand/left/output/haptic"}, {m.actHaptic, "/user/hand/right/output/haptic"}});
    suggest("/interaction_profiles/khr/simple_controller",
            {{m.actGrip, "/user/hand/left/input/grip/pose"}, {m.actGrip, "/user/hand/right/input/grip/pose"},
             {m.actAim, "/user/hand/left/input/aim/pose"}, {m.actAim, "/user/hand/right/input/aim/pose"},
             {m.actSelect, "/user/hand/left/input/select/click"}, {m.actSelect, "/user/hand/right/input/select/click"},
             {m.actMenu, "/user/hand/left/input/menu/click"}, {m.actMenu, "/user/hand/right/input/menu/click"},
             {m.actHaptic, "/user/hand/left/output/haptic"}, {m.actHaptic, "/user/hand/right/output/haptic"}});

    for (uint32_t h = 0; h < LLXR_HAND_COUNT; ++h)
    {
        XrActionSpaceCreateInfo sci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        sci.subactionPath = m.handPath[h];
        sci.poseInActionSpace.orientation.w = 1.f;
        sci.action = m.actGrip;
        if (XR_FAILED(f.createActionSpace(m.session, &sci, &m.gripSpace[h]))) m.gripSpace[h] = XR_NULL_HANDLE;
        sci.action = m.actAim;
        if (XR_FAILED(f.createActionSpace(m.session, &sci, &m.aimSpace[h]))) m.aimSpace[h] = XR_NULL_HANDLE;
    }
    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &m.actionSet;
    r = f.attachSessionActionSets(m.session, &ai);
    m.actionsAttached = XR_SUCCEEDED(r);
    LL_INFOS("VR") << "controller actions attached -> " << llxrResultText(m.inst, r) << LL_ENDL;
}

void LLXrSession::logInteractionProfiles()
{
    Impl& m = *mImpl;
    const LLXrFns& f = LLXrLoader::fn();
    if (!m.actionsAttached) return;
    for (uint32_t h = 0; h < LLXR_HAND_COUNT; ++h)
    {
        XrInteractionProfileState ps{XR_TYPE_INTERACTION_PROFILE_STATE};
        std::string name = "(none)";
        if (XR_SUCCEEDED(f.getCurrentInteractionProfile(m.session, m.handPath[h], &ps)) &&
            ps.interactionProfile != XR_NULL_PATH)
        {
            char buf[XR_MAX_PATH_LENGTH] = {};
            uint32_t n = 0;
            if (XR_SUCCEEDED(f.pathToString(m.inst, ps.interactionProfile, XR_MAX_PATH_LENGTH, &n, buf))) name = buf;
        }
        LL_INFOS("VR") << (h == LLXR_HAND_LEFT ? "left" : "right") << " hand interaction profile: " << name << LL_ENDL;
    }
}

// waitFrame, after locateViews: the action states and the controller poses for the same
// predicted display time as the head. Not FOCUSED (dashboard open, headset off): nothing
// active, every button released (LLVRInput lets go of a held click on that edge).
void LLXrSession::syncInput()
{
    Impl& m = *mImpl;
    const LLXrFns& f = LLXrLoader::fn();
    m.input = LLXrInputState{};
    if (!m.actionsAttached || m.appSpace == XR_NULL_HANDLE) return;
    XrActiveActionSet active{m.actionSet, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    const XrResult r = f.syncActions(m.session, &si);
    if (r != XR_SUCCESS)   // XR_SESSION_NOT_FOCUSED (a success code) or a failure: no input
    {
        m.selectByValue[0] = m.selectByValue[1] = false;
        m.squeezeOn[0] = m.squeezeOn[1] = false;
        return;
    }
    m.input.focused = true;
    auto boolState = [&](XrAction a, XrPath sub) {
        if (!a) return false;
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        gi.subactionPath = sub;
        XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
        return XR_SUCCEEDED(f.getActionStateBoolean(m.session, &gi, &s)) && s.isActive && s.currentState;
    };
    auto locate = [&](XrSpace sp, LLXrPose& out) {
        out = LLXrPose{};
        if (!sp) return;
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(f.locateSpace(sp, m.appSpace, m.fs.predictedDisplayTime, &loc))) return;
        const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if ((loc.locationFlags & need) != need) return;
        out.valid = true;
        out.position[0] = loc.pose.position.x;
        out.position[1] = loc.pose.position.y;
        out.position[2] = loc.pose.position.z;
        out.orientation[0] = loc.pose.orientation.x;
        out.orientation[1] = loc.pose.orientation.y;
        out.orientation[2] = loc.pose.orientation.z;
        out.orientation[3] = loc.pose.orientation.w;
    };
    for (uint32_t h = 0; h < LLXR_HAND_COUNT; ++h)
    {
        LLXrHandState& hs = m.input.hand[h];
        const XrPath sub = m.handPath[h];
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = m.actGrip;
        gi.subactionPath = sub;
        XrActionStatePose ps{XR_TYPE_ACTION_STATE_POSE};
        hs.active = XR_SUCCEEDED(f.getActionStatePose(m.session, &gi, &ps)) && ps.isActive;
        if (!hs.active)
        {
            m.selectByValue[h] = false;
            m.squeezeOn[h] = false;
            continue;
        }
        locate(m.gripSpace[h], hs.grip);
        locate(m.aimSpace[h], hs.aim);
        hs.selectClick = boolState(m.actSelect, sub);
        if (m.actSelectValue)
        {
            gi.action = m.actSelectValue;
            XrActionStateFloat tv{XR_TYPE_ACTION_STATE_FLOAT};
            if (XR_SUCCEEDED(f.getActionStateFloat(m.session, &gi, &tv)) && tv.isActive && std::isfinite(tv.currentState))
                hs.selectValue = tv.currentState;
        }
        if (!m.selectBindLogged[h])
        {
            // Once per hand, no press needed: did the runtime bind each select source?
            m.selectBindLogged[h] = true;
            XrActionStateGetInfo bi{XR_TYPE_ACTION_STATE_GET_INFO};
            bi.action = m.actSelect;
            bi.subactionPath = sub;
            XrActionStateBoolean bs{XR_TYPE_ACTION_STATE_BOOLEAN};
            const bool clickBound = m.actSelect && XR_SUCCEEDED(f.getActionStateBoolean(m.session, &bi, &bs)) && bs.isActive;
            LL_INFOS("VR") << "select sources on the " << (h == LLXR_HAND_LEFT ? "left" : "right")
                           << " hand: click component active " << (int)clickBound << ", trigger value active "
                           << (int)(hs.selectValue >= 0.f) << LL_ENDL;
        }
        // Our own hysteresis on the analogue pull: press at 0.5, release at 0.3.
        bool& byValue = m.selectByValue[h];
        if (hs.selectValue < 0.f) byValue = false;
        else if (hs.selectValue >= 0.5f) byValue = true;
        else if (hs.selectValue <= 0.3f) byValue = false;
        hs.select = hs.selectClick || byValue;
        if (hs.select && !m.selectLogged[h])
        {
            m.selectLogged[h] = true;   // once per hand: which source works on this runtime
            LL_INFOS("VR") << "select: first press on the " << (h == LLXR_HAND_LEFT ? "left" : "right")
                           << " hand (click component " << (int)hs.selectClick << ", trigger value " << hs.selectValue
                           << ")" << LL_ENDL;
        }
        hs.context = boolState(m.actContext, sub);
        // #1143 the grip squeeze, our own hysteresis (press 0.7, release 0.35): an Index grip
        // reads above zero while merely holding the controller; the first press per hand logs
        // the value it pressed at.
        if (m.actSqueeze)
        {
            gi.action = m.actSqueeze;
            XrActionStateFloat sv{XR_TYPE_ACTION_STATE_FLOAT};
            if (XR_SUCCEEDED(f.getActionStateFloat(m.session, &gi, &sv)) && sv.isActive && std::isfinite(sv.currentState))
                hs.squeezeValue = sv.currentState;
        }
        bool& sq = m.squeezeOn[h];
        if (hs.squeezeValue < 0.f) sq = false;
        else if (hs.squeezeValue >= 0.7f) sq = true;
        else if (hs.squeezeValue <= 0.35f) sq = false;
        hs.squeeze = sq;
        if (sq && !m.squeezeLogged[h])
        {
            m.squeezeLogged[h] = true;
            LL_INFOS("VR") << "squeeze: first press on the " << (h == LLXR_HAND_LEFT ? "left" : "right") << " hand (value "
                           << hs.squeezeValue << ")" << LL_ENDL;
        }
        hs.menu = boolState(m.actMenu, sub);
        hs.flyCam = boolState(m.actFlyCam, sub);
        hs.stickClick = boolState(m.actStickClick, sub);
        if (m.actStick)
        {
            gi.action = m.actStick;
            XrActionStateVector2f vs{XR_TYPE_ACTION_STATE_VECTOR2F};
            if (XR_SUCCEEDED(f.getActionStateVector2f(m.session, &gi, &vs)) && vs.isActive &&
                std::isfinite(vs.currentState.x) && std::isfinite(vs.currentState.y))
            {
                hs.stick[0] = std::clamp(vs.currentState.x, -1.f, 1.f);
                hs.stick[1] = std::clamp(vs.currentState.y, -1.f, 1.f);
            }
        }
    }
}

bool LLXrSession::frameTiming(double& periodSec, uint32_t& missedSlots) const
{
    if (!mImpl || !(mImpl->frameBegun || mImpl->frameWaited) || !mImpl->timingValid) return false;
    periodSec = (double)mImpl->fs.predictedDisplayPeriod * 1e-9;
    missedSlots = mImpl->missedSlots;
    return true;
}

bool LLXrSession::pacing(LLXrPacing& out) const
{
    if (!mImpl || !(mImpl->frameBegun || mImpl->frameWaited) || !mImpl->timingValid || !mImpl->nativePeriod) return false;
    const Impl& m = *mImpl;
    out.periodSec = (double)m.fs.predictedDisplayPeriod * 1e-9;
    out.nativePeriodSec = (double)m.nativePeriod * 1e-9;
    out.throttled = m.throttled;
    out.missedNative = m.missedNative;
    out.gpuMs = m.gpuMs;
    out.cpuMs = m.lastCpuMs;
    out.slotWaitMs = m.lastSlotMs;
    out.blockedMs = m.blockedMs;
    return true;
}

void LLXrSession::noteSlotWait(double ms)
{
    if (mImpl && mImpl->frameWaited) mImpl->slotWaitMs = (float)ms;
}

void LLXrSession::noteGpuFrame(float ms, double ageSec)
{
    if (!mImpl) return;
    Impl& m = *mImpl;
    // (a sample older than 1 s is no frame time: the timestamps stopped -- no world frames)
    m.gpuMs = (ageSec < 1.0 && ms > 0.f) ? ms : -1.f;
    if (m.gpuMs > 0.f && m.frameWaited)
    {
        m.statGpuSum += m.gpuMs;
        m.statGpuMax = std::max(m.statGpuMax, m.gpuMs);
        ++m.statGpuN;
    }
}

// #1137: xrBeginFrame for this waited frame if it has not been begun (late begin, or an
// early-out frame that is ended now). False: the runtime refused it -- the frame is given up
// (no xrEndFrame for it), exactly as a failed begin at the top of the frame always was.
bool LLXrSession::ensureBegun()
{
    Impl& m = *mImpl;
    if (m.frameBegun) return true;
    if (!m.frameWaited || m.session == XR_NULL_HANDLE) return false;
    const LLXrFns& f = LLXrLoader::fn();
    XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
    const XrResult r = f.beginFrame(m.session, &fbi);
    if (XR_FAILED(r))
    {
        LL_WARNS("VR") << "xrBeginFrame -> " << llxrResultText(m.inst, r) << LL_ENDL;
        m.frameWaited = false;
        return false;
    }
    if (mLateBegin) ++m.statLate;
    m.frameBegun = true;
    return true;
}

// #1137: a frame is complete (endFrameNow, before xrEndFrame): its CPU span into the window.
void LLXrSession::pacingFrameEnd()
{
    Impl& m = *mImpl;
    if (m.frameStart == Clock::time_point{}) return;
    m.lastCpuMs = (float)msSince(m.frameStart);
    m.lastSlotMs = m.slotWaitMs;
    const uint32_t i = m.ringN % Impl::kPaceRing;
    m.ringCpu[i] = m.lastCpuMs;
    m.ringSlot[i] = m.slotWaitMs;
    m.ringBlocked[i] = m.blockedMs;
    ++m.ringN;
    m.statMissedNative += m.missedNative;
    if (m.throttled) ++m.statThrottled;
}

double LLXrSession::frameWorkSec() const
{
    return mImpl ? mImpl->workSec : 0.0;
}

std::string LLXrSession::summary() const
{
    if (!mImpl || mImpl->runtimeText.empty()) return std::string();
    const Impl& m = *mImpl;
    std::string s = m.runtimeText;
    if (!m.systemName.empty()) s += " on \"" + m.systemName + "\"";
    if (m.session != XR_NULL_HANDLE)
    {
        s += m.stereoOn ? ", layered stereo" : ", mono";
        char buf[64];
        if (m.renderW && m.renderH)
        {
            std::snprintf(buf, sizeof(buf), ", %ux%u per eye", m.renderW, m.renderH);
            s += buf;
        }
        if (m.fs.predictedDisplayPeriod > 0)
        {
            std::snprintf(buf, sizeof(buf), ", %.0f Hz", 1e9 / (double)m.fs.predictedDisplayPeriod);
            s += buf;
        }
    }
    else
    {
        s += ", no session";
    }
    return s;
}

bool LLXrSession::inputState(LLXrInputState& out) const
{
    out = LLXrInputState{};
    if (!mImpl || mImpl->session == XR_NULL_HANDLE || !mImpl->running) return false;
    out = mImpl->input;
    return true;
}

void LLXrSession::haptic(uint32_t hand, float amplitude, float seconds)
{
    if (!mImpl || hand >= LLXR_HAND_COUNT) return;
    Impl& m = *mImpl;
    if (!m.actionsAttached || !m.actHaptic || !m.running || !m.input.focused) return;
    XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
    hi.action = m.actHaptic;
    hi.subactionPath = m.handPath[hand];
    XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
    v.duration = (XrDuration)(std::clamp(seconds, 0.f, 1.f) * 1e9f);
    v.frequency = XR_FREQUENCY_UNSPECIFIED;
    v.amplitude = std::clamp(amplitude, 0.f, 1.f);
    LLXrLoader::fn().applyHapticFeedback(m.session, &hi, reinterpret_cast<const XrHapticBaseHeader*>(&v));
}

namespace
{
// #1076: the part of the symmetric mono render (w x h pixels, rendered at `render` around the
// head orientation `headQ`) that covers one eye's located FOV, and that rect's exact FOV (the
// integer rect, converted back: the declaration describes precisely the pixels it names).
// The image's row 0 is the top (+tan up). Left untouched (the whole image, the rendered FOV)
// when the eye's orientation is more than 1 degree off the head's (canted displays: its FOV is
// about another axis), or the eye's FOV is not inside the render.
void eyeRect(const XrView& eye, const float headQ[4], const XrFovf& render, uint32_t w, uint32_t h, XrRect2Di& rect,
             XrFovf& fov)
{
    if (w < 16 || h < 16) return;
    const XrQuaternionf& q = eye.pose.orientation;
    const float dot = std::fabs(q.x * headQ[0] + q.y * headQ[1] + q.z * headQ[2] + q.w * headQ[3]);
    if (!(dot >= 0.99996f)) return;   // cos(0.5 deg): a 1 degree rotation
    const float tx = std::tan(render.angleRight), ty = std::tan(render.angleUp);
    const float l = std::tan(eye.fov.angleLeft), r = std::tan(eye.fov.angleRight);
    const float u = std::tan(eye.fov.angleUp), d = std::tan(eye.fov.angleDown);
    if (!(tx > 0.f) || !(ty > 0.f) || !std::isfinite(l + r + u + d) || !(r > l) || !(u > d)) return;
    const float slack = 1.0005f;
    if (l < -tx * slack || r > tx * slack || u > ty * slack || d < -ty * slack) return;
    const float fw = (float)w, fh = (float)h;
    const int32_t x0 = std::clamp((int32_t)std::floor((l + tx) / (2.f * tx) * fw), 0, (int32_t)w - 8);
    const int32_t x1 = std::clamp((int32_t)std::ceil((r + tx) / (2.f * tx) * fw), x0 + 8, (int32_t)w);
    const int32_t y0 = std::clamp((int32_t)std::floor((ty - u) / (2.f * ty) * fh), 0, (int32_t)h - 8);
    const int32_t y1 = std::clamp((int32_t)std::ceil((ty - d) / (2.f * ty) * fh), y0 + 8, (int32_t)h);
    rect = XrRect2Di{{x0, y0}, {x1 - x0, y1 - y0}};
    fov.angleLeft = std::atan((float)x0 / fw * 2.f * tx - tx);
    fov.angleRight = std::atan((float)x1 / fw * 2.f * tx - tx);
    fov.angleUp = std::atan(ty - (float)y0 / fh * 2.f * ty);
    fov.angleDown = std::atan(ty - (float)y1 / fh * 2.f * ty);
}
} // namespace

bool LLXrSession::stereoWanted() const
{
    return mImpl && mImpl->cfg.stereo;
}

bool LLXrSession::stereo() const
{
    return mImpl && mImpl->stereoOn && mImpl->session != XR_NULL_HANDLE;
}

void LLXrSession::recordFrame(VkCommandBuffer cmd, VkImage windowImage, VkFormat windowFormat, VkExtent2D windowExtent,
                              VkImage worldImage, VkFormat worldFormat, VkExtent2D worldExtent, VkImage uiImage,
                              uint32_t worldLayers, const LLXrWorldDepth* depth)
{
    if (!mImpl) return;
    Impl& m = *mImpl;
    if (!m.frameWaited || m.recorded || !m.fs.shouldRender || m.session == XR_NULL_HANDLE || !m.locatedOk) return;
    // #1137 late begin: the frame's GPU work follows this recording -- begin it now.
    if (!ensureBegun()) return;
    const LLXrFns& f = LLXrLoader::fn();

    // #1074: the world reaches the eyes by a bit-exact copy, so its bytes must be the eye
    // images' format (the UNORM sibling of their sRGB format: the viewer's sRGB-encoded output).
    bool world = worldImage != VK_NULL_HANDLE;
    if (world && (worldFormat != unormSibling(m.eyes[0].imageFormat) || !worldExtent.width || !worldExtent.height))
    {
        if (!m.worldFormatWarned)
        {
            m.worldFormatWarned = true;
            LL_WARNS("VR") << "the world image (" << formatName(worldFormat) << ") cannot be copied into the eye images ("
                           << formatName(m.eyes[0].imageFormat) << "): the headset shows the desktop panel only" << LL_ENDL;
        }
        world = false;
    }

    // #1075: a STEREO frame -- the world has one layer per eye (the stereo session's world renders
    // them): layer v goes to eye v, declared at that eye's located pose and FOV. Otherwise M1: the
    // one mono image into both eyes.
    const bool stereoFrame = world && m.stereoOn && worldLayers >= m.eyeCount;
    // Acquire every eye swapchain (the array swapchain once).
    const uint32_t swapCount = m.eyeArray ? 1u : m.eyeCount;
    for (uint32_t s = 0; s < swapCount; ++s)
    {
        XrSwap& e = m.eyes[s];
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrResult r = f.acquireSwapchainImage(e.handle, &ai, &idx);
        if (XR_FAILED(r) || idx >= e.images.size())
        {
            LL_WARNS("VR") << "xrAcquireSwapchainImage (eye " << s << ") -> " << llxrResultText(m.inst, r) << LL_ENDL;
            return;   // endFrame releases what was acquired; no layers
        }
        e.acquired = idx;
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        r = f.waitSwapchainImage(e.handle, &wi);
        if (r != XR_SUCCESS)
        {
            LL_WARNS("VR") << "xrWaitSwapchainImage (eye " << s << ") -> " << llxrResultText(m.inst, r) << LL_ENDL;
            return;
        }
    }
    for (uint32_t i = 0; i < m.eyeCount; ++i)
    {
        XrSwap& e = m.eyes[m.eyeArray ? 0u : i];
        const uint32_t idx = e.acquired;
        const uint32_t layer = m.eyeArray ? i : 0u;   // (the eye's layer of its swapchain image)
        XrCompositionLayerProjectionView& pv = m.projViews[i];
        pv = XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        if (world)
        {
            // The world image, bit-exact (UNORM bytes into the sRGB image: no re-encode, unlike
            // a blit); contents discarded first. Back in COLOR_ATTACHMENT_OPTIMAL as
            // XR_KHR_vulkan_enable2 requires.
            xrBarrier(cmd, e.images[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, layer);
            VkImageCopy rg{};
            rg.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, stereoFrame ? i : 0u, 1};   // (#1075: eye i's layer)
            rg.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
            rg.extent = {std::min(e.width, worldExtent.width), std::min(e.height, worldExtent.height), 1};
            vkCmdCopyImage(cmd, worldImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, e.images[idx],
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
            xrBarrier(cmd, e.images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT, layer);
            const uint32_t cw = std::min(e.width, worldExtent.width), ch = std::min(e.height, worldExtent.height);
            XrRect2Di rect{{0, 0}, {(int32_t)cw, (int32_t)ch}};
            if (stereoFrame)
            {
                // #1075 STEREO: the eye was rendered from exactly its located pose and FOV
                // (LLVRCamera::eyeView, the same waitFrame locate): declared as such, whole image.
                pv.pose = m.located[i].pose;
                pv.fov = m.located[i].fov;
            }
            else
            {
                // Declared as RENDERED: the head orientation the camera used, at this eye's own
                // located position. Mono: both eyes get the same image, so there is no stereo
                // depth (M1). #1076: each eye declares only the part of the symmetric render that
                // covers its own located FOV (eyeRect) -- the runtime's native per-eye projection
                // -- unless the displays are canted or SlipstreamVRDebugLayers 3 asks for the M1
                // declaration (the whole image at the symmetric FOV).
                pv.pose.orientation = {m.head.orientation[0], m.head.orientation[1], m.head.orientation[2], m.head.orientation[3]};
                pv.pose.position = m.located[i].pose.position;
                pv.fov = m.renderFov;
                if (mUi.debugLayers != 3u) eyeRect(m.located[i], m.head.orientation, m.renderFov, cw, ch, rect, pv.fov);
            }
            pv.subImage.swapchain = e.handle;
            pv.subImage.imageRect = rect;
            pv.subImage.imageArrayIndex = layer;
            continue;
        }
        else
        {
            // No world this frame (login, loading): the login room's dark space behind the wall.
            xrBarrier(cmd, e.images[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, layer);
            VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            att.imageView = e.views[idx * e.layers + layer];
            att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            att.clearValue.color = {{0.025f, 0.028f, 0.045f, 1.0f}};   // #1076 the login room's dark space (sRGB-encoded)
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, {e.width, e.height}};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &att;
            vkCmdBeginRendering(cmd, &ri);
            vkCmdEndRendering(cmd);
            pv.pose = m.located[i].pose;
            pv.fov = m.located[i].fov;
        }
        pv.subImage.swapchain = e.handle;
        pv.subImage.imageRect = {{0, 0}, {(int32_t)e.width, (int32_t)e.height}};
        pv.subImage.imageArrayIndex = layer;
    }
    // #1075 DEPTH LAYER (XR_KHR_composition_layer_depth, stereo frames only): the world's depth,
    // layer v -> eye v's depth image, bit for bit (D32_SFLOAT both), chained into the eye's
    // projection view (positional reprojection; reversed-Z: the distance at depth 0 is the far
    // one -- nearZ > farZ is how the extension states an inverted depth range).
    m.depthRecorded = false;
    if (stereoFrame && depth && depth->image && m.depth[0].handle != XR_NULL_HANDLE)
    {
        const bool fits = depth->format == VK_FORMAT_D32_SFLOAT && depth->layers >= m.eyeCount &&
                          depth->extent.width >= m.renderW && depth->extent.height >= m.renderH;
        if (!fits && !m.depthWarned)
        {
            m.depthWarned = true;
            LL_WARNS("VR") << "no depth layer: the world depth (" << formatName(depth->format) << ", " << depth->extent.width
                           << "x" << depth->extent.height << " x" << depth->layers << ") does not match the depth swapchain"
                           << LL_ENDL;
        }
        bool ok = fits;
        const uint32_t dCount = m.eyeArray ? 1u : m.eyeCount;
        for (uint32_t s = 0; ok && s < dCount; ++s)
        {
            XrSwap& d = m.depth[s];
            uint32_t idx = 0;
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            ok = XR_SUCCEEDED(f.acquireSwapchainImage(d.handle, &ai, &idx)) && idx < d.images.size();
            if (!ok) break;
            d.acquired = idx;
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = XR_INFINITE_DURATION;
            ok = f.waitSwapchainImage(d.handle, &wi) == XR_SUCCESS;
        }
        if (ok)
        {
            const VkImageAspectFlags DA = VK_IMAGE_ASPECT_DEPTH_BIT;
            const VkPipelineStageFlags2 DS = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            for (uint32_t i = 0; i < m.eyeCount; ++i)
                xrBarrier(cmd, depth->image, depth->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          DS | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, i, DA);
            for (uint32_t i = 0; i < m.eyeCount; ++i)
            {
                XrSwap& d = m.depth[m.eyeArray ? 0u : i];
                const uint32_t layer = m.eyeArray ? i : 0u;
                xrBarrier(cmd, d.images[d.acquired], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          DS | VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT, layer, DA);
                VkImageCopy rg{};
                rg.srcSubresource = {DA, 0, i, 1};
                rg.dstSubresource = {DA, 0, layer, 1};
                rg.extent = {m.renderW, m.renderH, 1};
                vkCmdCopyImage(cmd, depth->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d.images[d.acquired],
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
                // XR_KHR_vulkan_enable2: a depth swapchain image goes back DEPTH_STENCIL_ATTACHMENT_OPTIMAL.
                xrBarrier(cmd, d.images[d.acquired], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT, DS, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, layer, DA);
                XrCompositionLayerDepthInfoKHR& di = m.depthInfo[i];
                di = XrCompositionLayerDepthInfoKHR{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
                di.subImage.swapchain = d.handle;
                di.subImage.imageRect = {{0, 0}, {(int32_t)m.renderW, (int32_t)m.renderH}};
                di.subImage.imageArrayIndex = layer;
                di.minDepth = 0.f;
                di.maxDepth = 1.f;
                di.nearZ = depth->distAtZero;
                di.farZ = depth->distAtOne;
                m.projViews[i].next = &di;
            }
            for (uint32_t i = 0; i < m.eyeCount; ++i)
                xrBarrier(cmd, depth->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depth->layout,
                          VK_PIPELINE_STAGE_2_COPY_BIT, 0, DS | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          i, DA);
            m.depthRecorded = true;
        }
    }
    m.recorded = true;
    ++m.rendered;
    recordFade(cmd);   // #1076 (nothing unless a fade runs)
    recordVignette(cmd);   // #1078 (nothing unless the view moves)
    recordLobby(cmd);      // #1076 (the login room: nothing in the world)
    recordWrist(cmd);      // #1120 (nothing unless the panel was redrawn this frame)

    // #1076 UI layers. Their content goes bit-exact into the UI swapchain (the window format's
    // sRGB sibling): with the world, the renderer's UI-only image (premultiplied alpha: the
    // world shows between the floaters); without it (login, loading) the window image itself,
    // exactly what the desktop shows. Shown as the UI wall (world-locked where it was
    // summoned; before the first placement, head-locked in front of you as in M1).
    VkImage uiSrc = VK_NULL_HANDLE;
    if (world && uiImage && (mUi.wall || mUi.panel)) uiSrc = uiImage;
    else if (!world && windowImage) uiSrc = windowImage;
    if (!uiSrc || !ensureUiSwapchain(windowFormat, windowExtent)) return;
    const bool srcIsWindow = uiSrc == windowImage;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrResult r = f.acquireSwapchainImage(m.ui.handle, &ai, &idx);
    if (XR_FAILED(r) || idx >= m.ui.images.size())
    {
        LL_WARNS("VR") << "xrAcquireSwapchainImage (UI) -> " << llxrResultText(m.inst, r) << LL_ENDL;
        return;
    }
    m.ui.acquired = idx;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if ((r = f.waitSwapchainImage(m.ui.handle, &wi)) != XR_SUCCESS)
    {
        LL_WARNS("VR") << "xrWaitSwapchainImage (UI) -> " << llxrResultText(m.inst, r) << LL_ENDL;
        return;
    }
    const VkImage uiImg = m.ui.images[idx];
    const uint32_t cw = std::min(m.ui.width, windowExtent.width), ch = std::min(m.ui.height, windowExtent.height);
    if (!cw || !ch) return;
    if (srcIsWindow)
        xrBarrier(cmd, uiSrc, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    xrBarrier(cmd, uiImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
              VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy rg{};
    rg.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.extent = {cw, ch, 1};
    if (mUi.panel)
    {
        const int32_t x0 = std::clamp(mUi.panelRect[0], 0, (int32_t)cw - 1);
        const int32_t y0 = std::clamp(mUi.panelRect[1], 0, (int32_t)ch - 1);
        m.panelRect = {{x0, y0}, {std::clamp(mUi.panelRect[2], 1, (int32_t)cw - x0), std::clamp(mUi.panelRect[3], 1, (int32_t)ch - y0)}};
    }
    vkCmdCopyImage(cmd, uiSrc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, uiImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    xrBarrier(cmd, uiImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    // The window image back to where renderFrame left it (its present transition follows).
    if (srcIsWindow)
        xrBarrier(cmd, uiSrc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    m.uiCopyW = cw;
    m.uiCopyH = ch;
    m.uiRecorded = true;
    m.uiAlpha = !srcIsWindow;   // the UI-only image: blend by its (premultiplied) alpha
}

// #1076: the comfort fade -- a tiny swapchain cleared to (0, 0, 0, a) (premultiplied black at
// coverage a), shown head-locked over every other layer while mFade > 0. Created on the first
// fade; a runtime refusing it leaves blinks as plain cuts (logged once).
void LLXrSession::recordFade(VkCommandBuffer cmd)
{
    Impl& m = *mImpl;
    m.fadeRecorded = false;
    const float a = std::isfinite(mFade) ? std::clamp(mFade, 0.f, 1.f) : 0.f;
    if (a <= 0.001f || m.fadeFailed) return;
    const LLXrFns& f = LLXrLoader::fn();
    if (m.fade.handle == XR_NULL_HANDLE)
    {
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        ci.format = m.eyes[0].imageFormat;   // an sRGB format the runtime offered
        ci.sampleCount = 1;
        ci.width = ci.height = 8;
        ci.faceCount = ci.arraySize = ci.mipCount = 1;
        const XrResult r = f.createSwapchain(m.session, &ci, &m.fade.handle);
        LL_INFOS("VR") << "xrCreateSwapchain fade 8x8 -> " << llxrResultText(m.inst, r) << LL_ENDL;
        if (XR_FAILED(r))
        {
            m.fade.handle = XR_NULL_HANDLE;
            m.fadeFailed = true;
            return;
        }
        m.fade.width = m.fade.height = 8;
        m.fade.imageFormat = m.fade.viewFormat = m.eyes[0].imageFormat;
        uint32_t n = 0;
        f.enumerateSwapchainImages(m.fade.handle, 0, &n, nullptr);
        std::vector<XrSwapchainImageVulkan2KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
        if (n) f.enumerateSwapchainImages(m.fade.handle, n, &n, (XrSwapchainImageBaseHeader*)imgs.data());
        for (uint32_t k = 0; k < n; ++k) m.fade.images.push_back(imgs[k].image);
    }
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(f.acquireSwapchainImage(m.fade.handle, &ai, &idx)) || idx >= m.fade.images.size()) return;
    m.fade.acquired = idx;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (f.waitSwapchainImage(m.fade.handle, &wi) != XR_SUCCESS) return;
    const VkImage img = m.fade.images[idx];
    xrBarrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT,
              VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkClearColorValue c{};
    c.float32[3] = a;
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
    xrBarrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    m.fadeRecorded = true;
}

// #1078: the comfort vignette. A kVigSize^2 image on a head-locked quad kVigDist in front,
// kVigHalfTan * kVigDist half wide (71.6 degrees each way: past the edges of the Index's per-eye
// FOV, corners included). Each texel is premultiplied black at an alpha that rises with the
// angle off the view axis: clear up to an inner angle, black past an outer one; both shrink as
// the strength grows (strength 1: clear to 20 degrees, black past 40), and weak strengths are
// also fainter. 1 m away: each eye's 32 mm offset moves the aperture by ~2 degrees (a blurred
// edge, unnoticeable); the fade's 0.25 m would move it by 7.
namespace
{
constexpr uint32_t kVigSize = 128;      // 128 * 128 * 4 = 65536 bytes: one vkCmdUpdateBuffer
constexpr float kVigDist = 1.f;
constexpr float kVigHalfTan = 3.f;
constexpr int kVigLevels = 48;          // strength quantization (the gradient is rebuilt per level)

float vigSmooth(float e0, float e1, float x)
{
    const float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// The gradient for strength s (0 .. 1) into out (kVigSize^2 texels, 4 bytes; alpha in byte 3,
// colour 0: the same bytes in BGRA and RGBA, and alpha is linear in sRGB formats).
void buildVignette(float s, uint8_t* out)
{
    const float deg = 3.14159265f / 180.f;
    const float outer = (75.f - 35.f * s) * deg;
    const float inner = outer - 20.f * deg;
    const float gain = vigSmooth(0.f, 0.25f, s);
    const uint32_t h = kVigSize / 2;
    for (uint32_t y = 0; y < h; ++y)
    {
        for (uint32_t x = 0; x < h; ++x)
        {
            // The quadrant's texel centre in tangent space, mirrored into the other three.
            const float tx = ((float)(h + x) + 0.5f) / (float)kVigSize * 2.f - 1.f;
            const float ty = ((float)(h + y) + 0.5f) / (float)kVigSize * 2.f - 1.f;
            const float ang = std::atan(kVigHalfTan * std::sqrt(tx * tx + ty * ty));
            const uint8_t a = (uint8_t)std::lround(255.f * gain * vigSmooth(inner, outer, ang));
            const uint32_t xs[2] = {h + x, h - 1 - x}, ys[2] = {h + y, h - 1 - y};
            for (uint32_t yi : ys)
                for (uint32_t xi : xs)
                {
                    uint8_t* p = out + 4u * (yi * kVigSize + xi);
                    p[0] = p[1] = p[2] = 0;
                    p[3] = a;
                }
        }
    }
}
} // namespace

void LLXrSession::recordVignette(VkCommandBuffer cmd)
{
    Impl& m = *mImpl;
    m.vigRecorded = false;
    const float s = std::isfinite(mVignette) ? std::clamp(mVignette, 0.f, 1.f) : 0.f;
    if (s <= 0.01f || m.vigFailed || !m.dev) return;
    const LLXrFns& f = LLXrLoader::fn();
    if (m.vig.handle == XR_NULL_HANDLE)
    {
        // First use this session (allocation failure: no vignette this session, logged once).
        m.vigBuf = m.dev->createBuffer((VkDeviceSize)kVigSize * kVigSize * 4u,
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        ci.format = m.eyes[0].imageFormat;
        ci.sampleCount = 1;
        ci.width = ci.height = kVigSize;
        ci.faceCount = ci.arraySize = ci.mipCount = 1;
        const XrResult r = m.vigBuf.buffer ? f.createSwapchain(m.session, &ci, &m.vig.handle) : XR_ERROR_RUNTIME_FAILURE;
        LL_INFOS("VR") << "comfort vignette: staging buffer " << (m.vigBuf.buffer ? "made" : "NOT made")
                       << ", xrCreateSwapchain " << kVigSize << "x" << kVigSize << " -> " << llxrResultText(m.inst, r)
                       << LL_ENDL;
        if (XR_FAILED(r) || !m.vigBuf.buffer)
        {
            if (m.vigBuf.buffer) m.dev->destroyBuffer(m.vigBuf);
            m.vig.handle = XR_NULL_HANDLE;
            m.vigFailed = true;
            return;
        }
        m.vig.width = m.vig.height = kVigSize;
        m.vig.imageFormat = m.vig.viewFormat = m.eyes[0].imageFormat;
        uint32_t n = 0;
        f.enumerateSwapchainImages(m.vig.handle, 0, &n, nullptr);
        std::vector<XrSwapchainImageVulkan2KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
        if (n) f.enumerateSwapchainImages(m.vig.handle, n, &n, (XrSwapchainImageBaseHeader*)imgs.data());
        for (uint32_t k = 0; k < n; ++k) m.vig.images.push_back(imgs[k].image);
        m.vigBuiltLevel = -1;
    }
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(f.acquireSwapchainImage(m.vig.handle, &ai, &idx)) || idx >= m.vig.images.size()) return;
    m.vig.acquired = idx;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (f.waitSwapchainImage(m.vig.handle, &wi) != XR_SUCCESS) return;
    // The gradient into the staging buffer when the strength level changed (after the last
    // frame's copy out of it: same queue, ordered by the barrier).
    const int level = std::clamp((int)std::lround(s * (float)kVigLevels), 1, kVigLevels);
    if (level != m.vigBuiltLevel)
    {
        static uint8_t s_texels[kVigSize * kVigSize * 4];   // (main thread only)
        buildVignette((float)level / (float)kVigLevels, s_texels);
        VkBufferMemoryBarrier2 bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        bb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        bb.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        bb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        bb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.buffer = m.vigBuf.buffer;
        bb.size = VK_WHOLE_SIZE;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.bufferMemoryBarrierCount = 1;
        di.pBufferMemoryBarriers = &bb;
        vkCmdPipelineBarrier2(cmd, &di);
        vkCmdUpdateBuffer(cmd, m.vigBuf.buffer, 0, sizeof(s_texels), s_texels);
        bb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        bb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier2(cmd, &di);
        m.vigBuiltLevel = level;
    }
    const VkImage img = m.vig.images[idx];
    xrBarrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
              VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy rg{};
    rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.imageExtent = {kVigSize, kVigSize, 1};
    vkCmdCopyBufferToImage(cmd, m.vigBuf.buffer, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    xrBarrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    m.vigRecorded = true;
}

// #1076 the login room's images, procedural, premultiplied alpha, sRGB-encoded bytes (the
// swapchains are sRGB: the compositor decodes them; alpha is linear).
namespace
{
constexpr uint32_t kDoorW = 128, kDoorH = 256;   // the doorway image (the door fills the middle, glow around)
constexpr uint32_t kMistN = 256;                 // the mist image (square)

uint8_t srgbByte(float linear)
{
    const float c = std::clamp(linear, 0.f, 1.f);
    const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
    return (uint8_t)std::lround(std::clamp(s, 0.f, 1.f) * 255.f);
}

bool sBgra = true;   // the swapchain's byte order (the eyes' format: B8G8R8A8 or R8G8B8A8), set per build

void putPremul(uint8_t* p, float r, float g, float b, float a)
{
    a = std::clamp(a, 0.f, 1.f);
    p[sBgra ? 0 : 2] = srgbByte(b * a);
    p[1] = srgbByte(g * a);
    p[sBgra ? 2 : 0] = srgbByte(r * a);
    p[3] = (uint8_t)std::lround(a * 255.f);
}

// A cheap smooth value noise in [0, 1] (hash on the integer lattice, smoothstep blend).
float hashF(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFFu) / 65535.f;
}

float valueNoise(float x, float y)
{
    const int xi = (int)std::floor(x), yi = (int)std::floor(y);
    const float fx = x - (float)xi, fy = y - (float)yi;
    const float sx = fx * fx * (3.f - 2.f * fx), sy = fy * fy * (3.f - 2.f * fy);
    const float a = hashF(xi, yi), b = hashF(xi + 1, yi), c = hashF(xi, yi + 1), d = hashF(xi + 1, yi + 1);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}

// The doorway: a warm glowing frame (with a soft halo outside it) around a pale, brighter
// towards its centre opening. The door occupies the middle 100 x 220 texels.
void buildDoor(uint8_t* out)
{
    const float x0 = 14.f, x1 = 114.f, y0 = 18.f, y1 = 238.f, frame = 7.f;
    for (uint32_t y = 0; y < kDoorH; ++y)
        for (uint32_t x = 0; x < kDoorW; ++x)
        {
            const float px = (float)x + 0.5f, py = (float)y + 0.5f;
            // Distance outside the door rectangle (0 inside) and inside it (to the nearest edge).
            const float dx = std::max({x0 - px, 0.f, px - x1}), dy = std::max({y0 - py, 0.f, py - y1});
            const float outside = std::sqrt(dx * dx + dy * dy);
            const float inside = std::min({px - x0, x1 - px, py - y0, y1 - py});
            float r, g, b, a;
            if (outside > 0.f)
            {
                const float h = std::exp(-outside / 4.f);   // the halo
                r = 1.f; g = 0.85f; b = 0.6f; a = 0.7f * h;
            }
            else if (inside < frame)
            {
                r = 1.f; g = 0.93f; b = 0.78f; a = 1.f;     // the glowing frame
            }
            else
            {
                // The opening: pale light, brighter in the middle and towards the floor.
                const float u = (px - 64.f) / 50.f, v = (py - y0) / (y1 - y0);
                const float c = std::exp(-u * u * 1.5f) * (0.55f + 0.45f * v);
                r = 0.75f + 0.25f * c; g = 0.82f + 0.18f * c; b = 1.f; a = 0.35f + 0.55f * c;
            }
            putPremul(out + 4u * (y * kDoorW + x), r, g, b, a);
        }
}

// The floor mist: a pale blue-grey haze, densest under you, fading out by the edge, broken up
// by two octaves of noise.
void buildMist(uint8_t* out)
{
    for (uint32_t y = 0; y < kMistN; ++y)
        for (uint32_t x = 0; x < kMistN; ++x)
        {
            const float u = ((float)x + 0.5f) / (float)kMistN * 2.f - 1.f, v = ((float)y + 0.5f) / (float)kMistN * 2.f - 1.f;
            const float rr = std::sqrt(u * u + v * v);
            const float fall = 1.f - std::clamp((rr - 0.15f) / 0.85f, 0.f, 1.f);
            const float n = 0.65f * valueNoise(u * 5.f + 11.f, v * 5.f + 7.f) + 0.35f * valueNoise(u * 13.f + 3.f, v * 13.f + 5.f);
            const float a = 0.42f * fall * fall * (0.45f + 0.55f * n);
            putPremul(out + 4u * (y * kMistN + x), 0.55f, 0.62f, 0.75f, a);
        }
}

bool makeStaticSwap(const LLXrFns& f, XrSession session, XrInstance inst, VkFormat fmt, uint32_t w, uint32_t h, XrSwap& s,
                    const char* what)
{
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = fmt;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    const XrResult r = f.createSwapchain(session, &ci, &s.handle);
    LL_INFOS("VR") << "login room: xrCreateSwapchain " << what << " " << w << "x" << h << " (static) -> "
                   << llxrResultText(inst, r) << LL_ENDL;
    if (XR_FAILED(r))
    {
        s.handle = XR_NULL_HANDLE;
        return false;
    }
    s.width = w;
    s.height = h;
    s.imageFormat = s.viewFormat = fmt;
    uint32_t n = 0;
    f.enumerateSwapchainImages(s.handle, 0, &n, nullptr);
    std::vector<XrSwapchainImageVulkan2KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    if (n) f.enumerateSwapchainImages(s.handle, n, &n, (XrSwapchainImageBaseHeader*)imgs.data());
    for (uint32_t k = 0; k < n; ++k) s.images.push_back(imgs[k].image);
    return !s.images.empty();
}
} // namespace

void LLXrSession::recordLobby(VkCommandBuffer cmd)
{
    Impl& m = *mImpl;
    m.lobbyRecorded = false;
    if (!mLobby.on || m.lobbyFailed || !m.dev) return;
    if (m.lobbyWritten)
    {
        m.lobbyRecorded = true;   // (static images: written once, shown as they are)
        return;
    }
    const LLXrFns& f = LLXrLoader::fn();
    const VkDeviceSize doorBytes = (VkDeviceSize)kDoorW * kDoorH * 4u, mistBytes = (VkDeviceSize)kMistN * kMistN * 4u;
    auto fail = [&](const char* why)
    {
        LL_WARNS("VR") << "login room: " << why << " -- the room shows without its doorway / mist this session" << LL_ENDL;
        for (XrSwap* s : {&m.door, &m.mist})
        {
            // (one still acquired is released by endFrameNow and destroyed with the session)
            if (s->handle == XR_NULL_HANDLE || s->acquired != UINT32_MAX || !f.destroySwapchain) continue;
            f.destroySwapchain(s->handle);
            *s = XrSwap{};
        }
        m.lobbyFailed = true;
    };
    m.lobbyBuf = m.dev->createBuffer(doorBytes + mistBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!m.lobbyBuf.buffer || !m.lobbyBuf.mapped)
    {
        if (m.lobbyBuf.buffer) m.dev->destroyBuffer(m.lobbyBuf);
        fail("no staging buffer");
        return;
    }
    const VkFormat fmt = m.eyes[0].imageFormat;
    sBgra = fmt == VK_FORMAT_B8G8R8A8_SRGB || fmt == VK_FORMAT_B8G8R8A8_UNORM;
    buildDoor((uint8_t*)m.lobbyBuf.mapped);
    buildMist((uint8_t*)m.lobbyBuf.mapped + doorBytes);
    if (!makeStaticSwap(f, m.session, m.inst, fmt, kDoorW, kDoorH, m.door, "doorway") ||
        !makeStaticSwap(f, m.session, m.inst, fmt, kMistN, kMistN, m.mist, "mist"))
    {
        fail("the runtime refused a static swapchain");
        return;
    }
    // Acquire each static image once, copy, release (endFrameNow): then it is shown as is.
    struct Job { XrSwap* s; VkDeviceSize off; uint32_t w, h; } jobs[2] = {{&m.door, 0, kDoorW, kDoorH},
                                                                         {&m.mist, doorBytes, kMistN, kMistN}};
    for (const Job& j : jobs)
    {
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(f.acquireSwapchainImage(j.s->handle, &ai, &idx)) || idx >= j.s->images.size())
        {
            fail("xrAcquireSwapchainImage failed");
            return;
        }
        j.s->acquired = idx;
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        if (f.waitSwapchainImage(j.s->handle, &wi) != XR_SUCCESS)
        {
            fail("xrWaitSwapchainImage failed");
            return;
        }
        const VkImage img = j.s->images[idx];
        xrBarrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy rg{};
        rg.bufferOffset = j.off;
        rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rg.imageExtent = {j.w, j.h, 1};
        vkCmdCopyBufferToImage(cmd, m.lobbyBuf.buffer, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
        xrBarrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    }
    m.lobbyWritten = true;
    m.lobbyRecorded = true;
    LL_INFOS("VR") << "login room: doorway and mist images written" << LL_ENDL;
}

// #1120 the wrist panel's format: R8G8B8A8_SRGB takes LLVRWrist's R8G8B8A8_UNORM bytes as they
// are (the bytes are sRGB-encoded, the compositor decodes them); B8G8R8A8_SRGB does too when the
// panel draws red and blue swapped. A blit would re-encode, so no other format is usable.
bool LLXrSession::wristFormat(bool& swapRB) const
{
    swapRB = false;
    if (!mImpl || mImpl->session == XR_NULL_HANDLE || mImpl->wristFailed || mImpl->formats.empty()) return false;
    const std::vector<int64_t>& fm = mImpl->formats;
    if (std::find(fm.begin(), fm.end(), (int64_t)VK_FORMAT_R8G8B8A8_SRGB) != fm.end()) return true;
    if (std::find(fm.begin(), fm.end(), (int64_t)VK_FORMAT_B8G8R8A8_SRGB) != fm.end())
    {
        swapRB = true;
        return true;
    }
    return false;
}

void LLXrSession::setWristImage(VkImage image, uint32_t width, uint32_t height)
{
    mWristSrc = image;
    mWristSrcW = width;
    mWristSrcH = height;
}

// #1120: the redrawn wrist image (LLVRWrist's render target) into the wrist swapchain, bit-exact.
// The swapchain is made on the first copy at the image's size (and again if that size changes:
// a UI scale change, rare). A runtime refusing it leaves the panel off for the session (logged).
// Frames that did not redraw copy nothing: the layer shows the last released image.
void LLXrSession::recordWrist(VkCommandBuffer cmd)
{
    Impl& m = *mImpl;
    const VkImage src = mWristSrc;
    mWristSrc = VK_NULL_HANDLE;
    if (!src || !mWristSrcW || !mWristSrcH || m.wristFailed) return;
    const LLXrFns& f = LLXrLoader::fn();
    if (m.wrist.handle != XR_NULL_HANDLE && (m.wrist.width != mWristSrcW || m.wrist.height != mWristSrcH))
    {
        LL_INFOS("VRWrist") << "wrist swapchain: image now " << mWristSrcW << "x" << mWristSrcH << ", recreating (was "
                            << m.wrist.width << "x" << m.wrist.height << ")" << LL_ENDL;
        if (m.vkDev) vkDeviceWaitIdle(m.vkDev);
        f.destroySwapchain(m.wrist.handle);
        m.wrist = XrSwap{};
        m.wristContent = false;
    }
    if (m.wrist.handle == XR_NULL_HANDLE)
    {
        bool swapRB = false;
        if (!wristFormat(swapRB) || (m.maxSwapW && mWristSrcW > m.maxSwapW) || (m.maxSwapH && mWristSrcH > m.maxSwapH))
        {
            m.wristFailed = true;
            LL_WARNS("VRWrist") << "no wrist panel this session: the runtime offers neither R8G8B8A8_SRGB nor B8G8R8A8_SRGB, "
                                   "or the panel image (" << mWristSrcW << "x" << mWristSrcH << ") is over its swapchain limit ("
                                << m.maxSwapW << "x" << m.maxSwapH << ")" << LL_ENDL;
            return;
        }
        const VkFormat fm = swapRB ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_R8G8B8A8_SRGB;
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        ci.format = fm;
        ci.sampleCount = 1;
        ci.width = mWristSrcW;
        ci.height = mWristSrcH;
        ci.faceCount = ci.arraySize = ci.mipCount = 1;
        const XrResult r = f.createSwapchain(m.session, &ci, &m.wrist.handle);
        LL_INFOS("VRWrist") << "xrCreateSwapchain wrist panel " << mWristSrcW << "x" << mWristSrcH << " " << formatName(fm)
                            << " (~" << (mWristSrcW * mWristSrcH * 4u + 1023u) / 1024u << " KB per image) -> "
                            << llxrResultText(m.inst, r) << LL_ENDL;
        if (XR_FAILED(r))
        {
            m.wrist.handle = XR_NULL_HANDLE;
            m.wristFailed = true;
            return;
        }
        m.wrist.width = mWristSrcW;
        m.wrist.height = mWristSrcH;
        m.wrist.imageFormat = m.wrist.viewFormat = fm;
        uint32_t n = 0;
        f.enumerateSwapchainImages(m.wrist.handle, 0, &n, nullptr);
        std::vector<XrSwapchainImageVulkan2KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
        if (n) f.enumerateSwapchainImages(m.wrist.handle, n, &n, (XrSwapchainImageBaseHeader*)imgs.data());
        for (uint32_t k = 0; k < n; ++k) m.wrist.images.push_back(imgs[k].image);
        LL_INFOS("VRWrist") << "wrist swapchain: " << m.wrist.images.size() << " images" << LL_ENDL;
        if (m.wrist.images.empty())
        {
            f.destroySwapchain(m.wrist.handle);
            m.wrist = XrSwap{};
            m.wristFailed = true;
            return;
        }
    }
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(f.acquireSwapchainImage(m.wrist.handle, &ai, &idx)) || idx >= m.wrist.images.size()) return;
    m.wrist.acquired = idx;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (f.waitSwapchainImage(m.wrist.handle, &wi) != XR_SUCCESS) return;   // (released by endFrame)
    const VkImage dst = m.wrist.images[idx];
    // The render target: SHADER_READ_ONLY after the frame's offscreen pass (earlier in this
    // frame's submission order), back to it after the copy (its next offscreen pass expects that).
    xrBarrier(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    xrBarrier(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy rg{};
    rg.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.extent = {m.wrist.width, m.wrist.height, 1};
    vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    xrBarrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    xrBarrier(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_PIPELINE_STAGE_2_COPY_BIT, 0,
              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0);
    m.wristContent = true;
}

// #1076: the UI wall's geometry, one definition for the layers and the pointer. Local frame:
// origin on the wall's vertical axis at its centre height, -Z towards the wall's centre, +X to
// the right as seen from the axis.
namespace
{
struct WallGeom
{
    float radius = LLXrSession::kWallRadius;
    float angle = LLXrSession::kWallAngle;
    float height = 1.f;   // arc length / aspect
};

WallGeom wallGeom(const LLXrUiLayers& ui)
{
    WallGeom g;
    const float aspect = std::isfinite(ui.wallAspect) ? std::clamp(ui.wallAspect, 0.5f, 4.f) : 16.f / 9.f;
    g.height = g.radius * g.angle / aspect;
    return g;
}

// Rotation about +Y by a (right-handed, OpenXR): (x cos a + z sin a, y, -x sin a + z cos a).
void rotY(float a, const float v[3], float out[3])
{
    const float c = std::cos(a), s = std::sin(a);
    const float x = v[0] * c + v[2] * s, z = -v[0] * s + v[2] * c;
    out[0] = x;
    out[1] = v[1];
    out[2] = z;
}

XrQuaternionf quatY(float a)
{
    return XrQuaternionf{0.f, std::sin(0.5f * a), 0.f, std::cos(0.5f * a)};
}

// #1076 diagnostics: one submitted quad layer, exactly.
void logQuad(const char* what, const XrCompositionLayerQuad& q)
{
    char buf[384];
    std::snprintf(buf, sizeof(buf),
                  "  %s: pos (%.4f %.4f %.4f) quat (%.4f %.4f %.4f %.4f) size %.4f x %.4f m, rect (%d,%d %dx%d), flags 0x%llx",
                  what, q.pose.position.x, q.pose.position.y, q.pose.position.z, q.pose.orientation.x, q.pose.orientation.y,
                  q.pose.orientation.z, q.pose.orientation.w, q.size.width, q.size.height, q.subImage.imageRect.offset.x,
                  q.subImage.imageRect.offset.y, q.subImage.imageRect.extent.width, q.subImage.imageRect.extent.height,
                  (unsigned long long)q.layerFlags);
    LL_INFOS("VR") << buf << LL_ENDL;
}

// Facet i of n: centre angle (radians, + = right), chord half-angle.
void facet(const WallGeom& g, uint32_t i, uint32_t n, float& thetaC, float& halfD)
{
    halfD = 0.5f * g.angle / (float)n;
    thetaC = -0.5f * g.angle + (2.f * (float)i + 1.f) * halfD;
}
} // namespace

bool LLXrSession::wallHit(const float origin[3], const float dir[3], float& u, float& v, float& dist) const
{
    if (!mImpl || !mUi.wall) return false;
    const WallGeom g = wallGeom(mUi);
    // Into the wall's local frame.
    const float rel[3] = {origin[0] - mUi.wallAxis[0], origin[1] - mUi.wallAxis[1], origin[2] - mUi.wallAxis[2]};
    float o[3], d[3];
    rotY(-mUi.wallYaw, rel, o);
    rotY(-mUi.wallYaw, dir, d);
    if (mUi.debugLayers == 2u)
    {
        // The diagnostic flat quad (endFrameNow): z = -R, facing +Z, 1.6 m wide.
        if (d[2] >= -1e-6f) return false;
        const float t = (-g.radius - o[2]) / d[2];
        if (!(t > 0.f)) return false;
        const float aspect = std::isfinite(mUi.wallAspect) ? std::clamp(mUi.wallAspect, 0.5f, 4.f) : 16.f / 9.f;
        const float qw = 1.6f, qh = qw / aspect;
        u = 0.5f + (o[0] + t * d[0]) / qw;
        v = 0.5f - (o[1] + t * d[1]) / qh;
        dist = t;
        return u >= 0.f && u <= 1.f && v >= 0.f && v <= 1.f;
    }
    if (mImpl->cylinderExt)
    {
        // x^2 + z^2 = r^2; from inside, the far root.
        const float a = d[0] * d[0] + d[2] * d[2];
        if (a < 1e-8f) return false;
        const float b = 2.f * (o[0] * d[0] + o[2] * d[2]);
        const float c = o[0] * o[0] + o[2] * o[2] - g.radius * g.radius;
        const float disc = b * b - 4.f * a * c;
        if (disc < 0.f) return false;
        const float t = (-b + std::sqrt(disc)) / (2.f * a);
        if (!(t > 0.f)) return false;
        const float p[3] = {o[0] + t * d[0], o[1] + t * d[1], o[2] + t * d[2]};
        const float theta = std::atan2(p[0], -p[2]);
        u = 0.5f + theta / g.angle;
        v = 0.5f - p[1] / g.height;
        dist = t;
        return u >= 0.f && u <= 1.f && v >= 0.f && v <= 1.f;
    }
    // The flat facets, exactly as submitted: the nearest one the ray enters from the front.
    bool hit = false;
    for (uint32_t i = 0; i < kWallFacets; ++i)
    {
        float thetaC, halfD;
        facet(g, i, kWallFacets, thetaC, halfD);
        const float rc = g.radius * std::cos(halfD);
        const float c[3] = {rc * std::sin(thetaC), 0.f, -rc * std::cos(thetaC)};
        const float nrm[3] = {-std::sin(thetaC), 0.f, std::cos(thetaC)};   // towards the axis
        const float tan[3] = {std::cos(thetaC), 0.f, std::sin(thetaC)};    // the facet's +X
        const float dn = d[0] * nrm[0] + d[2] * nrm[2];
        if (dn >= -1e-6f) continue;   // parallel, or from behind
        const float t = ((c[0] - o[0]) * nrm[0] + (c[2] - o[2]) * nrm[2]) / dn;
        if (!(t > 0.f) || (hit && t >= dist)) continue;
        const float p[3] = {o[0] + t * d[0], o[1] + t * d[1], o[2] + t * d[2]};
        const float w = 2.f * g.radius * std::sin(halfD);
        const float lx = (p[0] - c[0]) * tan[0] + (p[2] - c[2]) * tan[2];
        if (std::fabs(lx) > 0.5f * w || std::fabs(p[1]) > 0.5f * g.height) continue;
        u = ((float)i + lx / w + 0.5f) / (float)kWallFacets;
        v = 0.5f - p[1] / g.height;
        dist = t;
        hit = true;
    }
    return hit;
}

void LLXrSession::endFrame()
{
    if (!mImpl) return;
    endFrameNow(true);
}

void LLXrSession::endFrameNow(bool withLayers)
{
    Impl& m = *mImpl;
    // (#1137: a waited frame not begun yet -- late begin, or an early-out -- is begun here: the
    //  runtime needs every xrWaitFrame answered by xrBeginFrame + xrEndFrame)
    if (!m.frameBegun && !(m.frameWaited && ensureBegun())) return;
    pacingFrameEnd();
    // #1076 governor: a frame that rendered is our cost; an early-out's zero-layer end is not.
    if (withLayers && m.workStart != Clock::time_point{})
        m.workSec = std::chrono::duration<double>(Clock::now() - m.workStart).count();
    const LLXrFns& f = LLXrLoader::fn();
    // Release what was acquired (after the frame's submit: the runtime orders its use after our
    // work on the same queue). (#1075: + the depth swapchains of a stereo session.)
    XrSwap* swaps[10] = {&m.eyes[0], &m.eyes[1], &m.ui, &m.fade, &m.depth[0], &m.depth[1], &m.vig, &m.door, &m.mist, &m.wrist};
    for (XrSwap* s : swaps)
    {
        if (s->acquired == UINT32_MAX || s->handle == XR_NULL_HANDLE) continue;
        XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        f.releaseSwapchainImage(s->handle, &rel);
        s->acquired = UINT32_MAX;
    }
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    // #1076: projection + (#1078) the vignette + the fade + (#1076) the login room (2 mist, up
    // to 2 doorway) + the wall (one cylinder or kWallFacets quads) + the menu panel + (#1120) the
    // wrist panel's own quad: at most 9 + kWallFacets layers, 8 + kWallFacets quads (one spare each).
    static constexpr uint32_t kMaxLayers = 10 + kWallFacets;
    XrCompositionLayerQuad quads[kWallFacets + 9];
    XrCompositionLayerCylinderKHR cyl{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
    const XrCompositionLayerBaseHeader* layers[kMaxLayers] = {};
    uint32_t layerCount = 0, quadCount = 0;
    // #1076: log the exact layers once after every wall placement / diagnostic mode change.
    const bool logLayers = withLayers && m.recorded && m.uiRecorded && mUi.wall &&
                           (mUi.placeSerial != m.layersLoggedSerial || mUi.debugLayers != m.layersLoggedMode);
    if (logLayers)
    {
        m.layersLoggedSerial = mUi.placeSerial;
        m.layersLoggedMode = mUi.debugLayers;
    }
    if (withLayers && m.recorded)
    {
        proj.space = m.appSpace;
        proj.viewCount = m.eyeCount;
        proj.views = m.projViews;
        // (SlipstreamVRDebugLayers 1: the wall layers alone, no world behind them)
        const bool wallOnly = mUi.debugLayers == 1u && m.uiRecorded && mUi.wall;
        if (!wallOnly) layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
        // #1078: the comfort vignette over the world, under the UI layers (the wrist panel and
        // the wall stay readable): head-locked, kVigDist away, past the edges of the view.
        if (m.vigRecorded)
        {
            XrCompositionLayerQuad& q = quads[quadCount++];
            q = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            q.space = m.viewSpace;
            q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            q.subImage.swapchain = m.vig.handle;
            q.subImage.imageRect = {{0, 0}, {(int32_t)m.vig.width, (int32_t)m.vig.height}};
            q.subImage.imageArrayIndex = 0;
            q.pose.orientation.w = 1.0f;
            q.pose.position = {0.0f, 0.0f, -kVigDist};
            q.size.width = q.size.height = 2.f * kVigHalfTan * kVigDist;
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q);
        }
        // The comfort fade over the world (#1078: under the UI layers -- they are room-locked and
        // never jump, and a teleport's fade keeps the wall / wrist panel readable): head-locked,
        // 0.25 m away, 2 m square (covers the whole field of view).
        if (m.fadeRecorded)
        {
            XrCompositionLayerQuad& q = quads[quadCount++];
            q = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            q.space = m.viewSpace;
            q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            q.subImage.swapchain = m.fade.handle;
            q.subImage.imageRect = {{0, 0}, {(int32_t)m.fade.width, (int32_t)m.fade.height}};
            q.subImage.imageArrayIndex = 0;
            q.pose.orientation.w = 1.0f;
            q.pose.position = {0.0f, 0.0f, -0.25f};
            q.size.width = q.size.height = 2.0f;
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q);
        }
        // #1076 the login room, under the UI wall: the floor mist (two horizontal sheets, the
        // upper one turned for a less regular pattern) and the doorway (twice when hovered:
        // brighter).
        if (m.lobbyRecorded && mLobby.on && m.door.handle != XR_NULL_HANDLE && m.mist.handle != XR_NULL_HANDLE)
        {
            for (int sheet = 0; sheet < 2; ++sheet)
            {
                XrCompositionLayerQuad& q = quads[quadCount++];
                q = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
                q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                q.space = m.appSpace;
                q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                q.subImage.swapchain = m.mist.handle;
                q.subImage.imageRect = {{0, 0}, {(int32_t)m.mist.width, (int32_t)m.mist.height}};
                q.subImage.imageArrayIndex = 0;
                // Lying flat (+Z up: -90 degrees about X), the upper sheet also turned about +Y.
                const float yaw = sheet ? 1.1f : 0.f;
                const XrQuaternionf flat{-0.70710678f, 0.f, 0.f, 0.70710678f};
                const XrQuaternionf turn = quatY(yaw);
                // turn * flat (Hamilton product)
                q.pose.orientation = {turn.w * flat.x + turn.x * flat.w + turn.y * flat.z - turn.z * flat.y,
                                      turn.w * flat.y - turn.x * flat.z + turn.y * flat.w + turn.z * flat.x,
                                      turn.w * flat.z + turn.x * flat.y - turn.y * flat.x + turn.z * flat.w,
                                      turn.w * flat.w - turn.x * flat.x - turn.y * flat.y - turn.z * flat.z};
                q.pose.position = {mLobby.floorCenter[0], mLobby.floorCenter[1] + (sheet ? 0.35f : 0.04f), mLobby.floorCenter[2]};
                q.size.width = q.size.height = sheet ? kMistSize * 0.7f : kMistSize;
                layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q);
            }
            for (int copy = 0; copy < (mLobby.doorHover ? 2 : 1); ++copy)
            {
                XrCompositionLayerQuad& q = quads[quadCount++];
                q = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
                q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                q.space = m.appSpace;
                q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                q.subImage.swapchain = m.door.handle;
                q.subImage.imageRect = {{0, 0}, {(int32_t)m.door.width, (int32_t)m.door.height}};
                q.subImage.imageArrayIndex = 0;
                q.pose.orientation = quatY(mLobby.doorYaw);
                q.pose.position = {mLobby.doorCenter[0], mLobby.doorCenter[1], mLobby.doorCenter[2]};
                // The image is the door plus its halo margin (128 x 256 texels for 100 x 220).
                q.size.width = kDoorWidth * 128.f / 100.f;
                q.size.height = kDoorHeight * 256.f / 220.f;
                layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q);
            }
        }
        if (logLayers)
        {
            LL_INFOS("VR") << "layers (debug mode " << mUi.debugLayers << "): projection layer "
                           << (wallOnly ? "OMITTED" : "submitted") << ", app space type " << (int)m.appSpaceType << LL_ENDL;
            const double deg = 180.0 / 3.14159265358979;
            for (uint32_t i = 0; i < m.eyeCount; ++i)
            {
                const XrCompositionLayerProjectionView& pv = m.projViews[i];
                const XrView& lv = m.located[i];
                char buf[512];
                std::snprintf(buf, sizeof(buf),
                              "  eye %u declared: pos (%.4f %.4f %.4f) quat (%.4f %.4f %.4f %.4f) fov L%.2f R%.2f U%.2f D%.2f, "
                              "rect (%d,%d %dx%d); located: quat (%.4f %.4f %.4f %.4f) fov L%.2f R%.2f U%.2f D%.2f",
                              i, pv.pose.position.x, pv.pose.position.y, pv.pose.position.z, pv.pose.orientation.x,
                              pv.pose.orientation.y, pv.pose.orientation.z, pv.pose.orientation.w, pv.fov.angleLeft * deg,
                              pv.fov.angleRight * deg, pv.fov.angleUp * deg, pv.fov.angleDown * deg,
                              pv.subImage.imageRect.offset.x, pv.subImage.imageRect.offset.y,
                              pv.subImage.imageRect.extent.width, pv.subImage.imageRect.extent.height, lv.pose.orientation.x,
                              lv.pose.orientation.y, lv.pose.orientation.z, lv.pose.orientation.w, lv.fov.angleLeft * deg,
                              lv.fov.angleRight * deg, lv.fov.angleUp * deg, lv.fov.angleDown * deg);
                LL_INFOS("VR") << buf << LL_ENDL;
            }
        }
        if (m.uiRecorded)
        {
            const XrCompositionLayerFlags flags = m.uiAlpha ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
            const int32_t iw = (int32_t)m.uiCopyW, ih = (int32_t)m.uiCopyH;
            if (mUi.wall && mUi.debugLayers == 2u)
            {
                // Diagnostic: the whole UI image on ONE flat quad, 1.6 m wide, at the wall's
                // centre (where facet 3 of 7 is), facing the wall's axis.
                const WallGeom g = wallGeom(mUi);
                const float c[3] = {0.f, 0.f, -g.radius};
                float cw[3];
                rotY(mUi.wallYaw, c, cw);
                XrCompositionLayerQuad& q = quads[quadCount++];
                q = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
                q.layerFlags = flags;
                q.space = m.appSpace;
                q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                q.subImage.swapchain = m.ui.handle;
                q.subImage.imageRect = {{0, 0}, {iw, ih}};
                q.subImage.imageArrayIndex = 0;
                q.pose.orientation = quatY(mUi.wallYaw);
                q.pose.position = {mUi.wallAxis[0] + cw[0], mUi.wallAxis[1] + cw[1], mUi.wallAxis[2] + cw[2]};
                q.size.width = 1.6f;
                q.size.height = 1.6f * (float)ih / (float)std::max(1, iw);
                layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q);
                if (logLayers) logQuad("test quad", q);
            }
            else if (mUi.wall)
            {
                // The UI wall, world-locked in the app space (geometry: wallGeom, as wallHit).
                const WallGeom g = wallGeom(mUi);
                const XrVector3f axis{mUi.wallAxis[0], mUi.wallAxis[1], mUi.wallAxis[2]};
                if (m.cylinderExt)
                {
                    cyl.layerFlags = flags;
                    cyl.space = m.appSpace;
                    cyl.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    cyl.subImage.swapchain = m.ui.handle;
                    cyl.subImage.imageRect = {{0, 0}, {iw, ih}};
                    cyl.subImage.imageArrayIndex = 0;
                    cyl.pose.orientation = quatY(mUi.wallYaw);
                    cyl.pose.position = axis;
                    cyl.radius = g.radius;
                    cyl.centralAngle = g.angle;
                    cyl.aspectRatio = (float)iw / (float)std::max(1, ih);
                    layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cyl);
                }
                else
                {
                    // Column strips of the image on flat quads along the arc (chords).
                    for (uint32_t i = 0; i < kWallFacets; ++i)
                    {
                        float thetaC, halfD;
                        facet(g, i, kWallFacets, thetaC, halfD);
                        const float rc = g.radius * std::cos(halfD);
                        const float c[3] = {rc * std::sin(thetaC), 0.f, -rc * std::cos(thetaC)};
                        float cw[3];
                        rotY(mUi.wallYaw, c, cw);
                        const int32_t x0 = (int32_t)((int64_t)iw * i / kWallFacets);
                        const int32_t x1 = (int32_t)((int64_t)iw * (i + 1) / kWallFacets);
                        XrCompositionLayerQuad& q = quads[quadCount++];
                        q = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
                        q.layerFlags = flags;
                        q.space = m.appSpace;
                        q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                        q.subImage.swapchain = m.ui.handle;
                        q.subImage.imageRect = {{x0, 0}, {std::max(1, x1 - x0), ih}};
                        q.subImage.imageArrayIndex = 0;
                        q.pose.orientation = quatY(mUi.wallYaw - thetaC);
                        q.pose.position = {axis.x + cw[0], axis.y + cw[1], axis.z + cw[2]};
                        q.size.width = 2.f * g.radius * std::sin(halfD);
                        q.size.height = g.height;
                        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q);
                        if (logLayers) logQuad(i == 0 ? "facet 0 (left)" : i + 1 == kWallFacets ? "facet 6 (right)" : "facet", q);
                    }
                }
            }
            else if (!m.uiAlpha)
            {
                // Not placed yet (the first frames of the session): head-locked, 2 m in front,
                // 1.6 m wide (~44 degrees), the image's aspect.
                XrCompositionLayerQuad& q = quads[quadCount++];
                q = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
                q.layerFlags = flags;
                q.space = m.viewSpace;
                q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                q.subImage.swapchain = m.ui.handle;
                q.subImage.imageRect = {{0, 0}, {iw, ih}};
                q.subImage.imageArrayIndex = 0;
                q.pose.orientation.w = 1.0f;
                q.pose.position = {0.0f, 0.0f, -2.0f};
                q.size.width = 1.6f;
                q.size.height = 1.6f * (float)ih / (float)std::max(1, iw);
                layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q);
            }
            // #1134 the floating menu panel: the open context menu's part of the UI image on a
            // flat quad world-locked in the app space (facing the viewer, placed by LLVRInput).
            if (mUi.panel && m.uiAlpha && mUi.panelSize[0] > 0.f && mUi.panelSize[1] > 0.f &&
                std::isfinite(mUi.panelCenter[0]) && std::isfinite(mUi.panelCenter[1]) && std::isfinite(mUi.panelCenter[2]))
            {
                XrCompositionLayerQuad& q = quads[quadCount++];
                q = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
                q.layerFlags = flags;
                q.space = m.appSpace;
                q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                q.subImage.swapchain = m.ui.handle;
                q.subImage.imageRect = m.panelRect;
                q.subImage.imageArrayIndex = 0;
                q.pose.orientation = quatY(mUi.panelYaw);
                q.pose.position = {mUi.panelCenter[0], mUi.panelCenter[1], mUi.panelCenter[2]};
                q.size.width = mUi.panelSize[0];
                q.size.height = mUi.panelSize[1];
                layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q);
            }
        }
        // #1120 the wrist panel: its own swapchain (the last released image), opaque, in the
        // left aim pose's space at the pose LLVRWrist gave (the runtime keeps it on the hand).
        // Over the wall and the menu panel: it is the nearest thing to the eyes when shown.
        if (mUi.wrist && m.wristContent && m.wrist.handle != XR_NULL_HANDLE &&
            m.aimSpace[LLXR_HAND_LEFT] != XR_NULL_HANDLE && mUi.wristSize[0] > 0.f && mUi.wristSize[1] > 0.f &&
            std::isfinite(mUi.wristPos[0]) && std::isfinite(mUi.wristPos[1]) && std::isfinite(mUi.wristPos[2]) &&
            std::isfinite(mUi.wristOri[3]))
        {
            XrCompositionLayerQuad& q = quads[quadCount++];
            q = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            q.layerFlags = 0;
            q.space = m.aimSpace[LLXR_HAND_LEFT];
            q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            q.subImage.swapchain = m.wrist.handle;
            q.subImage.imageRect = {{0, 0}, {(int32_t)m.wrist.width, (int32_t)m.wrist.height}};
            q.subImage.imageArrayIndex = 0;
            q.pose.orientation = {mUi.wristOri[0], mUi.wristOri[1], mUi.wristOri[2], mUi.wristOri[3]};
            q.pose.position = {mUi.wristPos[0], mUi.wristPos[1], mUi.wristPos[2]};
            q.size.width = mUi.wristSize[0];
            q.size.height = mUi.wristSize[1];
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q);
        }
    }
    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime = m.fs.predictedDisplayTime;
    fei.environmentBlendMode = m.blend;
    fei.layerCount = layerCount;
    fei.layers = layerCount ? layers : nullptr;
    const XrResult r = f.endFrame(m.session, &fei);
    if (XR_FAILED(r) && m.endFailLogs < 10)
    {
        ++m.endFailLogs;
        LL_WARNS("VR") << "xrEndFrame (" << layerCount << " layers) -> " << llxrResultText(m.inst, r) << LL_ENDL;
    }
    m.frameBegun = m.frameWaited = false;
    m.recorded = m.uiRecorded = m.fadeRecorded = m.vigRecorded = false;
    ++m.frames;
}

// ---------------------------------------------------------------------------------------------
// 5. Shutdown.
// ---------------------------------------------------------------------------------------------
void LLXrSession::destroySessionObjects()
{
    Impl& m = *mImpl;
    const LLXrFns& f = LLXrLoader::fn();
    m.frameBegun = m.frameWaited = m.recorded = m.uiRecorded = false;
    XrSwap* swaps[10] = {&m.eyes[0], &m.eyes[1], &m.ui, &m.fade, &m.depth[0], &m.depth[1], &m.vig, &m.door, &m.mist, &m.wrist};
    m.wristContent = m.wristFailed = false;   // (#1120: a new session tries again)
    mWristSrc = VK_NULL_HANDLE;
    m.stereoOn = m.eyeArray = m.depthRecorded = false;
    // #1078: the vignette's staging buffer (the device is idle here: shutdownSession).
    if (m.vigBuf.buffer && m.dev) m.dev->destroyBuffer(m.vigBuf);
    m.vigBuf = GpuBuffer{};
    m.vigFailed = m.vigRecorded = false;
    m.vigBuiltLevel = -1;
    // #1076: the login room's staging buffer.
    if (m.lobbyBuf.buffer && m.dev) m.dev->destroyBuffer(m.lobbyBuf);
    m.lobbyBuf = GpuBuffer{};
    m.lobbyFailed = m.lobbyWritten = m.lobbyRecorded = false;
    for (std::vector<float>& v : m.visMask) v.clear();   // (#1075)
    m.visMaskDirty = false;
    ++m.visMaskGen;
    for (XrSwap* s : swaps)
    {
        for (VkImageView v : s->views)
            if (v && m.vkDev) vkDestroyImageView(m.vkDev, v, nullptr);
        if (s->handle != XR_NULL_HANDLE && f.destroySwapchain) f.destroySwapchain(s->handle);
        *s = XrSwap{};
    }
    // #1076: the controller spaces, then the action set (destroys its actions).
    for (uint32_t h = 0; h < LLXR_HAND_COUNT; ++h)
    {
        if (m.gripSpace[h] != XR_NULL_HANDLE && f.destroySpace) f.destroySpace(m.gripSpace[h]);
        if (m.aimSpace[h] != XR_NULL_HANDLE && f.destroySpace) f.destroySpace(m.aimSpace[h]);
        m.gripSpace[h] = m.aimSpace[h] = XR_NULL_HANDLE;
    }
    if (m.actionSet != XR_NULL_HANDLE && f.destroyActionSet) f.destroyActionSet(m.actionSet);
    m.actionSet = XR_NULL_HANDLE;
    m.actGrip = m.actAim = m.actSelect = m.actContext = m.actMenu = XR_NULL_HANDLE;
    m.actFlyCam = m.actStick = m.actStickClick = m.actHaptic = m.actSelectValue = XR_NULL_HANDLE;
    m.actionsAttached = false;
    m.input = LLXrInputState{};
    if (m.viewSpace != XR_NULL_HANDLE && f.destroySpace) f.destroySpace(m.viewSpace);
    if (m.appSpace != XR_NULL_HANDLE && f.destroySpace) f.destroySpace(m.appSpace);
    m.viewSpace = m.appSpace = XR_NULL_HANDLE;
    m.locatedOk = false;
    m.head = LLXrHeadPose{};
    if (m.session != XR_NULL_HANDLE && f.destroySession)
    {
        const XrResult r = f.destroySession(m.session);
        LL_INFOS("VR") << "xrDestroySession -> " << llxrResultText(m.inst, r) << " (" << m.frames << " frames, "
                       << m.rendered << " with layers)" << LL_ENDL;
    }
    m.session = XR_NULL_HANDLE;
    m.state = XR_SESSION_STATE_UNKNOWN;
    if (m.running)
    {
        m.running = false;
        if (mRunningCb) mRunningCb(false);
    }
}

void LLXrSession::shutdownSession()
{
    if (!mImpl || mImpl->session == XR_NULL_HANDLE) return;
    Impl& m = *mImpl;
    const LLXrFns& f = LLXrLoader::fn();
    if (m.frameBegun || m.frameWaited) endFrameNow(false);
    if (m.running)
    {
        // Ask the runtime to stop and wait briefly for STOPPING (pollEvents ends the session).
        const XrResult r = f.requestExitSession(m.session);
        LL_INFOS("VR") << "xrRequestExitSession -> " << llxrResultText(m.inst, r) << LL_ENDL;
        const Clock::time_point t = Clock::now();
        while (m.running && m.session != XR_NULL_HANDLE && msSince(t) < 1000.0)
        {
            pollEvents();
            if (m.running) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    if (m.session != XR_NULL_HANDLE)
    {
        if (m.vkDev) vkDeviceWaitIdle(m.vkDev);
        destroySessionObjects();
    }
    mRunningCb = nullptr;
}

void LLXrSession::shutdownInstance()
{
    sActive = false;
    if (!mImpl) return;
    Impl& m = *mImpl;
    if (m.worker.joinable()) m.worker.join();
    if (m.inst != XR_NULL_HANDLE && LLXrLoader::fn().destroyInstance)
    {
        const XrResult r = LLXrLoader::fn().destroyInstance(m.inst);
        LL_INFOS("VR") << "xrDestroyInstance -> " << llxrResultText(XR_NULL_HANDLE, r) << LL_ENDL;
    }
    m.inst = XR_NULL_HANDLE;
    LLXrLoader::forgetInstance();
}
