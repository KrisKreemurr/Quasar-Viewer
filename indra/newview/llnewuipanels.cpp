// #703 The new interface's docked panels (see llnewuipanels.h).
#include "llviewerprecompiledheaders.h"
#include "llnewuipanels.h"

#include "llagent.h"
#include "llavataractions.h"
#include "llavatarnamecache.h"
#include "llcallingcard.h"
#include "llfloaterimnearbychat.h"
#include "llimview.h"
#include "llinventoryrows.h"
#include "llnearbychatlog.h"
#include "llworld.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <algorithm>
#include <memory>
#include <new>

namespace
{
constexpr int kPool  = 40; // row slots per list: the most a full-height panel shows
constexpr int kRowDp = 22;

struct PRow
{
    std::string name, sub;
    bool        online = false, used = false;
};
struct Msg
{
    std::string from, text, time;
};
struct IRow
{
    std::string label, icon, twist;
    int         indent = 0;
    bool        used = false, selected = false, worn = false;
};
struct Person
{
    LLUUID      id, session;
    std::string name, sub;
    bool        online = false;
    F32         dist   = 0.f;
};

struct State
{
    Rml::Context*         context = nullptr;
    Rml::ElementDocument* people  = nullptr;
    Rml::ElementDocument* inv     = nullptr;
    Rml::DataModelHandle  pm, im;
    // people model
    bool                collapsed = false, chatOpen = false;
    int                 view = 0, count = 0, first = 0, fit = 20;
    std::string         countLabel, chatTitle, input;
    std::vector<PRow>   rows;
    std::vector<Msg>    msgs;
    std::vector<Person> friends, nearby, chats;
    LLUUID              chatSession; // null = nearby chat
    bool                chatIsNearby = true;
    uint64_t            chatRevision = ~0ull;
    F64                 nextPeople = 0, nextChats = 0;
    // inventory model
    bool                             invCollapsed = false;
    int                              invFirst = 0, invFit = 30;
    std::string                      filter, lastFilter;
    std::vector<IRow>                irows;
    std::unique_ptr<LLInventoryRows> tree;
    LLUUID                           selected;
    bool                             invDirty = true;
    // layout
    int  top = -1, bottom = -1;
    bool changed = true;
};
State* gState = nullptr;

const std::vector<Person>& currentList(State* s)
{
    return s->view == 0 ? s->friends : s->view == 1 ? s->nearby : s->chats;
}

std::string nameOf(const LLUUID& id)
{
    LLAvatarName name;
    return LLAvatarNameCache::get(id, &name) ? name.getCompleteName() : std::string("Loading...");
}

void refreshFriends(State* s)
{
    LLAvatarTracker::buddy_map_t buddies;
    LLAvatarTracker::instance().copyBuddyList(buddies);
    std::vector<Person> out;
    out.reserve(buddies.size());
    for (const auto& [id, rel] : buddies)
    {
        Person p;
        p.id     = id;
        p.name   = nameOf(id);
        p.online = rel && rel->isOnline();
        p.sub    = p.online ? "online" : "";
        out.push_back(std::move(p));
    }
    // Online first, then by name.
    std::sort(out.begin(), out.end(), [](const Person& a, const Person& b)
              { return a.online != b.online ? a.online : LLStringUtil::compareDict(a.name, b.name) < 0; });
    s->friends.swap(out);
}

void refreshNearby(State* s)
{
    uuid_vec_t               ids;
    std::vector<LLVector3d>  positions;
    const LLVector3d         me = gAgent.getPositionGlobal();
    LLWorld::getInstance()->getAvatars(&ids, &positions, me, 96.f);
    std::vector<Person> out;
    for (size_t i = 0; i < ids.size() && out.size() < 512; ++i)
    {
        if (ids[i] == gAgentID)
            continue;
        Person p;
        p.id   = ids[i];
        p.name = nameOf(ids[i]);
        p.dist = (F32)dist_vec(positions[i], me);
        p.sub  = llformat("%.0f m", p.dist);
        out.push_back(std::move(p));
    }
    std::sort(out.begin(), out.end(), [](const Person& a, const Person& b) { return a.dist < b.dist; });
    s->nearby.swap(out);
}

void refreshChats(State* s)
{
    std::vector<Person> out;
    Person nearby;
    nearby.name = "Nearby chat";
    nearby.sub  = llformat("%u", (unsigned)LLNearbyChatLog::instance().size());
    out.push_back(nearby);
    for (const auto& [id, session] : LLIMModel::instance().mId2SessionMap)
    {
        if (!session || out.size() >= 64)
            continue;
        Person p;
        p.session = id;
        p.id      = session->mOtherParticipantID;
        p.name    = session->mName;
        p.sub     = llformat("%u", (unsigned)session->mMsgs.size());
        out.push_back(std::move(p));
    }
    s->chats.swap(out);
}

// The visible window of the current list into the row slots.
void fillRows(State* s)
{
    const std::vector<Person>& list = currentList(s);
    s->count      = int(list.size());
    s->countLabel = s->view == 0 ? "friends" : s->view == 1 ? "nearby" : "conversations";
    s->first      = std::clamp(s->first, 0, std::max(0, s->count - s->fit));
    for (int i = 0; i < kPool; ++i)
    {
        PRow&      r   = s->rows[size_t(i)];
        const int  idx = s->first + i;
        const bool use = i < s->fit && idx < s->count;
        r.used         = use;
        if (use)
        {
            const Person& p = list[size_t(idx)];
            r.name          = p.name;
            r.sub           = p.sub;
            r.online        = p.online;
        }
    }
    s->pm.DirtyVariable("rows");
    s->pm.DirtyVariable("count");
    s->pm.DirtyVariable("countLabel");
    s->changed = true;
}

void refreshMessages(State* s, bool force)
{
    if (!s->chatOpen)
        return;
    std::vector<Msg> out;
    uint64_t         revision = 0;
    if (s->chatIsNearby)
    {
        const LLNearbyChatLog& log = LLNearbyChatLog::instance();
        revision                   = log.revision();
        if (!force && revision == s->chatRevision)
            return;
        const size_t n = log.size(), from = n > 40 ? n - 40 : 0;
        for (size_t i = from; i < n; ++i)
            out.push_back({ log.at(i).from, log.at(i).text, log.at(i).time });
    }
    else
    {
        LLIMModel::LLIMSession* session = LLIMModel::instance().findIMSession(s->chatSession);
        if (!session)
        {
            s->chatOpen = false;
            s->pm.DirtyVariable("chatOpen");
            s->changed = true;
            return;
        }
        revision = session->mMsgs.size();
        if (!force && revision == s->chatRevision)
            return;
        std::vector<const LLIMModel::LLIMMessage*> sorted;
        for (const auto& m : session->mMsgs)
            sorted.push_back(&m);
        std::sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) { return a->mIndex < b->mIndex; });
        const size_t from = sorted.size() > 40 ? sorted.size() - 40 : 0;
        for (size_t i = from; i < sorted.size(); ++i)
            out.push_back({ sorted[i]->mFrom, sorted[i]->mText, sorted[i]->mTime });
    }
    s->chatRevision = revision;
    s->msgs.swap(out);
    s->pm.DirtyVariable("msgs");
    s->changed = true;
}

