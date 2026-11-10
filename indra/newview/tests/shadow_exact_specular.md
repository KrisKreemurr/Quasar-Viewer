# Exact-dominant local specular

Branch stoch-foliage-swim; follows 68780f7121. The owner confirmed that the
sample-grid A/B does not change this older bug, the lobe-guard patch does not
fix it, and RR off still smears. This change extracts the dominant term instead
of trying to tune the same filtered radiance signal again.

## Signal contract

At each glossy denoiser texel, rank eligible point/projector lights in the
existing deterministic cluster list by their EXACT unshadowed specular term
(projDirectTerms, including material BRDF, attenuation and gobo). The list is
hard-capped at CLUSTER_CAP. Ties use stable light IDs. This is the maximum over
that bounded eligible candidate list, not a claim to rank every light in the
world or to know occlusion before tracing. Emitters remain stochastic.

Glossy means PBR perceptual roughness < 0.4, or legacy normalized Blinn exponent
gloss^2*368 >= 64. Legacy's existing 1-gloss proxy otherwise misclassifies a
narrow legacy highlight as rough. Cel shading is excluded.

Let d be the deterministic selected light, q the existing proposal and W its
existing RIS/ReSTIR weight. Ordinary samples keep their original proposal,
PDF, reservoir reuse, diffuse term and visibility ray. After the existing source
clamp, zero ONLY the selected light's specular integrand:

    diffuse sample:             W * D_i * V_i          (unchanged)
    residual specular sample:   W * S_i * V_i * [i != d]
    dedicated exact term:           S_d * V_d_center

The extra term has weight 1, not 1/q. The selected light is NOT removed from
proposal support: it still supplies diffuse in ordinary samples. There is no
MIS balance factor because these are disjoint integrands, not two overlapping
estimators of the same specular term. The original estimator's existing reuse /
firefly-clamp bias is not claimed to disappear. The deterministic dominant term
does not need its stochastic firefly clamp. Its shadow is a sharp centre ray;
soft-area penumbra averaging remains on diffuse and the residual, not on this
one selected specular term.

## Storage, filtering and resolve

Reuse the existing budgeted split set. Its last IN/OUT layer carries RAW RGB
of S_d*V_d divided by the SAME F0/legacy specular factor used for normal radiance.
Split GEOM carries the stable selected-light ID per parity. Temporal and every
a-trous iteration copy that RGB without accumulation, clamping or neighbourhood
filtering. No dominant colour enters HIST. Empty/sky texels carry zero, NOT the
old visibility-slot default of one.

Residual specular history, neighbourhood statistics and a-trous taps may borrow
only from texels excluding the SAME light ID. Diffuse still uses its original
history/stops. This matters when neighbouring glossy texels select different
lights: blindly filtering residuals and then adding only the centre's dominant
term drops/double-counts energy. Current temporal IDs come from the existing RAW
snapshot; previous IDs from the other parity, avoiding same-dispatch image races.

At full denoiser resolution, add the raw exact term directly to residual specular
in final lighting. At Half, residual and raw terms use the SAME existing four
bilinear/depth-aware upsample weights, then current full-resolution F0 remodulates
the sum. This is at most a 2x2 resolve of current samples, not a-trous or temporal
filtering. The analytic BRDF is evaluated at the denoiser's sampled receiver;
this is not a second full-raster BRDF pass. The finite half-grid resolve can still
limit sub-texel detail; Full is the control for that distinction.

## RR routing

Radiance DI retains its own denoiser even with RR on: rrRawSignal(kRrRawProj)
returns false in radiance mode. The exact term now bypasses OUR denoiser and
joins normal scene-linear lighting before the ordinary surface/atmosphere tail.
RR receives it in the normal pre-exposed scene colour (Eval.color), like other
analytic direct lighting. There is no separate direct-specular-radiance port in
this integration. clean_rr_guides derives specular albedo from material F0/BRDF;
its hit-distance guide remains RT REFLECTION distance, not direct-light distance.
The exact term is not fed to GI/RT-reflection raw-noise buffers or given their
noisy hit-distance guide. RR itself can still reconstruct the combined colour;
visual A/B with RR on and off is required, not a claim that RR can never filter it.

## Bounds and cost

RenderVKShadowExactSpecular: Boolean, Persist 0, default 1. Ratio mode is unchanged.
One extra hard visibility SAMPLE per selected glossy denoiser texel, zero elsewhere.
S.nRays is forced to 1, radius to zero and SH_SPLIT off (one mixed opaque/alpha/tint
walk rather than an additional translucent tail). No moved-light disc expansion.
The same hard sample is restored if the existing queue walker rebuilds a request.

Important distinction: one additional light-sample direction is NOT a guarantee
of one physical traversal. Existing receiver-normal/terminator/self-owner repairs
and point-light own-fixture re-traversals remain bounded but can issue auxiliary
queries. They have not been bypassed to hide cost or break occlusion. A strict
one-physical-query budget is not established by this patch; report that explicitly
when assessing the requested budget, and measure total kernel time.

No new image types, descriptor bindings, dispatches or push bytes. With the normal
SplitK>0 allocation, additional VRAM is zero. If SplitK=0, enabling this feature
requests the existing split set through its existing VRAM precheck/soft allocation:
+96 B/denoiser texel with sun, +112 without (~190/~221 MiB at 4K Half before DLSS).
Refusal keeps the existing bounded fallback/placeholder policy; actual split image
extent gates every use, so no absent layer is accessed and no contribution is zeroed
without storage for its exact counterpart. The once-per-route log prints exactSpecular.

