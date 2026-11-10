"""Regression guard for the blend layers' OWN light (RenderVKBlendHitLighting): no borrowed or unshadowed light.

An alpha-blended layer -- glass, a decal, makeup, hair -- has no pixel history and no denoiser, the case of a
ray-traced reflection hit, so it is shaded the way a hit is (clean_draw_common.glsl blendHitLocal: the hits' light
walk clean_rt_hit_lights.glsl rtHitLocalLights over the froxel cell, the 2 strongest lights exact + 2 sampled
picks, a shadow ray each; clean_surface_light.glsl: its own occlusion ray for the GI fallback and the environment).
Before it, the forward walk lit every point light (and, by default, every ray-traced projector) UNSHADOWED, and
the borrowing fixes took the opaque surface's lamp record (#1440) or GI occlusion record instead -- light that is
not the layer's own.

The check runs the SHADERS' OWN code: for every forward lit FS variant regen_world_spv.py builds with ray queries
(clean_draw.frag's, clean_oit_resolve.frag -DOIT_LIT's), glslc -E with that variant's flags expands every hook, the
layer's light walk and GI (rtHitLocalLights, giFwdOn, giFwdCache, blendHitEnv, giForwardRefl + the constants they
name) are cut out, turned into C++ and compiled into scripts/check/test_blend_hit.cpp: a sealed room lights the
layer with nothing (from the froxel cell and the cluster list), the room's own lamp is the mean, the strongest
lights are exact, the noise is below plain NEE with the same rays, the budget holds, the glare stays, and the GI
fallback is dark in a sealed room and kept outdoors. Plus the wiring the fixture cannot see: the setting (A/B,
Persist 0), the host's gates (no lamp / GI occlusion record published, written or kept while it is on) and the
lit FS's use of the walk. On the code before it the blend FS compiles no rtHitLocalLights: the check fails.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

NAME = "blend-hit"
STAGE = "source"
DESCRIPTION = "the blend layers' own light: hit-style walk + own GI occlusion, no borrowed lamp / GI records"
GUARDS = ("Alpha-blend layers lit by lamps through walls or by the surface behind them: the forward walk's "
          "unshadowed point lights, the borrowed lamp / GI occlusion records while RenderVKBlendHitLighting is on, "
          "a noisy or unbounded shadow-ray budget, the sealed-room glass lit by the probes / sky.")

_FNS = ("vec3 rtHitLocalLights(", "bool giFwdOn(", "void giFwdCache(", "vec3 blendHitEnv(", "SurfaceRefl giForwardRefl(")
_CONSTS = re.compile(r"^const (?:uint|float|int) (?:SURF_\w+|BLEND_HIT_\w+)\b[^;]*;$", re.M)
_LIT = re.compile(r"(?<![\w.])(\d+\.\d*(?:[eE][-+]?\d+)?|\.\d+(?:[eE][-+]?\d+)?|\d+[eE][-+]?\d+)(?![\w.])")


def _to_cpp(fn):
    """The preprocessed GLSL as C++ for the fixture's shims."""
    fn = re.sub(r"\[\[(?:dont_)?unroll\]\]", "", fn)
    fn = re.sub(r"\b(?:out|inout)\s+(\w+)\s+(\w+)", r"\1& \2", fn)
    fn = re.sub(r"\.(?:rgb|xyz)\b(?!\s*\()", ".xyz()", fn)
    fn = re.sub(r"\.xy\b(?!\s*\()", ".xy()", fn)
    fn = re.sub(r"\.r\b", ".x", fn)
    fn = re.sub(r"\.g\b", ".y", fn)
    fn = re.sub(r"\.b\b", ".z", fn)
    return _LIT.sub(lambda m: m.group(1) + "f", fn)


def _cut(text):
    """The constants + the functions; None when the variant has no blend-layer walk."""
    if "vec3 rtHitLocalLights(" not in text or "vec3 blendHitEnv(" not in text:
        return None
    parts = _CONSTS.findall(text)
    for fn in _FNS:
        i = text.find(fn)
        if i < 0:
            return None
        j = text.find("\n}\n", i)
        if j < 0:
            return None
        parts.append(text[i:j + 3])
    return "\n".join(parts)


