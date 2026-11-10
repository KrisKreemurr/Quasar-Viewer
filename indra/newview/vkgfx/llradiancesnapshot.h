/**
 * Bounded Radiance RGBE serialization for one-shot HDR snapshots.
 * Input pixels are tightly packed RGBE, bottom-up (the LLImageRaw convention).
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <limits>
#include <new>

namespace LLRadianceSnapshot
{
constexpr uint64_t kMaxPixels = 64ull * 1024ull * 1024ull;

inline bool maxSerializedBytes(uint32_t width, uint32_t height, uint64_t& bytes)
{
    bytes = 0;
    const uint64_t pixels = uint64_t(width) * height;
    constexpr uint64_t max = ~uint64_t(0);
    if (!width || !height || pixels > kMaxPixels || pixels > max / 4) return false;
    bytes = pixels * 4;
    uint64_t overhead = 128; // bounded header, including decimal dimensions and newline
    if (width >= 8 && width <= 32767)
    {
        const uint64_t packetsPerChannel = (uint64_t(width) + 127) / 128;
        const uint64_t perRow = 4 + 4 * packetsPerChannel; // scanline marker + worst literal packet headers
        if (uint64_t(height) > (max - overhead) / perRow) return false;
        overhead += uint64_t(height) * perRow;
    }
    if (bytes > max - overhead) return false;
    bytes += overhead;
    return true;
}

inline bool peakHostBytes(uint32_t width, uint32_t height, uint64_t sourceBytes, uint64_t& bytes)
{
    bytes = 0;
    constexpr uint64_t max = ~uint64_t(0);
    const uint64_t pixels = uint64_t(width) * height;
    uint64_t fileBytes = 0;
    if (!pixels || pixels > kMaxPixels || pixels > max / 4
        || !maxSerializedBytes(width, height, fileBytes)) return false;
    const uint64_t readbackBytes = pixels * 4;
    const uint64_t scratchBytes = uint64_t(width) + 256; // channel work row + temporary header
    if (sourceBytes > max - readbackBytes) return false;
    if (scratchBytes > max - sourceBytes - readbackBytes) return false;
    if (fileBytes > (max - sourceBytes - readbackBytes - scratchBytes) / 2) return false;
    bytes = readbackBytes + 2 * fileBytes + sourceBytes + scratchBytes;
    return true;
}

inline bool serialize(uint32_t width, uint32_t height, const uint8_t* bottomUpRgbe,
                      size_t inputBytes, std::vector<uint8_t>& fileBytes)
{
    fileBytes.clear();
    const uint64_t pixels = uint64_t(width) * height;
    if (!width || !height || pixels > kMaxPixels || pixels > std::numeric_limits<size_t>::max() / 4
        || !bottomUpRgbe || inputBytes != size_t(pixels) * 4)
        return false;

    // RGBE is written top-down in Radiance files; the live snapshot readback is bottom-up.
    try
    {
        uint64_t serializedLimit = 0;
        if (!maxSerializedBytes(width, height, serializedLimit)
            || serializedLimit > std::numeric_limits<size_t>::max())
            return false;
        // Reserve the full admitted bound up front. This prevents vector growth from
        // transiently retaining old and new allocations while the file is serialized.
        fileBytes.reserve(size_t(serializedLimit));
        const std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y "
            + std::to_string(height) + " +X " + std::to_string(width) + "\n";
        fileBytes.insert(fileBytes.end(), header.begin(), header.end());
        auto put = [&fileBytes](uint8_t value) { fileBytes.push_back(value); };
        auto encodeChannel = [&](const uint8_t* channel)
        {
            uint32_t x = 0;
            while (x < width)
            {
                uint32_t run = 1;
                while (x + run < width && run < 127 && channel[x] == channel[x + run]) ++run;
                if (run >= 4)
                {
                    put(uint8_t(128u + run));
                    put(channel[x]);
                    x += run;
                    continue;
                }

                const uint32_t start = x;
                x += run;
                while (x < width && x - start < 128)
                {
                    uint32_t nextRun = 1;
                    while (x + nextRun < width && nextRun < 4 && channel[x] == channel[x + nextRun]) ++nextRun;
                    if (nextRun >= 4) break;
                    x += std::min(nextRun, 128u - (x - start));
                }
                const uint32_t count = x - start;
                put(uint8_t(count));
                for (uint32_t i = 0; i < count; ++i) put(channel[start + i]);
            }
        };

        if (width < 8 || width > 32767)
        {
            // Legacy flat scanlines avoid the Radiance RLE width encoding limits.
            for (uint32_t y = 0; y < height; ++y)
            {
                const uint8_t* row = bottomUpRgbe + size_t(height - 1 - y) * width * 4;
                fileBytes.insert(fileBytes.end(), row, row + size_t(width) * 4);
            }
            return true;
        }

        std::vector<uint8_t> channel(width);
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t* row = bottomUpRgbe + size_t(height - 1 - y) * width * 4;
            put(2); put(2); put(uint8_t(width >> 8)); put(uint8_t(width));
            for (uint32_t c = 0; c < 4; ++c)
            {
                for (uint32_t x = 0; x < width; ++x) channel[x] = row[size_t(x) * 4 + c];
                encodeChannel(channel.data());
            }
        }
    }
    catch (const std::bad_alloc&)
    {
        fileBytes.clear();
        return false;
    }
    return true;
}
}
