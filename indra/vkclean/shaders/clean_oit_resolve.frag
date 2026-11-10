#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require
#extension GL_KHR_shader_subgroup_ballot : require   // the covered-pixel counter (one atomic per subgroup)
#ifdef OIT_LIT
#extension GL_EXT_nonuniform_qualifier : require
#define CLEAN_SKY_LUT   // (#819) skyColor() reads the sky-view LUT (draw set 0 binding 52) when valid
// FORWARD FROXEL WALK (clean_draw_common.glsl localLights), as the forward lit FS.
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_KHR_shader_subgroup_arithmetic : require
#define CLEAN_FWD_FROXEL
#ifdef CLEAN_PROJ_SHADOW_RAYQUERY
#extension GL_EXT_ray_query : require   // the ray-query twin: one sun ray per covered pixel (oitSunVis)
#endif
#endif
// vkclean -- WBOIT RESOLVE (#1410, RenderVKAlphaOIT; llcleanalphaoitvk.cpp, clean_oit.glsl).
//
// A fullscreen triangle inside the pass-2 rendering scope, at the place of the OIT group in the
// blend order (where the ordered rigged alpha list drew). Reads the accumulation targets of this
// water side and composites the resolved transparent surface over the scene:
//     colour' = value * (1 - revealage) + colour * revealage      (blend ONE / SRC_ALPHA)
// the glow likewise, and the coverage 1 - revealage into the TAA reactive mask (MAX, R).
// Pixels the OIT group does not cover (revealage 1) leave at once.
//
// Default build (mode 1): the accumulated value is the per-fragment LIT colour -- the composite only.
// -DOIT_LIT (mode 2): the accumulated value is the weighted mean SURFACE (clean_draw.frag
// -DCLEAN_OIT_GBUF): albedo, normal, emission, material, view depth. It is lit HERE, once per
// pixel, by the same lightSurface() as the forward lit FS and the deferred lighting
// (clean_surface_light.glsl): sun + shadows, clustered local lights, probes / GI, haze, then the
// forward alpha tail (surfacePost, volumetric fog, precipitation veil). Position: the pixel's ray
// at the weighted mean view depth. Lit as an alpha-blend surface (v_flags 2: no SSAO, the
// transparent reflection rules). Sun shadow: a fixed per-pixel lookup (oitSunVis) -- in traced
// frames one opaque-only ray per covered pixel (the ray-query twin; the plain one: the RT shadow
// mask where a tap lies on the resolved surface), else the split lighting's visibility
// at the pixel; otherwise the cascades -- see oitSunVis: the ray-query twin traces one ray per
// covered pixel instead. Projectors: the forward alpha FS's default (RT-traced ones unshadowed,
// shadow-map ones from the atlas); on the visibility reuse's strict match (a layer lying on the opaque
// surface) that surface's projector slots and, in radiance mode, its lamp record (visReuseMatch in
// main). RT set: 3 (the ray-query twin).
//
// Set 2 (LLCleanAlphaOITVK): 0 the accumulation (mode 1: rgb w a colour, a w a; mode 2: rgb w a
// albedo, a w a), 1 mode 1: w a' glow / mode 2: w a normal + w a view depth, 2 mode 2: w a
// emission + w a glow, 3 mode 2: w a (rough | gloss, metal | specular lum, ao | env, PBR share),
// 4 the revealage (R16F), 5 the covered-pixel counter.

layout(set = 2, binding = 0) uniform texture2D uOitAccum;
layout(set = 2, binding = 1) uniform texture2D uOit1;
layout(set = 2, binding = 2) uniform texture2D uOit2;
layout(set = 2, binding = 3) uniform texture2D uOit3;
layout(set = 2, binding = 4) uniform texture2D uOitReveal;
layout(std430, set = 2, binding = 5) buffer OitStats { uint oit_px; };

