// vkclean — WATER INTERACTION (#1019): ripples, shoreline / contact foam, surf lines and crest
// foam for the water surface FS. Shared include: clean_water.frag uses it today, and the
// waves surface (#1020) samples the same hooks. Include it AFTER clean_draw_common.glsl
// (ssr = clean_frame_info.glsl's block, pc = the draw push).
//
// INPUTS
//  * ssr.water_fx[2] (clean_frame_info.glsl; LLCleanWaterFxVK::fillFrameInfo). All zero = off:
//    every hook below then returns "nothing" after one uniform test.
//  * THE SEA STATE (#1020, clean_sea_state.glsl / slseastate.h; ssr.sea): the wind, Beaufort,
//    storm and the swash clock (kept on flat water too), and the waves' masks via the adapter.
//  * set 2 bindings 3 / 4 of the water layout (LLCleanRendererVK mDslWaterDepth; PARTIALLY_BOUND,
//    so they are read only while the flags say the module is live):
//      3  uWaterRipple   RGBA32F, 2 layers, REPEAT / linear: the ripple height field. Texel
//                        (h_new, h_cur, dh/dx, dh/dy) of world cell floor(xy / texel) mod N --
//                        WORLD-ANCHORED TOROIDAL addressing, so uv = xy / extent; only the
//                        window around the camera (water_fx[1]) holds this frame's cells.
//      4  uWaterFoamTex  RG8 + mips, REPEAT: R the foam's lacy cell pattern, G a soft
//                        tileable noise (low-frequency breakup).
//    The WATER_OPAQUE build (probe / mirror captures: no set 2) gets the hooks as no-ops.
//
// HOOKS (stable names):
//    N = waterFxRippleNormal(N, P);         // THE one-line ripple hook: N any unnormalized
//                                           // world normal of the surface at world P
//    vec4 r = waterFxRipple(P.xy);          // raw: (dh/dx, dh/dy, height m, window fade)
//    float f = waterFxFoam(P, depth, ray, r, duvdx, duvdy);   // foam coverage 0..1
//
// WAVES ADAPTER (#1020; required). The includer defines, BEFORE including this file:
//    #define WATER_WAVES_CREST     + float waterFxCrestMask(vec3 P, float depth)
//        0..1: how much this surface point is a breaking crest / whitecap (open water);
//    #define WATER_WAVES_BREAKING  + float waterFxBreakMask(vec3 P, float depth)
//        0..1: the breaking surf near the shore (shoaling crests, the bore rushing in, the swash).
// clean_water.frag implements them from the waves' seaSurface masks (g_sea.crest / .breaking /
// .swash). P is the DISPLACED surface position and depth the water column under it (surface z
// minus the scene's z behind it, m). The band at the contact line (shores, rocks, legs) stays
// this file's -- it comes from the depth, which the displaced surface already moves up and down
// the beach (the swash).
#ifndef CLEAN_WATER_FX_GLSL
#define CLEAN_WATER_FX_GLSL
#include "clean_sea_state.glsl"

#ifndef WATER_OPAQUE
#define WATER_FX_MAPS 1
layout(set = 2, binding = 3) uniform sampler2DArray uWaterRipple;
layout(set = 2, binding = 4) uniform sampler2D uWaterFoamTex;
#endif

const uint WFX_RIPPLE = 1u, WFX_FOAM = 2u, WFX_SURF = 4u, WFX_DBG_RIPPLE = 8u, WFX_DBG_FOAM = 16u;
uint  waterFxFlags()     { return uint(ssr.water_fx[0].x + 0.5); }
bool  waterFxRippleOn()  { return (waterFxFlags() & WFX_RIPPLE) != 0u; }
bool  waterFxFoamOn()    { return (waterFxFlags() & WFX_FOAM) != 0u; }
bool  waterFxSurfOn()    { return (waterFxFlags() & WFX_SURF) != 0u; }

// ---- SEA STATE (clean_sea_state.glsl: the one sea state, #1020) ----
// 0 calm .. 1 storm: the one "how bad is it" number for the band's width.
float wfxRoughness()   { return clamp(max(seaBeaufort() * 0.1, seaStorm()), 0.0, 1.0); }

// ---- RIPPLES ----
// (dh/dx, dh/dy, height, fade) at world xy; zero outside the camera window / when off.
vec4 waterFxRipple(vec2 xy)
{
#ifdef WATER_FX_MAPS
    if (!waterFxRippleOn()) return vec4(0.0);
    const vec2 d = abs(xy - ssr.water_fx[1].yz);
    const float half_ext = ssr.water_fx[1].w;
    const float fade = 1.0 - smoothstep(half_ext * 0.72, half_ext * 0.94, max(d.x, d.y));
    if (fade <= 0.0) return vec4(0.0);
    const vec4 r = textureLod(uWaterRipple, vec3(xy * ssr.water_fx[1].x, ssr.water_fx[0].w), 0.0);
    return vec4(r.zw, r.x, 1.0) * fade;
#else
    return vec4(0.0);
#endif
}
vec3 waterFxRippleNormal(vec3 n, vec4 rip)
{
    return n + vec3(-rip.xy * ssr.water_fx[0].z, 0.0);
}
vec3 waterFxRippleNormal(vec3 n, vec3 P) { return waterFxRippleNormal(n, waterFxRipple(P.xy)); }

