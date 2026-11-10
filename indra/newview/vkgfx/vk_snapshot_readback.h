// Conversion of FP16 snapshot pixels into the SDR bytes used by LLImageRaw.
#pragma once
#include "vk_display_hdr.h"
#include "llradiancesnapshot.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <glm/packing.hpp>
#include <glm/gtc/packing.hpp>

namespace vksnapshot
{
inline uint8_t encodedByte(float value) noexcept
{
    if (!std::isfinite(value)) value = 0.f;
    return static_cast<uint8_t>(std::lround(std::clamp(value, 0.f, 1.f) * 255.f));
}

// Live HDR color is linear nits; the offscreen tonemap already writes SDR-encoded
// values even when its attachment is FP16. Alpha is coverage in either domain.
inline uint8_t colorByte(float value, bool linearNits, float paperWhiteNits) noexcept
{
    if (!linearNits) return encodedByte(value);
    const float linear = vkdisplay::sdrSnapshotLinear(value, paperWhiteNits);
    const float encoded = linear <= 0.0031308f ? 12.92f * linear
                                               : 1.055f * std::pow(linear, 1.f / 2.4f) - 0.055f;
    return encodedByte(encoded);
}

inline void toImageRaw(const uint8_t* src, uint32_t width, uint32_t height,
                       VkFormat format, bool linearNits, float whiteNits,
                       bool alpha, std::vector<uint8_t>& out, bool relativeLinear = false)
{
    const uint32_t channels = alpha ? 4u : 3u;
    out.resize(std::size_t(width) * height * channels);
    const bool floatPixels = format == VK_FORMAT_R16G16B16A16_SFLOAT;
    const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
    for (uint32_t y = 0; y < height; ++y)
    {
        const uint8_t* row = src + std::size_t(height - 1u - y) * width * (floatPixels ? 8u : 4u);
        uint8_t* dst = out.data() + std::size_t(y) * width * channels;
        for (uint32_t x = 0; x < width; ++x, row += floatPixels ? 8u : 4u, dst += channels)
        {
            if (floatPixels)
            {
                uint32_t rg = 0, ba = 0;
                std::memcpy(&rg, row, 4); std::memcpy(&ba, row + 4, 4);
                const glm::vec2 rgf = glm::unpackHalf2x16(rg), baf = glm::unpackHalf2x16(ba);
                dst[0] = relativeLinear ? colorByte(rgf.x * 80.f, true, 80.f) : colorByte(rgf.x, linearNits, whiteNits);
                dst[1] = relativeLinear ? colorByte(rgf.y * 80.f, true, 80.f) : colorByte(rgf.y, linearNits, whiteNits);
                dst[2] = relativeLinear ? colorByte(baf.x * 80.f, true, 80.f) : colorByte(baf.x, linearNits, whiteNits);
                if (alpha) dst[3] = encodedByte(baf.y);
            }
            else
            {
                dst[0] = bgra ? row[2] : row[0]; dst[1] = row[1]; dst[2] = bgra ? row[0] : row[2];
                if (alpha) dst[3] = row[3];
            }
        }
    }
}

// Avatar companion: FP16 straight relative-linear RGBA from the same offscreen
// tonemap as the SDR PNG. Resample in coverage-weighted linear light, then write
// bottom-up RGBE in absolute display nits (Radiance has no alpha channel).
inline bool avatarToRgbe(const uint8_t* src, std::size_t srcBytes, uint32_t width, uint32_t height,
                         uint32_t outWidth, uint32_t outHeight, float whiteNits, std::vector<uint8_t>& out,
                         bool sourceNits = false, bool useCoverage = true)
{
    out.clear();
    const uint64_t pixels = uint64_t(width) * height, outPixels = uint64_t(outWidth) * outHeight;
    if (!src || !pixels || !outPixels || pixels > LLRadianceSnapshot::kMaxPixels ||
        outPixels > LLRadianceSnapshot::kMaxPixels || pixels > srcBytes / 8u) return false;
    if (!std::isfinite(whiteNits)) whiteNits = 203.f;
    whiteNits = std::clamp(whiteNits, 80.f, vkdisplay::kDisplayReferenceWhiteMaxNits);
    out.resize(std::size_t(outPixels) * 4u);
    auto sample = [&](int x, int y, float weight, float* rgb, float& coverage)
    {
        x = std::clamp(x, 0, int(width) - 1); y = std::clamp(y, 0, int(height) - 1);
        const uint8_t* p = src + (std::size_t(y) * width + uint32_t(x)) * 8u;
        uint32_t rg = 0, ba = 0; std::memcpy(&rg, p, 4); std::memcpy(&ba, p + 4, 4);
        const glm::vec2 a = glm::unpackHalf2x16(rg), b = glm::unpackHalf2x16(ba);
        const float alpha = useCoverage ? (std::isfinite(b.y) ? std::clamp(b.y, 0.f, 1.f) : 0.f) : 1.f;
        const float c[3] = {a.x, a.y, b.x};
        for (unsigned k = 0; k < 3; ++k)
            rgb[k] += (std::isfinite(c[k]) ? std::max(c[k], 0.f) : 0.f) * alpha * weight;
        coverage += alpha * weight;
    };
    for (uint32_t y = 0; y < outHeight; ++y)
        for (uint32_t x = 0; x < outWidth; ++x)
        {
            const float sx = (float(x) + .5f) * float(width) / float(outWidth) - .5f;
            const float sy = (float(outHeight - 1u - y) + .5f) * float(height) / float(outHeight) - .5f;
            const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
            const float fx = sx - ix, fy = sy - iy;
            float rgb[3] = {}, alpha = 0.f;
            sample(ix, iy, (1.f-fx)*(1.f-fy), rgb, alpha);
            sample(ix+1, iy, fx*(1.f-fy), rgb, alpha);
            sample(ix, iy+1, (1.f-fx)*fy, rgb, alpha);
            sample(ix+1, iy+1, fx*fy, rgb, alpha);
            uint8_t* dst = out.data() + (std::size_t(y) * outWidth + x) * 4u;
            if (alpha < 0.5f / 255.f) { std::memset(dst, 0, 4); continue; }
            for (float& c : rgb) c = std::min(c * (sourceNits ? 1.f : whiteNits) / alpha, 1e30f);
            const float peak = std::max(rgb[0], std::max(rgb[1], rgb[2]));
            if (!(peak > 1e-32f)) { std::memset(dst, 0, 4); continue; }
            int exponent = 0;
            const float mantissa = std::frexp(peak, &exponent);
            const float scale = mantissa * 256.f / peak;
            for (unsigned k = 0; k < 3; ++k)
                dst[k] = uint8_t(std::clamp(std::floor(rgb[k] * scale + .5f), 0.f, 255.f));
            dst[3] = uint8_t(std::clamp(exponent + 128, 1, 255));
        }
    return true;
}

}