#ifdef OIT_LIT
#define CLEAN_DEBUG_CONST
const bool kDebugViews = false;   // (no debug twin: debug features take the ordered path)
#define CLEAN_SPEC_SETTINGS       // graphics-setting specialization constants 8-17 (the live key's)
#define CLEAN_COMPUTE_LIGHT       // no vertex inputs (clean_draw_common.glsl) ...
#define CLEAN_COMPUTE_V_FLAGS 2u  // ... and v_flags = alpha-blend: the forward alpha FS's lighting rules
                                  // (no SSAO, transparent reflections, RT projectors unshadowed)
// The forward lit FS's split-lighting visibility images (draw set 0 bindings 41-43): the sun
// visibility the split lighting resolved for the opaque surface at this pixel (oitSunVis).
#define CLEAN_VIS_REUSE
// (RenderVKBlendHitLighting) the ray-query twin lights the resolved surface at its own point, as the static blend
// FS lights a layer (clean_draw_common.glsl blendHitOn): its own local-light rays, sun ray, GI and environment
// occlusion -- not the opaque surface's lamp record, GI record, RT-mask sun or AO (g_oit_near off).
#if defined(OIT_LIT) && defined(CLEAN_PROJ_SHADOW_RAYQUERY)
#define CLEAN_BLEND_HIT
#endif
// SUN VISIBILITY (clean_surface_light.glsl hook): oitSunVis, evaluated at g_oit_P / g_oit_Ng.
#define CLEAN_SUN_VIS_EXTERNAL oitSunVis
#include "clean_draw_common.glsl"
#define CLEAN_GI_FORWARD          // (#87) GI for the forward lit surfaces, as the forward lit FS
#define GI_STATE_SET 0
#define GI_STATE_BINDING 53
#define GI_STATE_ACCESS readonly
#define GI_STATE_READONLY
#include "clean_gi_common.glsl"

