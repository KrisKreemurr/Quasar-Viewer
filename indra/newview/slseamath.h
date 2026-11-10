/**
 * @file slseamath.h
 * @brief The sea state's pure math (#1020), shared by SLSeaState (slseastate.cpp) and the
 *        regression fixture (scripts/check/test_sea_math.cpp): no viewer types, header only.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * (LGPL v2.1, see slweather.h)
 * $/LicenseInfo$
 *
 * eval()           the CPU twin of clean_sea_state.glsl seaSurface's vertical part and masks (no
 *                  horizontal shift, no LOD fade): KEEP THE TWO IN STEP.
 * boreStrength()   the shore breakers' strength at a point (#1020 river fix): the global surf x
 *                  the LOCAL wave energy (the fetch-gated amplitudes, as a fetch-limited Hs over
 *                  the open sea's) x how squarely the shore faces the incoming waves. A lee shore
 *                  gets none, a narrow channel (little fetch across it) laps, an open beach facing
 *                  the wind keeps full breakers. (Before: the global surf everywhere within the surf
 *                  zone -- a river narrower than two zones was ALL surf zone and bores started at its
 *                  centre line, running out to both banks.)
 * reduceFloor()    the sea-floor pass's 1 m depth map -> the 4 m field: per block the 8th highest
 *                  of its 16 samples (a lower median over "nothing found" = -inf): a floor needs
 *                  at least half the block covered -- a post or a pillar (a few samples) is not a
 *                  shallow, a mesh beach / sandbar is.
 */
#ifndef SL_SEAMATH_H
#define SL_SEAMATH_H

#include <algorithm>
#include <cmath>
#include <functional>