The selection adds up to CLUSTER_CAP exact BRDF/gobo rankings per glossy texel.
It is not free; count SELECT, VIS, SHADE, temporal, spatial AND final lighting in
A/B totals. Register counts / driver spill measurements are pending GPU access:
vqb_run.sh refused because the owner's viewer was running. No estimates are
presented as measurements. No viewer/GPU run was performed for this change.

## Checks and remaining gate

- Production selector + ownership compiled in a strict CPU fixture: ranking,
  stable ties, legacy/PBR classification and 1,000 unequal-PDF partition cases PASS.
- Source checks cover residual ID stops, unfiltered raw RGB transport, sky zero,
  bounded extra slot/sample, fallback and transient setting.
- Earlier 2,549,376 sample-grid cases and specular-lobe regressions still PASS.
- 44 affected shader variants compile and pass spirv-val, including fused, SELECT,
  VIS/SHADE, queue and beacon variants. Final shader validation is recorded in
  .validation/logs/exact-spec-release-shaders.log.
- Real Linux project -Werror objects: llcleandenoisevk, llcleanprojstochvk and
  llcleanrenderervk PASS. No generated vk_*_spv.h is committed.
- Visual result, register/spill counts, total GPU delta and strict physical-query
  budget remain unproven. This is a source candidate, not performance sign-off.

## Owner A/B

Use radiance 1 and RenderVKVisQueue 0; keep sample-grid/lobe-guard settings fixed.
Compare RenderVKShadowExactSpecular 0/1, waiting five seconds after each toggle.
Hold still, pan across the glossy point/projector highlight, stop, and inspect its
wake. Repeat RR off/on, then Half/Full RT-shadow resolution. Debug 12 is a white
coverage mask for selected receivers, black otherwise; check it first if the A/B
looks unchanged, then restore RenderVKDenoiseDebug 0. Diffuse softness should remain.
Compare total GPU time plus shadowsel, shadowvis, shadowmerge, dn_tmp, dn_sp and
BASE/LOCAL. Compare against 68780f7121 too: runtime off/on shares the new pipeline's
register allocation, so it cannot expose the whole compile-time occupancy tax.

CPU fixtures:
python3 indra/newview/tests/shadow_exact_specular_checks.py "$PWD" "$PWD/.validation/exact-specular"
python3 indra/newview/tests/shadow_sample_grid_checks.py "$PWD" "$PWD/.validation/sample-grid"
python3 indra/newview/tests/shadow_specular_lobe_checks.py "$PWD" "$PWD/.validation/specular-lobe"

## Prepared vqbench fixture

prepare_exact_specular_vqbench.py copies a supplied read-only vqbench into a NEW
owned output, adds --specular-baseline / --specular-exact, a glossy metallic floor,
stable synthetic light IDs, and three-point/three-projector cases. Synthetic points
get valid shadow records because this harness has no ReGIR population producer;
this tests generic point visibility/BRDF code, NOT viewer grid population or reuse.
It prints separate diffuse-plane hashes and raw-exact nonzero counts. Expected:
same diffuse hash off/on, nonzero exact layer only on. The rest/specular per-frame
hash is intentionally different; the CPU partition test checks its expectation.
The harness records raw SELECT/VIS/SHADE, not the denoiser or a beauty resolve, so
it cannot demonstrate that camera-motion trailing is visually cured.

Prepared fixture full standalone -Werror build and --dry reflection checks PASS.
No GPU fixture run yet. Preparation (output must not exist):

python3 indra/newview/tests/prepare_exact_specular_vqbench.py --bench-source /home/bryan/Documents/git/slipstream/.claude/worktrees/vq-bench/indra/vqbench --source "$PWD" --out "$PWD/.validation/exact-vqbench"

The tool prints build and short 320x180 GPU commands. GPU commands MUST use the
copied vqb_run.sh, which refuses if the viewer runs or the lock exists. --dry is
CPU-only and returns before device initialization. No source/build in the supplied
harness worktree is modified.

## combo-test integration

Integrated onto combo-test 7cce28aeb2 in stoch-exactspec-integrated. Only 700bad20f9
was after the already-integrated 68780f7121. Dropped the queue-only request rebuild
hunk; none of CLEAN_VQ_SHADOW, vqSh*, VQR_SHSUN/VQR_SHVIS, visQueueShadow* or
recordProjStochShade is restored. Preserved combo-test's hardcoded ReSTIR High
architecture and boolean mRsNow diagnostics; added exactSpecular to that route log.
The source-bound sample-cap test accepts the two retained fused/normal-VIS sites.

Full regen_world_spv.py PASS: 427 world + 18 FSR variants, push/shared-memory budget
checks included. Changed C++ files pass -Werror syntax-only with real project flags
and freshly generated integrated headers. The three CPU regression suites pass.
Generated headers/caches live only in this owned worktree's .validation directory.

The prepared six pipeline probes and glossy fixture are pinned to 700bad20f9,
not combo-test's later queue deletion/High-only refactor. GPU results must retain
that qualification; integration compilation is a separate check. The optional
legacy vqbench preparer targets the pre-deletion manifest; use the already prepared
fixture on stoch-foliage-swim for this requested GPU batch.