// The scene's (opaque) depth: the near-surface rule and the depth debug view (draw set 0 binding 40,
// as the blend FS's tolerance test; reversed-Z, 0 = sky).
layout(set = 0, binding = 40) uniform sampler2DArray uOitSceneDepth;
vec3 g_oit_P = vec3(0.0);
vec3 g_oit_Ng = vec3(0.0, 0.0, 1.0);
// NEAR-SURFACE RULE: the resolved depth lies within OIT_NEAR_GAP in front of the opaque surface at
// the pixel (hair on the scalp, a fringe on its own deferred-lit core -- #1420: the scene depth holds
// the near-opaque texels the G-buffer took): the resolved surface takes THAT surface's deferred
// terms -- its SSAO, and in traced frames its RT mask visibility (no ray) -- so the fringe matches
// what it lies on. Farther in front (hair against a wall metres behind): the forward set and the
// per-pixel sun ray.
const float OIT_NEAR_GAP = 0.05;
bool g_oit_near = false;
#define CLEAN_SURFACE_AO_OVERRIDE g_oit_near
// The resolved surface's sun / moon visibility (1 = lit) and where it came from (the debug view):
const uint OIT_SUN_CASCADE = 0u, OIT_SUN_RAY = 1u, OIT_SUN_MASK = 2u, OIT_SUN_VISR = 3u, OIT_SUN_NONE = 4u;
float g_oit_sun = 1.0;
uint  g_oit_sun_src = OIT_SUN_CASCADE;
// RAY-TRACED SUN: the ray-query twin (-DCLEAN_PROJ_SHADOW_RAYQUERY, bound in every tracing frame)
// traces ONE opaque-only ray from the resolved position per covered pixel (sunShadowT with
// v_flags 2: the forward alpha FS's receiver rules, one ray, no terminator triangle) -- what the
// ordered pass traced per fragment, once per pixel. (#1410 mode-2 artefact: the plain variant read
// the RT shadow MASK at the pixel. The mask holds the visibility of the OPAQUE surface behind the
// hair; when no tap lies within its depth tolerance, rtMaskTaps takes the CLOSEST tap and the
// thin-receiver fallback the BRIGHTEST behind it -- the background's sunlight, so hair over a sunlit
// wall or the sky came out fully lit, "seeing the lit background through it".) The plain variant
// (a traced frame without the ray-query set: rare) takes the mask only where a tap lies on or
// within the thin-receiver window behind the resolved position, else the split lighting's
// visibility when that surface matches, else unshadowed (flagged in the debug view).
float oitSunVis(out vec3 tint)
{
    tint = vec3(1.0);
#ifndef CLEAN_PROJ_SHADOW_RAYQUERY
    if (specSunOn() && specSunTraced() && !litDbg(LDBG_NO_SUN_SHADOW))
    {
        const float vz = dot(g_oit_P - linfo.cam_pos.xyz, linfo.cam_fwd.xyz);
        float vis = 1.0;
        g_oit_sun_src = OIT_SUN_NONE;
        if (vz >= ssr.shadow_clip.w) g_oit_sun_src = OIT_SUN_CASCADE;   // (past the shadowed range)
        else
        {
            bool near = false;   // a mask tap on (or just behind) the resolved surface
            if (ssr.rt_mask.x > 0.5)
            {
                const RtMaskTaps t = rtMaskTaps(g_oit_P);
                const float tol = max(vz * 0.015, rtMaskPlaneTolFloor());
                [[unroll]] for (int k = 0; k < 4; ++k)
                {
                    const float d = rtMaskDepthAt(t.px[k]).x;
                    near = near || (d > 0.0 && d - vz > -tol && d - vz < RT_MASK_THIN_BEHIND_E * tol);
                }
            }
            if (near)
            {
                const vec3 m = rtMaskSun(g_oit_P);   // (the mask pass applied the far fade)
                vis = max(max(m.r, m.g), m.b);
                if (vis > 1e-4) tint = m / vis;
                g_oit_sun_src = OIT_SUN_MASK;
            }
            else if ((g_draw_push.pad0 & VISR_ON) != 0u)
            {
                const uint d = imageLoad(uVisReuseDepth, ivec2(CLEAN_FRAG_XY)).x;
                const float vzS = unpackHalf2x16(d & 0xFFFFu).x;
                if (vzS > 0.0 && abs(vzS - vz) < 0.05 + 0.02 * vz)   // (that surface IS the resolved one)
                {
                    const vec4 v = imageLoad(uVisReuseSun, ivec2(CLEAN_FRAG_XY));
                    tint = v.rgb * v.rgb;
                    vis = v.a * v.a;
                    g_oit_sun_src = OIT_SUN_VISR;
                }
            }
        }
        g_oit_sun = vis;
        return vis * cloudShadowMap(g_oit_P);
    }
#else
    if (g_oit_near && ssr.rt_mask.x > 0.5 && specSunOn() && specSunTraced() && !litDbg(LDBG_NO_SUN_SHADOW))
    {   // (the near-surface rule: the mask traced for the surface the hair lies on)
        const vec3 m = rtMaskSun(g_oit_P);
        const float vis = max(max(m.r, m.g), m.b);
        if (vis > 1e-4) tint = m / vis;
        g_oit_sun = vis;
        g_oit_sun_src = OIT_SUN_MASK;
        return vis * cloudShadowMap(g_oit_P);
    }
    g_oit_sun_src = specSunTraced() ? OIT_SUN_RAY : OIT_SUN_CASCADE;
#endif
    const float v = sunShadowT(g_oit_P, g_oit_Ng, tint);
    g_oit_sun = v;
    return v;
}

#include "clean_surface_light.glsl"
#include "clean_vol_apply.glsl"     // volumetric fog consumer (#718)
#include "clean_weather_veil.glsl"  // precipitation veil consumer (#954)
// RESOLVE-INPUT DEBUG VIEWS (RenderVKAlphaOITDebug bits 2-5 -> the resolve's pad1; mode 2 only):
// opaque over every covered pixel, the lowest set bit wins.
//   4  sun visibility used (grey level) by source: blue ray-traced here, yellow the RT mask,
//      cyan the split lighting's visibility, green cascades, RED unshadowed (no source matched)
//   8  coverage 1 - revealage (grey); red where the accumulated w a is faint (< 0.02: fringes)
//   16 resolved depth vs the opaque depth behind it: green within 5 cm in front (the near-surface
//      rule: that surface's AO + RT mask), yellow 5-50 cm,
//      orange 0.5-5 m, red > 5 m, blue = sky behind, MAGENTA = resolved BEHIND the opaque surface
//   32 resolved normal (N * 0.5 + 0.5); red where the layers' normals cancelled (|mean N| < 0.3)
const uint OIT_DBG_SUN = 4u, OIT_DBG_COV = 8u, OIT_DBG_DEPTH = 16u, OIT_DBG_NORMAL = 32u;
#endif   // OIT_LIT

