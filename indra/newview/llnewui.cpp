// #703 / #1242 The new interface (see llnewui.h).
#include "llviewerprecompiledheaders.h"
#include "llnewui.h"

#include "llagent.h"
#include "llcommandindex.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llkeyboard.h"
#include "llnewuikit.h"
#include "llnewuipanels.h"
#include "llrender.h"
#include "lltoolbarview.h"
#include "llui.h"
#include "llview.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "slkeycommands.h"
#include "vkgfx/vk_ui_texture_host.h" // slUIMarkExternalLayer

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/StringUtilities.h>
#include <new>
#include <string>
#include <vector>

namespace
{
struct BarButton
{
    std::string label, target;
};
struct ResultRow
{
    std::string kind, label, path, shortcut;
};

// The bar's main windows (registry names; one that is not registered is left out).
const BarButton kBarButtons[] = { { "People", "people" },     { "Chat", "im_container" },  { "Inventory", "inventory" },
                                  { "Build", "build" },       { "Map", "world_map" },      { "Search", "search" },
                                  { "Snapshot", "snapshot" }, { "Settings", "preferences" } };
constexpr size_t kMaxResults = 12;

class InputView;

struct State
{
    Rml::Context*         context = nullptr;
    Rml::ElementDocument* bar     = nullptr;
    Rml::ElementDocument* palette = nullptr;
    Rml::DataModelHandle  barModel, paletteModel;
    InputView*            input = nullptr;
    LLHandle<LLView>      navBar;
    LLCommandIndex        index;
    // bar model
    std::string            location, hint;
    std::vector<BarButton> buttons;
    // palette model
    bool                         open = false;
    std::string                  query, lastQuery = "\x01";
    std::vector<ResultRow>       rows;
    std::vector<LLCommandIndex::Entry> entries; // what each row runs (copies: safe across rebuilds)
    int                          selected = 0;
    // state
    float density = 1.f;
    bool  glass = true, glassApplied = false, atBottom = false, placed = false, changed = true, inputSeen = false;
    F64   nextRefresh = 0;
    S32   barTop = -1, barBottom = -1;
    U32   markedFrame = 0;
    size_t indexedSize = 0; // index size the open surface's results were computed from
    F64    nextRequery = 0;
};
State* gState = nullptr;

void markChanged()
{
    if (gState)
        gState->changed = true;
}

int rmlModifiers(MASK mask)
{
    int m = 0;
    if (mask & MASK_CONTROL)
        m |= Rml::Input::KM_CTRL;
    if (mask & MASK_SHIFT)
        m |= Rml::Input::KM_SHIFT;
    if (mask & MASK_ALT)
        m |= Rml::Input::KM_ALT;
    return m;
}

void runRow(size_t i);
void closePalette();

// The RmlUi layer's z slot: above the main view (world UI, chrome, floaters), below the menus,
// pop-ups and tooltips (the root view draws those after the main view).
void markLayer()
{
    State* s = gState;
    const U32 frame = LLFrameTimer::getFrameCount();
    if (!s || s->markedFrame == frame)
        return;
    s->markedFrame = frame;
    gGL.flush();
    slUIMarkExternalLayer();
}
bool presentHook(LLView* v)
{
    if (gMenuHolder && v == gMenuHolder)
        markLayer();
    return false; // the new interface never takes over an LLView
}

// Frontmost view: mouse events reach RmlUi first and are kept only over its elements; while the
// command surface is open it holds the keyboard focus (and a click outside closes it).
class InputView final : public LLView
{
public:
    InputView(const LLView::Params& p) : LLView(p) {}

    S32           lastX = S32_MIN, lastY = S32_MIN;
    Rml::Element* lastHover = nullptr;

