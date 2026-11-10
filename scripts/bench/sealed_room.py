#!/usr/bin/env python3
# Copyright (C) 2026, Slipstream contributors.
# This library is free software; you can redistribute it and/or modify it under the terms of
# the GNU Lesser General Public License as published by the Free Software Foundation;
# version 2.1 of the License only.
"""SEALED-ROOM BENCH: does light reach a closed room through its walls? (GPU part of the hit-nee check.)

A closed box room (walls and ceiling 0.15 m thick on a floor slab shared with the rooms around it: no opening) with,
a GLOSSY TILED floor and far wall (the camera's view: its RT reflections see the room's other walls) and an
alpha-blended glass pane standing in the room (the forward path), and,
OUTSIDE it and 6 m from its walls, a pink point light, a projector aimed at the room and an emissive
panel facing it. The camera stands inside. Two captures differ ONLY in those three: `lit` has them,
`dark` has none. Everything else -- sun, sky, probes, GI -- is the same, so

    leak = mean(lit still) - mean(dark still)

is exactly the light the outside lights bring into the room. Correct is 0 (no direct light, and the
GI has nothing inside to bounce). The check fails when the leak's mean luminance passes --max-leak
(post-tonemap 0..255 units, fixed exposure).

THE GLASS (the forward GI fallback, clean_surface_light.glsl giFwdScreenOcc): the sky outside is LIT (a
day sky and sky ambient, no sun disc), so a probe / sky fallback that ignores the walls lights the pane --
the owner's shower glass in a sealed bathroom. The pane stands free in the room (no radiance-cache cell at
it), ~1.5 m of view depth in front of the far wall it is seen against. In the `dark` still its inner area (the pane
projected through the still's camera, shrunk toward its centre) must be as dark as the room: its mean
luminance at most --max-glass.

    sealed_room.py make OUT                    # OUT/lit, OUT/dark (captures), OUT/path.json
    sealed_room.py run --viewer PKG --out OUT  # make + replay both (the owner's RT settings) + compare
    sealed_room.py compare LIT.png DARK.png    # the comparison alone

`run` replays with SLIPSTREAM_HOME=OUT/home (its own settings and caches: nothing of the user's is
touched) and the settings of the owner's sealed-bathroom report: ray tracing, RenderVKGI 3,
RenderVKProjStochastic 1, RenderVKGIHitPointShadows 1, RenderVKReSTIREmissiveLightTakeover 0, RT
reflections on, dynamic exposure off. (The glossy tiles' reflection hits are lit by the outside lamps only
through a wall: their RT reflections must stay black -- the specular leak of the owner's bathroom.) OUT must lie outside any git checkout (the viewer refuses captures there).
Needs numpy (capture writer) and Pillow (comparison). One GPU run each, ~1 minute.
"""
import argparse
import json
import math
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sl_capture_core as core  # noqa: E402

# The room's INSIDE (agent metres, above the empty region). Walls 0.15 m thick and placed so that no
# radiance-cache cell boundary (0.25 m lattice near the camera) falls inside a wall: the floor cells
# along every wall hold floor from BOTH sides -- the case a cache must not carry light across.
ROOM_LO = np.array([120.2, 120.2, 30.0])
ROOM_HI = np.array([123.55, 123.55, 33.0])
WALL = 0.15
HOUSE = 8.0                                # the shared floor slab reaches this far past the walls
GAP = 6.0                                  # outside lights: this far from the wall they face
STILL_FRAMES = 96
SKY_AMBIENT = (0.30, 0.33, 0.40)   # (a day sky outside the shell: THE GLASS)
SKY_HORIZON = (0.62, 0.72, 0.86)
SKY_ZENITH = (0.25, 0.42, 0.75)
STILL_FOV = 1.1                    # (vertical, rad)
SETTINGS = {
    "RenderVKRayTracing": "1",
    "RenderVKGI": "3",
    "RenderVKProjStochastic": "1",
    "RenderVKGIHitPointShadows": "1",
    "RenderVKPointShadowMaps": "0",
    "RenderVKReSTIREmissiveLightTakeover": "0",
    "RenderVKRTReflections": "1",
    "RenderDynamicExposureEnabled": "0",
    "BenchFreezeStills": "1",
}


