from pathlib import Path
import re
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[4]
paths = [
    root / "indra/newview/vkgfx/vk_display_limits.h",
    root / "indra/newview/vkgfx/llrendervk/shaders/ui_display.glsl",
    root / "indra/vkclean/shaders/clean_display_common.glsl",
]
for path in paths:
    source = path.read_text()
    assert re.search(r"kDisplayReferenceWhiteMaxNits\s*=\s*1000\.(?:0|f)", source), path
ui = ET.parse(root / "indra/newview/skins/default/xui/en/floater_preferences_graphics_advanced.xml")
slider = ui.find(".//slider[@name='DisplayHdrPaperWhite']")
assert slider is not None and slider.get("max_val") == "1000" and slider.get("increment") == "1"
host = (root / "indra/newview/vkgfx/vk_display_hdr.h").read_text()
assert "std::min(kDisplayReferenceWhiteMaxNits, r.peakNits)" in host
print("display reference-white bounds source checks passed")
