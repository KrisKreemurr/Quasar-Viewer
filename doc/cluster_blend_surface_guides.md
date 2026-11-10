# CLOD near-opaque blend surface parity

## Decisive MASK/BLEND A/B: early-surface route repaired

Restoring Alpha masking makes the owner gap disappear with CLOD on. Auditing that
route shows a concrete omission: CLOD blend records are excluded from normal
opaque buckets, but only the LEGACY blend lists were drawn into the early
near-opaque depth/normal/motion and G-buffer passes. CLOD blend depth was instead
written after deferred lighting. Consequently the same body surface has its own
early depth/material/motion guides as MASK or legacy BLEND, but background guides
as CLOD BLEND. This is a rendering/reconstruction mismatch, not evidence that a
second skeletal pose was intentionally chosen.

The new CLPASS_ALPHA_PREPASS and CLPASS_ALPHA_GBUFFER use the already ordered
alpha lists and EXACTLY the normal full cluster vertex/mesh stage, skin map and
welded SKIN_OUT. All four pre-pass attachment variants are provided. The same
blendDepthKeeps threshold/discards used by legacy near-opaque texels decide which
texels enter the early surface. Pre/post-water lists both participate. Readiness
is coupled: no new early depth is written unless the pre/G-buffer pair is ready.
Only after the G-buffer draws succeed does forward alpha skip its near-opaque
texels; the translucent fringe keeps its original blending. The duplicate late
depth pass stands down after an admitted early pre-pass. Unready pipelines or an
off/non-deferred/transparent-snapshot path keep the existing forward fallback.
Per-recording flags reset even when deferred allocation is refused.

The task stage masks the reserved high near-opaque skip bit out of its cluster
view index. RT forward still binds the SAME cluster descriptor at set3 (instead
of set2); it does not get a different skin output or pivot. No skin binding,
mesh/DAG/cache identity, weld rule, selected cut or alpha authored value changes.
No new setting, render target, staging buffer, per-face CPU walk or ray is added.
Additional raster work is bounded by the existing alpha-list cap; heavy forward
lighting is skipped for texels now lit through the deferred path. No GPU cost
measurement is claimed.

## Regression/evidence

`python3 indra/newview/tests/cluster_blend_surface_checks.py <external-output-dir>`
executes the production clusterSkinnedVertex POSITION prefix for two faces with
identical welded boundary positions, distinct selection/skin slots and MASK vs
BLEND flags. Projection results are bit-identical. It executes the production
alphaDrawReady method to prove the paired readiness contract for all four guide
variants, and checks the renderer's early passes, late-depth gate and high-bit
view/skip handoff. The old source fails the early-pass contract assertions.

Regenerated normal and RT task binaries, and normal and RT mesh binaries, are
word-identical after normalizing their seven cluster descriptor-set decorations
from3 to2. This independently rules out a different compiled deformation program;
it does not prove buffer contents or the owner's visual result on the GPU.

All464 active shader variants regenerate and pass SPIR-V validation. Four changed
C++ translation units pass actual-source/project-flag -Werror syntax checks;
existing selection and alpha fixtures pass. No generated headers are committed.
No viewer build/GPU run. Owner visual acceptance remains: keep the original BLEND
material, compare against MASK at identical pose, with native/no temporal, DLSS SR
and RR; select/deselect and toggle the read-only diagnostic. The near-opaque body
must retain its silhouette and motion while the soft alpha fringe stays soft.
If a raw pre-resolve vertex gap remains, that is NOT established as explained by
missing guides and requires further investigation rather than claiming it fixed.

This repair is independent of the optional skin diagnostic follow-ups.
