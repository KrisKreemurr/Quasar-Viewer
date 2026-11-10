// Sealed-room fixture for the light at RAY-TRACED HITS, run on the shader's OWN code: scripts/check/checks/
// refl_hit_nee.py preprocesses clean_deferred_light.comp per kernel variant (glslc -E, the variant's own -D
// flags from indra/vkclean/regen_world_spv.py), cuts rtHitLocalLights out of it with every macro expanded
// (the RT reflection hits, the mirrors, the forward layer, the GI hits, the visibility queue's producers), turns
// its GLSL into C++ (literal suffixes, swizzles) and compiles it into this file as RHL_FN_FILE. The shims
// below stand in for the light buffers, the BRDFs and the shadow rays; the walk, its budget and its estimator
// are the kernel's.
//   SEALED   a glossy receiver's reflection hit (PBR mirror-like, PBR metal, legacy shiny) inside a closed room:
//            every light reaching it lies outside -- a point light, a projector without a shadow record, a
//            ray-traced projector record, a shadow-map record -- and every shadow test is blocked -> 0 for every
//            frame seed and K. (Before the fix the reflection kernels lit the hit with the point light and the
//            record-less projector unshadowed: the bathroom's tiles reflected the bedroom lamp.)
//   OWN      the room's own lamp (visible) beside those -> the mean over frames is the own lamp alone.
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
    vec3 xyz() const { return vec3(x, y, z); }
    vec2 xy() const { return vec2(x, y); }
};
static vec3 xyzOf(vec3 v) { return v; }
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
static vec3 mix(vec3 a, vec3 b, float t) { return a * (1.f - t) + b * t; }
static uint floatBitsToUint(float f) { uint u; std::memcpy(&u, &f, 4); return u; }

#include "clean_hit_nee.h"

// ---- the light buffers ----
struct Light { vec4 pos_radius, color_falloff, proj_params, proj_origin, proj_axis; mat4 proj_mat; uvec4 tex; };
struct ProjShadow { vec4 params; };
struct LegacySpec { vec3 spec; float gloss, env; };
struct LInfo { vec4 cam_pos; };
struct DrawView { vec4 u_cam_time; };
struct GiHeader { uvec4 cfg; vec4 prm; };
static Light lights[16];
static ProjShadow pshadow[4];
static uint cluster_count[1] = {0};
static uint cluster_index[64] = {0};
static LInfo linfo;
static DrawView draw_view_main[1];
static GiHeader gi;
static const uint kViewCount = 1u;   // (clean_view.glsl CLEAN_VIEW_Z: one view)
static const uvec4 gl_WorkGroupID = {0u, 0u, 0u, 0u};
static uint g_rtr_stat[16];
static float g_vis[16];        // the true visibility of light i from the hit (the wall: 0)
static uint g_K = 1;           // the kernel's hit shadow-ray budget
static uint g_rays = 0;        // shadow rays the walk cast (inline loop or queued picks)

static uint clusterLen(uint v) { return v; }
static uint rtrOpts() { return g_K << 1; }
static uint rtrLightCluster(vec3) { return 0xFFFFFFFFu; }   // (walk every light)
static uint rtrRegir(vec3, vec3, uint*, float*) { return 0xFFFFFFFFu; }   // (no light grid cell)
static void rtrRegirNote(uint) {}
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
    sp = sc * (std::pow(nh, 2.f / (a * a)) * (2.f / (a * a) + 2.f) / 6.2831853f);   // (a glossy lobe)
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
// (the visibility queue's producers: the picks the REQ stage queues; vqHitLocal subtracts their blocked parts)
static uint g_vq_bn = 0;
static uint g_vq_bl[4];
static vec3 g_vq_bs[4];
static bool g_vq_used = false;
static void vqBudgetOut(uint projK, float* sc, uint* li, vec3* sh)
{
    g_vq_used = true;
    g_vq_bn = 0;
    for (uint j = 0; j < 4; ++j)
    {
        if (g_vq_bn == j && j < projK && sc[j] > 0.f) g_vq_bn = j + 1;
        g_vq_bl[j] = li[j];
        g_vq_bs[j] = sh[j];
    }
}

#include RHL_FN_FILE