def translate(x, y, z):
    m = np.eye(4)
    m[:3, 3] = (x, y, z)
    return m


def rot_x(a):
    m = np.eye(4)
    c, s = math.cos(a), math.sin(a)
    m[1:3, 1:3] = [[c, -s], [s, c]]
    return m


def rot_y(a):
    m = np.eye(4)
    c, s = math.cos(a), math.sin(a)
    m[0, 0], m[0, 2], m[2, 0], m[2, 2] = c, s, -s, c
    return m


def box_matrix(lo, hi):
    c = (np.asarray(lo) + np.asarray(hi)) * 0.5
    e = np.asarray(hi) - np.asarray(lo)
    return translate(*c) @ np.diag([e[0], e[1], e[2], 1.0])


def cube():
    """A unit cube, outward normals (closed: every face is a real wall face from either side)."""
    pos, nrm, uvs, tan, idx = [], [], [], [], []
    for axis in range(3):
        for sign in (-1, 1):
            n = [0, 0, 0]
            n[axis] = sign
            u = [0, 0, 0]
            u[(axis + 1) % 3] = 1
            v = list(np.cross(n, u))
            c = np.array(n) * 0.5
            pts = [c + (a * np.array(u) + b * np.array(v)) * 0.5 for a, b in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
            b0 = len(pos)
            pos += [list(map(float, p)) for p in pts]
            nrm += [n] * 4
            tan += [u + [1]] * 4
            uvs += [[0, 0], [1, 0], [1, 1], [0, 1]]
            idx += [b0, b0 + 1, b0 + 2, b0, b0 + 2, b0 + 3]
    return {"positions": pos, "normals": nrm, "uvs": uvs, "tangents": tan, "indices": idx}


def quad():
    return {"positions": [[-.5, -.5, 0], [.5, -.5, 0], [.5, .5, 0], [-.5, .5, 0]], "normals": [[0, 0, 1]] * 4,
            "uvs": [[0, 0], [1, 0], [1, 1], [0, 1]], "tangents": [[1, 0, 0, 1]] * 4, "indices": [0, 1, 2, 0, 2, 3]}


def inside_view():
    """The still's eye and the point it looks at (agent metres)."""
    lo, c = ROOM_LO, (ROOM_LO + ROOM_HI) * 0.5
    return [c[0] + 1.0, c[1] - 1.0, c[2] + 0.3], [lo[0] + 0.2, c[1] + 0.8, lo[2] + 0.4]


def glass_matrix():
    c = (ROOM_LO + ROOM_HI) * 0.5
    return translate(c[0] - 0.5, c[1] + 0.3, c[2]) @ rot_y(math.pi / 2) @ np.diag([1.2, 1.5, 1.0, 1.0])


def glass_roi(width, height, shrink=0.35):
    """The pane's inner area in the still (pixel polygon): its corners pulled toward its centre by `shrink`,
    projected through the still's camera (SL frame: at +X, left +Y, up +Z; vertical fov STILL_FOV)."""
    m = glass_matrix()
    pts = [m @ np.array([x, y, 0.0, 1.0]) for x, y in ((-.5, -.5), (.5, -.5), (.5, .5), (-.5, .5))]
    pts = [p[:3] for p in pts]
    ctr = sum(pts) / 4.0
    pts = [ctr + (p - ctr) * (1.0 - shrink) for p in pts]
    eye, target = (np.array(v, dtype=float) for v in inside_view())
    f = target - eye
    f /= np.linalg.norm(f)
    left = np.cross([0.0, 0.0, 1.0], f)
    left /= np.linalg.norm(left)
    up = np.cross(f, left)
    t = math.tan(STILL_FOV * 0.5)
    aspect = width / float(height)
    poly = []
    for p in pts:
        d = p - eye
        z = float(d @ f)
        x = -float(d @ left) / (z * t * aspect)
        y = float(d @ up) / (z * t)
        poly.append(((x + 1.0) * 0.5 * width, (1.0 - y) * 0.5 * height))
    return poly


def scene(lit):
    lo, hi, w = ROOM_LO, ROOM_HI, WALL
    O = np.array([w, w, w])
    olo, ohi = lo - O, hi + O                       # the shell's outside
    c = (lo + hi) * 0.5
    # ONE floor slab under the whole house (the neighbouring rooms' floor is the same plane as this
    # room's), the walls standing on it, the ceiling slab over the walls' outer extent: every seam is a
    # face contact, nothing is open
    walls = [
        ("Floor", (olo[0] - HOUSE, olo[1] - HOUSE, lo[2] - w), (ohi[0] + HOUSE, ohi[1] + HOUSE, lo[2])),
        ("Ceiling", (olo[0], olo[1], hi[2]), (ohi[0], ohi[1], ohi[2])),
        ("WallXn", (olo[0], olo[1], lo[2]), (lo[0], ohi[1], hi[2])),
        ("WallXp", (hi[0], olo[1], lo[2]), (ohi[0], ohi[1], hi[2])),
        ("WallYn", (lo[0], olo[1], lo[2]), (hi[0], lo[1], hi[2])),
        ("WallYp", (lo[0], hi[1], lo[2]), (hi[0], ohi[1], hi[2])),
    ]
    # the floor and the wall the camera faces are glossy tile (RT-reflected: their reflections see the other
    # walls, whose hits the outside lamps reach only through the shell)
    glossy = ("Floor", "WallXn")
    objects = [{"name": n, "mesh": "tilecube" if n in glossy else "cube", "matrix": box_matrix(a, b)} for n, a, b in walls]
    # an alpha-blended glass pane in the room, in view, facing the camera's side (+X): the forward lit path
    objects.append({"name": "GlassPane", "mesh": "glass", "matrix": glass_matrix()})
    lights = []
    if lit:
        # +X: a pink point light at the room's mid height, GAP outside the +X wall, its radius well past the room
        lights.append({"name": "NextRoomLamp", "type": "point",
                       "matrix": translate(ohi[0] + GAP, c[1], c[2]) @ np.diag([0.2, 0.2, 0.2, 1.0]),
                       "color": [1.0, 0.45, 0.6], "intensity": 1.0, "radius": 20.0, "falloff": 0.5})
        # +Y: a projector GAP outside the +Y wall aimed at it (its -Z axis toward -Y), ambiance on
        lights.append({"name": "NextRoomProjector", "type": "spot",
                       "matrix": translate(c[0], ohi[1] + GAP, c[2]) @ rot_x(-math.pi / 2) @ np.diag([0.4, 0.4, 0.2, 1.0]),
                       "color": [1.0, 0.85, 0.4], "intensity": 1.0, "radius": 20.0, "falloff": 0.5, "fov": 1.2,
                       "focus": 0.0, "ambiance": 0.5})
        # -X: an emissive panel GAP outside the -X wall, facing it (+X)
        objects.append({"name": "NextRoomPanel", "mesh": "panel",
                        "matrix": translate(olo[0] - GAP, c[1], c[2]) @ rot_y(math.pi / 2) @ np.diag([2.0, 2.0, 1.0, 1.0])})
    def frame(eye, target):
        look = np.array(target, dtype=float) - np.array(eye, dtype=float)
        yaw = math.atan2(look[1], look[0])
        pitch = -math.atan2(look[2], math.hypot(look[0], look[1]))   # (SL frame: at +X, left +Y, up +Z)
        cy, sy, cp, sp = math.cos(yaw / 2), math.sin(yaw / 2), math.cos(pitch / 2), math.sin(pitch / 2)
        return [-sy * sp, cy * sp, sy * cp, cy * cp]                  # q_yaw(z) * q_pitch(y)
    # outside, in the lamp's room, looking at the floor along the +X wall (the replay's 300-frame warm-up
    # holds this pose: the cache fills with that floor's light), then inside for the still
    out_eye = [ohi[0] + 3.0, c[1] - 0.5, lo[2] + 1.5]
    out_rot = frame(out_eye, [ohi[0] + 0.2, c[1], lo[2]])
    eye, target = inside_view()
    rot = frame(eye, target)
    return {
        "name": "sealed room (%s)" % ("lit" if lit else "dark"), "seconds": 1.5, "track_hz": 30.0,
        "materials": [
            {"name": "plaster", "base_color": [0.8, 0.8, 0.8, 1], "roughness": 0.9},
            {"name": "panel", "base_color": [0, 0, 0, 1], "emissive": [1.0, 0.7, 0.3], "emissive_strength": 6.0},
            {"name": "tile", "base_color": [0.85, 0.85, 0.82, 1], "roughness": 0.06},
            {"name": "glass", "base_color": [0.8, 0.85, 0.9, 0.35], "roughness": 0.05, "alpha_mode": "BLEND"},
        ],
        "meshes": {"cube": [dict(cube(), material=0)], "quad": [dict(quad(), material=0)],
                   "panel": [dict(quad(), material=1)], "tilecube": [dict(cube(), material=2)],
                   "glass": [dict(quad(), material=3)]},
        "objects": objects,
        "lights": lights,
        # no sun (below the horizon, black) but a LIT day sky and sky ambient: nothing of it may reach the
        # sealed room -- the glass pane's probe / sky fallback included (THE GLASS above)
        "sun": {"direction": [0.2, 0.3, -0.93], "color": [0.0, 0.0, 0.0], "ambient": list(SKY_AMBIENT),
                "sky_horizon": list(SKY_HORIZON), "sky_zenith": list(SKY_ZENITH)},
        "camera": {"keys": [{"t": 0.0, "pos": out_eye, "rot": out_rot, "fov": 1.1, "segment": "outside"},
                            {"t": 0.5, "pos": eye, "rot": rot, "fov": STILL_FOV, "segment": "inside",
                             "still": STILL_FRAMES, "name": "inside"},
                            {"t": 1.5, "pos": eye, "rot": rot, "fov": STILL_FOV}]},
    }


def make(out):
    os.makedirs(out, exist_ok=True)
    for name in ("lit", "dark"):
        d = os.path.join(out, name)
        core.write_capture(scene(name == "lit"), d, overwrite=True, log=lambda *a: None)
    with open(os.path.join(out, "path.json"), "w") as f:
        json.dump(core.camera_path_json(scene(True)["camera"]), f, indent=1)
    return os.path.join(out, "lit"), os.path.join(out, "dark"), os.path.join(out, "path.json")


def lum_mean(png):
    from PIL import Image
    a = np.asarray(Image.open(png).convert("RGB"), dtype=np.float64)
    return a, float((0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]).mean())


