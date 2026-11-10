#!/usr/bin/env python3
# vqbench (#1039): compile the viewer's lighting kernels EXACTLY as the viewer build does -- the SHADERS table
# of indra/vkclean/regen_world_spv.py (its flags + -DCLEAN_STAGE_COMP, variant()) through spvbuild.compile_many
# (glslc --target-env=vulkan1.3 -O) -- one <symbol>.spv per kernel, plus an UNOPTIMISED twin (<symbol>.names.spv:
# the same flags without -O) that keeps the block / member names the bench reflects its named uniform writes from
# (-O strips names; the layouts are the source's either way). Also the bench's own helper shaders (shaders/*.comp).
#
#   vqb_spv.py --out DIR [--cache DIR]
import argparse, os, subprocess, sys, struct

HERE = os.path.dirname(os.path.abspath(__file__))
VKCLEAN = os.environ.get("VQB_VKCLEAN", os.path.normpath(os.path.join(HERE, "..", "vkclean")))
sys.path.insert(0, VKCLEAN)
import regen_world_spv as rw   # (importable: its main() runs only as a script)
import spvbuild

# The kernels the bench times (the embedded symbols the viewer's host code creates its pipelines from:
# llcleangivk.cpp giInit, llcleanvisqueuevk.cpp visQueueInit, llcleanprojstochvk.cpp).
KERNELS = [
    # GI split (RenderVKGISplitKernels + RenderVKGIShadeSplit), its hits through the #1039 queue (M2; M1: bit 64)
    "kWorldDeferGiSplitTraceSpv", "kWorldDeferGiSplitMissSpv", "kWorldDeferGiSplitHitMatSpv",
    "kWorldDeferGiSplitHitLightSpv", "kWorldDeferGiSplitReuseSpv", "kWorldDeferGiSplitReshadeSpv",
    "kWorldDeferGiSplitHitMatVqSpv", "kWorldDeferGiSplitHitLightVqSpv",
    "kWorldDeferGiSplitHitReqVq2Spv", "kWorldDeferGiSplitHitLightVq2Spv",
    "kWorldDeferVisWalkSpv",
    # pass 15 split shadow core (RenderVKShadowSplitKernels)
    "kWorldDeferShadowMergeSelectSpv", "kWorldDeferShadowMergeSunVisSpv", "kWorldDeferShadowMergeSunVisIsoSpv",
    "kWorldDeferShadowSplitVisSpv", "kWorldDeferShadowSplitVisIsoSpv", "kWorldDeferShadowSplitShadeSpv",
    # (perf-4k60) the split lighting's REFLECT pair (half-resolution blocks + the sharp pixels at full resolution)
    "kWorldDeferReflectHalfSpv", "kWorldDeferReflectSharpSpv",
]
OWN = ["vqb_gbuf.comp"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--cache", default=None)
    ap.add_argument("--only", default=None, help="comma-separated symbols (default: all)")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    table = {e[2]: e for e in rw.SHADERS}
    names = a.only.split(",") if a.only else [n for n in KERNELS if n in table]
    missing = [n for n in names if n not in table]
    if missing:
        raise SystemExit("vqb_spv: not in regen_world_spv.SHADERS: " + ", ".join(missing))
    jobs = [rw.variant(table[n]) for n in names]
    words = spvbuild.compile_many(jobs, a.cache)
    for n, w in zip(names, words):
        with open(os.path.join(a.out, n + ".spv"), "wb") as f:
            f.write(struct.pack(f"<{len(w)}I", *w))
    # the names twins (no -O): reflection only
    glslc = spvbuild.tool("glslc")
    procs = []
    for n, (path, stage, flags) in zip(names, jobs):
        procs.append(subprocess.Popen([glslc, spvbuild.TARGET, f"-fshader-stage={stage}", *flags, path, "-o",
                                       os.path.join(a.out, n + ".names.spv")]))
    for own in OWN:
        procs.append(subprocess.Popen([glslc, spvbuild.TARGET, "-O", "-fshader-stage=comp", "-DCLEAN_STAGE_COMP",
                                       "-I", os.path.join(VKCLEAN, "shaders"), os.path.join(HERE, "shaders", own),
                                       "-o", os.path.join(a.out, os.path.splitext(own)[0] + ".spv")]))
    rc = 0
    for p in procs:
        rc |= p.wait()
    if rc:
        raise SystemExit("vqb_spv: a glslc run failed")
    # the shader set's identity (the report prints it: a timing is only comparable within one tree)
    import hashlib
    h = hashlib.blake2b(digest_size=8)
    for n, w in zip(names, words):
        h.update(n.encode())
        h.update(struct.pack(f"<{len(w)}I", *w))
    with open(os.path.join(a.out, "kernels.txt"), "w") as f:
        f.write("hash " + h.hexdigest() + "\n")
        for n, (path, stage, flags) in zip(names, jobs):
            f.write(n + " " + " ".join(flags) + "\n")
    print(f"vqb_spv: {len(names)} kernels -> {a.out} (hash {h.hexdigest()})")


if __name__ == "__main__":
    main()
