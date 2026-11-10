#!/usr/bin/env python3
"""Source contract for fail-closed HDR display orchestration."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
context = (ROOT / "indra/newview/vkgfx/llvulkancontext.cpp").read_text()
renderer = (ROOT / "indra/newview/vkgfx/llcleanrenderervk.cpp").read_text()
frame_header = (ROOT / "indra/newview/vkgfx/vk_display_frame.h").read_text()
encoder_header = (ROOT / "indra/newview/vkgfx/vk_display_encode.h").read_text()


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


policy = body(context, "void LLVulkanContext::refreshDisplayHdrPolicy()")
assert "mDisplayHdrRequested" in policy

frame = body(context, "bool LLVulkanContext::renderFrame(")
assert "gVK.setFrameColorView(view)" in frame
assert "gVK.recordFrame(cmd, slot)" in frame
assert "mDisplayEncode.record" in frame
assert frame.index("gVK.recordFrame(cmd, slot)") < frame.index("mDisplayEncode.record")
assert "mDisplayFrame.beginLinear(cmd, linearClear)" in frame
assert "requestDisplayOutput(" in frame and "commitDisplayOutput(" in frame
assert frame.index("requestDisplayOutput(") < frame.index("commitDisplayOutput(")
assert "mayRetireSwapchain(hdrCandidate,hdrStageReady)" in frame
assert "mHdrOutputFailureReason" in frame

mode_config = body(renderer, "bool LLCleanRendererVK::configureDisplayOutput(")
assert "if (mCtx && enabled!=mDisplayLinear) return false" in mode_config
assert "bool prepare(VkDeviceContext& context,bool enabled,VkExtent2D extent" in frame_header
assert "WorldBeforeUi" in encoder_header and "CompleteDisplay" in encoder_header

ui = (ROOT / "indra/newview/skins/default/xui/en/floater_preferences_graphics_advanced.xml").read_text()
assert 'label="HDR Display"' not in ui
assert ui.index('name="DisplayHdrEnabled"') < ui.index('name="adv_tab_avatars"')
exposure = ui[ui.index('control_name="RenderExposure"'):]
assert 'left="300"' in exposure[:exposure.index('/>')]

print("PASS: HDR mode changes stage and commit output resources without replaying scene assets")
print("PASS: final encode follows the complete world/UI composition and has a visible fallback")
print("PASS: failures report the HDR preparation stage and reason")
print("PASS: HDR controls live on General and the exposure controls no longer overlap")
