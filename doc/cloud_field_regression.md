# Cloud field, filtering and horizon regression guard (2026-11-07)

Branch base: mega-1106 f90a273696 (includes owner test 600d88cfb4).

## Source bisect: what did and did not change

The following revisions contain byte-identical cloud density/common and weather
shaders: 1f6c606d25, 9264d604aa, 764e90e880, 0799494e90, 600d88cfb4,
f90a273696. SHA256 prefixes respectively:

- common: 0cc68db2792ac0b3
- weather: cd3f07cbd0a29a22

Thus none of the three later cloud commits removed #1283's world field. The
altitude change only raises the layer; with zero offset the cloud equations are
unchanged. The #1105 commit changes history validation, not coverage. The cited
5da3e41905 only changes RT-hit shading, not the cloud density/weather kernels.
A claim that these commits directly reverted the world field is unsupported.

Three concrete source defects remain:

1. #1283 varies **amplitude**, but the periodic EEP map still defines cloud support
   (zero local coverage returns before the world correction). The base and erosion
   noises also repeat horizontally. A world field which merely dims the same puffs
   does not move their grid. This is an incomplete anti-repetition contract in the
   original change, not a newly deleted world field.
2. Source EEP detail and the baked weather field are sampled at LOD0 regardless of
   their footprint. Far cloud steps span many weather texels. Frame-varying march
   phase undersamples them: coherent stripe/band aliasing is possible. Existing
   follow-ups 1e2f3f9c62, 3dd0795f72 and a0db81f0cc address this but are NOT ancestors
   of the owner build. They are adapted here, retaining newer physical-medium and
   panorama-mean paths.
3. 6b6c0108a1 introduced a physical-medium early return before the existing range
   fade. It correctly deferred prefix haze but inadvertently also skipped the fade,
   leaving clouds ending at the finite slab/range boundary. This predates #1283.

The supplied screenshot is consistent with these defects. Exact attribution of
all displayed vertical streaks still requires an in-world retest; no GPU image or
performance claim is made from the numeric fixture alone. RR may amplify aliasing;
this patch does not change RR or glow.

## Repair and bounds

Use the existing world field to shift periodic EEP support and both 3D noise
phases coherently, not just their brightness. Preserve coverage controls, wind and
world-cell identity. Phase uses the continuous first octave of the same world field, independent
of view/light footprints and RGBA8 quantization. Otherwise LOD or alpha quantization
would move geometry between queries/frames. The four-hash interpolation adds bounded
ALU; empty density samples still need only one weather texture fetch. No new world-field cache or parameter-block fields.

Adapt the three unmerged filtering follow-ups: prefilter EEP inputs, build a fixed
512->1 weather chain, select local/world LODs separately from march/lighting/shadow
footprints, and clamp world alpha within both trilinear levels. Nine fixed compute
reductions, ten disjoint mip views; +349524 bytes weather payload (~341KiB, device
allocation padding not included). No extra view/light march samples. Partial
allocation failure releases the chain and retains the existing cloud fallback.

Both physical and classic paths execute the same smooth range fade. Physical
history stays raw linear radiance; current-camera prefix transport remains in the
composite. It is not run twice or legacy-clamped.

History rejects true foreground truncation. Scene depths both beyond the reachable
cloud slab do not reject identical cloud history merely because the distant scene
surface changed. Sky's FP16 far sentinel remains accepted; zero/old metadata and
nearby moving occluders remain rejected.

## Source regression check

`python3 -I scripts/check/run.py --stage source` discovers `cloud-field`.
It uses Python standard library only, evaluates production scalar warp/coverage
expressions and weather hash parameters against a controlled periodic EEP texture
and a 128km-wide world field. It is not the owner's actual EEP asset.

At the owner's 8097m tile period, autocorrelation X/Y:

| Policy | X | Y |
|---|---:|---:|
| Prior amplitude-only support |0.977|0.970|
| World-shifted support |0.175|0.124|

The guard requires old >0.95 and corrected absolute correlation <0.8. Zeroing the
world phase fails numerically at0.977. For a controlled32m weather stripe under a
300m footprint, the production LOD is4.25 and box-filter response0.031 rather than
unfiltered1. Zeroing weather LOD fails. Restoring the physical early return fails.

Temporal fixtures execute the production scalar predicate: sky65000 versus FP16
64992 is accepted, as are differing surfaces beyond the cloud exit; foreground
1000 versus sky is rejected both directions, and old metadata0 is rejected.

Owner retest: same EEP, z22, cloud altitude1500, quality3, DLSS RR UltraPerformance;
compare grid, vertical streaks and horizon, then compare RR off and camera movement.
Only the owner render can confirm the full screenshot is resolved.
