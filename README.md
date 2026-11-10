# Slipstream Viewer

<img src="indra/newview/icons/slipstream/png/slipstream-icon-128.png" width="96" alt="Slipstream">

A third-party viewer for Second Life, built around a **GPU-driven Vulkan renderer**. Slipstream began
as a fork of Alchemy Viewer (itself built on Linden Lab's viewer) and is now developed independently:
the render pipeline has been rewritten from scratch, and features from other open source viewers,
plus our own, are folded in.

> This software is not provided or supported by Linden Lab, the makers of Second Life.

> **Status:** beta. Linux x86_64 and Windows x64. Please report problems on the
> [issue tracker](https://git.slipstreamviewer.org/sky_line/Slipstream/issues).

- Website and downloads: <https://slipstreamviewer.org>
- Help wiki: <https://slipstreamviewer.org/wiki/>
- Privacy policy: <https://slipstreamviewer.org/privacy.html> (we collect no data)

## Why

Second Life viewers traditionally render the way they did in 2007: the CPU walks the scene every
frame, binds state and issues draw calls one face at a time. In busy places (clubs, crowds, mainland)
the CPU becomes the bottleneck long before the GPU does. Slipstream moves that work onto the GPU.

On an RTX 5090, a busy scene that runs around **24 FPS** in a conventional GL-based viewer runs around
**90 FPS** here; quieter scenes hit the monitor's refresh rate. Numbers vary a lot by scene and
hardware; these are indicative, not a benchmark.

## The render engine

The Vulkan renderer owns the scene on the GPU. The CPU's per-frame job shrinks to streaming *changes*
and dispatching a handful of compute and indirect draws.

- **Persistent GPU scene.** Every visible face lives in a persistent instance buffer (transform,
  material, bounds, flags) and a shared vertex/index buffer. Objects are uploaded when they change,
  not every frame.
- **Bindless materials and textures.** One large descriptor table holds every resident texture, and
  materials index into it. There are no per-draw binds.
- **Compute culling + multi-draw indirect.** A compute pass does frustum culling, LOD
  selection and bucket sorting (opaque, alpha-mask, alpha-blend, rigged, …) and writes indirect draw
  commands. The whole world draws in a few `vkCmdDrawIndexedIndirectCount` calls.
- **Depth pre-pass.** A depth-only pass over the same indirect draws, then the lit pass with
  depth-equal tests, so every pixel is shaded exactly once.
- **Clustered local lighting.** Point lights and projectors are culled into screen-space clusters.
- **GPU skinning and animation.** Skin palettes and other avatars' animation are evaluated on the GPU,
  with a pre-skin compute pass feeding the rigged draws. Flexible prims are skinned on the GPU too.
- **Streaming that follows visibility.** Texture residency and quality follow what the GPU culler
  actually sees, with async uploads, a staging ring and a VRAM budget.
- **Modern post-processing.** HDR with seven tone mappers, bloom, SSR, reflection probes, planar
  mirrors, SSAO / GTAO ambient occlusion, depth of field, and FXAA / SMAA / TAA with temporal upscaling.

The legacy OpenGL pipeline has been removed.

## Highlights

- **Hardware ray tracing (RTX 20-series / RX 6000 and newer):** ray-traced sun, moon and projector
  shadows with adjustable softness. They're exact, don't leak through walls, and keep up with moving
  stage lights.
- **Shadow maps done properly:** cascaded sun shadows with Gaussian or contact-hardening (PCSS)
  filtering, and a large, cached projector-shadow budget.
- **Photo Studio**: colour grading, curves, scopes, tone mapping, depth of field, sky
  effects.
- **Third-person shooter camera** (Preferences → Move & View), pie menus, live font switching with
  **Add Font…**, sim-wide @mentions, snapshot presets up to 10240 × 4320, and **Import from Another
  Viewer** for your old settings.

See the [features page](https://slipstreamviewer.org/wiki/features/) for the full list.

## Requirements

- **Linux:** x86_64, glibc 2.38 or newer, an x86-64-v2 CPU.
- **Windows:** Windows 10 or 11, x64.
- **GPU:** a Vulkan 1.3 driver: NVIDIA GTX 750 / 900 series or newer, AMD RX 400 series or newer
  (any GCN Radeon on Linux), or Intel HD/UHD 500 / 600 series or newer.
- Ray-traced features need `VK_KHR_ray_query` (NVIDIA RTX 20-series+, AMD RX 6000+). Without it,
  everything else still works.

## Download

Builds are published on the [website](https://slipstreamviewer.org/#download) and as
[releases](https://git.slipstreamviewer.org/sky_line/Slipstream/releases), each with a SHA-256
checksum.

- **Windows:** run `Slipstream_<version>_Setup.exe` (per-user install, no admin prompt), or unzip
  `Slipstream_<version>_win64.zip` and run `SlipstreamViewer.exe`.
- **Linux:** unpack `Slipstream_<version>_x86_64.tar.xz` and run `./slipstream`.

## Building

Slipstream builds on Linux; the Windows version is cross-compiled from Linux with clang-cl and xwin.

```sh
make            # venv + dependencies + configure + build + package
make run        # launch the packaged viewer
make windows    # cross-build the Windows x64 package
make help       # every target and override
```

The first build downloads about 1 GB of prebuilt third-party libraries. Package lists and the
Windows toolchain setup are on the wiki's
[building page](https://slipstreamviewer.org/wiki/building/).

## Credits and license

Slipstream is built on [Linden Lab's Second Life viewer](https://github.com/secondlife/viewer) and
the work of the [Alchemy Viewer](https://github.com/AlchemyViewer/Alchemy) project, and includes
RLVa. Some features are ported from other open source viewers:

- [Firestorm Viewer](https://www.firestormviewer.org/) (The Phoenix Firestorm Project, Inc. and
  contributors, including Ansariel Hiller, Cinder Roxley and Techwolf Lupindo): area search,
  teleport history, LSL preprocessor, virtual trackpad, pie menus, the Contacts window. The LSL
  preprocessor is Copyright (c) 2010 Modular Systems (BSD license; see
  `indra/newview/licenses-*.txt`).
- [Black Dragon Viewer](https://github.com/NiranV/Black-Dragon-Viewer) by NiranV Dean: camera tools,
  camera recording and playback, bone camera and head tracking, depth of field focus modes,
  right-click-drag orbit.

Original copyright headers are kept in the ported files. Source is licensed under the LGPL 2.1; see
`doc/LICENSE-source.txt` and `doc/LGPL-license.txt`. Second Life and related marks belong to Linden
Research, Inc. Slipstream is not affiliated with or endorsed by Linden Lab.
