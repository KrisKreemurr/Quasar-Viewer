# Reflection-only RT hang beacon

`RenderVKRTHangBeacon` is now an unsaved U32 diagnostic:

| Value | Coverage |
|---|---|
| 0 | Off, normal pipelines |
| 1 | Existing full coverage, all 16 twins |
| 2 | Fused reflection-list trace and separately queued reflection blend layers only |

Mode 2 does not replace GI, sun/local shadows, visibility-queue walks or forward
reflection pipelines, even if full-mode twins are already cached. It requests
only two twins initially. Switching to full mode lazily requests the remaining
ones; an asynchronous job must finish/join before any output handle is read.
Normal pipelines remain bound while it is pending, and a missing twin retains
its normal pipeline. Buffer retirement and four-frame address arming are unchanged.

The device-lost report keeps its existing section, kernel IDs, lane/pixel,
query ordinal/site and ray inputs. Each recorded frame additionally names its
mode. Mode 2 targets the fused list path of the reported split=0 incidents.
It does not instrument the separate closest-hit TRACE kernel of split shading;
its fused tail and queued blend stage are the selected twins. No shader or
SPIR-V ABI changes, new shader variants or persistent settings were added.

## Cost estimate, not a GPU measurement

The existing twin stores 3 uvec4 (48 bytes) before **each attempted query**,
with two buffer barriers; lane completion stores another 4 bytes. Thus a few
hundred selected reflection lanes can issue more than a few hundred stores.
The full-screen GI/shadow/forward writes disappear in mode 2.

Approximate emitted store payload for N selected lanes and Q query attempts:
`N * (48 * Q + 4)` bytes, plus small kernel/header stamps. For 360–900 lanes:

- Q=1: 18,720–46,800 bytes/frame.
- Q=20 example: 347,040–867,600 bytes/frame (0.331–0.827 MiB).

These are payload examples, not PCIe traffic or GPU-time bounds: they omit
cache-line transactions, reads, barriers and register/scheduling effects.
The host-visible allocation remains bounded at 96 MiB plus a 1 KiB header,
reused across modes and retired when switched off. No per-frame full-buffer clear.
GPU-ms overhead is unmeasured; compare mode 0 with mode 2 on the same view after
the log reports selected twins ready. Keep deliberate-hang SelfTest disabled.

## CPU checks

`python3 indra/newview/tests/rt_beacon_mode_checks.py <source-root> <owned-output-dir>`
executes the actual mask and pipeline-selection functions with fake handles.
It checks all 16 pipeline mappings in each mode, full→narrow transitions,
pending/not-armed fallback, missing twins, null handles, typed unsaved XML,
selected shader IDs and the pending-job publication contract. It runs no GPU.
