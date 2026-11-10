# Grazing-floor stochastic-shadow reconstruction (2026-11-07)

This fixes the depth reconstruction path independently of the rejected specular
partition candidate. Base c3b9ce5d0b; no b70878ae94 changes. The owner already ruled
out exact-specular extraction, the lobe guard and Cluster LOD with their controls off.

## Root cause and stage isolation

A perspective plane has **affine reciprocal view depth**, not affine view depth.
The old local-shadow path mixed three incompatible approximations:

1. Bilinear temporal taps compared the tap's raw view depth with the depth at the
   reprojected receiver, ignoring the displacement between those points. At grazing
   angles they differ substantially despite belonging to the same floor. Local
   reprojection search defaults off, so failure discards colour history immediately.
2. Spatial statistics/a-trous used a first-order, half-precision view-depth gradient
   per nominal denoiser texel, ignoring perspective curvature and actual truncated
   sample positions. Their stops could reject or unevenly weight that same plane.
3. Local-light upsampling again compared four tap depths with one constant depth.
   When all weights vanished its shared fallback chose one tap. Jitter changed
   that choice, creating moving row bands even from a smooth, noiseless input.
   The merged-sun consumer already had a receiver-plane test; local light did not.

Farther or lower-camera floor points have larger relative depth changes per pixel.
Increasing a relative tolerance alone would admit other geometry. The repair instead
compares the different sample positions on the **same projected receiver plane**.
It does not change rays, offsets, lighting, specular ownership or filtering controls.

## Representation and control

`RenderVKShadowPlaneReconstruction`: Boolean, Persist0, default1. It is latched per
frame and enabled with existing sample-grid history (also default1). Off restores
the old producer gradient and old temporal/spatial/upsample tests. Changing effective
policy invalidates local and merged-sun histories once; old/new formats never blend.
GI's default is false, so its plane representation and algorithm stay unchanged.

Flagged local GEOM.z stores two FP16 **relative reciprocal-depth slopes**, per the
same fixed render-pixel step the producer already samples. Orthographic gradients
remain affine view-depth slopes. The predictor evaluates the plane at actual raster
sample offsets, including previous sample phase and odd extents. Nonpositive/invalid
predictions (including crossing the horizon) are rejected. Existing normal cuts,
receiver-guide uncertainty and geometric tolerance values remain in effect. A
substantially shifted virtual-specular tap retains its previous validity semantics.

Local upsampling intersects each sampled raster ray with the current receiver's
**geometric** plane (perspective and orthographic cases), using the existing inverse
projection. Its final depth guide is FP16: ignore sub-rounding-interval mismatch in
tap similarity, then rescale the remaining interval so the rejection endpoint is
still the original tolerance. This removes quantization-driven weight changes
without widening acceptance across silhouettes. Unknown-tap downweighting is kept.

No new rays, images, buffers, allocations, descriptor bindings or dispatches. Work
is bounded arithmetic on existing taps plus at most one current G-buffer normal
fetch and four plane intersections for reduced local-light reconstruction. Full
resolution bypasses that upsample, but still benefits from corrected temporal and
spatial geometry. GPU time/register impact is not measured.

## Source-bound fixture

```
python3 indra/newview/tests/shadow_grazing_plane_checks.py SOURCE_ROOT OUTPUT_DIR
```

Uses system GLM headers (or CPLUS_INCLUDE_PATH pointing at the existing dependency).
Compiles actual shader plane/producer-gradient/upsample/GGX/time helpers, with a
D32 reverse-Z camera, truncated Full/Half sample grids, FP16 slopes/final guide
depths, 256 Halton phases and a grazing floor. No viewer or GPU is required.

Selected results at nominal distances (the actual sampled row is raster-quantized):

| Stage/control | Old | Corrected |
|---|---:|---:|
| Full, 5m: empty history phases |0/256|0/256|
| Full, 20m: empty history phases |127/256|0/256|
| Full, 40m: empty history phases |223/256|0/256|
| Full, 80m: empty history phases |239/256|0/256|
| Half, 40m: empty history phases |223/256|0/256|
| Camera height0.4m, Full10m: empty phases |223/256|0/256|
| GGX + stochastic soft-point-light40m, normalized MSE |0.879150|0.062500|
| Half upsample10m: fallback phases |128/256|0/256|
| Half upsample10m: maximum frame error-step |0.0400001|~0.0000001|
| Spatial same-plane mean relative prediction error |1.5118814|0.0001389|

The GGX test starts each phase with settled history and feeds identical one-ray
Bernoulli visibility to both policies. It isolates history admission's noise gain,
not a full multi-frame warm-up simulation. The upsample test isolates deterministic
banding with an affine segment of a smooth highlight: no stochastic noise exists
in its input. It retains the FP16 guide rounding that the actual consumer sees.

Also verifies different parallel-plane rejection, unchanged tolerance endpoint,
orthographic affine prediction, horizon rejection, one-frame format latch, GI-off
default, transient setting, and unchanged existing ownership/grid/caster contracts.
The 160m Half case reports only phases where both candidate taps are valid floor;
sky/out-of-frame samples are not manufactured into valid history.

These are source-bound CPU stage results, not GPU beauty images or an owner-room
capture. The owner's video/live retest remains the final visual check.

## Owner A/B

On the same floor and camera, compare `RenderVKShadowPlaneReconstruction 0` and1;
wait at least3 seconds after each switch for the intentional history reset. Keep
`RenderVKShadowSampleGridHistory 1`, stochastic shadows on and all other settings
fixed. First keep exact specular/lobe guard/ClusterLOD off as in the ruling-out A/B.
Then repeat with normal settings, Half and Full shadow resolution, DLSS/RR on/off.
Compare near vs far (and low camera height), with a still camera before panning.
Full should improve temporal retention even though it has no upsample. No larger
shadow bias or missing-occluder workaround is part of this change.
