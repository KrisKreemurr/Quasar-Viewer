#!/usr/bin/env python3
"""Check the renderer bloom predicate and its preference controls."""
import itertools
import re
import xml.etree.ElementTree as ET
from pathlib import Path

root = Path(__file__).resolve().parents[2]
renderer = (root / "newview/vkgfx/llcleanrenderervk.cpp").read_text()
match = re.search(r"want = (s_hdr_on && s_glow_on && !s_classic_glow && \(F32\)s_bloom_strength > 0\.f);", renderer)
assert match, "production bloom predicate is missing"
expr = match.group(1).replace("(F32)", "").replace("0.f", "0.0").replace("&&", "and").replace("!", "not ")
for hdr, bloom, classic, strength in itertools.product((False, True), (False, True), (False, True), (0.0, 0.5)):
    variables = dict(s_hdr_on=hdr, s_glow_on=bloom, s_classic_glow=classic, s_bloom_strength=strength)
    actual = eval(expr, {"__builtins__": {}}, variables)
    assert actual == ((hdr, bloom, classic, strength) == (True, True, False, 0.5)), variables
resolve = renderer.split("bool LLCleanRendererVK::taaGlowWanted() const", 1)[1].split("void LLCleanRendererVK::ensureTAA", 1)[0]
assert "(s_bloom || s_classic)" in resolve
assert re.search(r"if \(glow_on(?: && [^\n]+)?\)\s*recordGlow\(cmd, toneView, width, height\)", renderer)
ui = ET.parse(root / "newview/skins/default/xui/en/floater_preferences_graphics_advanced.xml").getroot()
for name in ("BloomStrength", "BloomThreshold", "BloomSatPreserve", "BloomHueHighlights"):
    control = ui.find(".//*[@name='" + name + "']")
    assert control is not None and control.get("enabled_control") is None
assert "instead of HDR bloom" in ui.find(".//*[@name='ClassicGlow']").get("tool_tip")
source = (root / "newview/llfloaterpreferencesgraphicsadvanced.cpp").read_text()
assert '{"RenderClassicGlow", "RenderGlow", "RenderHDREnabled"}' in source
assert 'const bool bloom_controls = bloom_available && gSavedSettings.getBOOL("RenderGlow")' in source
assert 'setBOOL("RenderGlow"' not in source
print("PASS: production predicate 16 cases, classic glow input, UI wiring")
