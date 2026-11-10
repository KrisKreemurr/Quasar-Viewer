# Beta 6 HDR display, brightness, and snapshots

Branch: `hdr-display-beta6-exposure`, based on rel15 `11ea16cd71` (which is based
on the then-current `86aa535cdb`). Snapshot-format change cherry-picked with `-x`
as `5778aff2d3`; the HDR exposure/brightness changes are the final commit in this
branch.

## Changes

- HDR output now retains the exact SDR tone-mapped result at and below diffuse
  white. The HDR headroom shoulder only extends highlights above diffuse white,
  using exposed scene luminance, preserving the SDR appearance of midtones while
  allowing brighter highlights. This fixes the HDR-only path that replaced the
  SDR tone curve's luminance with exposed scene values.
- Added a debug-only probe at the final HDR encode input, after the world, HUD,
  name-tag/world-overlay, and main UI composition. With `RenderVKFSRDebugProbe`
  enabled, it reports the exposure factor, actual sampled input luminance in
  nits, sampled mean, and approximate P95. A second pre-tonemap probe reports
  exposed scene mean/P95. The histogram uses 4,096 bounded grid samples and 128
  quarter-stop bins; its mean accumulator is bounded for FP16 maximum values.
  No extra image or persistent allocation is added when the debug setting is off.
- The existing HDR paper-white slider is now labelled `Paper white / UI
  brightness (nits)` and is the single SDR-to-HDR reference-white control for
  scene display and UI. It defaults to 203 nits. UI/HUD paths linearize sRGB
  before scaling to paper white. On Windows, if the preference is still at its
  default, the renderer queries the current monitor's SDR white level and uses
  it as the initial value when the OS provides one. The current SDL/Wayland
  route exposes no KDE reference-luminance value, so KDE and other systems use
  the adjustable 203-nit fallback.
- HDR snapshots have a local-only Radiance `.hdr` option, offered only while HDR
  output is active. SDR remains the default and the only inventory/upload
  format. The preview and SDR snapshot path read the linear-nits display image,
  convert paper white to SDR white, and retain the existing orientation/size
  handling. Values above paper white clip in this SDR conversion; the optional
  local HDR file preserves them.

## Validation

- Real rel15 configured object compile commands passed for
  `llcleanrenderervk.cpp`, `llcleanfsr3vk.cpp`, `llvulkancontext.cpp`,
  `llcleansnapshotvk.cpp`, `llfxruntimevk.cpp`, `llrendervk.cpp`,
  `vk_display_encode.cpp`, `vk_swapchain.cpp`, `llpanelsnapshotlocal.cpp`,
  `llsnapshotlivepreview.cpp`, and `llviewerwindow.cpp`. Commands were taken
  from the rel15 Ninja build database, pointed at this worktree's source and
  generated headers, and wrote objects under `.hdr-validation/objects`.
- `clean_tonemap.frag` normal and straight-alpha variants, `clean_fsr_probe.comp`,
  six live-snapshot shader variants, `ui.frag`, and `ui_occl.frag` passed
  `glslc` and `spirv-val`. World, UI, and FSR generated shader packs regenerated
  with the rel15 shader cache; world shared-memory/push-budget checks passed.
- `display_hdr_checks.cpp` and `radiance_snapshot_checks.cpp` compiled with
  `-Wall -Wextra -Werror` and passed. Snapshot source-contract checks and
  `git diff --check` passed. Both edited XML files parse.
- No viewer link/build or GPU/runtime screenshot test was performed. The owner
  should compare the same scene in SDR and HDR and inspect the `HDR probe` log
  with `RenderVKFSRDebugProbe` enabled.

## Windows platform follow-up

The configured Windows clang-cl compile found the new SDR-white lookup needed an
explicit `../llviewercontrol.h` include. This follow-up supplies that declaration
and repeats default-white calibration during Windows initialization, after the
HWND exists and before creating the first swapchain. Startup previously installed
the preference before any window existed, so an HDR-enabled startup could miss
OS white calibration. Initial swapchain creation consumes that pending policy,
avoiding a redundant recreation.

