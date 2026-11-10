"""Regression guard for the specular leak: lamps through walls in ray-traced reflections (and every RT hit).

A reflection hit (the RT reflections, the RT mirrors, the forward layer's reflections) traced shadow rays only
for its K brightest ray-traced projector records and lit it with every POINT light and every record-less
projector UNSHADOWED: a dark bathroom's glossy tiles reflected its own walls lit by the bedroom lamp behind the
wall, while every matte surface stayed dark. The hits now take the GI hits' sampled budget (clean_hit_nee.h):
every light is a candidate for the K rays, none lights a hit unshadowed.

The check runs the KERNELS' OWN light walk: for every clean_deferred_light.comp variant regen_world_spv.py builds
that compiles rtHitLocalLights, glslc -E with that variant's flags expands every hook, the function is cut out,
turned into C++ and compiled into scripts/check/test_refl_hit_nee.cpp, which shades a glossy receiver's hit in a
sealed room (every light behind a wall: 0 for every frame and K) and beside the room's own lamp (the mean is the
lamp alone). Plus the wiring the fixture cannot see: the host never asks for 0 rays, and the GI hits' ambient
mode 3 ("the cache, else nothing") is nothing without a cache.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

NAME = "refl-hit-nee"
STAGE = "source"
DESCRIPTION = "sealed-room fixture on every RT hit kernel's own light walk: no light through walls in reflections"
GUARDS = ("Lamps lighting ray-traced reflection / mirror / GI hits through walls: point lights and record-less "
          "projectors unshadowed at reflection hits, a 0-ray budget, GI hit ambient mode 3 falling back to the "
          "probes without a cache.")

_FN = "vec3 rtHitLocalLights("
_LIT = re.compile(r"(?<![\w.])(\d+\.\d*(?:[eE][-+]?\d+)?|\.\d+(?:[eE][-+]?\d+)?|\d+[eE][-+]?\d+)(?![\w.])")


def _to_cpp(fn):
    """The preprocessed GLSL function as C++ for the fixture's shims."""
    fn = re.sub(r"\[\[(?:dont_)?unroll\]\]", "", fn)
    fn = re.sub(r"\.(?:rgb|xyz)\b(?!\s*\()", ".xyz()", fn)
    fn = re.sub(r"\.xy\b(?!\s*\()", ".xy()", fn)
    fn = re.sub(r"\.r\b", ".x", fn)
    fn = re.sub(r"\.g\b", ".y", fn)
    fn = re.sub(r"\.b\b", ".z", fn)
    return _LIT.sub(lambda m: m.group(1) + "f", fn)


def _cut(text):
    i = text.find(_FN)
    if i < 0:
        return None
    j = text.find("\n}\n", i)
    return text[i:j + 3] if j > 0 else None


def _wiring(ctx):
    missing = []

    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    r = read("indra/newview/vkgfx/llcleanrenderervk.cpp")
    if not re.search(r"mRtReflHitShadows\s*=\s*std::clamp<U32>\(\(U32\)s_hitsh,\s*1u,\s*4u\)", r):
        missing.append("llcleanrenderervk.cpp lets RenderVKRTReflectionsHitShadowRays reach 0 (no ray: the hit's lights dropped)")
    if not re.search(r"mRtMirrorHitShadows\s*=\s*std::clamp<U32>\(\(U32\)s_rtm_sh,\s*1u,\s*4u\)", r):
        missing.append("llcleanrenderervk.cpp lets RenderVKRTMirrorsHitShadowRays reach 0")
    vq = read("indra/newview/vkgfx/llcleanvisqueuevk.cpp")
    if not re.search(r"budgetK\[VQR_RT\]\s*=\s*std::clamp<uint32_t>\(mRtReflHitShadows,\s*1u,\s*4u\)", vq):
        missing.append("llcleanvisqueuevk.cpp's RT budget K is not clamped to 1..4 like the push's")
    d = read("indra/vkclean/shaders/clean_deferred_light.comp")
    env = d[d.find("void rtrHitEnv("):]
    env = env[:env.find("#elif")]
    if not re.search(r"amb\s*==\s*GI_HIT_AMB_CACHE0\s*&&\s*giCacheSlots\(\)\s*==\s*0u", env):
        missing.append("rtrHitEnv (GI hits): ambient mode 3 without a cache does not return nothing (the probe walk "
                       "lights every GI hit with the unshadowed captures and their sky)")
    return missing


def run(ctx, result):
    missing = _wiring(ctx)   # (reported with the fixture's result: both say what broke)
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    glslc = os.environ.get("GLSLC") or shutil.which("glslc")
    if not cxx or not glslc:
        if missing:
            result.fail("\n".join(missing))
            return
        result.warn("no C++ compiler / glslc: the kernels' light walks not run (wiring checked)")
        result.summary = "wiring only"
        return
    vk = ctx.path("indra/vkclean")
    sys.path.insert(0, vk)
    try:
        import regen_world_spv as regen
    finally:
        sys.path.pop(0)
    jobs = [regen.variant(e) for e in regen.SHADERS if e[0] == "clean_deferred_light.comp"]
    names = [e[2] for e in regen.SHADERS if e[0] == "clean_deferred_light.comp"]

    def pre(job):
        path, stage, flags = job
        r = subprocess.run([glslc, "-E", "-fshader-stage=" + stage, "--target-env=vulkan1.3",
                            *[f for f in flags if f.startswith(("-D", "-I"))], path], capture_output=True, text=True)
        return _cut(r.stdout) if r.returncode == 0 else ("ERR", r.stderr)

    with ThreadPoolExecutor(max_workers=max(1, os.cpu_count() or 1)) as ex:
        cut = list(ex.map(pre, jobs))
    walks = {}
    for n, c in zip(names, cut):
        if isinstance(c, tuple):
            result.fail("\n".join(missing + ["glslc -E failed for %s:\n%s" % (n, c[1][:2000])]))
            return
        if c is not None:
            walks.setdefault(c, []).append(n)
    if not any("Rt" in n for ns in walks.values() for n in ns):
        result.fail("\n".join(missing + ["no RT reflection kernel compiles rtHitLocalLights any more: the fixture covers nothing"]))
        return
    src = ctx.path("scripts/check/test_refl_hit_nee.cpp")
    inc = ctx.path("indra/vkclean/shaders")
    failures = []

    def build_run(item):
        k, (fn, ns) = item
        with tempfile.TemporaryDirectory() as tmp:
            f = os.path.join(tmp, "walk.inc")
            with open(f, "w") as o:
                o.write(_to_cpp(fn))
            exe = os.path.join(tmp, "t")
            c = subprocess.run([cxx, "-std=c++17", "-O2", "-w", "-I" + inc, '-DRHL_FN_FILE="%s"' % f, src, "-o", exe],
                               capture_output=True, text=True)
            if c.returncode:
                return ns, "the fixture does not compile against this walk (a new hook needs a shim?):\n" + c.stderr[:3000]
            r = subprocess.run([exe], capture_output=True, text=True)
            return ns, (r.stdout + r.stderr) if r.returncode else None

    with ThreadPoolExecutor(max_workers=max(1, os.cpu_count() or 1)) as ex:
        for ns, err in ex.map(build_run, enumerate(walks.items())):
            if err:
                failures.append("%s:\n%s" % (", ".join(ns), err.strip()))
    if failures or missing:
        result.fail("\n".join(missing + failures))
        return
    result.summary = "%d distinct light walks over %d kernels: sealed room 0, own lamp" % (
        len(walks), sum(len(v) for v in walks.values()))
