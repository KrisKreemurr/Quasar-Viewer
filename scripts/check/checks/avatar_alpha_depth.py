"""Regression guard for avatars blending with themselves (RenderVKAvatarAlphaDepthThreshold).

An avatar's alpha-blend layers (rigged meshes, worn attachments, cluster LOD avatars) blended over each
other in triangle / attachment order: a 90% avatar showed every hidden layer (body over body, the far
side of the hair through the near side), and how much changed with the view. Now a depth pre-pass over
the avatars' blend faces writes a SEPARATE avatar depth (mAvDepth: a copy of pass 1's final depth + every
texel whose final alpha reaches T, clean_blend_depth_rule.glsl THE rule at the avatar threshold) and the
avatars' blend draws test it: per pixel only the nearest dense surface of an avatar shades and blends, over
everything opaque behind it (its own face / body included) and over the world's blend layers, which keep
the main depth. Fringes (< T) blend as before.

The fixture runs THE rule's own source (blendDepthKeeps from clean_blend_depth_rule.glsl, avatarDepthFace
from clean_draw_view.glsl, alphaKeeps from clean_alpha_rule.glsl -- translated to Python) through a CPU
model of the pre-pass + composite: a 0.9 hair over an opaque face gives 0.9 hair + 0.1 face; two 0.9
layers over the face give the near one only; a 90% ghost of three body shells reads as one 90% shell; a
fringe in front still blends, one behind is rejected; world glass behind the ghost still shows through it;
T > 1 is today's self-blending; back faces, Highlight Transparent, a cutoff the lit FS would discard, HUD
faces and world faces write nothing. Plus the wiring the model cannot see: the setting, the target, its
binding (69) in the layout and both readers, the frame word (history_time.y), the push bit agreeing
between host and shaders (and the cluster task stage masking it out of its view index), the pass-2 avatar
scope, the OIT accumulation and the covers testing the avatar depth.
"""
import re

NAME = "avatar-alpha-depth"
STAGE = "source"
DESCRIPTION = "avatar alpha depth: CPU model of THE rule at T (no self-blending; opaque + world still behind)"
GUARDS = ("An avatar's blend layers blending over each other (a 90% avatar showing its hidden shells, the far "
          "side of the hair through the near side), the avatar pre-pass depth leaking into the main depth "
          "(a ghost opaque to the lighting / SSR / haze, or hiding its own opaque face), or the avatar depth "
          "not bound / not tested by one of the avatar blend paths.")

HT_ON = False   # Highlight Transparent (ssr.ao_params2.z) in the current case


def _cut(text, head):
    i = text.find(head)
    if i < 0:
        return None
    e = text.find("\n", i)
    first = text[i:e]
    if "{" in first and first.rstrip().endswith("}"):   # a one-line function
        body = first[first.find("{") + 1:first.rfind("}")]
        return first[:first.find("{")] + "\n" + body.strip() + "\n}"
    j = text.find("\n}", i)
    return text[i:j + 2] if j > 0 else None


def _to_py(fn):
    """A short GLSL function (ifs with single-statement returns) as Python source."""
    fn = re.sub(r"//[^\n]*", "", fn)
    fn = re.sub(r"^\s*(?:bool|float|uint|int)\s+(\w+)\s*\(([^)]*)\)\s*\{?",
                lambda m: "def %s(%s):" % (m.group(1), ", ".join(p.split()[-1] for p in m.group(2).split(",") if p.strip())),
                fn, count=1)
    body = []
    for raw in fn.split("\n")[1:]:
        s = raw.strip()
        if not s or s in ("{", "}"):
            continue
        s = s.rstrip(";")
        s = re.sub(r"(\d)u\b", r"\1", s)
        s = s.replace("&&", " and ").replace("||", " or ")
        s = re.sub(r"!(?!=)", " not ", s)
        s = re.sub(r"\btrue\b", "True", s)
        s = re.sub(r"\bfalse\b", "False", s)
        m = re.match(r"if\s*(\(.*\))\s*return\s+(.*)$", s)
        if m:
            body.append("    if %s: return %s" % (m.group(1), m.group(2)))
        else:
            body.append("    " + s)
    return fn.split("\n")[0] + "\n" + "\n".join(body) + "\n"


