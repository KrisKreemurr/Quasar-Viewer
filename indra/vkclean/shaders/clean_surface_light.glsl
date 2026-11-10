// vkclean — SURFACE LIGHTING shared by the forward lit FS (clean_draw.frag) and the
// deferred lighting compute (clean_deferred_light.comp).
//
// clean_draw.frag decodes a fragment's material into a SurfaceLit (decodeSurface), then
// either lights it here inline (forward: alpha, probe / mirror captures, fallbacks) or
// packs it into the G-buffer (clean_gbuffer.glsl); the compute unpacks the same struct
// and calls the same lightSurface(). One lighting code path, two back-ends, so forward
// and deferred stay bit-for-bit comparable (up to the G-buffer quantization).
//
// Needs clean_draw_common.glsl (lights, probes, shadows, SSR, atmosphere, CLEAN_FRAG_XY,
// v_flags) included first. CLEAN_SURFACE_DEBUG keeps the RenderVKDebugView branches (the lit
// FS, and the deferred uber kernel for its kDebugViews twin; the views that read vertex inputs
// or the material are the G-buffer overlay's there: dbgViewElsewhere).

#ifndef CLEAN_SURFACE_LIGHT_GLSL
#define CLEAN_SURFACE_LIGHT_GLSL

#include "clean_surface_lit.glsl"   // SURF_* / SurfaceLit / surfaceInit (#970: shared with the RT hit weather)

// BUMP SHADOW TERMINATOR (Chiang, Li & Burley 2019, "Taming the Shadow Terminator";
// RenderVKSunTerminatorSoften, frame info lit_dbg.w bit 4, default on). The sun's
// shadow -- ray-traced (whose origin follows the smooth interpolated surface, Hanika
// lift included: clean_shadow_rt.glsl) or the cascades -- knows the SMOOTH surface Ng
// (the interpolated vertex normal), not the normal map. Where the normal map tilts N
// toward the sun past Ng, N.L is still well above zero where Ng.L reaches zero and the
// surface self-shadows: the light ends in a hard cut along the shadow line instead of
// falling off. The microfacet-derived shadowing term fades the sun over that band:
//   G = min(1, Ng.L / (N.L * Ng.N)),  G' = -G^3 + G^2 + G   (C1 at G = 1)
// It is exactly 1 wherever N == Ng (no normal map) or the bump leans away from the sun,
// so unbumped surfaces and the lit side keep their look. Applied to the sun visibility
// (scol), so every sun form -- the PBR product, LL's min(da, scol), cel -- sees it.
// The facet-vs-vertex-normal (low-poly) terminator is NOT this term's job: with the
// triangle plane as Ng it would cut every facet facing away from the sun to black --
// the very faceting the Hanika lift removes from the shadow rays.
float sunTerminatorG(vec3 N, vec3 Ng, vec3 L)
{
    if ((uint(ssr.lit_dbg.w + 0.5) & 16u) == 0u) return 1.0;
    const float nl  = dot(N, L);
    const float ngn = dot(Ng, N);
    if (nl <= 1e-4 || ngn <= 1e-4) return 1.0;   // (N.L gates the sun anyway; a flipped bump: no frame)
    const float g = clamp(dot(Ng, L) / (nl * ngn), 0.0, 1.0);
    return ((-g + 1.0) * g + 1.0) * g;           // -g^3 + g^2 + g
}


// Ambient occlusion target (RenderDeferredSSAO: clean_ao.comp; later ray-traced AO),
// main view only. Folded into ambient/irradiance: standard multiply
// (RenderVKAOStandard), or LL softenLightF's adjustIrradiance:
// mix(ssao_effect_mat * min(irr * scale, max), irr, ao).
// [0] the AO (r); [1] the half-resolution upsample's per-texel surface (rg32f: x the plane
// offset dot(Ng, P - eye) of the texel's representative pixel, y its world Ng as 12 + 12-bit
// oct in an exact integer, -1 = sky; clean_ao.comp aoMeta) -- a 1 x 1 placeholder unless
// ssr.ao_up.x == 2.
layout(set = 0, binding = 21) uniform sampler2D uAO[2];

bool aoActive()
{
#ifdef CLEAN_SURFACE_AO_OVERRIDE
    // (#1410 the WBOIT resolve: a resolved surface lying on the opaque one at the pixel takes its AO)
    if (CLEAN_SURFACE_AO_OVERRIDE) return specAOOn() && !litDbg(LDBG_NO_AO);
#endif
    return specAOOn() && !probeCapture() && !mirrorPass() && (v_flags & (2u | 8u)) == 0u
        && !litDbg(LDBG_NO_AO);
}
vec3 aoOctDecode(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    const float t = max(-n.z, 0.0);
    n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}
// THE AO read of every consumer (PBR + legacy ambient, specular occlusion, debug view 16;
// forward FS, uber kernel and split combine alike). P: the pixel's world position, Ng its
// oriented smooth geometric normal.
// HALF RESOLUTION (RenderVKAOHalfRes; clean_ao.comp): AO texel q describes ONE pixel of the
// 2 x 2 block at 2q (its representative: the closest / farthest depth on a checkerboard),
// so the taps are the 4 blocks around this pixel, bilinear by block centre. Depth-aware
// (RenderVKAOUpsample 1): each tap also weighted by how far this P is from the tap
// surface's PLANE (exact on any plane at any viewing angle -- grazing ground keeps all 4
// taps), within an absolute tolerance (RenderVKAOBlurTol, as the AO blur: shell-grass layers
// a few cm apart stay apart) + 0.05 % of the view depth + a 1-degree slope over the taps'
// along-surface distance; x a mild normal similarity. No tap on this surface -> the closest
// one (clean_upsample_common.glsl, the RT mask's rule).
float aoSample(vec3 P, vec3 Ng)
{
    if (!aoActive()) return 1.0;
    const uint mode = kSpecAOUp >= 0 ? uint(kSpecAOUp) : uint(ssr.ao_up.x + 0.5);   // (#818: constant 19)
    if (mode == 0u) return texelFetch(uAO[0], ivec2(CLEAN_FRAG_XY), 0).r;
    const ivec2 hsz = textureSize(uAO[0], 0);
    const ivec2 hmax = hsz - 1;
    const vec2 f = CLEAN_FRAG_XY * 0.5 - 0.5;   // block q's centre is render px 2q + 1
    const ivec2 b = ivec2(floor(f));
    const vec2 fr = f - vec2(b);
    vec4 bw, a;
    ivec2 tp[4];
    [[unroll]] for (int k = 0; k < 4; ++k)
    {
        const ivec2 o = ivec2(k & 1, k >> 1);
        tp[k] = clamp(b + o, ivec2(0), hmax);
        bw[k] = (o.x == 1 ? fr.x : 1.0 - fr.x) * (o.y == 1 ? fr.y : 1.0 - fr.y);
        a[k] = texelFetch(uAO[0], tp[k], 0).r;
    }
    if (mode == 1u) return dot(bw, a);   // plain bilinear (manual: the target may lack linear filtering)
    const vec3 rel = P - linfo.cam_pos.xyz;
    const float vz = max(dot(rel, linfo.cam_fwd.xyz), 1e-3);
    // Render-pixel footprint at this depth (grid.w = tan(half fov y)); a tap's representative
    // is <= ~2 px from this pixel on screen, more along a surface seen at a grazing angle.
    const float foot = CL_VIEW_Z(linfo, vz) * 2.0 * linfo.grid.w / float(2 * hsz.y);
    const float cosv = max(abs(dot(Ng, normalize(rel))), 0.05);
    const float tol = max(ssr.ao_up.y, 1e-3) + 5e-4 * vz + 0.0175 * 2.0 * foot / cosv;
    vec4 e, nw;
    [[unroll]] for (int k = 0; k < 4; ++k)
    {
        const vec2 m = texelFetch(uAO[1], tp[k], 0).xy;
        if (m.y < 0.0) { e[k] = 1e30; nw[k] = 0.0; continue; }   // sky representative
        const uint qn = uint(m.y);
        const vec3 n = aoOctDecode(vec2(float(qn >> 12), float(qn & 4095u)) * (2.0 / 4095.0) - 1.0);
        e[k] = abs(dot(n, rel) - m.x) / tol;
        const float nd = max(dot(n, Ng), 0.0);
        nw[k] = 0.25 + 0.75 * nd * nd;
    }
    return dot(upsampleWeights4(bw, e, nw), a);
}
vec3 adjustIrradiance(vec3 irr, float ambocc)
{
    if (ambocc >= 1.0) return irr;
    if (specAOStandard()) return irr * ambocc;   // standard (RenderVKAOStandard)
    vec3 c = min(irr * ssr.ao_params.x, vec3(ssr.ao_params.y));
    // ssao_effect_mat: diag on the diagonal, nondiag elsewhere (symmetric)
    c = (ssr.ao_params.z - ssr.ao_params.w) * c + ssr.ao_params.w * (c.r + c.g + c.b);
    return mix(c, irr, ambocc);
}
// Specular occlusion (RenderVKSpecularOcclusion; beyond LL, which occludes diffuse
// ambient only): Lagarde & de Rousiers 2014 (Frostbite) from the AO term, NdotV and
// linear roughness, applied to the environment reflections.
float specularOcclusion(float NdotV, float ambocc, float perceptualRough)
{
    if (ambocc >= 1.0 || !specSpecOcc()) return 1.0;
    float a = perceptualRough * perceptualRough;
    return clamp(pow(NdotV + ambocc, exp2(-16.0 * a - 1.0)) - 1.0 + ambocc, 0.0, 1.0);
}

// Whether the sun / moon visibility is evaluated for a surface (sunShadowT): every use is
// gated by N.L > 0 with the final (normal-mapped) N -- PBR on llPunctual's nl = clamp(N.L,
// 0.001, 1) > 0, legacy on N.L > 0; fullbright is unlit. Shared by lightSurface and the
// split deferred lighting's shadow pass (clean_deferred_light.comp), which must evaluate
// the visibility for exactly the pixels the combine reads it on.
bool surfaceSunWant(SurfaceLit s, vec3 L)
{
    return s.model != SURF_FULLBRIGHT && (s.model == SURF_PBR ? clamp(dot(s.N, L), 0.001, 1.0) > 0.0
                                                              : dot(s.N, L) > 0.0);
}

