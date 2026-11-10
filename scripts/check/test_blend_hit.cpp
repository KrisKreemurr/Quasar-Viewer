// Fixture for THE BLEND LAYERS' OWN LIGHT (RenderVKBlendHitLighting), run on the shaders' OWN code:
// scripts/check/checks/blend_hit.py preprocesses every forward lit FS that carries the path (clean_draw.frag's
// ray-query variants: the pass-2 blend FS, its wire / WBOIT-accumulation twins, the plain lit FS that draws the
// cluster-LOD rigged alpha; clean_oit_resolve.frag -DOIT_LIT's ray-query twin) per variant (glslc -E, the
// variant's own -D flags from indra/vkclean/regen_world_spv.py), cuts the layer's light walk (rtHitLocalLights,
// with the forward hooks expanded: the froxel mask, the exact slots, the glare) and its GI (giForwardRefl,
// giFwdCache, giFwdOn, blendHitEnv) out of it, turns the GLSL into C++ and compiles it in as BH_FN_FILE. The shims
// below stand in for the light buffers, the BRDFs, the froxel cell, the shadow / occlusion rays and the radiance
// cache; the walk, its budget, its estimator and the GI fallback are the shader's.
//   SEALED   a glass layer in a closed room: every light reaching it lies outside (a point lamp, a record-less
//            projector, a ray-traced projector record, a shadow-map record), every shadow test blocked -> 0 for
//            every frame, from the froxel cell and from the cluster list. (The forward walk lit it with every point
//            light and the record-less projector unshadowed: the old path, or the next room's lamp record.)
//   OWN      the room's own lamp visible beside those -> the mean over frames is the own lamp alone.
//   EXACT    the shadowed face: the key light blocked, a fill light visible (two lights: both in the exact slots)
//            -> the fill light alone on EVERY frame (no estimator noise where the strongest lights dominate).
//   NOISE    a lamp + five dim lights (half blocked): the frame-to-frame RMS error below plain next-event
//            estimation with the same 4 rays (clean_hit_nee.h hnAdd, K = 4) -- the measured budget choice.
//   BUDGET   at most BLEND_HIT_M + BLEND_HIT_K shadow rays per shaded layer, whatever the light count (16 lights).
//   GLARE    a legacy shiny layer under a lamp: LL's alpha glare from its local specular (coverage under highlights).
//   GI       a glass layer with no radiance-cache cell: its own occlusion ray blocked (a sealed room) -> zero
//            ambient and zero environment specular with the probes on or off, in every GI hit-ambient mode; the
//            ray escaped (outdoors) -> the probes' ambient (modes 1, 2) and radiance kept; mode 0 / 3 -> no probe
//            ambient; the cache's own cover -> the cache; an RT-traced forward reflection keeps its specular.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

