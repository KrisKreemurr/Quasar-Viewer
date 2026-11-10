#pragma once

#include "llvktextureprepsource.h"

#include <array>
#include <cstdint>

// Value-only contract between the main-thread texture source producer and the
// renderer-owned preparation tickets. This header deliberately has no Vulkan,
// viewer-texture, GL, or cache implementation dependencies.
namespace LLVKTexturePrep
{
enum class RecipeKind : uint8_t
{
    ConvertPixels,
    ReducePixels,
    CoveragePixels,
    CopyCompressed,
    ConvertPrivateMedia
};

// Compact storage used only by the UI image cache. None preserves the shared
// world's existing RGBA conversion. FontCoverageAuto reproduces the old glyph
// atlas rule: an all-255 luminance channel is stored as coverage-only; otherwise
// keep both luminance and alpha channels.
enum class UiFormat : uint8_t
{
    None,
    RGBA8,
    LA8,
    L8,
    A8,
    Coverage8,
    FontCoverageAuto
};

enum class UiSwizzle : uint8_t
{
    None,
    RGBA,
    LuminanceAlpha,
    Luminance,
    Alpha,
    Coverage
};

// Scalar values match LLPixelFormat/LLPixelType wire enums without making this
// neutral worker contract include viewer or GL headers.
enum class PixelFormat : uint32_t
{
    Red = 0x1903, Alpha = 0x1906, RGB = 0x1907, RGBA = 0x1908,
    Luminance = 0x1909, LuminanceAlpha = 0x190A, BGR = 0x80E0, BGRA = 0x80E1,
    RG = 0x8227, SRGB = 0x8C40, SRGBAlpha = 0x8C42
};
enum class PixelType : uint32_t
{
    UnsignedByte = 0x1401,
    // GStreamer publishes packed RGBA bytes with this GL type. On the wire the
    // producer's byte sequence is already R,G,B,A; accept no other format/type pair.
    Packed8888Rev = 0x8367
};

enum class Priority : uint8_t
{
    Background,
    Visible,
    SelfOrHud
};

enum class QueueResult : uint8_t
{
    Accepted,
    AlreadyQueued,
    Ineligible,
    Deferred
};

struct Recipe
{
    RecipeKind kind = RecipeKind::ConvertPixels;
    uint32_t targetWidth = 0, targetHeight = 0, targetLevels = 0;
    uint32_t payloadLevels = 0, residentTailLevels = 0;
    uint32_t targetStorageFormat = 0;   // final image format, possibly block-compressed
    uint32_t preparedStorageFormat = 0; // bytes written by CPU preparation before any GPU encoder
    uint8_t encode = 0, trialEncode = 0, alphaCutoff = 0;
    bool srgb = false;
    // Non-None selects the compact UI path; shared world recipes keep the RGBA default.
    UiFormat uiFormat = UiFormat::None;
};

struct MipLayout
{
    uint64_t offset = 0, bytes = 0;
    uint32_t width = 0, height = 0;
};

struct OutputLayout
{
    std::array<MipLayout, kMaxMipLevels> mips{};
    uint64_t byteCount = 0;
    uint32_t mipCount = 0;
    // storageFormat describes the actual bytes in the service-owned staging buffer.
    // destinationStorageFormat is the final image format after any acquired-frame GPU encode.
    // Both are scalar VkFormat values without importing Vulkan headers into this contract.
    uint32_t storageFormat = 0, destinationStorageFormat = 0;
    bool constant = false, alphaOpaque = false;
    uint8_t components = 4;
    uint32_t constantRgba = 0;
    uint8_t encode = 0, trialEncode = 0;
    bool srgb = false;
    // UI-only sampled storage/swizzle metadata. None means the ordinary world layout.
    UiFormat uiFormat = UiFormat::None;
    UiSwizzle uiSwizzle = UiSwizzle::None;
};
}
