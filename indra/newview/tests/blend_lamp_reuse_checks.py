#!/usr/bin/env python3
"""Blended layers lying on a surface take that surface's shadowed lamp light.

Source checks for the wiring (the shader logic itself is GLSL; --compile builds every
variant that carries it with glslc, as the build does):
  - the shadow walk ignores an alpha-blend caster lying on its receiver (RT_LAYER_SKIP, before
    every rule that could block or tint), so a surface's lighting holds no shadow of the layer
    drawn over it;
  - a nothing-casts frame (#823, no SHADOW pass) still writes the reuse's depth + normal record (BASE /
    COMBINE, DEFER_FLAG_VIS_FILL: its bit against every other flag and field of the split's push word);
  - in radiance mode BASE / COMBINE record the surface's demodulated shadowed lamp light (pass 15's image,
    set 3 binding 29 = draw set 0 binding 65), and a matched layer (VISR_RAD) skips exactly the lights the
    deferred walk skips -- the forward hooks mirror deferRsStoch / deferLightTaken / deferPdnRadOn /
    deferPdnVisLight -- and adds the record, recoloured by its own colours.
  - the rigged alpha-blend draws push the same visibility word in every #1410 route (ordered, the WBOIT
    accumulation, the per-pixel lit resolve, which runs the strict match on the resolved surface).

Usage:
  python3 <source>/indra/newview/tests/blend_lamp_reuse_checks.py --source-root <source> [--compile --out <dir>]
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


def check_layer_skip(sh: pathlib.Path) -> None:
    # A blend caster within RT_LAYER_SKIP along the ray is a layer lying on the receiver: it casts
    # nothing there. The skip is the first thing after the ONE alpha test (which says `blend`), so no
    # foliage / opaque-set / fixture / solid / translucent rule can block or tint the ray with it.
    walk = (sh / "clean_vis_walk.glsl").read_text()
    check("#define RT_LAYER_SKIP 0.005" in walk, "the layer distance (5 mm along the ray)")
    cand = body(walk, "bool rtWalkCand(")
    alpha = cand.find("rtCandidateAlphaCol(")
    skip = cand.find("if (blend && t < RT_LAYER_SKIP) return false;")
    check(alpha >= 0 and skip > alpha, "the candidate test must skip a blend caster lying on the receiver, after the alpha test")
    check("return" not in cand[alpha:skip], "nothing may decide the candidate between the alpha test and the layer skip")
    for rule in ("rtWalkFol(kind, ks)", "rtWalkVisK(kind, ks)", "rtFixtureBox(id, q, false", "rt_blend_parity && a >= ALPHA_BLEND_SOLID",
                 "translucentTransmit(col, a)", "return a >= cutoff;"):
        at = cand.find(rule)
        check(at > skip, f"the layer skip must come before {rule}")


def flag_bits(expr: str) -> int:
    m = re.fullmatch(r"\(1u << (\d+)\)", expr.strip())
    if m:
        return 1 << int(m.group(1))
    m = re.fullmatch(r"(\d+)u", expr.strip())
    check(m is not None, f"unparsed flag value: {expr}")
    return int(m.group(1))


def check_vis_fill(sh: pathlib.Path, renderer: str, header: str) -> None:
    # A nothing-casts frame (#823: no SHADOW pass) still has the visibility reuse's depth + normal record:
    # BASE / COMBINE write it (sun-less), so a blended layer can find the surface it lies on.
    kernel = (sh / "clean_deferred_light.comp").read_text()
    flags = dict(re.findall(r"#define (DEFER_FLAG_\w+)\s+(\([^)]*\)|\d+u)", kernel))
    check("DEFER_FLAG_VIS_FILL" in flags, "the fill flag must exist in the kernel")
    fill = flag_bits(flags["DEFER_FLAG_VIS_FILL"])
    m = re.search(r"static constexpr uint32_t kDeferFlagVisFill = 1u << (\d+);", header)
    check(m is not None and (1 << int(m.group(1))) == fill, "the host's kDeferFlagVisFill must equal DEFER_FLAG_VIS_FILL")
    # Its bit: no other flag's, except the uber kernel's RT_IN (never set on a split dispatch, read only by
    # pass 0 -- the fill is read only by passes 3 / 7); clear of the push word's fields (the sharp-roughness
    # byte 8-15, the denoiser debug view 21-23, the RT roughness byte 24-31).
    for name, expr in flags.items():
        if name in ("DEFER_FLAG_VIS_FILL", "DEFER_FLAG_RT_IN"):
            continue
        check(flag_bits(expr) & fill == 0, f"DEFER_FLAG_VIS_FILL shares a bit with {name}")
    dbg = re.search(r"#define DEFER_PSTOCH_DBG_SHIFT (\d+)u", kernel)
    check(dbg is not None, "the denoiser debug field's shift")
    fields = 0xFF00 | (7 << int(dbg.group(1))) | 0xFF000000
    check(fill & fields == 0, "DEFER_FLAG_VIS_FILL must be clear of the push word's byte / debug fields")
    base = between(kernel, "#elif CLEAN_DEFER_PASS == 7\n// ---- BASE", "#elif CLEAN_DEFER_PASS == 8")
    comb = kernel[kernel.find("#else   // CLEAN_DEFER_PASS 0 (uber) / 3 (combine)"):]
    if flag_bits(flags["DEFER_FLAG_RT_IN"]) == fill:
        uber = between(kernel, "#if CLEAN_DEFER_PASS == 0\nlayout(set = 2, binding = 11, rgba16f) uniform readonly image2D uRtUber;",
                       "#elif CLEAN_DEFER_PASS == 12")
        reads = re.findall(r"g_draw_push\.flags & DEFER_FLAG_RT_IN", kernel)
        check(len(reads) >= 1 and len(reads) == len(re.findall(r"g_draw_push\.flags & DEFER_FLAG_RT_IN", uber)),
              "DEFER_FLAG_RT_IN must be read by the uber kernel only")
        fill_reads = r"g_draw_push\.flags & \(?[A-Z_ |]*DEFER_FLAG_VIS_FILL"
        allowed = [base, comb]
        if "vec3 deferEmitterLight(vec3 P, vec3 N, vec3 diffuseColor, vec3 specularColor)\n{" in kernel:
            allowed.append(deferred_emitter(kernel))   # (passes 3 / 7 only: CLEAN_EMITTER_LIGHT)
        check(len(re.findall(fill_reads, kernel)) == sum(len(re.findall(fill_reads, s)) for s in allowed),
              "DEFER_FLAG_VIS_FILL must be read by BASE / COMBINE only")
        rdl = body(renderer, "void LLCleanRendererVK::recordDeferredLighting(")
        rt_in = rdl.find("p.flags |= (1u << 17);")
        check(rt_in > rdl.rfind("p.flags = mDeferSplitFlags"), "the host sets DEFER_FLAG_RT_IN on the uber dispatch only")
        for m in re.finditer(r"mDeferSplitFlags\s*\|?=\s*([^;]*);", renderer):
            check("<< 17" not in m.group(1), f"no split flag may use bit 17 but the fill: {m.group(0)}")
    # One record writer; the SHADOW pass, BASE and COMBINE (never the uber kernel) call it.
    check("#if DEFER_SHADOW || CLEAN_DEFER_PASS == 3 || CLEAN_DEFER_PASS == 7\n" in kernel, "BASE / COMBINE must see the record image")
    store = body(kernel, "void visDepthStore(ivec2 px, const SurfaceLit s, vec3 P, bool sunWant)")
    check("sunWant ? vz : -vz" in store and "imageStore(uVisDepth, px, uvec4(d));" in store and "SURF_FULLBRIGHT" in store,
          "visDepthStore: the depth signed by the sun's evaluation, 0 for fullbright")
    check("if (visOut) visDepthStore(px, s, P, sunWant);" in kernel, "the SHADOW pass writes through visDepthStore")
    check("(g_draw_push.flags & DEFER_FLAG_VIS_FILL) != 0u" in base and "if (visFill) imageStore(uVisDepth, px, uvec4(0u));" in base
          and "if (visFill) visDepthStore(px, s, P, false);" in base, "BASE must fill the record (0 off a surface, sun-less on one)")
    check(re.search(r"#if CLEAN_DEFER_PASS == 3\s*\n\s*const bool visFill = \(g_draw_push.flags & DEFER_FLAG_VIS_FILL\) != 0u;", comb)
          and re.search(r"#if CLEAN_DEFER_PASS == 3\s*\n\s*if \(visFill\) imageStore\(uVisDepth, px, uvec4\(0u\)\);", comb)
          and re.search(r"g_ds_P = P;\s*\n\s*if \(visFill\) visDepthStore\(px, s, P, false\);\s*\n#endif", comb),
          "COMBINE (pass 3 only, never the uber kernel) must fill the record")
    skip = between(renderer, "mDeferShadowSkip = true;", "if (mShadowMergeTry && !mShadowMergeNow)")
    check("mDeferSplitFlags &= ~8u;" in skip and "if (mDsVisReuse) { mDeferSplitFlags |= kDeferFlagVisFill; mVisReuseFlags = 1u; }" in skip,
          "the nothing-casts decision must keep VISR_ON and ask BASE / COMBINE for the record")


def deferred_emitter(kernel: str) -> str:
    return body(kernel, "vec3 deferEmitterLight(vec3 P, vec3 N, vec3 diffuseColor, vec3 specularColor)\n{")


def check_lamp_record(sh: pathlib.Path, renderer: str, header: str) -> None:
    # BASE / COMBINE record the lit surface's demodulated shadowed lamp light (RADIANCE MODE: pass 15's image);
    # a blended layer on the strict match skips exactly the lights the deferred walk skips and adds the
    # record, recoloured by its own colours.
    kernel = (sh / "clean_deferred_light.comp").read_text()
    common = (sh / "clean_draw_common.glsl").read_text()
    draw = (sh / "clean_draw.frag").read_text()
    pack = (sh / "clean_probe_pack.glsl").read_text()

    # The push bit: its own, beside VISR_ON / _PROJ / _PROJ_RGB and the near-opaque bit 31; host and shader agree.
    check("const uint VISR_ON = 1u, VISR_PROJ = 2u, VISR_PROJ_RGB = 4u;" in common and "const uint VISR_RAD = 8u;" in common,
          "VISR_RAD = 8u, clear of the other VISR_* bits")
    check("static constexpr uint32_t kPushVisReuseRad = 8u;" in header and "kPushBlendGbuf = 0x80000000u;" in header,
          "the host's kPushVisReuseRad must equal VISR_RAD (and stay clear of kPushBlendGbuf)")
    word = between(renderer, "const uint32_t visrWord = ", ";")
    check(re.search(r"\(mVisReuseFlags != 0u && mPdnRadiance && \(mDeferSplitFlags & \(1u << 18\)\) != 0u\)\s*\?\s*kPushVisReuseRad\s*:\s*0u", word)
          and word.startswith("const uint32_t visrWord = mVisReuseFlags\n"),
          "the visibility word carries VISR_RAD only on a reuse frame whose BASE / COMBINE read pass 15's radiance (DEFER_FLAG_PSTOCH)")
    alpha_set = body(renderer, "auto drawAlphaSet = [&](")
    static_push = between(alpha_set, "pushMainView(cmd, mDrawLayout, drawPush);", "drawListBlend(mAlphaPipe, stat, stat_ctr);")
    check("const uint32_t padStatic = visrWord |" in static_push and "kPushBlendGbuf" in static_push,
          "pass 2's word carries VISR_RAD only on a reuse frame whose BASE / COMBINE read pass 15's radiance (DEFER_FLAG_PSTOCH)")

    # The record: set 3 binding 29 (kDsBindingLamp) = draw set 0 binding 65, RG32UI (two RGB9E5 words).
    check("layout(set = 3, binding = 29, rg32ui) uniform writeonly uimage2D uLampOut;" in kernel, "the record's set-3 binding (29)")
    check("static constexpr uint32_t kDsBindingLamp = kDsImgBindings + 22u;" in header
          and 'static_assert(kDsBindingLamp == 29u' in header and "static constexpr uint32_t kDsBindings = kDsImgBindings + 23u;" in header,
          "kDsBindingLamp = 29 and the set grows by one")
    check("layout(set = 0, binding = 65, rg32ui) uniform readonly uimage2D uVisReuseLamp;" in common, "the record's draw set 0 binding (65)")
    check("VkDescriptorSetLayoutBinding db0[67]{};" in renderer and "VkDescriptorBindingFlags db0f[67] = {};" in renderer
          and "db0fi.bindingCount=67;" in renderer and "dl0.bindingCount=67;" in renderer, "draw set 0 grows to 67 bindings")
    check("db0[65].binding=65; db0[65].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;" in renderer
          and "db0[66].binding=66; db0[66].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;" in renderer
          and "db0f[65] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;" in renderer
          and "db0f[66] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;" in renderer,
          "draw set 0 bindings 65 (the record) / 66 (the light grid), partially bound")
    check("db0[65].descriptorCount=1; db0[65].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;" in renderer
          and "db0[66].descriptorCount=1; db0[66].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;" in renderer,
          "bindings 65 / 66: one descriptor each, the fragment stage")
    check("constexpr uint32_t kBindlessReserve = 67;" in renderer, "the bindless table's reserve counts the two new descriptors")
    # The light grid on draw set 0 binding 66, read-only, the deferred set's buffer (or its placeholder).
    regir_inc = common.find('#include "clean_regir.glsl"')
    check(regir_inc > 0, "the forward reuse needs the light grid (clean_regir.glsl)")
    pre = common[:regir_inc]
    for d in ("#define REGIR_SET 0", "#define REGIR_BINDING 66", "#define REGIR_ACCESS readonly", "#define RS_SHADOW_PARAMS(i) pshadow[i].params"):
        check(d in pre[pre.rfind("#ifdef CLEAN_VIS_REUSE"):], f"{d} before the forward include")
    regir = body(renderer, "void LLCleanRendererVK::writeRegirDescriptors()")
    check("dstSet = mDsetDraw0; w[n].dstBinding = 66;" in regir and "pBufferInfo = &dbi;" in regir,
          "writeRegirDescriptors must point draw set 0 binding 66 at the grid (or the placeholder)")

    # The write: BASE / COMBINE's radiance branch, on every frame the depth record is written, the very values
    # it lights the surface with.
    em = deferred_emitter(kernel)
    rad = em[em.find("if (deferPdnRadOn())"):em.find("if (!deferPstochOn()")]
    check("radD = pdnSane(radD); radS = pdnSane(radS);" in rad
          and "if ((g_draw_push.flags & (DEFER_FLAG_VIS_REUSE | DEFER_FLAG_VIS_FILL)) != 0u)" in rad
          and "imageStore(uLampOut, ivec2(CLEAN_FRAG_XY), uvec4(packRGB9E5(radD), packRGB9E5(radS), 0u, 0u));" in rad
          and "return radD * pdnDiffFactor(diffuseColor) + radS * pdnSpecFactor(specularColor);" in rad,
          "deferEmitterLight's radiance branch must store the sane'd radiance it returns, under VIS_REUSE | VIS_FILL")
    check(rad.find("imageStore(uLampOut") < rad.find("return radD"), "the store comes before the return")
    check("vec3 unpackRGB9E5(uint v)" in pack and "vec3 dsUnpackRGB9E5(uint v) { return unpackRGB9E5(v); }" in kernel,
          "one RGB9E5 decoder (clean_probe_pack.glsl), shared by the kernel and the forward FS")

    # The forward hooks: defined in the reuse block, before localLight, and they mirror the deferred ones.
    block = common[common.find("#ifdef CLEAN_VIS_REUSE"):common.find("vec3 localLight(uint li, vec3 P, vec3 N, vec3 V")]
    check("#define CLEAN_PROJ_STOCH_LIT" in block, "the forward reuse compiles localLight's stochastic hooks")
    match = body(common, "void visReuseMatch(vec3 P, vec3 Ng)")
    check("g_visr_rad = false;" in match and match.find("g_visr_rad = false;") < match.find("return;"), "the match clears VISR_RAD first")
    rad_set = match.find("g_visr_rad = (flags & VISR_RAD) != 0u && (linfo.fwd.z & 2u) != 0u;")
    check(rad_set > match.find("if (dot(normalize(n), Ng) < 0.9) return;"),
          "VISR_RAD counts on the strict match only, in radiance mode (deferPdnRadOn's frame bit)")
    check(re.search(r"bool deferPdnRadOn\(\) \{ return deferPstochOn\(\) && \(linfo\.fwd\.z & 2u\) != 0u; \}", kernel),
          "the deferred radiance test this mirrors")
    on = body(common, "bool visReuseRadOn()")
    check("return g_visr_rad;" in on and "#define CLEAN_PROJ_STOCH_ON visReuseRadOn" in block, "CLEAN_PROJ_STOCH_ON: the matched radiance-mode fragment")
    check("#define CLEAN_PROJ_STOCH_RAD visReuseRad" in block and "return true;" in body(common, "bool visReuseRad()"),
          "CLEAN_PROJ_STOCH_RAD: always (VISR_RAD is radiance mode's)")
    vis = body(common, "void visReuseVisLight(uint li, vec3 P, vec3 N, out vec3 visD, out vec3 visS)")
    check("visD = visS = vec3(0.0);" in vis and "#define CLEAN_PROJ_STOCH_VIS visReuseVisLight" in block,
          "CLEAN_PROJ_STOCH_VIS: 0, as deferPdnVisLight in radiance mode")
    dvis = body(kernel, "void deferPdnVisLight(uint li, vec3 P, vec3 N, out vec3 visD, out vec3 visS)\n{")
    check("if (deferPdnRadOn()) { visD = visS = vec3(0.0); return; }" in dvis, "the deferred visibility hook this mirrors")
    # deferRsStoch's three conditions, verbatim (its per-pixel grid flag renamed).
    drs = body(kernel, "bool deferRsStoch(uint li, vec3 P)\n{")
    frs = body(common, "bool visReuseRsStoch(uint li, vec3 P)")
    for line in ("if (!rsFrameOn()) return false;", "if (!rsInSet(li) || rsRecordLight(li)) return false;",
                 "if (g_rs_in < 0) g_rs_in = regirCell(P) != REGIR_NO_CELL ? 1 : 0;", "return g_rs_in == 1;"):
        check(line in drs, f"deferRsStoch changed (update the forward mirror): {line}")
        check(line.replace("g_rs_in", "g_visr_rs_in") in frs, f"visReuseRsStoch must mirror deferRsStoch: {line}")
    check("#define CLEAN_RS_STOCH visReuseRsStoch" in block and "int g_visr_rs_in = -1;" in block, "CLEAN_RS_STOCH: the forward mirror")
    check("bool deferLightTaken(uint li) { return rsFrameOn() && deferPstochOn() && emTaken(li); }" in kernel
          and "bool visReuseLightTaken(uint li) { return g_visr_rad && rsFrameOn() && emTaken(li); }" in common
          and "#define CLEAN_LIGHT_TAKEN visReuseLightTaken" in block, "CLEAN_LIGHT_TAKEN: the forward mirror of deferLightTaken")
    lamp = body(common, "vec3 visReuseLamp(vec3 P, vec3 N, vec3 diffuseColor, vec3 specularColor)")
    check("if (!g_visr_rad || litDbg(LDBG_NO_LOCAL)) return vec3(0.0);" in lamp and "imageLoad(uVisReuseLamp, ivec2(CLEAN_FRAG_XY))" in lamp
          and "unpackRGB9E5(rec.x) * pdnDiffFactor(diffuseColor) + unpackRGB9E5(rec.y) * pdnSpecFactor(specularColor)" in lamp
          and "#define CLEAN_EMITTER_LIGHT visReuseLamp" in block,
          "CLEAN_EMITTER_LIGHT: the record remodulated by the fragment's own colours, as deferEmitterLight does")
    check(draw.find('#include "clean_draw_common.glsl"') < draw.find('#include "clean_surface_light.glsl"'),
          "the hook must be defined before lightSurface's CLEAN_EMITTER_LIGHT call site")

    # Host: the image (render extent with the reuse, else 1 x 1; dedicated), its writes, barriers, lifetime.
    check("mDsLamp = mCtx->createImage2D(visReuse ? rw : 1u, visReuse ? rh : 1u, VK_FORMAT_R32G32_UINT, su, VK_IMAGE_ASPECT_COLOR_BIT);" in renderer
          and "if (mDsLamp.image) { mCtx->destroyImage(mDsLamp); mDsLamp = GpuImage{}; }" in renderer
          and renderer.count("|| !mDsGain.image || !mDsLamp.image || !mDsTileList.buffer;") == 2
          and "vkUpdateDescriptorSets(dev, 1, &wr[kDsBindingLamp], 0, nullptr);" in renderer
          and "const uint32_t src[4] = {0u, 1u, 6u, kDsBindingLamp};" in renderer and "const uint32_t dst[4] = {41u, 42u, 43u, 65u};" in renderer
          and "k == DS_ALIASED + 2u ? mDsLamp.image" in renderer and "k == DS_ALIASED + 1u ? mDsLamp.image" in renderer,
          "the record image's lifecycle, descriptor writes and per-frame barriers")


def check_rigged_reuse(sh: pathlib.Path, renderer: str) -> None:
    # The rigged alpha-blend draws take the same visibility word as the static ones in every route
    # #1410 gives them: the ordered pass (RenderVKAlphaOIT 0: LIT_RIG_ALPHA runs the static blend FS),
    # the WBOIT accumulation (mode 1 lights per fragment there) and the per-pixel lit resolve (mode 2,
    # the default), which runs the static FS's strict match on the resolved surface.
    alpha_set = body(renderer, "auto drawAlphaSet = [&](")
    ordered = between(alpha_set, "else if (do_rigged)", "drawListBlend(mRiggedAlphaPipe, rig, rig_ctr);")
    check("const uint32_t padRig = visrWord | alphaKPad(post ? 3u : 1u, mRiggedAlphaPipe)" in ordered,
          "the ordered rigged draws push the visibility word with the cover-list word")
    resolve = between(alpha_set, "if (do_rigged && mOitMode)", "mAlphaOIT->recordResolve(")
    check("if (visrWord) pushPad0(visrWord);" in resolve, "the WBOIT resolve gets the visibility word (VISR_RAD included)")
    accum = body(renderer, "auto oitAccumSide = [&](bool post)\n    {")
    check("const uint32_t padOit = visrWord | (mBlendGbufNow ? kPushBlendGbuf : 0u);" in accum,
          "the WBOIT accumulation pushes the visibility word for the rigged draws too (mode 1 lights per fragment)")
    res = (sh / "clean_oit_resolve.frag").read_text()
    check(res.find("#define CLEAN_VIS_REUSE") < res.find('#include "clean_draw_common.glsl"'), "the resolve compiles the reuse block")
    mode2 = res[res.find("// Mode 2: the weighted mean surface, lit once."):]
    m = mode2.find("visReuseMatch(P, N);")
    check(m > mode2.find("g_oit_Ng = N;") and 0 < m < mode2.find("vec3 c = lightSurface(s, P, 1.0).rgb;"),
          "the resolve runs the strict match on the resolved surface before lighting it")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source-root", required=True)
    ap.add_argument("--compile", action="store_true")
    ap.add_argument("--out")
    args = ap.parse_args()
    root = pathlib.Path(args.source_root)
    sh = root / "indra/vkclean/shaders"
    renderer = (root / "indra/newview/vkgfx/llcleanrenderervk.cpp").read_text()
    header = (root / "indra/newview/vkgfx/llcleanrenderervk.h").read_text()

    check_layer_skip(sh)
    check_vis_fill(sh, renderer, header)
    check_lamp_record(sh, renderer, header)
    check_rigged_reuse(sh, renderer)

    print("blend lamp reuse wiring: PASS")

    if args.compile:
        out = pathlib.Path(args.out or "blend_lamp_reuse_spv")
        out.mkdir(parents=True, exist_ok=True)
        variants = [
            # every walk the layer skip sits in: the forward lit FS (its own rays), the RT shadow mask,
            # pass 15's shadow cores (the lamps' shadowed radiance)
            ("clean_draw.frag", []),
            ("clean_draw.frag", ["-DCLEAN_BLEND_TOL"]),
            ("clean_draw.frag", ["-DCLEAN_BLEND_TOL", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=2"]),
            ("clean_deferred_light.comp", ["-DCLEAN_DEFER_PASS=15", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=4"]),
            ("clean_deferred_light.comp", ["-DCLEAN_DEFER_PASS=15", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=4", "-DCLEAN_SHADOW_CORE"]),
            ("clean_rt_shadow_mask.comp", ["-DRT_SET=2", "-DCLEAN_PROJ_SHADOW_RAYQUERY"]),
            # the visibility reuse record's writers: SHADOW (1, + the fused classify 10), COMBINE (3, and the
            # uber kernel 0 that shares its body), BASE (7)
            ("clean_deferred_light.comp", []),
            ("clean_deferred_light.comp", ["-DCLEAN_DEFER_PASS=1"]),
            ("clean_deferred_light.comp", ["-DCLEAN_DEFER_PASS=10"]),
            ("clean_deferred_light.comp", ["-DCLEAN_DEFER_PASS=3"]),
            ("clean_deferred_light.comp", ["-DCLEAN_DEFER_PASS=7"]),
            # the lamp record's walk hooks: LOCAL (8) beside COMBINE / BASE; the static flavour's rez-fx wire twin
            ("clean_deferred_light.comp", ["-DCLEAN_DEFER_PASS=8"]),
            ("clean_draw.frag", ["-DREZFX_WIRE", "-DCLEAN_BLEND_TOL"]),
            # every other FS that compiles the reuse block (CLEAN_VIS_REUSE): the #1411 basic blend
            # placeholder, the #1410 WBOIT accumulation FSs and the per-pixel lit resolve
            ("clean_draw.frag", ["-DCLEAN_BLEND_TOL", "-DCLEAN_BASIC"]),
            ("clean_draw.frag", ["-DCLEAN_BLEND_TOL", "-DCLEAN_OIT_ACCUM"]),
            ("clean_draw.frag", ["-DCLEAN_BLEND_TOL", "-DCLEAN_OIT_ACCUM", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=2"]),
            ("clean_draw.frag", ["-DCLEAN_BLEND_TOL", "-DCLEAN_OIT_GBUF"]),
            ("clean_oit_resolve.frag", ["-DOIT_LIT"]),
            ("clean_oit_resolve.frag", ["-DOIT_LIT", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=3"]),
        ]
        for i, (src, defs) in enumerate(variants):
            stage = "comp" if src.endswith(".comp") else "frag"
            cmd = ["glslc", "--target-env=vulkan1.3", "-O", f"-fshader-stage={stage}", "-I", str(sh),
                   *defs, f"-DCLEAN_STAGE_{stage.upper()}", str(sh / src), "-o", str(out / f"v{i}.spv")]   # (the regen's stage define)
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
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
