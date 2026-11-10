// Fixtures for the ray-traced hits' light sampling (the sealed-room leak; indra/vkclean/shaders/clean_hit_nee.h,
// the very code rtHitLocalLights runs on the GPU), run by scripts/check/checks/hit_nee.py with the host C++
// compiler (no viewer build, no GPU).
//   SEALED   a GI hit inside a closed room: every light reaching it lies outside, every shadow ray is blocked
//            -> 0 (float rounding) for every seed and every K (the old brightest-K rule lit it with the rest, unshadowed).
//   OWN      the room's own lamp (visible) + the next room's lamps (blocked) -> the mean is the own lamp alone.
//   MEAN     random light sets and visibilities -> the mean over seeds is the exact shadowed sum (unbiased).
//   SLOTS    the budget's slot contract: a valid prefix, every pick's luminance W / K (no firefly above the total).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

typedef uint32_t uint;
struct vec3
{
    float x, y, z;
    vec3() : x(0.f), y(0.f), z(0.f) {}
    explicit vec3(float v) : x(v), y(v), z(v) {}
    vec3(float a, float b, float c) : x(a), y(b), z(c) {}
};
static vec3 operator+(vec3 a, vec3 b) { return vec3(a.x + b.x, a.y + b.y, a.z + b.z); }
static vec3 operator*(vec3 a, float s) { return vec3(a.x * s, a.y * s, a.z * s); }
static vec3 operator*(vec3 a, vec3 b) { return vec3(a.x * b.x, a.y * b.y, a.z * b.z); }

#include "clean_hit_nee.h"

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

struct Share { vec3 light; vec3 vis; };   // a light's unshadowed share at the hit and its true visibility

// rtHitLocalLights' sampled path: every share a candidate, K picks, each traced (vis) and subtracted as
// share x (1 - vis) -- the inline loop's / vqHitLocal's resolve.
static vec3 estimate(const std::vector<Share>& s, uint K, uint seed)
{
    float score[HN_MAXK]; uint light[HN_MAXK]; vec3 share[HN_MAXK];
    hnInit(score, light, share);
    float W = 0.f;
    for (uint i = 0; i < (uint)s.size(); ++i) W = hnAdd(W, K, seed, i, i, s[i].light, score, light, share);
    vec3 sum = hnFinish(W, K, score, share);
    for (uint j = 0; j < K; ++j)
    {
        if (score[j] <= 0.f) break;
        const vec3 v = s[light[j]].vis;
        sum = sum + share[j] * vec3(-1.f) * vec3(1.f - v.x, 1.f - v.y, 1.f - v.z);
    }
    return vec3(std::fmax(sum.x, 0.f), std::fmax(sum.y, 0.f), std::fmax(sum.z, 0.f));
}
// The rule it replaced (clean_rt_shade.glsl before): the K brightest traced, the rest unshadowed.
static vec3 brightestK(const std::vector<Share>& s, uint K)
{
    std::vector<int> ord;
    for (int i = 0; i < (int)s.size(); ++i) ord.push_back(i);
    for (size_t a = 0; a < ord.size(); ++a)
        for (size_t b = a + 1; b < ord.size(); ++b)
            if (hnLum(s[ord[b]].light) > hnLum(s[ord[a]].light)) std::swap(ord[a], ord[b]);
    vec3 sum;
    for (size_t r = 0; r < ord.size(); ++r)
        sum = sum + (r < K ? s[ord[r]].light * s[ord[r]].vis : s[ord[r]].light);
    return sum;
}
static vec3 exact(const std::vector<Share>& s)
{
    vec3 sum;
    for (const Share& x : s) sum = sum + x.light * x.vis;
    return sum;
}
static uint rng(uint& st) { st = hnHash(st); return st; }
static float frand(uint& st) { return float(rng(st) >> 8) / 16777216.f; }

static void testSealed()
{
    // a pink lamp in the next room (radius reaches the bathroom), a warm one, a dim hall light: all behind walls
    const std::vector<Share> s = {{vec3(0.9f, 0.4f, 0.5f), vec3(0.f)},
                                  {vec3(0.6f, 0.5f, 0.2f), vec3(0.f)},
                                  {vec3(0.05f, 0.05f, 0.05f), vec3(0.f)}};
    for (uint K = 1; K <= 4; ++K)
        for (uint seed = 0; seed < 4096; ++seed)
        {
            const vec3 e = estimate(s, K, seed * 2654435761u);
            // (0 up to the float rounding of sum - share: below 1e-6 of the light at the hit)
            CHECK(e.x < 1e-6f && e.y < 1e-6f && e.z < 1e-6f, "sealed room lit: K %u seed %u -> %g %g %g", K, seed, e.x, e.y, e.z);
            if (g_fail) return;
        }
    const vec3 old = brightestK(s, 1);
    CHECK(hnLum(old) > 0.1f, "the old rule should leak here (fixture sanity): %g", hnLum(old));
}

