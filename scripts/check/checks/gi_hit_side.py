"""Regression guard for the GI fireflies behind back-facing layers: GI hits lit through flipped-normal surfaces.

rtSurfaceEval oriented a ray-traced hit's shading normal by the triangle's winding alone (the raster's
gl_FrontFacing rule). On a mesh whose vertex normals oppose its winding -- flipped-normal uploads, inside-out
sculpts, double-sided cloth / leaves with one side's normals -- the normal then points AWAY from the ray: the GI
hit is lit by the lamps BEHIND the layer (N . l > 0) and their shadow rays start on the lamps' side
(rtrProjRaySetup turns the origin toward the light along N), so they never cross the layer. A lamp just behind a
thin layer makes a few hits on its hidden side very bright; the reservoirs prefer them and hand them on: isolated
bright pixels on the surface just behind a back-facing layer. GI hits now take the side the ray arrived from
(indra/vkclean/shaders/clean_rt_hit_side.h, shared by the shader and the fixture), as the GI receivers already do.

  * the fixture (scripts/check/test_rt_hit_side.cpp, host C++ compiler): a lamp 5 cm behind a flipped-normal
    sheet lights none of the hits a receiver behind the sheet sees (the raster rule: it does -- fixture sanity), a
    lamp on the ray's side lights them as an agreeing sheet's, agreeing normals unchanged, and over random
    geometry the GI rule's normal is always on the arrival side;
  * source wiring: rtSurfaceEval takes its facing from rtHitNormalFront for the vertex normal and every normal map,
    the GI trace (and only it) turns the arrival rule on, and rtrProjRaySetup still aims its origin as the fixture
    models it.
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "gi-hit-side"
STAGE = "source"
DESCRIPTION = "GI hits shade the side their ray arrived from (clean_rt_hit_side.h): no lamp light through flipped-normal layers"
GUARDS = ("GI fireflies on surfaces behind back-facing / flipped-normal / inside-out layers: GI hits shaded with a "
          "normal pointing away from the ray, lit unshadowed by the lamps behind the layer.")


def _wiring(ctx):
    """The source facts the fixture's rule is only meaningful with: [] when all hold, else what is missing."""
    missing = []

    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    surf = read("indra/vkclean/shaders/clean_rt_surface.glsl")
    if '#include "clean_rt_hit_side.h"' not in surf:
        missing.append("clean_rt_surface.glsl does not include clean_rt_hit_side.h")
    ev = surf[surf.find("RtSurface rtSurfaceEval("):]
    ev = ev[:ev.find("\n}\n")]
    if not ev:
        missing.append("clean_rt_surface.glsl has no rtSurfaceEval")
    if not re.search(r"const bool nfront\s*=\s*rtHitNormalFront\(s\.front,\s*nv_raw,\s*ng,\s*RT_SURF_ARRIVAL_NORMAL\);", ev):
        missing.append("rtSurfaceEval does not take its shading facing from rtHitNormalFront(s.front, nv_raw, ng, ...)")
    if not re.search(r"s\.Nv\s*=\s*nfront\s*\?\s*nv_raw\s*:\s*-nv_raw;", ev):
        missing.append("rtSurfaceEval's vertex normal does not take that facing (s.Nv = nfront ? nv_raw : -nv_raw)")
    if re.search(r"rtSurfApplyNormalMap\(\s*s\.front", ev):
        missing.append("a normal map in rtSurfaceEval still takes the winding's facing (rtSurfApplyNormalMap(s.front, ...))")
    if len(re.findall(r"rtSurfApplyNormalMap\(\s*nfront\b", ev)) < 4:
        missing.append("rtSurfaceEval's normal maps (terrain, PBR, legacy, bump) do not all take nfront")
    defer = read("indra/vkclean/shaders/clean_deferred_light.comp")
    if not re.search(r"#if DEFER_GI_TRACE\n(?://[^\n]*\n)*#define RT_SURF_ARRIVAL_NORMAL true\n#endif\n"
                     r"#include \"clean_rt_surface.glsl\"", defer):
        missing.append("clean_deferred_light.comp does not turn RT_SURF_ARRIVAL_NORMAL on for the GI trace "
                       "(#if DEFER_GI_TRACE ... before clean_rt_surface.glsl)")
    if len(re.findall(r"#define RT_SURF_ARRIVAL_NORMAL\b", defer)) != 1:
        missing.append("clean_deferred_light.comp defines RT_SURF_ARRIVAL_NORMAL other than once (GI only)")
    ray = read("indra/vkclean/shaders/clean_rt_light_ray.glsl")   # (shared with the forward blend layers)
    setup = ray[ray.find("void rtrProjRaySetup("):]
    setup = setup[:setup.find("\n}\n")]
    if not re.search(r"const vec3 n = dot\(N, l\) < 0\.0 \? -N : N;\s*o = offsetRayOrigin\(P \+ n \* 0\.01, n\);", setup):
        missing.append("rtrProjRaySetup no longer aims its shadow origin as test_rt_hit_side.cpp models it "
                       "(n = dot(N, l) < 0 ? -N : N; P + n x 1 cm): update the fixture's hitLight")
    return missing


def run(ctx, result):
    hdr = ctx.path("indra/vkclean/shaders/clean_rt_hit_side.h")
    if not os.path.isfile(hdr):
        result.fail("indra/vkclean/shaders/clean_rt_hit_side.h is missing: GI hits on flipped-normal layers are lit "
                    "through themselves again")
        return
    missing = _wiring(ctx)
    if missing:
        result.fail("\n".join(missing))
        return
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: the hit-side fixture not run (source wiring checked)")
        result.summary = "wiring only (no compiler)"
        return
    src = ctx.path("scripts/check/test_rt_hit_side.cpp")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_rt_hit_side")
        c = subprocess.run([cxx, "-std=c++17", "-O2", "-Wall", "-I" + os.path.dirname(hdr), src, "-o", exe],
                           capture_output=True, text=True)
        if c.returncode:
            result.fail("compile failed:\n" + c.stdout + c.stderr)
            return
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return
    result.summary = "flipped-normal sheet: no lamp through it, ray-side lamp as agreeing; arrival side always + GI wiring"
