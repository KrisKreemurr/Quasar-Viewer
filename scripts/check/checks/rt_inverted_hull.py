"""Regression guard for inverted-hull cel-shading outlines in ray tracing (GI fireflies, lamp shadows).

An inverted-hull outline is a rigged copy of the body pushed a few mm out with its winding flipped: back-face
culled, the raster shows only its far inside past the silhouette. Ray traced, every ray leaving the skin starts
inside it and meets its near face from the FRONT:
  * point lights / projectors shadowed the whole wrapped avatar -- their shadow rays had no facing cull (the sun's
    rays did: RenderVKRTRiggedFacingParity). LL's spot-shadow maps cull back faces (renderShadow, GL_CULL_FACE).
    Fixed by rtFacingCullSet (clean_rt_common.glsl) around every positional shadow trace.
  * GI rays took the shell's dark inside, millimetres off the skin, for the world; the shell's hits face the body,
    so a lamp beyond a thin part (an ear, a finger) lit them through it -- their shadow ray (rtrProjRaySetup,
    1 cm along N toward the lamp) starts past the part: bright samples the reservoirs spread (fireflies). No
    facing rule can remove these hits (from the skin the shell IS front-facing), so the shell is detected
    (indra/newview/llcleaninvhull.h, once per captured rigged face) and left out of the ray-traced scene
    (LLCleanRTVK's rigged plan; RenderVKRTOutlineHullExclude). The raster still draws it.

  * the fixture (scripts/check/test_inv_hull.cpp, includes llcleaninvhull.h): the classifier marks the inverted
    body / sleeve / head shells and nothing wound outward, no flat card, no tiny detail; the old positional rule
    shadows every lamp-facing receiver of the wrapped body, the facing cull and the exclusion none, another body
    still shadows; GI rays all hit the traced hull within mm and none with it out; the ear leak reproduces;
  * source wiring: the positional traces set the facing cull; the capture classifies, the ingest tags the row,
    the RT plan skips it (and its row signature sees the bit).
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "rt-inverted-hull"
STAGE = "source"
DESCRIPTION = ("Inverted-hull outlines: positional RT shadows take the sun's facing cull; the shell is detected at "
               "capture and left out of the RT scene (no GI fireflies, no lamp self-shadow)")
GUARDS = ("A cel-shaded avatar's inverted-hull outline shadowing the avatar under point lights / projectors, and GI "
          "fireflies / darkening on the skin it wraps.")


def _wiring(ctx):
    missing = []

    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    def body(src, sig):
        b = src[src.find(sig):]
        return b[:b.find("\n}\n")] if sig in src else ""

    sh = "indra/vkclean/shaders/"
    common = read(sh + "clean_rt_common.glsl")
    if "void rtFacingCullSet(vec4 p)" not in common:
        missing.append("clean_rt_common.glsl has no rtFacingCullSet (the positional facing cull)")
    if not re.search(r"uint\s+rtFolMode\(\)\s*\{\s*return rt_fol_cull \? min\(rtFolBits\(\) & 3u, rt_fol_sun \? 2u : 1u\) : 0u; \}", common):
        missing.append("rtFolMode no longer keeps mode 2 (tinted transmission) to the sun's traces")
    flags = body(common, "uint rtFolShadowFlags()")
    if "if (!rt_fol_cull) return gl_RayFlagsNoneEXT;" not in flags:
        missing.append("rtFolShadowFlags no longer keys the facing cull on rt_fol_cull")
    core = read(sh + "clean_shadow_core.glsl")
    if not re.search(r"if \(fol\) rtFolBegin\(\);\s*(//[^\n]*\n\s*)*else if \(pos\) rtFacingCullSet\(ssr\.foliage_rt\);",
                     body(core, "ShRayGen shadowSampleSetup(")):
        missing.append("shadowSampleSetup does not set the facing cull for positional samples")
    if "rtFoliageEnd();" not in body(core, "void shadowSampleEnd("):
        missing.append("shadowSampleEnd does not end the facing cull")
    proj = body(read(sh + "clean_shadow_rt.glsl"), "vec3 projShadowRayCore(")
    if not re.search(r"rtFacingCullSet\(ssr\.foliage_rt\);[^\n]*\n\s*const vec3 v = projShadowRayCoreImpl\(", proj):
        missing.append("projShadowRayCore does not trace with the facing cull")
    ray = body(read(sh + "clean_rt_light_ray.glsl"), "vec3 rtrProjRay(")
    if not re.search(r"rtFacingCullSet\(ssr\.foliage_rt\);[^\n]*\n\s*const vec3 v = rtVisTraceK\(", ray):
        missing.append("rtrProjRay (the RT / GI hits' light rays) does not trace with the facing cull")
    if not re.search(r"else if \(kind == VQ_K_LIGHT\) rtFacingCullSet\(ssr\.foliage_rt\);", read(sh + "clean_deferred_light.comp")):
        missing.append("the visibility queue's VQ_K_LIGHT requests do not take the facing cull")

    nv = "indra/newview/"
    asset = read(nv + "llcleanmeshasset.cpp")
    if not re.search(r"f\.inside_out = cleanInsideOutShell\(", asset):
        missing.append("LLCleanMeshAsset::prepareCapture does not classify faces (cleanInsideOutShell)")
    if "slot.inside_out       = pf.inside_out;" not in asset:
        missing.append("commitCapture does not keep the classification in the Slot")
    vol = read(nv + "llvovolume.cpp")
    if not re.search(r"if \(gpu_rigged && slot_inside_out && \(inst\.flags & \(2u \| 8u \| 256u\)\) == 0u\)\s*"
                     r"inst\.flags \|= LLCleanInstanceStore::kRigInsideOut;", vol):
        missing.append("registerCleanFace does not tag single-sided GPU-rigged inside-out faces (kRigInsideOut)")
    store = read(nv + "llcleaninstancestore.h")
    if "static constexpr U32 kRigInsideOut = 16384u;" not in store:
        missing.append("LLCleanInstanceStore::kRigInsideOut is not bit 14")
    rt = read(nv + "vkgfx/llcleanrtvk.cpp")
    if not re.search(r"if \(hull_exclude && \(flags & 16384u\)\) continue;", rt):
        missing.append("LLCleanRTVK's rigged plan does not skip kRigInsideOut rows")
    if "mix(hull_exclude ? 1u : 0u);" not in rt:
        missing.append("the rigged plan's reuse key does not include RenderVKRTOutlineHullExclude")
    m = re.search(r"uint64_t h = \(uint64_t\)\(flags & (0x[0-9A-Fa-f]+)u\)", rt)
    if not m or (int(m.group(1), 16) & 0x4000) == 0:
        missing.append("refreshRigSet's rowSig does not hash bit 14 (a newly tagged row would not invalidate the plan)")
    settings = read(nv + "app_settings/settings.xml")
    if "<key>RenderVKRTOutlineHullExclude</key>" not in settings:
        missing.append("settings.xml has no RenderVKRTOutlineHullExclude")
    return missing


def run(ctx, result):
    hdr = ctx.path("indra/newview/llcleaninvhull.h")
    if not os.path.isfile(hdr):
        result.fail("indra/newview/llcleaninvhull.h is missing: inverted-hull outlines are ray traced again")
        return
    missing = _wiring(ctx)
    if missing:
        result.fail("\n".join(missing))
        return
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: the inverted-hull fixture not run (source wiring checked)")
        result.summary = "wiring only (no compiler)"
        return
    src = ctx.path("scripts/check/test_inv_hull.cpp")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_inv_hull")
        c = subprocess.run([cxx, "-std=c++17", "-O2", "-Wall", "-I" + os.path.dirname(hdr), src, "-o", exe],
                           capture_output=True, text=True)
        if c.returncode:
            result.fail("compile failed:\n" + c.stdout + c.stderr)
            return
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return
    result.summary = "hull shells classified; lamp self-shadow + GI hull hits gone (fixture) + positional cull / RT skip wiring"