// ---- REFLECTIONS of one surface: the probe walk + SSR + planar mirror + legacy env ----
// Everything lightSurface's material branches read from the environment, gathered in one
// place so the split deferred lighting can run it as its own pass (clean_deferred_light.comp
// CLEAN_DEFER_PASS 2: no probe blend / SSR march in the combine kernel) while the forward FS
// and the uber kernel call it inline -- one source. A pure function of the surface, P, V and
// amblit: the values are those the branches computed in place before (same calls, same
// order of operations per value).
struct SurfaceRefl {
    vec3 irr;     // PBR / legacy ambient irradiance before AO: the probe blend, amblit where no
                  // probe covers P (legacy classic mode keeps the sky ambient)
    vec3 spec;    // PBR: specular radiance (probes, sky along R uncovered) + SSR + mirror;
                  // legacy: the glossy env (glossenv, + SSR + mirror, clamped; 0 without gloss);
                  // fullbright: the shiny env along reflect(-V, Ng) + mirror (0 without shine)
    vec3 env;     // legacy: the sharp lod-0 env (legacyenv, + SSR + mirror)
    vec3 prad2;   // debug view 9 only: the walk's radiance 2 (lod 0 along R)
    bool have;    // debug view 9 only: a probe covered P
};
// (#87) GI's AMBIENT OCCLUSION of this pixel for the terms that take the SKY AMBIENT (amblit)
// directly instead of refl.irr: cel shading's flatten reference -- the level its flattened ambient
// and its RIM LIGHT are pulled toward (celFlatten, cel_rim x irradiance). 1 = open sky (no GI, GI
// without an estimate here, RenderVKGICelOcclusion 0). Set by the GI readers: deferReflIn
// (clean_deferred_light.comp: the denoised sky visibility) and giForwardRefl (GI / probe ratio).
float g_gi_amb_occ = 1.0;
// (#87, GI_F_HAZE_OCC) the haze IN-SCATTER's scale at this pixel: 1 = as LL (no GI here); set by
// deferReflIn from the GI sky visibility. Only the additive (sky light) term -- the extinction stays,
// so far things still fade; surfaceLocalGain's slope (the attenuation) is unchanged.
float g_haze_sky_k = 1.0;
#ifdef CLEAN_GI_FORWARD
// (#1438) RenderVKGIDebug 6 (GI_DBG_CACHE) on a forward surface: a > 0 replaces its lit colour (opaque) --
// green = the cache's cover of P (wf), red = no cell (the probes), MAGENTA = a cell it read lies in a change
// sphere this frame (before #1438 that read was rejected: the probes, a flash).
vec4 g_gi_fwd_dbg = vec4(0.0);
#ifdef CLEAN_VIS_REUSE
// THE OPAQUE PIXEL'S OCCLUSION (the main view's pass-2 forward FSs: the alpha-blend lit FS, the WBOIT
// accumulation / resolve -- CLEAN_VIS_REUSE): where the radiance cache has no cell at a forward fragment
// (glass standing free in a room: no opaque surface near it), its ambient and specular fell back to the
// probes / the sky UNOCCLUDED -- in a sealed dark bathroom the shower glass stayed lit while every opaque
// surface went black, brighter still with probes on. The fragment takes the occlusion GI applied to the
// opaque surface behind it at the same pixel (clean_gi_common.glsl THE OCCLUSION RECORD, written by BASE /
// COMBINE: its own upsample, latency and DLSS rules, the same value), one load. The push's pad0 says the
// record is this frame's (VISR_GI_OCC: a split GI frame, mono). NO record (the sky behind the fragment, a
// fullbright surface, no GI estimate there): 1, outdoors. FAR IN FRONT of that surface: its environment
// is not the fragment's (an outside window over a room metres deep), so the record fades out between
// GI_FWD_OCC_GAP0 and GI_FWD_OCC_GAP1 metres of view depth in front of it (a shower door ~1 m off the
// tiles, seen at an angle, takes it in full). Mirror / probe capture passes: their own pixel grid -- none.
layout(set = 0, binding = 68, r32ui) uniform readonly uimage2D uGiOccFwd;
const uint VISR_GI_OCC = 16u;
const float GI_FWD_OCC_GAP0 = 2.0, GI_FWD_OCC_GAP1 = 4.0;
bool giFwdScreenOcc(vec3 P, out float amb, out float spec)
{
    amb = 1.0;
    spec = 1.0;
    if ((g_draw_push.pad0 & VISR_GI_OCC) == 0u || mirrorPass() || probeCapture()) return false;
    const ivec2 px = clamp(ivec2(CLEAN_FRAG_XY), ivec2(0), imageSize(uGiOccFwd) - 1);
    float vzO, a, sp;
    if (!giOccUnpack(imageLoad(uGiOccFwd, px).x, vzO, a, sp)) return false;
    const float vz = dot(P - linfo.cam_pos.xyz, linfo.cam_fwd.xyz);
    const float w = 1.0 - smoothstep(GI_FWD_OCC_GAP0, GI_FWD_OCC_GAP1, vzO - vz);
    if (w <= 0.0) return false;
    amb = mix(1.0, a, w);
    spec = mix(1.0, sp, w);
    return true;
}
#endif
// The radiance cache at a forward fragment (giCacheForward: the cells around P, wf = their found weight; the debug
// view's colour). One statement body: giForwardRefl expands it in place (the forward and the VR deferred kernels'
// text as it was); a blend layer's own light reads it ONCE per fragment through giFwdCache, which its environment
// occlusion (blendHitEnv) and giForwardRefl share.
#define GI_FWD_CACHE_READ(s, P, wf, E) \
    if ((gi.cfg.x & GI_F_CACHE) != 0u && giCacheSlots() != 0u) \
    { \
        const vec3 N = dot(s.N, mainReceiverEye(P, linfo.cam_pos.xyz) - P) < 0.0 ? -s.N : s.N; \
        bool inSph; \
        E = giCacheForward(P, N, linfo.cam_pos.xyz, wf, inSph); \
        if (gi.cfg.w == GI_DBG_CACHE) \
            g_gi_fwd_dbg = vec4(inSph ? vec3(1.0, 0.0, 1.0) : vec3(wf > 0.0 ? 0.0 : 1.0, wf, 0.0), 1.0); \
    }
#ifdef CLEAN_BLEND_HIT
bool  g_gi_fwd_read = false;
float g_gi_fwd_wf = 0.0;
vec3  g_gi_fwd_E = vec3(0.0);
void giFwdCache(SurfaceLit s, vec3 P)
{
    if (g_gi_fwd_read) return;
    g_gi_fwd_read = true;
    GI_FWD_CACHE_READ(s, P, g_gi_fwd_wf, g_gi_fwd_E)
}
// (giForwardRefl's own gate: a GI frame, a lit model)
bool giFwdOn(SurfaceLit s)
{
    return giFrameOn() && s.model != SURF_FULLBRIGHT && (gi.cfg.x & GI_F_ON) != 0u;
}
// (RenderVKBlendHitLighting) A BLEND LAYER'S OWN OCCLUSION on a GI frame, as a reflection hit's (clean_deferred_light
// .comp rtrHitEnv): ONE ray along its reflection direction R (rtOccRaySetup, clean_rt_light_ray.glsl: off the layer's
// own plane on the viewer's side, every world blocker by its transmittance, to RTR_OCC_FAR) -- its visibility of
// the far field the probes / the sky hold. g_bh_occ: that visibility, -1 = not traced yet. Traced once per layer
// (blendHitOcc: lightSurface, ONE call site -- every inlined ray walk costs the FS registers and size), read by the
// environment (blendHitEnv) and by the GI fallback's ambient (giForwardRefl).
// Cost: one first-hit ray per shaded layer on GI frames.
float g_bh_occ = -1.0;
float blendHitOcc(SurfaceLit s, vec3 P)
{
    if (g_bh_occ < 0.0)
    {
        const vec3 R = reflect(-mainViewDirection(P, pc.u_cam_time.xyz), s.N);   // (surfaceReflectionsW's R)
        vec3 o, Ro;
        bool bent;
        rtOccRaySetup(P, R, s.Ng, o, Ro, bent);
        rt_block_t = 1e30;
        g_bh_occ = clamp(traceShadowRay(o, Ro, 0.0, RTR_OCC_FAR, RT_MASK_WORLD_BLOCK), 0.0, 1.0);
    }
    return g_bh_occ;
}
// ... THE ENVIRONMENT: the ray mixes ESCAPED -- the probe / sky radiance, which hold the far field -- and BLOCKED --
// R sees another surface whose light the probes do not know: the radiance cache at the layer (its share wf of the
// cells around P; none: 0). The environment term only, before SSR / the planar mirror (they see the real scene), and
// not where the RT forward reflection traced this layer (its rays are occluded hits already).
vec3 blendHitEnv(SurfaceLit s, vec3 P, vec3 R, vec3 rad)
{
#ifdef CLEAN_RT_FWD_AVAILABLE
    if (g_rt_fwd_hit) return rad;
#endif
    if (!blendHitOn() || !giFwdOn(s)) return rad;
    const float vis = max(g_bh_occ, 0.0);   // (traced by lightSurface before the reflections)
    giFwdCache(s, P);
    const vec3 E = max(g_gi_fwd_E, vec3(0.0)) * max(gi.prm.x, 0.0);
    const vec3 bv = (atmosClassic() ? atmosLinearToSrgb(E) : E) * g_gi_fwd_wf;
    return mix(bv, rad, vis);
}
#define CLEAN_REFL_ENV_FWD(s, P, R, rad) blendHitEnv(s, P, R, rad)
#endif
// (#87) A FORWARD surface on a GI frame: the radiance cache's irradiance at P (this frame's resolve,
// GI_F_LAG last frame's; giCacheForward, the cells around P) replaces the probes' diffuse (x intensity,
// floored by the minimum ambient) and the probe / SSR / env specular is occluded by GI / probe irradiance
// (the deferred kernels use the sky visibility; a forward fragment has no GI pixel of its own). The share
// of the cells around P that have none -- all of it with no cell yet, or the cache off -- takes the
// probes x the opaque pixel's occlusion (giFwdScreenOcc), so the cache's cover ends without a seam; without
// a record there the probes stand as they are. A fragment the RT forward reflections traced keeps its
// traced specular from that share (its rays are occluded already, as the deferred RT pixels').
SurfaceRefl giForwardRefl(SurfaceRefl r, SurfaceLit s, vec3 P)
{
    if (!giFrameOn() || s.model == SURF_FULLBRIGHT || (gi.cfg.x & GI_F_ON) == 0u) return r;
    float wf = 0.0;
    vec3 E = vec3(0.0);
#ifdef CLEAN_BLEND_HIT
    giFwdCache(s, P);   // (the read blendHitEnv took)
    wf = g_gi_fwd_wf;
    E = g_gi_fwd_E;
#else
    GI_FWD_CACHE_READ(s, P, wf, E)
#endif
    float occA = 1.0, occS = 1.0;
    bool scr = false;
#ifdef CLEAN_BLEND_HIT
    // (RenderVKBlendHitLighting) a blend layer's own light: the share of the cells around P without an estimate
    // follows the GI hits' ambient mode (RenderVKGIHitAmbient, rtrHitEnv) -- the probes (1 probes, 2 the cache
    // else the probes) or nothing (0 none, 3 the cache else nothing) -- x the layer's OWN visibility of the far
    // field the probes hold (blendHitOcc: its one occlusion ray; a sealed room's glass sees walls only: 0), never
    // the opaque pixel's occlusion record. Its specular took that ray in blendHitEnv (the environment share only).
    const bool bh = blendHitOn();
    if (bh)
    {
        const uint amb = uint(gi.prm.w + 0.5);
        occA = (amb == GI_HIT_AMB_PROBES || amb == GI_HIT_AMB_CACHE) ? max(g_bh_occ, 0.0) : 0.0;
        occS = occA;
        scr = true;
    }
#ifdef CLEAN_VIS_REUSE
    else if (wf < 1.0) scr = giFwdScreenOcc(P, occA, occS);
#endif
#else
#ifdef CLEAN_VIS_REUSE
    if (wf < 1.0) scr = giFwdScreenOcc(P, occA, occS);
#endif
#endif
#ifdef CLEAN_RT_FWD_AVAILABLE
    if (g_rt_fwd_hit) occS = 1.0;
#endif
    if (wf <= 0.0 && !scr) return r;
    E = max(E, vec3(0.0)) * max(gi.prm.x, 0.0);
    const vec3 probeIrr = r.irr;
    const vec3 Eg = atmosClassic() ? atmosLinearToSrgb(E) : E;
    const vec3 Ec = mix(probeIrr * occA, Eg, wf);
    const float floorA = clamp(gi.prm.y, 0.0, 1.0);
    r.irr = max(Ec, probeIrr * floorA);
    const float pl = giLum(probeIrr);
    if ((gi.cfg.x & GI_F_CEL_OCC) != 0u)
        g_gi_amb_occ = max(pl > 1e-4 ? clamp(giLum(Ec) / pl, 0.0, 1.0) : mix(occA, 1.0, wf), floorA);
#ifdef CLEAN_BLEND_HIT
    if (bh) return r;   // (the environment took its own occlusion ray: blendHitEnv)
#endif
    if (((gi.cfg.x >> GI_SPECOCC_SHIFT) & 3u) != 0u)
    {
        const float occ = max(pl > 1e-4 ? clamp(giLum(mix(probeIrr * occS, Eg, wf)) / pl, 0.0, 1.0)
                                        : mix(occS, 1.0, wf), floorA);
        r.spec *= occ;
        r.env *= occ;
    }
    return r;
}
#endif
#ifdef CLEAN_REFL_EXTERNAL
// (the split combine's reader; the includer defines it after this file)
SurfaceRefl CLEAN_REFL_EXTERNAL(SurfaceLit s);
#endif

