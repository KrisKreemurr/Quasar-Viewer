// #703 / #1242 The new interface (UIRenderer 2): its own RML documents and C++ controllers,
// bound directly to the viewer's models. No LLView floater is its model, no XUI is its layout.
//
// C1 today: the bar (top, or bottom with UINewBarAtBottom) with the location, the command field
// and the main windows, and the command surface (Ctrl+K: the menu tree, the floater registry and
// the preferences search, through LLCommandIndex). The classic toolbars and nav bar are hidden
// while it runs; the menu / status bar and the LLView floaters stay (C2+ replaces them).
//
// Rendering: one RmlUi layer recorded above the whole LLView UI (markOverlay at the end of the
// UI draw). Input: a frontmost LLView that hands mouse events to RmlUi only over its elements,
// and the keyboard while the command surface is open. Fixed cost: the models refresh at 4 Hz
// and only push changed values; RmlUi re-captures only when something changed.
#pragma once

#include <cstdint>

namespace Rml
{
class Context;
}

class LLNewUI
{
public:
    static LLNewUI& instance();

    bool start(Rml::Context* context);
    void stop();
    bool active() const;

    // Each frame from the RmlUi host before it decides to capture: refresh the models, keep
    // the classic chrome hidden. `density` is framebuffer pixels per UI unit, `glass` whether
    // the target supports the glass look (SDR; HDR targets use the solid surfaces).
    // True when the documents changed (capture).
    bool sync(float density, bool glass);

    // Ctrl+K (SLKeyCommands "command_palette", taken before the menus in UIRenderer 2).
    void togglePalette();
    // End of the LLView UI draw: the RmlUi layer goes above everything drawn so far.
    void markOverlay();
};
