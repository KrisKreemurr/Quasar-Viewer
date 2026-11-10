// Standalone CPU geometry fixture for the exact scalar DDA included by the shader.
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace
{
constexpr int kResolution = 8;
constexpr float kExtent = 1.0f;
constexpr float kEmpty = -1.0e9f;
float gTop[kResolution][kResolution];

float fixtureTop(int, int, int x, int y, float frameZ);
}

#define WX_SH_DDA_TOP(layer, source, cellX, cellY, frameZ) fixtureTop(layer, source, cellX, cellY, frameZ)
#define WX_SHELTER_DDA_CPU 1
#include "../shaders/clean_weather_shelter_dda.glsl"
#undef WX_SHELTER_DDA_CPU
#undef WX_SH_DDA_TOP

namespace
{
float fixtureTop(int, int, int x, int y, float frameZ)
{
    if (x < 0 || x >= kResolution || y < 0 || y >= kResolution) return kEmpty;
    return shDdaNormalizeTop(gTop[y][x], frameZ, -1.0e3f);
}

void clearTop()
{
    for (int y = 0; y < kResolution; ++y)
        for (int x = 0; x < kResolution; ++x)
            gTop[y][x] = kEmpty;
}

float worldAtPixel(float pixel)
{
    return pixel / float(kResolution) - 0.5f;
}

// Choose shear so the atlas-space path advances by (dx, dy) texels per metre of vertical fall.
int tracePixels(float pixelX, float pixelY, float dx, float dy, float pz = 1.0f,
                float headZ = 2.0f, float yaw = 0.0f, float frameZ = 0.0f,
                int maxSteps = 2 * kResolution + 1)
{
    const float shearX = -(dx * kExtent / float(kResolution));
    const float shearY = -(dy * kExtent / float(kResolution));
    return shShelterSegmentHit(worldAtPixel(pixelX), worldAtPixel(pixelY), pz,
                               shearX, shearY, 0.0f, 0.0f, frameZ, yaw, kExtent,
                               kResolution, headZ, 0.45f, 0, 0, maxSteps);
}

void expect(const char* name, int got, int wanted)
{
    if (got != wanted)
    {
        std::cerr << name << ": got " << got << ", expected " << wanted << '\n';
        std::abort();
    }
}
}