class _V:
    def __init__(self, **k):
        self.__dict__.update(k)


def _rule(ctx, missing):
    """THE rule's functions from the shader sources, executable."""
    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()
    rule = read("indra/vkclean/shaders/clean_blend_depth_rule.glsl")
    view = read("indra/vkclean/shaders/clean_draw_view.glsl")
    alpha = read("indra/vkclean/shaders/clean_alpha_rule.glsl")
    srcs = {"blendDepthKeeps": _cut(rule, "bool blendDepthKeeps("),
            "avatarDepthFace": _cut(view, "bool avatarDepthFace("),
            "alphaKeeps": _cut(alpha, "bool alphaKeeps("),
            "alphaCoverage": _cut(alpha, "float alphaCoverage(")}
    for k, v in srcs.items():
        if v is None:
            missing.append("%s not found (moved / renamed?): the fixture covers nothing" % k)
    if missing:
        return None
    # alphaCoverage is a ternary one-liner: give it its Python form directly (same arithmetic)
    srcs["alphaCoverage"] = ("def alphaCoverage(a, lod, k):\n"
                             "    return a * (1.0 + max(lod, 0.0) * k) if k > 0.0 else a\n")
    m = re.search(r"const float kBlendDepthAlpha\s*=\s*([0-9.]+)", rule)
    g = {"REZFX_DYING": 0x00400000, "rezfxFadeOpacity": lambda flags: 1.0,
         "kBlendDepthAlpha": float(m.group(1)) if m else 0.99}
    for k in ("alphaCoverage", "alphaKeeps", "avatarDepthFace", "blendDepthKeeps"):
        try:
            exec(_to_py(srcs[k]), g)
        except Exception as e:   # (the translation is the fixture's; a reshaped function needs it extended)
            missing.append("%s: the fixture could not translate it (%s):\n%s" % (k, e, _to_py(srcs[k])))
    return None if missing else g


RIG, ATTACH, FLEXI, HUD, DOUBLE = 1, 512, 524288, 8, 256


def _composite(g, T, opaque, layers, ht=False):
    """One pixel, reversed-Z (nearer = larger). opaque = (depth, colour) -- the world or the avatar's own
    opaque face / body; layers: dicts depth, alpha, colour, flags[, front, cutoff]. The host's sequence:
    the near-opaque 0.99 texels in the MAIN depth (existing), the avatar depth = copy + the avatar faces'
    texels at T (only while 0 < T <= 1), then the back-to-front composite: avatar faces test the avatar
    depth (GEQUAL: the dense surface passes its own depth), every other face the main depth."""
    g["gl_FrontFacing"] = True
    g["ssr"] = _V(ao_params2=_V(z=1.0 if ht else 0.0))

    def keeps(L, thr):
        g["gl_FrontFacing"] = L.get("front", True)
        return g["blendDepthKeeps"](L["alpha"], L.get("cutoff", -1.0), L["flags"], thr)
    main = opaque[0]
    for L in layers:
        if keeps(L, g["kBlendDepthAlpha"]):
            main = max(main, L["depth"])
    live = 0.0 < T <= 1.0
    av = main
    if live:
        for L in layers:
            if g["avatarDepthFace"](L["flags"]) and keeps(L, T):
                av = max(av, L["depth"])
    col = opaque[1]
    for L in sorted(layers, key=lambda x: x["depth"]):   # back to front
        if not L.get("front", True) and (L["flags"] & (DOUBLE | HUD)) == 0:
            continue                                        # the lit FS's single-sided back-face discard
        if L.get("cutoff", -1.0) >= 0.0 and L["alpha"] < L["cutoff"]:
            continue                                        # the lit FS's cutoff discard
        ref = av if (live and g["avatarDepthFace"](L["flags"])) else main
        if L["depth"] < ref:
            continue                                        # behind: the depth test rejects it before shading
        col = L["alpha"] * L["colour"] + (1.0 - L["alpha"]) * col
    return col