def glass_lum(img):
    """Mean / max luminance of the pane's inner area (glass_roi) in an RGB still array."""
    from PIL import Image, ImageDraw
    h, w = img.shape[:2]
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).polygon(glass_roi(w, h), fill=255)
    sel = np.asarray(mask) > 0
    if not sel.any():
        return None, None
    lum = 0.2126 * img[..., 0] + 0.7152 * img[..., 1] + 0.0722 * img[..., 2]
    return float(lum[sel].mean()), float(lum[sel].max())


def compare(lit_png, dark_png, max_leak, max_glass):
    a, la = lum_mean(lit_png)
    b, lb = lum_mean(dark_png)
    d = a - b
    dl = 0.2126 * d[..., 0] + 0.7152 * d[..., 1] + 0.0722 * d[..., 2]
    res = {"lit_mean": la, "dark_mean": lb, "leak_mean": float(dl.mean()),
           "leak_p99": float(np.percentile(dl, 99)), "leak_max": float(dl.max()),
           "leak_rgb": [float(d[..., k].mean()) for k in range(3)], "max_leak": max_leak}
    # THE GLASS: the pane in the dark still (the lit sky outside, no lamps) is as dark as the room
    gm, gx = glass_lum(b)
    res["glass_mean"], res["glass_max"], res["max_glass"] = gm, gx, max_glass
    res["glass_ok"] = gm is not None and gm <= max_glass
    res["ok"] = res["leak_mean"] <= max_leak and res["glass_ok"]
    return res


