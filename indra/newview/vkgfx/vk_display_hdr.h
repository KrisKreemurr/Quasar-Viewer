// Current Vulkan display-output contract. Independent of scene HDR and render2.
#pragma once
#include "vk_display_limits.h"
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

namespace vkdisplay {
enum class Encoding : uint8_t { Sdr, Hdr10, ScRgb };
enum class Platform : uint8_t { Unsupported, Windows, Wayland, X11 };
enum class Reason : uint8_t {
    Disabled, Applied, X11Unsupported, PlatformUnsupported, OsHdrDisabled,
    ColourSpaceExtensionUnavailable, SurfaceUnsupported, PipelineUnavailable,
    ResourcesUnavailable, BudgetExceeded, SurfaceQueryFailed, DisplayUnavailable
};
struct Request {
    bool enabled = false;
    float paperWhiteNits = 203.f;
    float peakNits = 1000.f;
};
inline Request sanitized(Request r) noexcept {
    if (!std::isfinite(r.paperWhiteNits)) r.paperWhiteNits = 203.f;
    if (!std::isfinite(r.peakNits)) r.peakNits = 1000.f;
    r.peakNits = std::clamp(r.peakNits, 80.f, 10000.f);
    r.paperWhiteNits = std::clamp(r.paperWhiteNits, 80.f, std::min(kDisplayReferenceWhiteMaxNits, r.peakNits));
    return r;
}
struct Capability {
    Platform platform = Platform::Unsupported;
    bool colourSpaceExtension = false;
    bool osHdrEnabled = false; // Windows only; Wayland's advertised pairs are authoritative.
    bool displayAvailable = true;
    uint32_t displayId = 0;
};
struct Status {
    bool requested = false;
    bool displayCapable = false; // Independent of request and temporary pipeline/allocation failure.
    Encoding encoding = Encoding::Sdr;
    Reason reason = Reason::Disabled;
    Reason supportReason = Reason::PlatformUnsupported;
    VkSurfaceFormatKHR actual{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    float paperWhiteNits = 203.f;
    float peakNits = 1000.f;
    uint32_t displayId = 0;
};
inline Encoding encodingOf(VkSurfaceFormatKHR f) noexcept {
    if (f.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 && f.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT)
        return Encoding::Hdr10;
    if (f.format == VK_FORMAT_R16G16B16A16_SFLOAT && f.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT)
        return Encoding::ScRgb;
    return Encoding::Sdr;
}
// Called only at startup, display change or explicit preference change; no per-frame query.
// Caller passes the actual successful bounded enumeration, never a partially filled array.
inline Status choose(Request request, Capability cap, std::span<const VkSurfaceFormatKHR> formats,
                     VkSurfaceFormatKHR sdr, bool linearPipelineReady) noexcept {
    request = sanitized(request);
    Status s; s.requested = request.enabled; s.actual = sdr;
    s.displayId = cap.displayId;
    s.paperWhiteNits = request.paperWhiteNits; s.peakNits = request.peakNits;
    Reason unavailable = Reason::SurfaceUnsupported;
    if (!cap.displayAvailable) unavailable = Reason::DisplayUnavailable;
    else if (cap.platform == Platform::X11) unavailable = Reason::X11Unsupported;
    else if (cap.platform != Platform::Windows && cap.platform != Platform::Wayland) unavailable = Reason::PlatformUnsupported;
    else if (!cap.colourSpaceExtension) unavailable = Reason::ColourSpaceExtensionUnavailable;
    else if (cap.platform == Platform::Windows && !cap.osHdrEnabled) unavailable = Reason::OsHdrDisabled;
    else if (formats.empty() || formats.size() > 256) unavailable = Reason::SurfaceQueryFailed;
    else {
        VkSurfaceFormatKHR selected{};
        for (Encoding wanted : {Encoding::Hdr10, Encoding::ScRgb}) {
            for (auto f : formats) if (encodingOf(f) == wanted) { selected = f; break; }
            if (selected.format != VK_FORMAT_UNDEFINED) break;
        }
        s.displayCapable = selected.format != VK_FORMAT_UNDEFINED;
        if (s.displayCapable) s.supportReason = Reason::Applied;
        if (s.displayCapable && request.enabled) {
            if (!linearPipelineReady) s.reason = Reason::PipelineUnavailable;
            else { s.actual = selected; s.encoding = encodingOf(selected); s.reason = Reason::Applied; }
            return s;
        }
    }
    s.reason = request.enabled ? unavailable : Reason::Disabled;
    if (!s.displayCapable) s.supportReason = unavailable;
    return s;
}
inline const char* reasonText(Reason r) noexcept {
    switch (r) {
    case Reason::Disabled: return "HDR display is off";
    case Reason::Applied: return "HDR display active";
    case Reason::X11Unsupported: return "HDR display is unavailable on X11";
    case Reason::PlatformUnsupported: return "HDR display is unavailable on this window system";
    case Reason::OsHdrDisabled: return "Enable HDR in Windows display settings";
    case Reason::ColourSpaceExtensionUnavailable: return "Vulkan extended swapchain colour spaces unavailable";
    case Reason::SurfaceUnsupported: return "Display surface offers no supported HDR format and colour space";
    case Reason::PipelineUnavailable: return "HDR display composition is unavailable; using SDR";
    case Reason::ResourcesUnavailable: return "HDR display allocation failed; using SDR";
    case Reason::BudgetExceeded: return "HDR display exceeds its memory budget; using SDR";
    case Reason::SurfaceQueryFailed: return "HDR display surface query failed or exceeded its bound; using SDR";
    case Reason::DisplayUnavailable: return "Window display is unavailable; using SDR";
    }
    return "HDR display unavailable";
}
// Admission covers ALL caller-owned copies, including retired/in-flight images. No extent cap
// is inferred from desktop resolution. Vulkan measured requirements are checked again before allocation.
inline bool imageBudget(uint32_t width, uint32_t height, uint32_t liveCopies,
                        uint64_t measuredBytesPerImage, uint64_t budgetBytes) noexcept {
    if (!width || !height || !liveCopies || liveCopies > 3 || !measuredBytesPerImage) return false;
    const uint64_t logical = uint64_t(width) * height;
    if (logical > UINT64_MAX / 8 || measuredBytesPerImage < logical * 8) return false;
    return measuredBytesPerImage <= budgetBytes / liveCopies;
}
// Absolute luminance input/output: Rec.709 linear nits, no SDR/PQ bytes accepted here.
struct LinearNits { float red, green, blue; };
inline float mapScenePeak(float exposedScenePeak, float sceneWhite,
                          float peakOverPaperWhite) noexcept;
inline float pqFromNits(float nits) noexcept {
    if (!std::isfinite(nits)) nits = 0.f;
    const float y = std::pow(std::clamp(nits / 10000.f, 0.f, 1.f), 2610.f / 16384.f);
    return std::pow((3424.f / 4096.f + (2413.f / 128.f) * y) / (1.f + (2392.f / 128.f) * y), 2523.f / 32.f);
}
// Hue-preserving shoulder, diffuse white maps exactly to paper-white; above it extends
// smoothly toward the display peak. Tone before output transfer, not after UI composition.
inline LinearNits toneScene(LinearNits scene, Request request, float sceneWhite = 6.f) noexcept {
    request = sanitized(request);
    auto clean=[](float x) { return std::isfinite(x) ? (std::max)(x, 0.f) : 0.f; };
    scene = {clean(scene.red), clean(scene.green), clean(scene.blue)};
    const float intensity = (std::max)({scene.red, scene.green, scene.blue});
    if (intensity == 0.f) return {};
    const float mapped = mapScenePeak(intensity, sceneWhite, request.peakNits / request.paperWhiteNits);
    const float scale = request.paperWhiteNits * (mapped / intensity);
    return {scene.red * scale, scene.green * scale, scene.blue * scale};
}
// Absolute HDR scene luminance in paper-white units. The SDR tone curve still
// supplies chroma; this is the single bounded luminance shoulder used by the
// HDR output shader and by its encode diagnostics.
inline float mapScenePeak(float exposedScenePeak, float sceneWhite,
                          float peakOverPaperWhite) noexcept {
    if (!std::isfinite(exposedScenePeak)) exposedScenePeak = 0.f;
    if (!std::isfinite(sceneWhite)) sceneWhite = 1.f;
    if (!std::isfinite(peakOverPaperWhite)) peakOverPaperWhite = 1.f;
    const float knee = std::max(sceneWhite, 1.f);
    const float ratio = std::max(peakOverPaperWhite, 1.f);
    if (exposedScenePeak <= 1.f || ratio <= 1.f) return std::clamp(exposedScenePeak, 0.f, 1.f);
    const float room = ratio - 1.f;
    const float mapped = 1.f + room * (1.f - std::exp(-(exposedScenePeak - 1.f) / (2.f * knee)));
    return std::clamp(mapped, 0.f, ratio);
}
inline LinearNits encode(LinearNits value, Encoding encoding) noexcept {
    if (encoding == Encoding::Hdr10) {
        const float red = .627404f*value.red + .329283f*value.green + .043313f*value.blue;
        const float green = .069097f*value.red + .919540f*value.green + .011362f*value.blue;
        const float blue = .016391f*value.red + .088013f*value.green + .895595f*value.blue;
        return {pqFromNits(red), pqFromNits(green), pqFromNits(blue)};
    }
    if (encoding == Encoding::ScRgb) return {value.red / 80.f, value.green / 80.f, value.blue / 80.f};
    return value; // SDR transfer is owned by the existing unchanged SDR pipeline.
}
// HDR live snapshots are already display-referred linear nits. Convert their SDR
// portion relative to the same paper white used by UI composition; values beyond
// paper white are clipped just as they would be by the SDR output range.
inline float sdrSnapshotLinear(float nits, float paperWhiteNits) noexcept {
    if (!std::isfinite(nits)) nits = 0.f;
    if (!std::isfinite(paperWhiteNits)) paperWhiteNits = 203.f;
    paperWhiteNits = std::clamp(paperWhiteNits, 80.f, kDisplayReferenceWhiteMaxNits);
    return std::clamp(nits, 0.f, paperWhiteNits) / paperWhiteNits;
}
} // namespace vkdisplay