    bool toRml(S32 x, S32 y, MASK mask)
    {
        State* s = gState;
        if (!s || !s->context)
            return false;
        // LLViewerWindow sends a hover every frame even with a resting mouse: only a real move
        // reaches RmlUi, and only a change of the hovered element is a change to capture.
        if (x != lastX || y != lastY)
        {
            lastX = x;
            lastY = y;
            const S32 rootH = LLUI::getInstance()->getRootView()->getRect().getHeight();
            s->context->ProcessMouseMove(int(x * s->density), int((rootH - y) * s->density), rmlModifiers(mask));
        }
        Rml::Element* hover = s->context->GetHoverElement();
        if (hover != lastHover)
        {
            lastHover = hover;
            markChanged(); // :hover styles
        }
        if (!hover)
            return false;
        Rml::ElementDocument* doc = hover->GetOwnerDocument();
        return hover != doc && (doc == s->bar || doc == s->palette || LLNewUIPanels::instance().ownsDocument(doc));
    }
    bool handleHover(S32 x, S32 y, MASK mask) override
    {
        const bool over = toRml(x, y, mask);
        return over || (gState && gState->open);
    }
    bool handleMouseDown(S32 x, S32 y, MASK mask) override
    {
        if (!gState)
            return false;
        if (toRml(x, y, mask))
        {
            gState->context->ProcessMouseButtonDown(0, rmlModifiers(mask));
            // A text field of the new interface takes the keyboard (chat line, inventory search).
            if (Rml::Element* focus = gState->context->GetFocusElement(); focus && focus->GetTagName() == "input")
                gFocusMgr.setKeyboardFocus(this);
            markChanged();
            return true;
        }
        if (gState->open)
        {
            closePalette(); // a click outside the surface closes it and is not passed on
            return true;
        }
        return false;
    }
    bool handleMouseUp(S32 x, S32 y, MASK mask) override
    {
        if (!gState)
            return false;
        const bool over = toRml(x, y, mask);
        gState->context->ProcessMouseButtonUp(0, rmlModifiers(mask));
        markChanged();
        return over;
    }
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override
    {
        if (!gState || !toRml(x, y, 0))
            return false;
        gState->context->ProcessMouseWheel(float(clicks), 0);
        markChanged();
        return true;
    }
    // A text field of a docked panel has the RmlUi focus.
    Rml::Element* panelInput() const
    {
        if (!gState || !gState->context)
            return nullptr;
        Rml::Element* focus = gState->context->GetFocusElement();
        return focus && focus->GetTagName() == "input" && LLNewUIPanels::instance().ownsDocument(focus->GetOwnerDocument()) ? focus
                                                                                                                         : nullptr;
    }
    bool handleKeyHere(KEY key, MASK mask) override
    {
        State* s = gState;
        if (s && !s->open)
            if (Rml::Element* input = panelInput())
            {
                if (key == KEY_ESCAPE)
                {
                    input->Blur();
                    gFocusMgr.setKeyboardFocus(nullptr);
                }
                else if (key == KEY_RETURN)
                    LLNewUIPanels::instance().onReturn(input);
                else
                    forwardKey(key, mask);
                markChanged();
                return true;
            }
        if (!s || !s->open)
            return false;
        switch (key)
        {
            case KEY_ESCAPE:
                closePalette();
                return true;
            case KEY_UP:
                if (!s->rows.empty())
                    s->selected = (s->selected + int(s->rows.size()) - 1) % int(s->rows.size());
                s->paletteModel.DirtyVariable("selected");
                markChanged();
                return true;
            case KEY_DOWN:
                if (!s->rows.empty())
                    s->selected = (s->selected + 1) % int(s->rows.size());
                s->paletteModel.DirtyVariable("selected");
                markChanged();
                return true;
            case KEY_RETURN:
                runRow(size_t(s->selected));
                return true;
            default:
                break;
        }
        forwardKey(key, mask);
        markChanged();
        return true; // the surface is modal: keys do not reach the world
    }
    void forwardKey(KEY key, MASK mask)
    {
        State* s = gState;
        Rml::Input::KeyIdentifier ki = Rml::Input::KI_UNKNOWN;
        switch (key)
        {
            case KEY_BACKSPACE: ki = Rml::Input::KI_BACK; break;
            case KEY_DELETE: ki = Rml::Input::KI_DELETE; break;
            case KEY_LEFT: ki = Rml::Input::KI_LEFT; break;
            case KEY_RIGHT: ki = Rml::Input::KI_RIGHT; break;
            case KEY_HOME: ki = Rml::Input::KI_HOME; break;
            case KEY_END: ki = Rml::Input::KI_END; break;
            default: break;
        }
        if (ki == Rml::Input::KI_UNKNOWN && (mask & MASK_CONTROL) && key == 'A')
            ki = Rml::Input::KI_A;
        if (ki != Rml::Input::KI_UNKNOWN)
            s->context->ProcessKeyDown(ki, rmlModifiers(mask));
    }
    bool handleUnicodeCharHere(llwchar uni_char) override
    {
        State* s = gState;
        if (!s || (!s->open && !panelInput()) || uni_char < 0x20)
            return false;
        s->context->ProcessTextInput(static_cast<Rml::Character>(uni_char));
        markChanged();
        return true;
    }
    void onFocusLost() override
    {
        if (Rml::Element* input = panelInput())
            input->Blur();
        if (gState && gState->open)
            closePalette();
        LLView::onFocusLost();
    }
    void draw() override {} // nothing of its own: RmlUi draws the documents
};

const char* kBarRml = R"rml(<rml><head><title>bar</title><style>
body { position: absolute; left: 0; top: 0; right: 0; bottom: 0; pointer-events: none; }
#bar { pointer-events: auto; }
#command { width: 260dp; color: $muted; cursor: text; }
</style></head><body><div data-model="bar">
<div id="bar" class="bar">
  <div class="chip" data-attr-title="location">{{location}}</div>
  <div class="field" id="command" data-event-click="open()">{{hint}}</div>
  <div class="spacer"></div>
  <div class="btn" data-for="b : buttons" data-event-click="window(it_index)">{{b.label}}</div>
</div></div></body></rml>)rml";

