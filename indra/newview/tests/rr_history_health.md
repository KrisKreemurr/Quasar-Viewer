# RR semantic resets and guide diagnostics

This is diagnostic/reset hardening, **not a demonstrated fix for spec-lines**.
The DLSS-cycling observation was confounded by RT shadow budget fallback: shadow
maps replaced stochastic RT shadows. Retest only after confirming the RT path is
active (including the budget-latch repair).

## Runtime controls and logs

`RenderVKDlssGuideHealth=1` (Boolean, Persist=0, default0) samples at most one RR
frame per second per view. At fenced reuse of that frame slot, `VKDlssHealth`
logs pixel count and nonfinite/out-of-range counts for diffuse albedo, specular
albedo, world normal, roughness, specular hit distance, pre-exposed colour,
exposure, motion, depth and responsivity. Counts describe values before the final
guards, including source colour nonfinites caught by its existing sanitization.
Sky writes its explicit safe guides; normal/roughness/hit checks cover surfaces.
Motion and depth are checked after the existing input-preparation pass.

The diagnostic uses the existing coherent constant-buffer ring: each slot grows
from512 to1024 bytes, with84 bytes of GPU counters at byte512 and CPU-only metadata
at768. No extra images, buffer allocation, readback copy, dispatch, queue idle or
GPU wait is introduced by enabling it. The caller already fences slot reuse.
A sampled dispatch has subgroup-reduced counters and a compute-write->host-read
barrier; the next reuse reads its completed counters. Zero pixels means that
recording did not execute, **not** a clean sample. Sampling does have unmeasured
GPU cost, so turn it off after capturing evidence. No GPU execution was used to
validate this patch.

`VKDlssReset` is always logged at most once per second per feature instance.
It reports recorded reset requests with temporal/history, skipped-frame, network,
new-feature, input-target and policy reasons; source target generation/RT epoch,
extents, raw route, shadow dimensions, estimator/sun policy and overlay context.
A burst ORs all reason/policy bits and counts requests; even a one-frame burst is
flushed on a later non-reset frame. These are **recorded requests**, not proof the
GPU submission completed. Existing dropped-recording recovery rearms temporal
reset for the next frame.

PolicyBits decimal bit positions (bits may be ORed):

| Bit | Semantic field |
|---|---|
|0|Effective RR raw-signal routing|
|1,2|Local shadow denoiser width/height (Half/Full)|
|3|Radiance/ratio, exact-specular partition, sample-grid/plane history policy|
|4|Transparency overlay|
|5,6|Hit-guide march distance/step limit|
|7,8|Responsivity/jitter-sign policy|
|9|Requested sun/merge/projector paths; actual map/RT and merged path; RT budget fallback|
|10|Stochastic shadow mode/specular-lobe guard|
|11,12|Exposure mode/static exposure control (not auto-exposure output)|
|13|Configured raw-signal policy|
|14|Guide debug/reflection-enable policy|
|15|Full-resolution signal routing policy|

In the reset line, `signal` bits0/1/2/3 are radiance, exact-specular, sample-grid
and plane-history policy. `sunPolicy` packs merge setting in bits0..3, requested
sun mode in4..7, projector mode in8..11, budget fallback at16, actual sun mode
(off/map/RT=0/1/2) at20..21, and actual merged pipeline at24. Weak-sun idle is
excluded. `policyBits` identifies which of the16 table entries changed; `sunPolicy`
is its current context, so the two numbers have different meanings.

Only semantic changes reset history, once when acknowledged by a successful
vendor evaluate recording. No timer periodically resets/recreates the feature.
No light IDs, candidate counts, sun intensity, moving-caster tags, frame clocks,
per-frame auto-exposure values or per-light content enter the key. A change of
actual pipeline ownership (map/RT/merged), including budget fallback, is semantic;
a weak sun becoming idle within the same pipeline is not. Existing camera cuts,
teleports, origin shifts, skipped-frame and feature-create resets are preserved.
Target-generation/RT-epoch changes also reset the retained feature after its
sets have been rewritten; no raw-handle ABA assumption is introduced.

## Guards and failure behaviour

Always-on finite guards supply safe normals, specular/diffuse colours, roughness
and positive finite hit distance (invalid/absent ->65504). Invalid position skips
hit marching; invalid source exposure uses1 consistently for RR pre-exposure and
unexposure. Invalid no-history input requests maximal responsivity. Valid guide
values preserve their existing ranges; no visibility, ray-origin, BRDF ownership
or stochastic estimator changes are part of this patch.

An allocation failure still follows the existing RR soft-allocation/fallback
route. SR shares the constant ring but does not sample guide health or reset for
RR-only semantic policy changes.

## Validation / next owner test

```
python3 indra/newview/tests/rr_history_health_checks.py SOURCE_ROOT OUTPUT_DIR
```

The source-bound CPU fixture compiles actual guard functions and policy helpers:
NaN/Inf/zero/negative/range cases, unchanged valid values, each of16 semantic fields,
one-shot acknowledgement and saturated/coalesced log reasons. Wiring checks cover
the transient setting, rate limits, ring layout, host visibility and exclusions.
Shader compilation/validation and actual project object compiles are separate gates.

For the next in-world run, enable guide health, confirm stochastic RT shadows really
are active, and retain the log around symptom onset and any policy/resource changes.
Do not claim a cause from the earlier DLSS cycling. The original pre-toggle trigger
remains unknown; a clean guide sample does not prove every intervening frame was clean.
