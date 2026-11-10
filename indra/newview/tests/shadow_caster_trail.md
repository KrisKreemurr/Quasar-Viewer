# Moving-caster shadow history

Base760157d5bc, source-only; no viewer/GPU run.

## Proven source failure

Radiance-mode local lighting retains our temporal/spatial denoiser even when
DLSS RR is on (rrRawSignal(kRrRawProj) explicitly returns false). A static floor
has receiver motion0. The producer already observes moving blockers but its
PDN_VIS_MOVER bit only protects reservoir reuse; it does not reach the temporal
colour history. MOT.w reports moved LIGHTS, not moved CASTERS. The radiance
variance floor can deliberately keep old colour in its clamp box. Existing
sample-grid reprojection and exact-specular partition do not fix this omission.

The merged sun separately has moving-blocker persistence, but its history caps
are60Hz durations: a configured2-frame horizon can be many actual frames at high
FPS, and the fast-variance change threshold can retain a partial shadow.

## Repair

Pass existing monochrome blocker motion through VIS/SHADE and fused producers
into spare MOT.w bit8. Keep low8 moved-light bits unchanged. Temporal stores
current caster presence in compatibility GEOM bit11, and recent influence in
bit10; actual history ages are separate HIST millisecond layers. No accumulation
reader consumes those old compatibility duration bits. A previous frame's tag
allows the newly lit trailing edge to react even when its ray meets no caster.

Only affected radiance texels cap history to two actual frame intervals. If raw
and same-surface mean both leave prior colour in the same direction beyond a
bounded current-neighbourhood threshold, restart that channel immediately. Do
not use the widened old variance as the detector threshold. Disable first-a-trous
history feedback on these texels so static neighbours cannot write stale colour
back after a temporal reset. Static-caster texels retain their original long
histories and spatial feedback. An idle rigged caster is conservatively tagged:
its penumbra may be noisier due to the short cap, rather than retain a stale pose.
Merged sun uses the same fast change predicate and two-actual-frame cap only on
its existing dynamic-caster/persistence path; static sun policy is unchanged.

No new setting, image, buffer, dispatch or extra ray. Tracking is enabled for
nonmerged shadow-core variants too, using the existing previous-instance binding.
Separate RT mask kernels are unchanged; they already have a dynamic-caster
protocol. RR may additionally accumulate the combined output; this source-only
fixture cannot establish RR's final visual response or assign every observed
trail exclusively to our denoiser. The exact dominant specular remains raw; the
residual local radiance can suffer this same stale-caster mechanism.

Coloured VIS packing uses bit29 for colour, so the split producer's new tag
covers monochrome blockers (including ordinary opaque avatar shadows). Coloured
transmission through moving glass retains the previous path; it is not claimed
fixed by stealing a colour bit.

## Checks and owner test

Production helper C++ fixture -Werror: zero receiver motion with changed raw
shadow retains>.93 old value after2 frames in a32-sample history, versus immediate
new-state reset; departure, unchanged/noisy-penumbra and30..240Hz cap cases PASS.
Producer/previous-presence/spatial-feedback source contract PASS. Existing exact
specular, lobe and sample-grid CPU suites PASS.18 actual manifest shader variants,
including temporal, spatial, fused/split producers and beacon twins, compile
-Werror and pass spirv-val Vulkan1.3 scalar layout. No C++ changed.

Compare760157d5bc to this commit with identical radiance/RR/sun/projector settings.
Walk the avatar over a stationary, plain receiver; watch entry AND departure,
then stand still. Test RR off/on and Half/Full. Expect our dynamic-region history
to follow in1-2 frames; renderer/upscaler visual sign-off remains an owner gate.
If a residual trail appears only with RR, the next investigation is its input
reactivity, not another global reduction of static shadow history.
