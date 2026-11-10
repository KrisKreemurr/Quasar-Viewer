// Fixtures for the sea state's pure math (#1020; indra/newview/slseamath.h), run by
// scripts/check/checks/sea_math.py (compiled with the host C++ compiler, no viewer build).
//   FLOOR   the sea-floor pass's 1 m depth map -> the field's 4 m texels (slsea::reduceFloor):
//           a thin pillar is ignored, a mesh floor is found, nothing found keeps the terrain.
//   BORES   the shore breakers' strength (slsea::boreStrength): a 20 m river laps, a lee shore
//           gets none, a windward beach keeps full breakers.
#include "slseamath.h"

#include <cstdio>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

// A storm-ish sea on SLSeaState's fixed ladder (80 .. 4 m), travelling +x.
static void testWaves(slsea::Waves& w, slsea::Params& p)
{
    const float lam[slsea::kWaves] = {80.f, 52.f, 34.f, 22.f, 14.5f, 9.5f, 6.2f, 4.f};
    const float amp[slsea::kWaves] = {0.55f, 0.45f, 0.3f, 0.2f, 0.12f, 0.07f, 0.04f, 0.02f};
    float m0 = 0.f;
    for (int i = 0; i < slsea::kWaves; ++i)
    {
        w.k[i] = slsea::kTwoPi / lam[i];
        w.kvec[i][0] = w.k[i]; w.kvec[i][1] = 0.f;
        w.amp[i] = amp[i];
        m0 += 0.5f * amp[i] * amp[i];
    }
    p.hs = 4.f * std::sqrt(m0);
    p.surf = 1.f;
    p.runup = 0.8f;
    p.swashLen = 30.f;
    p.dir[0] = 1.f; p.dir[1] = 0.f;
}

static void testFloor()
{
    const int hi = 16, f = 4, lo = hi / f;
    std::vector<float> h((size_t)hi * hi, slsea::kFloorNone), out((size_t)lo * lo, 0.f);
    // block (0, 0): one 1 m pillar reaching -1 m, nothing else -> no floor
    h[1 * hi + 1] = -1.f;
    // block (1, 0): a mesh sandbar at -2 m over the whole block (a post at -0.5 m on it)
    for (int y = 0; y < 4; ++y) for (int x = 4; x < 8; ++x) h[(size_t)y * hi + x] = -2.f;
    h[2 * hi + 5] = -0.5f;
    // block (2, 0): half covered (8 of 16) at -3 m -> found
    for (int y = 0; y < 2; ++y) for (int x = 8; x < 12; ++x) h[(size_t)y * hi + x] = -3.f;
    // block (3, 0): 7 of 16 -> not enough
    for (int k = 0; k < 7; ++k) h[(size_t)(k / 4) * hi + 12 + (k % 4)] = -3.f;
    // block (0, 1): a sloping mesh beach, -4 .. -1 m across the block
    for (int y = 4; y < 8; ++y) for (int x = 0; x < 4; ++x) h[(size_t)y * hi + x] = -4.f + (float)x;
    slsea::reduceFloor(h.data(), hi, f, out.data(), 0, lo);
    CHECK(out[0] <= slsea::kFloorNone * 0.5f, "thin pillar made a floor: %g", out[0]);
    CHECK(out[1] == -2.f, "mesh sandbar floor %g (want -2: the post on it ignored)", out[1]);
    CHECK(out[2] == -3.f, "half-covered block %g (want -3)", out[2]);
    CHECK(out[3] <= slsea::kFloorNone * 0.5f, "7 / 16 covered made a floor: %g", out[3]);
    CHECK(out[lo] > -4.f && out[lo] < -1.f, "sloping beach floor %g (want inside -4 .. -1)", out[lo]);
    // nothing found anywhere else: the terrain stays the floor (the caller's rule)
    for (int k = lo + 1; k < lo * lo; ++k) CHECK(out[k] <= slsea::kFloorNone * 0.5f, "empty block %d made a floor", k);
}

