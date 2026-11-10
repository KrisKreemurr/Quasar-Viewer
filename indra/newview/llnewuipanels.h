// #703 The new interface's docked panels (C2): People & chat on the left, Inventory on the right.
// Each collapses to a 12 dp edge strip. Bound to the viewer's models directly: LLAvatarTracker
// (friends), LLWorld (nearby), LLIMModel / LLNearbyChatLog (conversations), LLInventoryRows over
// LLInventoryModel (the tree). Lists are virtualised: a fixed pool of row slots shows the visible
// window of the list, so a 10000-item inventory costs what a 30-row one does.
#pragma once

#include <functional>
#include <string>

namespace Rml
{
class Context;
class Element;
class ElementDocument;
}

class LLNewUIPanels
{
public:
    using DocumentLoader = std::function<Rml::ElementDocument*(const char* rml, const char* name)>;

    static LLNewUIPanels& instance();
    bool start(Rml::Context* context, const DocumentLoader& load);
    void stop();
    bool active() const;
    // `top` / `bottom`: the docks' extent in UI units from the window top / bottom. True when
    // their documents changed.
    bool sync(int top, int bottom, bool glass);
    bool ownsDocument(const Rml::ElementDocument* doc) const;
    // Return in a focused text field of a panel (chat line: send). True when it was handled.
    bool onReturn(Rml::Element* focused);
};
