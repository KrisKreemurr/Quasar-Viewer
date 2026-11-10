"""Regression guard for the lit glass of a sealed dark room: the forward GI fallback without occlusion.

An alpha-blended surface (glass) the GI radiance cache has no cell at -- glass standing free in a room, no
opaque surface near it -- took its ambient and specular from the probes / the sky UNOCCLUDED (giForwardRefl
returned the probes as they were): in a fully sealed, unlit bathroom the shower glass stayed lit while every
opaque surface went black, brighter still with reflection probes on. The forward fragment now takes the
occlusion GI applied to the opaque surface behind it at the same pixel (clean_gi_common.glsl THE OCCLUSION
RECORD, written by BASE / COMBINE; clean_surface_light.glsl giFwdScreenOcc).

The check runs the FORWARD FSs' OWN fallback: for every pass-2 forward variant regen_world_spv.py builds
(clean_draw.frag -DCLEAN_BLEND_TOL and its twins, clean_oit_resolve.frag -DOIT_LIT), glslc -E with that
variant's flags expands every macro, giForwardRefl (+ giFwdScreenOcc / giOccUnpack / giOccPack and the
constants they name) is cut out, turned into C++ and compiled into scripts/check/test_blend_gi_occ.cpp, which
lights a glass fragment in a sealed room (record 0, cache empty or off: zero ambient and specular, probes on
and off) and outdoors (record 1 or the sky behind: unchanged). Plus the wiring the fixture cannot see: the
record's image is set 3 binding 31 / draw set 0 binding 68 in the layouts, both lit kernels store it, pass 2's
push carries its bit and the barrier publishes it.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

NAME = "blend-gi-occ"
STAGE = "source"
DESCRIPTION = "sealed-room fixture on the forward FSs' own GI fallback: no unoccluded probe / sky light on glass"
GUARDS = ("Glass (alpha-blend, WBOIT) lit by the probes / the sky in a sealed dark room where the GI radiance "
          "cache has no cell: the forward fallback without the opaque pixel's GI occlusion, or the occlusion "
          "record not bound / written / published to pass 2.")

_FNS = ("uint giOccPack(", "bool giOccUnpack(", "bool giFwdScreenOcc(", "bool giFwdOn(", "void giFwdCache(",
        "SurfaceRefl giForwardRefl(")
_CONSTS = re.compile(r"^const (?:uint|float|int) (?:SURF_\w+|VISR_GI_OCC|GI_FWD_OCC_\w+)\b[^;]*;$", re.M)
_LIT = re.compile(r"(?<![\w.])(\d+\.\d*(?:[eE][-+]?\d+)?|\.\d+(?:[eE][-+]?\d+)?|\d+[eE][-+]?\d+)(?![\w.])")


def _to_cpp(fn):
    """The preprocessed GLSL as C++ for the fixture's shims."""
    fn = re.sub(r"\b(?:out|inout)\s+(\w+)\s+(\w+)", r"\1& \2", fn)
    fn = re.sub(r"\.(?:rgb|xyz)\b(?!\s*\()", ".xyz()", fn)
    fn = re.sub(r"\.xy\b(?!\s*\()", ".xy()", fn)
    return _LIT.sub(lambda m: m.group(1) + "f", fn)


def _cut(text):
    """The functions (those present) and the constants they name; None without giForwardRefl."""
    parts = _CONSTS.findall(text)
    for fn in _FNS:
        i = text.find(fn)
        if i < 0:
            continue
        j = text.find("\n}\n", i)
        if j < 0:
            return None
        parts.append(text[i:j + 3])
    return "\n".join(parts) if any(p.startswith("SurfaceRefl giForwardRefl(") for p in parts) else None