def _wiring(ctx):
    missing = []

    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    st = read("indra/newview/app_settings/settings.xml")
    m = re.search(r"<key>RenderVKBlendHitLighting</key>\s*<map>(.*?)</map>", st, re.S)
    if not m:
        missing.append("settings.xml has no RenderVKBlendHitLighting (the A/B toggle)")
    elif not (re.search(r"<key>Persist</key>\s*<integer>0</integer>", m.group(1))
              and re.search(r"<key>Type</key>\s*<string>Boolean</string>", m.group(1))
              and re.search(r"<key>Value</key>\s*<integer>1</integer>", m.group(1))):
        missing.append("RenderVKBlendHitLighting is not a Boolean, default 1, Persist 0 (an A/B toggle never persists)")
    r = read("indra/newview/vkgfx/llcleanrenderervk.cpp")
    h = read("indra/newview/vkgfx/llcleanrenderervk.h")
    if not re.search(r"mBlendHitLive = blendHit && mLitRt;\s*mLightInfoCPU\.fwd\[3\] = mBlendHitLive \? 1u : 0u;", r):
        missing.append("the light info's fwd.w bit 0 is not RenderVKBlendHitLighting on a ray-query lit frame (mBlendHitLive)")
    if not re.search(r"!mBlendHitLive && mDsLampFull\)\s*\?\s*kPushVisReuseRad", r):
        missing.append("pass 2's push can carry VISR_RAD (the lamp record) while the layers light themselves")
    if not re.search(r"giOccFwdNow\(\) const \{ return [^}]*!mBlendHitLive; \}", h):
        missing.append("giOccFwdNow (kPushGiOcc, the barrier) is not off while the layers light themselves")
    if not re.search(r"mDsGiOccWant = [^;]*!mBlendHitLive;", r):
        missing.append("the GI occlusion record is still wanted (allocated) while the layers light themselves")
    if not re.search(r"const bool lampFull = visReuse && mDsLampWant;", r) \
            or not re.search(r"mDsLampWant = now - mBlendHitLastLive >= kDsSssGraceSecs;", r):
        missing.append("the lamp record is allocated at full size while the layers light themselves")
    d = read("indra/vkclean/shaders/clean_deferred_light.comp")
    if not re.search(r"giFrameOn\(\) && \(linfo\.fwd\.w & 1u\) == 0u && all\(lessThan\(px, imageSize\(uGiOccOut\)\)\)", d):
        missing.append("BASE / COMBINE still store the GI occlusion record while the layers light themselves")
    if not re.search(r"\(linfo\.fwd\.w & 1u\) == 0u\s*&& all\(lessThan\(ivec2\(CLEAN_FRAG_XY\), imageSize\(uLampOut\)\)\)", d):
        missing.append("BASE / COMBINE still store the lamp record while the layers light themselves (or past its extent)")
    c = read("indra/vkclean/shaders/clean_draw_common.glsl")
    if not re.search(r"if \(blendHitOn\(\)\) g_visr_rad = false;", c):
        missing.append("visReuseMatch can still take the lamp record (VISR_RAD) under the layers' own light")
    s = read("indra/vkclean/shaders/clean_surface_light.glsl")
    if not re.search(r"vec3 local = blendHitOn\(\) \? blendHitLocal\(", s):
        missing.append("lightSurface does not light a blended layer through blendHitLocal")
    o = read("indra/vkclean/shaders/clean_oit_resolve.frag")
    if not re.search(r"if \(blendHitOn\(\)\) g_oit_near = false;", o):
        missing.append("the WBOIT lit resolve still borrows the opaque surface's RT-mask sun / AO under the layers' own light")
    return missing


def _variants(regen):
    out = []
    for e in regen.SHADERS:
        flags = e[3] if len(e) > 3 else ()
        if "-DCLEAN_PROJ_SHADOW_RAYQUERY" not in flags:
            continue
        if e[0] == "clean_draw.frag" and not any(f in flags for f in ("-DCLEAN_CAPTURE", "-DCLEAN_GBUFFER",
                                                                       "-DCLEAN_OIT_GBUF", "-DCLEAN_BASIC")):
            out.append(e)
        elif e[0] == "clean_oit_resolve.frag" and "-DOIT_LIT" in flags:
            out.append(e)
    return out


def run(ctx, result):
    missing = _wiring(ctx)
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    glslc = os.environ.get("GLSLC") or shutil.which("glslc")
    if not cxx or not glslc:
        if missing:
            result.fail("\n".join(missing))
            return
        result.warn("no C++ compiler / glslc: the layers' walk not run (wiring checked)")
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
            missing.append("%s does not light its blended layers through the hit walk (rtHitLocalLights / blendHitEnv)" % n)
            continue
        walks.setdefault(c, []).append(n)
    for want in ("kWorldFragBlendRtSpv", "kWorldOitResolveLitRtFragSpv"):
        if want not in names:
            missing.append("regen_world_spv.py builds no %s (the blend layers' ray-query FS)" % want)
    if not walks:
        result.fail("\n".join(missing + ["no forward variant carries the blend layers' own light"]))
        return
    src = ctx.path("scripts/check/test_blend_hit.cpp")
    inc = ctx.path("indra/vkclean/shaders")
    failures = []
    notes = []

    def build_run(item):
        k, (fn, ns) = item
        with tempfile.TemporaryDirectory() as tmp:
            f = os.path.join(tmp, "walk.inc")
            with open(f, "w") as o:
                o.write(_to_cpp(fn))
            exe = os.path.join(tmp, "t")
            defs = ["-I" + inc, '-DBH_FN_FILE="%s"' % f]
            if "g_rt_fwd_hit" in fn:
                defs.append("-DBH_RT_FWD")
            c = subprocess.run([cxx, "-std=c++17", "-O2", "-w", *defs, src, "-o", exe], capture_output=True, text=True)
            if c.returncode:
                return ns, "the fixture does not compile against this walk (a new hook needs a shim?):\n" + c.stderr[:3000], ""
            r = subprocess.run([exe], capture_output=True, text=True)
            return ns, (r.stdout + r.stderr) if r.returncode else None, r.stdout

    with ThreadPoolExecutor(max_workers=max(1, os.cpu_count() or 1)) as ex:
        for ns, err, out in ex.map(build_run, enumerate(walks.items())):
            if err:
                failures.append("%s:\n%s" % (", ".join(ns), err.strip()))
            else:
                notes += [l for l in out.splitlines() if l.startswith("NOISE")][:1]
    if failures or missing:
        result.fail("\n".join(missing + failures))
        return
    result.summary = "%d distinct walks over %d variants: sealed 0, own lamp, exact key/fill, budget, GI%s" % (
        len(walks), sum(len(v) for v in walks.values()), ("; " + notes[0]) if notes else "")
