# Local stochastic specular recovery

Owner result: 68780f7121 did NOT fix the reported smear/trailing. Keep this as
source-audit history; shadow_exact_specular.md describes the new signal split.

Branch: stoch-foliage-swim, follow-up to fc1b8292d8.
Target: radiance mode (RenderVKReSTIRRadiance 1), including DLSS RR.

## Root-cause audit

The owner's sample-grid A/B looked identical: fc1b8292d8 is not implicated in
this older highlight defect. Its independent A/B remains available.

Proven from source:

* SHADE estimates diffuse and specular separately. Radiance divides specular by
  the same F0/specular colour that final lighting reapplies. It does NOT divide
  out the BRDF lobe; that lobe remains in the filtered signal. This is not a
  diffuse-albedo divisor bug or a second specular-ratio multiplication.
* Radiance local shadows explicitly retain their own denoiser with RR on
  (rrRawSignal rejects kRrRawProj when mPdnRadiance). RR bypassing a GI channel
  does not bypass this one.
* Specular already has a roughness/hit-distance iteration limit and a short
  roughness-dependent history, plus virtual reprojection for smooth lobes.
* However, temporal young-history initialization could replace even a mirror's
  current specular with the same broad 3x3 mean used for diffuse. Those neighbours
  had only the common depth/normal test, without roughness/lobe support.
* Spatial young-history recovery relaxed specular's normal exponent toward 8
  and doubled step sizes. The specular iteration limit alone did not cap the
  resulting tap distances. First-iteration spatial colour also feeds young
  radiance history, allowing spread to survive into later frames.
* If a sharp virtual-history lookup failed, the code silently retained the
  surface-reprojected specular colour AND its age. A surface tap cannot follow
  the moving highlight merely because its depth/normal match.

These source paths can spread a narrow signal and preserve an incorrectly
reprojected highlight. Their responsibility for all owner-visible mud/trailing
is inferred, not GPU/visual proof.

Specular BRDF evaluation can be cheap, but its visibility is not free: omitting
occlusion makes a light's highlight shine through a wall. Radiance mode includes
shadowed specular in the estimator for that reason. Ratio mode instead applies
its filtered ratio per light; a shared ratio can couple unrelated lights, but
that is not the confirmed owner's current radiance route.

## Repair

RenderVKShadowSpecularLobeGuard (Boolean, Persist 0, default 1) changes ONLY
local radiance, not GI or ratio mode:

* Temporal specular neighbourhoods obey the existing lobe-radius heuristic,
  a matching roughness stop and the existing sharp specular normal exponent.
  Diffuse retains its existing neighbourhood and young-history recovery.
* Specular spatial recovery keeps its sharp normal exponent and rejects taps
  outside the roughness-derived radius even when young diffuse doubles the
  common step. No new neighbourhood texture fetch is added: the temporal guide
  is reused. Spatial rejection can avoid specular input reads.
* Failed virtual reprojection reduces fallback surface-history age by 1-kv,
  where kv is the existing virtual weight. A mirror (kv=1) takes the current
  specular estimate; rough lobes smoothly retain their existing surface history.
* Toggling the guard invalidates history for an uncontaminated comparison.

No extra ray, image, allocation, descriptor, dispatch or push bytes. Work remains
fixed: the existing loops only. Arithmetic cost is unmeasured. The spatial
radius uses the conservative zero-hit-distance upper bound because spatial GEOM
has no hit distance; the temporal test uses the actual hit distance. Existing
iteration selection still uses that actual distance and is numerically unchanged.

## Preferred exact-dominant split: NOT implemented

This repair does not provide an exact-dominant-specular pass. A correct version
must select a consistent light identity, exclude that light's specular from the
stochastic estimator, carry identity through temporal/spatial filtering, and add
its analytically evaluated specular with its own visibility. Adding a fresh ray
only in final lighting would double-count the already accumulated contribution.
At half resolution, interpolating residuals that exclude different dominant
lights also requires explicit ownership handling. This needs a separate signal
contract; it is not safe to claim it follows from a final-lighting-only patch.
The present repair preserves all occlusion and never substitutes an unoccluded
analytic term. It may expose more sample noise when incorrect history is rejected.
If visual A/B still shows the issue, this bounded repair is not a complete fix.

## Owner A/B

Keep radiance 1, DLSS RR on, async compute off and all other settings unchanged.
Compare RenderVKShadowSpecularLobeGuard 0/1, allowing five seconds to refill
history. Use a glossy floor/metal object with a narrow point/projector highlight:
first hold the camera still, then pan laterally, stop, and inspect the wake.
Compare Half and Full RT-shadow/denoiser resolution. Both have the recovery bug;
Half makes each denoiser-texel footprint larger on the displayed image.
Keep RenderVKShadowSampleGridHistory fixed during this comparison. Debug 2
shows ages; debug 11 can identify history loss/clamping but is not a beauty view.
Restore RenderVKDenoiseDebug 0 for the visual comparison.

## Checks

Actual GLSL lobe-radius function and failed-virtual branch are extracted into a
strict CPU fixture: sharp impulse support, rough recovery, unchanged original
radius and bounded fallback age pass. Source checks cover flag publication in
both passes, iteration masking, radiance-only scope and transient setting.
The earlier 2,549,376 sample-grid regression cases still pass.
Two affected denoiser shaders compile and pass spirv-val. Real Linux project
-Werror object checks cover llcleandenoisevk, llcleanprojstochvk and the renderer TU. No generated
SPIR-V header is committed. No viewer/GPU run or measured performance claim.

Commands (owned output paths only):
python3 indra/newview/tests/shadow_specular_lobe_checks.py "$PWD" "$PWD/.validation/specular-lobe"
python3 indra/newview/tests/shadow_sample_grid_checks.py "$PWD" "$PWD/.validation/sample-grid"