int main()
{
    // Calm vertical rain: a roof in the same column covers; an empty column stays open.
    clearTop();
    gTop[4][4] = 3.0f;
    expect("calm umbrella", tracePixels(4.5f, 4.5f, 0.0f, 0.0f), 1);
    clearTop();
    expect("calm open sky", tracePixels(4.5f, 4.5f, 0.0f, 0.0f), 0);

    // The old step-zero predicate accepted this high roof although the slanted segment leaves
    // its cell long before reaching the roof height.
    clearTop();
    gTop[4][4] = 4.0f;
    expect("same-column non-crossing roof", tracePixels(4.5f, 4.5f, 1.0f, 0.0f), 0);

    // Both wind signs on both atlas axes cross a roof at the actual segment height.
    clearTop(); gTop[4][6] = 2.6f;
    expect("positive X wind", tracePixels(4.5f, 4.5f, 1.0f, 0.0f), 1);
    clearTop(); gTop[4][2] = 2.6f;
    expect("negative X wind", tracePixels(4.5f, 4.5f, -1.0f, 0.0f), 1);
    clearTop(); gTop[6][4] = 2.6f;
    expect("positive Y wind", tracePixels(4.5f, 4.5f, 0.0f, 1.0f), 1);
    clearTop(); gTop[2][4] = 2.6f;
    expect("negative Y wind", tracePixels(4.5f, 4.5f, 0.0f, -1.0f), 1);

    // Broad flat and sloped roofs are traversed rather than reduced to the last candidate.
    clearTop();
    for (int x = 5; x <= 7; ++x) gTop[4][x] = 2.6f;
    expect("broad flat roof", tracePixels(4.5f, 4.5f, 1.0f, 0.0f), 1);
    clearTop();
    gTop[4][5] = 2.0f;
    gTop[4][6] = 3.0f;
    gTop[4][7] = 4.0f;
    expect("sloped roof", tracePixels(4.5f, 4.5f, 1.0f, 0.0f), 1);

    // Exact grid-corner crossing advances both axes; atlas-edge entry is also covered.
    clearTop(); gTop[6][6] = 2.6f;
    expect("diagonal corner", tracePixels(4.5f, 4.5f, 1.0f, 1.0f), 1);
    clearTop(); gTop[4][0] = 2.6f;
    expect("enter at atlas edge", tracePixels(-1.0f, 4.5f, 1.0f, 0.0f), 1);
    expect("leave away from atlas", tracePixels(-1.0f, 4.5f, -1.0f, 0.0f), 0);
    clearTop(); gTop[4][7] = 2.6f;
    expect("enter at opposite edge", tracePixels(9.0f, 4.5f, -1.0f, 0.0f), 1);
    expect("leave away from opposite edge", tracePixels(9.0f, 4.5f, 1.0f, 0.0f), 0);
    clearTop(); gTop[4][7] = 1.5f;
    expect("upper boundary moving inward", tracePixels(8.0f, 4.5f, -1.0f, 0.0f, 1.0f, 0.0f), 1);
    // A point-only corner touch uses diagonal/half-open ownership, not supercover.
    clearTop(); gTop[4][5] = 1.5f;
    expect("corner side only is open", tracePixels(4.5f, 4.5f, 1.0f, 1.0f, 1.0f, 0.0f), 0);
    clearTop(); gTop[5][5] = 1.5f;
    expect("corner diagonal is covered", tracePixels(4.5f, 4.5f, 1.0f, 1.0f, 1.0f, 0.0f), 1);

    // A 45-degree avatar frame rotates world shear into the atlas axes.
    clearTop(); gTop[2][5] = 2.6f;
    expect("rotated frame", tracePixels(4.5f, 4.5f, 1.0f, 0.0f, 1.0f, 2.0f, 0.78539816339f), 1);
    clearTop(); gTop[4][6] = 2.6f;
    expect("frame-relative height", tracePixels(4.5f, 4.5f, 1.0f, 0.0f, 11.0f, 12.0f, 0.0f, 10.0f), 1);

    // Explicitly exercise both bounded failure exits. They conservatively count as covered.
    clearTop(); gTop[4][6] = 2.6f;
    expect("truncated traversal", tracePixels(4.5f, 4.5f, 1.0f, 0.0f, 1.0f, 2.0f, 0.0f, 0.0f, 1), 2);
    expect("zero traversal budget", tracePixels(4.5f, 4.5f, 1.0f, 0.0f, 1.0f, 2.0f, 0.0f, 0.0f, 0), 2);
    expect("non-progress guard", shShelterSegmentHit(-1.25e19f, worldAtPixel(4.5f), 1.0f,
           -0.125f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, kExtent, kResolution,
           2.0f, 0.45f, 0, 0, 2 * kResolution + 1), 2);

    const float nan = std::bit_cast<float>(std::uint32_t{0x7fc00000u});
    const float inf = std::bit_cast<float>(std::uint32_t{0x7f800000u});
    clearTop(); gTop[4][4] = nan;
    expect("NaN atlas height in calm", tracePixels(4.5f, 4.5f, 0.0f, 0.0f), 2);
    gTop[4][4] = inf;
    expect("positive infinity atlas height", tracePixels(4.5f, 4.5f, 0.0f, 0.0f), 2);
    gTop[4][4] = -inf;
    expect("negative infinity atlas height", tracePixels(4.5f, 4.5f, 1.0f, 0.0f), 2);
    clearTop();
    expect("NaN segment height", tracePixels(4.5f, 4.5f, 0.0f, 0.0f, nan), 2);
    expect("infinite segment height", tracePixels(4.5f, 4.5f, 0.0f, 0.0f, inf), 2);
    expect("NaN head threshold", tracePixels(4.5f, 4.5f, 0.0f, 0.0f, 1.0f, nan), 2);
    expect("infinite frame height", tracePixels(4.5f, 4.5f, 0.0f, 0.0f, 1.0f, 2.0f, 0.0f, inf), 2);
    expect("NaN cover threshold", shShelterSegmentHit(worldAtPixel(4.5f), worldAtPixel(4.5f), 1.0f,
           0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, kExtent, kResolution,
           2.0f, nan, 0, 0, 2 * kResolution + 1), 2);

    std::cout << "weather shelter DDA geometry checks passed\n";
    return 0;
}