def _wiring(ctx):
    missing = []

    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    r = read("indra/newview/vkgfx/llcleanrenderervk.cpp")
    h = read("indra/newview/vkgfx/llcleanrenderervk.h")
    c = read("indra/vkclean/shaders/clean_deferred_light.comp")
    s = read("indra/vkclean/shaders/clean_surface_light.glsl")
    if not re.search(r"db0\[68\]\.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE", r) \
            or not re.search(r"dl0\.pNext=&db0fi; dl0\.bindingCount=(69|[7-9]\d)\b", r):
        missing.append("llcleanrenderervk.cpp: draw set 0 has no binding 68 (the GI occlusion record) in its layout")
    if not re.search(r"dst\[\d+\] = \{41u, 42u, 43u, 65u, 68u\}", r) or "kDsBindingGiOcc}" not in r:
        missing.append("llcleanrenderervk.cpp: draw set 0 binding 68 is not written from set 3's occlusion record")
    if not re.search(r"kDsBindingGiOcc = kDsImgBindings \+ 24u", h) \
            or not re.search(r"layout\(set = 3, binding = 31, r32ui\) uniform writeonly uimage2D uGiOccOut", c):
        missing.append("set 3 binding 31 (the occlusion record) differs between the host and clean_deferred_light.comp")
    if not re.search(r"kPushGiOcc = 16u", h) or not re.search(r"const uint VISR_GI_OCC = 16u", s) \
            or not re.search(r"layout\(set = 0, binding = 68, r32ui\) uniform readonly uimage2D uGiOccFwd", s):
        missing.append("the forward reader's binding 68 / push bit 16 differs from the host's (kPushGiOcc)")
    if not re.search(r"\(giOccFwdNow\(\) \? kPushGiOcc : 0u\)", r):
        missing.append("pass 2's push (visrWord) never carries kPushGiOcc")
    if not re.search(r"if \(mVisReuseFlags \|\| giOccFwdNow\(\)\) \{ dep\.memoryBarrierCount=1", r):
        missing.append("deferOutsToAttach does not publish the occlusion record (compute write -> fragment read)")
    if "g_gi_occ_rec = giOccPack(" not in c:
        missing.append("deferReflIn does not set the occlusion record")
    base = c[c.find("#elif CLEAN_DEFER_PASS == 7"):c.find("#elif CLEAN_DEFER_PASS == 8")]
    comb = c[c.find("#else   // CLEAN_DEFER_PASS 0 (uber) / 3 (combine)"):]
    if base.count("giOccStore(px)") < 2 or comb.count("giOccStore(px)") < 2:
        missing.append("BASE / COMBINE do not store the occlusion record on every pixel (sky and lit)")
    return missing


def _variants(regen):
    out = []
    for e in regen.SHADERS:
        flags = e[3] if len(e) > 3 else ()
        if e[0] == "clean_draw.frag" and "-DCLEAN_BLEND_TOL" in flags:
            out.append(e)
        elif e[0] == "clean_oit_resolve.frag" and "-DOIT_LIT" in flags:
            out.append(e)
    return out


def run(ctx, result):
    missing = _wiring(ctx)   # (reported with the fixture's result: both say what broke)
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    glslc = os.environ.get("GLSLC") or shutil.which("glslc")
    if not cxx or not glslc:
        if missing:
            result.fail("\n".join(missing))
            return
        result.warn("no C++ compiler / glslc: the forward fallback not run (wiring checked)")
        result.summary = "wiring only"
        return
    vk = ctx.path("indra/vkclean")
    sys.path.insert(0, vk)
    try:
        import regen_world_spv as regen
    finally:
        sys.path.pop(0)
    entries = _variants(regen)
    names = [e[2] for e in entries]
    jobs = [regen.variant(e) for e in entries]

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
        if c is None:
            missing.append("%s compiles no giForwardRefl (the forward GI fallback moved?): the fixture covers nothing" % n)
            continue
        walks.setdefault(c, []).append(n)
    if "kWorldFragBlendSpv" not in names or not walks:
        result.fail("\n".join(missing + ["no pass-2 forward variant (kWorldFragBlendSpv) found in regen_world_spv.py"]))
        return
    src = ctx.path("scripts/check/test_blend_gi_occ.cpp")
    failures = []

    def build_run(item):
        k, (fn, ns) = item
        with tempfile.TemporaryDirectory() as tmp:
            f = os.path.join(tmp, "fallback.inc")
            with open(f, "w") as o:
                o.write(_to_cpp(fn))
            exe = os.path.join(tmp, "t")
            defs = ['-DBGO_FN_FILE="%s"' % f]
            if "g_rt_fwd_hit" in fn:
                defs.append("-DBGO_RT_FWD")
            if "uint giOccPack(" in fn:
                defs.append("-DBGO_HAS_PACK")
            if "blendHitOn()" in fn:   # (RenderVKBlendHitLighting's branch, off here: test_blend_hit.cpp runs it)
                defs.append("-DBGO_BLEND_HIT")
            c = subprocess.run([cxx, "-std=c++17", "-O2", "-w", *defs, src, "-o", exe], capture_output=True, text=True)
            if c.returncode:
                return ns, "the fixture does not compile against this fallback (a new hook needs a shim?):\n" + c.stderr[:3000]
            r = subprocess.run([exe], capture_output=True, text=True)
            return ns, (r.stdout + r.stderr) if r.returncode else None

    with ThreadPoolExecutor(max_workers=max(1, os.cpu_count() or 1)) as ex:
        for ns, err in ex.map(build_run, enumerate(walks.items())):
            if err:
                failures.append("%s:\n%s" % (", ".join(ns), err.strip()))
    if failures or missing:
        result.fail("\n".join(failures + missing))
        return
    result.summary = "%d distinct fallbacks over %d forward variants: sealed glass 0, outdoors kept" % (
        len(walks), sum(len(v) for v in walks.values()))
