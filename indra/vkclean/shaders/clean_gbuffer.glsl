// vkclean — DEFERRED G-BUFFER layout: pack / unpack of a
// SurfaceLit (clean_surface_light.glsl). Written by clean_draw.frag -DCLEAN_GBUFFER (pass
// 1b's G-buffer draws, EQUAL after the depth pre-pass), read by
// clean_deferred_light.comp. Depth is the pre-pass depth (reconstructed position).
//
//   G0 RGBA8_SRGB          rgb albedo (linear in, sRGB-encoded storage), a = PBR ORM
//                          occlusion | legacy env intensity | fullbright shiny level
//   G1 RGBA16F             xy oct(N) shading normal, zw oct(Ng) geometric normal
//   G2 RGBA8_UNORM         x PBR roughness | legacy glossiness, y PBR metallic,
//                          z glow (LL te glow, PBR: x max emissive), w model byte:
//                          bits 0-1 SURF_* (0 = no surface: cleared), bit 2 no distance fade,
//                          bits 3-4 the VARIANT (GB_VAR_*, below): 1 cel shading (#550,
//                          SurfaceLit.cel), 2 <GMDX> rez effect emission in G3 (legacy:
//                          instead of the specular colour), 3 SKIN (SurfaceLit.skin: the
//                          subsurface scattering's pixels), bits 5-7 the OVERLAY code (GB_OV_*:
//                          drawn over the lit colour, #1415)
//   G3 B10G11R11_UFLOAT    PBR emissive (linear HDR) | legacy specular colour (linear)
// 20 B / pixel (+ the existing depth).
//
// GLOW: the G-buffer carries the glow FACTOR only; the lighting pass emits the glow
// contribution (display-encoded lit colour x factor) through ONE function
// (clean_deferred_light.comp writeLitOutput), the merge point for packing glow elsewhere.

#ifndef CLEAN_GBUFFER_GLSL
#define CLEAN_GBUFFER_GLSL

vec2 gbOctWrap(vec2 v) { return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }
vec2 gbOctEncode(vec3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-20);
    return n.z >= 0.0 ? n.xy : gbOctWrap(n.xy);
}
vec3 gbOctDecode(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    const float t = max(-n.z, 0.0);
    n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}

const uint GB_NOFADE = 4u;
// THE VARIANT (bits 3-4): one of the lit models' exclusive treatments. They were two flag bits
// (cel 8, rez effect 16); the fourth code is SKIN. Precedence when a surface asks for more than
// one: the rez effect (a few seconds of rez paint) over cel, cel (toon lighting: no scattering)
// over skin -- so a cel-shaded face mid rez effect loses its toon terms for those seconds.
const uint GB_VAR_SHIFT = 3u;
const uint GB_VAR_NONE  = 0u;
const uint GB_VAR_CEL   = 1u;   // toon lighting (#550)
const uint GB_VAR_REZFX = 2u;   // <GMDX> legacy face with rez effect emission: G3 = emissive (no specular)
const uint GB_VAR_SKIN  = 3u;   // SKIN (RenderVKSkinSSS): PBR / legacy only
uint gbufferVariant(vec4 g2) { return (uint(g2.w * 255.0 + 0.5) >> GB_VAR_SHIFT) & 3u; }
bool gbufferIsSkin(vec4 g2) { return gbufferVariant(g2) == GB_VAR_SKIN; }
// OVERLAY (#1415, bits 5-7): what the lighting draws over this pixel's lit colour
// (clean_deferred_light.comp writeLitOutput) -- the builder / debug views the G-buffer FS
// knows per fragment. 0 none; HL_*: Highlight Transparent (LL sShowDebugAlpha: alpha-mask
// faces red, water exclusion surfaces green); RATE_*: the shading-rate overlay
// (RenderVKLitDebugMask 4096: the fragment's coarse size); DBG_*: a RenderVKDebugView colour
// the G-buffer FS computed (G3 holds it instead of the emissive / specular colour) -- RAW is
// shown as is, OVER_LUM over the lit colour's brightness (0 = dark: no texture).
const uint GB_OV_SHIFT    = 5u;
const uint GB_OV_NONE     = 0u;
const uint GB_OV_HL_RED   = 1u;
const uint GB_OV_HL_GREEN = 2u;
const uint GB_OV_RATE_2X1 = 3u;
const uint GB_OV_RATE_1X2 = 4u;
const uint GB_OV_RATE_2X2 = 5u;
const uint GB_OV_DBG_RAW  = 6u;
const uint GB_OV_DBG_LUM  = 7u;
uint gbufferOverlay(vec4 g2) { return (uint(g2.w * 255.0 + 0.5) >> GB_OV_SHIFT) & 7u; }

