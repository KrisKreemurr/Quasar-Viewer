"""Regression guard for #1493: dark camouflage cells over the water with RT reflections on.

The RT forward pass (clean_deferred_light.comp CLEAN_DEFER_PASS 14) finds the water layer on the FLAT LL
water plane in the TLAS, moves the point onto the displaced sea (seaRayHit) and traces one reflection ray
from there. That ray's walk (rtrReflWalk) took every class but blend, the water class included. From a
trough -- the displaced point under the flat plane -- the upward reflection (rtWaterReflectDir: R.z >= 0)
met the flat plane from below a few cm away; the hit shaded as water seen from under the surface (the
water fog colour) and the water FS spliced it in place of the sky: dark blotches over every trough, the
sky only on the crests -- the sign of the Gerstner sum, at the half-resolution block grid.

This check evaluates, from the production source:
  * the reflection walk's class mask for a WATER receiver of pass 14 (rtrReflWalk's rmask expression with
    the water branch's g_rtr_mask_drop, RT_MASK_* values from clean_rt_common.glsl): the water class must
    be out of it -- else a trough's reflection ray can hit the plane that stands in for its own surface;
  * the same mask for a blend / PBR receiver of pass 14: the water class must stay in (glass over the sea
    reflects the sea);
  * seaRayHit's surface: the waves VS's geometry LOD fade (seaSurface's camDist = the camera's horizontal
    distance to the undisplaced point), so the traced point -- the reflection origin and the depth the
    water FS matches -- is the drawn one, not a full-amplitude trough under a geometrically flat far sea.
The pre-#1493 source fails the first and the last.
"""
import re

NAME = "water-refl-self-hit"
STAGE = "source"
DESCRIPTION = "RT forward reflections: a water receiver's reflection ray never hits the flat water plane"
GUARDS = ("#1493: blotchy dark (hexagonal / camouflage) cells all over the water with RT reflections on "
          "(RenderVKRTReflectionsForward), the sky reflection only between them -- trough reflections "
          "hitting the flat TLAS water plane from below.")

LIGHT = "indra/vkclean/shaders/clean_deferred_light.comp"
COMMON = "indra/vkclean/shaders/clean_rt_common.glsl"
WAVES_VS = "indra/vkclean/shaders/clean_water_waves.vert"


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


def _literals(e):
    """GLSL uint literals (0x20u, 3u) -> Python ints."""
    e = re.sub(r"\b0x([0-9A-Fa-f]+)u?\b", lambda m: str(int(m.group(1), 16)), e)
    return re.sub(r"\b(\d+)u\b", r"\1", e)


def _masks(common):
    vals = {}
    for name, expr in re.findall(r"#define\s+(RT_MASK_\w+)\s+([^\n]+)", _strip_comments(common)):
        vals[name] = expr.strip()
    out = {}

    def ev(name, depth=0):
        if name in out:
            return out[name]
        if depth > 16 or name not in vals:
            raise KeyError(name)
        e = re.sub(r"\bRT_MASK_\w+\b", lambda m: "(%d)" % ev(m.group(0), depth + 1), _literals(vals[name]))
        out[name] = eval(e, {"__builtins__": {}}) & 0xFF
        return out[name]

    for n in vals:
        ev(n)
    return out


def _eval_mask(expr, masks, drop):
    e = _literals(re.sub(r"\bg_rtr_mask_drop\b", "(%d)" % drop, expr))
    e = re.sub(r"\bRT_MASK_\w+\b", lambda m: "(%d)" % masks[m.group(0)], e)
    if re.search(r"[A-Za-z_]", e):
        raise ValueError(expr)
    return eval(e, {"__builtins__": {}}) & 0xFF


def run(ctx, result):
    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    light = _strip_comments(read(LIGHT))
    try:
        masks = _masks(read(COMMON))
        water = masks["RT_MASK_WATER"]
    except (KeyError, SyntaxError) as e:
        result.fail("clean_rt_common.glsl: RT_MASK_* not evaluable (%s): the check covers nothing" % e)
        return

    # rtrReflWalk's class mask
    m = re.search(r"RtHit\s+rtrReflWalk\s*\(", light)
    walk = _block(light, m.end()) if m else None
    rm = re.search(r"const\s+uint\s+rmask\s*=\s*([^;]+);", walk or "")
    if not rm:
        result.fail("rtrReflWalk / its rmask not found (moved or renamed?): the check covers nothing")
        return
    rmask = rm.group(1)

    # pass 14's block, its main and the water / other receiver branches
    p = re.search(r"#elif\s+CLEAN_DEFER_PASS\s*==\s*14\b", light)
    end = re.search(r"#elif\s+CLEAN_DEFER_PASS\s*==", light[p.end():]) if p else None
    if not p or not end:
        result.fail("the RT forward pass (CLEAN_DEFER_PASS 14) not found: the check covers nothing")
        return
    fwd = light[p.end():p.end() + end.start()]
    mm = re.search(r"void\s+DEFER_MAIN\s*\(\s*\)", fwd)
    main = _block(fwd, mm.end()) if mm else None
    wb = re.search(r"if\s*\(\s*ls\.model\s*==\s*RT_SURF_WATER\s*\)", main or "")
    if not wb or "rtrTraceRefl(" not in main:
        result.fail("pass 14's water branch / its rtrTraceRefl not found: the check covers nothing")
        return
    water_branch = _block(main, wb.end())
    before_trace = main[:main.find("rtrTraceRefl(")]
    others = before_trace.replace(water_branch, "")

    def drop_in(text):
        d = 0
        for expr in re.findall(r"\bg_rtr_mask_drop\s*=\s*([^;]+);", text):
            d = _eval_mask(expr, masks, 0)
        return d

    failures = []
    water_mask = _eval_mask(rmask, masks, drop_in(water_branch))
    other_mask = _eval_mask(rmask, masks, drop_in(others))
    if water_mask & water:
        failures.append("pass 14: a WATER receiver's reflection walk takes the water class (mask 0x%02x): a trough's "
                        "upward ray hits the flat TLAS water plane from below -- the #1493 dark cells" % water_mask)
    if not other_mask & water:
        failures.append("pass 14: blend / PBR receivers lost the water class (mask 0x%02x): glass over the sea no "
                        "longer reflects it" % other_mask)

    # seaRayHit's surface = the waves VS's (the geometry LOD fade by the horizontal camera distance)
    vs = _strip_comments(read(WAVES_VS))
    if not re.search(r"seaSurface\(\s*xy\s*,\s*length\(\s*xy\s*-\s*cam\s*\)", vs) \
            or not re.search(r"const\s+vec2\s+cam\s*=\s*ssr\.cam_origin\.xy\s*;", vs):
        failures.append("clean_water_waves.vert: the geometry LOD fade is no longer seaSurface(xy, length(xy - "
                        "ssr.cam_origin.xy), ...): re-check seaRayHit's parity and update this check")
    sr = re.search(r"vec3\s+seaRayHit\s*\(", fwd)
    sea_hit = _block(fwd, sr.end()) if sr else ""
    if not re.search(r"seaSurface\(\s*x0\s*,\s*length\(\s*x0\s*-\s*ssr\.cam_origin\.xy\s*\)", sea_hit):
        failures.append("pass 14 seaRayHit: the march's surface is not the drawn one (seaSurface(x0, length(x0 - "
                        "ssr.cam_origin.xy), 0.0)): far-field troughs the raster drew flat")

    if failures:
        for f in failures:
            result.fail(f)
        return
    result.summary = "water receiver walk mask 0x%02x (no water), blend/PBR 0x%02x; seaRayHit = drawn surface" % (
        water_mask, other_mask)
