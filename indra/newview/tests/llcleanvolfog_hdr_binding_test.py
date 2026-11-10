#!/usr/bin/env python3
"""Source-bound regression for camera-medium scene-HDR descriptor publication."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[3]
hdr = (root / "indra/newview/vkgfx/llcleanvolfogvk.h").read_text()
medium = (root / "indra/newview/vkgfx/llcleanvolfogvk.cpp").read_text()
renderer = (root / "indra/newview/vkgfx/llcleanrenderervk.cpp").read_text()
frame = hdr.split("struct Frame\n", 1)[1].split("};", 1)[0]
assert "VkImageView hdrView" in frame and "uint64_t hdrGeneration" in frame
assert "mHdrGeneration" in hdr and "mHdrBindingDirty" in hdr
prepare = medium.split("bool LLCleanVolFogVK::prepare", 1)[1].split("void LLCleanVolFogVK::fillFrameInfo", 1)[0]
assert "mHdrView != f.hdrView || mHdrGeneration != f.hdrGeneration" in prepare
assert prepare.index("if (hdrChanged) mHdrBindingDirty = true") < prepare.index("if (hdrChanged) mHdrView = f.hdrView") < prepare.index("ensurePipelines(f)")
# Pipeline/retry/image failures precede publication; the dirty bit is only cleared after
# writeSets succeeds in the ready path.
assert prepare.index("if (!ensurePipelines(f)) return false") < prepare.index("if (mHdrBindingDirty)")
assert prepare.index("if (!ensureImages") < prepare.index("if (mHdrBindingDirty)")
publish = prepare.split("if (mHdrBindingDirty)", 1)[1].split("// Volumetric particles", 1)[0]
assert publish.index("writeSets()") < publish.index("mHdrGeneration = f.hdrGeneration") < publish.index("mHdrBindingDirty = false")
# writeSets reaches all p/s combinations and writes the HDR image binding for every set.
sets = medium.split("void LLCleanVolFogVK::writeSets()", 1)[1].split("// DEBUG TEST VOLUMES", 1)[0]
assert "for (uint32_t p = 0; p < 2; ++p)" in sets and "for (uint32_t s = 0; s < 2; ++s)" in sets
assert "wr[3].dstBinding = i==4 ? 12u : 8u+i" in sets or "ew[i].dstBinding=8+i" in sets
assert "mHdrView" in sets
assert "mHdrGeneration = 0" in medium and "mHdrBindingDirty = true" in medium
# Renderer passes the existing monotonic image-target generation alongside its view.
assert re.search(r"f\.hdrView\s*=\s*sceneHdr\(\)\.(?:view|arrayView);\s*f\.hdrGeneration\s*=\s*mTargetGen;", renderer)
print("PASS: view-handle reuse, early-return retry, four-set publish, teardown reset, renderer generation")