All eleven production objects listed above now also compile with the actual
configured rel15 Windows target/SDK/include flags. The pre-existing configured
Windows build has no target for the new display-encode TU, so it uses the
swapchain TU flags with only the source/object names replaced. Candidate sources
and current generated shader headers are real; no stubs were used. Compiler
warnings in existing LLTrace/unused-member code and configured argument warnings
remain; no whole-viewer link is claimed. The final changed context object is
also recompiled with the actual configured Linux command.

Private command/output receipts are retained at the shared repository root in
`agents/hdr_beta6_windows_receipts/`. The Windows macro check and diff check pass.
No GPU/runtime validation was performed.

## Live snapshot shader brightness parity

The live snapshot compute path used `displayNitsToSdrLinear`, which still
applied a second shoulder to the already tone-mapped linear-nits display image.
At reference white this produced about 0.52 linear SDR instead of 1.0, unlike
the corrected CPU readback, so snapshots/previews could remain dim. The shared
GLSL helper now matches `vkdisplay::sdrSnapshotLinear`: finite values in the
SDR range are divided by paper white, invalid channels become black, and
HDR-only values clip at SDR white. Radiance HDR capture bypasses this SDR
conversion and keeps its extended values. No CPU code or shader ABI changed.

All seven actual manifest variants consuming the helper (display output plus
six snapshot modes, including RGBE) passed `glslc` and `spirv-val`. The snapshot
source contract and diff check pass. No GPU visual test or viewer link is
claimed. Private shader command receipts are in
`agents/hdr_beta6_windows_receipts/snapshot-shader-parity.json`.

## Snapshot panel setup crash

The base `LLPanelSnapshot::postBuild()` invoked virtual `updateControls()` before
initializing `mSnapshotFloater`. The new local HDR option queried that null
floater during the virtual call, matching the owner's SIGSEGV stack. Parent
assignment now precedes the call. HDR option availability also explicitly waits
for both a floater and a preview, and HDR save guards preview access. The normal
draw refresh admits the HDR option after preview construction completes.

Both changed production TUs (`llpanelsnapshot.cpp`, `llpanelsnapshotlocal.cpp`)
compiled as real objects with configured rel15 Linux and Windows commands. No
GPU/runtime snapshot test was run. Private receipts are retained in
`agents/hdr_beta6_windows_receipts/snapshot-setup-crash.json`.

## HDR brightness ceiling: relative chain converted to nits twice

The owner's probe showed sane pre-tonemap scene RGB becoming exact paper-white
RGB before final encode. Source tracing found a unit mismatch: the tonemap HDR
specialization multiplied by paper white, but the remaining display chain uses
relative linear values (1 = paper white), and `clean_final.frag` multiplied by
paper white again. The FX scRGB adapter and classic glow likewise expect relative
linear input. This double scaling saturated coloured surfaces downstream.

Tonemap and its motion-debug output now retain relative linear units. Final
effects perform the sole conversion to absolute nits; HUD/UI add their own
paper-white-scaled content afterward, then PQ/scRGB output encodes that completed
linear-nits image. Scene exposure still runs exactly once before the tone curve,
on the same code path as SDR. HDR sliders do not alter exposure.

The headroom shoulder now adds to the SDR-base luminance continuously above
scene diffuse white, avoiding the former jump to relative luminance 1 at the
threshold. A uniform RGB scale bounds peak channels without per-channel hue
clipping. SDR specialization and its arithmetic are unchanged by this correction.
The retracted SDR washout report was weather, not evidence for exposure resets.

Validation: both tonemap variants passed GLSL/SPIR-V checks; the actual complete
world/FSR pack regenerated and passed budgets. The changed renderer TU compiled
as real Linux and Windows objects with configured rel15 command lines and the
fresh shader pack. `hdr_display_units_checks.py` compiles extracted production
GLSL helpers using GLM and verifies coloured midtones, threshold continuity,
monotonic/hue-preserving headroom, physical peak bounds, and unchanged SDR identity
at paper whites 203/350 and peaks 409/1000/1350. The previous production shader
fails the coloured-midtone regression; corrected source passes. Snapshot source,
Windows macro, and diff checks pass. No GPU visual comparison is claimed.

Use `RenderVKFSRDebugProbe=1` with X/Y at a known outdoor coloured surface for the
owner retest; compare the pre-post scene and final encode-input lines. The final
input should remain coloured and reflect nits only once. Mode-switch and
compositor-default work are still separate pending candidates.
