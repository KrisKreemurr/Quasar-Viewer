#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
// vkclean — DEPTH + MOTION PRE-PASS, no normals (RenderVKPrepassDepthOnlyDeferred, temporal
// frames that go DEFERRED): the G-buffer pass writes the normals the AO reads (G1), so pass
// 1a carries only the motion target (attachment 0, RGBA16F) and the no-history mask
// (attachment 1, R8). The motion / no-history values and the mask discards are
// clean_prepass_normal.frag's PREPASS_MOTION / PREPASS_MASK, verbatim (same convention:
// UV units, current minus previous, unjittered matrices, camera-relative; the EQUAL passes
// after it need the identical surviving fragment set).
//
// PREPASS_BLEND (RenderVKBlendOpaqueGBuffer): the alpha lists' near-opaque texels, the
// discards clean_prepass_blend.frag's (THE rule, clean_blend_depth_rule.glsl).
// PREPASS_MASK: the mask buckets (and every cluster draw); without it the opaque buckets
// (nothing discarded; still an FS: the motion output).
//
// Inputs: exactly the slim motion VS's outputs (clean_draw{,_rigged}.vert / cluster stages
// -DCLEAN_DEPTH_ONLY -DCLEAN_DEPTH_MOTION): 0 material, 1 uv, 3 tint, 4 flags, 5 world
// position, 10 previous world position, 20/21 previous normal/tangent. The full VS writes a superset (RenderVKDepthOnlyVS 0).
// Reuse the normal prepass's material/normal helpers and bindings. Its location0
// output is this pass's motion output; the depth/MV/discard expressions stay shared.
#define outColor outMotion
#include "clean_draw_common.glsl"
#undef outColor
#include "clean_normal_tex.glsl"
layout(location = 10) in vec3 v_prev_world_pos;
layout(location = 19) flat in uint v_receiver_class;
#include "clean_receiver_motion.glsl"
#include "clean_receiver_normal.glsl"

layout(location = 1) out float outNoTemporal;

#if defined(PREPASS_MASK) || defined(PREPASS_BLEND)
// <GMDX> rez effects (CombatFX): the de-rez cut of a dying object, as the lit FS runs it
#define GMDX_REZFX_FS
#include "gmdx_rezfx.glsl"
#endif
#ifdef PREPASS_BLEND
#include "clean_blend_depth_rule.glsl"   // THE near-opaque rule (blendDepthKeeps)
#endif

void main()
{
#ifdef PREPASS_MASK
    // clean_prepass_mask.frag's discards (= the lit FS's), verbatim.
    if ((v_flags & 2048u) != 0u) discard;                              // water
    if (!gl_FrontFacing && (v_flags & (256u | 8u)) == 0u && !rezfxBack(v_flags)) discard;   // single-sided back face (<GMDX> but the printer's cut surface)
    if ((v_flags & 32u) == 0u)                                         // terrain: alpha 1
    {
        const Material m = materials[v_material_id];
        precise vec4 base = m.base_color_factor * v_tint;   // same evaluation order as the lit FS
        float a_lod = 0.0;
        if ((m.flags & 1u) != 0u)
        {
            base *= texture(uTextures[nonuniformEXT(m.base_color_handle.x)], v_uv, taaLodBias(v_flags));   // + the lit FS's TAA mip bias
            a_lod = textureQueryLod(uTextures[nonuniformEXT(m.base_color_handle.x)], v_uv).y + taaLodBias(v_flags);
        }
        rezfxDiscard(v_flags);   // <GMDX> the de-rez cut (= the lit FS's), after the implicit-LOD fetch
        if (ssr.ao_params2.z <= 0.5 && m.alpha_cutoff >= 0.0 && !rezfxRerouted(v_flags) && maskTestAlpha(base.a, a_lod, m.alpha_cutoff) < m.alpha_cutoff)   // Highlight Transparent keeps cutouts; <GMDX> rerouted opaque rows never
            discard;
    }
#elif defined(PREPASS_BLEND)
    // The alpha lists' NEAR-OPAQUE texels (RenderVKBlendOpaqueGBuffer): clean_prepass_blend.frag's
    // discards, verbatim -- THE near-opaque rule decides (clean_blend_depth_rule.glsl).
    if ((v_flags & 2048u) != 0u) discard;                              // water
    if (!gl_FrontFacing && (v_flags & (256u | 8u)) == 0u) discard;     // single-sided back face (= the rule's, before the sample)
    rezfxDiscard(v_flags);                                             // <GMDX> a build effect's hidden part
    {
        const Material m = materials[v_material_id];
        precise vec4 base = m.base_color_factor * v_tint;   // same evaluation order as the lit FS
        if ((m.flags & 1u) != 0u)
            base *= texture(uTextures[nonuniformEXT(m.base_color_handle.x)], v_uv, taaLodBias(v_flags));   // + the lit FS's TAA mip bias
        if (!blendDepthKeeps(base.a, m.alpha_cutoff, v_flags, kBlendDepthAlpha)) discard;
    }
#endif
    // Camera-relative (#555): both matrices map (P - cam_origin).
    const vec4 cur  = ssr.views[CLEAN_VIEW].taa_cur_vp  * vec4(v_world_pos - ssr.cam_origin.xyz, 1.0);
    const vec4 prev = ssr.views[CLEAN_VIEW].taa_prev_vp * vec4(v_prev_world_pos - ssr.cam_origin.xyz, 1.0);
    // Behind last frame's eye (w <= 0: no valid projection) -> 0; TAA rejects it anyway.
    outMotion = receiverMotionNormal(receiverMotionPayload(cur, prev, v_receiver_class), receiverPreviousShadingNormal(materials[v_material_id], prev));
    outNoTemporal = noTemporalFace(v_flags) ? 1.0 : 0.0;
}