#ifdef WATER_FX_MAPS
// The soft noise (G) at world xy * scale, no derivatives (smooth at every scale it is used at).
float wfxNoise(vec2 uv) { return textureLod(uWaterFoamTex, uv, 0.0).g; }
#endif

#if !defined(WATER_WAVES_CREST) || !defined(WATER_WAVES_BREAKING)
#error "clean_water_fx.glsl: define the waves' masks (WATER_WAVES_CREST / WATER_WAVES_BREAKING) first"
#endif

// ---- FOAM ----
// Coverage 0..1 at surface point P (world).
//   depthV    the water column under P (m: surface z - the scene's z behind, >= 0)
//   depthRay  how far the view ray runs on under the surface to the scene (m): small at
//             contacts seen edge-on (legs, piles, hulls) where depthV is not
//   rip       waterFxRipple(P.xy) (churned water around wakes / splash rings)
//   duvdx/dy  dFdx / dFdy of P.xy, taken in uniform control flow by the caller
// The band: an exponential falloff from the contact line whose width breathes with the swash
// (a fast rush in, a slow back-wash, per-place phase), wider in rough seas (0.25 m calm ..
// 2.2 m storm), plus a thin lapping line that never leaves; then the waves' breakers / swash,
// their whitecaps and the ripple churn. The coverage thresholds the scrolling foam pattern, so edges break up
// into lace instead of a hard ring; past ~1 texel per pixel the pattern blends to its mean
// (no sparkle at a distance, no TAA fight).
float waterFxFoam(vec3 P, float depthV, float depthRay, vec4 rip, vec2 duvdx, vec2 duvdy)
{
#ifdef WATER_FX_MAPS
    if (!waterFxFoamOn()) return 0.0;
    const float rough = wfxRoughness();
    const float lf = wfxNoise(P.xy * 0.021);
    const float d = max(min(depthV, depthRay), 0.0);

    const float W = mix(0.25, 2.2, rough);
    // (the swash clock: one surge per breaker at the shore, the waves' own swashPeriod)
    const float x0 = fract(seaSwashPhase() + lf * 1.7);
    const float surge = x0 < 0.22 ? smoothstep(0.0, 0.22, x0) : 1.0 - smoothstep(0.22, 1.0, x0);
    const float We = W * (0.45 + 0.85 * surge);
    float cov = exp(-d / max(We * 0.45, 0.03)) * (0.75 + 0.5 * lf);
    cov = max(cov, exp(-d * 16.0));                       // the lapping line
    if (waterFxSurfOn())
    {
        cov = max(cov, waterFxBreakMask(P, d));
        cov = max(cov, waterFxCrestMask(P, d));
    }
    cov = max(cov, clamp(length(rip.xy) * 1.6 - 0.1, 0.0, 0.8) * rip.w);   // churn
    cov *= ssr.water_fx[0].y;

    // Breakup: two scrolls of the lacy pattern (downwind and across), the soft noise mixed in
    // (the LOW tier -- no surf bit -- one scroll).
    const vec2 w = seaWindDir();   // (the waves' travel = where the wind blows to)
    const float t = pc.u_cam_time.w;
    const vec2 uv1 = P.xy * 0.33 + w * (t * 0.035);
    const vec2 uv2 = P.xy * 0.61 + vec2(0.37, 0.71) + vec2(w.y, -w.x) * (t * 0.021);
    const float p1 = textureGrad(uWaterFoamTex, uv1, duvdx * 0.33, duvdy * 0.33).r;
    const float p = waterFxSurfOn() ? p1 * 0.5 + textureGrad(uWaterFoamTex, uv2, duvdx * 0.61, duvdy * 0.61).r * 0.3 + lf * 0.2
                                    : p1 * 0.8 + lf * 0.2;
    // pattern texels per pixel (256-texel tile)
    const float fw = max(length(duvdx), length(duvdy)) * 0.61 * 256.0;
    const float c = clamp(cov, 0.0, 1.25);
    const float lace = smoothstep(1.0 - c, 1.0 - c + 0.16, p);
    return clamp(mix(lace, min(c, 1.0) * 0.8, clamp(fw - 1.0, 0.0, 1.0)), 0.0, 1.0);
#else
    return 0.0;
#endif
}

#endif
