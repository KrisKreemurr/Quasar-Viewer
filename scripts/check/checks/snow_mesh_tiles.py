"""Regression guard for #1496: the snow mesh rebuilt from scratch whenever the camera moved 0.25 m or
turned 8 degrees (~21 ms on an RTX 3060 in deep snow; a walking camera paid it every 2 - 3 frames),
because the build baked the camera in: the strides per block, the stride / inner -> ring geomorphs,
the lift, the depth fades, the view cone.

The tile cache (RenderVKWeatherSnowMeshTiles) builds world-anchored 8 x 8 tiles once, camera-free, and
does the LOD and the culling at draw time. This check reads the production source and fails on the
pre-#1496 tree:
  * the setting exists, default on, not saved (the A/B with the old path until confirmed);
  * planMeshBuild takes the tile path before the old build, and the tile plan has no camera-move /
    turn rebuild trigger (RenderVKWeatherSnowMeshRebuildMove, kMeshRebuildTurnCos);
  * the tile build (clean_weather_snowmesh.comp -DWX_MESH_TILES) reads no camera: texelHD's fades are
    the old path's only, the tile section never touches mp.cam / mp.fwd / innerBand / ringFade;
  * the VS does the stride morph, the ring geomorph, the fades and the lift from its per-draw camera;
  * the cull gives every sun cascade its own list (recordMeshDraw's cascade, the renderer passes it
    and the cascades' planes).
And it runs a CPU MODEL of the cross-tile invariants, with the radii / windows / thresholds parsed
from the shaders (so it tests what ships):
  I1 wrap-around addressing: a snap moves no vertex (a texel that stays keeps its slot; the slots of
     a window are a bijection), tile records likewise;
  I2 the dirty radii cover every reader: a texel change reaches vertices up to the apron field's reach
     + 1 texel and cells up to their corners / neighbour blocks + the vertices those read -- the plan's
     radii (2 / 3 tiles) and its 3 x 3 vertex neighbourhood must hold them;
  I3 no cracks at stride hand-overs: with the cull's stride choice from the tile AABB (never coarser
     than the distance asks; finer when a stride is not built), every vertex on a coarse tile's border
     is fully morphed onto the coarse lattice (the VS's smoothstep window ends inside the switch
     distance) -- and a policy that draws coarser than asked must fail the model (it has teeth);
  I4 the seams are symmetric (both tiles of a stride hand-over draw their seam skirts).
"""
import math
import os
import random
import re

NAME = "snow-mesh-tiles"
STAGE = "source"
DESCRIPTION = "snow mesh tile cache: camera-free tiles, draw-time LOD / cull, cross-tile invariants (CPU model)"
GUARDS = ("#1496: the snow mesh's full rebuild on every 0.25 m of camera movement / 8 degrees of turn "
          "(~21 ms on a 3060 in deep snow, every 2 - 3 frames while walking).")

CPP = "indra/newview/vkgfx/llcleanweathervk.cpp"
REN = "indra/newview/vkgfx/llcleanrenderervk.cpp"
COMP = "indra/vkclean/shaders/clean_weather_snowmesh.comp"
MGR = "indra/vkclean/shaders/clean_weather_snowmesh_tiles.comp"
VERT = "indra/vkclean/shaders/clean_weather_snowmesh.vert"
JOIN = "indra/vkclean/shaders/clean_weather_snow_join.glsl"
SETTINGS = "indra/newview/app_settings/settings.xml"


def _strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def _block(src, start):
    i = src.find("{", start)
    if i < 0:
        return None
    depth = 0
    for j in range(i, len(src)):
        depth += {"{": 1, "}": -1}.get(src[j], 0)
        if depth == 0:
            return src[i:j + 1]
    return None


def _func(src, sig):
    m = re.search(sig, src)
    return _block(src, m.end()) if m else None


