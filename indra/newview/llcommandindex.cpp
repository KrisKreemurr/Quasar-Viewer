// #703 The viewer's command index (see llcommandindex.h).
#include "llviewerprecompiledheaders.h"
#include "llcommandindex.h"

#include "lldir.h"
#include "llfloaterpreference.h"
#include "llfloaterreg.h"
#include "llkeyboard.h"
#include "llmenugl.h"
#include "llscrollcontainer.h"
#include "llsearchablecontrol.h"
#include "lltabcontainer.h"
#include "lluictrlfactory.h"
#include "llviewermenu.h"
#include "llxmlnode.h"

#include <algorithm>
#include <cctype>
#include <new>
#include <unordered_map>
#include <unordered_set>

namespace
{
const char* const kSep = " \xe2\x80\xba "; // " › "

std::string fold(const std::string& s)
{
    std::string out = s;
    for (char& c : out)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string trim(std::string s)
{
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

// "Snapshot...", "Shadows:" -> "Snapshot", "Shadows" (whitespace runs collapsed)
std::string cleanLabel(std::string label)
{
    std::string collapsed;
    for (char ch : label)
    {
        const bool space = std::isspace(static_cast<unsigned char>(ch)) != 0;
        if (space && (collapsed.empty() || collapsed.back() == ' '))
            continue;
        collapsed += space ? ' ' : ch;
    }
    label = trim(collapsed);
    while (!label.empty() && (label.back() == '.' || label.back() == ':' || label.back() == ' '))
        label.pop_back();
    return label;
}

// "im_container" -> "Im container" (a registry name with no better label)
std::string prettyName(const std::string& name)
{
    std::string out = name;
    for (char& c : out)
        if (c == '_')
            c = ' ';
    if (!out.empty())
        out[0] = char(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

// Acronyms in a label or tooltip ("Temporal upscaling (turns TAA on)" -> "taa"): words of 2-6
// characters that are all upper case letters / digits with at least two letters.
std::string acronyms(const std::string& text)
{
    std::string out;
    size_t      i = 0;
    while (i < text.size())
    {
        while (i < text.size() && !std::isalnum(static_cast<unsigned char>(text[i])))
            ++i;
        size_t j = i, letters = 0;
        bool   upper = true;
        while (j < text.size() && std::isalnum(static_cast<unsigned char>(text[j])))
        {
            const unsigned char c = static_cast<unsigned char>(text[j]);
            if (std::isalpha(c))
            {
                ++letters;
                upper = upper && std::isupper(c);
            }
            ++j;
        }
        if (j - i >= 2 && j - i <= 6 && upper && letters >= 2)
        {
            if (!out.empty())
                out += ' ';
            out += fold(text.substr(i, j - i));
        }
        i = j;
    }
    return out;
}

// Common terms the XUI spells out (the query word on the left also searches the right).
const std::pair<const char*, const char*> kAliases[] = {
    { "aa", "antialiasing" },     { "anti-aliasing", "antialiasing" }, { "msaa", "antialiasing" }, { "vsync", "vertical sync" },
    { "fps", "frame rate" },      { "lod", "level of detail" },        { "dof", "depth of field" }, { "ao", "ambient occlusion" },
    { "ssao", "ambient occlusion" }, { "gi", "global illumination" },  { "rt", "ray" },             { "hud", "hud" },
    { "ui", "interface" },        { "vram", "video memory" },          { "ram", "memory" },         { "dd", "draw distance" },
};

std::string join(const std::vector<std::string>& parts, const char* sep)
{
    std::string out;
    for (const std::string& p : parts)
    {
        if (p.empty())
            continue;
        if (!out.empty())
            out += sep;
        out += p;
    }
    return out;
}

std::string attr(LLXMLNode* node, const char* name)
{
    std::string v;
    node->getAttributeString(name, v);
    return v;
}

void walkMenu(LLMenuGL* menu, const std::string& path, std::vector<LLCommandIndex::Entry>& out, int depth)
{
    if (!menu || depth > 6)
        return;
    for (U32 i = 0, n = menu->getItemCount(); i < n && out.size() < LLCommandIndex::kMaxEntries; ++i)
    {
        LLMenuItemGL* item = menu->getItem(S32(i));
        if (!item || !item->getVisible())
            continue;
        const std::string label = cleanLabel(item->getLabel());
        if (label.empty())
            continue;
        if (auto* branch = dynamic_cast<LLMenuItemBranchGL*>(item))
        {
            walkMenu(branch->getBranch(), path.empty() ? label : path + kSep + label, out, depth + 1);
            continue;
        }
        if (!dynamic_cast<LLMenuItemCallGL*>(item)) // separators, tear-offs, ...
            continue;
        LLCommandIndex::Entry e;
        e.kind  = LLCommandIndex::Kind::Menu;
        e.id    = "menu:" + path + "/" + label;
        e.label = label;
        e.path  = path;
        if (item->getAcceleratorKey() != KEY_NONE)
            e.shortcut = LLKeyboard::stringFromAccelerator(item->getAcceleratorMask(), item->getAcceleratorKey());
        e.foldedLabel = fold(label);
        e.foldedPath  = fold(path);
        e.item        = item->getHandle();
        out.push_back(std::move(e));
    }
}

std::vector<std::string> words(const std::string& query)
{
    std::vector<std::string> out;
    const std::string        q = fold(query);
    size_t                   i = 0;
    while (i < q.size())
    {
        while (i < q.size() && q[i] == ' ')
            ++i;
        size_t j = i;
        while (j < q.size() && q[j] != ' ')
            ++j;
        if (j > i)
            out.push_back(q.substr(i, j - i));
        i = j;
    }
    return out;
}

// Every word somewhere in `text` (any order); the sum of positions (earlier is better).
bool allIn(const std::string& text, const std::vector<std::string>& ws, size_t& cost)
{
    cost = 0;
    for (const std::string& w : ws)
    {
        const size_t at = text.find(w);
        if (at == std::string::npos)
            return false;
        cost += at;
    }
    return true;
}

// Tiers: 0 exact label, 1 label prefix, 2 all words in the label, 3 in label + path,
// 4 in the setting name. Lower is better; within a tier, earlier and shorter first.
bool rank(const LLCommandIndex::Entry& e, const std::string& q, const std::vector<std::string>& ws, size_t& score)
{
    size_t cost = 0;
    size_t tier = 6;
    if (e.foldedLabel == q)
        tier = 0;
    else if (e.foldedLabel.compare(0, q.size(), q) == 0)
        tier = 1;
    else if (allIn(e.foldedLabel, ws, cost))
        tier = 2;
    else if (allIn(e.foldedLabel + " " + e.foldedPath, ws, cost))
        tier = 3;
    else if (!e.foldedAlias.empty() && allIn(" " + e.foldedAlias + " ", ws, cost))
        tier = 3;
    else if (!e.foldedSetting.empty() && allIn(e.foldedSetting, ws, cost))
        tier = 4;
    else if (!e.foldedTip.empty() && allIn(e.foldedTip, ws, cost))
        tier = 5;
    if (tier == 6)
        return false;
    // Menus and windows before sections and settings of the same tier.
    const size_t kindCost = e.kind == LLCommandIndex::Kind::Menu || e.kind == LLCommandIndex::Kind::Window ? 0 : 1;
    score = tier * 1000000 + kindCost * 100000 + std::min<size_t>(cost, 999) * 64 + std::min<size_t>(e.label.size(), 63);
    return true;
}

// The floaters the index may open, each with the key it opens with. A floater gets a Window
// entry (and its sections / settings) only when the viewer itself opens it by name alone: from
// the main menu or a toolbar command with Floater.Show / Toggle / ...OrBringToFront. Keyed and
// multi-instance floaters (profiles, IM sessions, item properties, previews, ...) open broken or
// empty without their key, so they are left to the menu items and code paths that pass one.
// kKeyed lists the few singletons the menu opens with a fixed key through their own function.
const std::pair<const char*, const char*> kKeyed[] = {
    { "settings_debug", "all" }, // Advanced.ShowDebugSettings "all"
};
std::unordered_map<std::string, std::string> sOpenable;

bool isFloaterOpen(const std::string& function)
{
    return function == "Floater.Show" || function == "Floater.Toggle" || function == "Floater.ShowOrBringToFront" ||
           function == "Floater.ToggleOrBringToFront";
}

void collectMenuOpens(LLXMLNode* node, int depth)
{
    if (!node || depth > 16)
        return;
    std::string function, parameter;
    if (node->getAttributeString("function", function) && isFloaterOpen(function) &&
        node->getAttributeString("parameter", parameter) && !parameter.empty())
        sOpenable.emplace(parameter, std::string());
    for (LLXMLNodePtr child = node->getFirstChild(); child.notNull(); child = child->getNextSibling())
        collectMenuOpens(child.get(), depth + 1);
}

void buildOpenable()
{
    if (!sOpenable.empty())
        return;
    LLXMLNodePtr root;
    if (LLUICtrlFactory::getLayeredXMLNode("menu_viewer.xml", root) && root.notNull())
        collectMenuOpens(root.get(), 0);
    LLXMLNodePtr commands;
    const std::string file = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "commands.xml");
    if (LLXMLNode::parseFile(file, commands, nullptr) && commands.notNull())
        for (LLXMLNodePtr c = commands->getFirstChild(); c.notNull(); c = c->getNextSibling())
        {
            std::string function, parameter;
            if (c->getAttributeString("execute_function", function) && isFloaterOpen(function) &&
                c->getAttributeString("execute_parameters", parameter) && !parameter.empty())
                sOpenable.emplace(parameter, std::string());
        }
    for (const auto& [name, key] : kKeyed)
        sOpenable[name] = key;
}

// The key `floater` opens with; false when the index must not open it.
bool openKey(const std::string& floater, LLSD& key)
{
    const auto it = sOpenable.find(floater);
    if (it == sOpenable.end())
        return false;
    key = it->second.empty() ? LLSD() : LLSD(it->second);
    return true;
}

// Flash highlights in flight (a navigation's target control).
struct Flash
{
    LLHandle<LLView> view;
    F64              until = 0;
};
std::vector<Flash> sFlashes;
} // namespace

void LLCommandIndex::rebuild()
{
    try
    {
        std::vector<Entry> entries;
        entries.reserve(1024);
        buildOpenable();
        walkMenu(gMenuBarView, std::string(), entries, 0);
        LLFloaterReg::forEachRegistered(
            [&entries](const std::string& name)
            {
                if (entries.size() >= kMaxEntries / 4 || !sOpenable.count(name))
                    return;
                Entry e;
                e.kind        = Kind::Window;
                e.id          = "window:" + name;
                e.label       = prettyName(name);
                e.path        = "Window";
                e.target      = name;
                e.foldedLabel = fold(e.label);
                e.foldedPath  = fold(name + " window");
                entries.push_back(std::move(e));
            });
        mMenu.swap(entries);
    }
    catch (const std::bad_alloc&)
    {
        // keep the previous lists: the surface still offers the settings search
    }
    if (!mStarted)
    {
        // The floaters' structure: queued once, read a few files per tick.
        mStarted = true;
        try
        {
            LLFloaterReg::forEachRegisteredFile(
                [this](const std::string& name, const std::string& file)
                {
                    if (!file.empty() && sOpenable.count(name))
                        mTasks.push_back(Task{ name, file, std::string(), {}, {}, 0 });
                });
        }
        catch (const std::bad_alloc&)
        {
            mTasks.clear();
        }
    }
}

void LLCommandIndex::addStructure(Entry&& e)
{
    if (mStructure.size() + mMenu.size() >= kMaxEntries)
        return;
    if (e.foldedLabel.empty())
        e.foldedLabel = fold(e.label);
    e.foldedPath    = fold(e.path);
    e.foldedSetting = fold(e.setting);
    mStructure.push_back(std::move(e));
}

void LLCommandIndex::walk(const Task& task, LLXMLNode* node, std::vector<std::string>& path, std::vector<std::string>& chain,
                          int depth)
{
    if (!node || depth > 24)
        return;
    std::string lastText; // a <text> label just before a control names it
    for (LLXMLNodePtr child = node->getFirstChild(); child.notNull(); child = child->getNextSibling())
    {
        LLXMLNode*        c   = child.get();
        const std::string tag = c->getName() ? c->getName()->mString : std::string();
        if (tag.find('.') != std::string::npos) // parameter nodes (combo_box.item, ...)
            continue;
        const std::string name = attr(c, "name");
        if (tag == "text")
        {
            lastText = cleanLabel(c->getTextContents());
            continue;
        }
        if (tag == "tab_container")
        {
            for (LLXMLNodePtr tab = c->getFirstChild(); tab.notNull(); tab = tab->getNextSibling())
            {
                LLXMLNode* t = tab.get();
                if (!t->getName() || std::string(t->getName()->mString) != "panel")
                    continue;
                const std::string tname = attr(t, "name");
                std::string       label = cleanLabel(attr(t, "label"));
                if (label.empty())
                    label = cleanLabel(attr(t, "title"));
                if (label.empty())
                    label = prettyName(tname);
                Entry e;
                e.kind   = Kind::Section;
                e.label  = label;
                e.path   = join(path, kSep);
                e.target = task.floater;
                e.chain  = chain;
                e.chain.push_back(tname);
                e.id     = "section:" + task.floater + "/" + join(e.chain, "/");
                addStructure(std::move(e));
                path.push_back(label);
                chain.push_back(tname);
                const std::string file = attr(t, "filename");
                if (!file.empty())
                    mTasks.push_back(Task{ task.floater, file, task.title, path, chain, depth + 1 });
                else
                    walk(task, t, path, chain, depth + 1);
                path.pop_back();
                chain.pop_back();
            }
            lastText.clear();
            continue;
        }
        if (tag == "accordion_tab")
        {
            const std::string title = cleanLabel(attr(c, "title"));
            if (!title.empty())
            {
                Entry e;
                e.kind    = Kind::Section;
                e.label   = title;
                e.path    = join(path, kSep);
                e.target  = task.floater;
                e.chain   = chain;
                e.control = name;
                e.id      = "section:" + task.floater + "/" + join(chain, "/") + "#" + name;
                addStructure(std::move(e));
                path.push_back(title);
            }
            walk(task, c, path, chain, depth + 1);
            if (!title.empty())
                path.pop_back();
            lastText.clear();
            continue;
        }
        const std::string setting = attr(c, "control_name");
        if (!setting.empty())
        {
            std::string label = cleanLabel(attr(c, "label"));
            if (label.empty())
                label = lastText;
            if (label.empty())
            {
                // A tooltip names it: its first sentence, kept short.
                label = cleanLabel(attr(c, "tool_tip"));
                const size_t stop = label.find_first_of(".:(");
                if (stop != std::string::npos && stop > 0)
                    label = cleanLabel(label.substr(0, stop));
                if (label.size() > 60)
                    label = label.substr(0, label.rfind(' ', 60)) + "...";
            }
            if (label.empty())
                label = prettyName(name.empty() ? setting : name);
            const std::string tip  = cleanLabel(attr(c, "tool_tip"));
            const std::string here = join(path, kSep);
            Entry e;
            e.kind        = Kind::Setting;
            e.label       = label;
            e.path        = here;
            e.target      = task.floater;
            e.chain       = chain;
            e.control     = name;
            e.setting     = setting;
            e.id          = "setting:" + task.floater + "/" + setting + (name.empty() ? std::string() : "#" + name);
            e.foldedTip   = fold(tip.substr(0, 240));
            e.foldedAlias = acronyms(label + " " + tip);
            addStructure(std::move(e));
            // Its choices (combo box / radio group / flyout items) are entries of their own:
            // picking one opens the panel at the control (the value is not changed).
            for (LLXMLNodePtr item = c->getFirstChild(); item.notNull(); item = item->getNextSibling())
            {
                const std::string itag = item->getName() ? item->getName()->mString : std::string();
                if (itag != "radio_item" && (itag.size() < 5 || itag.compare(itag.size() - 5, 5, ".item") != 0))
                    continue;
                std::string choice = cleanLabel(attr(item.get(), "label"));
                if (choice.empty())
                    choice = cleanLabel(item->getTextContents());
                if (choice.empty())
                    continue;
                const std::string itip = cleanLabel(attr(item.get(), "tool_tip"));
                Entry ce;
                ce.kind        = Kind::Setting;
                ce.label       = label + ": " + choice;
                ce.foldedLabel = fold(choice); // "taa" matches the choice itself first
                ce.path        = here;
                ce.target      = task.floater;
                ce.chain       = chain;
                ce.control     = name;
                ce.setting     = setting;
                ce.id          = "choice:" + task.floater + "/" + setting + "=" + attr(item.get(), "value") + "#" + name;
                ce.foldedTip   = fold((label + " " + itip).substr(0, 240));
                ce.foldedAlias = acronyms(choice + " " + itip);
                addStructure(std::move(ce));
            }
            lastText.clear();
            continue; // a bound control's other children are params, not more controls
        }
        // An included panel file inherits this place in the structure.
        const std::string file = attr(c, "filename");
        if (!file.empty() && (tag == "panel" || tag == "layout_panel"))
            mTasks.push_back(Task{ task.floater, file, task.title, path, chain, depth + 1 });
        else if (c->getFirstChild().notNull())
            walk(task, c, path, chain, depth + 1);
        if (tag != "icon" && tag != "view_border")
            lastText.clear();
    }
}

bool LLCommandIndex::tick()
{
    for (int n = 0; n < kFilesPerTick && !mTasks.empty(); ++n)
    {
        Task task = std::move(mTasks.front());
        mTasks.pop_front();
        if (mStructure.size() + mMenu.size() >= kMaxEntries)
        {
            mTasks.clear();
            break;
        }
        try
        {
            LLXMLNodePtr root;
            if (!LLUICtrlFactory::getLayeredXMLNode(task.file, root) || root.isNull()) // localised, skinned
                continue;
            std::vector<std::string> path  = task.path;
            std::vector<std::string> chain = task.chain;
            if (path.empty())
            {
                // A floater file: its title starts the breadcrumb.
                std::string title = cleanLabel(attr(root.get(), "title"));
                if (title.empty())
                    title = prettyName(task.floater);
                task.title = title;
                path.push_back(title);
            }
            walk(task, root.get(), path, chain, task.depth);
        }
        catch (const std::bad_alloc&)
        {
            mTasks.clear(); // stop where it is: the index keeps what it has
            break;
        }
    }
    return !mTasks.empty();
}

void LLCommandIndex::query(const std::string& query, size_t max, std::vector<const Entry*>& out) const
{
    out.clear();
    const std::vector<std::string> ws = words(query);
    if (ws.empty())
    {
        for (const Entry& e : mMenu)
            if (out.size() < max && e.kind == Kind::Menu)
                out.push_back(&e);
        return;
    }
    const std::string q = join(ws, " ");
    // Query words the XUI spells out differently ("aa" -> "antialiasing"): a second, lower pass.
    std::vector<std::string> expanded;
    bool                     anyAlias = false;
    for (const std::string& w : ws)
    {
        const auto* hit = std::find_if(std::begin(kAliases), std::end(kAliases), [&w](const auto& a) { return w == a.first; });
        if (hit != std::end(kAliases) && w != hit->second)
        {
            anyAlias = true;
            for (const std::string& part : words(hit->second))
                expanded.push_back(part);
        }
        else
            expanded.push_back(w);
    }
    const std::string qExpanded = join(expanded, " ");
    // Keep the best `max` (small: a bounded insertion list).
    std::vector<std::pair<size_t, const Entry*>> best;
    best.reserve(max + 1);
    auto consider = [&](const Entry& e)
    {
        size_t s = 0;
        if (!rank(e, q, ws, s))
        {
            if (!anyAlias || !rank(e, qExpanded, expanded, s))
                return;
            s += 500000; // an alias match ranks after the literal ones of its tier
        }
        if (best.size() == max && s >= best.back().first)
            return;
        auto at = std::upper_bound(best.begin(), best.end(), s, [](size_t v, const auto& p) { return v < p.first; });
        best.insert(at, { s, &e });
        if (best.size() > max)
            best.pop_back();
    };
    for (const Entry& e : mMenu)
        consider(e);
    for (const Entry& e : mStructure)
        consider(e);
    for (const auto& [s, e] : best)
        out.push_back(e);
}

LLCommandIndex::Entry LLCommandIndex::settingsSearch(const std::string& query)
{
    Entry e;
    e.kind   = Kind::Settings;
    e.id     = "settings-search";
    e.label  = "Search settings for \"" + query + "\"";
    e.path   = "Preferences";
    e.target = query;
    return e;
}

bool LLCommandIndex::run(const Entry& e)
{
    switch (e.kind)
    {
        case Kind::Menu:
        {
            auto* item = dynamic_cast<LLMenuItemGL*>(e.item.get());
            if (!item || !item->getVisible())
                return false;
            item->buildDrawLabel(); // refreshes its enable / check state, as opening the menu does
            if (!item->getEnabled())
                return false;
            item->onCommit();
            return true;
        }
        case Kind::Window:
        {
            LLSD key;
            if (!openKey(e.target, key))
                return false;
            LLFloaterReg::toggleInstanceOrBringToFront(LLSD(e.target), key);
            return true;
        }
        case Kind::Section:
        case Kind::Setting:
        {
            // Open it at the place: select each tab of the chain, then the control.
            LLSD key;
            if (!openKey(e.target, key))
                return false;
            LLFloater* floater = LLFloaterReg::showInstance(e.target, key, true);
            if (!floater)
                return false;
            LLView* scope = floater;
            for (const std::string& name : e.chain)
            {
                LLView* panel = scope->findChildView(name, true);
                if (!panel)
                    break;
                if (auto* tabs = dynamic_cast<LLTabContainer*>(panel->getParent()))
                    if (auto* p = dynamic_cast<LLPanel*>(panel))
                        tabs->selectTabPanel(p);
                scope = panel;
            }
            LLView* control = e.control.empty() ? nullptr : scope->findChildView(e.control, true);
            if (!control)
                return true; // the section itself
            // Scroll its scroll container (if any) to show it.
            LLView* scrolled = control;
            while (scrolled->getParent() && !dynamic_cast<LLScrollContainer*>(scrolled->getParent()))
                scrolled = scrolled->getParent();
            if (auto* scroller = dynamic_cast<LLScrollContainer*>(scrolled->getParent()))
            {
                LLRect rect;
                control->localRectToOtherView(control->getLocalRect(), &rect, scrolled);
                scroller->scrollToShowRect(rect);
            }
            // Flash it (the preferences search highlight) for a moment.
            if (auto* searchable = dynamic_cast<ll::ui::SearchableControl*>(control))
            {
                searchable->setHighlighted(true);
                if (sFlashes.size() >= 8)
                    sFlashes.erase(sFlashes.begin());
                sFlashes.push_back({ control->getHandle(), LLTimer::getTotalSeconds() + 2.5 });
            }
            if (auto* ctrl = dynamic_cast<LLUICtrl*>(control); ctrl && ctrl->getEnabled())
                ctrl->setFocus(true);
            return true;
        }
        case Kind::Settings:
            if (auto* prefs = LLFloaterReg::showTypedInstance<LLFloaterPreference>("preferences", LLSD(), true))
            {
                prefs->showSearch(e.target);
                return true;
            }
            return false;
    }
    return false;
}

void LLCommandIndex::tickHighlights()
{
    if (sFlashes.empty())
        return;
    const F64 now = LLTimer::getTotalSeconds();
    for (auto it = sFlashes.begin(); it != sFlashes.end();)
    {
        if (now < it->until && !it->view.isDead())
        {
            ++it;
            continue;
        }
        if (auto* searchable = dynamic_cast<ll::ui::SearchableControl*>(it->view.get()))
            searchable->setHighlighted(false);
        it = sFlashes.erase(it);
    }
}
