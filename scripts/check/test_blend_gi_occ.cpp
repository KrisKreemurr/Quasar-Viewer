// Sealed-room fixture for the FORWARD lit surfaces' GI fallback, run on the shader's OWN code:
// scripts/check/checks/blend_gi_occ.py preprocesses the pass-2 forward FSs (clean_draw.frag -DCLEAN_BLEND_TOL
// and its OIT / wire / basic twins, clean_oit_resolve.frag -DOIT_LIT) per variant (glslc -E, the variant's own
// -D flags from indra/vkclean/regen_world_spv.py), cuts giForwardRefl (+ giFwdScreenOcc / giOccUnpack /
// giOccPack and the constants they name) out of it with every macro expanded, turns its GLSL into C++ and
// compiles it into this file as BGO_FN_FILE. The shims stand in for the radiance cache, the occlusion record
// image and the frame state; the fallback rule is the shader's.
//   SEALED    an alpha-blend fragment (glass) in a sealed dark room: the opaque surface behind it took GI
//             occlusion 0 (no sky seen, no bounce light), the radiance cache has no cell at the glass (or is
//             off) -> zero ambient and zero specular, with the probes' values (probes on) or the sky ambient /
//             sky radiance (probes off) as the input, specular occlusion modes 1 and 2. (Before the fix the
//             fallback returned the probes / sky unoccluded: the owner's lit shower glass.)
//   OUTDOORS  the record says open sky (occlusion 1), or there is no record (the sky behind the glass) ->
//             the input light is kept unchanged.
//   plus: a fragment far in front of the record's surface keeps its light (the fade), the cache's own cover
//   (wf 1) is the cache path unchanged, mirror passes / frames without the record ignore it, an RT-traced
//   reflection keeps its specular (its rays are occluded already).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

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
struct vec4
{
    float x, y, z, w;
    vec4(float a = 0.f, float b = 0.f, float c = 0.f, float d = 0.f) : x(a), y(b), z(c), w(d) {}
    vec4(vec3 v, float d) : x(v.x), y(v.y), z(v.z), w(d) {}
    vec3 xyz() const { return vec3(x, y, z); }
    vec2 xy() const { return vec2(x, y); }
};
struct ivec2
{
    int x, y;
    explicit ivec2(int v = 0) : x(v), y(v) {}
    ivec2(int a, int b) : x(a), y(b) {}
    explicit ivec2(vec2 v) : x((int)v.x), y((int)v.y) {}
};
static ivec2 operator-(ivec2 a, int b) { return ivec2(a.x - b, a.y - b); }
struct uvec4 { uint x, y, z, w; };

static float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float max(float a, float b) { return a > b ? a : b; }
static float min(float a, float b) { return a < b ? a : b; }
static int max(int a, int b) { return a > b ? a : b; }
static int min(int a, int b) { return a < b ? a : b; }
static vec3 max(vec3 a, vec3 b) { return vec3(max(a.x, b.x), max(a.y, b.y), max(a.z, b.z)); }
static float clamp(float v, float a, float b) { return min(max(v, a), b); }
static ivec2 clamp(ivec2 v, ivec2 a, ivec2 b) { return ivec2(min(max(v.x, a.x), b.x), min(max(v.y, a.y), b.y)); }
static float mix(float a, float b, float t) { return a * (1.f - t) + b * t; }
static vec3 mix(vec3 a, vec3 b, float t) { return a * (1.f - t) + b * t; }
static float smoothstep(float e0, float e1, float x) { const float t = clamp((x - e0) / (e1 - e0), 0.f, 1.f); return t * t * (3.f - 2.f * t); }
// (IEEE half: round to nearest even, as the GPU's packHalf2x16; positive finite inputs here)
static uint halfBits(float f)
{
    uint u; std::memcpy(&u, &f, 4);
    const uint sign = (u >> 16) & 0x8000u;
    int e = (int)((u >> 23) & 0xFFu) - 127 + 15;
    uint m = u & 0x7FFFFFu;
    if (e <= 0) return sign;   // (no subnormals needed: view depths >= a few mm)
    if (e >= 31) return sign | 0x7C00u;
    uint h = sign | ((uint)e << 10) | (m >> 13);
    const uint rest = m & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (h & 1u))) ++h;
    return h;
}
static float halfFloat(uint h)
{
    const uint e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    if (e == 0) return std::ldexp((float)m, -24);
    if (e == 31) return INFINITY;
    return std::ldexp((float)(m | 0x400u), (int)e - 25);
}
static uint packHalf2x16(vec2 v) { return halfBits(v.x) | (halfBits(v.y) << 16); }
static vec2 unpackHalf2x16(uint u) { return vec2(halfFloat(u & 0xFFFFu), halfFloat(u >> 16)); }

