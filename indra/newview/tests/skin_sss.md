# Skin SSS integration repair

Source-only branch skin-sss-fix from mega-1106 a92a0971db. No viewer/GPU runs.

## Evidence and diagnosis

A normalized gather of constant packed irradiance must give that same irradiance.
Therefore a mismatch in k12, diffuse colour or affine tail alone cannot explain a
constant-field loss: E-S0 cancels regardless of export gain. Export/remodulation
use the same G0-linear albedo and PBR diffuse factor; RGB9E5 is round-to-nearest.
No change to those contracts is justified by this reproduction.

There IS a proven loss case in the old integration: resolved lobes start with
zero centre mass, reject all non-skin/unsupported taps, then acc/max(wsum,1e-12)
returns zero. Subtracting S0 removes their diffuse light. The fixture compiles the
actual production normalization helper and tests this old failure/new retention.
It does not establish that this exact case accounted for every interior pixel
in the owner's photograph. A softly-lit crop is not a constant irradiance field;
we cannot attribute the reported uniform4% without the actual irradiance data.

The old one-disc16-tap quadrature also couples the measured Gaussian weights to
sampling density, rounding and radius truncation. In an ideal-plane fixture at
profile-space .05mm/pixel with the48px cap, the broadest red Gaussian integrates
to .321956 of its intended mass; narrower lobes are close to1. Normalizing their
SUM hides the missing mass by redistributing the channel toward the narrower
lobes, weakening the red spread. This example is a near-field/capped case, not
an assertion of the owner's exact pixel size.

mmpp itself is correct: world-distance-per-RENDER-pixel *1000/scale. Scale4 gives
four times the world radius, until the existing48 render-pixel cap. For example
1mm per render pixel gives radius8px at scale1 and32px at scale4. Output/DLSS
resolution is not substituted into that conversion. Red3sigma is8.17mm at1,
32.68mm at4. The new outer radial stratum samples that small tail explicitly.

## Change

Four importance strata per Gaussian, at most24 taps instead of16, preserving the
six measured per-channel lobe weights independently. Each lobe accumulates the
DIFFERENCE from the packed centre and normalizes only its own compatible support;
missing support retains its centre. Flat packed irradiance is an exact no-op,
including clipped silhouettes. The pixel-radius cap and skin-only support remain.
World-distance/proposal correction suppresses depth jumps; fixed loop counts,
no images/allocations/dispatches/settings added. Spatial rotation remains; remove
the per-frame rotation so static skin does not acquire SSS sampling noise that
an upscaler must continually integrate. At most24 depth/G2/irradiance taps vs16;
no measured GPU cost claim. The old16-tap loop evaluated up to96 Gaussian exps,
while the new loop evaluates24 importance corrections plus24 radial logs.

Diffuse-only delta is added to existing HDR; specular/emissive and image alpha
remain untouched. RR guides use G-buffer albedo and copy the resulting scene
colour with exposure: there is no viewer-side post-RR albedo division that undoes
SSS. The closed network's reconstruction cannot be established by source alone;
compare RR off/on. No modification to RR inputs is warranted by this fixture.

## Checks

- skin_sss_checks.py compiles the actual GLSL helper as a strict C++ fixture:
  zero support, tiny weights, constant deltas, nonzero normalization PASS.
- Ideal-plane illumination step: .5mm into shadow at scale1 gives added irradiance
  R/G/B=.24607/.04803/.01439;2mm into shadow at scale4 gives
  .22582/.03627/.00780. Unit channel/lobe mass and24-tap bound PASS.
  These are ideal-plane CPU values, not a rendered image or performance claim.
- Actual generator-manifest flags: Combine, Base, Local, SkinSss compile with
  glslc Vulkan1.3 -O -Werror and spirv-val scalar layout PASS. SkinSss was recompiled
  and revalidated after the final radial-strata change.
- No C++ TU changed. No generated SPIR-V headers committed.

Run:
python3 indra/newview/tests/skin_sss_checks.py "$PWD" "$PWD/.validation/skin-sss"

## Owner check

Keep camera, lighting and exposure fixed; inspect a strong shadow edge across
BoM skin, not only a uniformly lit interior. Compare RenderVKSkinSSSScale0/1/4
with RenderVKSkinSSS1; wait for history after changes. Expect red spread into the
shadow, narrower green/blue, with stable texture detail and unchanged flat light.
Repeat RR off/on and close/mid distance. If every interior still loses4%, capture
irradiance at the actual face: the fixture does not prove that separate symptom
eliminated. Source correction READY; visual sign-off remains with the owner.
