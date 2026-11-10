# HDR display-output orchestration status

Source checkpoint resumed from `d77dfe9727c3a0f895890d6b6806f1950ba9b40f` on 2026-11-01. The pre-existing `hdr-display-15-latest` worktree at that commit was left unchanged; this work used an isolated local clone because shared `.git` metadata is read-only.

HDR remains **not READY** and readiness remains fail-closed:

- `LLVulkanContext::refreshDisplayHdrPolicy()` passes `linearPipelineReady=false`, so a capable surface still chooses the existing SDR format.
- `LLCleanRendererVK::configureDisplayOutput()` rejects a mode change after renderer initialization. `renderFrame()` creates the world renderer once from the current swapchain format; no drained transaction rebuilds the display-dependent renderer pipeline set.
- `VkDisplayFrame` and `VkDisplayEncode` have no live `LLVulkanContext` owner or calls. UI replay targets the acquired swapchain view. No linear-nits target receives the complete composition, and no final encode follows main UI.
- Snapshot points 1/2/3 and offscreen capture still use the fixed four-byte swapchain readback. There is no canonical SDR resolve followed by the exact overlays/HUD/UI replay for each point.

`hdr_live_output_contract.py` guards the current false readiness input, post-init mode-change refusal, and the explicit target-budget/content-stage API contracts. It also prints these remaining gaps. It does not establish functional HDR output.

## Exact integration contracts still open

1. At the existing drained recreation boundary, prepare renderer display mode, all mode-specific pipelines, the admitted FP16 target, encoder, and matching swapchain format as one transaction. Commit only when every component is ready. On any failure, destroy the candidate resources after safe retirement, recreate/select SDR, expose the failure reason, and ensure no incomplete frame is presented.
2. Include every optional HDR allocation and all fenced live copies in measured admission. Bound allocation/extent work; optional allocation failure must visibly fall back to SDR. Disabled HDR must create no optional target, pipeline, dispatch, or per-frame polling.
3. Record the full world/post chain and all overlays, HUD, and main UI in the linear-nits target; record exactly one final encoder after UI. Refuse HDR presentation when any required stage, descriptor, or pipeline is missing.
4. Provide canonical SDR capture images for points 1/2/3 and offscreen/supersampled/logout paths. Tone-map only world-before-UI content, then replay the corresponding SDR overlays/HUD/UI. Keep SDR storage/readback four-byte compatible and never treat HDR swapchain bytes as SDR or HDR scene input.
5. Complete changed-source C++ and shader compilation against current generated headers, generated-pack verification, and the required Windows cross-build before landing. No viewer build, GPU, deploy, or push was run for this source-only checkpoint.