static void testBores()
{
    slsea::Waves w; slsea::Params p;
    testWaves(w, p);
    // WINDWARD BEACH: waves travel +x onto a beach to the east; offshore = -x; open sea upwind.
    slsea::Texel beach; beach.depth = 1.f; beach.shore = 8.f; beach.fetch = 4000.f;
    beach.shoreN[0] = -1.f; beach.shoreN[1] = 0.f;
    const float kBeach = slsea::boreStrength(w, p, beach);
    CHECK(kBeach > 0.95f, "windward beach strength %g (want ~1: full breakers)", kBeach);
    // LEE SHORE: land to the west, the sea downwind of it: offshore = +x, land right upwind.
    slsea::Texel lee; lee.depth = 1.f; lee.shore = 8.f; lee.fetch = 8.f;
    lee.shoreN[0] = 1.f; lee.shoreN[1] = 0.f;
    const float kLee = slsea::boreStrength(w, p, lee);
    CHECK(kLee == 0.f, "lee shore strength %g (want 0)", kLee);
    // A 20 m RIVER across the wind: its downwind (windward-facing) bank has 20 m of fetch.
    slsea::Texel bank; bank.depth = 1.f; bank.shore = 4.f; bank.fetch = 20.f;
    bank.shoreN[0] = -1.f; bank.shoreN[1] = 0.f;
    const float kBank = slsea::boreStrength(w, p, bank);
    CHECK(kBank < 0.1f, "20 m river bank strength %g (want < 0.1: lapping)", kBank);
    // ... its upwind bank faces away from the waves.
    slsea::Texel bank2 = bank; bank2.shoreN[0] = 1.f;
    CHECK(slsea::boreStrength(w, p, bank2) == 0.f, "river's upwind bank has breakers");
    // ... and its CENTRE LINE (the shore-distance ridge, ~0 gradient) starts nothing.
    slsea::Texel centre = bank; centre.shore = 10.f; centre.shoreN[0] = 0.f;
    CHECK(slsea::boreStrength(w, p, centre) == 0.f, "river centre line starts bores");
    // A RIVER ALONG THE WIND (long fetch): its banks are parallel to the waves -- refraction's floor.
    slsea::Texel along = bank; along.fetch = 4000.f; along.shoreN[0] = 0.f; along.shoreN[1] = 1.f;
    const float kAlong = slsea::boreStrength(w, p, along);
    CHECK(kAlong < 0.2f, "bank parallel to the waves strength %g (want < 0.2)", kAlong);
    // The surface follows: on the 20 m river the swash lift stays tiny, on the beach it runs up.
    float hb = 0.f, bb = 0.f, sb = 0.f, hr = 0.f, br = 0.f, sr = 0.f, peakB = 0.f, peakR = 0.f;
    for (int t = 0; t < 64; ++t)
    {
        p.swashPhase = (float)t / 64.f;
        slsea::Texel b2 = beach; b2.depth = -0.2f; b2.shore = -2.f;
        slsea::Texel r2 = bank;  r2.depth = -0.2f; r2.shore = -2.f;
        slsea::eval(w, p, b2, 0.f, 0.f, hb, bb, sb);
        slsea::eval(w, p, r2, 0.f, 0.f, hr, br, sr);
        peakB = std::max(peakB, hb); peakR = std::max(peakR, hr);
    }
    CHECK(peakB > 0.5f, "beach swash never runs up (peak lift %g m)", peakB);
    CHECK(peakR < 0.1f * peakB, "river bank swash %g m vs beach %g m (want lapping)", peakR, peakB);
}

int main()
{
    testFloor();
    testBores();
    if (g_fail) { std::printf("%d sea math fixture(s) failed\n", g_fail); return 1; }
    std::printf("sea math fixtures passed\n");
    return 0;
}
