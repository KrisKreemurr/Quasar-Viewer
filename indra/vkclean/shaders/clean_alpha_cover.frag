#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
// vkclean -- NEAR-OPAQUE COVER PASS (#1147; clean_alpha_k.glsl NEAR-OPAQUE COVERS has the
// contract, LLCleanAlphaKVK / recordWorld record it). Pass 2's alpha lists in pass 2's order into
// an R32_UINT target (one per water side), hardware depth test against the opaque surface (strict:
// in front), no blending: the rasterization order leaves the LAST cover's key at each pixel. A
// cover is a fragment pass 2 draws with alpha >= AK_COVER_A: every lit-FS discard is repeated (or
// more), and the alpha is computed as the list pass does (same VS twins, same shading rate), so it
// is the alpha pass 2 composites -- glare / Highlight Transparent only raise that.
#include "clean_frame_info.glsl"   // ssr: Highlight Transparent, the TAA mip bias
#include "clean_draw_view.glsl"    // g_draw_push.pad0
#include "clean_alpha_k.glsl"
layout(early_fragment_tests) in;   // the depth test before the FS (no depth write)

layout(location = 0) flat in uint v_material_id;
layout(location = 1)      in vec2 v_uv;
layout(location = 3) flat in vec4 v_tint;
layout(location = 4) flat in uint v_flags;
layout(location = 18) flat in uint v_akdraw;

layout(location = 0) out uint outKey;

// 96 bytes -- matches LLCleanMaterialStore::Material (see clean_draw_common.glsl).
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
#define GMDX_REZFX_FS
#include "gmdx_rezfx.glsl"   // <GMDX> the de-rez cut and fade, as the lit FS
#include "clean_blend_depth_rule.glsl"   // #1420 THE near-opaque rule (blendDepthKeeps): pass 2's skip

void main()
{
    if (ssr.ao_params2.z > 0.5) discard;                               // Highlight Transparent
    // The lit FS's discards (clean_draw.frag decodeSurface), or more.
    if ((v_flags & (2048u | 65536u | 0x00400000u)) != 0u) discard;   // water, invisiprim, dying
    if (!gl_FrontFacing && (v_flags & (256u | 8u)) == 0u) discard;    // single-sided back face
    rezfxDiscard(v_flags);
    const Material m = materials[v_material_id];
    vec4 base = m.base_color_factor * v_tint;
    if ((m.flags & 1u) != 0u)
        base *= texture(uTextures[nonuniformEXT(m.base_color_handle.x)], v_uv, taaLodBias(v_flags));
    if (m.alpha_cutoff >= 0.0 && base.a < m.alpha_cutoff) discard;
    // #1420 (RenderVKBlendOpaqueGBuffer): a texel the G-buffer took (>= the rule's 0.99 + margin) is
    // not drawn by pass 2 -- it is opaque in the depth and lit deferred -- so it is no cover: the
    // covers keep the band pass 2 still blends (0.98 up to the rule's skip).
    if ((g_draw_push.pad0 & BLEND_GBUF_PUSH) != 0u && blendDepthKeeps(base.a, m.alpha_cutoff, v_flags, kBlendSkipAlpha)) discard;
    // (a hair below: the lit FS may associate the product differently)
    if (base.a * rezfxFadeOpacity(v_flags) < AK_COVER_A + 1e-5) discard;
    outKey = akCoverKey(g_draw_push.pad0, v_akdraw, uint(gl_PrimitiveID), gl_FrontFacing);
}
