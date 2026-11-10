#include "../llradiancesnapshot.h"
#include "../llsnapshotbufferlimits.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

static size_t findHeaderEnd(const std::vector<uint8_t>& bytes)
{
    const std::string marker = "\n-Y 2 +X 8\n";
    const auto it = std::search(bytes.begin(), bytes.end(), marker.begin(), marker.end());
    assert(it != bytes.end());
    return size_t(it - bytes.begin()) + marker.size();
}

int main()
{
    uint64_t descriptorBytes = 0;
    assert(LLSnapshotBufferLimits::packedRgbeBytes(3840, 2160, 1ull << 32, descriptorBytes));
    assert(descriptorBytes == uint64_t(3840) * 2160 * 4);
    const uint64_t insufficientRange = descriptorBytes - 1;
    assert(!LLSnapshotBufferLimits::packedRgbeBytes(3840, 2160, insufficientRange, descriptorBytes));
    uint64_t peakBytes = 0;
    assert(LLSnapshotBufferLimits::snapshotPeakSystemBytes(3840, 2160, 4, 3, 3840, 2160, 4, peakBytes));
    assert(peakBytes == uint64_t(3840) * 2160 * 16);
    assert(!LLSnapshotBufferLimits::snapshotPeakSystemBytes(UINT32_MAX, UINT32_MAX, UINT32_MAX,
                                                             UINT32_MAX, UINT32_MAX, UINT32_MAX,
                                                             UINT32_MAX, peakBytes));
    uint64_t serializedCap = 0;
    assert(LLRadianceSnapshot::maxSerializedBytes(3840, 2160, serializedCap));
    assert(LLRadianceSnapshot::peakHostBytes(3840, 2160, 640ull * 1024 * 1024, peakBytes));
    assert(peakBytes == uint64_t(3840) * 2160 * 4 + 2 * serializedCap
        + 640ull * 1024 * 1024 + 3840 + 256);
    assert(!LLRadianceSnapshot::peakHostBytes(3840, 2160, UINT64_MAX, peakBytes));

    // Two bottom-up rows; serialization must reverse them to Radiance's top-down order.
    std::vector<uint8_t> pixels(8u * 2u * 4u);
    for (uint32_t y = 0; y < 2; ++y)
    for (uint32_t x = 0; x < 8; ++x)
    {
        const size_t i = (size_t(y) * 8 + x) * 4;
        pixels[i + 0] = uint8_t(y ? 20 : 10);
        pixels[i + 1] = uint8_t(x);
        pixels[i + 2] = 30;
        pixels[i + 3] = 128;
    }
    std::vector<uint8_t> file;
    assert(LLRadianceSnapshot::serialize(8, 2, pixels.data(), pixels.size(), file));
    assert(file.size() <= serializedCap);
    const size_t data = findHeaderEnd(file);
    assert(file.size() > data + 2 * 4 + 2 * 4);
    assert(file[data + 0] == 2 && file[data + 1] == 2 && file[data + 3] == 8);
    // First top-down scanline comes from bottom-up row 1 (red 20), encoded as one RLE run.
    assert(file[data + 4] == 136 && file[data + 5] == 20);

    // Flat scanline widths retain exact RGBE bytes and the same vertical orientation.
    const uint8_t tiny[] = {1,2,3,4, 5,6,7,8}; // bottom row, then top row
    assert(LLRadianceSnapshot::serialize(1, 2, tiny, sizeof(tiny), file));
    const std::string tinyHeader = "\n-Y 2 +X 1\n";
    const auto flat = std::search(file.begin(), file.end(), tinyHeader.begin(), tinyHeader.end());
    assert(flat != file.end());
    const size_t p = size_t(flat - file.begin()) + tinyHeader.size();
    assert(file[p + 0] == 5 && file[p + 4] == 1);

    assert(!LLRadianceSnapshot::serialize(0, 2, tiny, sizeof(tiny), file));
    assert(!LLRadianceSnapshot::serialize(1, 2, tiny, sizeof(tiny) - 1, file));
    assert(!LLRadianceSnapshot::serialize(9000, 9000, tiny, sizeof(tiny), file));
}
