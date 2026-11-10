#!/usr/bin/env python3
"""Source contract for the current HDR candidate's SDR snapshot boundaries."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[3]
renderer = (ROOT / "indra/newview/vkgfx/llcleanrenderervk.cpp").read_text()
context = (ROOT / "indra/newview/vkgfx/llvulkancontext.cpp").read_text()
encode = (ROOT / "indra/newview/vkgfx/vk_display_encode.h").read_text()


def body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for pos in range(opening, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : pos]
    raise AssertionError(f"unterminated function: {signature}")


capture = body(
    renderer,
    "void LLCleanRendererVK::recordCaptureAt(VkCommandBuffer cmd, uint32_t point",
)
assert "if (!mCapPending || mCapPending != point) return;" in capture
assert "recordSwapCopy(cmd, mCapBuf, mCapCap, width, height)" in capture

copy = body(
    renderer,
    "bool LLCleanRendererVK::recordSwapCopy(VkCommandBuffer cmd, GpuBuffer& buf",
)
assert "vkCmdCopyImageToBuffer(cmd, mSwapImage" in copy
assert "width * height * 4" in copy

world_record = body(
    renderer,
    "void LLCleanRendererVK::recordWorld(VkCommandBuffer cmd",
)
assert re.search(r"recordCaptureAt\(cmd,\s*1,", world_record)

point2 = context.index("recordCaptureAt(cmd, 2, ext.width, ext.height)")
ui_replay = context.index("gVK.recordFrame(cmd, slot)", point2)
point3 = context.index("recordCaptureAt(cmd, 3, ext.width, ext.height)", ui_replay)
assert point2 < ui_replay < point3
assert "W->capturePending() != 2u && !W->liveSnapPoint2Pending()" in context
offscreen = body(context, "bool LLVulkanContext::renderOffscreen(")
assert "recordCaptureAt(cmd, 2, w, h)" in offscreen

assert "SDR output requires WorldBeforeUi" in encode
assert "tone-map the world, then replay UI/HUD" in encode
assert "VkDisplayEncode" not in capture

print("PASS: capture points 1/2/3 preserve world, pre-UI, and post-UI ordering")
print("PASS: current copies still use a four-byte swap-image source")
print("GAP: renderer capture hooks do not resolve WorldBeforeUi into canonical SDR before readback")
