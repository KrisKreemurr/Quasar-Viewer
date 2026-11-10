#include "linden_common.h"

#include "llvktextureprepworker.h"
#include "llcleantexencvk.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>

namespace LLVKTexturePrep
{
namespace
{
uint32_t mipLevels(uint32_t w, uint32_t h) noexcept
{
    uint32_t n = std::max(w, h), levels = 0;
    while (n) { ++levels; n >>= 1; }
    return std::max(levels, 1u);
}

bool mul(uint64_t a, uint64_t b, uint64_t& out) noexcept
{
    if (a && b > (std::numeric_limits<uint64_t>::max)() / a) return false;
    out = a * b;
    return true;
}

bool pixelBytes(const SourceSnapshot& s, uint64_t& out) noexcept
{
    return mul((uint64_t)s.width, (uint64_t)s.height, out)
        && mul(out, 4u, out);
}

uint32_t uiBytesPerPixel(UiFormat format) noexcept
{
    switch (format)
    {
    case UiFormat::RGBA8: return 4;
    case UiFormat::LA8: return 2;
    case UiFormat::L8: case UiFormat::A8: case UiFormat::Coverage8:
    case UiFormat::FontCoverageAuto: return 1;
    default: return 0;
    }
}

uint32_t uiVkFormat(UiFormat format) noexcept
{
    switch (format)
    {
    case UiFormat::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case UiFormat::LA8: return VK_FORMAT_R8G8_UNORM;
    case UiFormat::L8: case UiFormat::A8: case UiFormat::Coverage8:
    case UiFormat::FontCoverageAuto: return VK_FORMAT_R8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}

UiSwizzle uiSwizzle(UiFormat format) noexcept
{
    switch (format)
    {
    case UiFormat::RGBA8: return UiSwizzle::RGBA;
    case UiFormat::LA8: return UiSwizzle::LuminanceAlpha;
    case UiFormat::L8: return UiSwizzle::Luminance;
    case UiFormat::A8: return UiSwizzle::Alpha;
    case UiFormat::Coverage8: return UiSwizzle::Coverage;
    default: return UiSwizzle::None;
    }
}

bool uiSourceRecipeValid(const SourceSnapshot& source, const Recipe& recipe) noexcept
{
    if (!source.valid() || source.kind != SourceKind::PixelBytes || !source.width || !source.height
        || !source.components || source.components > 4 || source.baseGeneration >= 0
        || recipe.kind != RecipeKind::ConvertPixels
        || recipe.srgb || recipe.encode || recipe.trialEncode || recipe.alphaCutoff
        || recipe.payloadLevels || recipe.residentTailLevels || (recipe.targetLevels && recipe.targetLevels != 1)
        || (recipe.targetWidth && recipe.targetWidth != source.width)
        || (recipe.targetHeight && recipe.targetHeight != source.height)
        || source.spanCount != 1) return false;

    const InputSpan& span = source.spans[0];
    if (span.layout != SpanLayout::Rows || span.x || span.y || span.width != source.width
        || span.height != source.height || source.width > (std::numeric_limits<uint64_t>::max)() / source.components
        || span.rowBytes != (uint64_t)source.width * source.components
        || source.height > (std::numeric_limits<uint64_t>::max)() / span.rowBytes
        || span.bytes != span.rowBytes * source.height || span.bytes != source.byteCount)
        return false;

    const PixelFormat pixelFormat = (PixelFormat)source.pixelFormat;
    const bool packedRgba = source.pixelType == (uint32_t)PixelType::Packed8888Rev
        && pixelFormat == PixelFormat::RGBA && source.components == 4;
    if (source.pixelType != (uint32_t)PixelType::UnsignedByte && !packedRgba) return false;
    const bool pixelFormatMatches = ((pixelFormat == PixelFormat::Red || pixelFormat == PixelFormat::Alpha
                                       || pixelFormat == PixelFormat::Luminance) && source.components == 1)
        || ((pixelFormat == PixelFormat::RG || pixelFormat == PixelFormat::LuminanceAlpha) && source.components == 2)
        || ((pixelFormat == PixelFormat::RGB || pixelFormat == PixelFormat::SRGB
             || pixelFormat == PixelFormat::BGR) && source.components == 3)
        || ((pixelFormat == PixelFormat::RGBA || pixelFormat == PixelFormat::SRGBAlpha
             || pixelFormat == PixelFormat::BGRA) && source.components == 4);
    if (!pixelFormatMatches) return false;
    const UiFormat format = recipe.uiFormat;
    uint32_t expectedVkFormat = 0;
    bool compatible = false;
    switch (format)
    {
    case UiFormat::L8:
        compatible = source.components == 1 && pixelFormat != PixelFormat::Alpha;
        expectedVkFormat = VK_FORMAT_R8_UNORM;
        break;
    case UiFormat::A8:
        compatible = source.components == 1 && pixelFormat == PixelFormat::Alpha;
        expectedVkFormat = VK_FORMAT_R8_UNORM;
        break;
    case UiFormat::LA8:
        compatible = source.components == 2;
        expectedVkFormat = VK_FORMAT_R8G8_UNORM;
        break;
    case UiFormat::Coverage8:
        compatible = source.components == 2;
        expectedVkFormat = VK_FORMAT_R8_UNORM;
        break;
    case UiFormat::FontCoverageAuto:
        // The output is selected from pixel content on the worker. Fixed-format hints
        // would reject either a valid coverage-only page or a luminance-alpha fallback.
        compatible = source.components == 2 && !recipe.targetStorageFormat && !recipe.preparedStorageFormat;
        break;
    case UiFormat::RGBA8:
        compatible = source.components >= 3;
        expectedVkFormat = VK_FORMAT_R8G8B8A8_UNORM;
        break;
    default:
        return false;
    }
    if (!compatible) return false;
    return !expectedVkFormat
        || ((!recipe.targetStorageFormat || recipe.targetStorageFormat == expectedVkFormat)
            && (!recipe.preparedStorageFormat || recipe.preparedStorageFormat == expectedVkFormat));
}

bool prepareUiPixels(const SourceSnapshot& source, const Recipe& recipe,
                     const InputSpan& span, uint8_t* destination,
                     uint64_t destinationBytes, OutputLayout& result) noexcept
{
    if (recipe.kind != RecipeKind::ConvertPixels || recipe.srgb || recipe.encode || recipe.trialEncode
        || recipe.alphaCutoff || recipe.payloadLevels || recipe.residentTailLevels
        || (recipe.targetLevels && recipe.targetLevels != 1)) return false;

    UiFormat format = recipe.uiFormat;
    const PixelFormat pixelFormat = (PixelFormat)source.pixelFormat;
    if (format == UiFormat::FontCoverageAuto)
    {
        if (source.components != 2 || source.width > (std::numeric_limits<uint64_t>::max)() / source.height)
            return false;
        const uint64_t count = (uint64_t)source.width * source.height;
        format = UiFormat::Coverage8;
        for (uint64_t i = 0; i < count; ++i)
            if (span.data[(size_t)i * 2u] != 255u) { format = UiFormat::LA8; break; }
    }

    const bool validFormat =
        (format == UiFormat::L8 && source.components == 1 && pixelFormat != PixelFormat::Alpha)
        || (format == UiFormat::A8 && source.components == 1 && pixelFormat == PixelFormat::Alpha)
        || ((format == UiFormat::LA8 || format == UiFormat::Coverage8) && source.components == 2)
        || (format == UiFormat::RGBA8 && source.components >= 3);
    if (!validFormat) return false;

    const uint32_t bpp = uiBytesPerPixel(format);
    const uint32_t storage = uiVkFormat(format);
    const uint32_t targetW = recipe.targetWidth ? recipe.targetWidth : source.width;
    const uint32_t targetH = recipe.targetHeight ? recipe.targetHeight : source.height;
    if (!bpp || !storage || targetW != source.width || targetH != source.height
        || (uint64_t)targetW > (std::numeric_limits<uint64_t>::max)() / targetH / bpp)
        return false;
    const uint64_t bytes = (uint64_t)targetW * targetH * bpp;
    if (bytes > destinationBytes || (recipe.preparedStorageFormat && recipe.preparedStorageFormat != storage)
        || (recipe.targetStorageFormat && recipe.targetStorageFormat != storage)) return false;

    const uint64_t count = (uint64_t)source.width * source.height;
    if (format == UiFormat::RGBA8)
    {
        for (uint64_t i = 0; i < count; ++i)
        {
            const uint8_t* in = span.data + (size_t)i * source.components;
            uint8_t* out = destination + (size_t)i * 4u;
            if (source.components == 4)
                std::memcpy(out, in, 4);
            else
            {
                out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; out[3] = 255;
            }
        }
    }
    else if (format == UiFormat::Coverage8)
    {
        for (uint64_t i = 0; i < count; ++i)
            destination[i] = span.data[(size_t)i * 2u + 1u];
    }
    else
    {
        if (bytes != source.byteCount) return false;
        std::memcpy(destination, span.data, (size_t)bytes);
    }

    result.mips[0] = {0, bytes, targetW, targetH};
    result.byteCount = bytes;
    result.mipCount = 1;
    result.storageFormat = storage;
    result.destinationStorageFormat = storage;
    result.components = (uint8_t)bpp;
    result.uiFormat = format;
    result.uiSwizzle = uiSwizzle(format);
    return true;
}

void expandPixel(const uint8_t* in, uint8_t components, PixelFormat f, uint8_t* out) noexcept
{
    switch (f)
    {
    case PixelFormat::Alpha: out[0] = out[1] = out[2] = 255; out[3] = in[0]; break;
    case PixelFormat::Luminance: out[0] = out[1] = out[2] = in[0]; out[3] = 255; break;
    case PixelFormat::LuminanceAlpha: out[0] = out[1] = out[2] = in[0]; out[3] = in[1]; break;
    case PixelFormat::RG: out[0] = in[0]; out[1] = in[1]; out[2] = 0; out[3] = 255; break;
    case PixelFormat::BGR: out[0] = in[2]; out[1] = in[1]; out[2] = in[0]; out[3] = 255; break;
    case PixelFormat::BGRA: out[0] = in[2]; out[1] = in[1]; out[2] = in[0]; out[3] = in[3]; break;
    case PixelFormat::RGB: case PixelFormat::SRGB:
        out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; out[3] = 255; break;
    case PixelFormat::RGBA: case PixelFormat::SRGBAlpha:
        std::memcpy(out, in, 4); break;
    case PixelFormat::Red:
        out[0] = in[0]; out[1] = out[2] = 0; out[3] = 255; break;
    default:
        (void)components;
        out[0] = out[1] = out[2] = out[3] = 0;
        break;
    }
}

bool reduceRGBA(const uint8_t* src, uint8_t* dst, uint32_t w, uint32_t h,
                uint32_t nw, uint32_t nh, bool srgb) noexcept
{
    static float s2l[256];
    static uint8_t l2s[4096];
    static std::once_flag once;
    try { std::call_once(once, []
    {
        for (int i = 0; i < 256; ++i)
        {
            const float c = (float)i / 255.f;
            s2l[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        for (int j = 0; j < 4096; ++j)
        {
            const float l = (float)j / 4095.f;
            const float c = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.f / 2.4f) - 0.055f;
            l2s[j] = (uint8_t)std::clamp((int)(c * 255.f + 0.5f), 0, 255);
        }
    }); } catch (...) { return false; }
    for (uint32_t y = 0; y < nh; ++y)
    {
        const uint32_t y0 = std::min(2u * y, h - 1u), y1 = std::min(2u * y + 1u, h - 1u);
        const uint8_t* r0 = src + (size_t)y0 * w * 4u;
        const uint8_t* r1 = src + (size_t)y1 * w * 4u;
        uint8_t* o = dst + (size_t)y * nw * 4u;
        for (uint32_t x = 0; x < nw; ++x, o += 4)
        {
            const uint32_t x0 = std::min(2u * x, w - 1u) * 4u;
            const uint32_t x1 = std::min(2u * x + 1u, w - 1u) * 4u;
            for (uint32_t c = 0; c < 4; ++c)
            {
                if (srgb && c < 3)
                {
                    const float a = (s2l[r0[x0 + c]] + s2l[r0[x1 + c]]
                                   + s2l[r1[x0 + c]] + s2l[r1[x1 + c]]) * 0.25f;
                    o[c] = l2s[std::clamp((int)(a * 4095.f + 0.5f), 0, 4095)];
                }
                else
                    o[c] = (uint8_t)(((uint32_t)r0[x0 + c] + r0[x1 + c]
                                    + r1[x0 + c] + r1[x1 + c] + 2u) >> 2);
            }
        }
    }
    return true;
}

void histogram(const uint8_t* img, uint32_t w, uint32_t h,
               std::array<uint32_t, 1021>& hist, uint64_t& samples) noexcept
{
    constexpr uint32_t sub = 2, scale = 4, bins = 1021, d = 2 * sub, dd = d * d;
    hist.fill(0);
    samples = 0;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t sy = 0; sy < sub; ++sy)
        {
            const bool ny = 2 * sy + 1 < sub;
            const uint32_t ya = ny ? (y ? y - 1 : 0) : y;
            const uint32_t yb = ny ? y : std::min(y + 1, h - 1);
            const uint32_t wy = ny ? 2 * sy + 1 + sub : 2 * sy + 1 - sub;
            const uint8_t* ra = img + (size_t)ya * w * 4u + 3u;
            const uint8_t* rb = img + (size_t)yb * w * 4u + 3u;
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint32_t xl = (x ? x - 1 : 0) * 4u, xc = x * 4u;
                const uint32_t xr = std::min(x + 1, w - 1) * 4u;
                const uint8_t v = ra[xc];
                if (ra[xl] == v && ra[xr] == v && rb[xl] == v && rb[xc] == v && rb[xr] == v)
                {
                    hist[(uint32_t)v * scale] += sub;
                    continue;
                }
                for (uint32_t sx = 0; sx < sub; ++sx)
                {
                    const bool nx = 2 * sx + 1 < sub;
                    const uint32_t xa = (nx ? (x ? x - 1 : 0) : x) * 4u;
                    const uint32_t xb = (nx ? x : std::min(x + 1, w - 1)) * 4u;
                    const uint32_t wx = nx ? 2 * sx + 1 + sub : 2 * sx + 1 - sub;
                    const uint32_t top = ra[xa] * (d - wx) + ra[xb] * wx;
                    const uint32_t bot = rb[xa] * (d - wx) + rb[xb] * wx;
                    const uint32_t a = top * (d - wy) + bot * wy;
                    ++hist[std::min((a * scale + dd / 2) / dd, bins - 1)];
                }
            }
            samples += (uint64_t)w * sub;
        }
}

double coverageAt(const std::array<uint32_t, 1021>& hist, uint64_t samples,
                  float cutoff, float scale) noexcept
{
    constexpr uint32_t bins = 1021, binScale = 4;
    if (scale <= 0.f) return 0.0;
    const float threshold = cutoff / scale * (float)binScale;
    const uint32_t first = threshold <= 0.f ? 0u
        : (uint32_t)std::min(std::ceil(threshold - 0.5f), (float)bins);
    uint64_t passing = 0;
    for (uint32_t i = first; i < bins; ++i) passing += hist[i];
    return (double)passing / (double)std::max<uint64_t>(samples, 1);
}

bool buildCoverage(uint8_t* dst, uint8_t* cur, uint8_t* alt, uint64_t altBytes,
                   const uint8_t* pixels, uint32_t components, PixelFormat format,
                   uint32_t w, uint32_t h, uint32_t skip, bool srgb,
                   uint8_t cutoff) noexcept
{
    static float s2l[256], u2f[256];
    static uint8_t l2s[4096];
    static std::once_flag once;
    try { std::call_once(once, []
    {
        for (int i = 0; i < 256; ++i)
        {
            const float c = (float)i / 255.f;
            s2l[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
            u2f[i] = c;
        }
        for (int j = 0; j < 4096; ++j)
        {
            const float l = (float)j / 4095.f;
            const float c = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.f / 2.4f) - 0.055f;
            l2s[j] = (uint8_t)std::clamp((int)(c * 255.f + 0.5f), 0, 255);
        }
    }); } catch (...) { return false; }
    const float* linear = srgb ? s2l : u2f;
    const uint64_t pixelsN = (uint64_t)w * h;
    for (uint64_t i = 0; i < pixelsN; ++i)
        expandPixel(pixels + (size_t)i * components, (uint8_t)components, format, cur + (size_t)i * 4u);

    const uint32_t levels = mipLevels(w, h);
    skip = std::min(skip, levels - 1u);
    const float threshold = (float)std::max<uint32_t>(cutoff, 1u);
    std::array<uint32_t, 1021> hist{};
    uint64_t sampleN = 0;
    histogram(cur, w, h, hist, sampleN);
    const double target = coverageAt(hist, sampleN, threshold, 1.f);
    const bool scaleOn = target > 0.0 && target < 1.0;
    uint32_t lw = w, lh = h;
    uint8_t* src = cur;
    uint8_t* temp = alt;
    for (uint32_t level = 0; level < levels; ++level)
    {
        if (level)
        {
            const uint32_t nw = std::max(lw / 2u, 1u), nh = std::max(lh / 2u, 1u);
            const uint64_t need = (uint64_t)nw * nh * 4u;
            if (need > altBytes) return false;
            for (uint32_t y = 0; y < nh; ++y)
            {
                const uint32_t y0 = std::min(2u * y, lh - 1u), y1 = std::min(2u * y + 1u, lh - 1u);
                const uint8_t* row0 = src + (size_t)y0 * lw * 4u;
                const uint8_t* row1 = src + (size_t)y1 * lw * 4u;
                uint8_t* out = temp + (size_t)y * nw * 4u;
                for (uint32_t x = 0; x < nw; ++x, out += 4)
                {
                    const uint32_t x0 = std::min(2u * x, lw - 1u) * 4u;
                    const uint32_t x1 = std::min(2u * x + 1u, lw - 1u) * 4u;
                    const uint8_t* q[4] = {row0 + x0, row0 + x1, row1 + x0, row1 + x1};
                    const uint32_t alphaSum = (uint32_t)q[0][3] + q[1][3] + q[2][3] + q[3][3];
                    out[3] = (uint8_t)((alphaSum + 2u) >> 2);
                    const float a0 = alphaSum ? (float)q[0][3] : 1.f;
                    const float a1 = alphaSum ? (float)q[1][3] : 1.f;
                    const float a2 = alphaSum ? (float)q[2][3] : 1.f;
                    const float a3 = alphaSum ? (float)q[3][3] : 1.f;
                    const float inv = 1.f / (a0 + a1 + a2 + a3);
                    for (uint32_t c = 0; c < 3; ++c)
                    {
                        const float v = (linear[q[0][c]] * a0 + linear[q[1][c]] * a1
                                       + linear[q[2][c]] * a2 + linear[q[3][c]] * a3) * inv;
                        out[c] = srgb ? l2s[std::clamp((int)(v * 4095.f + 0.5f), 0, 4095)]
                                      : (uint8_t)std::clamp((int)(v * 255.f + 0.5f), 0, 255);
                    }
                }
            }
            std::swap(src, temp);
            lw = nw; lh = nh;
        }
        if (level < skip) continue;
        const uint64_t n = (uint64_t)lw * lh;
        uint8_t* out = dst;
        std::memcpy(out, src, (size_t)n * 4u);
        if (level && scaleOn)
        {
            histogram(src, lw, lh, hist, sampleN);
            float lo = 0.f, hi = 256.f;
            for (int it = 0; it < 24; ++it)
            {
                const float mid = 0.5f * (lo + hi);
                if (coverageAt(hist, sampleN, threshold, mid) < target) lo = mid; else hi = mid;
            }
            const double errLo = target - coverageAt(hist, sampleN, threshold, lo);
            const double errHi = std::fabs(coverageAt(hist, sampleN, threshold, hi) - target);
            float best = errHi <= 2.0 * errLo ? hi : lo;
            const double bestErr = std::min(errLo, errHi);
            if (std::fabs(coverageAt(hist, sampleN, threshold, 1.f) - target)
                <= bestErr + 0.5 / (double)std::max<uint64_t>(sampleN, 1)) best = 1.f;
            if (best != 1.f)
            {
                uint8_t lut[256];
                for (uint32_t a = 0; a < 256; ++a)
                    lut[a] = (uint8_t)std::min((uint32_t)((float)a * best + 0.5f), 255u);
                for (uint64_t p = 0; p < n; ++p) out[p * 4u + 3u] = lut[out[p * 4u + 3u]];
            }
        }
        dst += n * 4u;
    }
    return true;
}
}

bool workspaceSize(const SourceSnapshot& source, const Recipe& recipe,
                   WorkspaceSize& size) noexcept
{
    size = {};
    if (!source.valid()) return false;
    if (source.kind == SourceKind::CompressedLevels)
    {
        if (recipe.uiFormat != UiFormat::None) return false;
        const VkFormat format = (VkFormat)recipe.targetStorageFormat;
        const uint32_t w = recipe.targetWidth ? recipe.targetWidth : source.width;
        const uint32_t h = recipe.targetHeight ? recipe.targetHeight : source.height;
        const uint32_t levels = recipe.targetLevels ? recipe.targetLevels : mipLevels(w, h);
        const uint32_t payloadLevels = recipe.payloadLevels ? recipe.payloadLevels : levels;
        const uint32_t residentTailLevels = recipe.residentTailLevels;
        if (recipe.kind != RecipeKind::CopyCompressed || !format || !w || !h
            || !levels || levels > kMaxMipLevels || source.storageFormat != (uint32_t)format
            || (recipe.preparedStorageFormat && recipe.preparedStorageFormat != (uint32_t)format))
            return false;
        const uint32_t sourceBaseWidth = std::max(source.width >> source.firstLevel, 1u);
        const uint32_t sourceBaseHeight = std::max(source.height >> source.firstLevel, 1u);
        if (!payloadLevels || payloadLevels > levels || residentTailLevels > levels - payloadLevels
            || payloadLevels + residentTailLevels != levels
            || source.firstLevel >= source.levels || payloadLevels > source.levels - source.firstLevel
            || sourceBaseWidth != w || sourceBaseHeight != h
            || (!recipe.payloadLevels && (source.firstLevel != 0 || residentTailLevels)))
            return false;
        size.outputBytes = texChainBytesFmt(w, h, format, payloadLevels);
        return size.outputBytes != 0;
    }
    if (source.kind != SourceKind::PixelBytes || !source.components || source.components > 4) return false;
    const uint32_t tw = recipe.targetWidth ? recipe.targetWidth : source.width;
    const uint32_t th = recipe.targetHeight ? recipe.targetHeight : source.height;
    if (!tw || !th) return false;
    if (recipe.uiFormat != UiFormat::None)
    {
        if (tw != source.width || th != source.height || !uiSourceRecipeValid(source, recipe)) return false;
        const uint32_t bpp = uiBytesPerPixel(recipe.uiFormat);
        if (!bpp || (uint64_t)tw > (std::numeric_limits<uint64_t>::max)() / th / bpp
            || !mul((uint64_t)tw * th, bpp, size.outputBytes)) return false;
        // FontCoverageAuto can fall back to LA8, so reserve its two-byte worst case;
        // the worker reports and writes one byte for coverage-only pages.
        if (recipe.uiFormat == UiFormat::FontCoverageAuto)
        {
            if (!mul((uint64_t)tw * th, 2u, size.outputBytes)) return false;
        }
        return true;
    }
    uint32_t skip = 0, rw = source.width, rh = source.height;
    while ((rw > tw || rh > th) && (rw > 1 || rh > 1))
    {
        rw = std::max(rw / 2u, 1u); rh = std::max(rh / 2u, 1u); ++skip;
    }
    if (rw != tw || rh != th || (recipe.kind == RecipeKind::ConvertPixels && skip)) return false;
    if (recipe.kind != RecipeKind::ConvertPixels && recipe.kind != RecipeKind::ReducePixels
        && recipe.kind != RecipeKind::ConvertPrivateMedia && recipe.kind != RecipeKind::CoveragePixels)
        return false;
    const bool coverage = recipe.kind == RecipeKind::CoveragePixels;
    uint64_t rgbaBytes = 0;
    if (!pixelBytes(source, rgbaBytes)) return false;
    if (coverage)
    {
        if (!recipe.alphaCutoff || !source.components || source.components != 4) return false;
        const uint32_t levels = mipLevels(source.width, source.height) - skip;
        size.outputBytes = texChainBytesFmt(tw, th, VK_FORMAT_R8G8B8A8_UNORM, levels);
    }
    else if (!mul((uint64_t)tw * th, 4u, size.outputBytes)) return false;
    size.scratchBytes = rgbaBytes;
    if ((coverage && mipLevels(source.width, source.height) > 1) || skip)
    {
        const uint32_t nw = std::max(source.width / 2u, 1u);
        const uint32_t nh = std::max(source.height / 2u, 1u);
        uint64_t alternate = 0;
        if (!mul((uint64_t)nw * nh, 4u, alternate)
            || alternate > (std::numeric_limits<uint64_t>::max)() - size.scratchBytes) return false;
        size.scratchBytes += alternate;
    }
    return size.outputBytes != 0;
}

bool prepareCompressedPayload(const SourceSnapshot& source, const Recipe& recipe,
                              uint8_t* destination, uint64_t destinationBytes,
                              OutputLayout& output) noexcept
{
    if (!destination || source.kind != SourceKind::CompressedLevels || recipe.uiFormat != UiFormat::None) return false;
    WorkspaceSize ws;
    if (!workspaceSize(source, recipe, ws) || ws.outputBytes > destinationBytes) return false;
    const VkFormat format = (VkFormat)recipe.targetStorageFormat;
    const uint32_t w = recipe.targetWidth ? recipe.targetWidth : source.width;
    const uint32_t h = recipe.targetHeight ? recipe.targetHeight : source.height;
    const uint32_t levels = recipe.targetLevels ? recipe.targetLevels : mipLevels(w, h);
    const uint32_t payloadLevels = recipe.payloadLevels ? recipe.payloadLevels : levels;
    if (format != (VkFormat)source.storageFormat || recipe.encode || recipe.trialEncode
        || !payloadLevels || payloadLevels > levels || levels > mipLevels(w, h)
        || (uint64_t)payloadLevels + recipe.residentTailLevels != levels
        || source.firstLevel >= source.levels
        || payloadLevels > source.levels - source.firstLevel
        || std::max(source.width >> source.firstLevel, 1u) != w
        || std::max(source.height >> source.firstLevel, 1u) != h
        || (!recipe.payloadLevels && (source.firstLevel != 0 || recipe.residentTailLevels)))
        return false;

    OutputLayout result{};
    uint64_t outputOffset = 0;
    uint32_t mw = w, mh = h;
    for (uint32_t l = 0; l < payloadLevels; ++l)
    {
        const uint64_t n = texLevelBytes(mw, mh, format);
        if (!n || n > ws.outputBytes - outputOffset) return false;
        result.mips[l] = {outputOffset, n, mw, mh};
        outputOffset += n;
        mw = std::max(mw / 2u, 1u); mh = std::max(mh / 2u, 1u);
    }
    if (outputOffset != ws.outputBytes) return false;

    uint64_t copied = 0;
    uint32_t expectedLevel = source.firstLevel;
    for (uint32_t i = 0; i < source.spanCount; ++i)
    {
        const InputSpan& span = source.spans[i];
        if (span.layout != SpanLayout::MipChain || span.level != expectedLevel
            || expectedLevel < source.firstLevel
            || expectedLevel - source.firstLevel > payloadLevels
            || span.levelCount > payloadLevels - (expectedLevel - source.firstLevel)
            || span.width != std::max(source.width >> span.level, 1u)
            || span.height != std::max(source.height >> span.level, 1u)) return false;
        uint64_t spanExpected = 0;
        uint32_t sw = span.width, sh = span.height;
        for (uint32_t mip = 0; mip < span.levelCount; ++mip)
        {
            const uint64_t n = texLevelBytes(sw, sh, format);
            if (!n || n > (std::numeric_limits<uint64_t>::max)() - spanExpected)
                return false;
            spanExpected += n;
            sw = std::max(sw / 2u, 1u); sh = std::max(sh / 2u, 1u);
        }
        if (span.bytes != spanExpected || span.bytes > ws.outputBytes - copied)
            return false;
        std::memcpy(destination + copied, span.data, (size_t)span.bytes);
        copied += span.bytes;
        expectedLevel += span.levelCount;
    }
    if (copied != ws.outputBytes || expectedLevel != source.firstLevel + payloadLevels)
        return false;
    result.byteCount = ws.outputBytes;
    result.mipCount = payloadLevels; // final image also includes the resident GPU tail
    result.storageFormat = (uint32_t)format;
    result.destinationStorageFormat = (uint32_t)format;
    result.encode = recipe.encode; result.trialEncode = recipe.trialEncode;
    result.srgb = recipe.srgb;
    result.components = (uint8_t)std::min(source.components, 4u);
    output = result;
    return true;
}

bool prepareTextureInput(const SourceSnapshot& source, const Recipe& recipe,
                         uint8_t* destination, uint64_t destinationBytes,
                         uint8_t* scratch, uint64_t scratchBytes,
                         OutputLayout& output) noexcept
{
    if (!destination) return false;
    WorkspaceSize ws;
    if (!workspaceSize(source, recipe, ws) || ws.outputBytes > destinationBytes
        || ws.scratchBytes > scratchBytes || (ws.scratchBytes && !scratch)) return false;
    if (source.kind == SourceKind::CompressedLevels)
        return prepareCompressedPayload(source, recipe, destination, destinationBytes, output);
    OutputLayout result{};
    const PixelFormat pixelFormat = (PixelFormat)source.pixelFormat;
    const bool packed = source.pixelType == (uint32_t)PixelType::Packed8888Rev
        && pixelFormat == PixelFormat::RGBA && source.components == 4;
    if ((source.pixelType != (uint32_t)PixelType::UnsignedByte && !packed)
        || source.baseGeneration >= 0 || recipe.residentTailLevels) return false;
    const uint64_t pixelCount = (uint64_t)source.width * source.height;
    if (pixelCount > (std::numeric_limits<uint64_t>::max)() / source.components
        || pixelCount * source.components != source.byteCount) return false;
    const InputSpan& span = source.spans[0];
    if (source.spanCount != 1 || span.layout != SpanLayout::Rows || span.x || span.y
        || span.width != source.width || span.height != source.height
        || span.rowBytes != (uint64_t)source.width * source.components
        || span.bytes != source.byteCount) return false;
    const bool formatMatches = ((pixelFormat == PixelFormat::Red || pixelFormat == PixelFormat::Alpha
                                  || pixelFormat == PixelFormat::Luminance) && source.components == 1)
        || ((pixelFormat == PixelFormat::RG || pixelFormat == PixelFormat::LuminanceAlpha) && source.components == 2)
        || ((pixelFormat == PixelFormat::RGB || pixelFormat == PixelFormat::SRGB
             || pixelFormat == PixelFormat::BGR) && source.components == 3)
        || ((pixelFormat == PixelFormat::RGBA || pixelFormat == PixelFormat::SRGBAlpha
             || pixelFormat == PixelFormat::BGRA) && source.components == 4);
    if (!formatMatches) return false;

    if (recipe.uiFormat != UiFormat::None)
    {
        if (source.components == 1)
        {
            if ((pixelFormat == PixelFormat::Alpha && recipe.uiFormat != UiFormat::A8)
                || (pixelFormat != PixelFormat::Alpha && recipe.uiFormat != UiFormat::L8)) return false;
        }
        else if (source.components == 2)
        {
            if (recipe.uiFormat != UiFormat::LA8 && recipe.uiFormat != UiFormat::Coverage8
                && recipe.uiFormat != UiFormat::FontCoverageAuto) return false;
        }
        else if (recipe.uiFormat != UiFormat::RGBA8) return false;
        if (!prepareUiPixels(source, recipe, span, destination, destinationBytes, result)) return false;
        output = result;
        return true;
    }

    const bool coverageMode = recipe.kind == RecipeKind::CoveragePixels;
    // Coverage stores encoded color bytes in UNORM staging even for an sRGB source;
    // buildCoverage uses recipe.srgb for filtering and the GPU encode keeps that fact.
    const VkFormat fallback = !coverageMode && recipe.srgb
        ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    const VkFormat format = recipe.preparedStorageFormat ? (VkFormat)recipe.preparedStorageFormat : fallback;
    const uint32_t levels = mipLevels(source.width, source.height);
    const VkFormat expectedFormat = coverageMode ? VK_FORMAT_R8G8B8A8_UNORM : format;
    const bool rgba = format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB;
    const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
    const VkFormat destinationFormat = recipe.targetStorageFormat
        ? (VkFormat)recipe.targetStorageFormat : format;
    if ((!rgba && !(recipe.kind == RecipeKind::ConvertPrivateMedia && bgra))
        || (!coverageMode && recipe.srgb != (format == VK_FORMAT_R8G8B8A8_SRGB
                                             || format == VK_FORMAT_B8G8R8A8_SRGB))
        || format != expectedFormat || recipe.payloadLevels || recipe.trialEncode
        || (recipe.encode == TEXENC_NONE && destinationFormat != format)
        || (recipe.encode != TEXENC_NONE && destinationFormat != texEncFormat(recipe.encode, recipe.srgb)))
        return false;
    const uint32_t tw = recipe.targetWidth ? recipe.targetWidth : source.width;
    const uint32_t th = recipe.targetHeight ? recipe.targetHeight : source.height;
    uint32_t skip = 0, rw = source.width, rh = source.height;
    while ((rw > tw || rh > th) && (rw > 1 || rh > 1))
    {
        rw = std::max(rw / 2u, 1u); rh = std::max(rh / 2u, 1u); ++skip;
    }
    if (rw != tw || rh != th || (recipe.kind == RecipeKind::ConvertPixels && skip)) return false;
    const uint32_t mipCount = coverageMode ? levels - skip : 1u;
    if (recipe.targetLevels && recipe.targetLevels != (coverageMode ? mipCount : mipLevels(tw, th))) return false;
    if (coverageMode)
    {
        if (source.components != 4 || !recipe.alphaCutoff || format != VK_FORMAT_R8G8B8A8_UNORM) return false;
        uint8_t* alt = ws.scratchBytes > (uint64_t)source.width * source.height * 4u
            ? scratch + (size_t)source.width * source.height * 4u : nullptr;
        const uint64_t altBytes = alt ? ws.scratchBytes - (uint64_t)source.width * source.height * 4u : 0;
        if (!buildCoverage(destination, scratch, alt, altBytes, span.data, source.components,
                           pixelFormat, source.width, source.height, skip, recipe.srgb,
                           recipe.alphaCutoff)) return false;
        uint64_t off = 0; uint32_t mw = tw, mh = th;
        for (uint32_t i = 0; i < mipCount; ++i)
        {
            const uint64_t n = (uint64_t)mw * mh * 4u;
            if (n > ws.outputBytes - off) return false;
            result.mips[i] = {off, n, mw, mh}; off += n;
            mw = std::max(mw / 2u, 1u); mh = std::max(mh / 2u, 1u);
        }
        if (off != ws.outputBytes) return false;
    }
    else
    {
        for (uint64_t i = 0; i < pixelCount; ++i)
            expandPixel(span.data + (size_t)i * source.components, (uint8_t)source.components,
                        pixelFormat, scratch + (size_t)i * 4u);
        uint8_t* current = scratch;
        uint8_t* alternate = ws.scratchBytes > (uint64_t)source.width * source.height * 4u
            ? scratch + (size_t)source.width * source.height * 4u : nullptr;
        uint32_t cw = source.width, ch = source.height;
        for (uint32_t l = 0; l < skip; ++l)
        {
            if (!alternate) return false;
            const uint32_t nw = std::max(cw / 2u, 1u), nh = std::max(ch / 2u, 1u);
            if (!reduceRGBA(current, alternate, cw, ch, nw, nh, recipe.srgb)) return false;
            std::swap(current, alternate); cw = nw; ch = nh;
        }
        if (cw != tw || ch != th || ws.outputBytes != (uint64_t)tw * th * 4u) return false;
        if (bgra)
            for (uint64_t i = 0, n = (uint64_t)tw * th; i < n; ++i)
                std::swap(current[i * 4u], current[i * 4u + 2u]);
        std::memcpy(destination, current, (size_t)ws.outputBytes);
        result.mips[0] = {0, ws.outputBytes, tw, th};
    }
    result.byteCount = ws.outputBytes; result.mipCount = mipCount;
    result.storageFormat = (uint32_t)format; result.components = 4;
    result.destinationStorageFormat = (uint32_t)destinationFormat;
    result.encode = recipe.encode; result.trialEncode = recipe.trialEncode;
    result.srgb = recipe.srgb;
    output = result;
    return true;
}

bool preparedTextureOutputMatches(const Recipe& recipe, const OutputLayout& output,
                                  uint32_t width, uint32_t height,
                                  uint64_t capacity) noexcept
{
    if (!width || !height || !output.byteCount || output.byteCount > capacity
        || (recipe.targetWidth && recipe.targetWidth != width)
        || (recipe.targetHeight && recipe.targetHeight != height)
        || !output.mipCount || output.mipCount > kMaxMipLevels) return false;
    if (recipe.uiFormat != UiFormat::None)
    {
        if (recipe.kind != RecipeKind::ConvertPixels || output.mipCount != 1
            || recipe.srgb || recipe.encode || recipe.trialEncode || recipe.alphaCutoff
            || recipe.payloadLevels || recipe.residentTailLevels
            || output.encode || output.trialEncode || output.srgb) return false;
        const uint32_t bpp = uiBytesPerPixel(output.uiFormat);
        const uint32_t format = uiVkFormat(output.uiFormat);
        const UiSwizzle swizzle = uiSwizzle(output.uiFormat);
        if (!bpp || !format || output.uiFormat == UiFormat::FontCoverageAuto
            || output.uiSwizzle != swizzle || output.components != bpp
            || output.storageFormat != format || output.destinationStorageFormat != format
            || (recipe.preparedStorageFormat && recipe.preparedStorageFormat != format)
            || (recipe.targetStorageFormat && recipe.targetStorageFormat != format)
            || (uint64_t)width > (std::numeric_limits<uint64_t>::max)() / height / bpp
            || output.byteCount != (uint64_t)width * height * bpp
            || output.mips[0].offset != 0 || output.mips[0].width != width
            || output.mips[0].height != height || output.mips[0].bytes != output.byteCount)
            return false;
        return true;
    }
    if (output.uiFormat != UiFormat::None || output.uiSwizzle != UiSwizzle::None) return false;
    VkFormat format = recipe.preparedStorageFormat ? (VkFormat)recipe.preparedStorageFormat
        : recipe.targetStorageFormat ? (VkFormat)recipe.targetStorageFormat
        : (recipe.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM);
    const bool coverage = recipe.kind == RecipeKind::CoveragePixels;
    if (coverage) format = VK_FORMAT_R8G8B8A8_UNORM;
    if (output.storageFormat != (uint32_t)format
        || (recipe.kind == RecipeKind::CopyCompressed ? (!output.components || output.components > 4)
                                                      : output.components != 4)
        || recipe.trialEncode) return false;
    const VkFormat destinationFormat = recipe.targetStorageFormat
        ? (VkFormat)recipe.targetStorageFormat : format;
    if (recipe.encode == TEXENC_NONE)
    {
        if (destinationFormat != format) return false;
    }
    else if (recipe.kind == RecipeKind::CopyCompressed
             || destinationFormat != texEncFormat(recipe.encode, recipe.srgb)) return false;
    if (output.destinationStorageFormat != (uint32_t)destinationFormat
        || output.encode != recipe.encode || output.trialEncode != recipe.trialEncode
        || output.srgb != recipe.srgb) return false;
    uint32_t expectedLevels = 1;
    if (recipe.kind == RecipeKind::CopyCompressed)
        expectedLevels = recipe.payloadLevels ? recipe.payloadLevels
            : recipe.targetLevels ? recipe.targetLevels : mipLevels(width, height);
    else if (coverage)
        expectedLevels = recipe.targetLevels ? recipe.targetLevels : output.mipCount;
    if (output.mipCount != expectedLevels) return false;
    uint64_t total = 0;
    uint32_t mw = width, mh = height;
    for (uint32_t i = 0; i < output.mipCount; ++i)
    {
        const MipLayout& mip = output.mips[i];
        if (recipe.kind != RecipeKind::CopyCompressed
            && (uint64_t)mw > (std::numeric_limits<uint64_t>::max)() / std::max<uint32_t>(mh, 1u) / 4u)
            return false;
        const uint64_t expected = recipe.kind == RecipeKind::CopyCompressed
            ? texLevelBytes(mw, mh, format) : (uint64_t)mw * mh * 4u;
        if (!expected || mip.offset != total || mip.width != mw || mip.height != mh
            || mip.bytes != expected || expected > output.byteCount - total) return false;
        total += expected;
        mw = std::max(mw / 2u, 1u); mh = std::max(mh / 2u, 1u);
    }
    return total == output.byteCount;
}
}
