#version 460
#extension GL_EXT_nonuniform_qualifier : require
// vkclean — ALPHA-BLEND DEPTH PASS (RenderVKAlphaDepthPrepass). After the lit opaque
// pass (pass 1, depth still writable), the alpha-blend buckets write depth where they
// are NEAR-OPAQUE (alpha >= kOpaqueAlpha): dense foliage stacks 4-10 blended layers
// per pixel, and every layer behind a solid leaf texel was shaded and blended for
// < 1% visible contribution. With the leaf in depth those fragments fail the early
// depth test in pass 2; the leaf itself draws there with LESS_OR_EQUAL (same VS
// modules, invariant gl_Position). Soft edges (alpha below the threshold) write
// nothing and blend exactly as before. Highlight Transparent writes nothing (everything
// stays visible through). THE near-opaque rule is clean_blend_depth_rule.glsl's, shared with
// the depth + normal / + motion flavours (clean_prepass_normal.frag / clean_prepass_motion.frag
// PREPASS_BLEND), the G-buffer's blend draws and pass 2's skip (RenderVKBlendOpaqueGBuffer: the
// lists draw in the DEPTH PRE-PASS, before AO / the RT shadow mask / the lighting, which then
// light the near-opaque texels as opaque surfaces). Without it this FS runs at the end of
// pass 1, after everything that reads the opaque depth, so none of those see the foliage.
#extension GL_GOOGLE_include_directive : require
#include "clean_frame_info.glsl"   // ssr.ao_params2.z: Highlight Transparent
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
// <GMDX> rez effects: a build effect's hidden part writes no depth (as the lit FS cuts it)
#define GMDX_REZFX_FS
#include "gmdx_rezfx.glsl"
#include "clean_blend_depth_rule.glsl"   // THE near-opaque rule (blendDepthKeeps)
// AVATAR ALPHA DEPTH (clean_blend_depth_rule.glsl AVATAR DENSE LAYER): pad0 AVD_PREPASS_PUSH = this
// draw writes the AVATAR depth (mAvDepth) -- avatar faces only, at the avatar threshold T -- not the
// main depth's near-opaque texels. (The VS already collapsed the non-avatar instances.)
#include "clean_draw_view.glsl"         // g_draw_push.pad0

void main()
{
    const bool avPre = (g_draw_push.pad0 & AVD_PREPASS_PUSH) != 0u;
    if (avPre && (!avatarDepthFace(v_flags) || !avatarDepthLive())) discard;
    if ((v_flags & 2048u) != 0u) discard;                              // water
    if (!gl_FrontFacing && (v_flags & (256u | 8u)) == 0u) discard;     // single-sided back face (= the rule's, before the sample)
    rezfxDiscard(v_flags);                                             // <GMDX> a build effect's hidden part
    Material m = materials[v_material_id];
    precise vec4 base = m.base_color_factor * v_tint;   // same evaluation order as the lit FS
    if ((m.flags & 1u) != 0u)
        base *= texture(uTextures[nonuniformEXT(m.base_color_handle.x)], v_uv, taaLodBias(v_flags));   // + the lit FS's TAA mip bias
    // Highlight Transparent / dying / faded alpha / cutoff -- at the avatar threshold in the avatar pre-pass
    if (!blendDepthKeeps(base.a, m.alpha_cutoff, v_flags, avPre ? avatarDepthThreshold() : kBlendDepthAlpha)) discard;
}