// WANT MASK (surfaceReflectionsW): which of irr / spec / env the caller keeps. A term it
// drops skips its probe fetches (and the SSR march when neither radiance is kept); the
// terms it keeps are bit-identical to the full call (the walk's weights don't depend on
// what is fetched). surfaceReflections = all three. The split lighting's half-resolution
// reflections (clean_deferred_light.comp) evaluate irradiance + rough specular per 2 x 2
// block and only the sharp specular / legacy env per pixel.
const uint SR_IRR  = 1u;
const uint SR_SPEC = 2u;
const uint SR_ENV  = 4u;
const uint SR_ALL  = 7u;

// ---- (#872) PBR ENVIRONMENT SPECULAR, MATCHED TO ITS DIFFUSE AMBIENT ----
// A PBR surface's ambient is two terms that must come from the SAME light: the diffuse lights
// with the irradiance (refl.irr), the specular with the probes' prefiltered radiance. They did
// not wherever the irradiance is NOT the probes': the sky probe ambiance < 1 (every legacy
// sky: 0) pulls the irradiance to amblit (probeAccum), and with no probe (probes off, probe
// captures) it IS amblit -- while the specular still read the full captured environment, sky
// included, at the sky's own HDR level (skyResolve: srgb_to_linear of up to 5 -> ~43 x the
// sky HDR scale, against an amblit near 0.1-0.4). A dark dielectric's ambient is then ~all
// specular (EnvBRDF ~1.6-4 % of that sky): black PBR clothing showed navy, bright skies washed
// PBR out to grey (probes on) or streaked it (probes off) -- legacy matte faces have no
// environment specular at all, so they kept their colour.
// The standard remedy where reflections and diffuse ambient come from different sources
// (reflection-capture normalization against the pixel's diffuse ambient): scale the
// environment radiance by (the diffuse ambient used) / (the irradiance of the environment the
// radiance came from), faded in by roughness -- near-mirror surfaces keep the reflected image
// as is (SSR covers them; a mirror shows the sky's real brightness), rough ones get the
// ambient's level. The ratio is a scalar, so the environment's hue stays (a wood room still
// reflects brown) and it only DIMS (capped at 1): nothing gets brighter than before. Where the
// probes light the diffuse fully (ambiance >= 1) the ratio is 1: unchanged.
// Off (A/B): RenderVKLitDebugMask 8192 (LDBG_NO_IBL_NORM). Debug view 29 shows the weight.
float g_ibl_norm_w = 1.0;            // the weight applied to this surface (debug view 29)
const float IBL_NORM_ROUGH0 = 0.1;   // roughness where the normalization starts
const float IBL_NORM_ROUGH1 = 0.3;   // ... and is complete
float iblLum(vec3 c) { return dot(max(c, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722)); }
// The diffuse ambient lightSurface's PBR branch lights diffuseColor with, linear (before AO):
// classic skies take the irradiance as sRGB (x 0.9), linear ones as is.
vec3 pbrAmbientLinear(vec3 irr) { return atmosClassic() ? atmosSrgbToLinear(irr * 0.9) : irr; }
// The weight for a surface of roughness `rough`: irrUsed = the irradiance its diffuse takes,
// irrEnv = the irradiance of the environment its specular reads (same space as irrUsed).
float iblNormWeight(float rough, vec3 irrUsed, vec3 irrEnv)
{
    const float m = smoothstep(IBL_NORM_ROUGH0, IBL_NORM_ROUGH1, rough);
    if (m <= 0.0) return 1.0;
    const float le = iblLum(pbrAmbientLinear(irrEnv));
    if (!(le > 1e-5)) return 1.0;
    const float k = clamp(iblLum(pbrAmbientLinear(irrUsed)) / le, 0.0, 1.0);
    return mix(1.0, k, m);
}
// The sky along a direction for the reflection fallbacks (probe captures: from their origin).
vec3 materialFallbackSky(vec3 direction,vec3 origin)
{
    return probeCapture() ? skyColorAtOrigin(direction,origin) : skyColor(direction);
}
// The PBR specular radiance where no probe covers P (probes off / not ready, probe captures).
// It was skyColor(R): the sky along the mirror direction whatever the roughness, so a rough
// surface reflected the sharp sun glow (#1051's view-dependent streaks with probes off).
// Now: the sky prefiltered over the GGX lobe (dominant direction, Lagarde & de Rousiers 2014;
// cone ~ alpha), normalized to the diffuse ambient (amblit) as the probe path.
// THE SKY CONE (the lobe's cone, and the normalization's 60-degree cone around N): the sky averaged
// over a cone around d (half-angle th, radians): the centre + 4 taps on the ring, weighted 1 : cos(th)
// -- a 5-tap stand-in for a lobe prefilter (the sky-view LUT has no mips). th = 60 degrees ~ the
// cosine-weighted mean (the sky's irradiance / pi around d).
// (perf-4k60) ONE materialFallbackSky call site, in loops (the cones, then the ring's taps), not ten
// inlined copies: skyColor is the WindLight haze model / the physical sky when the sky-view LUT does
// not answer, and ten copies of it in every lit kernel made the compiler spill across the whole kernel
// whether or not a pixel ever reached this fallback (REFLECT HALF 1216 B -> 384 B, REFLECT SHARP ->
// 288 B of local memory; on the vqbench 4K scene REFLECT HALF 2.9 -> 1.6 ms, SHARP 2.7 -> 0.8 ms).
// The same taps summed in the same order.
vec3 iblSkyRadiance(vec3 N, vec3 R, float rough, vec3 amblit,vec3 origin)
{
    if (litDbg(LDBG_NO_IBL_NORM)) { g_ibl_norm_w = 1.0; return materialFallbackSky(R,origin); }
    const float a = rough * rough;
    const vec3 md = mix(N, R, (1.0 - a) * (sqrt(1.0 - a) + a));
    const vec3 Rd = dot(md, md) > 1e-8 ? normalize(md) : N;
    const bool norm = rough > IBL_NORM_ROUGH0;
    vec3 rad = vec3(0.0), env = vec3(0.0);
    for (uint pass = 0u; pass < (norm ? 2u : 1u); ++pass)
    {
        const vec3 d = pass == 0u ? Rd : N;
        const float th = pass == 0u ? min(a, 1.0) : 1.0472;
        const vec3 c = materialFallbackSky(d,origin);
        vec3 res = c;
        if (!(th < 0.02))
        {
            const vec3 t = abs(d.z) < 0.999 ? normalize(cross(vec3(0.0, 0.0, 1.0), d)) : vec3(1.0, 0.0, 0.0);
            const vec3 b = cross(d, t);
            const float ct = cos(th), st = sin(th);
            vec3 ring = vec3(0.0);
            for (uint k = 0u; k < 4u; ++k)   // (+t, -t, +b, -b: the old sum's order)
            {
                const vec3 o = (k < 2u ? t : b) * st;
                ring += materialFallbackSky((k & 1u) == 0u ? d * ct + o : d * ct - o, origin);
            }
            res = (c + ring * ct) / (1.0 + 4.0 * ct);
        }
        if (pass == 0u) rad = res; else env = res;
    }
    g_ibl_norm_w = 1.0;
    if (norm)
    {
        g_ibl_norm_w = iblNormWeight(rough, amblit, env);
        rad *= g_ibl_norm_w;
    }
    return rad;
}

#ifdef CLEAN_REFL_RT_MARK
// RT REFLECTION MARK (#859; the split lighting's reflect kernels only -- clean_deferred_light.comp
// passes 2 / 5): what the RT reflection pass needs to put a traced hit in place of the
// environment term of r.spec / r.env without re-running this function. Both words are
//   word = tapMirror(mix(env, ssr, a)) = (1 - mw)(1 - a) env + (1 - mw) a ssr + mw mirror
// -- LINEAR in env -- so with k = (1 - mw)(1 - a) the traced radiance rt replaces env by
//   word' = word + k rt - k env.
// Per word: (k x the environment radiance it used (probes, or the sky along R), k). k = 0
// where SSR answered fully (a = 1) or the planar mirror covers it: those pixels never change.
vec4 g_rtm_spec = vec4(0.0);   // r.spec (PBR specular radiance / legacy glossenv)
vec4 g_rtm_env  = vec4(0.0);   // r.env  (legacy sharp env)
#endif
#ifdef CLEAN_REFL_RT_IN
// RT PRE-TRACE IN (#859; the uber deferred kernel): this pixel's traced reflection (rgb) and its
// splice weight (a; 0 = none), mixed into the environment radiance BEFORE SSR -- so SSR's share
// stays SSR's and only the probe / sky share is replaced (the split path's splice, in place).
vec4 CLEAN_REFL_RT_IN();
#endif
// CLEAN_REFL_RT_FWD(P) (#859; clean_draw_common.glsl, the main view's forward lit FS): the RT
// reflection of the front-most alpha-blend layer at this fragment (rgb, a = weight; 0 = none),
// mixed into the environment radiance before SSR, as CLEAN_REFL_RT_IN.

