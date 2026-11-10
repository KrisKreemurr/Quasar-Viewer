#!/usr/bin/env python3
"""Near-opaque alpha-blend texels are lit as opaque surfaces (RenderVKBlendOpaqueGBuffer).

Source checks for the wiring (the shader logic itself is GLSL; --compile builds every
variant that carries it with glslc, as the build does):
  - ONE rule (clean_blend_depth_rule.glsl blendDepthKeeps) decides "near-opaque" for the
    alpha-blend depth pre-pass, the G-buffer's blend draws and pass 2's skip; every FS
    that asks includes it, none keeps a copy of the 0.99 threshold;
  - the three attachment variants of the pre-pass FS (depth + normal, + motion, motion
    only) carry a PREPASS_BLEND flavour, and the regen builds them + the G-buffer blend FS;
  - pass 2 skips a near-opaque blend fragment only when told by the push word (the frames
    whose G-buffer took it), with a margin over the keep threshold (no holes if the two
    passes round the alpha product differently), never in the capture / G-buffer builds;
  - the renderer draws the alpha lists in the depth pre-pass (the variant in force), the
    RT terminator pass and the G-buffer scope, pushes the bit for pass 2's static and rigged
    draws, and the late alpha-depth pass stands down for them, all behind one decision.

Usage:
  python3 <source>/indra/newview/tests/blend_opaque_gbuffer_checks.py --source-root <source> [--compile --out <dir>]
"""
from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys


