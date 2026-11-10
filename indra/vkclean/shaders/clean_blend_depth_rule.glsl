// vkclean — THE NEAR-OPAQUE RULE for alpha-blend texels (RenderVKAlphaDepthPrepass,
// RenderVKBlendOpaqueGBuffer). One function decides, for every pass that asks, whether a
// blend fragment counts as opaque this frame:
//   - the alpha-blend depth pre-pass (clean_prepass_blend.frag; the depth + normal / + motion /
//     motion-only flavours of clean_prepass_normal.frag and clean_prepass_motion.frag,
//     PREPASS_BLEND) writes its depth (+ the attachment set in force);
//   - the G-buffer's blend draws (clean_draw.frag CLEAN_GBUFFER_BLEND, EQUAL after it) take
//     it, so the deferred lighting lights it as any opaque surface: AO, the denoised shadow
//     masks, SSR, GI, motion vectors -- a blended mesh body looks like a masked one;
//   - pass 2's lit FS (the push word's BLEND_GBUF_PUSH bit, only on frames whose G-buffer took
//     the lists) skips it before any lighting, with a margin over the threshold: a texel the
//     G-buffer rounded just under 0.99 is still shaded and blended here (over itself at worst),
//     never left as a hole. The soft fringe below the threshold blends exactly as before.
// Not a rule of its own anywhere else: the three includers keep no copy of the threshold.
// Includers provide ssr (clean_frame_info.glsl: Highlight Transparent, alphaKeeps) and the
// rez-fx helpers (gmdx_rezfx.glsl GMDX_REZFX_FS: rezfxFadeOpacity, REZFX_DYING) first.
//
// AVATAR DENSE LAYER (RenderVKAvatarAlphaDepthThreshold, LLCleanRendererVK kPushAvDepthPre): the same
// rule at the avatar threshold T writes an avatar's dense blend texels into the AVATAR depth
// (mAvDepth: a copy of pass 1's final depth + these texels), which only the avatars' own blend draws
// test: per pixel only the nearest dense surface of an avatar shades and blends, over the opaque
// scene behind it (the avatar's own opaque body / face included -- they are in the copy, lit
// underneath, and blend-under exactly as today) and over the world's blend layers (which keep the
// main depth). Never the main depth: nothing that lights or reads it sees a ghost as opaque.
// T lives in ssr.history_time.y (-1: off this frame). The rule only REMOVES blend-over-blend within
// an avatar (and, through the one shared target, behind another avatar's dense surface).
#ifndef CLEAN_BLEND_DEPTH_RULE_GLSL
#define CLEAN_BLEND_DEPTH_RULE_GLSL
const float kBlendDepthAlpha = 0.99;    // faded alpha at or above this: near-opaque
const float kBlendSkipMargin = 0.005;   // pass 2 skips only this far above it (rounding guard)
const float kBlendSkipAlpha  = kBlendDepthAlpha + kBlendSkipMargin;   // pass 2's skip threshold
// Draw push pad0: this frame's G-buffer took the alpha lists. Bit 31: clear of the VISR_* bits
// (0-2) and of the cluster path's view index, which its draws carry in the same word.
const uint  BLEND_GBUF_PUSH  = 0x80000000u;
// The avatar alpha depth is live this recording (mAvDepth built; its readers test avatar faces
// against it), and its threshold.
bool  avatarDepthLive()      { return ssr.history_time.y > 0.0; }
float avatarDepthThreshold() { return ssr.history_time.y; }
// base_a: the face's alpha product (material factor x tint x texel, the lit FS's evaluation
// order); alpha_cutoff: the material's (< 0 = none); flags: the instance flags; threshold: the
// faded alpha a dense texel reaches -- kBlendDepthAlpha for the passes that write the near-opaque
// texels, kBlendSkipAlpha for pass 2's skip, avatarDepthThreshold() for the avatar pre-pass.
// The pre-pass flavours may discard a single-sided back face before sampling (cheaper); it is
// still this rule that says so, so pass 2 agrees on a rez-fx cut surface's back (rezfxBack).
bool blendDepthKeeps(float base_a, float alpha_cutoff, uint flags, float threshold)
{
    if (!gl_FrontFacing && (flags & (256u | 8u)) == 0u) return false;    // single-sided back face (the lit FS may still draw a rez-fx cut)
    if ((flags & 65536u) != 0u) return false;                              // water exclusion surface: never a surface (the lit FS drops it too)
    if (ssr.ao_params2.z > 0.5) return false;                              // Highlight Transparent: everything shows through
    if ((flags & REZFX_DYING) != 0u) return false;                         // <GMDX> dying (rez effects): blends only
    if (base_a * rezfxFadeOpacity(flags) < threshold) return false;
    // the lit FS's cutoff (blend faces may carry one): THE rule unscaled (clean_alpha_rule.glsl -- a
    // strict subset of what the lit FS keeps: the coverage scale only raises the tested alpha)
    return alpha_cutoff < 0.0 || alphaKeeps(base_a, alpha_cutoff, 0.0, 0.0);
}
#endif