SurfaceRefl surfaceReflectionsW(SurfaceLit s, vec3 P, vec3 V, vec3 amblit, bool dbg9, uint want)
{
#ifdef CLEAN_LPROBE_NO_REFL
    {   // (lighting register probe, measurement only: no probes / SSR / mirror)
        SurfaceRefl r0; r0.irr = amblit; r0.spec = vec3(0.0); r0.env = vec3(0.0); r0.prad2 = vec3(0.0); r0.have = false;
        return r0;
    }
#endif
    const bool fullbright = s.model == SURF_FULLBRIGHT;
    const bool pbr = s.model == SURF_PBR;
    const bool classic = atmosClassic();
    const vec3 N = s.N;
    const vec3 Ng = s.Ng;
    // ONE sampleProbesEx call site for every material model (each call inlines the whole
    // cluster walk + top-N + box-projection code). What each model needs, with its own
    // normal, reflection vector, lods and amblit:
    //   fullbright shiny : radiance along reflect(-V, Ng) at lod 0, amblit 0 (env > 0 only)
    //   PBR              : radiance along reflect(-V, N) at rough x max lod + irradiance
    //   legacy           : glossy radiance at (1 - gloss) x max lod (gloss > 0), the sharp
    //                      lod-0 env as radiance 2 (env > 0), irradiance (not classic sky)
    // The walk's weights do not depend on what is fetched, so every result a model uses
    // is bit-identical to its own call. Debug view 9 (mirror-sharp probe content, lod 0
    // along R) is radiance 2 of the same walk.
    const float envIntensity = s.env;   // legacy env intensity; fullbright: the shiny level
    const vec3 pN = fullbright ? Ng : N;
    const vec3 R  = reflect(-V, pN);
    const float pLod = fullbright ? 0.0 : (pbr ? s.rough : (1.0 - s.gloss)) * phdr.info.y;
    const bool wSpec = (want & SR_SPEC) != 0u, wEnv = (want & SR_ENV) != 0u;
    const bool needRad  = wSpec && (fullbright ? envIntensity > 0.0 : (pbr || s.gloss > 0.0));
    const bool needRad2 = !fullbright && (pbr ? dbg9 : ((envIntensity > 0.0 && wEnv) || dbg9));
    // (#872) PBR specular normalization: it needs the walk's irradiance even when the caller
    // keeps only the specular (the split lighting's sharp pass) -- rough surfaces only.
    const bool iblNorm  = pbr && wSpec && s.rough > IBL_NORM_ROUGH0 && !litDbg(LDBG_NO_IBL_NORM);
    const bool needIrr  = ((want & SR_IRR) != 0u || iblNorm) && !fullbright && (pbr || !classic);
    vec3 prad = vec3(0.0), prad2 = vec3(0.0), pirr = amblit;
    bool have = false;
    if (needRad || needRad2 || needIrr)
        have = sampleProbesEx(P, pN, R, pLod, 0.0, amblit, needRad, needRad2, needIrr, prad, prad2, pirr);
    // The sky along R where no probe covers P (the legacy / fullbright radiance fallbacks; PBR
    // takes the prefiltered, normalized iblSkyRadiance).
    const vec3 skyR = (!have && !pbr && (needRad || needRad2)) ? materialFallbackSky(R,P) : vec3(0.0);
    g_ibl_norm_w = 1.0;
    CLK_MARK(PROBE);   // (shader clock, clean_shader_clock.glsl: empty in every other build)

    SurfaceRefl r;
    r.irr = amblit; r.spec = vec3(0.0); r.env = vec3(0.0);
    r.prad2 = prad2; r.have = have;
    if (fullbright)
    {
        // Shiny fullbright (fullbrightShinyF): glossiness 0 (spec = 0 there) => SSR weight
        // 0, skipped; no AO either (unlit). Planar mirror as LL tapHeroProbe.
        if (envIntensity > 0.0) r.spec = tapMirror(have ? prad : skyR, P, 1.0);
        CLK_MARK(MIRROR);
        return r;
    }
    if (pbr)
    {
        const float rough = s.rough;
        r.irr = have ? pirr : amblit;
        // (#872) the environment radiance at the diffuse ambient's level (see iblNormWeight)
        vec3 radiance = vec3(0.0);
        if (have)
        {
            radiance = prad;
            if (iblNorm)
            {
                g_ibl_norm_w = iblNormWeight(rough, pirr, g_probe_irr_ref);
                radiance *= g_ibl_norm_w;
            }
        }
        else if (needRad)
            radiance = iblSkyRadiance(N, R, rough, amblit,P);
#ifdef CLEAN_REFL_RT_IN
        if (wSpec) { const vec4 ri = CLEAN_REFL_RT_IN(); radiance = mix(radiance, ri.rgb, ri.a); }
#endif
#ifdef CLEAN_REFL_RT_FWD
        if (wSpec) { const vec4 ri = CLEAN_REFL_RT_FWD(P); radiance = mix(radiance, ri.rgb, ri.a); }
#endif
#ifdef CLEAN_REFL_ENV_FWD
        if (wSpec) radiance = CLEAN_REFL_ENV_FWD(s, P, R, radiance);   // (a blend layer's own environment occlusion)
#endif
#ifdef CLEAN_REFL_RT_MARK
        const vec3 envRad = radiance;
        float ssrA = 0.0;
#endif
        // SSR (LL doProbeSample): glossy surfaces blend the traced reflection over the
        // environment radiance; transparent ones trace as mirror, weighted by gloss.
        if (wSpec && specSSROn() && !probeCapture() && !mirrorPass() && (1.0 - rough) >= 0.9 && !litDbg(LDBG_NO_SSR))
        {
            const bool transparent = (v_flags & 2u) != 0u;
            vec4 sr = ssrTap(P, N, transparent ? 1.0 : (1.0 - rough));
            if (transparent) sr.a *= (1.0 - rough);
            radiance = mix(radiance, sr.rgb, sr.a);
#ifdef CLEAN_REFL_RT_MARK
            ssrA = sr.a;
#endif
        }
#ifdef CLEAN_REFL_RT_MARK
        {
            const float k = (1.0 - clamp(ssrA, 0.0, 1.0)) * (1.0 - tapMirrorWeight(P, 1.0 - rough));
            g_rtm_spec = vec4(envRad * k, k);
        }
#endif
        CLK_MARK(SSR);   // (after the branch: lanes that skip the march charge its wait to it)
        // Planar mirror (LL tapHeroProbe after SSR).
        r.spec = tapMirror(radiance, P, 1.0 - rough);
        CLK_MARK(MIRROR);
        return r;
    }
    // Legacy (reflectionProbeF sampleReflectionProbesLegacy): ambient from the probes (sky
    // ambient in classic mode), glossenv blurred by glossiness, legacyenv a SHARP (lod 0)
    // mirror sample, SSR blended into both.
    const float gloss = s.gloss;
    vec3 irradiance = amblit;
    vec3 glossenv = vec3(0.0), legacyenv = vec3(0.0);
    {
        // ONE probe walk (above) for everything this face uses: the irradiance (not in
        // classic mode, which keeps the sky ambient), the glossy radiance (gloss > 0) and
        // the sharp lod-0 legacy env (env intensity > 0, radiance 2). Matte legacy faces --
        // most SL content -- fetch irradiance only.
        if (have && !classic) irradiance = pirr;
        if (gloss > 0.0) glossenv = have ? prad : skyR;
        if (envIntensity > 0.0) legacyenv = have ? prad2 : skyR;
#if defined(CLEAN_REFL_RT_IN) || defined(CLEAN_REFL_RT_FWD)
        if (gloss > 0.0 || envIntensity > 0.0)
        {
#ifdef CLEAN_REFL_RT_IN
            const vec4 ri = CLEAN_REFL_RT_IN();
#else
            const vec4 ri = CLEAN_REFL_RT_FWD(P);
#endif
            if (gloss > 0.0) glossenv = mix(glossenv, ri.rgb, ri.a);
            if (envIntensity > 0.0) legacyenv = mix(legacyenv, ri.rgb, ri.a);
        }
#endif
#ifdef CLEAN_REFL_ENV_FWD
        if (gloss > 0.0) glossenv = CLEAN_REFL_ENV_FWD(s, P, R, glossenv);   // (one ray for both: blendHitEnv)
        if (envIntensity > 0.0) legacyenv = CLEAN_REFL_ENV_FWD(s, P, R, legacyenv);
#endif
#ifdef CLEAN_REFL_RT_MARK
        const vec3 envGloss = glossenv, envSharp = legacyenv;
        float ssrA = 0.0;
#endif
        // A TRANSPARENT face's tap is weighted by gloss alone: with gloss 0 (shiny legacy
        // faces: env intensity, no specular) the full mirror march was traced per
        // fragment and multiplied by zero -- skip it (identical result).
        if (((gloss > 0.0 && wSpec) || (envIntensity > 0.0 && wEnv)) && specSSROn() && !probeCapture() && !mirrorPass()
            && !litDbg(LDBG_NO_SSR) && !((v_flags & 2u) != 0u && gloss <= 0.0))
        {
            const bool transparent = (v_flags & 2u) != 0u;
            vec4 sr = ssrTap(P, N, transparent ? 1.0 : gloss);
            if (transparent) sr.a *= gloss;
            glossenv  = mix(glossenv,  sr.rgb, sr.a);
            legacyenv = mix(legacyenv, sr.rgb, sr.a);
#ifdef CLEAN_REFL_RT_MARK
            ssrA = sr.a;
#endif
        }
#ifdef CLEAN_REFL_RT_MARK
        {
            // (glossenv is clamped to 10 below: the RT pass re-clamps the spliced word the same way)
            const float ka = 1.0 - clamp(ssrA, 0.0, 1.0);
            const float kg = gloss > 0.0 ? ka * (1.0 - tapMirrorWeight(P, gloss)) : 0.0;
            const float ke = envIntensity > 0.0 ? ka * (1.0 - tapMirrorWeight(P, 1.0)) : 0.0;
            g_rtm_spec = vec4(envGloss * kg, kg);
            g_rtm_env  = vec4(envSharp * ke, ke);
        }
#endif
        CLK_MARK(SSR);
        // Planar mirror (LL sampleReflectionProbesLegacy: tapHeroProbe on both).
        glossenv  = tapMirror(glossenv, P, gloss);
        legacyenv = tapMirror(legacyenv, P, 1.0);
        glossenv = clamp(glossenv, vec3(0.0), vec3(10.0));
        CLK_MARK(MIRROR);
    }
    r.irr = irradiance; r.spec = glossenv; r.env = legacyenv;
    return r;
}
SurfaceRefl surfaceReflections(SurfaceLit s, vec3 P, vec3 V, vec3 amblit, bool dbg9)
{
    return surfaceReflectionsW(s, P, V, amblit, dbg9, SR_ALL);
}

