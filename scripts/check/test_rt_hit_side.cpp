// GI-hit shading-normal side fixture (scripts/check/checks/gi_hit_side.py): the rule rtSurfaceEval applies to a
// ray-traced hit's shading normal (indra/vkclean/shaders/clean_rt_hit_side.h, included as-is), driven through the
// hit's local-light step as the GI kernels run it:
//   * the light term N . l (clean_rt_shade.glsl rtHitLocalLights: legacy multiPointLightF, point light);
//   * its shadow ray as rtrProjRaySetup (clean_deferred_light.comp) aims it: the origin 1 cm along the SHADING
//     normal turned toward the light, n = dot(N, l) < 0 ? -N : N;
//   * the geometry: a thin double-sided sheet (the plane z = 0, |x|, |y| < 1); the ray arrives from below (a
//     surface just behind a back-facing layer shooting its GI ray at it). A shadow segment that crosses the
//     sheet is blocked.
//   FLIPPED  the sheet's vertex normals oppose its winding (a flipped-normal upload / an inside-out sculpt): under
//            the raster's winding rule the shading normal points away from the ray, a lamp 5 cm BEHIND the sheet
//            lights the side the ray sees and its shadow ray starts on the lamp's side -- unshadowed light
//            through the layer (the old GI behaviour, kept for the RT reflections); under the arrival rule (the
//            GI hits) the lamp behind gives 0 and a lamp on the ray's side lights it as for a sheet whose normals
//            agree.
//   AGREE    normals that agree with the winding: both rules give the same normal (no change).
//   SIDE     random triangles, rays and vertex normals: the arrival rule's normal (and a normal-mapped one taking
//            its facing) always lies on the arrival side; the raster rule is the winding's facing, bit for bit.
#include <cmath>
#include <cstdint>
#include <cstdio>

struct vec3
{
    float x, y, z;
    vec3() : x(0.f), y(0.f), z(0.f) {}
    vec3(float a, float b, float c) : x(a), y(b), z(c) {}
};
static vec3 operator+(vec3 a, vec3 b) { return vec3(a.x + b.x, a.y + b.y, a.z + b.z); }
static vec3 operator-(vec3 a, vec3 b) { return vec3(a.x - b.x, a.y - b.y, a.z - b.z); }
static vec3 operator-(vec3 a) { return vec3(-a.x, -a.y, -a.z); }
static vec3 operator*(vec3 a, float s) { return vec3(a.x * s, a.y * s, a.z * s); }
static float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float length(vec3 a) { return std::sqrt(dot(a, a)); }
static vec3 normalize(vec3 a) { return a * (1.f / length(a)); }

#include "clean_rt_hit_side.h"

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

// the sheet: z = 0, |x|, |y| < 1
static bool crossesSheet(vec3 a, vec3 b)
{
    if ((a.z > 0.f) == (b.z > 0.f)) return false;
    const float t = a.z / (a.z - b.z);
    const vec3 p = a + (b - a) * t;
    return std::fabs(p.x) < 1.f && std::fabs(p.y) < 1.f;
}
// rtHitLocalLights' legacy point term at hit P (shading normal N) x the shadow ray rtrProjRaySetup traces
static float hitLight(vec3 P, vec3 N, vec3 lamp, float radius)
{
    const vec3 lv = lamp - P;
    const float d = length(lv);
    const vec3 l = lv * (1.f / d);
    const float atten = std::fmax(1.f - d / radius, 0.f);
    const float nl = dot(N, l);
    if (!(nl > 0.f) || atten <= 0.f) return 0.f;
    const vec3 n = dot(N, l) < 0.f ? -N : N;   // (rtrProjRaySetup)
    const vec3 o = P + n * 0.01f;
    return crossesSheet(o, lamp) ? 0.f : nl * atten;
}
// the shading normal rtSurfaceEval gives the hit (dir: the ray, ng: the winding normal, nv: the vertex normal)
static vec3 shadingN(vec3 dir, vec3 ng, vec3 nv, bool arrival)
{
    const bool front = dot(ng, dir) < 0.f;   // (rtSurfaceEval: s.front)
    return rtHitNormalFront(front, nv, ng, arrival) ? nv : -nv;
}

static uint32_t g_rng = 0x12345678u;
static float rnd() { g_rng = g_rng * 1664525u + 1013904223u; return float(g_rng >> 8) * (1.f / 16777216.f); }
static vec3 rndDir() { for (;;) { vec3 v(2.f * rnd() - 1.f, 2.f * rnd() - 1.f, 2.f * rnd() - 1.f); const float l = length(v); if (l > 0.05f && l <= 1.f) return v * (1.f / l); } }

