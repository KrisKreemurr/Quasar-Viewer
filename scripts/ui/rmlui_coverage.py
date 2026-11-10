#!/usr/bin/env python3
"""#703: static estimate of classic-on-RmlUi coverage over the en floaters.

A floater is presentable when every widget it declares visible has a presenter in
indra/newview/llrmluiclassic.cpp (hidden widgets may lack one). The runtime decides for
real (C++ may add or show views); this is the planning number. Usage:
    python3 scripts/ui/rmlui_coverage.py [--list]
"""
import collections, pathlib, sys, xml.etree.ElementTree as ET

root = pathlib.Path(__file__).resolve().parents[2]
xui = root / "indra/newview/skins/default/xui/en"
# Widget tags whose LLView class has a presenter (or no draw() of its own: locate = LLUICtrlLocate,
# a placeholder that draws nothing).
SUPPORTED = {"floater", "panel", "layout_stack", "layout_panel", "button", "text", "icon", "view_border",
             "check_box", "radio_group", "radio_item", "slider", "slider_bar", "line_editor", "spinner",
             "combo_box", "menu_button", "view", "scroll_list", "name_list", "filter_editor", "search_editor", "progress_bar", "tab_container", "scroll_container", "text_editor", "simple_text_editor", "chat_history", "accordion", "accordion_tab", "color_swatch", "texture_picker", "web_browser", "flat_list_view", "loading_indicator", "locate", "profile_image", "avatar_list"}
# Non-visual children (parameters, strings, list data, callbacks; combo_item = a combo entry).
IGNORED = {"string", "floater.string", "panel.string", "combo_box.item", "columns", "column", "row", "rows",
           "on_click", "on_check", "on_enable", "on_visible", "init_callback", "commit_callback", "validate_callback",
           "radio_item.commit_callback", "button.commit_callback", "check_box.commit_callback", "item", "value",
           "combo_box.commit_callback", "slider.commit_callback", "line_editor.commit_callback", "keystroke_callback",
           "mouse_down_callback", "mouse_up_callback", "rename_callback", "tab", "filter", "params", "usetemplate",
           "text.commit_callback", "spinner.commit_callback", "texture", "font", "button.mouse_held_callback", "combo_item",
           "label", "tooltip", "check_box.label", "accordion_tab.string"}

def visible(el):
    return el.get("visible", "true").lower() != "false"

def walk(el, hidden, blockers):
    tag = el.tag
    if tag in IGNORED or "." in tag:
        return
    h = hidden or not visible(el)
    if tag not in SUPPORTED and not h:
        blockers[tag] += 1
        return  # its subtree is not presented either
    inc = el.get("filename")
    if inc and (xui / inc).exists():
        try:
            walk(ET.parse(xui / inc).getroot(), h, blockers)  # included panel file
        except ET.ParseError:
            pass
    for child in el:
        walk(child, h, blockers)

ok, pending, why = [], [], collections.Counter()
for f in sorted(xui.glob("floater_*.xml")):
    try:
        top = ET.parse(f).getroot()
    except ET.ParseError:
        continue
    if top.tag != "floater":
        continue
    b = collections.Counter()
    walk(top, False, b)
    (ok if not b else pending).append((f.name, b))
    for tag in b:
        why[tag] += 1
total = len(ok) + len(pending)
print(f"floaters: {total}; presentable by static estimate: {len(ok)} ({100.0 * len(ok) / max(total, 1):.1f}%)")
print("top blockers (floaters each widget keeps on LLView):")
for tag, n in why.most_common(15):
    print(f"  {tag:24s} {n}")
if "--list" in sys.argv:
    print("\npresentable:")
    for name, _ in ok:
        print("  " + name)
    print("\npending:")
    for name, b in pending:
        print(f"  {name}: {', '.join(sorted(b))}")