// HAZE REUSE (#816): the lit models' additive / atten of the ray P - camPos, set by
// lightSurface's atmosLightTerms and reused by surfacePost's distance fade (same P, same
// camera: every caller passes lightSurface's P to surfacePost). g_haze_ok false: not computed
// (fullbright, or no lightSurface) -- surfacePost scatters the ray itself.
vec3 g_haze_add = vec3(0.0), g_haze_atten = vec3(1.0);
bool g_haze_ok = false;
bool g_medium_inline = false;

// WEATHER LIGHTNING (#783 M2; ssr.weather[8] / [9], LLCleanWeatherVK): a brief cool-white fill
// from the strike's direction -- soft (a wrapped Lambert plus a sky-wide share: the flash lights
// the whole cloud deck, so nothing goes fully dark on its far side) and unshadowed. Diffuse only
// (metals keep their reflections). Callers gate on ssr.weather[8].w > 0 (a uniform branch).
vec3 weatherFlashLight(vec3 N, vec3 base, float metal)
{
    const vec3 Lf = ssr.weather[9].xyz;
    const float wrap = clamp((dot(N, Lf) + 0.4) * (1.0 / 1.4), 0.0, 1.0);
    const float hemi = 0.5 + 0.5 * N.z;
    return ssr.weather[8].rgb * (base * (1.0 - metal)) * (0.65 * wrap + 0.35 * hemi);
}

#ifdef CLEAN_SURFACE_DEBUG
// Debug views lightSurface leaves to its caller, which tints / replaces the lit colour: 19,
// 23, 24 (the texture views) and 27 (lit guards) -- forward: clean_draw.frag main(); deferred:
// the G-buffer overlay / writeLitOutput -- and on the deferred path 11, 12 (the vertex tangent:
// the G-buffer overlay; clean_draw.frag gbufferDebugColor).
bool dbgViewElsewhere(int dbg)
{
#ifdef CLEAN_COMPUTE_LIGHT
    if (dbg == 11 || dbg == 12) return true;
#endif
    return dbg == 19 || dbg == 23 || dbg == 24 || dbg == 27;
}
#ifndef CLEAN_COMPUTE_LIGHT
// Views 11 (world tangent) / 12 (tangent sign: R = -1, G = +1; B = length).
vec3 dbgTangentView(bool sign)
{
    if (sign) return vec3(v_world_tangent.w < 0.0 ? 1.0 : 0.0, v_world_tangent.w > 0.0 ? 1.0 : 0.0,
                          clamp(length(v_world_tangent.xyz), 0.0, 1.0));
    return (dot(v_world_tangent.xyz, v_world_tangent.xyz) > 1e-12) ? normalize(v_world_tangent.xyz) * 0.5 + 0.5 : vec3(0.0);
}
#endif
#endif