void openChat(State* s, const LLUUID& session, bool nearby, const std::string& title)
{
    s->chatSession  = session;
    s->chatIsNearby = nearby;
    s->chatTitle    = title;
    s->chatOpen     = true;
    s->chatRevision = ~0ull;
    s->pm.DirtyVariable("chatOpen");
    s->pm.DirtyVariable("chatTitle");
    refreshMessages(s, true);
}

void pick(State* s, int slot)
{
    const std::vector<Person>& list = currentList(s);
    const size_t               idx  = size_t(s->first + slot);
    if (idx >= list.size())
        return;
    const Person p = list[idx];
    if (s->view == 2)
        openChat(s, p.session, p.session.isNull(), p.name);
    else if (gIMMgr)
    {
        // A conversation with them, shown here (the session is the IM model's own).
        const LLUUID session = gIMMgr->addSession(p.name, IM_NOTHING_SPECIAL, p.id);
        if (session.notNull())
            openChat(s, session, false, p.name);
    }
}

void fillInventory(State* s)
{
    const auto& rows = s->tree->rows();
    s->invFirst      = std::clamp(s->invFirst, 0, std::max(0, int(rows.size()) - s->invFit));
    for (int i = 0; i < kPool; ++i)
    {
        IRow&      r   = s->irows[size_t(i)];
        const int  idx = s->invFirst + i;
        const bool use = i < s->invFit && idx < int(rows.size());
        r.used         = use;
        if (!use)
            continue;
        const LLInventoryRows::Row& src = rows[size_t(idx)];
        r.label                         = src.name + (src.worn ? "  (worn)" : "");
        r.icon                          = src.icon;
        r.indent                        = src.depth * 12;
        r.twist                         = src.folder ? (src.open ? "\xe2\x96\xbe" : "\xe2\x96\xb8") : "";
        r.selected                      = src.id == s->selected;
        r.worn                          = src.worn;
    }
    s->im.DirtyVariable("irows");
    s->changed = true;
}