int main()
{
    const vec3 ng(0.f, 0.f, 1.f);              // the sheet's winding normal (+z)
    const vec3 recv(0.1f, 0.05f, -0.3f);       // the surface behind the layer (the ray's origin side, z < 0)
    const vec3 behind(0.f, 0.f, 0.05f);        // a lamp 5 cm BEHIND the sheet (its far side from the ray)
    const vec3 nearLamp(0.2f, -0.1f, -0.25f);      // a lamp on the ray's side
    const float R = 3.f;
    // every hit of the receiver's rays on the sheet (a grid over the square)
    float leakRaster = 0.f, leakArrival = 0.f, peakRaster = 0.f;
    int hits = 0;
    for (int iy = -9; iy <= 9; ++iy)
        for (int ix = -9; ix <= 9; ++ix)
        {
            const vec3 P(0.1f * float(ix), 0.1f * float(iy), 0.f);
            const vec3 dir = normalize(P - recv);
            ++hits;
            // FLIPPED: the vertex normal opposes the winding
            const vec3 nvF = -ng;
            const vec3 Nr = shadingN(dir, ng, nvF, false), Na = shadingN(dir, ng, nvF, true);
            const float lr = hitLight(P, Nr, behind, R), la = hitLight(P, Na, behind, R);
            leakRaster += lr;
            leakArrival += la;
            peakRaster = std::fmax(peakRaster, lr);
            CHECK(la == 0.f, "FLIPPED hit (%g, %g): the lamp behind the sheet lights it through the sheet (%g)", P.x, P.y, la);
            CHECK(dot(Na, dir) < 0.f, "FLIPPED hit (%g, %g): the GI hit's normal points away from its ray", P.x, P.y);
            // ... and the lamp on the ray's side lights it as a sheet whose normals agree
            const vec3 nvA = ng;
            const float want = hitLight(P, shadingN(dir, ng, nvA, true), nearLamp, R);
            const float got = hitLight(P, Na, nearLamp, R);
            CHECK(want > 0.f, "fixture sanity: the ray-side lamp lights nothing at (%g, %g)", P.x, P.y);
            CHECK(std::fabs(got - want) < 1e-6f, "FLIPPED hit (%g, %g): ray-side lamp %g, want %g (the agreeing sheet's)", P.x, P.y, got, want);
            // AGREE: no change
            const vec3 a0 = shadingN(dir, ng, nvA, false), a1 = shadingN(dir, ng, nvA, true);
            CHECK(a0.x == a1.x && a0.y == a1.y && a0.z == a1.z, "AGREE hit (%g, %g): the rule changed an agreeing normal", P.x, P.y);
        }
    // the mechanism the fix removes, as the raster rule still gives it (fixture sanity: the old GI rule leaked)
    CHECK(leakRaster > 0.05f * float(hits) && peakRaster > 0.9f,
          "fixture sanity: the raster rule no longer leaks the lamp behind (sum %g, peak %g) -- the fixture models nothing", leakRaster, peakRaster);
    CHECK(leakArrival == 0.f, "FLIPPED: %g of lamp light through the sheet under the GI rule", leakArrival);
    // SIDE: random geometry
    for (int i = 0; i < 200000; ++i)
    {
        const vec3 g = rndDir(), d = rndDir(), nv = rndDir();
        if (std::fabs(dot(g, d)) < 1e-3f || std::fabs(dot(nv, g)) < 1e-3f) continue;
        const bool front = dot(g, d) < 0.f;
        const vec3 Ngarr = front ? g : -g;   // (rtSurfaceEval: s.Ng, the arrival side)
        const bool sa = rtHitNormalFront(front, nv, g, true);
        const vec3 Na = sa ? nv : -nv;
        CHECK(dot(Na, Ngarr) > 0.f, "SIDE %d: the GI hit's normal is off the arrival side (%g)", i, dot(Na, Ngarr));
        // a normal-mapped normal (the vertex normal tilted < 60 deg off it) takes the same facing
        const vec3 nm = normalize(nv + rndDir() * 0.5f);
        const vec3 Nm = sa ? nm : -nm;
        CHECK(dot(Nm, Na) > 0.f, "SIDE %d: the normal map's result does not take the vertex normal's facing", i);
        CHECK(rtHitNormalFront(front, nv, g, false) == front, "SIDE %d: the raster rule is not the winding's facing", i);
        if (g_fail) break;
    }
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("ok (raster rule leak: %g over %d hits, peak %g; GI rule: 0)\n", leakRaster, hits, peakRaster);
    return 0;
}