def body(source: str, marker: str) -> str:
    start = source.find(marker)
    if start < 0:
        raise AssertionError(f"missing: {marker}")
    brace = source.find("{", start)
    depth = 0
    for i in range(brace, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[start:i + 1]
    raise AssertionError(f"unterminated: {marker}")


def between(source: str, first: str, last: str) -> str:
    a = source.find(first)
    check(a >= 0, f"missing: {first}")
    b = source.find(last, a)
    check(b >= 0, f"missing after {first}: {last}")
    return source[a:b]


def check(cond: bool, what: str) -> None:
    if not cond:
        raise AssertionError(what)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source-root", required=True)
    ap.add_argument("--compile", action="store_true")
    ap.add_argument("--out")
    args = ap.parse_args()
    root = pathlib.Path(args.source_root)
    sh = root / "indra/vkclean/shaders"
    rule = (sh / "clean_blend_depth_rule.glsl").read_text()
    pre_blend = (sh / "clean_prepass_blend.frag").read_text()
    pre_normal = (sh / "clean_prepass_normal.frag").read_text()
    pre_motion = (sh / "clean_prepass_motion.frag").read_text()
    draw = (sh / "clean_draw.frag").read_text()
    regen = (root / "indra/vkclean/regen_world_spv.py").read_text()
    renderer = (root / "indra/newview/vkgfx/llcleanrenderervk.cpp").read_text()
    header = (root / "indra/newview/vkgfx/llcleanrenderervk.h").read_text()
    settings = (root / "indra/newview/app_settings/settings.xml").read_text()

    # ONE rule: the threshold lives in the include, nowhere else; the push bit is declared there too.
    check("const float kBlendDepthAlpha = 0.99;" in rule, "the near-opaque threshold must be kBlendDepthAlpha = 0.99")
    keeps = body(rule, "bool blendDepthKeeps(float base_a, float alpha_cutoff, uint flags, float threshold)")
    check("rezfxFadeOpacity(flags)" in keeps and "base_a * rezfxFadeOpacity(flags) < threshold" in keeps and "alphaKeeps(base_a, alpha_cutoff, 0.0, 0.0)" in keeps,
          "blendDepthKeeps must test the faded alpha against the threshold and the face's own cutoff")
    check("ssr.ao_params2.z > 0.5" in keeps and "REZFX_DYING" in keeps, "blendDepthKeeps must refuse under Highlight Transparent and for a dying object")
    check("!gl_FrontFacing && (flags & (256u | 8u)) == 0u" in keeps and "(flags & 65536u) != 0u" in keeps,
          "blendDepthKeeps must refuse a single-sided back face and a water exclusion surface (the lit FS's own discards)")
    check(re.search(r"const uint\s+BLEND_GBUF_PUSH\s+= 0x80000000u;", rule), "the pass-2 push bit must be declared beside the rule")
    for name, src in (("clean_prepass_blend.frag", pre_blend), ("clean_prepass_normal.frag", pre_normal),
                      ("clean_prepass_motion.frag", pre_motion), ("clean_draw.frag", draw)):
        check('#include "clean_blend_depth_rule.glsl"' in src, f"{name} must include the rule")
        check("0.99" not in src.replace("kBlendDepthAlpha", ""), f"{name} must not keep its own 0.99 threshold")

    # The pre-pass flavours: PREPASS_BLEND keeps exactly what blendDepthKeeps keeps at kBlendDepthAlpha (the
    # avatar alpha-depth pre-pass: at the avatar threshold, its own draws only).
    check("!blendDepthKeeps(base.a, m.alpha_cutoff, v_flags, avPre ? avatarDepthThreshold() : kBlendDepthAlpha)) discard;" in pre_blend, "the alpha-depth pre-pass FS must discard by the rule")
    for name, src in (("clean_prepass_normal.frag", pre_normal), ("clean_prepass_motion.frag", pre_motion)):
        check("#elif defined(PREPASS_BLEND)" in src and "!blendDepthKeeps(base.a, m.alpha_cutoff, v_flags, kBlendDepthAlpha)) discard;" in src,
              f"{name} needs a PREPASS_BLEND flavour that discards by the rule")
        check(re.search(r"#if defined\(PREPASS_MASK\) \|\| defined\(PREPASS_BLEND\)[\s\S]*gmdx_rezfx\.glsl", src),
              f"{name}'s PREPASS_BLEND flavour needs the rez-fx helpers (the fade)")

    # The G-buffer blend FS: the same keep, in the EARLY_Z (G-buffer) decode; pass 2's skip with the margin.
    gb_keep = between(draw, "#ifdef CLEAN_GBUFFER_BLEND", "#endif")
    check("(v_flags & 2u) != 0u && !blendDepthKeeps(base.a, m.alpha_cutoff, v_flags, kBlendDepthAlpha)" in gb_keep and "discard" in gb_keep,
          "the G-buffer blend FS must discard the texels the pre-pass did not keep")
    skip = between(draw, "#if !defined(CLEAN_GBUFFER) && !defined(CLEAN_CAPTURE)\n    // NEAR-OPAQUE", "#endif")
    check("(g_draw_push.pad0 & BLEND_GBUF_PUSH) != 0u" in skip and "(v_flags & 2u) != 0u" in skip
          and "blendDepthKeeps(base.a, m.alpha_cutoff, v_flags, kBlendSkipAlpha)" in skip and "discard" in skip,
          "pass 2 must skip a near-opaque blend fragment only on the push bit, with the margin")
    check("const float kBlendSkipMargin = 0.005;" in rule and "const float kBlendSkipAlpha  = kBlendDepthAlpha + kBlendSkipMargin;" in rule,
          "the skip margin (and the skip threshold built from it) must be declared beside the rule")

    # The regen builds the new flavours.
    for entry in ('("clean_prepass_normal.frag", "frag", "kWorldPrepassNormalBlendFragSpv", ["-DPREPASS_BLEND"])',
                  '("clean_prepass_normal.frag", "frag", "kWorldPrepassMotionBlendFragSpv", ["-DPREPASS_MOTION", "-DPREPASS_BLEND"])',
                  '("clean_prepass_motion.frag", "frag", "kWorldPrepassMotionOnlyBlendFragSpv", ["-DPREPASS_BLEND"])',
                  '("clean_draw.frag", "frag", "kWorldFragGbufBlendSpv", ["-DCLEAN_EARLY_Z", "-DCLEAN_GBUFFER", "-DCLEAN_GBUFFER_NO_TERRAIN", "-DCLEAN_GBUFFER_BLEND"])'):
        check(entry in regen, f"regen_world_spv.py must build {entry}")

    # The renderer: one decision, the pre-pass draws, the G-buffer draws, the push, the late pass.
    check("static constexpr uint32_t kPushBlendGbuf = 0x80000000u;" in header, "the push bit must be declared in the header")
    decision = between(renderer, "mBlendGbufNow = s_blendGbuf", ";")
    for term in ("deferNow", "!mSnapMatteSeq", "mGbufBlendPipe[0]", "mGbufBlendPipe[1]"):   # (#1415: the pre-pass always runs; no overdraw-atomic frames)
        check(term in decision, f"the per-frame decision must include {term}")
    pre = between(renderer, "// ---- DEPTH PRE-PASS: depth-only over the SAME opaque + mask indirect draws ----", 'gpuSubMark(cmd, "g_prepass");')
    check("preBlend = p1aMo ? mPreMoBlendPipe" in pre and "mvNow ? mPreMvBlendPipe" in pre and "aoPre ? mPreNrmBlendPipe" in pre and ": mPreBlendPipe;" in pre,
          "the pre-pass must pick the blend pipe of the attachment variant in force")
    for term in ("mCmd[BUCKET_ALPHA].buffer", "mAlphaPost[0].buffer", "mCmd[BUCKET_R_ALPHA].buffer", "mAlphaPost[1].buffer"):
        check(term in pre, f"the pre-pass must draw {term}")
    check(pre.count("if (mBlendGbufNow)") == 2, "the pre-pass blend draws (static, rigged) must be behind the decision")
    gbuf = body(renderer, "auto recordGbufferScope = [&](bool vrsAttach)")
    stat = between(gbuf, "drawG(gbDs, 7, false);", 'gpuSubMark(cmd, "g_gbuf_static");')
    rig = between(gbuf, "drawBucket(gbp[3], 9);", 'gpuSubMark(cmd, "g_gbuf_rigged");')
    check("mGbufBlendPipe[0]" in stat and "mCmd[BUCKET_ALPHA].buffer" in stat and "mAlphaPost[0].buffer" in stat, "the G-buffer scope must draw the static alpha lists")
    check("mGbufBlendPipe[1]" in rig and "mCmd[BUCKET_R_ALPHA].buffer" in rig and "mAlphaPost[1].buffer" in rig, "the G-buffer scope must draw the rigged alpha lists")
    alpha_set = body(renderer, "auto drawAlphaSet = [&](")
    static_push = between(alpha_set, "pushMainView(cmd, mDrawLayout, drawPush);", "drawListBlend(mAlphaPipe, stat, stat_ctr);")
    rigged_push = between(alpha_set, "pushMainView(cmd, mDrawLayout, rigPush);", "drawListBlend(mRiggedAlphaPipe, rig, rig_ctr);")
    check("kPushBlendGbuf" in static_push and "kPushBlendGbuf" in rigged_push, "pass 2's static and rigged draws must get the push bit")
    tri = between(renderer, "drawBucket(mTermTriPipe[0][1], 7);", 'gpuSubMark(cmd, "g_term_tri");')
    check(tri.count("if (mBlendGbufNow)") == 2 and "mCmd[BUCKET_ALPHA].buffer" in tri and "mCmd[BUCKET_R_ALPHA].buffer" in tri
          and "mTermTriPipe[0][0]" in tri and "mTermTriPipe[1][0]" in tri,
          "the RT terminator pass must cover the alpha lists' near-opaque texels (back-culled)")
    late = between(renderer, 'gpuSubMark(cmd, "g_mirror_occl");', 'gpuSubMark(cmd, "g_alpha_depth");')
    check("!mBlendGbufNow" in late, "the late alpha-depth pass must stand down for the lists the pre-pass drew")
    check("<key>RenderVKBlendOpaqueGBuffer</key>" in settings, "the setting must exist")

    print("blend opaque G-buffer wiring: PASS")

    if args.compile:
        out = pathlib.Path(args.out or "blend_opaque_gbuffer_spv")
        out.mkdir(parents=True, exist_ok=True)
        variants = [
            ("clean_prepass_blend.frag", []),
            ("clean_prepass_normal.frag", ["-DPREPASS_BLEND"]),
            ("clean_prepass_normal.frag", ["-DPREPASS_MOTION", "-DPREPASS_BLEND"]),
            ("clean_prepass_motion.frag", ["-DPREPASS_BLEND"]),
            ("clean_draw.frag", ["-DCLEAN_EARLY_Z", "-DCLEAN_GBUFFER", "-DCLEAN_GBUFFER_NO_TERRAIN", "-DCLEAN_GBUFFER_BLEND"]),
            ("clean_draw.frag", []),
            ("clean_draw.frag", ["-DCLEAN_BLEND_TOL"]),
            ("clean_draw.frag", ["-DCLEAN_BLEND_TOL", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=2"]),
            ("clean_draw.frag", ["-DCLEAN_CAPTURE"]),
        ]
        for i, (src, defs) in enumerate(variants):
            cmd = ["glslc", "--target-env=vulkan1.3", "-O", "-fshader-stage=frag", "-I", str(sh),
                   *defs, str(sh / src), "-o", str(out / f"v{i}.spv")]
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
            if r.returncode:
                sys.stderr.write(f"compile failed: {src} {' '.join(defs)}\n{r.stderr[-3000:]}\n")
                return 1
            print(f"compiled {src} {' '.join(defs) or '(plain)'}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as e:
        sys.stderr.write(f"FAIL: {e}\n")
        raise SystemExit(1)