// overlay: GB_OV_*; dbgColor: G3 for the DBG_* codes.
void gbufferPack(SurfaceLit s, float glow, bool noFade, uint overlay, vec3 dbgColor,
                 out vec4 g0, out vec4 g1, out vec4 g2, out vec4 g3)
{
    float a = 0.0, x = 0.0, y = 0.0;
    vec3 e = vec3(0.0);
    if (s.model == SURF_PBR)            { a = s.ao;  x = s.rough; y = s.metal; e = s.emissive; }
    else if (s.model == SURF_LEGACY)    { a = s.env; x = s.gloss; e = s.spec; }
    else if (s.model == SURF_FULLBRIGHT){ a = s.env; }
    // <GMDX> rez effect (CombatFX): a legacy face's emission takes G3 (its specular colour drops there)
    const bool rezfx = s.model == SURF_LEGACY && max(max(s.emissive.r, s.emissive.g), s.emissive.b) > 0.0;
    if (rezfx) e = s.emissive;
    g0 = vec4(clamp(s.albedo, vec3(0.0), vec3(1.0)), clamp(a, 0.0, 1.0));
    g1 = vec4(gbOctEncode(s.N), gbOctEncode(s.Ng));
    if (overlay >= GB_OV_DBG_RAW) e = dbgColor;
    const bool lit = s.model == SURF_PBR || s.model == SURF_LEGACY;
    const uint var = rezfx ? GB_VAR_REZFX : (s.cel ? GB_VAR_CEL : ((s.skin && lit) ? GB_VAR_SKIN : GB_VAR_NONE));
    const uint mb = (s.model & 3u) | (noFade ? GB_NOFADE : 0u) | (var << GB_VAR_SHIFT)
                  | ((overlay & 7u) << GB_OV_SHIFT);
    g2 = vec4(clamp(x, 0.0, 1.0), clamp(y, 0.0, 1.0), clamp(glow, 0.0, 1.0), float(mb) * (1.0 / 255.0));
    g3 = vec4(max(e, vec3(0.0)), 0.0);
}

SurfaceLit gbufferUnpack(vec4 g0, vec4 g1, vec4 g2, vec4 g3, out float glow, out bool noFade)
{
    SurfaceLit s = surfaceInit();
    const uint mb = uint(g2.w * 255.0 + 0.5);
    s.model = mb & 3u;
    noFade = (mb & GB_NOFADE) != 0u;
    const uint var = (mb >> GB_VAR_SHIFT) & 3u;
    s.cel = var == GB_VAR_CEL;
    s.skin = var == GB_VAR_SKIN;
    glow = g2.z;
    s.albedo = g0.rgb;
    s.N  = gbOctDecode(g1.xy);
    s.Ng = gbOctDecode(g1.zw);
    if (s.model == SURF_PBR)
    {
        s.ao = g0.a; s.rough = g2.x; s.metal = g2.y; s.emissive = g3.rgb;
    }
    else if (s.model == SURF_LEGACY)
    {
        s.env = g0.a; s.gloss = g2.x; s.spec = g3.rgb;
        if (var == GB_VAR_REZFX) { s.emissive = g3.rgb; s.spec = vec3(0.0); }   // <GMDX>
    }
    else if (s.model == SURF_FULLBRIGHT)
    {
        s.env = g0.a;
    }
    if (((mb >> GB_OV_SHIFT) & 7u) >= GB_OV_DBG_RAW) { s.emissive = vec3(0.0); s.spec = vec3(0.0); }   // (G3: the debug colour)
    return s;
}

#endif   // CLEAN_GBUFFER_GLSL
