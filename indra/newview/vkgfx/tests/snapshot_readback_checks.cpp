// Synthetic CPU fixture: offscreen FP16/SDR and live FP16/nits readback contracts.
#include "../vk_snapshot_readback.h"
#include <cassert>
#include <cstring>
#include <limits>

int main()
{
    // Source rows top-down: red/half-covered, green/transparent, blue/opaque, white/quarter.
    const glm::vec4 pixels[] = {{1.f,0.f,0.f,.5f}, {0.f,1.f,0.f,0.f},
                                {0.f,0.f,1.f,1.f}, {1.f,1.f,1.f,.25f}};
    uint32_t half[8];
    for (unsigned i = 0; i < 4; ++i)
    {
        half[2*i] = glm::packHalf2x16(glm::vec2(pixels[i].r, pixels[i].g));
        half[2*i+1] = glm::packHalf2x16(glm::vec2(pixels[i].b, pixels[i].a));
    }
    std::vector<uint8_t> out;
    vksnapshot::toImageRaw(reinterpret_cast<const uint8_t*>(half), 2, 2,
                           VK_FORMAT_R16G16B16A16_SFLOAT, false, 351.f, true, out);
    const uint8_t expected[] = {0,0,255,255, 255,255,255,64, 255,0,0,128, 0,255,0,0};
    assert(out.size() == sizeof(expected) && std::memcmp(out.data(), expected, sizeof(expected)) == 0);
    std::vector<uint8_t> rgbe;
    assert(vksnapshot::avatarToRgbe(reinterpret_cast<const uint8_t*>(half), sizeof(half), 2, 2,
                                    2, 2, 351.f, rgbe));
    auto decode = [&](unsigned pixel, unsigned channel) {
        return std::ldexp(float(rgbe[pixel * 4 + channel]), int(rgbe[pixel * 4 + 3]) - 136);
    };
    assert(rgbe.size() == 16 && std::abs(decode(0, 2) - 351.f) < 3.f); // blue, bottom row
    assert(std::abs(decode(2, 0) - 351.f) < 3.f); // half-covered red stays straight
    assert(rgbe[12] == 0 && rgbe[13] == 0 && rgbe[14] == 0 && rgbe[15] == 0); // transparent
    assert(!vksnapshot::avatarToRgbe(reinterpret_cast<const uint8_t*>(half), sizeof(half)-1, 2, 2,
                                     2, 2, 351.f, rgbe));
    const uint32_t absolute[] = {glm::packHalf2x16(glm::vec2(702.f, 351.f)),
                                 glm::packHalf2x16(glm::vec2(0.f, 0.f))};
    assert(vksnapshot::avatarToRgbe(reinterpret_cast<const uint8_t*>(absolute), sizeof(absolute), 1, 1,
                                    1, 1, 351.f, rgbe, true, false));
    assert(std::abs(std::ldexp(float(rgbe[0]), int(rgbe[3]) - 136) - 702.f) < 6.f);
    assert(std::abs(std::ldexp(float(rgbe[1]), int(rgbe[3]) - 136) - 351.f) < 6.f);
    const uint32_t scope[] = {glm::packHalf2x16(glm::vec2(.5f, 1.f)),
                              glm::packHalf2x16(glm::vec2(0.f, 1.f))};
    vksnapshot::toImageRaw(reinterpret_cast<const uint8_t*>(scope), 1, 1,
                           VK_FORMAT_R16G16B16A16_SFLOAT, false, 80.f, true, out, true);
    assert(out == std::vector<uint8_t>({188,255,0,255})); // HDR scope: relative linear -> SDR
    assert(vksnapshot::colorByte(351.f, true, 351.f) == 255);
    assert(vksnapshot::colorByte(0.5f, false, 351.f) == 128);
    assert(vksnapshot::encodedByte(std::numeric_limits<float>::quiet_NaN()) == 0);
    assert(vksnapshot::encodedByte(-1.f) == 0 && vksnapshot::encodedByte(2.f) == 255);
    const uint8_t bgra[] = {11,22,33,44};
    vksnapshot::toImageRaw(bgra, 1, 1, VK_FORMAT_B8G8R8A8_UNORM, false, 203.f, true, out);
    assert(out == std::vector<uint8_t>({33,22,11,44}));
}