// Light one decoded surface at world position P. Returns (colour, alpha) with the haze
// applied for the lit models; fullbright returns its unhazed colour (surfacePost hazes
// it, as the forward main() did). With CLEAN_SURFACE_DEBUG a RenderVKDebugView term
// replaces the colour (alpha 1).
vec4 lightSurface(SurfaceLit s, vec3 P, float alpha)
{
    const vec3 camPos = pc.u_cam_time.xyz;
    // World fullbright (LL fullbrightF): unlit -- only the shiny env below (the single
    // probe walk) and its return; none of the lit terms in between run for it.
    const bool fullbright = s.model == SURF_FULLBRIGHT;

    vec3 N = s.N;
    const vec3 V = mainViewDirection(P, camPos);
    const bool classic = atmosClassic();
    const vec3 L = atmosLightDir();      // sun or moon, whichever is up (LL light_dir)
    // Sun/moon shadow (LL scol): offsets use the geometric normal, not a normal map.
    // Evaluated only where it can matter -- every use is gated by N.L > 0 with the
    // final (normal-mapped) N -- so surfaces facing away from the light never trace
    // (ray-traced sun, #185) nor tap the cascades.
    const vec3 Ng = s.Ng;
#ifdef CLEAN_SURFACE_DEBUG
    if (!fullbright && litDebugView() == 17 && !probeCapture())   // DEBUG VIEW: probe cluster lists (#249)
    {
        // G = listed probes / PROBE_CLUSTER_CAP, B = a manual probe is listed, R = the
        // cluster OVERFLOWED (more candidates than the cap; brighter = further over).
        // Dark grey: no local probe (default probe 0 only). Shaded by N.up for shape.
        const uint c   = mirrorPass() ? mainClusterProject(P) : mainClusterFrag(P);
        const uint raw = probe_count[c];
        const uint n   = min(raw & 0xFFFFu, PROBE_CLUSTER_CAP);
        const uint tot = raw >> 16;
        bool man = false;
        for (uint k = 0u; k < n; ++k)
            if (probes[probe_index[c * PROBE_CLUSTER_CAP + k]].params.w > 0.5) man = true;
        vec3 col = (n == 0u) ? vec3(0.12)
                 : vec3(tot > PROBE_CLUSTER_CAP ? 0.5 + 0.5 * min(float(tot - PROBE_CLUSTER_CAP) / float(PROBE_CLUSTER_CAP), 1.0) : 0.0,
                        0.2 + 0.8 * float(n) / float(PROBE_CLUSTER_CAP),
                        man ? 1.0 : 0.0);
        return vec4(col * (0.6 + 0.4 * abs(s.N.z)), 1.0);
    }
    if (!fullbright && litDebugView() == 26 && !probeCapture() && !mirrorPass())   // DEBUG VIEW: local-light clusters
    {
        // The pixel's LIGHT cluster (clean_lights.comp; packing: clean_lights_common.glsl):
        //   G = listed lights / CLUSTER_CAP, R = the cluster OVERFLOWED (more lights touched
        //   it than the cap; brighter = further over), B = shadowed projectors listed (/ 8);
        //   clusters alternate brightness in a checkerboard (tile x slice) so their edges show.
        //   MAGENTA = a list entry past this frame's lights, or the cluster's shadowed
        //   projectors alone overflowed the cap (some are traced nowhere there).
        //   YELLOW / ORANGE hatching = a projector lights this point UNSHADOWED although
        //   projector shadows are ray traced (see the block below: yellow = the RT shadow mask
        //   holds it in no slot, orange = it has no shadow record at all).
        //   Probe cluster fill (as view 17) in the cell corner dots: white dots = the probe
        //   list overflowed. Dark grey = no light in the cluster.
        const uint c   = mainClusterFrag(P);
        const uint raw = cluster_count[c];
        const uint n   = clusterLen(raw);
        const uint ns  = clusterShadowedLen(raw);
        const uint tot = clusterCandidates(raw);
        const uint lcount = uint(linfo.cam_pos.w);
        bool bad = clusterShadowedOverflow(raw);
        for (uint i = 0u; i < n; ++i)
            if (cluster_index[c * CLUSTER_CAP + i] >= lcount) bad = true;
        const uint cx = c % GX, cy = (c / GX) % GY, cz = c / (GX * GY);
        const float chk = ((cx + cy + cz) & 1u) != 0u ? 1.0 : 0.7;
        vec3 col = (n == 0u) ? vec3(0.12)
                 : vec3(tot > CLUSTER_CAP ? 0.5 + 0.5 * min(float(tot - CLUSTER_CAP) / float(CLUSTER_CAP), 1.0) : 0.0,
                        0.15 + 0.85 * float(n) / float(CLUSTER_CAP),
                        min(float(ns) / 8.0, 1.0));
        col *= chk;
        // UNSHADOWED PROJECTOR LIGHT at P, by the lit FS's OWN gates up to its visibility call
        // (range, attenuation x the cutoff fade, the projected frustum, N.L > 0 on the shading
        // normal, the gobo texel x attenuation > 0.002 -- localLight): only a projector that
        // actually lights P counts (a black gobo texel lights nothing to shadow; the old check
        // skipped that gate and hatched every in-frustum point of a gobo's dark part).
        //   YELLOW hatching (/): a ray-traced projector lights P but the RT shadow mask holds it
        //     in no slot of the taps the lit FS reads -- neither its own nor the REST slot
        //     (clean_lights_common.glsl THE REST SLOT) -- so it is lit UNSHADOWED. Debug views
        //     light with the uber kernel's twin, where the mask traces the projectors in BOTH
        //     RenderVKProjStochastic modes (the stochastic sampler needs the split deferred
        //     lighting): this is always the exact path. The stochastic sampler's candidates are
        //     the cluster's whole shadowed run (B channel; MAGENTA when it overflowed), i.e.
        //     every projector with a shadow record -- nothing to hatch for it beyond the orange.
        //   ORANGE hatching (\): a projector lights P while projector shadows are ray traced,
        //     but it has NO shadow record -- ranked past RenderVKRTProjShadowMax: lit
        //     UNSHADOWED in every mode, everywhere it reaches.
        bool untraced = false, unranked = false;
        {
            const bool rtProj = specProjTraced() && pshadow[0].params.w > 0.5 && pshadow[0].params.z > 1.5;
#ifndef CLEAN_PROJ_SHADOW_RAYQUERY
            const bool maskOn = rtMaskUsable();
            RtMaskTaps t;
            if (maskOn) t = rtMaskTaps(P);
#else
            const bool maskOn = false;   // (the ray-query variant traces every record inline)
#endif
            for (uint li = 0u; li < min(lcount, 256u) && rtProj && !(untraced && unranked); ++li)
            {
                const Light L = lights[li];
                if (L.proj_origin.w < 0.5) continue;
                bool traced = false;
                if (L.tex.z != 0u)
                {
                    const ProjShadow ps = pshadow[L.tex.z - 1u];
                    if (ps.params.w < 0.5 || ps.params.z < 1.5) continue;   // (no RT record: not this view's question)
                    traced = true;
                }
                if (traced && (!maskOn || untraced)) continue;
                if (!traced && unranked) continue;
                vec3 lv;
                float dist;
                const float atten = lightReach(L.pos_radius, L.color_falloff, P, lv, dist);   // (#1043)
                if (atten <= 0.0) continue;
                vec2 ptc;
                if (!lightProjFrustum(L.proj_mat, P, ptc)) continue;
                if (!(dot(s.N, normalize(L.proj_origin.xyz - P)) > 0.0)) continue;
                const vec3 dlit = projDiffuse(L, -dot(lv, L.proj_axis.xyz), ptc.xy);
                if (!(max(max(dlit.r, dlit.g), dlit.b) * atten > 0.002)) continue;
                if (!traced) { unranked = true; continue; }
#ifndef CLEAN_PROJ_SHADOW_RAYQUERY
                bool found = false;
                [[unroll]] for (int k = 0; k < 4; ++k)   // (constant RtMaskTaps indices)
                {
                    if (!found && !(t.w[k] <= 0.0))
                        found = rtMaskSlotOf(rtMaskIdsAt(t.px[k]), li + 1u) >= 0;
                }
                untraced = !found;
#endif
            }
        }
        if (untraced && fract((P.x + P.y + P.z) * 4.0) < 0.5) col = vec3(1.0, 0.9, 0.1);
        else if (unranked && fract((P.x - P.y + P.z) * 4.0) < 0.5) col = vec3(1.0, 0.45, 0.0);
        if (bad) col = vec3(1.0, 0.0, 1.0);
        // probe list of the same cluster: a dot pattern
        const uint praw = probe_count[c];
        if (fract(P.x * 2.0) < 0.12 && fract(P.z * 2.0 + P.y * 2.0) < 0.12)
            col = (praw >> 16) > PROBE_CLUSTER_CAP ? vec3(1.0)
                : vec3(0.2, 0.2 + 0.8 * float(min(praw & 0xFFFFu, PROBE_CLUSTER_CAP)) / float(PROBE_CLUSTER_CAP), 0.9);
        return vec4(col * (0.6 + 0.4 * abs(s.N.z)), 1.0);
    }
    if (!fullbright && litDebugView() == 20 && !probeCapture())   // DEBUG VIEW: probe placement / slots (#478)
    {
        // The nearest listed probe of the pixel's cluster (by distance to its origin):
        // hue = its cube slot, saturation = its class (automatic vivid, terrain grid
        // pastel, manual white-ish), brightness rising towards its origin so each probe
        // shows as a bright blob on the surfaces around it; a white core within 0.75 m of
        // an origin; dark grey = no local probe (default probe only). Not yet captured:
        // striped.
        const uint c   = mirrorPass() ? mainClusterProject(P) : mainClusterFrag(P);
        const uint n   = min(probe_count[c] & 0xFFFFu, PROBE_CLUSTER_CAP);
        float bd = 1e30; uint bi = 0u;
        for (uint k = 0u; k < n; ++k)
        {
            const uint pi = probe_index[c * PROBE_CLUSTER_CAP + k];
            const float dd = distance(P, probes[pi].sphere.xyz);
            if (dd < bd) { bd = dd; bi = pi; }
        }
        if (n == 0u) return vec4(vec3(0.12) * (0.6 + 0.4 * abs(s.N.z)), 1.0);
        const Probe pb = probes[bi];
        const uint slot = uint(pb.idx.x);
        const float hue = fract(float(slot) * 0.61803399);
        const vec3 rgb = clamp(abs(fract(hue + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
        const float sat = pb.idx.w == 3 ? 1.0 : (pb.idx.w == 2 ? 0.45 : 0.15);
        vec3 col = mix(vec3(1.0), rgb, sat);
        col *= 0.25 + 0.75 * clamp(1.0 - bd / max(pb.sphere.w, 1.0), 0.0, 1.0);
        if (bd < 0.75) col = vec3(1.0);
        if (pb.idx.y == 0 && fract((P.x + P.y + P.z) * 2.0) < 0.5) col *= 0.3;
        // #565 RenderVKProbeFormat 3: magenta diagonal stripes = this probe is read from
        // the uncompressed live cube (its realtime capture), not its BC6H slot.
        if ((pb.idx.y >> 1) == 2 && fract((P.x - P.z + P.y) * 3.0) < 0.5) col = mix(col, vec3(1.0, 0.0, 1.0), 0.75);
        return vec4(col * (0.6 + 0.4 * abs(s.N.z)), 1.0);
    }
    if (!fullbright && litDebugView() == 25 && !probeCapture())   // DEBUG VIEW: probe blend weights
    {
        // The pixel's actual probe blend (clean_draw_common probeBlendDebug): the probes'
        // identity colours mixed by their final weights -- a gradient where the lighting
        // blends, a hard edge where it steps. Two or more contributing probes: fine
        // diagonal hatching (denser = more). Magenta: probes off / none.
        const vec4 b = probeBlendDebug(P);
        vec3 col = b.rgb;
        if (b.a >= 2.0 && fract((P.x + P.y - P.z) * (1.5 * b.a)) < 0.15) col *= 0.55;
        return vec4(col * (0.6 + 0.4 * abs(s.N.z)), 1.0);
    }
#endif
    const bool pbr = s.model == SURF_PBR;
    // SUN / MOON VISIBILITY, evaluated ONCE here for whichever branch below uses it (the
    // PBR and legacy branches and the cascade debug view used to call sunShadowT each:
    // the ray-query lit FS inlined the whole ray-traced sun core three times -- compile
    // time, not run time, as one fragment only ever takes one of them). The guards are
    // the branches' own: PBR gates on llPunctual's nl = clamp(N.L, 0.001, 1) > 0, legacy
    // on N.L > 0 (N = s.N in both), the debug view always traces. Fullbright: unlit.
    bool sunWant = surfaceSunWant(s, L);
#ifdef CLEAN_SURFACE_DEBUG
    const int dbgView = litDebugView();
    const bool dbgCascade = !fullbright && dbgView == 10 && !probeCapture();
    if (dbgCascade) sunWant = true;
#endif
    vec3 sunTint = vec3(1.0);
    float sunVis = 1.0;
#ifdef CLEAN_SUN_VIS_EXTERNAL
    // Split deferred lighting (clean_deferred_light.comp combine): the shadow pass's
    // visibility of this pixel, evaluated there on the same surfaceSunWant() pixels.
    if (sunWant) sunVis = CLEAN_SUN_VIS_EXTERNAL(sunTint);
#elif defined(CLEAN_VIS_REUSE)
    // Forward alpha on an opaque surface the split lighting resolved (clean_draw_common.glsl
    // visReuseMatch; also feeds the projector hook): its visibility, else the full lookup.
    visReuseMatch(P, Ng);
    if (sunWant && !visReuseSun(sunTint, sunVis)) sunVis = sunShadowT(P, Ng, sunTint);
#else
    if (sunWant) sunVis = sunShadowT(P, Ng, sunTint);
#endif
    CLK_MARK(SUNVIS);
#ifdef CLEAN_SURFACE_DEBUG
    if (dbgCascade)   // DEBUG VIEW: cascade index
    {
        const vec3 cc[4] = vec3[4](vec3(1, 0.2, 0.2), vec3(0.2, 1, 0.2), vec3(0.2, 0.4, 1), vec3(1, 1, 0.2));
        const float vz = dot(P - linfo.cam_pos.xyz, linfo.cam_fwd.xyz);
        const vec3 c = (!specSunOn() || vz >= ssr.shadow_clip.w) ? vec3(0.5) : cc[shadowCascade(vz)];
        return vec4(c * (0.35 + 0.65 * sunVis), 1.0);
    }
#endif
    const vec3 rel = P - camPos;

    // ATMOSPHERE light terms (both lit models; fullbright takes none).
    vec3 sunlit = vec3(0.0), amblit = vec3(0.0), additive = vec3(0.0), atten = vec3(1.0);
    if (!fullbright)
    {
        atmosLightTerms(rel, N, sunlit, amblit, additive, atten);
        if (classic) sunlit *= 1.35;
        g_haze_add = additive; g_haze_atten = atten; g_haze_ok = true;   // (#816: surfacePost's fade)
    }
    CLK_MARK(ATMOS);

    // ---- REFLECTIONS (surfaceReflections: probe walk, SSR, planar mirror, legacy env) ----
    const float envIntensity = s.env;   // legacy env intensity; fullbright: the shiny level
#ifdef CLEAN_SURFACE_DEBUG
    const bool dbg9 = !fullbright && dbgView == 9 && !probeCapture();
#else
    const bool dbg9 = false;
#endif
#ifdef CLEAN_REFL_EXTERNAL
    // Split deferred lighting (clean_deferred_light.comp combine): the reflection pass's
    // result for this pixel (surfaceReflections there, same inputs).
    const SurfaceRefl refl = CLEAN_REFL_EXTERNAL(s);
    CLK_MARK(PROBE);   // (the reflection buffers' read)
#else
#ifdef CLEAN_GI_FORWARD
#ifdef CLEAN_BLEND_HIT
    if (blendHitOn() && giFwdOn(s)) blendHitOcc(s, P);   // (RenderVKBlendHitLighting: the layer's own occlusion ray)
#endif
    const SurfaceRefl refl = giForwardRefl(surfaceReflections(s, P, V, amblit, dbg9), s, P);   // (#87: + GI)
#else
    const SurfaceRefl refl = surfaceReflections(s, P, V, amblit, dbg9);   // (marks PROBE / SSR / MIRROR itself)
#endif
#endif

    // World fullbright (LL fullbrightF): sRGB texel * colour, linearized (SKIP_ATMOS-style
    // resolve: srgb_to_linear(c) * sky_hdr_scale). Haze still applies (surfacePost).
    if (fullbright)
    {
        vec3 fb = s.albedo * atmosHdrScale();
        // FULLBRIGHT + SHINY (LL PASS_FULLBRIGHT_SHINY, fullbrightShinyF): opaque legacy
        // faces mix in the sharp legacy env reflection (applyLegacyEnv) at the shininess
        // level. Ingest leaves env_intensity 0 for non-shiny / alpha-pool faces; blend /
        // mask buckets never take this pass in LL (the decode leaves env 0 there).
        const float fbEnv = envIntensity;
        if (fbEnv > 0.0)
        {
            const vec3 env = refl.spec;   // (+ planar mirror; no SSR / AO: surfaceReflections)
            float fresnel = 1.0 + dot(-V, Ng);
            fresnel *= fresnel;
            fresnel = min(fresnel + fbEnv, 1.0);
            fb = mix(fb, env * (fbEnv * fresnel) * 0.5, fbEnv);
        }
        CLK_MARK(SHADE);
        return vec4(fb, alpha);
    }

    // CEL SHADING (#550): the toon terms below (banded sun / moon N.L with a tinted shadow
    // fill, flattened ambient, specular step, rim light) and the banded local lights
    // (g_cel, clean_draw_common.glsl localLight). Off: every term is the unmodified one.
    // RenderVKCel off (spec constant 14, clean_frame_info.glsl): no surface is cel shaded, and
    // every toon term below compiles out (the per-surface flag stays run time otherwise).
    s.cel = s.cel && specCelOn();
    g_cel = s.cel;

    // COMMON TAIL (below both branches): ONE localLights call site -- compile cost again
    // (each call inlined the local-light loop with the ray-traced projector core). The
    // branches leave the lit colour, the haze terms and the local lights' inputs here.
    vec3 color;
    vec3 ll_base;
    float ll_metal, ll_rough;
    LegacySpec ls;
    // Legacy-only terms the tail's alpha-blend glare reads (PBR leaves them unused).
    vec3 legacyenv = vec3(0.0);
    vec3 sunSpec = vec3(0.0);    // legacy: the sun's specular (debug view 21 shows it)
#ifdef CLEAN_SURFACE_DEBUG
    bool dbg21 = false;          // legacy debug view 21: sun + local specular only
#endif

    if (pbr)
    {
        // ---- glTF metallic-roughness: cleanSoftenF PBR branch + cl_pbrBaseLight ----
        const float rough = s.rough;
        const float metal = s.metal;
        const float ao    = s.ao;
        const vec3  baseC = s.albedo;

        vec3 irradiance = refl.irr;
        const vec3 radiance = refl.spec;   // probes / sky + SSR + planar mirror (surfaceReflections)
        const float ambocc = aoSample(P, s.Ng);
        irradiance = adjustIrradiance(irradiance, ambocc);   // LL: after sampleReflectionProbes
        if (s.cel) irradiance = celFlatten(irradiance, amblit * g_gi_amb_occ);   // (#87: GI-occluded sky reference)

        vec3 diffuseColor  = baseC * (vec3(1.0) - vec3(0.04)) * (1.0 - metal);
        vec3 specularColor = mix(vec3(0.04), baseC, metal);

        // cl_pbrIbl (split-sum; analytic DFG in place of LL's brdfLut)
        float NdotV = clamp(abs(dot(N, V)), 0.001, 1.0);
        // LL pbrIbl: the ORM occlusion scales both environment terms.
        vec3 iblDiff = irradiance * diffuseColor * ao;
        vec3 iblSpec = radiance * EnvBRDFApprox(specularColor, rough, NdotV) * ao
                     * specularOcclusion(NdotV, ambocc, rough);

        float nl; vec3 diffPunc, specPunc;
        llPunctual(diffuseColor, specularColor, rough, N, V, L, nl, diffPunc, specPunc);
        // Sun visibility + the colour translucent casters give the light (#212).
        vec3 stint = vec3(1.0);
        float scol = 1.0;
        if (sunWant) { scol = sunVis * sunTerminatorG(N, Ng, L); stint = sunTint; }   // (was nl > 0.0: the same test)
        sunlit *= classic ? atmosLinearToSrgb(stint) : stint;

        // Cel: the quantized light amount (sun visibility folded in, as LL's min(da, scol)).
        const float celLv = s.cel ? celRamp(nl * scol) : 0.0;
        if (classic)
        {
            vec3 irr = atmosSrgbToLinear(irradiance * 0.9);
            float da = pow(nl, 1.2);
            vec3 sun_contrib = vec3(s.cel ? celRamp(min(da, scol)) : min(da, scol));   // LL pbrBaseLight: min(da, scol)
            sun_contrib = atmosSrgbToLinear(atmosLinearToSrgb(sun_contrib) * sunlit * 0.7) * PI;
            vec3 finalAmbient = irr * diffuseColor;
            vec3 finalSun     = clamp(sun_contrib * ((diffPunc + specPunc) * scol), vec3(0), vec3(10));
            color = atmosSrgbToLinear(atmosLinearToSrgb(finalAmbient) + atmosLinearToSrgb(finalSun) * 1.1);
#ifdef CLEAN_SSS_EXPORT
            // (skin: the same recombination without the specular -- approximate, the sRGB-space
            //  sum is not additive)
            if (g_sss_want)
                g_sss_d = atmosSrgbToLinear(atmosLinearToSrgb(finalAmbient)
                                            + atmosLinearToSrgb(clamp(sun_contrib * (diffPunc * scol), vec3(0), vec3(10))) * 1.1);
#endif
        }
        else if (s.cel)
        {
            // Banded diffuse + a tinted fill on the dark side + a hard specular highlight.
            const vec3 specTint = specularColor / max(max(max(specularColor.r, specularColor.g), specularColor.b), 1e-3);
            vec3 sunT = diffPunc * celLv + diffPunc * ((1.0 - celLv) * cel_shadow.a) * cel_shadow.rgb
                      + celSpecStep(nl * specPunc * scol, specTint);
            color = iblDiff + clamp(sunT, vec3(0), vec3(10)) * sunlit * 3.0;
        }
        else
        {
            color = iblDiff + clamp(nl * (diffPunc + specPunc), vec3(0), vec3(10)) * sunlit * 3.0 * scol;
#ifdef CLEAN_SSS_EXPORT
            // (skin: the ambient + the sun's diffuse share of the clamped BRDF)
            if (g_sss_want)
            {
                const vec3 t = nl * (diffPunc + specPunc);
                g_sss_d = iblDiff + nl * diffPunc * (clamp(t, vec3(0), vec3(10)) / max(t, vec3(1e-12))) * sunlit * 3.0 * scol;
            }
#endif
        }
        color += iblSpec;
        color += s.emissive;
        if (s.cel) color += cel_rim.rgb * celRimMask(NdotV, N.z) * (irradiance + sunlit * celLv);

#ifdef CLEAN_SURFACE_DEBUG
        // DEBUG VIEW (RenderVKDebugView): one shading term instead of the lit colour.
        {
            const int dbg = litDebugView();
            if (dbg > 0 && !dbgViewElsewhere(dbg) && !probeCapture())
            {
                vec3 d = vec3(0.0);
                if      (dbg == 1) d = baseC;
                else if (dbg == 2) d = vec3(metal, rough, ao);
                else if (dbg == 3) d = radiance;
                else if (dbg == 4) d = irradiance;
                else if (dbg == 5) d = iblSpec;
                else if (dbg == 6) d = iblDiff;
                else if (dbg == 7) d = clamp(nl * (diffPunc + specPunc), vec3(0), vec3(10)) * sunlit * 3.0;
                else if (dbg == 8) d = N * 0.5 + 0.5;
#ifndef CLEAN_COMPUTE_LIGHT
                else if (dbg == 11) d = dbgTangentView(false);
                else if (dbg == 12) d = dbgTangentView(true);
#endif
                else if (dbg == 16) d = vec3(ambocc);
                else if (dbg == 9)   // mirror-sharp probe content along R (lod 0): the walk's radiance 2
                    d = refl.have ? refl.prad2 : vec3(1.0, 0.0, 1.0);
                else if (dbg == 29)  // (#872) the environment specular's normalization weight:
                    // white = unchanged (smooth, or the probes light the diffuse), darker = dimmed to
                    // the diffuse ambient; amber = the sky fallback (no probe covers P)
                    d = vec3(g_ibl_norm_w) * (refl.have ? vec3(1.0) : vec3(1.0, 0.75, 0.3));
                return vec4(d, 1.0);
            }
        }
#endif

        // Local lights (common tail): PBR, no legacy specular.
        ll_base = baseC; ll_metal = metal; ll_rough = rough;
        ls = legacySpecNone();
    }
    else
    {
        // ---- legacy / Blinn-Phong: cleanSoftenF legacy branch ----
        const vec3 base_lin = s.albedo;
        const float gloss = s.gloss;
        const vec3 spec_lin = s.spec;

        // reflectionProbeF sampleReflectionProbesLegacy (surfaceReflections): ambient from
        // the probes (sky ambient in classic mode), glossenv blurred by glossiness,
        // legacyenv a SHARP (lod 0) mirror sample, SSR + planar mirror blended into both.
        vec3 irradiance = refl.irr;
        vec3 glossenv = refl.spec;
        legacyenv = refl.env;
        const float ambocc = aoSample(P, s.Ng);
        irradiance = adjustIrradiance(irradiance, ambocc);   // LL: after sampleReflectionProbesLegacy
        if (s.cel) irradiance = celFlatten(irradiance, amblit * g_gi_amb_occ);   // (#87: GI-occluded sky reference)
        {
            const float nv = clamp(abs(dot(N, V)), 0.001, 1.0);
            glossenv  *= specularOcclusion(nv, ambocc, 1.0 - gloss);
            legacyenv *= specularOcclusion(nv, ambocc, 0.0);
        }

        float da = clamp(dot(N, L), 0.0, 1.0);
        vec3 stint = vec3(1.0);
        float scol = 1.0;
        if (sunWant) { scol = sunVis * sunTerminatorG(N, Ng, L); stint = sunTint; }   // (was N.L > 0: the same test)
        sunlit *= classic ? atmosLinearToSrgb(stint) : stint;   // translucent casters (#212)
        color = irradiance;
        float celLv = 0.0;   // cel: quantized light amount (#550)
        if (classic)
        {
            da = pow(da, 1.2);
            if (s.cel) celLv = celRamp(min(da, scol));
            vec3 sun_contrib = vec3(s.cel ? celLv : min(da, scol));   // LL softenLight: min(da, scol)
            color = atmosSrgbToLinear(color * 0.9 + atmosLinearToSrgb(sun_contrib) * sunlit * 0.7);
            sunlit = atmosSrgbToLinear(sunlit);
        }
        else if (s.cel)
        {
            celLv = celRamp(min(da, scol));
            color += (celLv + (1.0 - celLv) * cel_shadow.a * cel_shadow.rgb) * sunlit;
        }
        else
        {
            color += min(da, scol) * sunlit;
        }
        color *= base_lin;
#ifdef CLEAN_SSS_EXPORT
        if (g_sss_want) g_sss_d = color;   // (skin: ambient + sun x the base, all diffuse so far)
#endif

        // Legacy specular env (reflectionProbeF applyGlossEnv / applyLegacyEnv).
        if (gloss > 0.0)
        {
            // Sun specular (softenLightF legacy branch): normalized Blinn-Phong from LL's
            // lightFunc LUT (clean_brdf.glsl legacySpecular, shared with the local lights),
            // shadowed by scol.
            {
                float slit;
                const float ss = legacySpecular(N, V, L, gloss, slit);
                sunSpec = (slit * scol * ss) * sunlit * spec_lin;
                if (s.cel)   // hard highlight in the light's colour x the specular hue (#550)
                    sunSpec = celSpecStep(sunSpec, sunlit * (spec_lin / max(max(max(spec_lin.r, spec_lin.g), spec_lin.b), 1e-3)));
                color += sunSpec;
            }
            vec3 ge = glossenv * 0.5;
            float fresnel = clamp(1.0 + dot(-V, N), 0.3, 1.0);
            fresnel *= fresnel;
            fresnel *= gloss;
            ge *= spec_lin * fresnel;
            ge *= max(vec3(1.0) - color, vec3(0.0));   // HDR / emissive past 1.0 must not subtract the reflection
            color += ge * 0.5;
        }
        if (envIntensity > 0.0)
        {
            float fresnel = 1.0 + dot(-V, N);
            fresnel *= fresnel;
            fresnel = min(fresnel + envIntensity, 1.0);
            vec3 reflected = legacyenv * (envIntensity * fresnel);
            color = mix(color, reflected * 0.5, envIntensity);
#ifdef CLEAN_SSS_EXPORT
            g_sss_d *= 1.0 - envIntensity;   // (the env mix keeps this share of the diffuse)
#endif
        }
        if (s.cel) color += cel_rim.rgb * celRimMask(clamp(abs(dot(N, V)), 0.0, 1.0), N.z) * (irradiance + sunlit * celLv);
        color += s.emissive;   // <GMDX> rez effect emission (CombatFX; legacy faces carry none otherwise)

#ifdef CLEAN_SURFACE_DEBUG
        {   // DEBUG VIEW (legacy faces): base / irradiance / env / normal; PBR-only terms black.
            const int dbg = litDebugView();
            if (dbg > 0 && !dbgViewElsewhere(dbg) && !probeCapture())
            {
                vec3 d = vec3(0.0);
                if      (dbg == 1) d = base_lin;
                else if (dbg == 21) dbg21 = true;   // legacy specular only: sun + local lights (#470), in the tail
                else if (dbg == 3) d = max(glossenv, legacyenv);
                else if (dbg == 4) d = irradiance;
                else if (dbg == 16) d = vec3(ambocc);
                else if (dbg == 8) d = N * 0.5 + 0.5;
#ifndef CLEAN_COMPUTE_LIGHT
                else if (dbg == 11) d = dbgTangentView(false);
                else if (dbg == 12) d = dbgTangentView(true);
#endif
                else if (dbg == 9)   // the walk's radiance 2 (lod 0 along R), as PBR
                    d = refl.have ? refl.prad2 : vec3(1.0, 0.0, 1.0);
                if (!dbg21) return vec4(d, 1.0);
            }
        }
#endif
        // Local lights with the legacy specular (#470: point + projector Blinn-Phong, and
        // the projector image reflection on shiny faces); debug view 21: specular only (no
        // albedo, no env).
        ll_metal = 0.0; ll_rough = 1.0;
        ls.spec = spec_lin; ls.gloss = gloss; ls.env = envIntensity;
        ll_base = base_lin;
#ifdef CLEAN_SURFACE_DEBUG
        if (dbg21) { ll_base = vec3(0.0); ls.env = 0.0; }
#endif
    }

    // Lightning (#783 M2): never in probe captures (frame info row 8 is zero in snapshots too).
    if (ssr.weather[8].w > 0.0 && !probeCapture()) color += weatherFlashLight(N, ll_base, ll_metal);
    CLK_MARK(SHADE);
    float glare;
#ifdef CLEAN_LOCAL_EXTERNAL
    // Feature-split deferred lighting's BASE (#827, clean_deferred_light.comp): the local lights
    // run in their own pass (LOCAL), which adds this call's sum x surfaceLocalGain onto the
    // output; here they contribute an exact 0 (the clamp + haze below see colour + 0).
    glare = 0.0;
    vec3 local = vec3(0.0);
#elif defined(CLEAN_BLEND_HIT)
    // (RenderVKBlendHitLighting) a blended layer's own local light, shaded as a ray-traced hit's (clean_draw_common
    // .glsl blendHitLocal: every light shadow-tested within a fixed ray budget); else the forward walk
    vec3 local = blendHitOn() ? blendHitLocal(P, N, V, pbr, ll_base, ll_metal, ll_rough, ls, glare)
                              : localLights(P, N, V, pbr, ll_base, ll_metal, ll_rough, ls, glare);
#else
    vec3 local = localLights(P, N, V, pbr, ll_base, ll_metal, ll_rough, ls, glare);
#ifdef CLEAN_SSS_EXPORT
    // (skin: the local sum scatters where it is all diffuse -- a legacy face with no specular and
    //  no env; otherwise it stays sharp, the walk is not split into its shares)
    if (g_sss_want && !pbr && ls.gloss <= 0.0 && ls.env <= 0.0) g_sss_tail = local;
#endif
#endif
    CLK_MARK(LOCAL);
#ifdef CLEAN_SURFACE_DEBUG
    if (dbg21) return vec4(sunSpec + local, 1.0);
#endif
#ifdef CLEAN_EMITTER_LIGHT
    // (#91) EMISSIVE SURFACES' light (the deferred BASE / combine kernels: clean_deferred_light.comp
    // deferEmitterLight -- the cell's emitting triangles x the denoised ReSTIR DI ratio), a diffuse term
    // beside the local lights (LOCAL never adds it: BASE carries it on every lit pixel). The local
    // lights' diffuse colour: PBR base x 0.96 x (1 - metal), legacy the base; (RenderVKReSTIRRadiance:
    // the whole stochastic lights' radiance, remodulated -- + their specular colour: PBR F0, legacy
    // the specular colour)
    local += CLEAN_EMITTER_LIGHT(P, N, pbr ? ll_base * (1.0 - 0.04) * (1.0 - ll_metal) : ll_base,
                                 pbr ? mix(vec3(0.04), ll_base, ll_metal) : ls.spec);
#endif
    color = atmosClampHDR(color * (classic ? 1.1 : 1.0)) + local * (classic ? 0.9 : 1.0);
    if (!cmOn() && !litDbg(LDBG_NO_HAZE))
        color = atmosOrWaterFog(color, P, camPos, additive * g_haze_sky_k, atten);
    CLK_MARK(HAZE);
#ifdef CLEAN_GI_FORWARD
    if (g_gi_fwd_dbg.a > 0.0) return g_gi_fwd_dbg;   // (#1438 GI debug view 6 on a forward surface)
#endif
    if (pbr) return vec4(color, alpha);
    // ALPHA-BLEND GLARE (LL materialF DIFFUSE_ALPHA_MODE_BLEND): the local lights'
    // specular and the shiny env reflection raise the coverage, so a highlight on clear
    // glass shows at full strength instead of at the texture's alpha:
    // al = max(texel alpha, glare) * vertex alpha, glare = min(sum, 1).
    if (s.glareA > 0.0)
    {
        if (envIntensity > 0.0)
            glare += clamp(max(max(legacyenv.r, legacyenv.g), legacyenv.b), 0.0, 1.0) * envIntensity;
        alpha = max(alpha, min(glare, 1.0) * s.glareA);
    }
    return vec4(color, alpha);
}

// The world post the forward main() applies after shading (not HUD, no debug view):
// fullbright faces take the haze here (they skipped the lit branches), then the
// main-view distance fade -- except probe captures, underwater points (the water fog
// is the fade) and objects enclosing the camera (VS bit14: domes / sim surrounds, never
// distance-culled, so no cull pop to hide).
// HAZE REUSE (#816): the lit models' haze of the ray P - camPos (lightSurface's
// atmosLightTerms, this same P and camera) feeds the fade target; fullbright scatters the ray
// once here for both its haze and the fade. Bit-identical to scattering it again.
vec3 surfacePost(uint model, bool noFade, vec3 color, vec3 P)
{
    const vec3 camPos = pc.u_cam_time.xyz;
    const bool fog = waterFogged(P);
    if(cmOn())
    {
        if(probeCapture() || mirrorPass())
        {
            const vec3 origin=mainReceiverEye(P,camPos);
            return cmWaterPrefix(atmosApply(color,P,origin),P,origin);
        }
        if(!g_medium_inline) return color;
        const vec3 origin=mainReceiverEye(P,camPos);
        color=cmWaterPrefix(cmApply(color,cmAt(P,CLEAN_VIEW)),P,origin);
        if(!fog && !noFade)
        {
            g_cm_sky_prefix=true;g_cm_sky_origin=origin;
            color=atmosDistanceFade(color,P,origin,linfo.cam_fwd,linfo.zinfo.y);
        }
        return color;
    }
    vec3 hAdd = g_haze_add, hAtt = g_haze_atten;
    const bool fade = !probeCapture() && !fog && !noFade;
    const bool fbHaze = model == SURF_FULLBRIGHT && !litDbg(LDBG_NO_HAZE) && !fog;
    if (!g_haze_ok && atmosEnabled() && (fbHaze || fade)) atmosScatterHaze(P - camPos, hAdd, hAtt);
    if (model == SURF_FULLBRIGHT && !litDbg(LDBG_NO_HAZE))
        color = fog ? applyWaterFog(color, P - camPos, camPos.z - waterHeight())
                    : atmosApplyRay(color, hAdd, hAtt);
    if (fade)
        color = atmosDistanceFade(color, P, camPos, linfo.cam_fwd, linfo.zinfo.y, hAdd, hAtt);
    return color;
}

// LOCAL GAIN (#827, the feature-split deferred lighting: clean_deferred_light.comp BASE writes it,
// LOCAL multiplies its local-light sum by it): the derivative of a LIT surface's final colour
// (lightSurface, then surfacePost) with respect to lightSurface's local sum. Everything after
// `color = atmosClampHDR(...) + local * k2` is affine in colour with a scalar slope:
//   k2                     classic ? 0.9 : 1.0
//   haze (unless LDBG_NO_HAZE): atmosOrWaterFog -- water fog lin * f.a + f.rgb (f.a: waterFogLL),
//                          else the haze lin * atten.r + add (atten = g_haze_atten: lightSurface's
//                          own atmosLightTerms of this ray), or 1 with the atmosphere off
//   fade (surfacePost's `fade` gate): mix(lin, target, f) -> 1 - f (atmosDistanceFadeKeep)
// so final(local) = final(0) + local x gain. Call after lightSurface (g_haze_atten), with
// surfacePost's noFade / P. Mirrors those functions' conditions one for one: keep in step.
float surfaceLocalGain(bool noFade, vec3 P)
{
    const vec3 camPos = pc.u_cam_time.xyz;
    float g = atmosClassic() ? 0.9 : 1.0;
    const bool fog = waterFogged(P);
    if (!cmOn() && !litDbg(LDBG_NO_HAZE))
        g *= fog ? waterFogLL(P - camPos, camPos.z - waterHeight()).a : (atmosEnabled() ? g_haze_atten.r : 1.0);
    if (!cmOn() && !probeCapture() && !fog && !noFade)
        g *= atmosDistanceFadeKeep(P, camPos, linfo.cam_fwd.xyz, linfo.zinfo.y);
    return g;
}

#if defined(CLEAN_BASIC) || defined(CLEAN_DEFER_BASIC)
// #1411 BASIC LIGHTING (the deferred placeholder tier, LLCleanRendererVK mUpgradeJob): what the
// first frames of a cold-cache login are lit with while the full lit FS / lighting kernels
// compile -- seconds, then the full pipelines swap in at a frame boundary. Sun / moon N.L with
// the sky's sunlight and ambient terms (atmosLightTerms: the same per-frame constants the full
// path starts from), the material's albedo, AO and emissive; fullbright as the full path's plain
// fullbright. No shadows, probes / reflections, SSR, local lights, cel or specular: everything that
// makes the full shader 300k-650k SPIR-V words. The haze is applied by the caller (surfacePost),
// as for lightSurface's result.
vec4 basicLight(SurfaceLit s, vec3 P, float alpha)
{
    if (s.model == SURF_FULLBRIGHT) return vec4(s.albedo * atmosHdrScale(), alpha);
    vec3 sunlit, amblit, additive, atten;
    atmosLightTerms(P - pc.u_cam_time.xyz, s.N, sunlit, amblit, additive, atten);
    const vec3 diffuse = s.model == SURF_PBR ? s.albedo * (1.0 - s.metal) : s.albedo;
    const float ndl = max(dot(s.N, atmosLightDir()), 0.0);
    vec3 c = diffuse * (sunlit * ndl + amblit * s.ao);
    if (atmosClassic()) c = atmosSrgbToLinear(c);   // (classic terms are sRGB-space: the placeholder's approximation)
    return vec4(c * atmosHdrScale() + s.emissive, alpha);
}
#endif

#endif   // CLEAN_SURFACE_LIGHT_GLSL
