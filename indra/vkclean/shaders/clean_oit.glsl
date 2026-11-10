// vkclean -- WEIGHTED BLENDED ORDER-INDEPENDENT TRANSPARENCY (#1410, RenderVKAlphaOIT; McGuire &
// Bavoil 2013, "Weighted Blended Order-Independent Transparency", JCGT 2(2)). The accumulation
// variants of clean_draw.frag (-DCLEAN_OIT_ACCUM: the lit FS, mode 1; -DCLEAN_OIT_GBUF: the unlit
// surface, mode 2) and the resolve (clean_oit_resolve.frag) share this file.
//
// Every OIT fragment adds  w * a * (its value)  into additive RGBA16F targets and multiplies the
// revealage target by (1 - a) (R16F, ZERO / ONE_MINUS_SRC_COLOR). The resolve divides by the
// accumulated w * a and composites  value * (1 - revealage)  over the scene. One layer is exact;
// several layers are a weighted average in place of the ordered over-chain.
//
// WEIGHT (oitWeight): McGuire's 2015 alpha term (min(1, 10 a) + 0.01)^3 -- a near-opaque strand is
// not washed out by the faint fringes around it -- times the paper's near-field 1 / z^2 falloff
// (eq. 9's (z / 5)^2 term), normalised to [0.01, 1] for fp16:
//     w(z, a) = (min(1, 10 a) + 0.01)^3 * clamp((zr / z)^2, 0.01, 1)
// zr = RenderVKAlphaOITDepthRef (default 0.5 m): flat inside zr, 1 / z^2 out to 10 zr (5 m), flat
// beyond (SL view depths 0.1 .. ~500 m). Two heads at 2 m and 5 m weigh 6.25 : 1 (as eq. 9); layers
// of one head a few cm apart differ by a few percent, so inside a hair stack the alpha term rules.
// RANGE: w <= 1.03, so a sum of L layers of colour C stays below L * C -- no fp16 overflow below
// ~6000 HDR units over 10 layers (the paper's 3e3 cap overflowed at sunlit HDR values) -- and the
// layers that matter (a >= 0.1) keep w >= 0.01, well inside the fp16 normal range.
#ifndef CLEAN_OIT_GLSL
#define CLEAN_OIT_GLSL

// The OIT draws' push word (clean_draw_view.glsl g_draw_push.pad1, reset by the next
// pushDrawView): bit 0 the debug tint (RenderVKAlphaOITDebug bit 1), bits 8-31 zr in mm.
const uint OIT_PAD_TINT = 1u;
float oitDepthRef(uint pad) { const float mm = float(pad >> 8u); return mm > 0.0 ? mm * 0.001 : 0.5; }
bool  oitTint(uint pad) { return (pad & OIT_PAD_TINT) != 0u; }

// z: the fragment's view depth (m); a: its coverage.
float oitWeight(float a, float z, uint pad)
{
    const float zr = oitDepthRef(pad);
    const float at = min(1.0, 10.0 * a) + 0.01;
    const float dt = clamp((zr * zr) / max(z * z, 1e-6), 0.01, 1.0);
    return at * at * at * dt;
}

// The debug tint (RenderVKAlphaOITDebug bit 1): what the classification put in the OIT group.
const vec3 OIT_TINT = vec3(0.0, 1.0, 0.85);

// Largest colour the mode-1 accumulation takes (the sum stays finite in fp16, see above).
const float OIT_MAX_HDR = 4096.0;

#endif   // CLEAN_OIT_GLSL
