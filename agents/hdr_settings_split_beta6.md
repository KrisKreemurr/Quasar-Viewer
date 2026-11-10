# Beta 6 HDR, emissive, and classic-glow settings split

Branch: `hdr-settings-split-beta6`, based on `cd7e646ae3`.

## Setting semantics

- `RenderVKDisplayHDR` is the persistent HDR-to-monitor request. The Quick Preferences
  HDR display checkbox controls this setting. Both it and the Advanced checkbox are
  disabled unless the active display reports HDR capability. Paper white and peak
  remain the two persistent calibration settings.
- `RenderHDREnabled` remains the internal scene-HDR/vintage pipeline choice. It is not
  the monitor-output switch and is no longer exposed as the combined user checkbox.
- `RenderEnableEmissiveBuffer` independently controls emissive material rendering.
- `RenderGlow` controls HDR bloom. `RenderClassicGlow` independently controls the
  classic display-space glow pass. Both may be enabled intentionally; glow data is
  retained if either effect is enabled. New installs default to bloom only, matching
  the former HDR look.

## Migration and visual tradeoff

`RenderGlowControlsMigrated` makes the migration one-time. If the saved old scene mode
was vintage (`RenderHDREnabled=false`), the viewer enables `RenderClassicGlow` and
`RenderEnableEmissiveBuffer`, but does not change
`RenderHDREnabled`. That preserves the user's prior LDR scene pipeline, probe format,
exposure/tonemap availability, and low-dynamic reflection behavior. The intentional
visual change is that emissive materials now contribute in that vintage/LDR scene as
requested; values can be clamped by that scene's LDR targets. No display composition
or swapchain code is changed here.

## Checks

Run the focused source check from the repository root:

```sh
python3 indra/newview/tests/test_hdr_settings_split.py
```

The check covers persisted defaults, UI bindings/capability gating, migration shape,
and separate bloom/classic-glow consumers. The parent branch should run its integrated
real-1.5 translation-unit validation before accepting the cherry-pick.
