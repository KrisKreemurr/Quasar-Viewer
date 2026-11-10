# Main / Beta 6 integration

READY — branch `main-beta6-sync`, base `forgejo/main aa7609acca`, release `v1.5.0-beta.6` (`320e7b1083`). Production validation head `0b3109bcbbfb450bf42ecd038b6421e1371d374e`. The status-file commit follows this head; obtain the final tip with `git rev-parse main-beta6-sync`.

All 256 candidates processed, oldest first: **212 picked, 32 adapted, 12 skipped**. No unlanded candidate. Requested RT singular-transform fix `14f45e882f` is included as `31afc546a5`.

## Integration decisions

- Preserved main VR/multiview, orthographic capture and native-window rendering. The beta-only mono snapshot adapters were picked then neutralized by `b3959db6ed`; main uses layered `HDR_UV` and `endRenderingViews`.
- Preserved main elapsed-time denoiser age storage and combined it with beta receiver guides/variance recovery. Main age remains separate from beta compatibility history counts.
- Retained all main and beta CMake source/test blocks. Source authorship and `-x` provenance remain on the individual picks.
- Main-only auxiliary UI submits now participate in texture ticket lifetime proofs: `c6170aa4ae` uses the existing graphics timeline, and a later marker covers auxiliary submits without treating an unrelated main fence as completion. New actual-source fixture covers this boundary.
- **HDR/XR/replay interoperability:** HDR display output is deferred while XR presentation or instant replay is active, because those existing main consumers require SDR byte images. Switching those consumers triggers policy renegotiation; replay does not ingest PQ/scRGB during the transition. This preserves their SDR behavior; simultaneous HDR output plus XR/replay remains unsupported, explicitly logged.
- Main specialization IDs 20/21 remain reserved for view count/medium. HUD/selection HDR moved to 22. The HDR frame extension follows main history-time data (`VKSSRInfo` 8352 B); shader/C++ layouts match. Previous rigged VP uses the per-view main ABI.
- Windows cross checks exposed the imported keyboard constructor-order warning and a Linux-only helper unused on Windows; both were repaired. Vulkan context now includes its frame-timer dependency directly.

## Validation

- Real configured Linux object commands, retargeted to this worktree and owned outputs, with `-Werror`: **122/122** changed production TUs pass. No stubs in production object checks. Existing project warning exceptions retained.
- Real configured Windows clang-cl cross-object commands, retargeted to this worktree and owned outputs: **14/14** platform TUs pass. `/WX` retained; unchanged main baseline `reorder-ctor`, `inconsistent-missing-override`, and `infinite-recursion` remain warnings. These are cross-object checks, not a configured Windows viewer build.
- Actual world/FSR regeneration: **453/453** variants compile. UI **5/5** and FX **10/10** compile. All **468** actual compiler outputs pass immediate `spirv-val --target-env vulkan1.3 --scalar-block-layout`. Generated headers exist only in owned `.validation/generated/`; none committed.
- Imported Python fixtures: 71 executable/check fixtures from the 73-file inventory pass (two remaining files are object-compilation utility wrappers, covered by the real objects). This includes 24 CEF environment tests, telemetry ingest/settings, texture recipe/priority/lifetime/retirement/backpressure, animation/TPS, and transport main-thread/shutdown consumers. RT transform fixture and the new native UI submit/completion fixture also pass. Several source extractors/stubs were adapted to main APIs; retired narrow texture fixtures now delegate to the current unified-route lifetime/admission fixtures rather than pretending the removed route exists.
- Three standalone transport plans execute **38/38** serial compile/link/test commands successfully, including owner loop/wire/watchdog, graph detach and localhost UDP worker tests. These use the existing isolated fake boundary fixtures, not viewer runtime.
- `python3 -I scripts/check/run.py --stage source`: all four checks pass (10 key commands; 15 stochastic history source/math fixtures; 3631 files Windows macro check; 5275 XUI files).
- No full viewer link, GPU/runtime validation, push, protected rel15 writes, or generated SPIR-V header commits. Claude performs the full build and runtime gate.

Receipts remain in the owned worktree `.validation/`: `objects-plan.json`, `objects-results.jsonl`, `refreshed-objects-results.json`, `windows-objects-plan.json`, `windows-objects-results.jsonl`, `world-shaders.log`, `shaders-post.log`, `shaders.jsonl`, `fixture-latest-summary.json`, `fixtures/pure-results.json`, `network-plan-results.json`, and `source-checks.log`. Historical failed harness attempts are preserved; latest corrected passes determine the summary. Short CEF UNIX-socket fixtures used `/tmp/sync-cef-small`; no large artifacts used RAM-backed /tmp.

## Per-candidate ledger

