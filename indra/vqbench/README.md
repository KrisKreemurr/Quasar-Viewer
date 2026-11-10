# vqbench: #1039 visibility-queue timing bench

A standalone Vulkan bench that times the viewer's lighting kernels on a reproducible synthetic scene: the GI split
with its hits through the visibility queue (the viewer's default chain) and pass 15's split shadow core, against
the queue's A/B configurations. You don't have to stand at a spot in the viewer. It is not part of the viewer
build.

## Build

    cmake -S indra/vqbench -B build-vqbench -G Ninja
    ninja -C build-vqbench

`vqb_spv.py` imports `indra/vkclean/regen_world_spv.py` and compiles each kernel through `spvbuild.compile_many`
with the table's flags and `-DCLEAN_STAGE_COMP`. Those are the same sources, defines and `glslc -O` the viewer
embeds. It also writes an unoptimised `.names.spv` twin of each kernel, which the bench reflects names from. The
build re-runs whenever a viewer shader changes. `spv/kernels.txt` lists every kernel's flags and the shader-set
hash, which the report prints. For an A/B of a shader change, compile a copy of `indra/vkclean` into a separate
set (`VQB_VKCLEAN=<copy> python3 indra/vqbench/vqb_spv.py --out DIR`) and run the bench with `--spv DIR`.

## Run (GPU lock)

    indra/vqbench/vqb_run.sh build-vqbench/vqbench [options]

`vqb_run.sh` refuses to start, with exit code 3, if either of these holds:
- `~/.cache/render2-gpu.lock` exists;
- a `do-not-directly-run-slipstream-bin` process is running. It is found through `/proc/*/exe`, not `pgrep`.

Otherwise it creates the lock atomically (containing `vq-bench`), checks again for the viewer, runs, and removes
the lock on exit. Never run `vqbench` directly while the owner is testing.

    vqb_run.sh build-vqbench/vqbench                               # the default A/B: configs 0,64
    vqb_run.sh build-vqbench/vqbench --configs 0,256,512           # the GI queue policies (image-changing)
    vqb_run.sh build-vqbench/vqbench --configs 0 --no-shadow       # the GI alone
    vqb_run.sh build-vqbench/vqbench --frames 400 --rounds 6 --csv /tmp/x.csv
    build-vqbench/vqbench --dry                                    # no GPU: the layout + every named field

| Option | Meaning |
|---|---|
| `--configs a,b,..` | Configs: 0 = the viewer's default chain, plus the bits below (**Config bits**). |
| `--frames N` / `--warmup N` / `--rounds N` | Timed frames per config per round (200), untimed frames before each block (60), and round-robin rounds (4). Configs are interleaved so clock and thermal drift falls on all of them equally. |
| `--no-gi` / `--no-shadow` | Leave a consumer out. |
| `--iso-lod` | The shadow walk kernels' iso-LOD variants (`RenderVKShadowIsoLod 1`). |
| `--sun-rays N` | Sun disc rays, `ssr.sun_rt.y`. The default is 1, which is the viewer's `vkRTShadowRays()`. The disc radius is `RenderVKRTShadowSoftness` 0.01 x 5° = 0.05°, and the projectors use a 0.01 m radius with 1 ray. |
| `--serialize` | Adds a barrier after every dispatch, so each kernel's time can be attributed to it. The totals come out slightly inflated. Off by default, which is the viewer's launch shape: compare totals there. |
| `--gi-k N` | GI hit shadow-ray budget K (`RenderVKGIHitShadowRays`, default 1). |
| `--size WxH` | Render extent (default 2560x1440). GI and the denoiser run at half extent. |
| `--lights P,J` | Point lights and projectors (default 48,16; at most 64). |
| `--seed N` | Scene seed (default 1039). |
| `--no-verify`, `--csv FILE`, `--device N`, `--no-stats` | Self-explanatory. |
| `--dump-dir DIR` | Writes each config's pdn_vis visibilities (`mask-N.vis`) after verify. |
| env `VQB_VERIFY_FRAMES=N` / `VQB_DUMP=1` | Frames per verify run (default 4; 2 = the first reuse frame only) / print the first differing reservoir records with the texel's GI record q4 / q5 and its `uGiGeom` words. |

### Config bits

0 is the viewer's default: the GI hits through the queue (M2: HIT-MAT, HIT-REQ, the walk, HIT-LIGHT) in the frame
walk, the shadow core fused. The bits stand in for the queue's A/B settings in the viewer.

| Bit | Viewer setting | Effect in the bench | Image |
|---|---|---|---|
| 64 | `RenderVKVisQueue` 64 | the GI hits on M1 (HIT-MAT queues the sun ray alone, HIT-LIGHT keeps the light walk) | identical to 0 |
| 256 | `RenderVKVisQueueGISunOpaque` 1 | GI region policy, header word 5 `.y` = 1: the GI hits' sun rays walk OPAQUE (#981) | changes by design |
| 512 | `RenderVKVisQueueGIMinShare` | GI region policy, header word 5 `.x` = `--gi-min-share` (default 0.02) (#983) | changes by design |
| 1024 | `RenderVKGIReuseVisSkip` 1 | the GI header's GI_F_VIS_TSKIP (#985). The scene has no change spheres, so every temporal pick skips its ray | changes by design |

The RT reflections (`RenderVKVisQueue` 2) are not in the bench. Gone from the viewer, and so from the bench: the
fused GI hit shading as an A/B (it is only the fallback now), the GI reuse queue (old bit 4), the shadow core queue
(old bit 8), the frame walk's switch (old bit 16) and its schedule setting (`RenderVKVisQueueLevels`). The old
bench's mask 19 is this bench's 0.

## What it prints

1. The register count, local memory and SPIR-V words of every kernel. These come from
   `VK_KHR_pipeline_executable_properties` with spec constant 1 = 0, the same as regprobe and the viewer's
   VKPipeStats.
2. **VERIFY.** Each config runs from a zeroed state through frames 0..3, then prints these, with `*` marking a
   value that differs from the first config:
   - the HIT / MISS list counts;
   - the shadow core's job count;
   - the GI rays and the reuse-rejected count;
   - the GI region's request count;
   - hashes of the GI records (q0..q3 per texel), the GI reservoirs, the pdn_vis visibilities, `uPdnIn` (the sun
     channel) and `uGiIn`.

   It then prints, for each config, how far it is from the first one: the differing texels with the largest
   absolute and relative difference, per image layer and channel, and the reservoir records that differ. The
   first config is run once more at the end to check the harness itself is deterministic.

   The design claims 64 is bit-identical to 0, so a `*` there means one of two things: the harness feeds the two
   paths different inputs, or the claim does not hold. Settle that before you trust a timing. A zero count means a
   gate is closed, and then the kernel is timing nothing.
3. **TIMING.** The median, p10 and p90 in ms of every mark and of the GI, shadow and frame totals per config, then
   a delta table against the first config. A mark is the time since the previous mark, which is the viewer's
   `gpuSubMark`. The mark names are the viewer's `c_*` timer names: `c_gi_trace`, `c_gi_hitmat`, `c_gi_hitreq`,
   `c_gi_walk`, `c_gi_hitlight`, `c_gi_reuse`, `c_gi_reshade`, `c_gi_tail`, `c_defer_shadowsel`, `c_defer_shadowsun`,
   `c_defer_shadowvis`, `c_defer_shadowmerge` (SHADE), and `c_vq_prod`.

## What runs (it mirrors the host code)

- **Shadow core** (`llcleanprojstochvk.cpp` recordProjStoch, split, merged sun; recordShadowMergeFirst records it
  whole, ahead of the frame walk):
  - SELECT, then SUNVIS + VIS, then SHADE.
  - Pushes: `rtr = (D, frame, 1 | parity << 4)`, `rtr2 = (1, 0.25, 41, 0)`, `rtr3 = (0, 0, 0, S = 8)`.
- **GI** (`llcleangivk.cpp` recordGI + recordGISplitReuse, split + shade split, through `llcleanvisqueuevk.cpp`
  THE FRAME WALK):
  - THE LEVELS: TRACE (`rtr.z` = 1), then MISS + HIT-MAT, then [HIT-REQ (M2)], ONE barrier between two (compute ->
    compute + indirect + copy, the host's mask). M2's HIT-MAT is the fused stage's kernel, bound with the queue's
    push; M1 (bit 64) binds HIT-MAT-VQ.
  - THE WALK WAVE: one barrier, the walk, one barrier, then the head: HIT-LIGHT-VQ2 (M1: HIT-LIGHT-VQ).
  - Then REUSE, RESHADE and its TAIL.
  - The barriers and pushes are the same as the host's: `rtr = (64 m, K << 1, ..., 16)`,
    `rtr2 = (D, frame, parity)`, and `rtr3.zw` = the region.
  - The RT reflections are not in the bench, so their stages (levels 1-4) are absent.
- **The region header** is written in the `prep` mark, as the host's light prep does (visQueuePrepHeaders).
- **The region's size.** The GI region is sized WHOLE (exact fit), so no entry falls to the fused tail. The viewer's
  cap (`RenderVKVisQueueMB`) is not modelled.

## Scene

The scene is seeded (`--seed`) and z-up, in region space around (128, 128, 22):
- a 180 m terrain-like grid (opaque, but classed as a plain prim, not terrain);
- 28 buildings, a third of them with an overhanging roof slab;
- 40 trees: a bark trunk plus a crown of 56 alpha-masked cards from a 512² leaf atlas;
- 10 alpha-masked lattice fences;
- 14 tinted translucent glass panes (blend, base alpha 0.55 x 0.6 tint);
- 48 point lights and 16 projectors at 0.6 to 6 m height, with radius 5 to 14 m;
- the sun at 38° elevation;
- a street-level camera with a 60° vertical FOV.

The geometry is in the viewer's RT layout:
- the quantized QVertex VBO with its slot headers;
- 16-bit indices;
- `rt_geom` and `rt_geom_vc`;
- the RtInstance / RtMaterial records;
- TLAS custom index = the instance id, mask = the class (opaque 0x01, alpha-mask 0x02, blend 0x04), and
  FORCE_OPAQUE / FORCE_NO_OPAQUE as clean_rt_instances.comp sets them.

The G-buffer is ray-cast from the same TLAS by `shaders/vqb_gbuf.comp`, with the mask cut-outs alpha-tested.

## Trust: what the numbers mean

**Trustworthy:** A vs B on this scene, i.e. the difference between two configs. This holds for:
- the totals per consumer (GI total, shadow total);
- the kernels that differ between paths: the producers (HIT-MAT / HIT-REQ), the walk and the resolves (HIT-LIGHT).

These are the viewer's binaries on real traversals of mixed opaque, alpha-tested and translucent geometry, with
the viewer's launch shapes and barriers. The register / local memory figures are exact.

**Not trustworthy as viewer times:** the absolute milliseconds. The scene is not the owner's spot. Ray
coherence, the light count per receiver and the hit material mix all differ.

**How it submits.** Eight frames go in one submit. The GPU never idles between frames, so the clocks stay up. Each
frame opens with a full barrier, as the viewer's chain does after its prep. A total is the median of each frame's
sum, not the sum of the kernels' medians.

**Per-kernel splits inside one barrier-free group are approximate.** Dispatches with no barrier between them
overlap: MISS with HIT-MAT, SUNVIS with VIS. The later mark then absorbs the overlap. Use `--serialize` to get
attributable per-kernel times.

### Where the bench approximates (and why each is A/B-neutral)

- **Light lists.** Every light (at most 64, projectors first) is in every cluster, and each light's own range
  test decides. The viewer culls its lists per cell, so receivers here test more lights. Both paths walk the same
  list.
- **Pass-15 light samples.** These come from the 16 RT projectors only (pshadow `params.z` 2). Point lights get
  RT shadows in pass 15 only through ReSTIR DI, and ReSTIR is off here (`foliage_rt.x` bit 64 off, rsFrameOn
  false: no RegirGrid population). Point lights still enter the GI hit budget (GI_F_POINT_SHADOW).
- **The world-space light grid.** It is off: `rtr.y` bit 4 is clear, so the hits walk the cluster list.
  - The viewer sends it on (RenderVKLightGrid 1).
  - HIT-REQ's light walk then runs over the cluster list here, not the grid's picks.
- **GI radiance cache.** It is off (GI_F_CACHE / CACHE_FILL cleared). The bench does not run recordGITail (the
  cache resolve or the denoiser), so a cache would never fill. All other GiHeader values are the defaults from
  giWriteHeader, GI_F_STATS included, because the owner's timers were on.
- **Sun certificates (#771).** They are off (`rtr3.xy` = 0).
- **Split lights.** They are off (splitK 0).
- **Sky, probes, AO, clouds, weather, media, atmosphere LUTs.** These are zero placeholders, and their gates are
  off (probe count 0, physky off). The haze and sun colours are plain constants.
- **G-buffer formats.**
  - All four layers are RGBA16F, and depth is R32F reverse-Z. The viewer uses RGBA8 sRGB / RGBA16F / RGBA8 /
    B10G11R11 and D32.
  - The same values are decoded.
  - The texel-fetch bandwidth differs slightly, and equally for every config.
- **Set 2 binding 10.** It is a plain UBO, where the viewer uses a dynamic UBO.
- **The camera.** It is static and unjittered (`taa_params` 0).
- **Avatars.** There are no rigged avatars, no cluster LOD and no terrain records.

## Files

| File | Role |
|---|---|
| `vqb_spv.py` | Compiles the kernels (the viewer's flags) and the bench's own shader. |
| `vqb_reflect.*` | SPIR-V reflection: the bindings, plus block member offsets by name. |
| `vqb_vk.*` | Device (every supported feature on), buffers, images and one-shot submits. |
| `vqb_scene.*` | The seeded scene, the RT tables, BLAS / TLAS and textures. |
| `vqb_main.cpp` | Resources per reflected binding, the uniform blocks by name, the chains, verify and timing. |
| `vqb_run.sh` | The GPU-lock wrapper. |