def _fixture(g):
    bad = []

    def expect(name, got, want):
        if abs(got - want) > 1e-6:
            bad.append("%s: got %.6f, want %.6f" % (name, got, want))
    face = (0.30, 0.20)      # the avatar's own opaque face / body (in the main depth, lit underneath)
    world = (0.10, 0.70)     # the world behind the avatar
    H1, H2 = 1.0, 0.5        # two hair colours
    # (owner) the hair over the face: the face shows through at the hair's alpha, exactly as today
    expect("0.9 hair over an opaque face", _composite(g, 0.5, face, [dict(depth=0.40, alpha=0.9, colour=H1, flags=RIG)]),
           0.9 * H1 + 0.1 * face[1])
    # (owner) two 0.9 hair layers over the face: the near one only, over the face -- not the far layer
    two = [dict(depth=0.40, alpha=0.9, colour=H2, flags=RIG), dict(depth=0.45, alpha=0.9, colour=H1, flags=RIG)]
    expect("two 0.9 layers over the face", _composite(g, 0.5, face, two), 0.9 * H1 + 0.1 * face[1])
    # ... and today's self-blending with the setting above 1
    expect("two 0.9 layers, T > 1 (off)", _composite(g, 1.5, face, two),
           0.9 * H1 + 0.1 * (0.9 * H2 + 0.1 * face[1]))
    # the 90% ghost: three body shells (body, shirt, jacket; mixed rigged / worn rigid attachment) over the world
    ghost = [dict(depth=0.30, alpha=0.9, colour=0.2, flags=RIG), dict(depth=0.31, alpha=0.9, colour=0.4, flags=ATTACH),
             dict(depth=0.32, alpha=0.9, colour=0.6, flags=RIG | DOUBLE)]
    expect("90% ghost, three shells", _composite(g, 0.5, world, ghost), 0.9 * 0.6 + 0.1 * world[1])
    # fringes: in front of the dense surface still blends over it; behind it is rejected
    fr = [dict(depth=0.40, alpha=0.9, colour=H2, flags=RIG), dict(depth=0.42, alpha=0.3, colour=H1, flags=RIG)]
    expect("fringe in front of the dense layer", _composite(g, 0.5, face, fr), 0.3 * H1 + 0.7 * (0.9 * H2 + 0.1 * face[1]))
    fb = [dict(depth=0.40, alpha=0.3, colour=H1, flags=RIG), dict(depth=0.42, alpha=0.9, colour=H2, flags=RIG)]
    expect("fringe behind the dense layer", _composite(g, 0.5, face, fb), 0.9 * H2 + 0.1 * face[1])
    # world glass behind the ghost keeps the main depth: it still shows through the ghost's 10%
    glass = dict(depth=0.20, alpha=0.5, colour=0.0, flags=0)
    expect("world glass behind the ghost", _composite(g, 0.5, world, [glass] + ghost[:1]),
           0.9 * 0.2 + 0.1 * (0.5 * 0.0 + 0.5 * world[1]))
    # world blend layers never write the avatar depth: two 0.9 world layers still blend over each other
    w2 = [dict(depth=0.40, alpha=0.9, colour=H2, flags=0), dict(depth=0.45, alpha=0.9, colour=H1, flags=0)]
    expect("world layers unchanged", _composite(g, 0.5, world, w2), 0.9 * H1 + 0.1 * (0.9 * H2 + 0.1 * world[1]))
    # a world flexi chain (rigged + bit 19) and a HUD face are no avatar faces
    fx = [dict(depth=0.40, alpha=0.9, colour=H2, flags=RIG | FLEXI), dict(depth=0.45, alpha=0.9, colour=H1, flags=RIG | FLEXI)]
    expect("world flexi unchanged", _composite(g, 0.5, world, fx), 0.9 * H1 + 0.1 * (0.9 * H2 + 0.1 * world[1]))
    if g["avatarDepthFace"](RIG | HUD) or g["avatarDepthFace"](ATTACH | HUD):
        bad.append("a HUD face counts as an avatar face")
    # a single-sided back face writes nothing (the lit FS discards it)
    bk = [dict(depth=0.45, alpha=0.9, colour=H1, flags=RIG, front=False), dict(depth=0.40, alpha=0.9, colour=H2, flags=RIG)]
    expect("single-sided back face", _composite(g, 0.5, face, bk), 0.9 * H2 + 0.1 * face[1])
    # Highlight Transparent: nothing is dense, every layer shows
    expect("Highlight Transparent", _composite(g, 0.5, face, two, ht=True), 0.9 * H1 + 0.1 * (0.9 * H2 + 0.1 * face[1]))
    # a texel the lit FS's cutoff discards (0.55 < 0.6) is no dense surface at T = 0.5
    cu = [dict(depth=0.40, alpha=0.9, colour=H2, flags=RIG), dict(depth=0.45, alpha=0.55, colour=H1, flags=RIG, cutoff=0.6)]
    expect("cutoff-discarded texel", _composite(g, 0.5, face, cu), 0.9 * H2 + 0.1 * face[1])
    # below T: blends as today (a 0.4 layer is a fringe at T = 0.5)
    lo = [dict(depth=0.40, alpha=0.4, colour=H2, flags=RIG), dict(depth=0.45, alpha=0.4, colour=H1, flags=RIG)]
    expect("two layers below T", _composite(g, 0.5, face, lo), 0.4 * H1 + 0.6 * (0.4 * H2 + 0.6 * face[1]))
    # the threshold is the setting: at T = 0.95 the 0.9 layers are fringes again
    expect("T above the layers' alpha", _composite(g, 0.95, face, two), 0.9 * H1 + 0.1 * (0.9 * H2 + 0.1 * face[1]))
    # the opaque face in FRONT of hair: the hair stays hidden (main depth, as today)
    expect("opaque face in front", _composite(g, 0.5, (0.50, 0.20), [dict(depth=0.40, alpha=0.9, colour=H1, flags=RIG)]), 0.20)
    return bad