// ---- the frame state the fallback reads ----
struct SurfaceLit { uint model; vec3 N; };
struct SurfaceRefl { vec3 irr, spec, env, prad2; bool have; };
struct GiHeader { uvec4 cfg; vec4 prm; };
struct SsrBuf { vec4 foliage_rt; };
struct LInfo { vec4 cam_pos, cam_fwd; };
struct DrawPush { uint pad0; };
static GiHeader gi;
static SsrBuf ssr;
static LInfo linfo;
static DrawPush g_draw_push;
static vec4 gl_FragCoord(10.5f, 20.5f, 0.5f, 1.f);
static vec4 g_gi_fwd_dbg;
static float g_gi_amb_occ = 1.f;
static bool g_rt_fwd_hit = false;
static bool g_mirror = false;
static float g_wf = 0.f;     // the radiance cache's cover of the fragment (giCacheForward)
static vec3 g_cacheE;        // ... and its irradiance there
static uint g_rec = 0u;      // the occlusion record at the fragment's pixel
struct GiOccImage {};
static GiOccImage uGiOccFwd;
static ivec2 imageSize(GiOccImage) { return ivec2(64, 64); }
static uvec4 imageLoad(GiOccImage, ivec2) { return uvec4{g_rec, 0u, 0u, 0u}; }
static bool mirrorPass() { return g_mirror; }
static bool probeCapture() { return false; }
static uint giCacheSlots() { return gi.cfg.z; }
static vec3 mainReceiverEye(vec3, vec3 cam) { return cam; }
static vec3 giCacheForward(vec3, vec3, vec3, float& w, bool& inSph) { w = g_wf; inSph = false; return g_cacheE; }
static float giLum(vec3 c) { return dot(c, vec3(0.2126f, 0.7152f, 0.0722f)); }
static bool atmosClassic() { return false; }
static vec3 atmosLinearToSrgb(vec3 c) { return c; }
// (giFwdCache's per-fragment memo of the cache read)
static bool g_gi_fwd_read = false;
static float g_gi_fwd_wf = 0.f;
static vec3 g_gi_fwd_E;
#ifdef BGO_BLEND_HIT
// (RenderVKBlendHitLighting off: the borrowing path this fixture guards; test_blend_hit.cpp runs the other side)
static bool blendHitOn() { return false; }
static float g_bh_occ = -1.f;
#endif

#include BGO_FN_FILE

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

// THE OCCLUSION RECORD's word (clean_gi_common.glsl): half(view depth) | amb unorm8 << 16 | spec unorm8 << 24
static uint record(float vz, float amb, float spec)
{
    return (halfBits(vz) & 0xFFFFu) | ((uint)(amb * 255.f + 0.5f) << 16) | ((uint)(spec * 255.f + 0.5f) << 24);
}
static float vmax(vec3 v) { return max(max(std::fabs(v.x), std::fabs(v.y)), std::fabs(v.z)); }
static bool same(vec3 a, vec3 b) { return vmax(a - b) < 1e-6f; }

// The camera at the origin looking down +X; the glass fragment 2 m out, the opaque surface behind it
// `gap` metres further (the record's view depth).
static const float kGlassZ = 2.f;
static SurfaceRefl lightGlass(SurfaceRefl in, uint specOcc, bool cache, float gap, float amb, float spec, bool rec = true)
{
    gi.cfg = uvec4{1u | (cache ? 16u : 0u) | (specOcc << 8), 1u, cache ? 4096u : 0u, 0u};
    gi.prm = vec4(1.f, 0.f, 8.f, 2.f);   // (intensity 1, minimum ambient 0)
    ssr.foliage_rt = vec4(128.f, 0.f, 0.f, 0.f);   // (a GI frame)
    linfo.cam_pos = vec4(0.f, 0.f, 0.f, 0.f);
    linfo.cam_fwd = vec4(1.f, 0.f, 0.f, 0.f);
    g_rec = rec ? record(kGlassZ + gap, amb, spec) : 0u;
    SurfaceLit s; s.model = SURF_PBR; s.N = vec3(-1.f, 0.f, 0.f);
    g_gi_fwd_read = false;   // (a new fragment)
    return giForwardRefl(in, s, vec3(kGlassZ, 0.f, 0.f));
}
static SurfaceRefl probesOn()  { SurfaceRefl r{vec3(0.30f, 0.32f, 0.36f), vec3(0.20f, 0.22f, 0.25f), vec3(0.15f), vec3(0.f), true}; return r; }
static SurfaceRefl probesOff() { SurfaceRefl r{vec3(0.45f, 0.50f, 0.60f), vec3(0.80f, 0.85f, 0.95f), vec3(0.70f), vec3(0.f), false}; return r; }

