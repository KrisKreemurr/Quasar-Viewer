# Target-omega phase anchor repair

Base mega-1106 547ac3ae45; branch spinner-reset-fix. Source-only, no viewer/GPU run.

## Cause and reproduction

The old GPU record used angular velocity in radians per REAL world-clock second,
but reconstructed t0 as worldTime - mRotTime, where LL increments mRotTime by
timeDilation * frameDelta. At dilation d, inferred t0 moves by (1-d)*wallTime.
Commit7047f2c1fb began accepting those changes whenever rate*abs(t0delta)>.05rad,
without compensating the base matrix. Thus refreshing the row changes its angle
discontinuously. Re-registration separately copies the newly derived t0 outright,
so merely reverting the .05rad test would not fix every sweep/reset.

Example: rate1rad/s, dilation.8, two seconds elapsed. GPU phase under the old
anchor is2rad; rewriting inferred t0 changes it to1.6rad. The .4rad backward step
is not a server rotation command. The same mismatch occurs when the CPU idle
path does not advance mRotTime at all, but a GPU spinner is still admitted.
This is proven by source and fixture; live numeric diagnostics remain the owner
confirmation for the specific rings, not a claim to have observed their reset log.

## Repair

Each object owns one stable parent-local GPU base, effective angular velocity
(raw omega * region dilation) and world-clock epoch. First admission captures
getRotation and the epoch together after the object update has finished. Repeated
faces, rolling refreshes and re-registration reuse the same tuple bit-for-bit.

On rate/axis/dilation changes, first fold the OLD GPU phase into its base, then
publish the new rate and epoch. Rendered orientation is continuous at that event;
new phase0 is compensated by the rebased orientation. Normalize after rebasing
(LL's1ppm normalization policy) to avoid scale drift under many rate changes.
Dilation events in idleUpdate queue the existing linkset mover path (including
children); no event means no per-frame transform upload. The record itself and
GPU ABI are unchanged. No GPU buffers, shader variants, maps or heap allocations
were added. Per-object state has37bytes of fields plus class padding and dies
with the existing LLViewerObject lifetime.

resetRot and resetRotTime invalidate the anchor explicitly. The next pose capture
uses the COMPLETED LL state (new rotation plus whichever angular accumulation LL
retained), not a stale pre-update snapshot. Repeated server updates without a
rotation reset do not change the anchor. A genuinely new server/script rotation
remains authoritative, exactly as the CPU path: it is not silently ignored in
the name of continuity. Leaving GPU admission invalidates its old cache; re-entry
starts from current CPU state. Selected objects use the paused/editable CPU pose
rather than letting a stable GPU anchor ignore a rotation edit. Existing moving-
linkset A/B admission remains, with normal default nonphysical/unsat restrictions.

Nonspinning children of a spinning root share the ROOT's base, effective omega,
epoch and pivot. Multi-face objects share the same object anchor. Existing
single-effective-axis limits for nested independently spinning ancestors are
not redesigned by this change.

## Raster, CLOD, RT and motion

composeOwnedPose publishes the shared Instance.spin_axis/pivot tuple. Ordinary
spinPose, cluster clSpinPose and clean_rt_instances.comp all evaluate the same
rate*(worldTime-t0) and Rodrigues pivot transform. Previous-frame raster/CLOD
motion still uses the previous instance tuple and previous world time. A rebase
does not rewrite that previous tuple. Shader code and row ABI did not change;
this audit plus the numerical fixture is not a GPU-image equality claim.

## Diagnostic

RenderVKGpuSpinDiag: Boolean, Persist0, defaultoff. VKSpin logs local ID only,
reason (axis, t0 drift for a dilation rebase, resetRot, resetRotTime, or
re-registration/first admission), old/new phase in radians, old/new t0 and dilation.
Globally at most8lines per real second. No names/UUIDs. A phase0 after a compensated
rebase is expected: the old phase is now in the base, not lost. Explicit reset
and subsequent fresh anchor installation can each produce a line. Normal steady
sweeps should produce neither phase rewrites nor row changes.

## Checks

- Real project common-flag -Werror syntax checks: llviewerobject.cpp,
  llcleaninstancestore.cpp, llvovolume.cpp PASS. Existing project warning exceptions
  retained; no configured full viewer build or Windows build claimed. No platform
  branch or Windows API touched.
- spinner_reset_checks.py extracts actual gpuSpinPose/phase/reset/applyComposedPose
  methods and uses the actual LL quaternion constructor/operator math. Strict CPU
  compile with asserts enabled PASS:6000 steady samples; rate/dilation continuity
  including0; explicit resets after new rotation install; linked pivot;1000
  unchanged-row refreshes;10000 dilation rebases remain within LL's1ppm norm policy.
- Source contracts: root anchor sharing, dilation event notification, raster/CLOD/RT
  shared record, selection admission; XML and Windows macro declarations PASS.
- No shader changed and no generated SPIR-V headers committed.

Reproduce CPU fixture in this worktree:
python3 indra/newview/tests/spinner_reset_checks.py "$PWD" "$PWD/.validation/spinner" "$PWD/.validation/llviewerobject-syntax-command.json"
The last JSON is the real viewer common compile/include flags; the script writes
all fixture outputs to the supplied directory and re-enables assertions even when
the production flags define NDEBUG. LL's existing ignored-qualifier warning is
suppressed for its math headers; new fixture/helper warnings are errors.

## Owner check

Enable RenderVKGpuSpinDiag, watch the rings for several rolling sweeps without
selection, then select/edit/deselect. Repeated unchanged server updates and a
steady dilation should not reset phase. Varying region dilation should change
speed continuously. Check linked children, reflections and shadows at the same
time; CLOD/raster/RT consume one pose. Actual server rotation commands may change
orientation and must appear as explicit reset events. Disable the transient
diagnostic after the test. Live visual/RT validation remains the owner gate.