namespace slsea
{
constexpr int   kWaves = 8;
constexpr float kTwoPi = 6.2831853f;
constexpr float kFloorNone = -1.0e30f;   // reduceFloor: no floor found in the block
constexpr float kRunupReach = 15.f;      // m inland the swash can reach
constexpr float kFacingFloor = 0.15f;    // boreStrength: refraction's share on a shore parallel to the waves

struct Waves
{
    float kvec[kWaves][2] = {};
    float k[kWaves] = {};
    float amp[kWaves] = {};
    float phase[kWaves] = {};
};
struct Params
{
    float hs = 0.f, surf = 0.f, runup = 0.f, swashPhase = 0.f, swashLen = 20.f;
    float dir[2] = {1.f, 0.f};   // the waves' travel direction (unit)
};
struct Texel
{
    float depth = 100.f, shore = 1000.f, fetch = 4000.f, dh = 0.f;
    float shoreN[2] = {0.f, 0.f};   // d shore / d xy (the offshore direction, unnormalized)
};

inline float smooth01(float e0, float e1, float x)
{
    const float t = std::min(std::max((x - e0) / (e1 - e0), 0.f), 1.f);
    return t * t * (3.f - 2.f * t);
}
inline float fract1(float x) { return x - std::floor(x); }
inline float bore(float psi) { return smooth01(0.f, 0.07f, psi) * std::pow(1.f - psi, 2.5f); }
inline float surfWidth(float hs) { return std::min(std::max(8.f * hs + 6.f, 6.f), 40.f); }

// Each wave's share of the open sea that builds over this much open water upwind.
inline float fetchGate(float k, float fetch)
{
    const float lam = kTwoPi / std::max(k, 1e-4f);
    return smooth01(3.f * lam, 12.f * lam, fetch);
}

inline float boreStrength(const Waves& w, const Params& p, const Texel& f)
{
    if (!(p.surf > 0.f)) return 0.f;
    float e2 = 0.f, o2 = 0.f;
    for (int i = 0; i < kWaves; ++i)
    {
        const float a = w.amp[i] * fetchGate(w.k[i], f.fetch);
        e2 += a * a;
        o2 += w.amp[i] * w.amp[i];
    }
    const float energy = o2 > 1e-12f ? std::min(std::sqrt(e2 / o2), 1.f) : 0.f;   // local Hs / open Hs
    const float l = std::sqrt(f.shoreN[0] * f.shoreN[0] + f.shoreN[1] * f.shoreN[1]);
    if (l < 1e-4f) return 0.f;   // (no shore orientation: no breakers)
    const float facing = -(p.dir[0] * f.shoreN[0] + p.dir[1] * f.shoreN[1]) / l;   // 1: waves straight in
    const float dirK = std::min(std::max((facing + kFacingFloor) / (1.f + kFacingFloor), 0.f), 1.f);
    return p.surf * energy * dirK;
}

inline void eval(const Waves& w, const Params& p, const Texel& f, float x, float y,
                 float& height, float& breaking, float& swash)
{
    height = 0.f; breaking = 0.f; swash = 0.f;
    const float d = std::max(f.depth, 0.f);
    float A[kWaves];
    float aSum = 0.f;
    for (int i = 0; i < kWaves; ++i)
    {
        const float th = std::tanh(std::min(w.k[i] * d, 8.f));
        A[i] = w.amp[i] * fetchGate(w.k[i], f.fetch) * (1.f + 0.6f * (1.f - th));
        aSum += A[i];
    }
    const float aMax = 0.42f * d;
    breaking = f.depth > 0.f ? smooth01(0.85f, 1.4f, aSum / std::max(aMax, 1e-3f)) * smooth01(0.f, 0.05f, aSum) : 0.f;
    const float cap = aSum > aMax ? aMax / std::max(aSum, 1e-6f) : 1.f;
    for (int i = 0; i < kWaves; ++i)
        height += A[i] * cap * std::cos(w.kvec[i][0] * x + w.kvec[i][1] * y + w.phase[i]);
    const float W = surfWidth(p.hs);
    const float Rl = kRunupReach;
    const float kb = boreStrength(w, p, f);
    if (kb > 0.f && f.shore < W && f.shore > -Rl)
    {
        const float psi = fract1(p.swashPhase + f.shore / std::max(p.swashLen, 1.f));
        const float b = bore(psi);
        const float hb = std::min(0.45f * d + 0.05f, 0.6f * p.hs + 0.05f);
        const float E = f.shore >= 0.f
            ? (p.runup + (hb - p.runup) * smooth01(0.f, 0.35f * W, f.shore)) * (1.f - smooth01(0.35f * W, W, f.shore))
            : p.runup * smooth01(-Rl, 0.f, f.shore);
        const float lift = kb * b * E;
        height += lift;
        const float front = smooth01(0.f, 0.05f, psi) * (1.f - smooth01(0.07f, 0.3f, psi));
        breaking = std::max(breaking, kb * front * (1.f - smooth01(0.35f * W, W, std::max(f.shore, 0.f))));
        swash = smooth01(0.f, 0.08f, f.depth + lift) * (1.f - smooth01(-0.1f, 0.5f, f.depth)) * smooth01(0.f, 0.02f, lift);
    }
}

// hi: hiRes^2 floor heights (row-major; kFloorNone = nothing), out: (hiRes / factor)^2. Rows
// [row0, row1) of out only (the caller spreads the work over frames).
inline void reduceFloor(const float* hi, int hiRes, int factor, float* out, int row0, int row1)
{
    const int lo = hiRes / factor;
    const int n = factor * factor;
    const int keep = std::max(n / 2 - 1, 0);   // 0-based rank, descending: 16 samples -> the 8th highest
    float s[64];
    for (int j = row0; j < row1 && j < lo; ++j)
        for (int i = 0; i < lo; ++i)
        {
            int c = 0;
            for (int y = 0; y < factor; ++y)
                for (int x = 0; x < factor && c < 64; ++x)
                    s[c++] = hi[(size_t)(j * factor + y) * hiRes + (i * factor + x)];
            std::nth_element(s, s + keep, s + c, std::greater<float>());
            out[(size_t)j * lo + i] = s[keep];
        }
}
} // namespace slsea

#endif // SL_SEAMATH_H
