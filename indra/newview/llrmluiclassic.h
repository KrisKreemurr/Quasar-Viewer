// #703 UI on RmlUi: CLASSIC mode, the current UI presented through RmlUi.
//
// The LLView tree stays the model and controller: XUI still builds it (skins and the
// layered translations included), floater C++ still calls getChild<>(), control_name
// bindings and commit / enable / visible callbacks are unchanged, and input still goes
// through LLView. Only the drawing moves: a top-level view (a floater, later the chrome)
// whose every descendant has a presenter is claimed through LLView::sPresentHook, its
// LLView draw is skipped, and its state is mirrored into RmlUi elements (absolute boxes
// at the LLView rects, skin images through the llimage decorator, text per laid-out line).
// Anything without a presenter keeps the LLView renderer, per top-level view, so the
// viewer is always fully usable while coverage grows.
#pragma once

#include <cstdint>
#include <string>

namespace Rml
{
class Context;
}

class LLRmlUiClassic
{
public:
    static LLRmlUiClassic& instance();

    // Load the classic document into the context and install the LLView hook.
    bool start(Rml::Context* context);
    // Remove the hook and the document (the LLView renderer takes everything back).
    void stop();
    bool active() const;

    // After this frame's LLView draw: mirror the claimed views' state into the document.
    // True when the document changed (the host re-captures the RmlUi display list).
    bool sync();

    struct Stats
    {
        uint32_t roots = 0, claimed = 0, nodes = 0, changes = 0;
        uint32_t created = 0, destroyed = 0, represented = 0; // elements / root re-mirrors since the last report
        double   syncMs = 0, peakSyncMs = 0;
    };
    Stats stats() const;
    void  resetCounters(); // the per-report element / re-mirror counters
    // Which top-level views are presented, and the view classes that keep the others on
    // the LLView renderer (the coverage report; also written to the log every minute).
    std::string coverageReport() const;
};