def _wiring(ctx):
    missing = []

    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()
    r = read("indra/newview/vkgfx/llcleanrenderervk.cpp")
    h = read("indra/newview/vkgfx/llcleanrenderervk.h")
    cs = read("indra/newview/vkgfx/llcleanclusterselectvk.cpp")
    st = read("indra/newview/app_settings/settings.xml")
    view = read("indra/vkclean/shaders/clean_draw_view.glsl")
    pre = read("indra/vkclean/shaders/clean_prepass_blend.frag")
    fs = read("indra/vkclean/shaders/clean_draw.frag")
    kv = read("indra/vkclean/shaders/clean_alpha_kvis.frag")
    vs = read("indra/vkclean/shaders/clean_draw.vert")
    rvs = read("indra/vkclean/shaders/clean_draw_rigged.vert")
    task = read("indra/vkclean/shaders/clean_cluster_draw.task")
    rule = read("indra/vkclean/shaders/clean_blend_depth_rule.glsl")
    m = re.search(r"<key>RenderVKAvatarAlphaDepthThreshold</key>\s*<map>(.*?)</map>", st, re.S)
    if not m or "<string>F32</string>" not in m.group(1) or "<real>0.5</real>" not in m.group(1) \
            or not re.search(r"<key>Persist</key>\s*<integer>1</integer>", m.group(1)):
        missing.append("settings.xml: RenderVKAvatarAlphaDepthThreshold (F32, 0.5, persisted) missing")
    if not re.search(r'LLCachedControl<F32> s_avt\(gSavedSettings, "RenderVKAvatarAlphaDepthThreshold", 0\.5f\)', r):
        missing.append("llcleanrenderervk.cpp: the setting is not read per frame (Live)")
    hm = re.search(r"kPushAvDepthPre = (0x[0-9a-fA-F]+)u", h)
    gm = re.search(r"const uint AVD_PREPASS_PUSH = (0x[0-9a-fA-F]+)u", view)
    if not hm or not gm or int(hm.group(1), 16) != int(gm.group(1), 16):
        missing.append("the avatar pre-pass push bit differs between the host (kPushAvDepthPre) and clean_draw_view.glsl")
    elif int(hm.group(1), 16) & 0x3fffffff:
        missing.append("the avatar pre-pass push bit is inside the cluster task stage's view-index mask")
    if "g_draw_push.pad0 & 0x3fffffffu" not in task:
        missing.append("clean_cluster_draw.task: the view index does not mask pad0's high flags (bit 30 = the avatar pre-pass)")
    if "avatarDepthThreshold() : kBlendDepthAlpha" not in pre or "AVD_PREPASS_PUSH" not in pre:
        missing.append("clean_prepass_blend.frag: the avatar pre-pass does not test THE rule at the avatar threshold")
    if "AVD_PREPASS_PUSH" not in vs or "AVD_PREPASS_PUSH" not in rvs:
        missing.append("the world / rigged VS do not collapse non-avatar instances in the avatar pre-pass")
    if "ssr.history_time.y" not in rule or not re.search(r"si\.history_time\[1\] = mAvDepthNow \? mAvDepthT : -1\.f", r):
        missing.append("the frame word (history_time.y = T or -1) is not written / read")
    if not re.search(r"db0\[69\]\.binding=69; db0\[69\]\.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER", r) \
            or not re.search(r"dl0\.bindingCount=(7\d|[89]\d)\b", r) or "wd.dstBinding=69" not in r:
        missing.append("draw set 0 binding 69 (the avatar depth) is not in the layout / never written")
    for name, src in (("clean_draw.frag", fs), ("clean_alpha_kvis.frag", kv)):
        if not re.search(r"layout\(set = 0, binding = 69\) uniform sampler2DArray uAvatarSceneDepth", src) \
                or "avatarDepthLive() && avatarDepthFace(v_flags)" not in src:
            missing.append("%s: an avatar face does not test the avatar depth (binding 69)" % name)
    if "vkCmdCopyImage(cmd, mDepth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,\n                       mAvDepth.image" not in r:
        missing.append("the avatar depth is not a copy of pass 1's final depth (opaque world + the avatar's own body)")
    if "da.imageView=mainAtt(mAvDepth)" not in r or "clusterDrawAlpha(cmd, CLPASS_ALPHA_DEPTH, 0u, drawPush, 0u, true)" not in r:
        missing.append("the avatar pre-pass does not draw into the avatar depth (static + rigged + cluster lists)")
    if "mainAtt(avatarDepth ? mAvDepth : mDepth)" not in r or "if (avScope) avSwitch(true);" not in r \
            or "if (avScope) avSwitch(false);" not in r:
        missing.append("pass 2's avatar blend group does not draw in its own scope over the avatar depth")
    if "beginAccum(cmd, mAvDepthNow ? mAvDepth.view : mDepth.view)" not in r:
        missing.append("the OIT accumulation (rigged alpha) does not test the avatar depth")
    if "beginCover(cmd, side, mAvDepthNow ? mAvDepth.view : mDepth.view)" not in r:
        missing.append("the near-opaque covers do not test the avatar depth (a rejected avatar layer would cull a visible one)")
    if "in.pass == CLPASS_ALPHA || in.pass == CLPASS_ALPHA_DEPTH" not in cs:
        missing.append("llcleanclusterselectvk.cpp: the cluster alpha-depth draws do not carry the avatar pre-pass push word")
    if "VkDeviceContext::SoftAllocScope soft(mCtx);   // out of memory: today's path, not a crash" not in r:
        missing.append("the avatar depth allocation is not soft (allocation failure must never crash)")
    # never into the main depth: the pre-pass scope's depth is the avatar target only
    i = r.find("// ---- AVATAR ALPHA DEPTH (RenderVKAvatarAlphaDepthThreshold; header kPushAvDepthPre) ----")
    j = r.find("// ---- barrier between passes ----", i)
    if i < 0 or j < 0 or "mainAtt(mDepth)" in r[i:j]:
        missing.append("the avatar pre-pass block is missing or binds the MAIN depth as its attachment")
    return missing


def run(ctx, result):
    missing = _wiring(ctx)
    g = _rule(ctx, missing)
    bad = _fixture(g) if g else []
    if bad or missing:
        result.fail("\n".join(bad + missing))
        return
    result.summary = "THE rule at T: no self-blending, opaque face / world glass still behind, T > 1 = today"