def run(args):
    lit, dark, path = make(args.out)
    home = os.path.join(args.out, "home")
    os.makedirs(home, exist_ok=True)
    exe = os.path.join(args.viewer, "slipstream")
    stills, ran, noise = {}, {}, {}
    for name, cap in (("lit", lit), ("dark", dark)):
        rep = os.path.join(args.out, "report-%s.json" % name)
        cmd = [exe, "--bench", cap, "--bench-path", path, "--bench-loops", "1", "--bench-report", rep]
        for k, v in list(SETTINGS.items()) + [tuple(s.split("=", 1)) for s in args.set]:
            cmd += ["--set", k, v]
        env = dict(os.environ, SLIPSTREAM_HOME=home)
        with open(os.path.join(args.out, "viewer-%s.log" % name), "w") as log:
            rc = subprocess.run(cmd, cwd=args.viewer, env=env, stdout=log, stderr=subprocess.STDOUT,
                                timeout=args.timeout).returncode
        if rc != 0 or not os.path.isfile(rep):
            sys.exit("sealed-room: the %s replay failed (rc %d; %s)" % (name, rc, rep))
        # what ran: a replay without the ray-traced GI / ReSTIR DI tests nothing (a GPU without ray tracing)
        vlog = os.path.join(home, ".slipstreamviewer", "logs", "Slipstream.log")
        text = open(vlog, errors="replace").read() if os.path.isfile(vlog) else ""
        ran[name] = {"gi": "giApply : ray-traced GI: " in text, "restir": "lights in the set" in text}
        with open(rep) as f:
            r = json.load(f)
        imgs = [s.get("mean_image") for s in r.get("noise", []) if s.get("mean_image")]
        if not imgs:
            sys.exit("sealed-room: the %s report has no still image" % name)
        noise[name] = [{k: n.get(k) for k in ("temporal_variance", "psnr_db")} for n in r.get("noise", [])]
        stills[name] = imgs[0] if os.path.isabs(imgs[0]) else os.path.join(os.path.dirname(rep), imgs[0])
    res = compare(stills["lit"], stills["dark"], args.max_leak, args.max_glass)
    res["ran"] = ran
    res["noise"] = noise   # (the GI's sampling noise inside: temporal variance / PSNR of the still)
    res["skip"] = not (ran["lit"]["gi"] and ran["lit"]["restir"])
    res["settings"] = SETTINGS
    res["extra_settings"] = args.set
    with open(os.path.join(args.out, "sealed_room.json"), "w") as f:
        json.dump(res, f, indent=1)
    return res


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("make")
    m.add_argument("out")
    r = sub.add_parser("run")
    r.add_argument("--viewer", required=True, help="the packaged viewer directory (holds ./slipstream)")
    r.add_argument("--out", required=True)
    r.add_argument("--set", action="append", default=[], help="KEY=VALUE, an extra --set (an A/B)")
    r.add_argument("--max-leak", type=float, default=0.5)
    r.add_argument("--max-glass", type=float, default=1.0, help="the pane's mean luminance in the dark still (0..255)")
    r.add_argument("--timeout", type=float, default=600.0)
    c = sub.add_parser("compare")
    c.add_argument("lit")
    c.add_argument("dark")
    c.add_argument("--max-leak", type=float, default=0.5)
    c.add_argument("--max-glass", type=float, default=1.0)
    a = p.parse_args()
    if a.cmd == "make":
        print("\n".join(make(a.out)))
        return 0
    res = run(a) if a.cmd == "run" else compare(a.lit, a.dark, a.max_leak, a.max_glass)
    print(json.dumps(res, indent=1))
    if res.get("skip"):
        print("sealed-room: SKIP (the replay ran without ray-traced GI / ReSTIR DI: %s)" % res["ran"])
        return 2
    print("sealed-room: %s (leak %.3f, max %.3f; lit %.2f dark %.2f; glass %s, max %.3f)"
          % ("PASS" if res["ok"] else "FAIL", res["leak_mean"], a.max_leak, res["lit_mean"], res["dark_mean"],
             "%.3f" % res["glass_mean"] if res["glass_mean"] is not None else "not in view", a.max_glass))
    return 0 if res["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