static void testOwnLamp()
{
    // the room's own dim lamp visible, two brighter lamps next door blocked
    const std::vector<Share> s = {{vec3(1.2f, 0.6f, 0.6f), vec3(0.f)},
                                  {vec3(0.10f, 0.10f, 0.09f), vec3(1.f)},
                                  {vec3(0.8f, 0.7f, 0.3f), vec3(0.f)}};
    for (uint K = 1; K <= 4; ++K)
    {
        vec3 acc;
        const uint n = 200000;
        for (uint seed = 0; seed < n; ++seed) acc = acc + estimate(s, K, seed * 0x9E3779B9u + 17u);
        const vec3 m = acc * (1.f / float(n));
        const vec3 want = exact(s);
        CHECK(std::fabs(m.x - want.x) < 0.02f * want.x + 2e-3f && std::fabs(m.y - want.y) < 0.02f * want.y + 2e-3f
              && std::fabs(m.z - want.z) < 0.02f * want.z + 2e-3f,
              "own lamp mean K %u: %g %g %g, want %g %g %g", K, m.x, m.y, m.z, want.x, want.y, want.z);
    }
}

static void testMean()
{
    uint st = 1440u;
    for (int t = 0; t < 40; ++t)
    {
        std::vector<Share> s;
        const int nl = 1 + int(rng(st) % 12u);
        for (int i = 0; i < nl; ++i)
        {
            const float b = frand(st) < 0.3f ? 4.f : 0.3f;
            const float v = frand(st) < 0.5f ? 0.f : frand(st);
            s.push_back({vec3(b * frand(st), b * frand(st), b * frand(st)), vec3(v, v * frand(st), v)});
        }
        const uint K = 1u + rng(st) % 4u;
        vec3 acc;
        const uint n = 100000;
        for (uint seed = 0; seed < n; ++seed) acc = acc + estimate(s, K, hnHash(seed + 7919u * uint(t)));
        const vec3 m = acc * (1.f / float(n));
        const vec3 want = exact(s);
        float W = 0.f;
        for (const Share& x : s) W += hnLum(x.light);
        const float tl = 0.03f * W + 1e-3f;   // (relative to the light at the hit: the estimator's scale)
        CHECK(std::fabs(m.x - want.x) < tl && std::fabs(m.y - want.y) < tl && std::fabs(m.z - want.z) < tl,
              "biased: case %d (%d lights, K %u): %g %g %g, want %g %g %g", t, nl, K, m.x, m.y, m.z, want.x, want.y, want.z);
    }
}

static void testSlots()
{
    const std::vector<Share> s = {{vec3(2.f, 1.f, 0.5f), vec3(1.f)}, {vec3(0.01f, 0.f, 0.f), vec3(1.f)},
                                  {vec3(0.f), vec3(1.f)}, {vec3(0.3f, 0.3f, 0.3f), vec3(1.f)}};
    float W = 0.f;
    for (const Share& x : s) W += hnLum(x.light);
    for (uint K = 1; K <= 4; ++K)
        for (uint seed = 0; seed < 512; ++seed)
        {
            float score[HN_MAXK]; uint light[HN_MAXK]; vec3 share[HN_MAXK];
            hnInit(score, light, share);
            float w = 0.f;
            for (uint i = 0; i < (uint)s.size(); ++i) w = hnAdd(w, K, seed, i, i, s[i].light, score, light, share);
            hnFinish(w, K, score, share);
            for (uint j = 0; j < (uint)HN_MAXK; ++j)
            {
                if (j < K)
                {
                    CHECK(score[j] > 0.f && light[j] != 2u, "slot %u of K %u empty / a black light picked", j, K);
                    CHECK(std::fabs(hnLum(share[j]) - W / float(K)) < 1e-4f * W, "pick %u luminance %g != W/K %g", j,
                          hnLum(share[j]), W / float(K));
                }
                else CHECK(score[j] == 0.f, "slot %u past K %u holds a pick", j, K);
            }
        }
    // no light at all: no pick, nothing added
    float score[HN_MAXK]; uint light[HN_MAXK]; vec3 share[HN_MAXK];
    hnInit(score, light, share);
    const vec3 z = hnFinish(0.f, 1u, score, share);
    CHECK(z.x == 0.f && score[0] == 0.f, "an empty walk produced a pick");
}

static void testTiny()
{
    // a bright lamp + a share at the cutoff fade's edge (denormal-small): every seed finite, the tiny one never picked
    const std::vector<Share> s = {{vec3(1.f, 0.8f, 0.6f), vec3(1.f)}, {vec3(1e-38f, 1e-38f, 1e-38f), vec3(1.f)},
                                  {vec3(3e-21f, 0.f, 0.f), vec3(0.f)}};
    for (uint K = 1; K <= 4; ++K)
        for (uint seed = 0; seed < 1u << 16; ++seed)
        {
            const vec3 e = estimate(s, K, hnHash(seed));
            CHECK(std::isfinite(e.x) && std::isfinite(e.y) && std::isfinite(e.z) && e.x < 1.01f,
                  "non-finite / overflowing estimate: K %u seed %u -> %g %g %g", K, seed, e.x, e.y, e.z);
            if (g_fail) return;
        }
    // the uniform is exactly 0 for some candidate (the case that overflowed): a 0 draw must not pick a tiny share
    float score[HN_MAXK]; uint light[HN_MAXK]; vec3 share[HN_MAXK];
    hnInit(score, light, share);
    float W = hnAdd(0.f, 1u, 0u, 0u, 0u, vec3(1.f), score, light, share);
    W = hnAdd(W, 1u, 0u, 1u, 1u, vec3(1e-38f), score, light, share);
    CHECK(light[0] == 0u, "a share below HN_MIN_W became a pick");
}

int main()
{
    testTiny();
    testSealed();
    testOwnLamp();
    testMean();
    testSlots();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("ok\n");
    return 0;
}