const char* kPeopleRml = R"rml(<rml><head><title>people</title><style>
body { position: absolute; left: 0; top: 0; right: 0; bottom: 0; pointer-events: none; }
#chat { flex: 1 1 0; display: flex; flex-direction: column; gap: 4dp; border-top: 1dp $border; padding-top: 6dp; }
#chatBox { height: 26dp; line-height: 26dp; }
</style></head><body><div data-model="people">
<div id="strip" class="strip left" data-if="collapsed" data-event-click="toggle()"></div>
<div id="dock" class="dock left panel" data-if="!collapsed">
  <div class="head"><div class="title">People</div><div class="spacer"></div><div class="btn" data-event-click="toggle()">&lt;</div></div>
  <div class="segmented">
    <div class="seg" data-class-on="view == 0" data-event-click="setView(0)">Friends</div>
    <div class="seg" data-class-on="view == 1" data-event-click="setView(1)">Nearby</div>
    <div class="seg" data-class-on="view == 2" data-event-click="setView(2)">Chats</div>
  </div>
  <div class="viewport" data-event-mousescroll="scroll(ev.wheel_delta_y)">
    <div class="vrow" data-for="r : rows" data-if="r.used" data-style-top="it_index * 22 + 'dp'"
         data-event-click="pick(it_index)" data-event-dblclick="profile(it_index)">
      <div class="dot" data-class-on="r.online"></div><div class="label">{{r.name}}</div><div class="path">{{r.sub}}</div>
    </div>
  </div>
  <div class="caption">{{count}} {{countLabel}}</div>
  <div id="chat" data-if="chatOpen">
    <div class="head"><div class="title">{{chatTitle}}</div><div class="spacer"></div><div class="btn" data-event-click="closeChat()">x</div></div>
    <div class="messages">
      <div class="msg" data-for="m : msgs"><span class="time">{{m.time}} </span><span class="from">{{m.from}}</span> {{m.text}}</div>
    </div>
    <div id="chatBox" class="field"><input id="chatInput" class="bare" type="text" data-value="input" placeholder="Say something"/></div>
  </div>
</div></div></body></rml>)rml";

const char* kInventoryRml = R"rml(<rml><head><title>inventory</title><style>
body { position: absolute; left: 0; top: 0; right: 0; bottom: 0; pointer-events: none; }
#filterBox { height: 24dp; line-height: 24dp; }
</style></head><body><div data-model="inv">
<div id="strip" class="strip right" data-if="collapsed" data-event-click="toggle()"></div>
<div id="dock" class="dock right panel" data-if="!collapsed">
  <div class="head"><div class="btn" data-event-click="toggle()">&gt;</div><div class="title">Inventory</div></div>
  <div id="filterBox" class="field"><input id="filter" class="bare" type="text" data-value="filter" placeholder="Search inventory"/></div>
  <div class="viewport" data-event-mousescroll="scroll(ev.wheel_delta_y)">
    <div class="vrow" data-for="r : irows" data-if="r.used" data-class-selected="r.selected" data-style-top="it_index * 22 + 'dp'"
         data-event-click="click(it_index)" data-event-dblclick="open(it_index)">
      <div data-style-width="r.indent + 'dp'" data-style-flex="'0 0 ' + r.indent + 'dp'"></div>
      <div class="twist">{{r.twist}}</div>
      <div class="ico" data-style-decorator="'llimage(' + r.icon + ')'"></div>
      <div class="label">{{r.label}}</div>
    </div>
  </div>
  <div class="caption">Double-click: wear / open</div>
</div></div></body></rml>)rml";
} // namespace

LLNewUIPanels& LLNewUIPanels::instance()
{
    static LLNewUIPanels panels;
    return panels;
}

bool LLNewUIPanels::active() const { return gState != nullptr; }

