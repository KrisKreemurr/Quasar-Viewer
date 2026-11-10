"""Regression guard for #1494: weather (heavy snow) cost ~8 ms a frame on an RTX 3060.

The tester's 10 s capture with the weather on: c_snow_mesh_build avg 5.26 ms, p50 0.02, p95 21.0 ms --
the snow mesh's FULL build (~21 ms in deep snow) ran every RenderVKWeatherSnowMeshInterval (4)
recordings for the snow's slow, unmarked change, also when nothing had changed (the level capped at
1.15 m, the camera still): a 4-frame stutter, frame p50 25 ms / p95 46 ms. The local patches only
took the snow passes' marked edits (presses, berms, footprints).

Now every patch first SCANS the published grid against the BUILT REFERENCE (per texel the (h, d) the
mesh was built from; clean_weather_snowmesh.comp binding 8, written by every vertices phase) and marks
what left it, so the patch takes the slow change too; the host reads the patch head back and builds on
the interval only when a patch could not take it (tile-cap overflow, the ring changed, the triangle
budget three quarters spent).

This check reads the production source:
  * planMeshBuild: the interval test (mMeshAge + 1 >= the interval) is not an ungated term of the
    `due = ...` chain; it is gated by mMeshNeedFull (or the patches being off);
  * readMeshHead: mMeshNeedFull from the overflow word, the ring-dirty word and the triangle budget;
  * prepare: the per-slot head readback, serial-checked, feeds readMeshHead;
  * recordMeshPatch: the SCAN dispatch (u0.y bit 28) before the EXPAND (bit 29), and the head copy;
  * the shader: the scan mode bit 28 (maxTris no longer takes it), the scan before the expand in main,
    and the vertices phase storing the reference.
The pre-#1494 source fails every one of them.
"""
import re

NAME = "snow-mesh-interval"
STAGE = "source"
DESCRIPTION = "snow mesh: the interval full build is gated on the reference scan (no 21 ms rebuild of an unchanged mesh)"
GUARDS = ("#1494: weather causing low FPS -- the snow mesh's full build (~21 ms on a 3060 in deep snow) every 4th "
          "recording with nothing changed (c_snow_mesh_build p50 0.02 / p95 21 ms).")

CPP = "indra/newview/vkgfx/llcleanweathervk.cpp"
COMP = "indra/vkclean/shaders/clean_weather_snowmesh.comp"


def _block(src, start):
    """The text of the brace block whose '{' is the first one at or after start."""
    i = src.find("{", start)
    if i < 0:
        return None
    depth = 0
    for j in range(i, len(src)):
        depth += {"{": 1, "}": -1}.get(src[j], 0)
        if depth == 0:
            return src[i:j + 1]
    return None


def _strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def _func(src, sig):
    m = re.search(sig, src)
    return _block(src, m.end()) if m else None


INTERVAL = r"mMeshAge\s*\+\s*1u?\s*>=\s*std::max"