int main()
{
    g_draw_push.pad0 = 16u;   // (VISR_GI_OCC: this frame's record)
    const char* const kProbe[2] = {"probes on", "probes off"};
    // SEALED: the record 0 / 0 (a sealed room, no light inside), no cache cell -- or the cache off
    for (int p = 0; p < 2; ++p)
        for (uint so = 1u; so <= 2u; ++so)
            for (int cache = 0; cache < 2; ++cache)
            {
                g_wf = 0.f;
                const SurfaceRefl in = p == 0 ? probesOn() : probesOff();
                const SurfaceRefl o = lightGlass(in, so, cache != 0, 0.8f, 0.f, 0.f);
                CHECK(vmax(o.irr) < 1e-6f, "SEALED %s, spec occlusion %u, cache %s: the glass keeps ambient %g %g %g",
                      kProbe[p], so, cache ? "on (no cell)" : "off", o.irr.x, o.irr.y, o.irr.z);
                CHECK(vmax(o.spec) < 1e-6f && vmax(o.env) < 1e-6f,
                      "SEALED %s, spec occlusion %u, cache %s: the glass keeps specular %g %g %g / env %g",
                      kProbe[p], so, cache ? "on (no cell)" : "off", o.spec.x, o.spec.y, o.spec.z, o.env.x);
            }
    // OUTDOORS: the record says open sky -- or there is none (the sky behind the glass): kept as it was
    for (int p = 0; p < 2; ++p)
        for (int norec = 0; norec < 2; ++norec)
        {
            g_wf = 0.f;
            const SurfaceRefl in = p == 0 ? probesOn() : probesOff();
            const SurfaceRefl o = lightGlass(in, 1u, true, 0.8f, 1.f, 1.f, norec == 0);
            CHECK(same(o.irr, in.irr) && same(o.spec, in.spec) && same(o.env, in.env),
                  "OUTDOORS %s (%s): the sky light changed: irr %g -> %g, spec %g -> %g", kProbe[p],
                  norec ? "no record: sky behind" : "record 1", in.irr.x, o.irr.x, in.spec.x, o.spec.x);
        }
    // FAR IN FRONT of the record's surface (past the fade): the record is not the glass's environment
    {
        g_wf = 0.f;
        const SurfaceRefl in = probesOn();
        const SurfaceRefl o = lightGlass(in, 1u, true, 10.f, 0.f, 0.f);
        CHECK(same(o.irr, in.irr) && same(o.spec, in.spec), "FAR: a pane 10 m in front of the room took its occlusion");
        const SurfaceRefl m = lightGlass(in, 1u, true, 2.75f, 0.f, 0.f);   // (mid-fade: partly)
        CHECK(m.irr.x > 1e-4f && m.irr.x < in.irr.x - 1e-4f, "FADE: a pane 2.75 m in front: irr %g of %g", m.irr.x, in.irr.x);
    }
    // THE CACHE'S OWN COVER (wf 1): the cache path as it was, the record ignored
    {
        g_wf = 1.f;
        g_cacheE = vec3(0.05f, 0.04f, 0.03f);
        const SurfaceRefl in = probesOn();
        const SurfaceRefl o = lightGlass(in, 1u, true, 0.8f, 0.f, 0.f);
        const float occ = clamp(giLum(g_cacheE) / giLum(in.irr), 0.f, 1.f);
        CHECK(same(o.irr, g_cacheE), "CACHE: irr %g, want the cell's %g", o.irr.x, g_cacheE.x);
        CHECK(same(o.spec, in.spec * occ), "CACHE: spec %g, want %g (GI / probe ratio)", o.spec.x, in.spec.x * occ);
        g_wf = 0.f;
    }
    // A MIRROR PASS, or a frame without the record (pad0 clear): the fallback as it was
    {
        const SurfaceRefl in = probesOn();
        g_mirror = true;
        SurfaceRefl o = lightGlass(in, 1u, true, 0.8f, 0.f, 0.f);
        CHECK(same(o.irr, in.irr), "MIRROR: the mirror pass read the main view's record");
        g_mirror = false;
        g_draw_push.pad0 = 0u;
        o = lightGlass(in, 1u, true, 0.8f, 0.f, 0.f);
        CHECK(same(o.irr, in.irr), "NO RECORD THIS FRAME: pad0 clear, the record was read");
        g_draw_push.pad0 = 16u;
    }
#ifdef BGO_RT_FWD
    // RT FORWARD REFLECTION traced at this fragment: its specular stays (occluded rays), its ambient goes
    {
        g_rt_fwd_hit = true;
        const SurfaceRefl in = probesOn();
        const SurfaceRefl o = lightGlass(in, 1u, true, 0.8f, 0.f, 0.f);
        CHECK(vmax(o.irr) < 1e-6f, "RT FWD: the ambient stayed %g", o.irr.x);
        CHECK(same(o.spec, in.spec), "RT FWD: the traced specular was occluded again: %g of %g", o.spec.x, in.spec.x);
        g_rt_fwd_hit = false;
    }
#endif
#ifdef BGO_HAS_PACK
    // the producer's packing (giOccPack, what BASE / COMBINE store) is the word this fixture builds
    {
        const float zs[3] = {0.37f, 2.8f, 117.f};
        for (float z : zs)
            for (int k = 0; k <= 4; ++k)
            {
                const float a = 0.25f * (float)k, sp = 1.f - a;
                CHECK(giOccPack(z, a, sp) == record(z, a, sp), "PACK: giOccPack(%g, %g, %g) = %08x, fixture %08x", z, a, sp,
                      giOccPack(z, a, sp), record(z, a, sp));
            }
    }
#endif
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("ok\n");
    return 0;
}