const char* kPaletteRml = R"rml(<rml><head><title>palette</title><style>
body { position: absolute; left: 0; top: 0; right: 0; bottom: 0; pointer-events: none; }
#scrim { pointer-events: auto; }
#surface { pointer-events: auto; left: 50%; margin-left: -300dp; width: 600dp; top: 96dp; }
#queryBox { height: 32dp; line-height: 32dp; } #query { font-size: 15dp; }
#results { margin-top: 8dp; max-height: 400dp; }
.empty { padding: 8dp; }
</style></head><body><div data-model="palette">
<div id="scrim" class="scrim" data-event-click="close()"></div>
<div id="surface" class="popover">
  <div id="queryBox" class="field"><input id="query" class="bare" type="text" data-value="query" placeholder="Search menus, windows and settings"/></div>
  <div id="results" class="list">
    <div class="row" data-for="r : rows" data-class-selected="it_index == selected" data-event-click="run(it_index)">
      <div class="kind">{{r.kind}}</div><div class="label">{{r.label}}</div><div class="path">{{r.path}}</div>
      <div class="kbd" data-if="r.shortcut != ''">{{r.shortcut}}</div>
    </div>
    <div class="empty muted" data-if="rows.size == 0">No matches</div>
  </div>
</div></div></body></rml>)rml";

std::string themed(const char* rml, const LLNewUIKit::Tokens& tokens)
{
    // Every document carries the kit stylesheet; its own <style> is layout only and may use
    // $tokens too.
    std::string s = rml;
    const size_t head = s.find("<head>");
    if (head != std::string::npos)
        s.insert(head + 6, "<style>" + LLNewUIKit::stylesheet(tokens) + "</style>");
    for (const auto& [name, value] : tokens)
        for (size_t at = s.find("$" + name); at != std::string::npos; at = s.find("$" + name, at + value.size()))
            s.replace(at, name.size() + 1, value);
    return s;
}