layout(location = 0) out vec4 outOitColor;
layout(location = 1) out vec4 outOitGlow;
layout(location = 2) out vec4 outOitReact;

#ifdef OIT_LIT
// World position on this pixel's ray at view depth zv (the main view's inverse view-projection,
// world space as the water FS's scenePos; reversed-Z: a small depth is a point ~100 near planes out).
vec3 oitRayPoint(vec2 fragXY, vec2 res, float zv)
{
    const vec2 uv = fragXY / res;
    const vec4 h = pc.u_inv_view_proj * vec4(uv * 2.0 - 1.0, 0.01, 1.0);
    const vec3 cam = pc.u_cam_time.xyz;
    vec3 d = abs(h.w) > 1e-12 ? h.xyz / h.w - cam : linfo.cam_fwd.xyz;
    d = dot(d, d) > 1e-20 ? normalize(d) : linfo.cam_fwd.xyz;
    if (linfo.cam_fwd.w > 0.5) d = linfo.cam_fwd.xyz;   // (orthographic: parallel rays)
    return cam + d * (zv / max(dot(d, linfo.cam_fwd.xyz), 1e-4));
}
#endif

void main()
{
    const ivec2 px = ivec2(gl_FragCoord.xy);
    const float rev = clamp(texelFetch(uOitReveal, px, 0).r, 0.0, 1.0);
    const vec4 acc = texelFetch(uOitAccum, px, 0);
    const bool covered = rev < 0.998 && acc.a > 1e-8;
    {   // the covered-pixel counter (RenderVKAlphaOIT summary line)
        const uvec4 b = subgroupBallot(covered && !gl_HelperInvocation);
        if (subgroupElect())
        {
            const uint n = subgroupBallotBitCount(b);
            if (n != 0u) atomicAdd(oit_px, n);
        }
    }
    if (!covered) discard;
    const float cov = 1.0 - rev;
    const float iw = 1.0 / acc.a;
#ifndef OIT_LIT
    // Mode 1: the weighted mean of the lit fragments' colour and glow.
    const vec3 c = acc.rgb * iw;
    const vec3 g = texelFetch(uOit1, px, 0).rgb * iw;
    outOitColor = vec4(c * cov, rev);
    outOitGlow  = vec4(g * cov, rev);
#else
    // Mode 2: the weighted mean surface, lit once.
    const vec4 nz = texelFetch(uOit1, px, 0);
    const vec4 em = texelFetch(uOit2, px, 0);
    const vec4 mt = texelFetch(uOit3, px, 0) * iw;
    const vec3 P = oitRayPoint(gl_FragCoord.xy, vec2(textureSize(uOitAccum, 0)), nz.w * iw);
    const vec3 V = mainViewDirection(P, pc.u_cam_time.xyz);
    vec3 N = nz.xyz * iw;
    const float nl = length(N);
    N = nl > 1e-3 ? N / nl : V;   // (opposing layers cancelled: face the camera)
    SurfaceLit s = surfaceInit();
    s.model    = mt.w >= 0.5 ? SURF_PBR : SURF_LEGACY;
    s.albedo   = max(acc.rgb * iw, vec3(0.0));
    s.N        = N;
    s.Ng       = N;
    s.emissive = max(em.rgb * iw, vec3(0.0));
    if (s.model == SURF_PBR)
    {
        s.rough = clamp(mt.x, 0.0, 1.0); s.metal = clamp(mt.y, 0.0, 1.0); s.ao = clamp(mt.z, 0.0, 1.0);
    }
    else
    {
        s.gloss = clamp(mt.x, 0.0, 1.0); s.spec = vec3(clamp(mt.y, 0.0, 1.0)); s.env = clamp(mt.z, 0.0, 1.0);
    }
    g_oit_P = P;
    g_oit_Ng = N;
    // The split lighting's reuse on the strict match, as the static alpha FS (clean_draw_common.glsl
    // visReuseMatch): a resolved layer lying on the skin -- a blush, makeup -- takes the skin's
    // projector slots and, in radiance mode, its lamp record (VISR_RAD: the lamps the deferred walk
    // owns are skipped here, the skin's shadowed lamp light is added). The sun stays oitSunVis's.
    visReuseMatch(P, N);
    // The opaque surface behind: its view depth (reversed-Z, 0 = sky) -> the near-surface rule.
    const float zr = nz.w * iw;
    float gap = 1e30;
    {
        const float zo = texelFetch(uOitSceneDepth, ivec3(px, 0), 0).r;
        if (zo > 0.0)
        {
            const vec2 uv = (vec2(px) + 0.5) / vec2(textureSize(uOitAccum, 0));
            const vec4 h = pc.u_inv_view_proj * vec4(uv * 2.0 - 1.0, zo, 1.0);
            gap = dot(h.xyz / h.w - linfo.cam_pos.xyz, linfo.cam_fwd.xyz) - zr;
        }
    }
    g_oit_near = gap > -0.01 && gap < OIT_NEAR_GAP;
#ifdef CLEAN_BLEND_HIT
    if (blendHitOn()) g_oit_near = false;   // (its own sun ray and no borrowed AO)
#endif
    vec3 c = lightSurface(s, P, 1.0).rgb;
    // The forward alpha FS's tail (clean_draw.frag main): the post, the froxel fog, the veil.
    g_medium_inline = true;
    c = surfacePost(SURF_LEGACY, false, c, P);
    if (!cmOn() && volOn() && litDebugView() == 0) c = volFogApply(c, P);
    if (litDebugView() == 0) c = weatherVeilApply(c, P);
    c = lgFinite(c, LG_OUT);
    const float glow = em.a * iw;
    outOitColor = vec4(c * cov, rev);
    outOitGlow  = vec4(glow > 0.0 ? atmosLinearToSrgb(c) * (glow * cov) : vec3(0.0), rev);
    const uint dbg = g_draw_push.pad1 & (OIT_DBG_SUN | OIT_DBG_COV | OIT_DBG_DEPTH | OIT_DBG_NORMAL);
    if (dbg != 0u)
    {
        vec3 d = vec3(0.0);
        if ((dbg & OIT_DBG_SUN) != 0u)
        {
            const vec3 kSrc[5] = vec3[5](vec3(0.2, 1.0, 0.2), vec3(0.3, 0.5, 1.0), vec3(1.0, 0.9, 0.1),
                                         vec3(0.1, 1.0, 1.0), vec3(1.0, 0.0, 0.0));
            d = kSrc[min(g_oit_sun_src, 4u)] * (0.15 + 0.85 * clamp(g_oit_sun, 0.0, 1.0));
        }
        else if ((dbg & OIT_DBG_COV) != 0u)
            d = acc.a < 0.02 ? vec3(1.0, 0.1, 0.1) * (0.3 + 0.7 * cov) : vec3(cov);
        else if ((dbg & OIT_DBG_DEPTH) != 0u)
            d = gap > 1e29 ? vec3(0.1, 0.3, 1.0) : gap < -0.01 ? vec3(1.0, 0.0, 1.0) : gap < OIT_NEAR_GAP ? vec3(0.1, 1.0, 0.1)
              : gap < 0.5 ? vec3(1.0, 1.0, 0.1) : gap < 5.0 ? vec3(1.0, 0.5, 0.0) : vec3(1.0, 0.0, 0.0);
        else d = nl < 0.3 ? vec3(1.0, 0.0, 0.0) : N * 0.5 + 0.5;
        outOitColor = vec4(d * atmosHdrScale(), 0.0);
        outOitGlow  = vec4(0.0, 0.0, 0.0, 0.0);
    }
#endif
    outOitReact = vec4(cov, 0.0, 0.0, 0.0);
}
