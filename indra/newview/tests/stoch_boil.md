# Moving-caster floor boiling regression

`python3 indra/newview/tests/stoch_boil_checks.py SOURCE_ROOT OUTPUT_DIR`

Compiles production GLSL scalar change detection, visibility packing, cap selection,
and elapsed-time accumulation helpers into a deterministic C++ fixture. A static
lit floor remains inside a broad moving-caster tag while its sampled radiance varies.
The fixture compares the old two-frame policy with the new visibility-edge policy,
then moves an opaque eight-pixel occluder across the floor. Both leading and departing
edges must reset in the same frame at 30, 60, 120 and 240 Hz. It also checks unknown
visibility, a stationary noisy penumbra, the packing error bound, channel-independent
cap wiring, sun gating, diagnostic history invalidation, and Persist=0/default=2.

This is a scalar regression, not a rendered-image or GPU-performance measurement.
The original producer still supplies stochastic visibility estimates; different
sampled lights can disagree in a penumbra. The test does not prove every penumbra is
stable or that the owner's particular scene is fixed.

## Live A/B

Keep the camera and lights still, leave nearby avatars moving, and compare:

- `RenderVKShadowCasterHistory 1`: reproduce the old broad cap from 40a5446bba.
- `RenderVKShadowCasterHistory 0`: disable that cap/reset for diagnosis (shadow trails can return).
- `RenderVKShadowCasterHistory 2`: default; react only when this receiver's own visibility
  and its same-surface neighborhood agree that its shadow changed.

Each policy change invalidates history once. Allow at least two seconds to settle;
compare the stable lit pool separately from an avatar's advancing/departing shadow.
Expect the lit floor to retain history, while changed shadow pixels follow immediately.
Test Half and Full shadow resolution, with the same DLSS/RR settings. This applies to
local **radiance** shadows and the merged sun visibility channel. Ratio mode and the
existing moving-light anti-lag policy are unchanged.

## Signal and cost

The producer uses existing D/S visibility-weighted and unshadowed sample sums, after
exact-specular ownership exclusions. Their luminance ratio is independent of sampled
light energy when fully lit. Unknown denominator means no visibility-change evidence.
Two five-bit codes use spare MOT bits; two filtered codes reuse unused GEOM compatibility
age bits. Actual ages already live in HIST. No images, allocations or rays are added.
Temporal mode 2 adds at most eight neighbor MOT reads in the existing 3x3 same-surface
loop, plus fixed scalar arithmetic. D/S caps and resets are independent; stable pixels
keep neighborhood clamping and spatial feedback. A visibility change still resets
colour immediately, rather than merely shortening an old dark history.

The history-cause debug view temporarily uses the same low metadata bits. It never
consumes them as visibility, and entering/leaving that view resets the history.