bool LLNewUIPanels::start(Rml::Context* context, const DocumentLoader& load)
{
    if (gState || !context)
        return gState != nullptr;
    State* s = new (std::nothrow) State();
    if (!s)
        return false;
    try
    {
        s->context = context;
        s->rows.resize(kPool);
        s->irows.resize(kPool);
        s->tree = std::make_unique<LLInventoryRows>();

        Rml::DataModelConstructor p = context->CreateDataModel("people");
        if (!p)
            throw std::bad_alloc();
        p.Bind("collapsed", &s->collapsed);
        p.Bind("view", &s->view);
        p.Bind("count", &s->count);
        p.Bind("countLabel", &s->countLabel);
        p.Bind("chatOpen", &s->chatOpen);
        p.Bind("chatTitle", &s->chatTitle);
        p.Bind("input", &s->input);
        if (auto r = p.RegisterStruct<PRow>())
        {
            r.RegisterMember("name", &PRow::name);
            r.RegisterMember("sub", &PRow::sub);
            r.RegisterMember("online", &PRow::online);
            r.RegisterMember("used", &PRow::used);
        }
        p.RegisterArray<std::vector<PRow>>();
        p.Bind("rows", &s->rows);
        if (auto m = p.RegisterStruct<Msg>())
        {
            m.RegisterMember("from", &Msg::from);
            m.RegisterMember("text", &Msg::text);
            m.RegisterMember("time", &Msg::time);
        }
        p.RegisterArray<std::vector<Msg>>();
        p.Bind("msgs", &s->msgs);
        p.BindEventCallback("toggle", [](Rml::DataModelHandle h, Rml::Event&, const Rml::VariantList&)
                            { gState->collapsed = !gState->collapsed; h.DirtyVariable("collapsed"); gState->changed = true; });
        p.BindEventCallback("setView", [](Rml::DataModelHandle h, Rml::Event&, const Rml::VariantList& a)
                            {
                                if (a.empty()) return;
                                gState->view  = std::clamp(a[0].Get<int>(), 0, 2);
                                gState->first = 0;
                                h.DirtyVariable("view");
                                fillRows(gState);
                            });
        p.BindEventCallback("scroll", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                            {
                                if (a.empty()) return;
                                gState->first += a[0].Get<float>() > 0 ? 3 : -3;
                                fillRows(gState);
                            });
        p.BindEventCallback("pick", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                            { if (!a.empty()) pick(gState, a[0].Get<int>()); });
        p.BindEventCallback("profile", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                            {
                                if (a.empty()) return;
                                const auto& list = currentList(gState);
                                const size_t i = size_t(gState->first + a[0].Get<int>());
                                if (i < list.size() && list[i].id.notNull())
                                    LLAvatarActions::showProfile(list[i].id);
                            });
        p.BindEventCallback("closeChat", [](Rml::DataModelHandle h, Rml::Event&, const Rml::VariantList&)
                            { gState->chatOpen = false; h.DirtyVariable("chatOpen"); gState->changed = true; });
        s->pm = p.GetModelHandle();

        Rml::DataModelConstructor i = context->CreateDataModel("inv");
        if (!i)
            throw std::bad_alloc();
        i.Bind("collapsed", &s->invCollapsed);
        i.Bind("filter", &s->filter);
        if (auto r = i.RegisterStruct<IRow>())
        {
            r.RegisterMember("label", &IRow::label);
            r.RegisterMember("icon", &IRow::icon);
            r.RegisterMember("twist", &IRow::twist);
            r.RegisterMember("indent", &IRow::indent);
            r.RegisterMember("used", &IRow::used);
            r.RegisterMember("selected", &IRow::selected);
            r.RegisterMember("worn", &IRow::worn);
        }
        i.RegisterArray<std::vector<IRow>>();
        i.Bind("irows", &s->irows);
        i.BindEventCallback("toggle", [](Rml::DataModelHandle h, Rml::Event&, const Rml::VariantList&)
                            { gState->invCollapsed = !gState->invCollapsed; h.DirtyVariable("collapsed"); gState->changed = true; });
        i.BindEventCallback("scroll", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                            {
                                if (a.empty()) return;
                                gState->invFirst += a[0].Get<float>() > 0 ? 3 : -3;
                                fillInventory(gState);
                            });
        i.BindEventCallback("click", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                            {
                                if (a.empty()) return;
                                const auto& rows = gState->tree->rows();
                                const size_t idx = size_t(gState->invFirst + a[0].Get<int>());
                                if (idx >= rows.size()) return;
                                gState->selected = rows[idx].id;
                                if (rows[idx].folder)
                                    gState->tree->toggleFolder(rows[idx].id);
                                fillInventory(gState);
                            });
        i.BindEventCallback("open", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                            {
                                if (a.empty()) return;
                                const auto& rows = gState->tree->rows();
                                const size_t idx = size_t(gState->invFirst + a[0].Get<int>());
                                if (idx < rows.size() && !rows[idx].folder)
                                    LLInventoryRows::activate(rows[idx].id);
                            });
        s->im = i.GetModelHandle();
    }
    catch (const std::bad_alloc&)
    {
        delete s;
        return false;
    }
    gState    = s;
    s->people = load(kPeopleRml, "newui-people.rml");
    s->inv    = load(kInventoryRml, "newui-inventory.rml");
    if (!s->people || !s->inv)
    {
        stop();
        return false;
    }
    s->people->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    s->inv->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    LL_INFOS("RmlUi") << "New interface: docked panels on (people / chat left, inventory right)" << LL_ENDL;
    return true;
}

