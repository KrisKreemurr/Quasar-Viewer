#!/usr/bin/env python3
"""Check that the tested selection policy is wired to the real draw and VK paths."""
from pathlib import Path

root = Path(__file__).resolve().parents[2]
button = (root / "llui/llbutton.cpp").read_text()
editor = (root / "llui/lllineeditor.cpp").read_text()
image = (root / "llrender/lluiimage.cpp").read_text()
backend = (root / "newview/vkgfx/llrendervkbackend.cpp").read_text()
recorder = (root / "llrender/llrenderrecorder.h").read_text()

draw = (root / "llrender/llrender2dutils.cpp").read_text()
view = (root / "llui/llview.cpp").read_text()
floater = (root / "llui/llfloater.cpp").read_text()
inline = (root / "llrender/lluiimage.inl").read_text()
assert "ll_choose_widget_image" not in button + editor
assert "mLastDrawnImage" not in button and "mLastDrawnBgImage" not in editor
assert "LLUIImageDrawScope image_scope(viewp->mImageDrawState)" in view
assert "f->draw();" not in floater and "v->draw();" not in floater
for kind in range(3):
    assert f"choose_ui_draw_image(x, y, width, height, {kind}, desired)" in draw
assert draw.count("if (!chosen.texture) return;") == 3
assert "prepareImageGL(data)" in draw and "vkRequestCpuCopy()" in draw
assert "site->image" in draw and "state.sites.size() == 64" in draw
assert "if (!prepareForDraw" not in inline  # Borders must reach shared fallback too.
assert "return image && resolveTexture(image) != 0;" in backend
assert "if (!ready) mBroken = true;" in recorder
print("shared image wiring: PASS")
