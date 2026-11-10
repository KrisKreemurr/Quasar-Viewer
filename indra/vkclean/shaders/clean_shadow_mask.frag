#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
// vkclean — shadow-map alpha test (LL shadowAlphaMaskF). Depth-only pass for the
// mask / alpha-blend casters: discard below the material cutoff (0.5 for blend) -- THE alpha-test
// rule (#1044, clean_alpha_rule.glsl). Inputs match clean_draw.vert / clean_draw_rigged.vert.
#include "clean_alpha_rule.glsl"
layout(location = 0) flat in uint v_material_id;
layout(location = 1)      in vec2 v_uv;
layout(location = 3) flat in vec4 v_tint;
layout(location = 4) flat in uint v_flags;

// 96 bytes — matches LLCleanMaterialStore::Material (see clean_draw_common.glsl).
struct Material {
    uvec2 base_color_handle;
    uvec2 normal_handle;
    uvec2 mr_handle;
    uvec2 emissive_handle;
    vec4  base_color_factor;
    vec4  emissive_factor;
    vec4  spec_color_gloss;
    float metallic_factor;
    float roughness_factor;
    float alpha_cutoff;
    uint  flags;
};
layout(std430, set = 0, binding = 1) readonly buffer Materials { Material materials[]; };
layout(set = 1, binding = 0) uniform sampler2D uTextures[];
// #1159 WEATHER OCCLUSION MAP twin (constant 0 TRUE: mWeatherBlendPipe, recordWeatherOcclusion's
// alpha-blend bucket only): an alpha-blend caster is a roof from the weather's own low cutoff --
// g_draw_push.pad1, float bits (RenderVKWeatherGlassAlphaCutoff) -- instead of the binary
// question's 0.5, so tinted / clear glass keeps the rain out while an alpha-0 prim stays open.
// Mask materials keep their own cutoff (foliage gaps stay open). FALSE (every sun / projector /
// cluster shadow pipeline): the test below is exactly THE rule, the push is never read.
layout(constant_id = 0) const bool kWeatherOccl = false;
// (the world draw push, clean_draw_view.glsl DrawPush: same 16 B range, VS | FS)
layout(push_constant) uniform DrawPush { uint view; uint f8; uint pad0; uint pad1; } g_draw_push;

void main()
{
    // Shared cluster rig lists still carry retained rows: shadow-map casters pop at death.
    if ((v_flags & 0x00400000u) != 0u) discard;
    // Cull-none pipeline for alpha / double-sided casters: single-sided alpha
    // casters still drop their back faces, as LL's culled shadow pass does.
    if (!gl_FrontFacing && (v_flags & 256u) == 0u) discard;
    // <GMDX> REZ EFFECTS (CombatFX; ported from GunMetal-DX, squeedledorf, LGPL 2.1, v0.3.0 cf5f1f4):
    // an opaque rigged row the main view routed to the mask buckets for a build effect (slot bit 31,
    // not dying, natively neither blend nor mask; clean_cull.comp) casts solid, as from its own
    // opaque bucket. With avatar shadow detail 0 the renderer casts the rigged mask buckets for
    // these rows alone (push pad1 bit 31; never on the weather twin, whose pad1 is its cutoff):
    // everything else in them stays out.
    const bool rzBuild = (v_flags & 0x80400006u) == 0x80000000u;
    if (!kWeatherOccl && (g_draw_push.pad1 & 0x80000000u) != 0u && !rzBuild) discard;
    if (rzBuild) return;
    Material m = materials[v_material_id];
    float a = m.base_color_factor.a * v_tint.a;
    if ((m.flags & 1u) != 0u)
        a *= texture(uTextures[nonuniformEXT(m.base_color_handle.x)], v_uv).a;
    // THE rule (clean_alpha_rule.glsl): mask materials use their cutoff, alpha-blend casters 0.5; no
    // coverage scale (a shadow caster tests its texel's own coverage). (#1044: the old max(.., 0.05)
    // floor cut faint mask texels -- and every texel of a cutoff-0 mask -- that the raster draws and the
    // RT shadow rays block.) Deliberately NOT LL's screen-X stipple for alpha < 0.88: at grazing sun
    // angles it aliases into vertical stripes across the shadow.
    float cut = alphaRuleCutoff(m.alpha_cutoff);
    if (kWeatherOccl && m.alpha_cutoff < 0.0) cut = uintBitsToFloat(g_draw_push.pad1);   // (#1159)
    if (!alphaKeeps(a, cut, 0.0, 0.0)) discard;
}
