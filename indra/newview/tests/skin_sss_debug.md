# Skin SSS export/gather diagnostics

Second item after shadow-caster history. Base760157d5bc. No viewer/GPU runs.

RenderVKDebugView31 = white skin mask, black other G-buffer surfaces.
32 = abs(diffuse colour * gathered irradiance difference) *16, black other surfaces.
33 = exported centre irradiance S0 as linear HDR, black other surfaces.
Existing setting is Persist0; no new setting. Debug modes are carried in unused
SSS push high bits, not the lit shader's debug specialization. The normal async
split passes3/7/8 keep running: exclude31..33 from the lit/debug-twin route and
from frame-info shading-term overrides. Otherwise asking for the diagnostic
would disable split lighting and the SSS pass being diagnosed.

Debug modes request the existing skin export/image even with SSS disabled or
scale0. Mask and S0 work at scale0; delta is black at scale0. Normal view0 keeps
the old zero-scale early-out. Stereo, unready pipeline, placeholder and extent
gates remain. No extra allocation beyond the already budgeted skin irradiance
image, no extra pass, no GPU readback. The feed previously excluded ALL blend materials from skin marking even though
RenderVKBlendOpaqueGBuffer (default on) routes near-opaque blend texels through
the G-buffer. This drops skin on those BoM texels before export. Now retain the
material skin tag for blended BoM surfaces: only promoted opaque texels scatter;
translucent edges still draw forward. HUD/fullbright exclusions remain. Cel
variant priority remains unchanged. An allocated image alone never proved the
visible body pixels were marked skin.

## Source audit

- DEFER_SSS_EXPORT covers3(COMBINE),7(BASE),8(LOCAL).
- Their g_sss_want is set before shading from s.skin and sssFrameOn.
- BASE's CLEAN_LOCAL_EXTERNAL zero local sum is intentional. The stochastic
  local radiance is carried by deferEmitterLight in lightSurface's tail;
  g_sss_tail adds dRad before return. Deterministic eligible LOCAL diffuse is
  read-added to the same packed image using the local gain/overlay keep.
- The pass17 memory dependency covers compute storage writes from those passes,
  including the async route. It reads S0 and writes only its own HDR pixel.
- RR guide generation copies scene colour/pre-exposure; the G-buffer diffuse
  albedo is a guide, not a viewer-side replacement of scattered lighting. No
  subsequent explicit albedo divide cancels the SSS change. RR network behaviour
  itself is not provable from this source audit.
- testctl snapshot defaults to main live capture, but an offscreen fallback
  disables split lighting and therefore SSS. Check live-snapshot vs offscreen
  messages. An offscreen image cannot establish whether this live-only pass
  changed the body. No snapshot pathway changed here.

## What remains unknown

The promoted-blend missing-skin flag is a proven feed defect and a concrete cause
of absent SSS on a common BoM body path. Whether it explains this owner's exact
body is not visually established. Do not label the previous lobe integration
repair as visually confirmed. The image being allocated once proves liveness, not per-pixel
export magnitude or support. We need31/32/33 in the owner's actual body/light,
preferably first RR off, then RR on, at scale4 and fixed camera/exposure.

Interpretation:
-31 black body: the actual visible faces are not marked skin (or split/pipeline
  gates are not admitted); inspect whether the visible faces are cel/fully translucent.
  Promoted near-opaque BoM blend texels should now be white.
-31 white,33 black: no exported diffuse light here,
  or export/feed is empty. Inspect exact model/material and lighting route.
-33 lit but32 black away from flat irradiance: gather support/radius/scene
  contains no usable gradient; record body position/depth and actual shadow edge.
-32 nonzero RR off but erased with RR on: reconstruction/guide routing is the
  next owner-visible evidence. In a flat irradiance field32 SHOULD be black.

Wait several frames after changing debug modes; RR initially holds prior normal
scene colour. Use live capture, not an offscreen fallback. Restore DebugView0
before judging the real appearance. Mask/S0 diagnostics deliberately override
world colour and are not physical lighting output.

## Validation

skin_sss_debug_checks.py extracts and compiles actual skinSssWanted and
recordSkinSss with a CPU command recorder: modes31..33, disabled-SSS force,
zero-scale dispatch,4x scale encoding, original push restoration, stereo and
unready-image gates. Shader-channel/normal-split routing and XML checks PASS.
Existing skin profile fixture PASS. No viewer run or visual quality claim.

Final validation: full generator PASS441 world+18FSR, all459 SPIR-V modules pass
spirv-val Vulkan1.3 scalar layout. Real project common flags, own current sources
and freshly generated headers: llcleanrenderervk.cpp and llvovolume.cpp syntax-only
with-Werror PASS (existing project third-party warning exceptions retained).
Production-host command-recorder fixture and skin profile fixture PASS; XML parse
and diffcheck PASS. No generated headers committed; no viewer build or GPU run.
