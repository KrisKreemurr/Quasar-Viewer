#!/usr/bin/env python3
"""A light's own prim never shadows its light, whatever its size (the own-fixture rule).

A point light sits at its prim's centre, and every instance of that prim has its object origin
there. The shadow walk's own-fixture test (clean_vis_walk.glsl rtFixtureBox) now accepts such an
instance at any size; any other enclosing instance still has to hold the light's centre and be
no larger than RenderVKReSTIRFixtureSize. Before, a prim larger than the fixture limit that IS
the light (a 3 m wood stove with its fire light) blocked every one of its light's rays, and
"Shadows from all lights" left the room it lights dark.

Source checks for the rule; --compile builds every kernel that walks shadow rays with glslc.

Usage:
  python3 <source>/indra/newview/tests/restir_own_prim_checks.py --source-root <source> [--compile --out <dir>]
"""
from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys


def check(cond: bool, what: str) -> None:
    if not cond:
        raise AssertionError(what)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source-root", required=True)
    ap.add_argument("--compile", action="store_true")
    ap.add_argument("--out")
    args = ap.parse_args()
    root = pathlib.Path(args.source_root)
    sh = root / "indra/vkclean/shaders"
    walk = (sh / "clean_vis_walk.glsl").read_text()

    # The own-prim test: the instance's object-to-world origin is the light's centre (within RT_FIX_OWN_EPS).
    check("#define RT_FIX_OWN_EPS 0.02" in walk, "the own-prim tolerance (2 cm)")
    own = re.search(r"bool rtFixtureOwn\(mat4x3 o2w\)\s*\{(.*?)\n\}", walk, re.S)
    check(own is not None, "rtFixtureOwn(mat4x3 o2w) must exist")
    check("o2w[3] - RT_FIX_P" in own.group(1) and "RT_FIX_OWN_EPS * RT_FIX_OWN_EPS" in own.group(1),
          "the own test compares the instance's origin with the light's centre")

    # The fixture predicate: the box must be readable, then EITHER the light's own prim (any size)
    # OR an enclosing box that holds the centre and is small enough.
    m = re.search(r"#define rtFixtureBox\(ID, Q, C, A, B\)(.*?)\n(?!\s)", walk + "\n", re.S)
    check(m is not None, "rtFixtureBox must stay a macro")
    body = m.group(1)
    a = body.find("rtFixtureBoxAB(")
    o = body.find("rtFixtureOwn(")
    h = body.find("rtFixtureHolds(")
    s = body.find("rtFixtureSmall(")
    check(0 <= a < o < h < s, "order: the box, then own prim OR (holds AND small)")
    check(re.search(r"rtFixtureOwn\(rayQueryGetIntersectionObjectToWorldEXT\(\(Q\), \(C\)\)\)\s*\\?\s*\|\|", body),
          "the own prim short-circuits the size limit")
    check(re.search(r"\|\|\s*\\?\s*\(rtFixtureHolds\(", body), "holds AND small stay the rule for every other box")

    print("restir own prim: PASS")

    if args.compile:
        out = pathlib.Path(args.out or "restir_own_prim_spv")
        out.mkdir(parents=True, exist_ok=True)
        variants = [
            ("clean_deferred_light.comp", ["-DCLEAN_DEFER_PASS=15", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=4", "-DCLEAN_SHADOW_CORE"]),
            ("clean_deferred_light.comp", ["-DCLEAN_DEFER_PASS=15", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=4", "-DCLEAN_SHADOW_MERGE", "-DCLEAN_SHADOW_CORE"]),
            ("clean_deferred_light.comp", ["-DCLEAN_DEFER_PASS=11", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=4", "-DCLEAN_RT_LIST", "-DCLEAN_VIS_WALK"]),
            ("clean_deferred_light.comp", ["-DCLEAN_DEFER_PASS=11", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=4", "-DCLEAN_GI_TRACE"]),
            ("clean_deferred_light.comp", ["-DCLEAN_DEFER_PASS=11", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=4", "-DCLEAN_RT_LIST", "-DCLEAN_RT_STAGE=3", "-DCLEAN_VIS_QUEUE", "-DCLEAN_VIS_QUEUE_M2"]),
            ("clean_deferred_light.comp", ["-DCLEAN_DEFER_PASS=11", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=4", "-DCLEAN_RT_LIST", "-DCLEAN_RT_STAGE=5", "-DCLEAN_VIS_QUEUE", "-DCLEAN_VIS_QUEUE_M2"]),
            ("clean_vol_light.comp", ["-DVOL_RT", "-DRT_SET=3"]),
            ("clean_rt_shadow_mask.comp", ["-DRT_SET=2", "-DCLEAN_PROJ_SHADOW_RAYQUERY"]),
            ("clean_draw.frag", ["-DCLEAN_BLEND_TOL", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=2"]),
        ]
        for i, (src, defs) in enumerate(variants):
            stage = "comp" if src.endswith(".comp") else "frag"
            cmd = ["glslc", "--target-env=vulkan1.3", "-O", f"-fshader-stage={stage}", "-I", str(sh),
                   *defs, f"-DCLEAN_STAGE_{stage.upper()}", str(sh / src), "-o", str(out / f"v{i}.spv")]
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
            if r.returncode:
                sys.stderr.write(f"compile failed: {src} {' '.join(defs)}\n{r.stderr[-3000:]}\n")
                return 1
            print(f"compiled {src} {' '.join(defs)}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as e:
        sys.stderr.write(f"FAIL: {e}\n")
        raise SystemExit(1)
