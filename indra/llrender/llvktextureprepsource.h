#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <memory>

// Value-only contract between the main-thread texture producer and renderer-owned
// preparation jobs. This header intentionally has no viewer, GL, Vulkan, or cache
// implementation dependencies. The owner may retain immutable pixel storage, a private
// patch copy, or an immutable cache blob, but must never retain LLTextureData or a CPU
// lease. The main thread keeps the lifetime lease separately until the job is retired.
namespace LLVKTexturePrep
{
constexpr uint32_t kMaxInputSpans = 32;
constexpr uint32_t kMaxMipLevels = 32;

enum class SourceKind : uint8_t
{
    PixelBytes,
    CompressedLevels
};

enum class SpanLayout : uint8_t
{
    Rows,
    MipChain
};

enum class CaptureResult : uint8_t
{
    Ready,
    Pending,
    Deferred,
    Invalid
};

struct CaptureBudget
{
    uint64_t maxRetainedBytes = 0;
    uint64_t maxMainCopyBytes = 0;
};

struct InputSpan
{
    const uint8_t* data = nullptr;
    uint64_t bytes = 0;
    uint32_t rowBytes = 0;
    uint32_t x = 0, y = 0, width = 0, height = 0;
    uint32_t level = 0, levelCount = 1;
    SpanLayout layout = SpanLayout::Rows;
};

struct SourceSnapshot
{
    // Keeps every span alive and immutable for the entire worker read. This is source
    // storage only: never put LLTextureData, a viewer object, or LLVKCpuLease here.
    std::shared_ptr<const void> owner;
    std::array<InputSpan, kMaxInputSpans> spans{};
    uint64_t byteCount = 0;             // bytes addressed by spans
    uint64_t retainedBackingBytes = 0;  // complete retained allocation capacity, not just spans
    uint64_t mainCopyBytes = 0;         // bytes copied on main to publish mutable source data
    uint64_t mainCopyCpuNs = 0;         // measured thread CPU for that private copy, not wall time
    bool mainCopyCpuValid = true;       // false only when copy work ran but its CPU clock was unavailable
    uint64_t serial = 0;
    uint32_t generation = 0;
    int32_t baseGeneration = -1;        // accepted generation a dirty-patch source updates
    int32_t discard = -1;
    uint32_t width = 0, height = 0;     // logical uploaded level zero
    uint32_t fullWidth = 0, fullHeight = 0;
    uint32_t levels = 0, firstLevel = 0;
    uint32_t components = 0;
    // Producer-defined scalar format/type codes. Workers treat them as integers; the
    // renderer maps them to backend formats after admission.
    uint32_t pixelFormat = 0, pixelType = 0, storageFormat = 0;
    uint64_t payloadHash = 0;
    uint16_t alphaCommittedSide = 0;
    uint8_t alphaGradientPct = 255;
    uint8_t coverageCut = 0;
    uint8_t encoding = 0, encodingLevel = 0, sourceFlags = 0;
    uint8_t spanCount = 0;
    bool isMask = false;
    SourceKind kind = SourceKind::PixelBytes;

    bool valid() const noexcept
    {
        if (!owner || !spanCount || spanCount > spans.size() || !byteCount ||
            retainedBackingBytes < byteCount || mainCopyBytes > retainedBackingBytes || !serial ||
            !width || !height || !fullWidth || !fullHeight || !levels ||
            fullWidth < width || fullHeight < height || levels > kMaxMipLevels || firstLevel >= levels ||
            components == 0 || components > 4)
            return false;
        if (baseGeneration >= 0 &&
            (kind != SourceKind::PixelBytes || (uint32_t)baseGeneration >= generation))
            return false;

        uint64_t sum = 0;
        uint32_t expectedLevel = firstLevel;
        for (uint32_t i = 0; i < spanCount; ++i)
        {
            const InputSpan& span = spans[i];
            if (!span.data || !span.bytes || !span.width || !span.height || !span.levelCount ||
                span.level >= levels || span.levelCount > levels - span.level ||
                span.bytes > (std::numeric_limits<uint64_t>::max)() - sum)
                return false;
            if (kind == SourceKind::PixelBytes)
            {
                if (span.layout != SpanLayout::Rows || span.level != 0 || span.levelCount != 1 ||
                    !span.rowBytes || (uint64_t)span.x + span.width > width ||
                    (uint64_t)span.y + span.height > height ||
                    span.rowBytes != (uint64_t)span.width * components ||
                    span.bytes != (uint64_t)span.rowBytes * span.height)
                    return false;
            }
            else if (span.layout != SpanLayout::MipChain)
            {
                return false;
            }
            else
            {
                if (span.level != expectedLevel) return false;
                expectedLevel += span.levelCount;
            }
            sum += span.bytes;
        }
        return sum == byteCount;
    }
};
}