// rtHitLocalLights' value as the kernel's lighting stage sees it (the inline rays, or the queue's resolve)
static vec3 shadeHit(bool pbr, vec3 base, float metal, float rough, LegacySpec ls)
{
    const vec3 P(0.f, 0.f, 0.f), N(0.f, 1.f, 0.f);
    const vec3 V = normalize(vec3(0.3f, 1.f, 0.2f));
    g_vq_used = false;
    vec3 s = rtHitLocalLights(P, N, V, pbr, base, metal, rough, ls, 0u);
    if (g_vq_used)
    {
        for (uint j = 0; j < g_vq_bn && j < g_K; ++j)
        {
            ++g_rays;
            s -= g_vq_bs[j] * (1.f - g_vis[g_vq_bl[j]]);
        }
        s = max(s, vec3(0.f));
    }
    return s;
}

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
    L.proj_params = vec4(1.f, r, 1.f, 0.5f);   // (ambiance 0.5)
    L.tex.z = rec;
    return L;
}

struct Recv { const char* name; bool pbr; vec3 base; float metal, rough; LegacySpec ls; };
static const Recv kRecv[] = {
    {"PBR glossy tile", true, vec3(0.8f, 0.8f, 0.8f), 0.f, 0.08f, {vec3(0.f), 0.f, 0.f}},
    {"PBR polished metal", true, vec3(0.9f, 0.7f, 0.5f), 1.f, 0.15f, {vec3(0.f), 0.f, 0.f}},
    {"legacy shiny", false, vec3(0.7f, 0.7f, 0.7f), 0.f, 1.f, {vec3(1.f, 1.f, 1.f), 0.9f, 0.6f}},
};

// the next room's lights, all behind the wall: a point lamp, a record-less projector, a ray-traced projector
// record, a shadow-map record
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
    return 4u;
}
static void setFrame(uint f)
{
    const float t = 1000.f + 0.016f * float(f);
    draw_view_main[0].u_cam_time.w = t;
    gi.cfg.y = f;
}

int main()
{
    gi.cfg = uvec4{0xFFFFFFFFu, 0u, 0u, 0u};   // (every GI flag: the GI variants' point-light budget on)
    for (const Recv& r : kRecv)
        for (g_K = 1; g_K <= 4; ++g_K)
        {
            linfo.cam_pos.w = float(sealedRoom());
            g_rays = 0;
            for (uint f = 0; f < 2048; ++f)
            {
                setFrame(f);
                const vec3 e = shadeHit(r.pbr, r.base, r.metal, r.rough, r.ls);
                CHECK(e.x < 1e-5f && e.y < 1e-5f && e.z < 1e-5f,
                      "SEALED %s, K %u, frame %u: the hit is lit through the wall: %g %g %g", r.name, g_K, f, e.x, e.y, e.z);
                if (g_fail) return 1;
            }
            CHECK(g_rays <= 2048u * g_K, "%s K %u: %u rays for 2048 hits (budget %u per hit)", r.name, g_K, g_rays, g_K);
        }
    // OWN: + the room's own lamp (visible): the mean is the own lamp alone
    for (const Recv& r : kRecv)
        for (g_K = 1; g_K <= 4; g_K += 3)
        {
            uint n = sealedRoom();
            lights[n] = pointLight(vec3(-0.6f, 2.f, -0.4f), vec3(0.3f, 0.28f, 0.25f), 6.f);   // (near the mirror direction)
            g_vis[n] = 1.f;
            // the exact value: the own lamp alone
            Light own = lights[n];
            Light saved[16];
            std::memcpy(saved, lights, sizeof(lights));
            std::memset(lights, 0, sizeof(lights));
            lights[0] = own;
            g_vis[0] = 1.f;
            linfo.cam_pos.w = 1.f;
            setFrame(0);
            const vec3 want = shadeHit(r.pbr, r.base, r.metal, r.rough, r.ls);
            std::memcpy(lights, saved, sizeof(lights));
            for (int i = 0; i < 16; ++i) g_vis[i] = i == int(n) ? 1.f : 0.f;
            linfo.cam_pos.w = float(n + 1);
            vec3 acc;
            const uint frames = 60000;
            for (uint f = 0; f < frames; ++f) { setFrame(f); acc += shadeHit(r.pbr, r.base, r.metal, r.rough, r.ls); }
            const vec3 m = acc * (1.f / float(frames));
            const float tol = 0.04f * max(max(want.x, want.y), want.z) + 1e-4f;
            CHECK(want.x > 1e-4f, "%s: the own lamp lights nothing (fixture sanity)", r.name);
            CHECK(std::fabs(m.x - want.x) < tol && std::fabs(m.y - want.y) < tol && std::fabs(m.z - want.z) < tol,
                  "OWN %s K %u: mean %g %g %g, want the own lamp %g %g %g", r.name, g_K, m.x, m.y, m.z, want.x, want.y, want.z);
        }
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("ok\n");
    return 0;
}