typedef uint32_t uint;
struct vec2 { float x, y; vec2(float a = 0.f, float b = 0.f) : x(a), y(b) {} vec2 xy() const { return *this; } };
struct vec3
{
    float x, y, z;
    vec3() : x(0.f), y(0.f), z(0.f) {}
    explicit vec3(float v) : x(v), y(v), z(v) {}
    vec3(float a, float b, float c) : x(a), y(b), z(c) {}
    vec3 xyz() const { return *this; }
    vec3& operator+=(vec3 o) { x += o.x; y += o.y; z += o.z; return *this; }
    vec3& operator-=(vec3 o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    vec3& operator*=(vec3 o) { x *= o.x; y *= o.y; z *= o.z; return *this; }
    vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
};
static vec3 operator+(vec3 a, vec3 b) { return vec3(a.x + b.x, a.y + b.y, a.z + b.z); }
static vec3 operator-(vec3 a, vec3 b) { return vec3(a.x - b.x, a.y - b.y, a.z - b.z); }
static vec3 operator-(vec3 a) { return vec3(-a.x, -a.y, -a.z); }
static vec3 operator*(vec3 a, vec3 b) { return vec3(a.x * b.x, a.y * b.y, a.z * b.z); }
static vec3 operator*(vec3 a, float s) { return vec3(a.x * s, a.y * s, a.z * s); }
static vec3 operator*(float s, vec3 a) { return a * s; }
static vec3 operator/(vec3 a, float s) { return vec3(a.x / s, a.y / s, a.z / s); }
static vec3 operator/(vec3 a, vec3 b) { return vec3(a.x / b.x, a.y / b.y, a.z / b.z); }
struct vec4
{
    float x, y, z, w;
    vec4(float a = 0.f, float b = 0.f, float c = 0.f, float d = 0.f) : x(a), y(b), z(c), w(d) {}
    vec4(vec3 v, float d) : x(v.x), y(v.y), z(v.z), w(d) {}
    vec3 xyz() const { return vec3(x, y, z); }
    vec2 xy() const { return vec2(x, y); }
};
struct uvec4 { uint x, y, z, w; };
struct mat4 { float m[16]; };

static float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float length(vec3 a) { return std::sqrt(dot(a, a)); }
static vec3 normalize(vec3 a) { return a / length(a); }
static float max(float a, float b) { return a > b ? a : b; }
static float min(float a, float b) { return a < b ? a : b; }
static uint max(uint a, uint b) { return a > b ? a : b; }
static uint min(uint a, uint b) { return a < b ? a : b; }
static vec3 max(vec3 a, vec3 b) { return vec3(max(a.x, b.x), max(a.y, b.y), max(a.z, b.z)); }
static vec3 min(vec3 a, vec3 b) { return vec3(min(a.x, b.x), min(a.y, b.y), min(a.z, b.z)); }
static float clamp(float v, float a, float b) { return min(max(v, a), b); }
static vec3 clamp(vec3 v, vec3 a, vec3 b) { return min(max(v, a), b); }
static float mix(float a, float b, float t) { return a * (1.f - t) + b * t; }
static vec3 mix(vec3 a, vec3 b, float t) { return a * (1.f - t) + b * t; }
static uint floatBitsToUint(float f) { uint u; std::memcpy(&u, &f, 4); return u; }
static int bitCount(uint v) { return __builtin_popcount(v); }
static int findLSB(uint v) { return v ? __builtin_ctz(v) : -1; }

#include "clean_hit_nee.h"

// ---- the light buffers ----
struct Light { vec4 pos_radius, color_falloff, proj_params, proj_origin, proj_axis; mat4 proj_mat; uvec4 tex; };
struct ProjShadow { vec4 params; };
struct LegacySpec { vec3 spec; float gloss, env; };
struct LInfo { vec4 cam_pos; };
struct DrawView { vec4 u_cam_time; };   // (pc.u_cam_time: the draw view's, CLEAN_VIEW)
struct DrawPush { uint view; };
static Light lights[16];
static ProjShadow pshadow[4];
static uint cluster_count[1] = {0};
static uint cluster_index[64] = {0};
static LInfo linfo;
static DrawView draw_views[1];
static DrawPush g_draw_push;
static const uint kViewCount = 1u;
static const uint gl_ViewIndex = 0u;
static float g_vis[16];        // the true visibility of light i from the layer (the wall: 0)
static uint g_rays = 0;        // shadow rays the walk cast
static bool g_mask = true;     // the froxel cell is this view's (else the cluster list)

static uint clusterLen(uint v) { return v; }
static uint mainClusterFrag(vec3) { return 0u; }
static bool blendHitMask(vec3, uint* w)
{
    for (uint k = 0; k < 8; ++k) w[k] = 0u;
    if (!g_mask) return false;
    const uint n = uint(linfo.cam_pos.w);
    for (uint i = 0; i < n; ++i) w[i >> 5] |= 1u << (i & 31u);
    return true;
}
static int lightIndexOf(const Light& L)
{
    for (int i = 0; i < 16; ++i)
        if (std::memcmp(&lights[i], &L, sizeof(Light)) == 0) return i;
    return -1;
}
static bool lightInRange(vec4 pr, vec3 P, vec3& lv, float& d2)
{
    lv = pr.xyz() - P;
    d2 = dot(lv, lv);
    return d2 < pr.w * pr.w;
}
static float lightReachAt(vec4 pr, vec4, float d2, float& dist)
{
    dist = std::sqrt(d2);
    return max(1.f - dist / pr.w, 0.f);
}
static void llPunctual(vec3 dc, vec3 sc, float rough, vec3 N, vec3 V, vec3 l, float& nl, vec3& df, vec3& sp)
{
    nl = max(dot(N, l), 0.f);
    const vec3 h = normalize(V + l);
    const float nh = max(dot(N, h), 0.f);
    const float a = max(rough * rough, 0.02f);
    df = dc * (1.f / 3.14159265f);
    sp = sc * (std::pow(nh, 2.f / (a * a)) * (2.f / (a * a) + 2.f) / 6.2831853f);
}
static float legacySpecular(vec3 N, vec3 V, vec3 l, float gloss, float& slit)
{
    slit = 1.f;
    return std::pow(max(dot(N, normalize(V + l)), 0.f), 8.f + 120.f * gloss);
}
static bool lightProjFrustum(mat4, vec3, vec2& ptc) { ptc = vec2(0.5f, 0.5f); return true; }
static vec3 projDiffuse(Light L, float, vec2) { return L.color_falloff.xyz(); }
static vec3 projAmbiance(Light L, float da, float, float, float, vec2) { return L.color_falloff.xyz() * (0.2f * da); }
static vec3 projEnvReflect(Light L, vec3, vec3, vec3, float) { return L.color_falloff.xyz() * 0.3f; }
static bool projAmbShadowed() { return true; }
static bool rtBackfaceSelfShadow() { return false; }
static vec3 rtProjShadowMapQ0(Light L, uint, vec3, vec3) { return vec3(g_vis[lightIndexOf(L)]); }
static vec3 rtrProjRay(Light L, vec3, vec3) { ++g_rays; return vec3(g_vis[lightIndexOf(L)]); }
static float g_bh_glare = 0.f;

// ---- the GI side ----
struct SurfaceLit { uint model; vec3 N, Ng; };
struct SurfaceRefl { vec3 irr, spec, env, prad2; bool have; };
struct GiHeader { uvec4 cfg; vec4 prm; };
struct SsrBuf { vec4 foliage_rt; };
static GiHeader gi;
static SsrBuf ssr;
static vec4 g_gi_fwd_dbg;
static float g_gi_amb_occ = 1.f;
static bool g_rt_fwd_hit = false;
static bool g_gi_fwd_read = false;
static float g_gi_fwd_wf = 0.f;
static vec3 g_gi_fwd_E;
static float g_bh_occ = -1.f;   // (the layer's occlusion ray: lightSurface traces it before the reflections)
static float g_wf = 0.f;        // the radiance cache's cover of the layer (giCacheForward)
static vec3 g_cacheE;
static bool blendHitOn() { return true; }
static uint giCacheSlots() { return gi.cfg.z; }
static vec3 mainReceiverEye(vec3, vec3 cam) { return cam; }
static vec3 giCacheForward(vec3, vec3, vec3, float& w, bool& inSph) { w = g_wf; inSph = false; return g_cacheE; }
static float giLum(vec3 c) { return dot(c, vec3(0.2126f, 0.7152f, 0.0722f)); }
static bool atmosClassic() { return false; }
static vec3 atmosLinearToSrgb(vec3 c) { return c; }
static bool giFwdScreenOcc(vec3, float& a, float& s) { a = s = 1.f; return false; }   // (the borrowing side: never here)

#include BH_FN_FILE

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

static Light pointLight(vec3 p, vec3 c, float r)
{
    Light L{};
    L.pos_radius = vec4(p.x, p.y, p.z, r);
    L.color_falloff = vec4(c.x, c.y, c.z, 1.f);
    L.proj_origin = vec4(p.x, p.y, p.z, 0.f);
    return L;
}
static Light projector(vec3 p, vec3 c, float r, uint rec)
{
    Light L = pointLight(p, c, r);
    L.proj_origin.w = 1.f;
    L.proj_axis = vec4(0.f, -1.f, 0.f, 0.f);
    L.proj_params = vec4(1.f, r, 1.f, 0.5f);
    L.tex.z = rec;
    return L;
}
struct Recv { const char* name; bool pbr; vec3 base; float metal, rough; LegacySpec ls; };
static const Recv kRecv[] = {
    {"PBR glass", true, vec3(0.9f, 0.9f, 0.9f), 0.f, 0.05f, {vec3(0.f), 0.f, 0.f}},
    {"PBR blush on skin", true, vec3(0.8f, 0.4f, 0.4f), 0.f, 0.6f, {vec3(0.f), 0.f, 0.f}},
    {"legacy shiny decal", false, vec3(0.7f, 0.7f, 0.7f), 0.f, 1.f, {vec3(1.f, 1.f, 1.f), 0.9f, 0.6f}},
};
static vec3 shade(const Recv& r)
{
    const vec3 P(0.f, 0.f, 0.f), N(0.f, 1.f, 0.f);
    const vec3 V = normalize(vec3(0.3f, 1.f, 0.2f));
    g_bh_glare = 0.f;
    return rtHitLocalLights(P, N, V, r.pbr, r.base, r.metal, r.rough, r.ls, 0u);
}
static void setFrame(uint f) { draw_views[0].u_cam_time.w = 1000.f + 0.016f * float(f); }
// the next room's lights, all behind the wall
static uint sealedRoom()
{
    std::memset(lights, 0, sizeof(lights));
    lights[0] = pointLight(vec3(2.f, 1.5f, 0.5f), vec3(1.f, 0.5f, 0.6f), 8.f);
    lights[1] = projector(vec3(-2.f, 2.f, 0.3f), vec3(0.6f, 0.6f, 0.3f), 8.f, 0u);
    lights[2] = projector(vec3(0.5f, 2.5f, -2.f), vec3(0.4f, 0.7f, 0.9f), 8.f, 1u);
    lights[3] = projector(vec3(-0.5f, 2.2f, 2.f), vec3(0.8f, 0.8f, 0.8f), 8.f, 2u);
    pshadow[0].params = vec4(0.f, 0.f, 2.f, 1.f);   // (ray traced, valid)
    pshadow[1].params = vec4(0.f, 0.f, 1.f, 1.f);   // (shadow map, valid)
    for (int i = 0; i < 16; ++i) g_vis[i] = 0.f;
    cluster_count[0] = 4u;
    for (uint i = 0; i < 64; ++i) cluster_index[i] = i;
    linfo.cam_pos.w = 4.f;
    return 4u;
}
static void setLights(uint n) { linfo.cam_pos.w = float(n); cluster_count[0] = n; }
static float lum(vec3 c) { return hnLum(c); }

static void testWalk()
{
    const uint budget = BLEND_HIT_M + BLEND_HIT_K;
    for (int m = 0; m < 2; ++m)
    {
        g_mask = m == 0;
        const char* src = g_mask ? "froxel cell" : "cluster list";
        // SEALED
        for (const Recv& r : kRecv)
        {
            sealedRoom();
            g_rays = 0;
            for (uint f = 0; f < 2048; ++f)
            {
                setFrame(f);
                const vec3 e = shade(r);
                CHECK(e.x < 1e-5f && e.y < 1e-5f && e.z < 1e-5f, "SEALED %s (%s), frame %u: lit through the wall: %g %g %g",
                      r.name, src, f, e.x, e.y, e.z);
                if (g_fail) return;
            }
            CHECK(g_rays <= 2048u * budget, "%s: %u rays for 2048 layers (budget %u)", r.name, g_rays, budget);
            CHECK(g_rays > 0u, "%s (%s): no shadow ray at all -- the walk lights without testing", r.name, src);
        }
        // OWN: the room's own lamp visible beside them -> the mean is the own lamp alone
        for (const Recv& r : kRecv)
        {
            const uint n = sealedRoom();
            const Light own = pointLight(vec3(-0.6f, 2.f, -0.4f), vec3(0.3f, 0.28f, 0.25f), 6.f);
            std::memset(lights, 0, sizeof(lights));
            lights[0] = own; g_vis[0] = 1.f; setLights(1);
            setFrame(0);
            const vec3 want = shade(r);
            sealedRoom();
            lights[n] = own; g_vis[n] = 1.f; setLights(n + 1);
            vec3 acc;
            const uint frames = 60000;
            for (uint f = 0; f < frames; ++f) { setFrame(f); acc += shade(r); }
            const vec3 mean = acc * (1.f / float(frames));
            const float tol = 0.04f * max(max(want.x, want.y), want.z) + 1e-4f;
            CHECK(want.x > 1e-4f, "%s: the own lamp lights nothing (fixture sanity)", r.name);
            CHECK(std::fabs(mean.x - want.x) < tol && std::fabs(mean.y - want.y) < tol && std::fabs(mean.z - want.z) < tol,
                  "OWN %s (%s): mean %g %g %g, want the own lamp %g %g %g", r.name, src, mean.x, mean.y, mean.z, want.x, want.y, want.z);
        }
    }
    g_mask = true;
    // EXACT: the key light blocked, a fill light visible -> the fill alone, every frame
    for (const Recv& r : kRecv)
    {
        std::memset(lights, 0, sizeof(lights));
        lights[0] = pointLight(vec3(0.5f, 1.f, 0.2f), vec3(2.f, 1.9f, 1.7f), 6.f);    // key (blocked)
        lights[1] = pointLight(vec3(-0.4f, 1.2f, 0.6f), vec3(0.3f, 0.3f, 0.35f), 6.f);   // fill (visible)
        g_vis[0] = 0.f; g_vis[1] = 1.f;
        setLights(1);
        std::swap(lights[0], lights[1]); std::swap(g_vis[0], g_vis[1]);
        const vec3 want = shade(r);   // (the fill alone)
        std::swap(lights[0], lights[1]); std::swap(g_vis[0], g_vis[1]);
        setLights(2);
        for (uint f = 0; f < 512; ++f)
        {
            setFrame(f);
            const vec3 e = shade(r);
            CHECK(std::fabs(e.x - want.x) < 1e-5f + 1e-4f * want.x && std::fabs(e.z - want.z) < 1e-5f + 1e-4f * want.z,
                  "EXACT %s frame %u: %g, want the fill light %g (the strongest lights must not be sampled)", r.name, f, e.x, want.x);
            if (g_fail) return;
        }
    }
    // NOISE: a lamp + five dim lights, half of them blocked; the error over frames vs plain NEE with the same rays
    {
        const Recv& r = kRecv[0];
        std::memset(lights, 0, sizeof(lights));
        lights[0] = pointLight(vec3(0.3f, 1.4f, 0.1f), vec3(1.5f, 1.4f, 1.2f), 8.f);
        g_vis[0] = 1.f;
        for (int i = 1; i <= 5; ++i)
        {
            lights[i] = pointLight(vec3(-1.f + 0.4f * i, 1.6f, 0.8f - 0.3f * i), vec3(0.18f, 0.17f, 0.16f), 8.f);
            g_vis[i] = float(i & 1);
        }
        // each light's share at the layer, and the exact shadowed sum
        std::vector<vec3> share(6);
        Light all[16];
        std::memcpy(all, lights, sizeof(lights));
        float vsave[16];
        std::memcpy(vsave, g_vis, sizeof(g_vis));
        vec3 exact;
        for (int i = 0; i < 6; ++i)
        {
            std::memset(lights, 0, sizeof(lights));
            lights[0] = all[i]; g_vis[0] = 1.f; setLights(1);
            share[i] = shade(r);
            exact += share[i] * vsave[i];
        }
        std::memcpy(lights, all, sizeof(lights));
        std::memcpy(g_vis, vsave, sizeof(g_vis));
        setLights(6);
        double eWalk = 0.0, eNee = 0.0;
        const uint frames = 20000;
        for (uint f = 0; f < frames; ++f)
        {
            setFrame(f);
            const float a = lum(shade(r)) - lum(exact);
            eWalk += double(a) * a;
            float sc[HN_MAXK]; uint li[HN_MAXK]; vec3 sh[HN_MAXK];
            hnInit(sc, li, sh);
            float W = 0.f;
            for (uint i = 0; i < 6; ++i) W = hnAdd(W, 4u, hnHash(f * 2654435761u + 99u), i, i, share[i], sc, li, sh);
            vec3 s = hnFinish(W, 4u, sc, sh);
            for (uint j = 0; j < 4; ++j) if (sc[j] > 0.f) s -= sh[j] * (1.f - vsave[li[j]]);
            const float b = lum(s) - lum(exact);
            eNee += double(b) * b;
        }
        const double rw = std::sqrt(eWalk / frames), rn = std::sqrt(eNee / frames);
        std::printf("NOISE lamp + 5 dim: RMS error %.4f (%.1f%% of the light), plain NEE with 4 rays %.4f (%.1f%%)\n",
                    rw, 100.0 * rw / lum(exact), rn, 100.0 * rn / lum(exact));
        CHECK(rw < 0.6 * rn, "NOISE: the layer's walk (RMS %g) is not clearly below plain NEE with the same 4 rays (%g)", rw, rn);
    }
    // BUDGET: 16 lights, at most M + K rays per layer
    {
        std::memset(lights, 0, sizeof(lights));
        for (int i = 0; i < 16; ++i)
        {
            lights[i] = (i & 1) ? projector(vec3(-2.f + 0.25f * i, 2.f, 0.5f), vec3(0.2f + 0.03f * i), 7.f, (i % 4 == 1) ? 1u : 0u)
                                : pointLight(vec3(1.5f - 0.2f * i, 1.7f, -0.6f), vec3(0.1f + 0.05f * i), 7.f);
            g_vis[i] = float((i / 2) & 1);
        }
        pshadow[0].params = vec4(0.f, 0.f, 2.f, 1.f);
        setLights(16);
        for (uint f = 0; f < 256; ++f)
        {
            setFrame(f);
            g_rays = 0;
            shade(kRecv[2]);
            CHECK(g_rays <= BLEND_HIT_M + BLEND_HIT_K, "BUDGET: %u shadow rays for one layer (budget %u)", g_rays,
                  BLEND_HIT_M + BLEND_HIT_K);
            if (g_fail) return;
        }
    }
    // GLARE: a legacy shiny layer under a lamp raises its coverage
    {
        std::memset(lights, 0, sizeof(lights));
        lights[0] = pointLight(vec3(-0.6f, 2.f, -0.4f), vec3(1.f), 5.f);   // (along the mirror direction of V)
        g_vis[0] = 1.f;
        setLights(1);
        setFrame(3);
        shade(kRecv[2]);
        CHECK(g_bh_glare > 0.01f, "GLARE: a legacy shiny layer under a lamp has no alpha glare (%g)", g_bh_glare);
    }
}

static SurfaceRefl probesOn()  { SurfaceRefl r{vec3(0.30f, 0.32f, 0.36f), vec3(0.20f, 0.22f, 0.25f), vec3(0.15f), vec3(0.f), true}; return r; }
static SurfaceRefl probesOff() { SurfaceRefl r{vec3(0.45f, 0.50f, 0.60f), vec3(0.80f, 0.85f, 0.95f), vec3(0.70f), vec3(0.f), false}; return r; }
static float vmax(vec3 v) { return max(max(std::fabs(v.x), std::fabs(v.y)), std::fabs(v.z)); }
static bool same(vec3 a, vec3 b) { return vmax(a - b) < 1e-6f; }
// The layer's GI: lightSurface's order -- its occlusion ray (vis), the environment through blendHitEnv, then giForwardRefl
static SurfaceRefl glass(SurfaceRefl in, uint amb, bool cache, float vis)
{
    gi.cfg = uvec4{1u | (cache ? 16u : 0u) | (1u << 8), 1u, cache ? 4096u : 0u, 0u};
    gi.prm = vec4(1.f, 0.f, 8.f, float(amb));
    ssr.foliage_rt = vec4(128.f, 0.f, 0.f, 0.f);   // (a GI frame)
    linfo.cam_pos = vec4(0.f, 0.f, 0.f, 0.f);
    SurfaceLit s; s.model = SURF_PBR; s.N = vec3(-1.f, 0.f, 0.f); s.Ng = s.N;
    g_gi_fwd_read = false;
    g_bh_occ = vis;
    const vec3 P(2.f, 0.f, 0.f), R(-1.f, 0.f, 0.f);
    SurfaceRefl r = in;
    r.spec = blendHitEnv(s, P, R, r.spec);
    r.env = blendHitEnv(s, P, R, r.env);
    return giForwardRefl(r, s, P);
}
static void testGi()
{
    const char* const kProbe[2] = {"probes on", "probes off"};
    g_wf = 0.f;
    for (int p = 0; p < 2; ++p)
        for (uint amb = 0; amb < 4; ++amb)
            for (int cache = 0; cache < 2; ++cache)
            {
                const SurfaceRefl in = p == 0 ? probesOn() : probesOff();
                // SEALED: the occlusion ray blocked, no cache cell
                SurfaceRefl o = glass(in, amb, cache != 0, 0.f);
                CHECK(vmax(o.irr) < 1e-6f && vmax(o.spec) < 1e-6f && vmax(o.env) < 1e-6f,
                      "SEALED %s, hit-ambient %u, cache %d: the glass keeps ambient %g / specular %g / env %g", kProbe[p], amb,
                      cache, o.irr.x, o.spec.x, o.env.x);
                // OUTDOORS: the ray escaped -- the probes' ambient in modes 1 / 2 (none in 0 / 3), the radiance kept
                o = glass(in, amb, cache != 0, 1.f);
                const bool probes = amb == 1u || amb == 2u;
                CHECK(same(o.irr, probes ? in.irr : vec3(0.f)), "OUTDOORS %s, hit-ambient %u: ambient %g, want %g", kProbe[p], amb,
                      o.irr.x, probes ? in.irr.x : 0.f);
                CHECK(same(o.spec, in.spec) && same(o.env, in.env), "OUTDOORS %s, hit-ambient %u: the sky radiance changed %g -> %g",
                      kProbe[p], amb, in.spec.x, o.spec.x);
            }
    // THE CACHE'S OWN COVER: its value, also for a blocked environment (the cache at the layer)
    {
        g_wf = 1.f;
        g_cacheE = vec3(0.05f, 0.04f, 0.03f);
        const SurfaceRefl o = glass(probesOn(), 2u, true, 0.f);
        CHECK(same(o.irr, g_cacheE), "CACHE: ambient %g, want the cell's %g", o.irr.x, g_cacheE.x);
        CHECK(same(o.spec, g_cacheE), "CACHE: a blocked environment %g, want the cell's %g", o.spec.x, g_cacheE.x);
        g_wf = 0.f;
    }
#ifdef BH_RT_FWD
    // RT FORWARD REFLECTION traced at this layer: its specular stays (its rays are occluded hits already)
    {
        g_rt_fwd_hit = true;
        const SurfaceRefl in = probesOn();
        const SurfaceRefl o = glass(in, 2u, true, 0.f);
        CHECK(same(o.spec, in.spec), "RT FWD: the traced specular was occluded again: %g of %g", o.spec.x, in.spec.x);
        g_rt_fwd_hit = false;
    }
#endif
}

int main()
{
    testWalk();
    if (!g_fail) testGi();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("ok\n");
    return 0;
}