def _smoothstep(e0, e1, x):
    t = min(max((x - e0) / (e1 - e0), 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


# ---- the CPU model ------------------------------------------------------------------------------

def model_wrap(failures):
    """I1: the wrap-around slots (clean_weather_snowmesh.comp wrapTexel / clean_weather_snowmesh_tiles.glsl)."""
    rnd = random.Random(1496)
    for res in (512, 256, 128):
        snap = res // 8
        org = (rnd.randrange(-40, 40) * snap, rnd.randrange(-40, 40) * snap)
        def slot(o, t):
            return ((o[0] + t[0]) % res, (o[1] + t[1]) % res)
        for _ in range(6):
            new = (org[0] + rnd.choice((-1, 0, 1)) * snap, org[1] + rnd.choice((-1, 0, 1)) * snap)
            # a texel in both windows keeps its slot; the new window's slots are a bijection (sampled rows)
            for ty in range(0, res, 37):
                seen = set()
                for tx in range(res):
                    s = slot(new, (tx, ty))
                    seen.add(s)
                    w = (new[0] + tx, new[1] + ty)
                    told = (w[0] - org[0], w[1] - org[1])
                    if 0 <= told[0] < res and 0 <= told[1] < res and slot(org, told) != s:
                        failures.append("I1: a texel that stays in the window moved slot at res %d (snap %s -> %s)" % (res, org, new))
                        return
                if len(seen) != res:
                    failures.append("I1: the window's slots are not a bijection (res %d)" % res)
                    return
            if org[0] % 8 or org[1] % 8:
                failures.append("I1: a grid origin is not a whole number of tiles (res %d)" % res)
                return
            org = new


def model_radii(failures, k_ak, v_radius, c_radius, nb):
    """I2: every tile whose vertices / cells read a changed texel lies within the plan's radii. Offsets
    are texels from the tile's first; a tile's anchors are 0 .. 7, strides <= 4."""
    v_reach = k_ak + 1   # a vertex reads texels within kAK + 1 (the apron field's window + its central differences)
    # a tile's VERTICES read texels [-v, 7 + v]
    v_lo, v_hi = -v_reach, 7 + v_reach
    # its CELLS: corners [0, 8]; the neighbour cells t +- st and their corners [-4, 12]; the blocks of those
    # (listStride: 4 corners + 16 apron slots) [-4, 11] -- vertex records, each reading texels within v
    rec_lo, rec_hi = -4, 11
    c_lo, c_hi = min(-4, rec_lo - v_reach), max(12, rec_hi + v_reach)
    def reach(lo, hi):
        # the farthest tile g (|g|) whose window [8 g + lo, 8 g + hi] holds texel 0
        return max(abs(g) for g in range(-12, 13) if 8 * g + lo <= 0 <= 8 * g + hi)
    need_v, need_c = reach(v_lo, v_hi), reach(c_lo, c_hi)
    if need_v > v_radius:
        failures.append("I2: a texel change moves vertices %d tiles away, the plan's vertex radius is %d" % (need_v, v_radius))
    if need_c > c_radius:
        failures.append("I2: a texel change moves cells %d tiles away, the plan's cell radius is %d" % (need_c, c_radius))
    # the vertex records a tile's cells index / read: its 3 x 3 neighbourhood (plan B builds them first)
    vt = max(abs(rec_lo // 8), abs(rec_hi // 8))
    if vt > nb:
        failures.append("I2: a tile's cells read vertex records %d tiles away, plan B builds the %d-neighbourhood" % (vt, nb))


def model_morph(failures, m0, m1, lod1, lod2, coarser=False):
    """I3: at every stride hand-over the coarse tile's border vertices are fully morphed. Returns the
    number of violations (the 'coarser' policy must produce some)."""
    rnd = random.Random(7)
    bad = 0
    w = 1.0 / 16.0
    for _ in range(4000):
        cam = (rnd.uniform(-30, 30), rnd.uniform(-30, 30), rnd.uniform(-5, 15))
        gx = rnd.randrange(-60, 60)
        gy = rnd.randrange(-60, 60)
        def footprint(g):
            # the closed footprint's vertices Q = (x, y, h): 9 x 9 texels (+ a smoothed outline's half texel)
            pts = []
            for j in range(0, 9, 2):
                for i in range(0, 9, 2):
                    x = (8 * g[0] + i) * w + rnd.uniform(-0.5, 0.5) * w
                    y = (8 * g[1] + j) * w + rnd.uniform(-0.5, 0.5) * w
                    pts.append((x, y, rnd.uniform(-1.0, 1.5)))
            lo = tuple(min(p[a] for p in pts) for a in range(3))
            hi = tuple(max(p[a] for p in pts) for a in range(3))
            return pts, lo, hi
        def stride(lo, hi, built=(1, 2, 4)):
            d = math.sqrt(sum(max(lo[a] - cam[a], cam[a] - hi[a], 0.0) ** 2 for a in range(3)))
            want = 1 if d < lod1 else (2 if d < lod2 else 4)
            if coarser:
                want = min(want * 2, 4)   # (a broken cull: one stride too coarse)
            if want == 4 and 4 in built:
                return 4
            if want >= 2 and 2 in built:
                return 2
            return 1
        built = rnd.choice(((1, 2, 4), (1, 2), (1,)))
        A = footprint((gx, gy))
        B = footprint((gx + 1, gy))
        sa, sb = stride(A[1], A[2], built), stride(B[1], B[2], built)
        if sa == sb:
            continue
        coarse = A if sa > sb else B
        sc = max(sa, sb)
        # the shared column's vertices are in the coarse tile's footprint (its first / its last column)
        for q in coarse[0]:
            d = math.sqrt(sum((q[a] - cam[a]) ** 2 for a in range(3)))
            f1 = _smoothstep(m0 * lod1, m1 * lod1, d)
            f2 = _smoothstep(m0 * lod2, m1 * lod2, d)
            if f1 < 1.0 or (sc == 4 and f2 < 1.0):
                bad += 1
                break
    if bad and not coarser:
        failures.append("I3: %d stride hand-overs with a border vertex not fully morphed (cracks)" % bad)
    return bad


def run(ctx, result):
    def read(rel, strip=True):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            s = f.read()
        return _strip_comments(s) if strip else s

    failures = []
    missing = [r for r in (CPP, REN, COMP, MGR, VERT, JOIN, SETTINGS) if not os.path.exists(ctx.path(r))]
    if missing:
        result.fail("missing: %s (the tile cache's plan / cull kernels, #1496)" % ", ".join(missing))
        return
    cpp, ren, comp, mgr, vert, join = read(CPP), read(REN), read(COMP), read(MGR), read(VERT), read(JOIN)
    settings = read(SETTINGS, strip=False)

    # ---- the setting: default on, not saved
    m = re.search(r"<key>RenderVKWeatherSnowMeshTiles</key>\s*<map>(.*?)</map>", settings, re.S)
    if not m:
        result.fail("settings.xml: RenderVKWeatherSnowMeshTiles missing (the tile cache's A/B toggle)")
        return
    body = m.group(1)
    if not re.search(r"<key>Persist</key>\s*<integer>0</integer>", body) or not re.search(r"<key>Value</key>\s*<integer>1</integer>", body):
        failures.append("RenderVKWeatherSnowMeshTiles must default on and not persist (Persist 0, Value 1)")

    # ---- the host
    plan = _func(cpp, r"void\s+LLCleanWeatherVK::planMeshBuild\s*\(")
    if not plan:
        result.fail("planMeshBuild not found: the check covers nothing")
        return
    t = plan.find("RenderVKWeatherSnowMeshTiles")
    p = plan.find("planMeshTiles(")
    o = plan.find("ensureMesh(cmd, want)")
    if t < 0 or p < 0 or o < 0 or not (t < p < o):
        failures.append("planMeshBuild: the tile path (RenderVKWeatherSnowMeshTiles -> planMeshTiles) does not come before the "
                        "old per-camera build")
    pt = _func(cpp, r"void\s+LLCleanWeatherVK::planMeshTiles\s*\(")
    if not pt:
        failures.append("planMeshTiles not found")
    elif re.search(r"RenderVKWeatherSnowMeshRebuildMove|kMeshRebuildTurnCos|mMeshCam\b|mMeshDir\b", pt):
        failures.append("planMeshTiles reads a camera-move / turn rebuild trigger: the tile cache must not rebuild on camera motion")
    rt = _func(cpp, r"void\s+LLCleanWeatherVK::recordMeshTiles\s*\(")
    if not rt or not all(s in rt for s in ("mgr(6u)", "vkCmdDispatchIndirect(cmd, mTileState.buffer, (VkDeviceSize)kTsHCList")):
        failures.append("recordMeshTiles: the cull (mode 6) / the indirect cell build are not recorded")
    draw = _func(cpp, r"void\s+LLCleanWeatherVK::recordMeshDraw\s*\(\s*VkCommandBuffer\s+cmd\s*,\s*bool\s+shadow\s*,\s*uint32_t\s+cascade\s*\)")
    if not draw or "1u + cascade" not in draw:
        failures.append("recordMeshDraw: no per-cascade tile list (view 1 + cascade)")
    if len(re.findall(r"recordMeshDraw\s*\(\s*cmd\s*,\s*true\s*,\s*\(uint32_t\)\s*c\s*\)", ren)) < 2:
        failures.append("the renderer's sun-cascade passes do not hand recordMeshDraw their cascade")
    if not re.search(r"mShadowOn\s*\?\s*mShadowPlanes\s*:\s*nullptr\s*,\s*NUM_CASCADES", ren):
        failures.append("the renderer does not hand recordMeshBuild the cascades' planes")

    # ---- the tile build reads no camera
    i = comp.find("void tileScan()")
    j = comp.find("#endif", comp.find("void main()", i if i >= 0 else 0))
    if i < 0:
        failures.append("clean_weather_snowmesh.comp: the tile build (tileScan / tileVerts / tileCells) is missing")
    else:
        sec = comp[i:j if j > i else len(comp)]
        for bad in (r"\bmp\.cam\b", r"\bmp\.fwd\.xyz", r"\binnerBand\s*\(", r"\bringFade\s*\(", r"blockStrideW?\s*\("):
            if re.search(bad, sec):
                failures.append("clean_weather_snowmesh.comp tile build reads the camera (%s): a tile must be camera-free" % bad)
    tc = _func(comp, r"void\s+tileCells\s*\(")
    if not tc or not re.search(r"st0\s*&\s*\(\s*kSV\s*\|\s*kSHint\s*\)", tc):
        failures.append("clean_weather_snowmesh.comp tileCells: the record's state must keep kSV | kSHint (without the height "
                        "hint every built tile's plan box spans the whole map box: under a tilt nothing is evicted, the pool fills)")
    hd = _func(comp, r"bool\s+texelHD\s*\(")
    if not hd or not re.search(r"#ifndef\s+WX_MESH_TILES[^#]*ringFade\s*\(", hd):
        failures.append("clean_weather_snowmesh.comp texelHD: the camera's depth fades are not limited to the old path")

    # ---- the VS's draw-time LOD
    vm = vert[vert.find("float edgeFadeAt"):] if "float edgeFadeAt" in vert else ""
    for need, why in ((r"smoothstep\(\s*0\.75\s*\*\s*c4\.x\s*,\s*0\.92\s*\*\s*c4\.x", "the stride-2 morph"),
                      (r"smoothstep\(\s*0\.75\s*\*\s*c4\.y\s*,\s*0\.92\s*\*\s*c4\.y", "the stride-4 morph"),
                      (r"mix\(\s*z\s*,\s*a_morph\.z\s*,\s*band\s*\)", "the inner -> ring geomorph"),
                      (r"c4\.z\s*\+\s*c4\.w\s*\*\s*dist", "the lift"),
                      (r"c6\.xyz", "the live camera")):
        if not re.search(need, vm):
            failures.append("clean_weather_snowmesh.vert -DWX_MESH_TILES: %s is not done at draw time" % why)

    # ---- the CPU model, with the shaders' numbers
    mm = re.search(r"smoothstep\(\s*([0-9.]+)\s*\*\s*c4\.x\s*,\s*([0-9.]+)\s*\*\s*c4\.x", vert)
    m0, m1 = (float(mm.group(1)), float(mm.group(2))) if mm else (0.75, 0.92)
    lm = re.search(r"kMeshLod1\s*=\s*([0-9.]+)f;", cpp)
    lm2 = re.search(r"kMeshLod2\s*=\s*([0-9.]+)f;", cpp)
    lod1 = float(lm.group(1)) if lm else 10.0
    lod2 = float(lm2.group(1)) if lm2 else 20.0
    ds = _func(mgr, r"uint\s+drawStride\s*\(")
    if not ds or not re.search(r"dmin\s*<\s*lod\.x\s*\?\s*1u\s*:\s*\(\s*dmin\s*<\s*lod\.y\s*\?\s*2u\s*:\s*4u", ds):
        failures.append("clean_weather_snowmesh_tiles.comp drawStride: not the AABB-distance stride the model checks")
    if ds and not re.search(r"return\s+1u\s*;\s*\}\s*$", ds.strip()):
        failures.append("clean_weather_snowmesh_tiles.comp drawStride: the fallback is not the finest stride")
    pa = _func(mgr, r"void\s+planA\s*\(")
    rv = re.search(r"kSV\)\s*!=\s*0u\s*&&\s*changedWithin\(\s*layer\s*,\s*g\s*,\s*(\d+)\s*\)", pa or "")
    rc = re.search(r"kSC\)\s*!=\s*0u\s*&&\s*changedWithin\(\s*layer\s*,\s*g\s*,\s*(\d+)\s*\)", pa or "")
    pb = _func(mgr, r"void\s+planB\s*\(")
    nbm = re.search(r"for\s*\(\s*int\s+dy\s*=\s*-(\d+)\s*;", pb or "")
    ak = re.search(r"kSnowApronTexels\s*=\s*(\d+)", join)
    if not (rv and rc and nbm and ak):
        failures.append("the plan's dirty radii / vertex neighbourhood / the apron reach could not be read: the model covers nothing")
    else:
        model_radii(failures, int(ak.group(1)), int(rv.group(1)), int(rc.group(1)), int(nbm.group(1)))
    model_wrap(failures)
    if model_morph(failures, m0, m1, lod1, lod2) == 0 and model_morph([], m0, m1, lod1, lod2, coarser=True) == 0:
        failures.append("I3 model: a cull drawing one stride too coarse went unnoticed -- the model has no teeth")
    # I4: the seams -- a tile draws its seam skirts when ANY neighbour's stride differs (symmetric)
    cull = _func(mgr, r"void\s+tileCull\s*\(")
    if not cull or not re.search(r"drawStride\(\s*layer\s*,\s*nrec\s*,\s*nst\s*,\s*nlo\s*,\s*nhi\s*\)\s*!=\s*s", cull):
        failures.append("I4: the cull's seam test is not 'a neighbour drawn at another stride'")

    if failures:
        for f in failures:
            result.fail(f)
        return
    result.summary = ("tile path before the old build, camera-free tiles, draw-time LOD, per-cascade lists; model I1-I4 hold "
                      "(morph window %.2f-%.2f x the switch, radii from the source)" % (m0, m1))