def run(ctx, result):
    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return _strip_comments(f.read())

    cpp = read(CPP)
    comp = read(COMP)
    failures = []

    plan = _func(cpp, r"void\s+LLCleanWeatherVK::planMeshBuild\s*\(")
    if not plan or not re.search(INTERVAL, plan):
        result.fail("planMeshBuild / its interval test (mMeshAge + 1u >= std::max(...)) not found: the check covers nothing")
        return
    # every `due = ...;` / `due = due || ...;` statement: the interval must not be one of its terms
    for stmt in re.findall(r"\bdue\s*=\s*[^;]*;", plan):
        if re.search(INTERVAL, stmt):
            failures.append("planMeshBuild: the interval test is an ungated term of `due = ...` -- the ~21 ms full build "
                            "every RenderVKWeatherSnowMeshInterval recordings even with nothing changed (#1494)")
            break
    else:
        # the interval's own branch must consult mMeshNeedFull
        m = re.search(r"if\s*\([^{;]*" + INTERVAL + r"[^{;]*\)", plan)
        branch = _block(plan, m.end()) if m else None
        if not branch or "mMeshNeedFull" not in branch:
            failures.append("planMeshBuild: the interval build is not gated by mMeshNeedFull (the reference scan's verdict)")

    head = _func(cpp, r"void\s+LLCleanWeatherVK::readMeshHead\s*\(")
    if not head:
        failures.append("readMeshHead (the patch head's readback -> mMeshNeedFull) not found")
    else:
        if "mMeshNeedFull" not in head:
            failures.append("readMeshHead does not set mMeshNeedFull")
        if not re.search(r"w\s*\[\s*6u?\s*\]", head):
            failures.append("readMeshHead ignores the tile-list overflow word (marks head [6]): an overflowed patch's "
                            "edits would never reach the mesh")
        if not re.search(r"w\s*\[\s*8u?\s*\+\s*6u?\s*\]", head):
            failures.append("readMeshHead ignores the ring-dirty word (mesh_args[6]): the ring is never patched")
        if not re.search(r"w\s*\[\s*8u?\s*\+\s*5u?\s*\]", head):
            failures.append("readMeshHead ignores the triangle counter (mesh_args[5]): the patches' grown ranges "
                            "would exhaust the budget (holes)")

    prep = _func(cpp, r"bool\s+LLCleanWeatherVK::prepare\s*\(")
    if not prep or not re.search(r"mMeshReadSerial\s*\[\s*mSlot\s*\]\s*==\s*mMeshSerial", prep) or "readMeshHead(" not in prep:
        failures.append("prepare: the per-slot patch head readback (serial-checked against the last full build) does "
                        "not feed readMeshHead")

    patch = _func(cpp, r"void\s+LLCleanWeatherVK::recordMeshPatch\s*\(")
    if not patch:
        failures.append("recordMeshPatch not found")
    else:
        scan = patch.find("0x10000000u")
        expand = patch.find("0x20000000u")
        if scan < 0 or expand < 0 or scan > expand:
            failures.append("recordMeshPatch: no SCAN dispatch (u0.y bit 28) before the EXPAND (bit 29): the slow "
                            "change never reaches the patches")
        if not re.search(r"vkCmdCopyBuffer\s*\(\s*cmd\s*,\s*mMeshMarks\.buffer\s*,\s*mMeshRead\s*\[", patch) \
                or not re.search(r"vkCmdCopyBuffer\s*\(\s*cmd\s*,\s*mMeshArgs\.buffer\s*,\s*mMeshRead\s*\[", patch):
            failures.append("recordMeshPatch: the marks' head / the arguments are not copied back (mMeshRead)")

    # the shader
    if not re.search(r"bool\s+modeScan\s*\(\s*\)\s*\{\s*return\s*\(\s*mp\.u0\.y\s*&\s*0x10000000u\s*\)", comp):
        failures.append("clean_weather_snowmesh.comp: modeScan (u0.y bit 28) missing")
    if not re.search(r"uint\s+maxTris\s*\(\s*\)\s*\{\s*return\s+mp\.u0\.y\s*&\s*0x0FFFFFFFu", comp):
        failures.append("clean_weather_snowmesh.comp: maxTris() still takes bit 28 (the scan's mode bit)")
    mm = re.search(r"void\s+main\s*\(\s*\)", comp)
    main = _block(comp, mm.end()) if mm else ""
    s = main.find("modeScan()")
    e = main.find("modeExpand()")
    if s < 0 or e < 0 or s > e:
        failures.append("clean_weather_snowmesh.comp main: the scan mode is not dispatched (before the expand)")
    if not re.search(r"imageStore\s*\(\s*uRef\s*\[\s*0\s*\]", main) or not re.search(r"imageStore\s*\(\s*uRef\s*\[\s*1\s*\]", main):
        failures.append("clean_weather_snowmesh.comp main: the vertices phase does not store the built reference "
                        "(both layers): the scan would compare against a stale one")
    sc = _func(comp, r"void\s+scanReference\s*\(\s*\)")
    if not sc or not re.search(r"atomicOr\s*\(\s*mesh_args\s*\[\s*6u?\s*\]", sc) \
            or not re.search(r"atomicOr\s*\(\s*mk\s*\[", sc):
        failures.append("clean_weather_snowmesh.comp scanReference: must mark the inner tile (mk) and the ring-dirty "
                        "word (mesh_args[6])")

    if failures:
        for f in failures:
            result.fail(f)
        return
    result.summary = "interval build gated on the reference scan (overflow / ring / budget); scan before expand; head read back"