| Release commit / subject | Disposition | Main result / explanation |
|---|---|---|
| `ea75a7b158` #1039 GI reuse occupancy: the VIS TAIL finishes stored picks in a kernel of its own (56 registers) | skipped | Main 9b9e95ca96 already has stored-pick VIS TAIL/giReuseFinish, with subsequent newer CHG-TAG/reuse work; preserve main implementation. |
| `efdaebb4a1` render: exclude fully emissive world faces from shadow visibility | skipped | Main e72d474745 already excludes fully emissive faces from shadow visibility; retain main variants and architecture. |
| `ce12bb08a2` fix(vulkan): snapshot split raw visibility before temporal filtering | skipped | Main 3d1ad667f6 already snapshots raw split visibility before temporal filtering. |
| `8628417a45` fix: align RT audit readers with emissive metadata strides | picked | `08cf7cc73565` Unmodified `-x` pick. |
| `7897fd8f7e` 1.5 backport build fixes: no VR multi-view guard, no region-map ortho view, snapshot includes pipeline.h | skipped | 1.5-only adapter removes main VR multiview/region-map handling; main retains those features. Snapshot pipeline include checked separately. |
| `45ee2f88c0` #1306: Respect copy permissions for inventory selections | picked | `49413e8887f7` Unmodified `-x` pick. |
| `f834e6f074` Dither primary ReGIR proposal lookup in the receiver tangent plane | picked | `c1678debb65e` Unmodified `-x` pick. |
| `7b3691dd4c` Restart changed-source radiance history behind a transient control | picked | `7c2d4d8d6fd6` Unmodified `-x` pick. |
| `45dde749db` Remove unused light-preparation locals left by lookup pruning | picked | `b41c2fe2e51e` Unmodified `-x` pick. |
| `bb0629184c` Compare alias slot counts with explicit integer conversions | skipped | Main already casts the alias-count RHS operands to uint32_t; the enum arithmetic warning addressed by this adapter is absent. |
| `9d9cbe0c46` fix: gate optional particle fog and preserve physical-sky fallback | picked | `107656f5229c` Unmodified `-x` pick. |
| `c93574b802` RT reflections: roughness-scaled elapsed-time history in the resolve (was a fixed 20 % EMA) | picked | `58088d40821f` Unmodified `-x` pick. |
| `b25b2b97a7` RT reflections: shorter roughness history and anti-lag in the resolve (owner: history smears reflections) | picked | `bd887e97889c` Unmodified `-x` pick. |
| `e1049315fd` Keep reflection history port compatible with beta6 frame timing | picked | `d0e563c01f4e` Unmodified `-x` pick. |
| `79078f842a` Validate local shadow history against the previous skinned receiver | adapted | `523095d2154b` Retained main elapsed-time denoiser age storage and multiview snow matrices; applied previous-receiver normal/depth validation and motion payload. |
| `77d9101e72` Backport opt-in telemetry to owner beta6 candidate | picked | `9ea13d611b8e` Unmodified `-x` pick. |
| `dc91ff8379` Backport durable telemetry revocation security fix | picked | `590a879c719f` Unmodified `-x` pick. |
| `c1cef343fc` Add beta6 actual settings persistence privacy fixtures | picked | `9a8d624970f0` Unmodified `-x` pick. |
| `8aceec2c36` Bound secondary local reflection source variance before reconstruction | picked | `14ee370e8327` Unmodified `-x` pick. |
| `73f10fde27` Stabilize skinned local-shadow variance across colour resets | adapted | `bc8e646cf576` Applied variance recovery before storage while retaining main elapsed-time history age write. |
| `93007709cb` Revert "Bound secondary local reflection source variance before reconstruction" | picked | `b87a04cb06d6` Unmodified `-x` pick. |
| `566c561ac2` Add an analytic-light stochastic visibility comparison mode | skipped | Immediately reverted by610323a1fb; analytic-light comparison mode absent from shipped Beta6 and main. Omit net-zero experiment rather than adapting it to newer main shader architecture. |
| `610323a1fb` Revert "Add an analytic-light stochastic visibility comparison mode" | skipped | Reverts566c561ac2, which was skipped as a net-zero experiment absent from the shipped release; main remains without that rejected mode. |
| `db89e97a70` #1302 Restore controller body and prompt texture aliases | picked | `50409d0b67a0` Unmodified `-x` pick. |
| `16749fbfc0` fix: retain small growing snow-depth deposits | picked | `5522805ff01e` Unmodified `-x` pick. |
| `8ec3e73cb1` Fix report history replacement on Windows (#1315) | picked | `e38cbdc111a2` Unmodified `-x` pick. |
| `8fa6d7bcec` #1350 Preserve native key repeat metadata across focus resets | picked | `3f8b371f4735` Unmodified `-x` pick. |
| `6d5996a9c3` #1343: Bound unconsumed animation exports to the latest joint data | picked | `f7cabc4629dc` Unmodified `-x` pick. |
| `dccb47b7fa` Test actual animation emit and verification append bounds | picked | `fc26374fd8de` Unmodified `-x` pick. |
| `7facf444d5` #1192 Allow explicit re-invites for departed conference participants | picked | `3dc65d085fff` Unmodified `-x` pick. |
| `29e5b3815e` Add source contract and offline checks for TPS held-fire port | picked | `51d0c96d25a3` Unmodified `-x` pick. |
| `522e6062b1` Accept optional collection elapsed hour in telemetry minor1 | picked | `388d752d3b78` Unmodified `-x` pick. |
| `16af587cad` Align telemetry windows to post-consent collection hours | picked | `fd0f589c1a3a` Unmodified `-x` pick. |
| `64b6bde71d` World > Animation Explorer lists the animations objects and avatars started on your avatar (last 100, your own gestures / AO dropped first): who played it, Playing or Stopped Ns ago, priority. The selected one plays on a stand-in avatar (turn / zoom / pan as in the upload preview). Stop stops it for everyone (as Stop Animating Me); Stop and revoke also takes the animation permissions back from the object (the trigger and override bits, the only ones the server accepts); Block animation adds it to the asset blocklist, which now refuses blocked animations on our avatar (process_avatar_animation) and stops one when it is blocked. "Hide my own animations" (AnimationExplorerHideOwn) shows only what objects play on you. | picked | `7f7ebdc5cda9` Unmodified `-x` pick. |
| `38c3b180ef` #1099 Animation Explorer: Try on me plays the selected animation on your own avatar | picked | `c0a78b2ee078` Unmodified `-x` pick. |
| `8e490f6df6` Respect current name visibility in animation explorer | picked | `6cd85d20241e` Unmodified `-x` pick. |
| `9af25c5917` Refresh animation names after restriction exception commands | picked | `8cf2a6918a53` Unmodified `-x` pick. |
| `155533381f` Invalidate retained animation names when visibility policy changes | picked | `0f52f8ef00e2` Unmodified `-x` pick. |
| `dc946bf1ee` #1099: Animation Explorer: unanswered name requests are retried, and stopping a try-on leaves the sim's animation playing | picked | `8777eb9076c3` Unmodified `-x` pick. |
| `27faf260af` fix: report SDL window request failures and bound show recovery | picked | `639b38313cbe` Unmodified `-x` pick. |
| `6f9c683861` fix: bound the SDL show error snapshot on the stack | picked | `5336bd61b3f6` Unmodified `-x` pick. |
| `62427fa6f8` Preserve system body bakes and bound missing-bake fallback | adapted | `c6c68c9ab7a4` Main already has system-body bake preservation/fallback and newer no-body forensics; conflict resolution retains those paths (resulting pick changes only whitespace). |
| `1e02b133c0` #1176: unit-space physics-shape mesh and cost colours, with a unit test | picked | `0b0139a5904f` Unmodified `-x` pick. |
| `715a9ce174` LLCleanPhysShapeStore decides which shape each selected object shows by LL's renderPhysicsShape rules (the choice Develop > Physics Shapes makes; its physics-detail and prim-hull helpers move here and are shared), builds it once in unit space and captures it as raw slots of the clean mesh store, keyed by its content. A shape no selected object wants is freed 10 s later, and all shapes stay within 16 MB together (least recently used first). A refused capture is retried with the memory governor's backoff, then shown as LL's magenta box. Builds take at most 2 ms a frame; the rest wait for the next frame. | adapted | `c00c7b51701e` Retained main orthographic region capture and LLVKViewFrame; added independent physics-shape host records alongside multiview API. |
| `2d9fa80c06` #1176: draw the selection's physics shapes on the GPU | adapted | `61d71583dee4` Added physics passes beside main selection-outline/replay sources; retained endRenderingViews and main mono-only overlay guard (beta had no stereo physics behavior). |
| `fff491e6dd` #1176: Show Physics Shape When Editing on Edit > Features and Build > Options | picked | `4d4829deb5f0` Unmodified `-x` pick. |
| `9627802b15` #1176: count and log magenta-box physics shapes by reason; the limits stay constants | picked | `cfb42828727d` Unmodified `-x` pick. |
| `f24f2f1a83` #1176: 64 cost colour steps, so the ramp reads as continuous | picked | `31503b01e4e4` Unmodified `-x` pick. |
| `a3bf4bc041` #1176: physics shapes of rezzed rigged mesh and animesh, at the object's own pose | picked | `fb979f9fad43` Unmodified `-x` pick. |
| `5e47c8c314` #1176: a prim physics hull that can't be allocated is skipped and retried instead of crashing | picked | `681f05e7f7ee` Unmodified `-x` pick. |
| `dbb96dbe21` Publish physics hulls only after checked temporary allocation | picked | `fa80f1c9135b` Unmodified `-x` pick. |
| `6a182f4ae6` Recover UI pipeline creation with bounded owner retries | picked | `f60ecbf1c391` Unmodified `-x` pick. |
| `90361a05c4` Refresh replaced skin assets and compare every packed joint | picked | `3babfff3071b` Unmodified `-x` pick. |
| `e716d87497` Snapshot avatar layers: transparent avatar, holdout mattes, avatar scope + picker, orientation swap (#1209) | skipped | Main 24dd22fbf5 already implements transparent avatar layers, holdouts, scope/picker and orientation; preserve main version. |
| `91347288f9` #1328: snapshot scope: "Selected" can leave you out, animesh only in "All" | adapted | `6b624a43cd41` Kept all main test blocks and added the snapshot-scope test block; applied scope behavior. |
| `ffe94f2f8d` #1328: resize a transparent snapshot in premultiplied space | picked | `89197c001118` Unmodified `-x` pick. |
| `0421f28a8e` #1328: transparent avatar layer: alpha through the HDR chain, holdout on the GPU | adapted | `3be7bfacedf0` Preserved region-map fields and stereo tonemap variant/FX exclusions; integrated straight-alpha holdout and transparent snapshot coverage. |
| `abf699bb05` #1328: transparent avatar layer: avatars it leaves out stay in the world | picked | `d455da40df7f` Unmodified `-x` pick. |
| `f1be010939` #1328: rename the transparent layer "Avatars only (transparent background)" | picked | `f9416d19eae2` Unmodified `-x` pick. |
| `1517121fdd` #1328: avatar layer: screen-space reflections trace the warm-up's frame | picked | `71a46090eff2` Unmodified `-x` pick. |
| `19c9c2129b` #1328: avatar layer: find the world occluder by depth, not avatar ids | adapted | `4b320b996922` Applied depth-based holdout while preserving stereo avatar-mask exclusion. |
| `2807d83a1b` #1328: avatar layer: budget its occluder, fail the shot when the holdout cannot run | adapted | `cf3226d0f239` Kept main stereo tonemap selection and added successful-holdout eligibility before alpha tonemapping. |
| `d344e6aff1` #1328: avatar layer: refuse it under an RLVa vision sphere, and say so | picked | `45a714051302` Unmodified `-x` pick. |
| `163ec3fb99` Bound every snapshot storage buffer by the device descriptor range | picked | `d09eadc60866` Unmodified `-x` pick. |
| `8d97ef9755` Use mono HDR coordinates for beta transparent snapshots | adapted | `831705b06a` → `b3959db6ed99` 1.5 mono-only adapter neutralized in follow-up: retain HDR_UV for main layered HDR sampler. |
| `8cdfb747c5` Apply transparent highlight tint without waiting for debug pipelines | picked | `36a74329bdcc` Unmodified `-x` pick. |
| `b3ec066914` Keep dedicated probe captures free of transparent UI tint | picked | `e74a3c9f0bbd` Unmodified `-x` pick. |
| `245bbf5f73` Read live highlight flag within the forward shader scope | picked | `90697d9c2ec4` Unmodified `-x` pick. |
| `199a105a26` 1.5 port of #1328: end the mono rendering scope before the snapshot holdout | adapted | `287e381f58` → `b3959db6ed99` 1.5 mono scope adapter neutralized in follow-up: retain endRenderingViews for main layered rendering. |
| `95575482ee` Retain texture upgrade deduplication across upload refusals | picked | `c9c7baf0abb6` Unmodified `-x` pick. |
| `d5bacb573d` Include retained-tail bookkeeping in texture queue fixture extraction | picked | `ff8d553d122b` Unmodified `-x` pick. |
| `91f3371ba0` Remove unused snapshot layer callback bindings | picked | `4cbf73b007f1` Unmodified `-x` pick. |
| `dfaa322dfe` Revert "Recover UI pipeline creation with bounded owner retries" | picked | `81bbf710ead1` Unmodified `-x` pick. |
| `7693fc2f47` Review fixes for #1331: migrate saved filters and retain unchanged owner bars | picked | `2599d88e031d` Unmodified `-x` pick. |
| `98f2085f47` Review fixes for #1340: rearm previews when audio availability changes | picked | `a19145157a8c` Unmodified `-x` pick. |
| `644853a168` Bound sound preview ownership and fail softly on resource refusal | adapted | `be8937004f60` Retained held-kill CMake test and added sound-preview limits test separately. |
| `41e3c6838e` Adapt Area Search fallback and cursor comment for the release port | skipped | Main already has the simplified Area Search fallback and releaseCursorToCenter behavior; remaining changes are comments in a release adapter. |
| `623e2a5211` Let sound availability dispatch retry failed panel refresh | picked | `4e84a1a37950` Unmodified `-x` pick. |
| `d4a514730a` Skip quiescent texture dirty work after final demand merges | picked | `b10f8f46baa1` Unmodified `-x` pick. |
| `d097a3adce` Preserve incomplete and explicit-discard texture work | picked | `142b2c2a6761` Unmodified `-x` pick. |
| `4afcfcf415` Track scene HDR generation in medium descriptors | adapted | `7ae2ec76b385` Passed monotonic HDR incarnation with main array view and per-eye medium API; retained array-storage descriptor contract. |
| `6bc956a4bb` Make GI forward cache filter a nonpersistent A/B | skipped | Empty cherry-pick: main already makes the GI forward-cache A/B setting nonpersistent. |
| `186f44a8df` Preserve XWayland access for embedded CEF on Linux | picked | `1b9c10442ae5` Unmodified `-x` pick. |
| `139f261aa7` Stage the replacement HDR view before target writes | picked | `ec696d21a906` Unmodified `-x` pick. |
| `c3eee2e7ce` Reject probe candidates exceeding exact device allocation limits | adapted | `1d0fef2c3721` Added exact buffer-allocation preflight alongside main layered screen-image/array-view helpers; kept both APIs. |
| `3056d9b70f` Exercise production probe planning and permanent allocation refusal | picked | `1fdcef3d037e` Unmodified `-x` pick. |
| `871f24f2b1` fix: recover XWayland auth for Wayland sessions | picked | `8557118c49be` Unmodified `-x` pick. |
| `35fc8bccbd` fix: prefer CEF Wayland on live Wayland sessions | picked | `5f2cb88e7beb` Unmodified `-x` pick. |
| `d5b38fbdeb` fix: pass CEF Wayland selection only to plugin | picked | `653f91cea8ed` Unmodified `-x` pick. |
| `f943d61c95` Add bounded diagnostics to bug reports | picked | `cc54f37fc8b8` Unmodified `-x` pick. |
| `235c376dd3` Revert "Add bounded diagnostics to bug reports" | picked | `324aac057eaa` Unmodified `-x` pick. |
| `8ba2c739b4` Add bounded diagnostics to bug reports | picked | `4e24638c13f5` Unmodified `-x` pick. |
| `62cebec776` Fix Report Bug diagnostics on the 1.5 source base | picked | `afcb647c114a` Unmodified `-x` pick. |
| `14157d5799` Bind new notecard rename to its confirmed item without changing inventory context | picked | `2e2c660ff6fd` Unmodified `-x` pick. |
| `e60d1c8140` Refresh planar mirror after Dynamic toggle | picked | `81ef5d8a1445` Unmodified `-x` pick. |
| `97e6c88fdb` #1047 Share sun history policy and honor RR self-integration in merged shadows | adapted | `2393e3c63f64` Shared sun policy retains main htMotion elapsed-time conversion in both denoiser paths. |
| `3b4491fa03` #1047 Preserve the 64-frame sun ceiling with versioned packed history | adapted | `43485b15da43` Kept main floating count ABI/time-age bank while using versioned radix65 packed compatibility history and 64 ceiling. |
| `eff4af21c3` Include avatar binding in worn row dedup signature | picked | `945aea8a63d5` Unmodified `-x` pick. |
| `b8ebfaa299` Skip rigged rows until skin metadata resolves | picked | `8502c81ca45d` Unmodified `-x` pick. |
| `359062d165` Disable unavailable DLSS choices in Quick Preferences | picked | `ee77ba396684` Unmodified `-x` pick. |
| `db1405b563` Define current Vulkan HDR display policy and readiness gate | adapted | `e103d80eeeaf` Applied bounded HDR format policy while retaining main checkResult handling for device/surface errors. |
| `1dd46ed9b6` Wire independent HDR display preferences and capability status | adapted | `8e77c88fc26c` Retained XR running/vsync transaction and added HDR policy/display-change methods alongside it. |
| `0bec5e1a46` Add HDR output transfer source and optional metadata fallback | adapted | `14f905d5027a` Retained main nonfatal swapchain checkResult handling after HDR-to-SDR retry; added optional metadata. |
| `e664e382c1` Add bounded current-renderer final display encoder | picked | `9c2e6350bb20` Unmodified `-x` pick. |
| `4ef0b78e42` Resolve optional HDR metadata through the device dispatch | adapted | `32e5942ef215` Applied optional dispatch lookup while retaining main swapchain failure handling. |
| `81ba141751` Refresh HDR controls on UI events instead of draw polling | picked | `0ca948da06de` Unmodified `-x` pick. |
| `66c7d36d2e` Include graphics preference declaration for display refresh callback | picked | `e37a57391461` Unmodified `-x` pick. |
| `e629a3e9db` Bind HDR capability to current display at recreation boundary | picked | `1ae0d474da2d` Unmodified `-x` pick. |
| `ad0c621a1a` Add opt-in linear-nits UI and world-overlay raster output | picked | `185ab67bb83a` Unmodified `-x` pick. |
| `7b001b8d50` Add admitted float display staging and lazy SDR capture target | picked | `5e26c52727c9` Unmodified `-x` pick. |
| `1405f25fe5` Guard HDR math helpers against Windows macro expansion | picked | `6b3a4ecd75cb` Unmodified `-x` pick. |
| `fd6f680eff` Track incomplete HDR UI frames without vetoing keyed retries | picked | `c5d9b7fa506b` Unmodified `-x` pick. |
| `6acea33dea` Extend scene highlights through linear HDR display post output | adapted | `894a990e4a3d` HDR specialization applies to both main mono/stereo tonemap twins; retained RT-input initialization and stereo final/FX guard with frame-completeness check. |
| `68f2fce771` Tone-map HDR world captures before replaying SDR UI | picked | `ae6f76a57ef2` Unmodified `-x` pick. |
| `f14ebbe4f8` Preserve linear HDR headroom through SMAA FXAA and CAS | picked | `d23056aeba12` Unmodified `-x` pick. |
| `0ee941adc7` Carry float scRGB contracts through display FX permutations | picked | `e9ba5e163748` Unmodified `-x` pick. |
| `0e99634aa1` Convert live HDR HUD mesh colours to linear paper-white nits | adapted | `c28bc4dad189` Extended main SSR/frame ABI from8336 to8352 bytes, display metadata@8336 after history_time; retained VR HUD pipelines and both clocks. |
| `b1a7a43f19` Blend HDR selection outlines at display paper white | picked | `73251e84bd56` Unmodified `-x` pick. |
| `b4d0e5b121` Add source check for HDR SDR capture contract | picked | `3bde1be90be1` Unmodified `-x` pick. |
| `cd7e646ae3` Document HDR live output integration gaps | picked | `cafff4476594` Unmodified `-x` pick. |
| `ebe936f051` Split display HDR, emissive and classic glow settings | adapted | `3960e842f39f` Applied independent display/emissive/classic controls; retained main stereo/region-map exclusions for mono glow passes. |
| `439a6f56fa` Complete beta HDR display output and glow setting split | adapted | `509e0f3e9f66` Retained native UI lifecycle, headless/XR targets, XR recreate pacing and replay hook; integrated float world image selection and encode before presentation. Final c6170aa4ae explicitly defers HDR for active SDR-only XR/replay consumers. |
| `86aa535cdb` Keep HDR active across display mode changes | picked | `886c0260cec2` Unmodified `-x` pick. |
| `11ea16cd71` Fix HDR luminance mapping and snapshot source selection | picked | `0ae49d877031` Unmodified `-x` pick. |
| `b4749c1f87` Document beta HDR exposure and snapshot status | picked | `1b88387a24fb` Unmodified `-x` pick. |
| `5778aff2d3` Add local Radiance HDR snapshot output | adapted | `348da13b0cef` Kept main native-popout/VR window includes and added HDR snapshot helpers. |
| `446630ad66` Match HDR output and UI to SDR reference white | picked | `d9694823646c` Unmodified `-x` pick. |
| `e5ecd94a50` Initialize Windows HDR reference white after window creation | picked | `305ab72550cc` Unmodified `-x` pick. |
| `1c321e4b73` Preserve paper white in live SDR snapshot conversion | picked | `c7227a3b6ece` Unmodified `-x` pick. |
| `4594ec5486` Guard snapshot HDR format setup until preview exists | picked | `ca4304601ee6` Unmodified `-x` pick. |
| `f838a9243a` Draft independent display luminance calibration probes | picked | `3df9666ad7ae` Unmodified `-x` pick. |
| `a4873f9279` Prepare automatic display luminance calibration | adapted | `def8e34a3aab` Kept replay include and added calibration settings/platform helpers. |
| `a5ea381ca5` Keep HDR post processing relative until final nits conversion | picked | `248606bc954e` Unmodified `-x` pick. |
| `49c6c65a80` Preserve legacy calibration and avoid unavailable SDL properties | picked | `53374884612c` Unmodified `-x` pick. |
| `7590a685a8` Discover KDE display reference and EDID peak | picked | `35de15ffe9cb` Unmodified `-x` pick. |
| `6cce747395` Allow higher display reference white consistently | picked | `c89c3214badd` Unmodified `-x` pick. |
| `c8050ee688` Limit display metadata queries to calibration events | picked | `172655f2854b` Unmodified `-x` pick. |
| `24b4a07355` Avoid signaling an unowned calibration helper PID | picked | `8cf5a730cafc` Unmodified `-x` pick. |
| `a08defcd87` Run display calibration off the UI thread | picked | `dcb59d05a1df` Unmodified `-x` pick. |
| `6ba2b108be` vulkan: make HDR mode changes replay scene state | adapted | `542dde980f4d` Merged display-generation gating with main minimized-XR recreate pacing and headset world-target setup. |
| `49efaf580c` vulkan: reset avatar consumer state on display switch | picked | `cd265830f67f` Unmodified `-x` pick. |
| `97a01442b9` vulkan: preserve avatar pose across display modes | adapted | `ec88ce6e3877` Applied pose/readiness handoff while retaining main headset target transition setup. |
| `382ec725e8` vulkan: clarify avatar pose handoff fallback | picked | `543ee06b7dfa` Unmodified `-x` pick. |
| `477814cd4b` vulkan: preserve held avatar writes across display switch | picked | `c55abbbbf1fb` Unmodified `-x` pick. |
| `77934d2b85` Preserve GPU joint pose during morph handback | picked | `facf79d4052a` Unmodified `-x` pick. |
| `316995d785` Fail closed on calibration worker and child errors | picked | `24f6a5e816ec` Unmodified `-x` pick. |
| `0d32b946c6` Cover calibration queue rejection ownership | picked | `b09c13c0b944` Unmodified `-x` pick. |
| `9f94291342` Wake pending display calibration without a reply queue | picked | `57f9315263ff` Unmodified `-x` pick. |
| `bc8d4e1ab4` Fix HDR calibration control constness | picked | `ab22bb3fb4e7` Unmodified `-x` pick. |
| `75f5e9fc3b` Mark Linux calibration generation as platform-specific | picked | `59333f549274` Unmodified `-x` pick. |
| `faeadbe7e5` test: cover GPU morph delta across renderer reset | picked | `dc255dbb6beb` Unmodified `-x` pick. |
| `5692bcf563` Skip UI batches without usable pipeline state | picked | `4d659fc18d7b` Unmodified `-x` pick. |
| `32a5dbdb90` HDR: retain scene resources during mode changes and fix snapshots | adapted | `225d5df583fc` Preserved VR/native surface lifetime proofs and XR pacing; retained main snapshot naming helper with optional HDR companion collision check; kept larger main floater layout, layered scope helpers and source format metadata. |
| `c9e787bcd4` Update list layout after inventory removals | picked | `0c7b927892ae` Unmodified `-x` pick. |
| `9960ebd17a` Trace avatar shelter along precipitation path | picked | `4a383cee7367` Unmodified `-x` pick. |
| `1c88b9c0d5` HDR: retire superseded output staging on return to SDR | picked | `b65d0b365ca0` Unmodified `-x` pick. |
| `e93a41d1bb` HDR: let classic glow take precedence over bloom | picked | `d6ea5f6265ca` Unmodified `-x` pick. |
| `c5d3e77b16` telemetry: add initial session segment | picked | `db559fbe5143` Unmodified `-x` pick. |
| `50abd3f172` docs: describe telemetry initial summary timing | picked | `25b5d507a94e` Unmodified `-x` pick. |
| `a80159398e` telemetry: preserve sampled counters at initial seal | picked | `ede7c7ef31ea` Unmodified `-x` pick. |
| `1286c97337` telemetry: reuse initial settled predicate | picked | `cc77fe689f99` Unmodified `-x` pick. |
| `47a8ce06d0` telemetry: describe the initial report in privacy controls | picked | `786509cac225` Unmodified `-x` pick. |
| `858dd87504` Raise asset fetch connections to 20 texture and 20 mesh | picked | `11782b5ca5da` Unmodified `-x` pick. |
| `3f8ece655d` Prepare decoded mesh captures on repository workers | picked | `e067651b0cef` Unmodified `-x` pick. |
| `469e340e09` Invalidate mesh preparation on CPU store teardown | picked | `00f0a0cac9e9` Unmodified `-x` pick. |
| `93daaca175` Expose CPU mesh store readiness on beta | skipped | Main already exposes the identical CPU mesh ready() accessor; only its comment differs. |
| `66af4716da` Expose scoped mesh preparation CPU counters | picked | `4de2b82540e9` Unmodified `-x` pick. |
| `497f1eee50` texture: add immutable staging lease primitives | picked | `3dd35a350e36` Unmodified `-x` pick. |
| `09521dfdfe` Document immutable texture handoff and reject empty staging allocations | picked | `1a839e1d6ce0` Unmodified `-x` pick. |
| `29db8e0681` texture: stage immutable uploads without legacy fallback | picked | `37931201a14d` Unmodified `-x` pick. |
| `291f032393` texture: evaluate staged demand in residency pass | picked | `9aaad82095c5` Unmodified `-x` pick. |
| `7af5d4877b` texture: protect pixel limit from Windows max macro | picked | `4e3c306ac23d` Unmodified `-x` pick. |
| `6f22e088f4` vk: retain detached texture descriptor aliases | picked | `51ee86902f23` Unmodified `-x` pick. |
| `10251276a2` test: cover detached texture retirement aliases | picked | `b887c76036ea` Unmodified `-x` pick. |
| `33b146a061` vk: recover texture up queue after allocation failure | picked | `36aa6cf19873` Unmodified `-x` pick. |
| `749d85d5cd` vk: disambiguate completed texture lookup | picked | `29750426416a` Unmodified `-x` pick. |
| `7cfd4f48eb` vk: report texture staging thread CPU time | picked | `77f4715a49f9` Unmodified `-x` pick. |
| `48db596032` vk: refresh reserved texture views at transfer | picked | `a94072ee067c` Unmodified `-x` pick. |
| `6d07fb85de` test: cover stale reserved texture view handles | picked | `fb477a13e735` Unmodified `-x` pick. |
| `dcbe723433` test: exercise ordinary view retirement before transfer | picked | `0ee80bf3d62b` Unmodified `-x` pick. |
| `a0e7f01627` vk: pin late-created reserved texture aliases | picked | `15c37f1b5fb6` Unmodified `-x` pick. |
| `852d9de18b` test: cover late reserved descriptor aliases | picked | `6c2d9bf11712` Unmodified `-x` pick. |
| `7235bc0838` test: use production texture retire age cap | picked | `bfa506f886cc` Unmodified `-x` pick. |
| `8215396507` vk: apply reserved alias pins through retire cap | picked | `f0ff5d143123` Unmodified `-x` pick. |
| `2fe9b62499` test: execute reserved alias age-cap decision | picked | `bfab2bc0c896` Unmodified `-x` pick. |
| `16b9b69065` vk: prune retired borrowed texture aliases | picked | `d982b8d8f845` Unmodified `-x` pick. |
| `cc0031ce18` test: cover retired alias handle reuse | picked | `ae6fc71c2fdb` Unmodified `-x` pick. |
| `fee4153631` test: qualify production retire age cap in fixture | picked | `037b6b793b65` Unmodified `-x` pick. |
| `231e9b1099` vk: snapshot dropped-frame count for texture ack | skipped | Main already contains the identical dropped-frame snapshot; resolution was empty and no commit was needed. |
| `4fd9808c00` vk: normalize texture view handle keys | picked | `2acbaef69693` Unmodified `-x` pick. |
| `69bc4e9768` test: exercise immutable texture ticket transitions | picked | `460b7cdb9bcf` Unmodified `-x` pick. |
| `07eaf49529` texture: define neutral prep source contract | picked | `fc23355f9b15` Unmodified `-x` pick. |
| `ffd83a2dcc` texture: add capture budget to prep source contract | picked | `670209c70d97` Unmodified `-x` pick. |
| `e67dee5056` texture: record prep source base and ownership bounds | picked | `fc652aa93cb7` Unmodified `-x` pick. |
| `b9d7fac95f` texture: retain cached encoding facts in source input | picked | `a10a4530b656` Unmodified `-x` pick. |
| `1ea50e0adb` texture: share source capture result states | picked | `30aef670b168` Unmodified `-x` pick. |
| `dfc2d4d3f1` Add immutable texture prep source snapshots | picked | `aa1348da5236` Unmodified `-x` pick. |
| `36e24432f8` Correct texture source fixture assertions | picked | `fa386bd079c2` Unmodified `-x` pick. |
| `87e603d6a0` Fix source capture fixture generation | picked | `1b412781699f` Unmodified `-x` pick. |
| `072f21447e` Model deferred media dimensions in fixture | picked | `c640f87fa965` Unmodified `-x` pick. |
| `21ffe74f7b` texture: label canonical media prep bytes | picked | `8a56193c16bc` Unmodified `-x` pick. |
| `dbb989a1df` texture: bound alias retirement lookup work | picked | `b99032f107fe` Unmodified `-x` pick. |
| `ac1ad5d63c` texture: cap retained view-vector capacity | picked | `ad4a77d07888` Unmodified `-x` pick. |
| `7585590fa6` Add shared texture preparation service and UI adapter | adapted | `e36d9fc25338` Replaced remaining main synchronous first-UI upload with unified preparation ticket; retained surrounding native UI cache/lifetime integration. |
| `8bbbe887c6` Fix service release fixture type namespace | picked | `b27c8c59f1ee` Unmodified `-x` pick. |
| `c4a3ce307e` Close texture service admissions during teardown | picked | `c6a23da2a9b6` Unmodified `-x` pick. |
| `c05afa6aee` Keep compact UI texture formats in shared prep | picked | `b4aa6a73702c` Unmodified `-x` pick. |
| `69d01d76bb` textures: account preparation CPU and queue wait separately | picked | `a6a37a1510a4` Unmodified `-x` pick. |
| `594f53c2af` textures: avoid Windows macro names in CPU clocks | picked | `9b4d58fba871` Unmodified `-x` pick. |
| `0515f07c39` textures: retain capture budget field names | picked | `0607928f0bd9` Unmodified `-x` pick. |
| `7f9a8f8fee` tests: add llrender include to compact UI fixture | picked | `cfd884fafb59` Unmodified `-x` pick. |
| `c3c097ccbc` tests: compile extracted UI helpers in their namespace | picked | `8eba843ed2f0` Unmodified `-x` pick. |
| `af0a556c0e` tests: qualify compact UI fixture payload stub | picked | `c6906a2e968e` Unmodified `-x` pick. |
| `63f7f1dfa7` textures: publish UI preparation metadata and recycle refused IDs | picked | `06f442d994cd` Unmodified `-x` pick. |
| `8fe7d2dc72` textures: defer prepared UI images on allocation failure | picked | `00af94075434` Unmodified `-x` pick. |
| `089a6ff446` textures: fix configured service object diagnostics | picked | `6d2f23bb4f40` Unmodified `-x` pick. |
| `38f00178fe` textures: route decoded uploads through shared prep tickets | adapted | `7999ee5501dc` Kept main VR/native-window methods and UI-frame gate; added unified service guards. Final c6170aa4ae covers native auxiliary-submit acknowledgement and common-queue timeline retirement, tested by the actual-source regression. |
| `48a95373dd` texture: define context-owned prep recording seam | picked | `9c6909fdad92` Unmodified `-x` pick. |
| `ea362f2e08` texture: make prep drop proof explicit | picked | `d2bdbcdddc46` Unmodified `-x` pick. |
| `6b36db548d` texture: expose prep recording admission result | picked | `9e977ccc03c4` Unmodified `-x` pick. |
| `732572de14` vulkan: define unified texture preparation input API | picked | `49c27236f8a9` Unmodified `-x` pick. |
| `1b63671474` textures: repair renderer integration against the real beta types | picked | `ed4223e80919` Unmodified `-x` pick. |
| `48959febbb` textures: remove unused media dimensions | picked | `ce9638348c75` Unmodified `-x` pick. |
| `61cf054913` texture: queue GPU residency work until frame acquire | picked | `8f7b2c516835` Unmodified `-x` pick. |
| `2f02d691a9` textures: hold source images through GPU slot completion | picked | `cc6dce7bbcf2` Unmodified `-x` pick. |
| `2efc775557` tests: pin texture source use to retire-slot path | picked | `2740800e9811` Unmodified `-x` pick. |
| `2dd295d511` vk: retain texture source after uncertain queue completion | picked | `0d2256205294` Unmodified `-x` pick. |
| `53c67fc69b` tests: assert failed-idle guards precede offscreen allocation | picked | `f4be023e47ce` Unmodified `-x` pick. |
| `f63b9dbd0a` textures: align service retirement with the context owner | picked | `43291b94d2c9` Unmodified `-x` pick. |
| `981175cbbe` Retain deferred Vulkan world renderer safely | picked | `a8adf757b789` Unmodified `-x` pick. |
| `84c137a9ca` textures: retain the cache source during preparation | picked | `f292bc6b7bb9` Unmodified `-x` pick. |
| `9f1e17764a` textures: lease the texture-data owner at cache admission | picked | `9df9784109a7` Unmodified `-x` pick. |
| `231f1cd911` vkgfx: reject null tier-2 capture targets | picked | `e7a693141d9f` Unmodified `-x` pick. |
| `ddd12fab06` vkgfx: validate tier-2 capture extents | picked | `32f2b5714aaa` Unmodified `-x` pick. |
| `d6fb39b758` vkgfx: reuse retired texture prep stages | picked | `94c2606297e0` Unmodified `-x` pick. |
| `a3990b9f0b` vkgfx: preflight texture worker inputs and report failures | picked | `17ba692057c1` Unmodified `-x` pick. |
| `4461b4dcda` vkgfx: suppress repeated failed UI texture prep | picked | `2c1491550d2c` Unmodified `-x` pick. |
| `677887d39b` Test apply experimental network source on current beta | adapted | `64d62d94514c` Kept main NativeFloaterWindows setting/popout include alongside transport setting and event-poll wake integration. |
| `56c14dd870` Match transport operation ID type | picked | `edc442fdd53e` Unmodified `-x` pick. |
| `f9b82bb8b3` textures: accept sRGB coverage in UNORM staging | picked | `1932029db45f` Unmodified `-x` pick. |
| `9cc519d988` vkgfx: retain UI retry history until submitted publication | picked | `4c8833c24ed4` Unmodified `-x` pick. |
| `23dc5a1503` vkgfx: preserve unified texture demand through publication | picked | `1de3a2fd8c84` Unmodified `-x` pick. |
| `44d19b9fd6` tests: restore texture capture and release fixture dependencies | picked | `22fbe5d0b9b3` Unmodified `-x` pick. |
| `8454fea671` Fix authoritative transport ingress drain after owner join | picked | `2ca64daa8fe6` Unmodified `-x` pick. |
| `8c3fd816ff` Retain owner circuit closes through peer cancellation and application drain | picked | `047d3fd7137a` Unmodified `-x` pick. |
| `12925fbaaf` Default network transport owner on after runtime acceptance | picked | `a00eec8a36cf` Unmodified `-x` pick. |
| `82fa4fb875` textures: retain exact non-mipped UI raster metadata | picked | `3a869e73b8b7` Unmodified `-x` pick. |
| `1bb7af9f6b` textures: publish GPU-only avatar composites after accepted copies | picked | `57835bd52c83` Unmodified `-x` pick. |
| `73aa42e689` network: read authoritative circuit ages for the region ring | picked | `84b96a0ce803` Unmodified `-x` pick. |
| `9028ff680e` network: use owner values across main circuit consumers | picked | `f55e8130b131` Unmodified `-x` pick. |
| `4ce7efe3ec` texture prep: reserve visible admission ahead of background floods | picked | `47786ff01c3e` Unmodified `-x` pick. |
| `f445264d73` textures: prioritize visible preparation and preserve upload progress | picked | `46ac634d2544` Unmodified `-x` pick. |
| `e94b6ed618` texture prep: bound repeated saturated-lane admission checks | picked | `22c0b140c89a` Unmodified `-x` pick. |
| `56b80b9701` textures: retire hidden UI preparation without evicting residents | picked | `40bafd56425d` Unmodified `-x` pick. |
| `3f3d4c73ce` textures: validate cached upgrades against incoming encoding | picked | `1031c4d99ad4` Unmodified `-x` pick. |
| `320e7b1083` textures: overlap bounded retirement batches and finish constant refreshes | picked | `41a3a9998db6` Unmodified `-x` pick. |

## Additional integration commits

- `b3959db6ed`: restore main multiview snapshot coordinates/rendering scope after the two 1.5 mono adapters.
- `c6170aa4ae`: auxiliary texture completion proof, consistent native recorder target and explicit SDR-only XR/replay policy.
- `31afc546a5` (`-x 14f45e882f`): reject singular transforms before TLAS publication. Actual owner hang attribution remains unproven; this fixes the independently reachable invalid-transform case.
- `05bce1180a`: shader specialization/rigged VP and snapshot readback ABI integration, corresponding source checks.
- `e96d1387d7`: fixtures adapted to main/unified APIs; new native UI submission regression; keyboard constructor initialization order.
- `feb9f16b69`: scope kernel GPU line filter to Linux.
- `4c09ebf78c`: direct frame-timer include for Windows standalone compilation.
- `0b3109bcbb`: restore main light-preparation stage guard locals used by the retained sun/emissive/occlusion scheduling path.
