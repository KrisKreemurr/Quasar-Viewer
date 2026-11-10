"""Regression guard for the sealed-room leak: light through solid walls at ray-traced GI hits.

The GI hits' shadow-ray budget traced the K brightest lights at a hit and lit it with every other light
UNSHADOWED, so a closed room's GI bounces carried the next room's lamps through its walls. The hits now
SAMPLE their K rays among every shadowable light (indra/vkclean/shaders/clean_hit_nee.h, shared by the GPU
walk and this fixture). Two parts:

  * the fixture (scripts/check/test_hit_nee.cpp, host C++ compiler): the sealed room (every light behind a
    wall -> 0 for every seed and K), the room's own lamp beside blocked ones (the mean is the own lamp), the
    estimator's mean over random light sets (unbiased), the budget's slot contract;
  * source wiring: the GI walk really runs that code (clean_rt_shade.glsl samples points AND projectors through
    hnAdd / hnFinish, the GI trace turns it on, the host never asks for 0 rays).
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "hit-nee"
STAGE = "source"
DESCRIPTION = "sealed-room fixture: every light at a GI hit is shadow-tested (clean_hit_nee.h)"
GUARDS = ("Light leaking into closed rooms through walls at GI hits: lights past the hit shadow-ray budget "
          "lit unshadowed, record-less projectors and dim lights skipping their shadow test, a 0-ray budget.")


def _wiring(ctx):
    """The source facts the fixture's code is only meaningful with: [] when all hold, else what is missing."""
    missing = []

    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    shade = read("indra/vkclean/shaders/clean_rt_hit_lights.glsl")   # (rtHitLocalLights, clean_rt_shade.glsl includes it)
    if '#include "clean_rt_hit_lights.glsl"' not in read("indra/vkclean/shaders/clean_rt_shade.glsl"):
        missing.append("clean_rt_shade.glsl does not include clean_rt_hit_lights.glsl (the hits' light walk)")
    if '#include "clean_hit_nee.h"' not in shade:
        missing.append("clean_rt_hit_lights.glsl does not include clean_hit_nee.h")
    walk = shade[shade.find("vec3 rtHitLocalLights("):]
    walk = walk[:walk.find("\n}\n")]
    if len(re.findall(r"\bhnAdd\s*\(", walk)) < 2:
        missing.append("rtHitLocalLights does not sample both point lights and projectors through hnAdd")
    if "hnFinish(" not in walk:
        missing.append("rtHitLocalLights does not add the picks' estimates (hnFinish)")
    # the walk's sampled point lights must not also fall into the unshadowed sum
    if not re.search(r"hnAdd\([^;]*lw \* pt[^;]*\);\s*continue;", walk):
        missing.append("a sampled point light is not taken out of the unshadowed sum (hnAdd ...; continue;)")
    if not re.search(r"hnAdd\([^;]*\bsh\b[^;]*\);\s*continue;", walk):
        missing.append("a sampled projector share is not taken out of the unshadowed sum (hnAdd ...; continue;)")
    defer = read("indra/vkclean/shaders/clean_deferred_light.comp")
    gi = re.search(r"#if DEFER_GI_TRACE\n(?:(?!#endif).*\n)*?#define RT_SHADE_STOCH_SEED", defer)
    if not gi:
        missing.append("the GI trace does not define RT_SHADE_STOCH_SEED (sampling off: the brightest-K rule)")
    givk = read("indra/newview/vkgfx/llcleangivk.cpp")
    if not re.search(r"c\.hitShadow\s*=\s*std::clamp<U32>\(s_hsr,\s*1u,\s*4u\)", givk):
        missing.append("llcleangivk.cpp lets RenderVKGIHitShadowRays reach 0 (no ray: the sampled lights dropped)")
    vq = read("indra/newview/vkgfx/llcleanvisqueuevk.cpp")
    if not re.search(r"budgetK\[VQR_GI\]\s*=\s*std::clamp<uint32_t>\(giK,\s*1u,\s*4u\)", vq):
        missing.append("llcleanvisqueuevk.cpp's GI budget K is not clamped to 1..4 like the push's")
    return missing


def run(ctx, result):
    hdr = ctx.path("indra/vkclean/shaders/clean_hit_nee.h")
    if not os.path.isfile(hdr):
        result.fail("indra/vkclean/shaders/clean_hit_nee.h is missing: the GI hits light through walls again")
        return
    missing = _wiring(ctx)
    if missing:
        result.fail("\n".join(missing))
        return
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: hit sampling fixtures not run (source wiring checked)")
        result.summary = "wiring only (no compiler)"
        return
    src = ctx.path("scripts/check/test_hit_nee.cpp")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_hit_nee")
        c = subprocess.run([cxx, "-std=c++17", "-O2", "-Wall", "-I" + os.path.dirname(hdr), src, "-o", exe],
                           capture_output=True, text=True)
        if c.returncode:
            result.fail("compile failed:\n" + c.stdout + c.stderr)
            return
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return
    result.summary = "sealed room 0, own lamp, unbiased mean, slots + GI wiring"