void requery(State* s)
{
    s->lastQuery = s->query;
    std::vector<const LLCommandIndex::Entry*> found;
    s->index.query(s->query, kMaxResults - 1, found);
    s->rows.clear();
    s->entries.clear();
    auto add = [s](const LLCommandIndex::Entry& e)
    {
        const char* kind = e.kind == LLCommandIndex::Kind::Menu      ? "Menu"
                           : e.kind == LLCommandIndex::Kind::Window  ? "Window"
                           : e.kind == LLCommandIndex::Kind::Section ? "Panel"
                           : e.kind == LLCommandIndex::Kind::Setting ? "Setting"
                                                                     : "Search";
        s->rows.push_back({ kind, e.label, e.path, e.shortcut });
        s->entries.push_back(e);
    };
    for (const auto* e : found)
        add(*e);
    if (!s->query.empty())
        add(LLCommandIndex::settingsSearch(s->query));
    s->selected = 0;
    s->paletteModel.DirtyVariable("rows");
    s->paletteModel.DirtyVariable("selected");
    markChanged();
}

void openPalette()
{
    State* s = gState;
    if (!s || !s->palette || s->open)
        return;
    s->open = true;
    s->index.rebuild(); // fixed cost per open, not per frame
    s->query.clear();
    s->paletteModel.DirtyVariable("query");
    requery(s);
    s->palette->Show();
    if (Rml::Element* q = s->palette->GetElementById("query"))
        q->Focus();
    LL_INFOS("RmlUi") << "New interface: command surface opened (" << s->index.size() << " commands)" << LL_ENDL;
    if (s->input)
        gFocusMgr.setKeyboardFocus(s->input);
    markChanged();
}

void closePalette()
{
    State* s = gState;
    if (!s || !s->open)
        return;
    s->open = false;
    if (s->palette)
        s->palette->Hide();
    LL_INFOS("RmlUi") << "New interface: command surface closed" << LL_ENDL;
    if (s->input && gFocusMgr.getKeyboardFocus() == s->input)
        gFocusMgr.setKeyboardFocus(nullptr);
    markChanged();
}

void runRow(size_t i)
{
    State* s = gState;
    if (!s || i >= s->entries.size())
        return;
    const LLCommandIndex::Entry entry = s->entries[i]; // closing may change the lists
    closePalette();
    if (!LLCommandIndex::run(entry))
        LL_WARNS("RmlUi") << "New interface: command not available now: " << entry.label << " (" << entry.path << ")" << LL_ENDL;
}

std::string currentLocation()
{
    LLViewerRegion* region = gAgent.getRegion();
    if (!region)
        return "Not in a region";
    const std::string& parcel = LLViewerParcelMgr::getInstance()->getAgentParcelName();
    return parcel.empty() ? region->getName() : parcel + " - " + region->getName();
}

// The classic chrome the new bar replaces (restored by stop()).
void hideClassicChrome(State* s, bool hide)
{
    if (gToolBarView && gToolBarView->getVisible() == hide)
        gToolBarView->setVisible(!hide);
    if (LLView* nav = s->navBar.get())
    {
        if (hide && nav->getVisible())
            nav->setVisible(false);
        else if (!hide)
            nav->setVisible(gSavedSettings.getBOOL("ShowNavbarNavigationPanel"));
    }
}
} // namespace

LLNewUI& LLNewUI::instance()
{
    static LLNewUI ui;
    return ui;
}

bool LLNewUI::active() const { return gState != nullptr; }

