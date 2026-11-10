# Render benchmark

A repeatable way to time the Vulkan renderer: capture a scene once, then replay it with no
login and no network, along a recorded camera path, and get a JSON report you can diff
against another build (#1304).

Everything the benchmark writes lives under `~/.slipstreamviewer/bench/` (setting
`BenchRoot`). Captures hold other people's content, so the viewer refuses any output
directory inside a source checkout (a `.git` in it or above it). Never commit captures or
reports.

## 1. Capture a scene

Log in, go somewhere, wait until it has loaded, then either:

- **Develop > Render Benchmark > Capture Scene**, or
- `scripts/testctl/slctl.py bench capture seconds=10 name=mall` (a `make TESTCTL=1` build).

The capture records `BenchCaptureSeconds` (10 s) of tracks, then gathers textures, then
writes `captures/<name>/` on a worker thread. `slctl.py bench status` shows the phase. Move
the camera during the capture if you want its path. Lights and moving objects are recorded
too.

What is in a capture: the renderer's own inputs, never LL's object layer. That means the
clean-store instance rows (static scene, terrain, water, trees), their LOD groups, the
packed geometry mirror of every slot, materials, terrain records, the resolved sky / water /
cloud / atmosphere inputs (plus the sky and water settings themselves, which a replay makes
the local environment), reflection probe data, the local lights for every sample, the poses
of objects that moved, the camera, and the decoded pixels of every referenced texture.
Textures without a CPU copy (most of them once the GPU holds them, or ones served from the
GPU-ready disk cache) are decoded again from the local cache through LL's loaded-callback
path, at the level the scene shows; that can take a few seconds (`textures_pending` in
`status`, 30 s limit). Textures still missing after that are counted in the manifest.
`manifest.json` lists every file with its size and XXH64; a replay refuses a capture whose
files don't match.

**Not captured yet** (also listed in each manifest under `missing`):

- avatars: rigged rows, skins, joint and pose tracks (stage 2: needs the skin store and the
  per-frame palette fed without `LLVOAvatar`)
- HUD and worn rigid attachments
- particles (emitter state)
- objects that appear after the capture starts
- environment changes during the capture (the first frame's sky and water are kept)
- light-takeover face links

The memory budget is `BenchCaptureBudgetMB` (2048). Geometry over it fails the capture;
tracks are cut short and textures left out past it (`truncated` in the manifest).

## Skins and pose tracks (capture format version 2)

The viewer's own capture writer produces format version 1 (no avatars). Version 2 is an
additive extension for captures made outside the viewer (render2's Blender exporter,
`render2/blender/`): the replay reads both. A version 2 `manifest.json` has `"version": 2` and
every `.bin` header carries version 2. Two optional files are added; without them a version 2
capture is an ordinary static one. The byte layouts are in `indra/newview/slbenchscene.h`
(`SkinRec`, `PoseHdr`), which is the authority.

- **Rigged rows**: an instance row with flag bit 0 (`kFlagRigged`) whose `skin_id` is a capture
  skin id from `skins.bin`. Its LOD-group slots carry weights (`has_weights`, 10 B QWeight:
  4 x u8 joint index into the skin's joint list, 3 x u16 weights, the 4th implied so the sum is
  65535; at most 4 influences per vertex, bit 7 of the 4th joint byte is reserved). Vertices are
  in the mesh's own space; the skin's `bind_shape` takes them into bind space.
- **`skins.bin`**: per skin the armature it reads (a 216-joint joint table), `joint_count`
  (1..110), `joint_nums` (armature joint index per skin joint), `inv_bind` (mat4 per joint,
  column-major) and `bind_shape`. One armature serves every rigged mesh of a character.
- **`poses.bin`**: per sample (rate `track_hz`, as lights and movers) the joint WORLD matrices
  of the armatures whose pose changed, agent space, 12 floats per joint (the affine matrix's upper
  3x4, row-major: the skin store's own joint-table layout). Sample 0 lists every
  armature in use. Held between samples, looped, advanced from the fixed-step world time, so a
  replay is deterministic.
- **Skinning**: `palette[j] = jointWorld[armature][joint_nums[j]] * inv_bind[j]`,
  `skinned = sum_k w_k * palette[j_k] * bind_shape * v` (what the live pre-skin computes).
- **Limits**: 110 joints per skin, 216 per armature, 4096 skins, 256 armatures. The reader
  checks every count and size against the file before allocating, and refuses a capture with a
  rigged row naming an unknown skin, a skin naming a missing armature or joint, or non-finite
  matrices (the replay stops with an error line instead of drawing).
- Replay log lines (tag `Bench`): `replay: skins: N skins, M armatures, K pose samples,
  R rigged rows` at load and `replay: injected ... skins S (failed F), armatures A` at the end
  of injection.

## 2. Record a camera path

In world, with the normal camera or the flycam:

    slctl.py bench path_start hz=10 segment="slow pan"
    ... fly ...
    slctl.py bench path_segment name="fast strafe"
    ... fly ...
    slctl.py bench path_key still=64          # a noise sample point (see the report)
    slctl.py bench path_stop file=~/.slipstreamviewer/bench/paths/mall.json

(or Develop > Render Benchmark > Start / Add Keyframe / Stop.) `hz=0` records only the keys
you add. Paths are agent coordinates, so record them in the region of the capture.

Path files are plain JSON and easy to write by hand:

```json
{ "format": "slipstream-bench-path", "version": 1,
  "keys": [
    { "t": 0, "pos": [128, 90, 30], "focus": [128, 128, 25], "fov": 1.05, "segment": "slow pan" },
    { "t": 6, "pos": [160, 90, 30], "focus": [140, 128, 25] },
    { "t": 8, "pos": [170, 100, 28], "focus": [150, 140, 25], "segment": "fast strafe", "still": 64 },
    { "t": 10, "pos": [230, 100, 28], "rot": [0, 0, 0.38, 0.92] }
  ] }
```

`rot` is the camera frame quaternion (x, y, z, w; at = +X, left = +Y, up = +Z) or use
`focus` for a look-at point. `fov` is vertical, in radians. `segment` names the stretch from
that key until a key names another one; the report breaks every number down per segment.
`still: K` holds the camera at that key for K frames (not timed) for the noise measurement
(on every loop, the first key included).
Playback is smooth: Catmull-Rom positions over the key times, squad rotations, linear fov.

## 3. Run

Replay (the deterministic one):

    build-linux-x86_64/newview/packaged/slipstream --bench ~/.slipstreamviewer/bench/captures/mall \
        --bench-path ~/.slipstreamviewer/bench/paths/mall.json --bench-loops 3

- No login, no network: the viewer parks before the login screen and renders the capture.
- Waits for the world pipelines, holds the first pose for 300 frames (streaming settles),
  then times the path `--bench-loops` times, writes the report and quits.
- World time steps by exactly 1/60 s per frame and the main-thread RNG is seeded, so two runs
  render the same sequence of frames; lights and movers follow their tracks (looped).
- Without `--bench-path` it flies the camera recorded during the capture.
- `--bench-cold`: this session uses no disk pipeline cache (your cache files stay as they
  are), so the report's pipeline section is a cold-compile total. The driver's own shader
  cache is still warm: for a fully cold run also start with `__GL_SHADER_DISK_CACHE=0`
  (NVIDIA) or `MESA_SHADER_CACHE_DISABLE=true` (Mesa).
- `--bench-report FILE` picks the report path (default `reports/<stamp>-<capture>.json`).

### Either pipeline: `--bench-pipeline current|render2`

`--bench-pipeline render2` replays the same capture with render2 (the clean-room renderer,
`render2/`, built into the viewer as a library when CMake `SL_RENDER2` is on: Linux default,
needs volk, nlohmann/json and glslc) instead of the viewer's own world renderer. Default
`current`. Everything else is the same run: path, fixed 1/60 s step, seed, warm-up, loops,
still keys and the report file, so `compare.py` and `render2/tools/report.py` diff them.

- One host, one device: any bench replay of a viewer built with render2 creates the device
  with what render2 needs too (scalar block layout, update-after-bind sampled images, host
  query reset, int64 / 8 / 16-bit, memory model, where supported), for both pipelines; a
  normal session's device is unchanged. `render2` also turns ray tracing on in the device.
- With `render2` the viewer's world renderer is never created: no pipeline compiles, no world
  frame recorded or submitted. render2 records and submits its own frame on the viewer's
  graphics queue; the viewer's frame blits render2's linear output into the window image
  through an sRGB image (the same encode render2's lab window uses; no tonemap yet) and
  draws the UI over it. The run starts when render2 has queued and published the capture.
- Inputs: the capture the replay read and verified (its decoded records, handed over once;
  texture PNGs decoded by render2 from the same files). **Environment:** both pipelines use
  the environment the viewer's world frame is built with: env.bin when it has an atmosphere
  block, else LLEnvironment (the capture's sky.xml, else the default sky) with its sun turned
  to the capture's env.bin sun when that is above the horizon (an exported capture names one), and that sun's direction and colour lighting the frame; render2 gets it
  each frame as an env.bin record (`SLBenchScene::envFromFrame`), never the raw zero block of
  an exported capture. **Rigged rows** (version 2) are skinned by render2 with palettes the
  viewer builds from its own evaluation of the pose track (the record in effect at the world
  time, held until the next one, as the current pipeline's skin store gets it), so both
  pipelines pose every frame alike. render2's lab interpolates poses between samples instead;
  the hosted path does not.
- Stills are the mean over the hold, and the world clock (poses, movers, lights) keeps running
  during a hold in both pipelines, so anything animated shows as overlapping copies (render2:
  sharp copies of each held pose sample; the current pipeline: smeared by its temporal filter).
  render2's lab writes the hold's last frame instead, so its stills show no such copies.
- Report: `"pipeline": "render2"`, render2's passes in `passes` as `r2:<pass>` (its frame
  total in `gpu_ms`), its counters in `counters` as `r2.<name>` (`last`, `max`, `total`),
  its own sections (capture_info, caps, scene, pools, ray ledger, frame graph) under
  `render2`. Stills: the window image after the composite, before the UI (as the current
  pipeline's world image).

Both, from the packaged viewer (`render2/tools/bench.py run --pipeline old --bench-pipeline
render2 ...` drives the second):

    slipstream --bench CAPTURE --bench-path PATH --bench-loops 3 --bench-report A.json
    slipstream --bench CAPTURE --bench-path PATH --bench-loops 3 --bench-report B.json --bench-pipeline render2
    scripts/bench/compare.py A.json B.json

### Bench overlay

A replay shows a top-left debug panel (both pipelines; `BenchOverlay`, default on; key
command `bench_overlay`, Ctrl+Alt+B, toggles it): pipeline, GPU, resolution; fps, frame and
GPU ms; the six costliest passes (smoothed GPU ms); path segment, time / duration, run state
and loop; scene counts (render2: instances, drawn, triangles, lights; current: instances
uploaded and lights, its draw counts are not read back); and every non-zero overflow / drop /
unavailable counter. It only shows what the run already reads back and draws with the UI,
outside both pipelines' GPU timings.

Keep the window focused and visible: an unfocused viewer runs at the background FPS cap
(the report counts unfocused frames). During a run the benchmark turns on GPU timers and
turns off vsync and the FPS limit for the session (not saved), and restores them after.

Live run (your current scene, real time, not deterministic):

    slctl.py bench run path=~/.slipstreamviewer/bench/paths/mall.json loops=2

`slctl.py bench restart` restarts the path and drops what was measured (this is also the
hook hot shader reload calls); `slctl.py bench abort` stops and reports what it has.

## 4. The report

    { "format": "slipstream-bench-report", "version": 1, "ok": true,
      "mode": "replay", "capture_id": "...", "gpu": "...", "window": [w, h], "settings": {...},
      "segments": [ { "name": "slow pan",
                      "frame_ms": { "n", "avg", "p50", "p95", "p99", "max" },
                      "gpu_ms":   { ... },
                      "passes":   { "<every named GPU sub-timer>": { ... } } } ],
      "overall": { ... },
      "vram": { "usage_mb_peak", "usage_mb_end", "budget_mb", ... },
      "pipelines": { "cold", "wait_s", "total_pipelines", "compiled", "wall_ms", "sum_ms",
                     "batches": [ every pipeline batch report of the session ] },
      "noise": [ { "key", "segment", "frames", "temporal_variance", "max_pixel_variance",
                   "psnr_db", "mean_image" } ] }

Frame time is the wall time between frames; GPU numbers come from the renderer's timer
harvest (a few frames late, so a segment boundary can shift by that much). Noise: at each
`still` key the world image (post tonemap, before HUD and UI) is read back for K frames;
`temporal_variance` is the mean per-pixel variance across them (0..1 units), `psnr_db` the
PSNR of a frame against their mean, and `mean_image` that mean as a PNG next to the report.

## 5. Compare

    scripts/bench/compare.py base.json new.json [--pct 5] [--ms 0.2] [--psnr-db 1]

Flags a regression when a number is worse by more than `--pct` percent and `--ms`
milliseconds (per segment and overall: frame p50 / p95 / p99, GPU avg / p95, every pass),
VRAM peak +5% and +64 MB, cold compile wall time +10%, and a still-key PSNR drop over
`--psnr-db`. When both runs wrote still images it also reports the PSNR between them (the
picture changed). Exit status 1 = regression, so it can gate a script. `--json` for machines.

## Sealed-room check (light through walls)

`scripts/bench/sealed_room.py` is a correctness bench, not a timing one. It writes two synthetic
captures of a closed room, with walls and ceiling standing on a floor slab shared with the space
around it and no opening anywhere. In `lit`, a point light, a projector and an emissive panel stand
6 m outside the walls. In `dark` they are absent. Both are replayed with ray tracing, GI 3 and
ReSTIR DI, at fixed exposure and from the same camera inside the room. Any difference between the
two still images is light that came through a wall:

    scripts/bench/sealed_room.py run --viewer build-linux-x86_64/newview/packaged --out /tmp/sealed

It prints `sealed-room: PASS|FAIL|SKIP` and writes `sealed_room.json`. The leak is the mean
luminance difference in 0..255 units, and the limit is `--max-leak` (0.5). SKIP means the replay ran
without RT GI or ReSTIR DI, for example on a GPU without ray tracing. `--set KEY=VALUE` adds an A/B.
The run uses its own `SLIPSTREAM_HOME`, so nothing of yours is touched. `slvalidate.sh ... core`
runs it on the test box before every beta. Its CPU half is the `hit-nee` source check.

## Determinism: what is and is not pinned

Pinned: world time (fixed step), the main-thread `ll_frand` / `std::rand` seeds, the scene
(identical store contents, ids aside), the camera per frame, the track samples per frame.

Not pinned: worker-thread timing (texture encode / upload completion, cluster builds, async
pipeline compiles) — the warm-up absorbs most of it; the renderer's own frame counter (TAA
jitter phase) starts wherever the login-screen frames left it; GPU timing itself.