void LLNewUIPanels::stop()
{
    State* s = gState;
    if (!s)
        return;
    if (s->people)
        s->people->Close();
    if (s->inv)
        s->inv->Close();
    if (s->context)
    {
        s->context->RemoveDataModel("people");
        s->context->RemoveDataModel("inv");
    }
    delete s;
    gState = nullptr;
}

bool LLNewUIPanels::ownsDocument(const Rml::ElementDocument* doc) const
{
    return gState && doc && (doc == gState->people || doc == gState->inv);
}

bool LLNewUIPanels::sync(int top, int bottom, bool glass)
{
    State* s = gState;
    if (!s)
        return false;
    // Extent: from under the bar to the bottom edge; the lists show what fits.
    if (top != s->top || bottom != s->bottom)
    {
        s->top    = top;
        s->bottom = bottom;
        for (Rml::ElementDocument* doc : { s->people, s->inv })
            for (const char* id : { "dock", "strip" })
                if (Rml::Element* e = doc->GetElementById(id))
                {
                    e->SetProperty("top", std::to_string(top) + "dp");
                    e->SetProperty("bottom", std::to_string(bottom) + "dp");
                }
        const S32 rootH  = LLUI::getInstance()->getRootView()->getRect().getHeight();
        const int height = std::max(0, rootH - top - bottom);
        s->fit           = std::clamp((height - 120) / kRowDp, 4, kPool);
        s->invFit        = std::clamp((height - 100) / kRowDp, 4, kPool);
        fillRows(s);
        fillInventory(s);
    }
    for (Rml::ElementDocument* doc : { s->people, s->inv })
        doc->SetClass("glass", glass);
    // The people list follows its models at 1 Hz, conversations at 4 Hz, only while shown.
    const F64 now = LLTimer::getTotalSeconds();
    if (!s->collapsed && now >= s->nextPeople)
    {
        s->nextPeople = now + 1.0;
        if (s->view == 0)
            refreshFriends(s);
        else if (s->view == 1)
            refreshNearby(s);
        fillRows(s);
    }
    if (!s->collapsed && now >= s->nextChats)
    {
        s->nextChats = now + 0.25;
        if (s->view == 2)
        {
            refreshChats(s);
            fillRows(s);
        }
        refreshMessages(s, false);
    }
    // Inventory: rebuilt on model / folder / filter changes only.
    if (!s->invCollapsed)
    {
        if (s->filter != s->lastFilter)
        {
            s->lastFilter = s->filter;
            s->tree->setFilter(s->filter);
            s->invFirst = 0;
        }
        if (s->tree->refresh())
            fillInventory(s);
    }
    const bool changed = s->changed;
    s->changed         = false;
    return changed;
}

bool LLNewUIPanels::onReturn(Rml::Element* focused)
{
    State* s = gState;
    if (!s || !focused || focused->GetId() != "chatInput" || s->input.empty())
        return false;
    const std::string text = s->input;
    if (s->chatIsNearby)
        LLFloaterIMNearbyChat::sendChatFromViewer(text, CHAT_TYPE_NORMAL, true);
    else if (LLIMModel::LLIMSession* session = LLIMModel::instance().findIMSession(s->chatSession))
        LLIMModel::sendMessage(text, s->chatSession, session->mOtherParticipantID, session->mType);
    s->input.clear();
    s->pm.DirtyVariable("input");
    s->changed = true;
    return true;
}