bool LLNewUI::start(Rml::Context* context)
{
    if (gState || !context)
        return gState != nullptr;
    State* s = new (std::nothrow) State();
    if (!s)
        return false;
    try
    {
        s->context = context;
        LLNewUIKit::loadFonts();
        Rml::DataModelConstructor bar = context->CreateDataModel("bar");
        if (!bar)
            throw std::bad_alloc();
        bar.Bind("location", &s->location);
        bar.Bind("hint", &s->hint);
        if (auto b = bar.RegisterStruct<BarButton>())
            b.RegisterMember("label", &BarButton::label);
        bar.RegisterArray<std::vector<BarButton>>();
        bar.Bind("buttons", &s->buttons);
        bar.BindEventCallback("open", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { openPalette(); });
        bar.BindEventCallback("window",
                              [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
                              {
                                  if (gState && !args.empty())
                                  {
                                      const size_t i = size_t(args[0].Get<int>());
                                      if (i < gState->buttons.size())
                                          LLFloaterReg::toggleInstanceOrBringToFront(LLSD(gState->buttons[i].target));
                                  }
                              });
        s->barModel = bar.GetModelHandle();

        Rml::DataModelConstructor pal = context->CreateDataModel("palette");
        if (!pal)
            throw std::bad_alloc();
        pal.Bind("query", &s->query);
        pal.Bind("selected", &s->selected);
        if (auto r = pal.RegisterStruct<ResultRow>())
        {
            r.RegisterMember("kind", &ResultRow::kind);
            r.RegisterMember("label", &ResultRow::label);
            r.RegisterMember("path", &ResultRow::path);
            r.RegisterMember("shortcut", &ResultRow::shortcut);
        }
        pal.RegisterArray<std::vector<ResultRow>>();
        pal.Bind("rows", &s->rows);
        pal.BindEventCallback("run", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
                              { if (!args.empty()) runRow(size_t(args[0].Get<int>())); });
        pal.BindEventCallback("close", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { closePalette(); });
        s->paletteModel = pal.GetModelHandle();

        // Main windows that exist in this build.
        LLFloaterReg::forEachRegistered(
            [s](const std::string& name)
            {
                for (const BarButton& b : kBarButtons)
                    if (b.target == name)
                        s->buttons.push_back(b);
            });
        const std::string key = SLKeyCommands::instance().getBindingString("command_palette");
        s->hint      = "Search or run a command" + (key.empty() ? std::string() : "   " + key);
        s->navBar    = LLUI::getInstance()->getRootView()->findChildView("nav_bar_container", true)
                           ? LLUI::getInstance()->getRootView()->findChildView("nav_bar_container", true)->getHandle()
                           : LLHandle<LLView>();
        s->atBottom  = gSavedSettings.getBOOL("UINewBarAtBottom");
        s->index.rebuild(); // queues the floaters' structure: read a few files per frame from now
    }
    catch (const std::bad_alloc&)
    {
        delete s;
        return false;
    }
    gState = s; // the documents below read it (theme)
    const LLNewUIKit::Tokens tokens = LLNewUIKit::tokens(LLNewUIKit::Theme::Dark, true);
    s->bar     = context->LoadDocumentFromMemory(themed(kBarRml, tokens), "newui-bar.rml");
    s->palette = context->LoadDocumentFromMemory(themed(kPaletteRml, tokens), "newui-palette.rml");
    if (!s->bar || !s->palette)
    {
        stop();
        return false;
    }
    s->bar->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    LLView::sPresentHook = &presentHook; // (the classic presenter is off in UIRenderer 2)
    // The docks are a development experiment, off unless asked for: the bar's People, Chat and
    // Inventory buttons open the classic windows, which have the full functionality.
    if (gSavedSettings.getBOOL("UINewDocksDev"))
        LLNewUIPanels::instance().start(context, [tokens, context](const char* rml, const char* name)
                                        { return context->LoadDocumentFromMemory(themed(rml, tokens), name); });

    LLView::Params p;
    p.name           = "new_ui_input";
    p.rect           = LLUI::getInstance()->getRootView()->getLocalRect();
    p.follows.flags  = FOLLOWS_ALL;
    p.mouse_opaque   = false;
    s->input         = new (std::nothrow) InputView(p);
    if (s->input)
        LLUI::getInstance()->getRootView()->addChild(s->input); // front: first to see the mouse
    LL_INFOS("RmlUi") << "New interface on: bar + command surface (" << s->buttons.size() << " windows)" << LL_ENDL;
    return true;
}

void LLNewUI::stop()
{
    State* s = gState;
    if (!s)
        return;
    hideClassicChrome(s, false);
    LLNewUIPanels::instance().stop();
    if (LLView::sPresentHook == &presentHook)
        LLView::sPresentHook = nullptr;
    if (s->input)
    {
        if (gFocusMgr.getKeyboardFocus() == s->input)
            gFocusMgr.setKeyboardFocus(nullptr);
        if (LLView* parent = s->input->getParent())
            parent->removeChild(s->input);
        delete s->input;
    }
    if (s->bar)
        s->bar->Close();
    if (s->palette)
        s->palette->Close();
    if (s->context)
    {
        s->context->RemoveDataModel("bar");
        s->context->RemoveDataModel("palette");
    }
    delete s;
    gState = nullptr;
}

bool LLNewUI::sync(float density, bool glass)
{
    State* s = gState;
    if (!s)
        return false;
    s->density = density;
    hideClassicChrome(s, true);

    // The bar where the nav bar was (top) or at the bottom edge, in UI units.
    const S32 rootH = LLUI::getInstance()->getRootView()->getRect().getHeight();
    S32       top   = 0;
    if (!s->atBottom)
    {
        if (LLView* nav = s->navBar.get())
            top = rootH - nav->calcScreenRect().mTop;
    }
    else
        top = rootH - 28;
    if (top != s->barTop)
    {
        s->barTop = top;
        if (Rml::Element* bar = s->bar->GetElementById("bar"))
        {
            bar->SetProperty("top", std::to_string(top) + "dp");
            bar->SetClass("bottom", s->atBottom);
        }
        markChanged();
    }
    if (glass != s->glassApplied || !s->placed)
    {
        // Glass on SDR targets, solid surfaces otherwise (the automatic fallback).
        s->glassApplied = glass;
        s->placed       = true;
        s->bar->SetClass("glass", glass);
        s->palette->SetClass("glass", glass);
        markChanged();
    }

    // Models at 4 Hz, pushing changed values only.
    const F64 now = LLTimer::getTotalSeconds();
    if (now >= s->nextRefresh)
    {
        s->nextRefresh = now + 0.25;
        const std::string loc = currentLocation();
        if (loc != s->location)
        {
            s->location = loc;
            s->barModel.DirtyVariable("location");
            markChanged();
        }
    }
    // The floaters' structure, a few XUI files per frame until complete (fixed cost).
    if (!s->index.structureComplete())
    {
        s->index.tick();
        if (s->index.structureComplete())
            LL_INFOS("RmlUi") << "New interface: command index complete, " << s->index.size() << " entries ("
                              << s->index.structureSize() << " panels and settings)" << LL_ENDL;
    }
    LLCommandIndex::tickHighlights();
    // Re-run the query when it changes, and (at most 4x a second) while the index is still growing.
    if (s->open && (s->query != s->lastQuery || (s->indexedSize != s->index.size() && now >= s->nextRequery)))
    {
        s->indexedSize = s->index.size();
        s->nextRequery = now + 0.25;
        requery(s);
    }
    if (s->open)
        markChanged(); // the caret and typing: modal and short-lived, captured while open

    // The docks: under the bar (top) or above it (bottom), to the other edge.
    if (LLNewUIPanels::instance().active())
    {
        const int barH = 28;
        if (LLNewUIPanels::instance().sync(s->atBottom ? 0 : top + barH, s->atBottom ? barH : 0, s->glassApplied))
            markChanged();
    }

    const bool changed = s->changed;
    s->changed         = false;
    return changed;
}

void LLNewUI::togglePalette()
{
    if (!gState)
        return;
    if (gState->open)
        closePalette();
    else
        openPalette();
}

void LLNewUI::markOverlay()
{
    markLayer(); // no-op when the menu holder already placed it this frame
}
