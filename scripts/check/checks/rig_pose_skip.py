"""Regression guard for the GI speckle on a close-up avatar: a pose-skipped rigged BLAS outside the drawn skin.

The #1007 POSE SKIP left an avatar's rig copy + BLAS refit out while its summed pose drift since the last refit
stayed under RenderVKRigRefitPoseThreshold -- 2 mm everywhere within 10 m, the near / protected avatars included.
GI rays leave the RASTER skin lifted by half a pixel footprint (giFreshRay: about 75 um at 0.3 m on a 4K view), so
wherever the stale traced skin lay outside the drawn one they started inside the body: the GI back-face stop
returned no light (RenderVKGIDebug 9: magenta over the whole skin), the reuse's visibility rays from inside were
blocked, and the few texels whose origin was still outside kept their GI -- pale single-pixel specks changing every
frame. The threshold (indra/newview/llcleanrigposeskip.h) is now capped by the smallest GI origin lift the avatar can
have on screen (its nearest point's distance x the view's corner pixel angle).

  * the fixture (scripts/check/test_rig_pose_skip.cpp, includes the header): with the old threshold every close-up
    receiver of a skin patch starts inside its stale BLAS (fixture sanity: the bug); with the new one none does over
    6 views (1080p .. 8K, 20 .. 90 deg) x 9 distances (0.25 .. 40 m) x 5 screen positions (centre, edges, corners);
    a far avatar at 1080p keeps the metric threshold (#1007's savings); ortho / no projection / off never skip;
  * source wiring: llcleanrtvk.cpp takes its pose-skip threshold from the header with the main view's px_scale and
    corner angle, the old uncapped formula is gone, and giFreshRay still lifts its origin as the fixture models it.
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "rig-pose-skip"
STAGE = "source"
DESCRIPTION = ("Rigged refit pose skip capped by the pixel footprint: a near avatar's BLAS never lags outside its drawn "
               "skin by more than the GI ray origin's lift (no GI back-face stops / pale specks up close)")
GUARDS = ("Pale single-pixel GI specks and missing GI on an avatar's skin seen up close: its pose-skipped rigged BLAS "
          "lies millimetres outside the drawn skin, so GI rays start inside the traced body.")


def _wiring(ctx):
    missing = []

    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    rt = read("indra/newview/vkgfx/llcleanrtvk.cpp")
    if '#include "../llcleanrigposeskip.h"' not in rt:
        missing.append("llcleanrtvk.cpp does not include llcleanrigposeskip.h")
    if not re.search(r"const float pose_corner_cos2 = cleanRigCornerCos2\(R\.mCurProj\[0\], R\.mCurProj\[5\]\);", rt):
        missing.append("llcleanrtvk.cpp does not take the main view's corner angle (cleanRigCornerCos2 of mCurProj)")
    if not re.search(r"const float thr = cleanRigPoseSkipThreshold\(pose_thr, av_dist, cull\.lod_cam_pos\[3\], "
                     r"pose_corner_cos2,\s*\(cull\.flags & LLCleanRendererVK::kCullOrtho\) != 0u\);\s*"
                     r"pose_skip = thr > 0\.f && poseDrift\(av, rb\.lastRefit\) < thr;", rt):
        missing.append("the pose skip does not test poseDrift against cleanRigPoseSkipThreshold(... px_scale, corner, ortho)")
    if re.search(r"pose_thr \* std::max\(1\.f, av_dist / kPoseRefMeters\)", rt):
        missing.append("llcleanrtvk.cpp still has the uncapped metric pose-skip threshold")
    gi = read("indra/vkclean/shaders/clean_deferred_light.comp")
    fresh = gi[gi.find("void giFreshRay("):]
    fresh = fresh[:fresh.find("\n}\n")]
    if not re.search(r"const float pa = rtrPixelAngle\(g_frag_xy\) \* r\.x;", fresh) \
            or not re.search(r"const float lift = 0\.5 \* camd \* pa / max\(abs\(dot\(Ng, V\)\), 0\.25\);", fresh) \
            or "o = rtReceiverOrigin(P, Ng, camd, lift);" not in fresh:
        missing.append("giFreshRay no longer lifts its origin by 0.5 x camd x pixel angle / max(|Ng.V|, 0.25): update "
                       "llcleanrigposeskip.h's lift bound and test_rig_pose_skip.cpp's model")
    return missing


def run(ctx, result):
    hdr = ctx.path("indra/newview/llcleanrigposeskip.h")
    if not os.path.isfile(hdr):
        result.fail("indra/newview/llcleanrigposeskip.h is missing: the rigged pose skip lags near avatars' BLAS by a "
                    "fixed 2 mm again (GI back-face stops and pale specks on close-up skin)")
        return
    missing = _wiring(ctx)
    if missing:
        result.fail("\n".join(missing))
        return
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: the pose-skip fixture not run (source wiring checked)")
        result.summary = "wiring only (no compiler)"
        return
    src = ctx.path("scripts/check/test_rig_pose_skip.cpp")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_rig_pose_skip")
        c = subprocess.run([cxx, "-std=c++17", "-O2", "-Wall", "-I" + os.path.dirname(hdr), src, "-o", exe],
                           capture_output=True, text=True)
        if c.returncode:
            result.fail("compile failed:\n" + c.stdout + c.stderr)
            return
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return
    result.summary = "close-up receivers never start inside a pose-skipped BLAS (fixture) + threshold / GI lift wiring"
