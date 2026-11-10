#!/usr/bin/env python3
"""Source-level regression checks for the independent HDR/glow preferences."""

from pathlib import Path
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
SETTINGS = ROOT / "app_settings/settings.xml"
XUI = ROOT / "skins/default/xui/en"


def llsd_entry(name):
    document = ET.parse(SETTINGS).getroot()
    for mapping in document.iter("map"):
        children = list(mapping)
        for index, child in enumerate(children[:-1]):
            if child.tag == "key" and child.text == name:
                value = children[index + 1]
                if value.tag != "map":
                    raise AssertionError(f"{name} is not a settings map")
                entries = list(value)
                return {entries[i].text: entries[i + 1].text
                        for i in range(0, len(entries) - 1, 2)
                        if entries[i].tag == "key"}
    raise AssertionError(f"missing setting {name}")


def controls(path):
    return [node.attrib for node in ET.parse(path).iter()
            if node.tag == "check_box"]


def main():
    classic = llsd_entry("RenderClassicGlow")
    assert classic["Persist"] == "1" and classic["Type"] == "Boolean" and classic["Value"] == "0"
    assert llsd_entry("RenderGlow")["Value"] == "1"
    available = llsd_entry("RenderVKDisplayHDRAvailable")
    assert available["Persist"] == "0" and available["Value"] == "0"
    assert llsd_entry("RenderVKDisplayHDR")["Persist"] == "1"
    assert llsd_entry("RenderGlowControlsMigrated")["Persist"] == "1"

    qp = controls(XUI / "floater_quick_prefs.xml")
    hdr_qp = next(control for control in qp if control.get("name") == "qp_hdr")
    assert hdr_qp["control_name"] == "RenderVKDisplayHDR"
    assert hdr_qp["enabled_control"] == "RenderVKDisplayHDRAvailable"

    advanced = ET.parse(XUI / "floater_preferences_graphics_advanced.xml").getroot()
    adv_controls = [node.attrib for node in advanced.iter("check_box")]
    assert any(control.get("control_name") == "RenderEnableEmissiveBuffer" for control in adv_controls)
    assert any(control.get("control_name") == "RenderClassicGlow" for control in adv_controls)
    assert any(control.get("control_name") == "RenderVKDisplayHDR" and
               control.get("enabled_control") == "RenderVKDisplayHDRAvailable"
               for control in adv_controls)
    assert not any(control.get("control_name") == "RenderDisableVintageMode" for control in adv_controls)

    app = (ROOT / "llappviewer.cpp").read_text()
    assert "if (!gSavedSettings.getBOOL(\"RenderGlowControlsMigrated\"))" in app
    assert 'if (!gSavedSettings.getBOOL("RenderHDREnabled"))' in app
    assert "gSavedSettings.setBOOL(\"RenderClassicGlow\", true)" in app
    assert "gSavedSettings.setBOOL(\"RenderEnableEmissiveBuffer\", true)" in app
    assert 'setBOOL("RenderHDREnabled"' not in app

    control = (ROOT / "llviewercontrol.cpp").read_text()
    assert '"RenderClassicGlow", handleGlowControlsChanged' in control
    assert '"RenderEnableEmissiveBuffer", handleEnableEmissiveChanged' in control
    assert '"RenderDisableVintageMode", handleDisableVintageMode' not in control

    renderer = (ROOT / "vkgfx/llcleanrenderervk.cpp").read_text()
    assert 's_classic_glow(gSavedSettings, "RenderClassicGlow", true)' in renderer
    assert 's_glow_on(gSavedSettings, "RenderGlow", true)' in renderer
    assert "if (glow_on && !mBloomActive && !stereoActive())" in renderer
    assert "recordGlow(cmd, toneView, width, height);" in renderer
    print("HDR settings split source checks passed")


if __name__ == "__main__":
    main()
