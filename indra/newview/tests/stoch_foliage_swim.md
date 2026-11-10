# Stochastic shadow sample-grid repair

Base: forgejo/main b7034811b494623f5198346d48940084bd33e364.
Branch: stoch-foliage-swim. No viewer or GPU run performed.

## Proven defect and repair

The reduced-resolution producer selects ONE truncated render pixel, not a
minimum-depth or averaged receiver. With render/denoiser ratio r, texel k and
sample phase J, its raster centre is floor((k + .5 + J) * r) + .5.
Temporal reprojection instead addressed a continuous, unjittered grid and did
not carry the previous raster jitter. This can address a neighbouring receiver,
particularly on thin cut-out geometry. The separate RT sun-mask denoiser
already accounts for this distinction.

Carry the previous successful temporal dispatch's full-precision raster jitter
in unused temporal push words; invert the actual discrete sample lattice for
camera, object-motion and virtual-specular reprojection. Use the same inversion
for current local and merged-sun upsampling. Existing geometry rejection,
variance/clamping, alpha tests, ray counts and light selection are unchanged.
No new images, descriptors, rays or allocation; push size stays 96 bytes.
GI opts out. Mode changes invalidate shadow history for a clean comparison.

Full resolution with jitter also lacked previous raster phase. Full resolution
without jitter is unchanged. Half-resolution local radiance without jitter
already had an exact-grid correction; do not attribute that configuration's
floor noise to this defect. Merged-sun upsampling lacked the correction.

## What remains inferred

The coordinate error is proven; eliminating the owner's grass swimming is not
visually verified. RT MASK uses deterministic cutoff for fixed ray/geometry/UV/
footprint; there is no separate random cutoff to remove. Changing receiver or
ray inputs can still change alpha visibility.

The owner confirmed that the point-light floor noise is direct stochastic
shadows and GI has no effect. A shared cause is plausible with jitter enabled,
but NOT proven without the native AA state and a visual test. The default
radiance local-shadow path retains its own denoiser with DLSS RR; GI spatial
timers do not establish that local denoising was bypassed. No blind history
length, rejection-tolerance or sample-count change is included.

## Owner A/B

Keep RenderVKVisQueue 0. Compare RenderVKShadowSampleGridHistory 0/1 (Persist 0),
allowing five seconds after a toggle. Compare RenderVKDenoiseScale .5 and 1 on
still grass and a plain point-lit floor, native first. Repeat with jitter on
and off; the existing receiver-route log now prints sampleGrid and jitter.
RenderVKRTShadowMaskScale controls the separate mask, not this local denoiser.

RenderVKDenoiseDebug 11 shows local-history causes: red = no matched receiver
history, green = light-reference cut or changed-light reset, blue = neighbourhood
colour clamp. Combinations add colours; pixels without a light reference are
black. Normal temporal colour and spatial feedback still run; causes travel in
unused compatibility metadata bits and are displayed only at the final spatial
output. This diagnostic does not describe merged-sun history. Debug 2 shows
history age and 3 variance. Restore debug 0 for the quality comparison.

If a jitter-off native floor still fails to converge, capture debug 11 and the
receiver-route line: that is a separate remaining investigation, not evidence
that the sample-grid repair fixes all stochastic variance.

## Validation

- Extracted production GLSL inverse: 2,549,376 receiver/phase cases; maximum
  inversion error 0 texels. Old mapping mean drift 0.180555 texels.
- Actual recordTemporal method fixture: previous phase publication, toggle
  invalidation, unready retention and GI isolation pass.
- Source-bound checks cover producer/consumer flag agreement, both upsamplers,
  temporal push reuse and diagnostic preservation of normal colour/feedback.
- Real Linux -c objects with project flags and -Werror: llcleandenoisevk,
  llcleanprojstochvk and llcleanrenderervk pass. Outputs are owned-worktree only.
- 22 affected/required shader variants compiled and passed spirv-val; latest two
  denoiser shaders recompiled and validated after the diagnostic change.
- XML parsing and git diff --check pass. No generated SPIR-V headers committed.

Run the CPU checks:
python3 indra/newview/tests/shadow_sample_grid_checks.py "$PWD" "$PWD/.validation/sample-grid"

GPU cost and visual convergence are unmeasured. This is ready for owner A/B,
not a claim that both reported scenes are visually fixed.
