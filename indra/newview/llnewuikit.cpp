// #703 / #1242 The new interface's widget kit (see llnewuikit.h).
#include "llviewerprecompiledheaders.h"
#include "llnewuikit.h"

#include "lldir.h"
#include "llviewercontrol.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/DataVariable.h>

#include <RmlUi/Core/Core.h>
#include <algorithm>

namespace
{
// The component stylesheet. Sizes in dp (UI units: they follow the UI size and the display
// scale). Spacing grid 4 dp; type scale caption 11 / body 13 / title 15 / display 20.
const char* kKit = R"rcss(
body { font-family: Inter; font-size: 13dp; color: $text; font-effect: $textEffect; } /* inherited */
div, span, p { display: block; }
span { display: inline; }
.caption { font-size: 11dp; color: $muted; }
.title { font-size: 15dp; font-weight: bold; }
.muted { color: $muted; }

/* bar: the top (or bottom) bar */
.bar { position: absolute; left: 0; right: 0; height: $barH; display: flex; flex-direction: row; align-items: center;
       padding: 0 8dp; gap: 4dp; background-color: $bar; border-bottom: 1dp $border; }
.bar.bottom { border-bottom-width: 0; border-top: 1dp $border; }
.glass .bar, .glass .panel, .glass .popover { backdrop-filter: blur(12px); }
.spacer { flex: 1 1 0; }

/* btn: icon or text buttons (bar and panels) */
.btn { display: block; height: 22dp; line-height: 22dp; padding: 0 8dp; border-radius: $radius; color: $text;
       background-color: transparent; cursor: pointer; white-space: nowrap; }
.btn:hover { background-color: $hover; }
.btn:active { background-color: $pressed; }
.btn:focus { border: 1dp $accent; }
.btn.accent { background-color: $accent; color: $onAccent; font-effect: none; } /* opaque fill; dark text */

/* field: text inputs and the command field look-alike */
.field { display: block; box-sizing: border-box; height: 22dp; line-height: 22dp; padding: 0 8dp; border-radius: $radius; background-color: $field;
         border: 1dp $border; color: $text; }
.field:hover { border-color: $borderStrong; }
.field:focus { border-color: $accent; }
/* A rounded text input clips its text through a full-screen mask texture: the rounded box is
   a wrapper (.field), the input inside it is square and bare (rectangular scissor only). */
input.bare { display: block; width: 100%; height: 100%; font-family: Inter; font-size: 13dp; color: $text;
             background-color: transparent; border-width: 0; }

/* chip: small status pills (balance, location) */
.chip { height: 20dp; line-height: 20dp; padding: 0 8dp; border-radius: 10dp; background-color: $chip; color: $text;
        white-space: nowrap; }

/* panel / popover: surfaces */
.panel { background-color: $surface; border: 1dp $border; border-radius: $radiusPanel; }
.popover { position: absolute; background-color: $raised; border: 1dp $border; border-radius: $radiusPopover;
           border-top-color: $borderStrong; padding: 8dp; }
/* No box-shadow: RmlUi bakes it through full-screen layers (a 4K target spent the UI image
   budget on one popover). Depth comes from the scrim and the raised surface. */
.scrim { position: absolute; left: 0; top: 0; right: 0; bottom: 0; background-color: $scrim; }

/* list rows (the command surface's results, later the docked lists) */
.list { display: block; } /* bounded rows; no overflow clip (a clip under rounded corners costs a mask) */
.row { display: flex; flex-direction: row; align-items: center; height: 28dp; padding: 0 8dp; border-radius: $radius;
       gap: 8dp; cursor: pointer; }
.row:hover { background-color: $hover; }
.row.selected { background-color: $selected; }
.row .label { flex: 1 1 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.row .path { color: $muted; font-size: 11dp; white-space: nowrap; }
.kbd { font-size: 11dp; color: $muted; border: 1dp $border; border-radius: 4dp; padding: 0 4dp; height: 16dp;
       line-height: 16dp; white-space: nowrap; }
.kind { font-size: 11dp; color: $accent; width: 56dp; white-space: nowrap; }

/* segmented: a small choice of views (People / Nearby / Chats) */
.segmented { display: flex; flex-direction: row; background-color: $field; border: 1dp $border; border-radius: $radius; padding: 2dp; gap: 2dp; }
.seg { flex: 1 1 0; text-align: center; height: 20dp; line-height: 20dp; border-radius: 4dp; color: $muted; cursor: pointer; }
.seg:hover { background-color: $hover; }
.seg.on { background-color: $selected; color: $text; }

/* toggle: a boolean setting */
.toggle { display: block; width: 30dp; height: 16dp; border-radius: 8dp; background-color: $chip; position: relative; cursor: pointer; }
.toggle .knob { position: absolute; left: 2dp; top: 2dp; width: 12dp; height: 12dp; border-radius: 6dp; background-color: $muted; }
.toggle.on { background-color: $accent; }
.toggle.on .knob { left: 16dp; background-color: $onAccent; }

/* tabs */
.tabs { display: flex; flex-direction: row; gap: 4dp; border-bottom: 1dp $border; }
.tab { padding: 4dp 8dp; color: $muted; cursor: pointer; }
.tab.on { color: $text; border-bottom: 2dp $accent; }

/* slider (RmlUi range input) */
input.range { display: block; width: 120dp; height: 16dp; }
input.range slidertrack { height: 4dp; margin-top: 6dp; background-color: $chip; border-radius: 2dp; }
input.range sliderbar { width: 12dp; height: 12dp; margin-top: 2dp; border-radius: 6dp; background-color: $accent; }
input.range sliderbar:hover { background-color: $text; }

/* docked panels and their edge strips */
.dock { position: absolute; width: 300dp; display: flex; flex-direction: column; gap: 6dp; padding: 8dp; pointer-events: auto; }
.dock.left { left: 0; border-right: 1dp $border; }
.dock.right { right: 0; border-left: 1dp $border; }
.dock .head { display: flex; flex-direction: row; align-items: center; gap: 4dp; }
.strip { position: absolute; width: 12dp; background-color: $bar; pointer-events: auto; cursor: pointer; }
.strip:hover { background-color: $hover; }
.strip.left { left: 0; border-right: 1dp $border; }
.strip.right { right: 0; border-left: 1dp $border; }
.viewport { position: relative; flex: 1 1 0; overflow: hidden; } /* square: a rectangular scissor, no mask */
.vrow { position: absolute; left: 0; right: 0; height: 22dp; line-height: 22dp; padding: 0 6dp; white-space: nowrap;
        overflow: hidden; cursor: pointer; display: flex; flex-direction: row; align-items: center; gap: 6dp; }
.vrow:hover { background-color: $hover; }
.vrow.selected { background-color: $selected; }
.vrow .ico { width: 16dp; height: 16dp; flex: 0 0 16dp; }
.vrow .twist { width: 10dp; flex: 0 0 10dp; color: $muted; }
.dot { width: 8dp; height: 8dp; border-radius: 4dp; background-color: $chip; flex: 0 0 8dp; }
.dot.on { background-color: #3ddc84; }
.messages { flex: 1 1 0; display: flex; flex-direction: column; justify-content: flex-end; overflow: hidden; }
.msg { padding: 2dp 4dp; }
.msg .from { color: $accent; font-weight: bold; }
.msg .time { color: $muted; font-size: 11dp; }
)rcss";

void replaceAll(std::string& s, const std::string& from, const std::string& to)
{
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
        s.replace(at, from.size(), to);
}
} // namespace

LLNewUIKit::Tokens LLNewUIKit::tokens(Theme theme, bool glass)
{
    // Design note §4 (research/UI_NEW_UX_DESIGN_2026-11-05.md). Legibility rule: every surface
    // that carries text is opaque enough (>= 0.92 alpha) that all text tokens keep WCAG AA (4.5:1)
    // over the worst world behind it (white behind the dark theme, black behind the light one),
    // including the hover / pressed / selected / chip overlays on it. Glass adds a blur where it is
    // allowed; it never lowers the backing. Text on translucent surfaces also gets a 1 px dark
    // outline ($textEffect). Ratios: the #703 legibility commit message.
    Tokens t;
    if (theme == Theme::Light)
        t = { { "surface", "#f5f6f8f5" }, { "raised", "#fffffffa" }, { "bar", "#f5f6f8f5" }, { "border", "#0000001a" },
              { "borderStrong", "#00000033" }, { "text", "#1b1d22" }, { "muted", "#4c535d" }, { "accent", "#1858b8" },
              { "onAccent", "#ffffff" }, { "hover", "#0000000d" }, { "pressed", "#00000017" }, { "selected", "#1f6fe01f" },
              { "field", "#ffffff" }, { "chip", "#0000000d" }, { "scrim", "#00000026" }, { "textEffect", "none" } };
    else
        t = { { "surface", glass ? "#10141aeb" : "#16181dff" }, { "raised", glass ? "#181d26f0" : "#1e2128ff" },
              { "bar", glass ? "#10141aeb" : "#16181dff" }, { "border", "#ffffff14" }, { "borderStrong", "#ffffff2e" },
              { "text", "#e8eaed" }, { "muted", "#aab0b8" }, { "accent", "#6aafff" }, { "onAccent", "#0b1220" },
              { "hover", "#ffffff0f" }, { "pressed", "#ffffff1a" }, { "selected", "#4c9dff2b" },
              { "field", glass ? "#00000040" : "#0f1114ff" }, { "chip", "#ffffff10" }, { "scrim", "#00000059" },
              { "textEffect", glass ? "outline(1px #000000a0)" : "none" } };
    t.push_back({ "radius", "6dp" });
    t.push_back({ "radiusPanel", "10dp" });
    t.push_back({ "radiusPopover", "14dp" });
    t.push_back({ "barH", "28dp" });
    return t;
}

std::string LLNewUIKit::stylesheet(const Tokens& tokens)
{
    std::string s = kKit;
    // Longest names first, so "$borderStrong" is not cut by "$border".
    Tokens sorted = tokens;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    for (const auto& [name, value] : sorted)
        replaceAll(s, "$" + name, value);
    return s;
}

void LLNewUIKit::loadFonts()
{
    for (const char* face : { "Inter-Regular.ttf", "Inter-Medium.ttf", "Inter-SemiBold.ttf", "Inter-Bold.ttf" })
        Rml::LoadFontFace(gDirUtilp->getExpandedFilename(LL_PATH_FONTS, face));
    // Glyphs Inter lacks (symbols, scripts) fall back to DejaVu.
    Rml::LoadFontFace(gDirUtilp->getExpandedFilename(LL_PATH_FONTS, "DejaVuSans.ttf"), true);
}

bool LLNewUISettingBinding::bind(Rml::DataModelConstructor& model, const std::string& variable, const std::string& control)
{
    LLControlVariable* var = gSavedSettings.getControl(control);
    if (!var)
        return false;
    const std::string name = control;
    const bool ok = model.BindFunc(
        variable,
        [name](Rml::Variant& out)
        {
            LLControlVariable* v = gSavedSettings.getControl(name);
            if (!v)
                return;
            const LLSD value = v->getValue();
            switch (v->type())
            {
                case TYPE_BOOLEAN: out = value.asBoolean(); break;
                case TYPE_S32:
                case TYPE_U32: out = value.asInteger(); break;
                case TYPE_F32: out = float(value.asReal()); break;
                default: out = Rml::String(value.asString()); break;
            }
        },
        [name](const Rml::Variant& in)
        {
            LLControlVariable* v = gSavedSettings.getControl(name);
            if (!v)
                return;
            switch (v->type())
            {
                case TYPE_BOOLEAN: v->setValue(LLSD(in.Get<bool>())); break;
                case TYPE_S32:
                case TYPE_U32: v->setValue(LLSD(in.Get<int>())); break;
                case TYPE_F32: v->setValue(LLSD(in.Get<float>())); break;
                default: v->setValue(LLSD(in.Get<Rml::String>())); break;
            }
        });
    if (!ok)
        return false;
    try
    {
        Rml::DataModelHandle handle = mModel;
        mConnections.emplace_back(var->getSignal()->connect(
            [handle, variable](LLControlVariable*, const LLSD&, const LLSD&) mutable { handle.DirtyVariable(variable); }));
    }
    catch (const std::bad_alloc&)
    {
        // bound, but changes made elsewhere show at the next full refresh only
    }
    return true;
}
