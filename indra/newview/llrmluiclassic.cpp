// #703 UI on RmlUi: CLASSIC mode presenter (see llrmluiclassic.h).
#include "llviewerprecompiledheaders.h"
#include "llrmluiclassic.h"

#include "llrmluiatlas.h"
#include "llrmluiaccess.h"

#include "llaccordionctrl.h"
#include "llaccordionctrltab.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcolorswatch.h"
#include "lldraghandle.h"
#include "llfloater.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llfontregistry.h"
#include "lliconctrl.h"
#include "lllayoutstack.h"
#include "lllineeditor.h"
#include "llpanel.h"
#include "llresizehandle.h"
#include "llscrollbar.h"
#include "llscrollcontainer.h"
#include "lltexteditor.h"
#include "llscrolllistcell.h"
#include "llscrolllistcolumn.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llprogressbar.h"
#include "llsearcheditor.h"
#include "llslider.h"
#include "lltabcontainer.h"
#include "llstyle.h"
#include "lltextbase.h"
#include "lltextbox.h"
#include "llkeyboard.h"
#include "llnavigationbar.h"
#include "llrand.h"
#include "llfloaterimcontainer.h"
#include "llfloaterpreference.h"
#include "llfloaterpreferencesgraphicsadvanced.h"
#include "llinventorypanel.h"
#include "llpanelmaininventory.h"
#include "llfavoritesbar.h"
#include "llchiclet.h"
#include "lllocationinputctrl.h"
#include "llavatarlist.h"
#include "llprofileimagectrl.h"
#include "llwindow.h"
#include "llflatlistview.h"
#include "llloadingindicator.h"
#include "llmediactrl.h"
#include "lltexturectrl.h"
#include "llviewertexture.h"
#include "vkgfx/llrmluivk.h"
#include "llrender.h"
#include "vkgfx/vk_ui_texture_host.h" // slUIMarkExternalLayer / slUIBatchCount
#include "llstatusbar.h"
#include "lltoolbar.h"
#include "lltoolbarview.h"
#include "llui.h"
#include "llviewborder.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/StringUtilities.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <new>
#include <typeinfo>
#include <unordered_map>
#include <vector>

extern void (*gFloaterChangeListener)(LLFloater*); // llfloater.cpp

namespace
{
const char* const kDrawClasses[] = {
#include "llrmluiclassic_drawlist.inc"
};

// Classes that define draw() and have a presenter here (exact dynamic class match).
const char* const kPresented[] = {
    "LLAccordionCtrl", "LLAccordionCtrlTab", "LLAccordionCtrlTabHeader", "LLAvatarList", "LLButton", "LLChicletPanel",
    "LLColorSwatchCtrl", "LLDragHandleTop", "LLFavoritesBarCtrl", "LLFlatListView", "LLFloater",
    "LLFloaterIMContainer", "LLFloaterPreference", "LLFloaterPreferenceGraphicsAdvanced", "LLIconCtrl",
    "LLInventoryPanel", "LLLayoutStack", "LLLineEditor", "LLLoadingIndicator", "LLLocationInputCtrl", "LLMediaCtrl",
    "LLNavigationBar", "LLPanel", "LLPanelMainInventory", "LLPanelPreferenceGraphics", "LLProfileImageCtrl",
    "LLProgressBar", "LLResizeHandle", "LLScrollColumnHeader", "LLScrollContainer", "LLScrollListCtrl", "LLScrollbar",
    "LLSearchEditor", "LLSlider", "LLStatusBar", "LLTabContainer", "LLTextBase", "LLTextEditor", "LLTextureCtrl",
    "LLToolBar", "LLToolBarView", "LLView", "LLViewBorder"
};

std::string className(const LLView* v)
{
    // GCC/Clang: "12LLFloaterMap"; MSVC: "class LLFloaterMap". Namespaced names stay mangled
    // and are simply never found in the lists (=> not presented: the safe answer).
    const char* n = typeid(*v).name();
    if (std::strncmp(n, "class ", 6) == 0)
    {
        // MSVC: "class Outer::Inner" -> the innermost name
        n += 6;
        if (const char* colon = std::strrchr(n, ':'))
            n = colon + 1;
        return n;
    }
    // Itanium: "12LLFloaterMap", nested "N5Outer5InnerE" -> the innermost name, so a nested
    // class that defines draw() is found in the list too (and not mistaken for a plain view).
    const bool nested = *n == 'N';
    if (nested)
        ++n;
    std::string last;
    while (*n >= '0' && *n <= '9')
    {
        const long len = std::strtol(n, const_cast<char**>(&n), 10);
        if (len <= 0 || std::strlen(n) < size_t(len))
            return std::string(typeid(*v).name());
        last.assign(n, size_t(len));
        n += len;
        if (!nested)
            break;
    }
    return last.empty() ? std::string(typeid(*v).name()) : last;
}
bool inList(const char* const* first, const char* const* last, const std::string& name)
{
    auto it = std::lower_bound(first, last, name, [](const char* a, const std::string& b) { return std::strcmp(a, b.c_str()) < 0; });
    return it != last && name == *it;
}

std::string hex(const LLColor4& c)
{
    char buf[16];
    auto b = [](F32 v) { return unsigned(llclamp(ll_round(v * 255.f), 0, 255)); };
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", b(c.mV[0]), b(c.mV[1]), b(c.mV[2]), b(c.mV[3]));
    return buf;
}
std::string dp(S32 v) { return std::to_string(v) + "dp"; }
std::string dpf(F32 v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2fdp", v);
    return buf;
}

} // namespace

namespace sl_rmlui
{
struct Node
{
    LLHandle<LLView> handle;
    LLView*          view    = nullptr;
    Rml::Element*    element = nullptr;
    Rml::Element*    content = nullptr; // the view's own drawing (text, overlay), under its children
    std::string      inner;             // last content RML
    std::vector<std::pair<const char*, std::string>> props;   // element: last values
    std::vector<std::pair<const char*, std::string>> cprops;  // content: last values
    bool             unsupported = false; // no presenter: must stay hidden (else the LLView renderer)
    Rml::Element*    clip = nullptr;      // LLScrollContainer: the clip box its scrolled view sits in
    S32              clipLeft = 0, clipTop = 0; // that box's offset (UI units) in this view
    bool             inClip = false;      // this view is a scroll container's scrolled view
    std::string      why;
};

struct Root
{
    LLHandle<LLView>  handle;
    Rml::Element*     element = nullptr;
    std::vector<Node> nodes;      // pre-order; nodes[0] is the root view
    std::vector<int>  parents;    // index of each node's parent node (-1 for the root)
    std::string       blocker;    // why it is not presented ("" = presentable)
    bool              dynamicBlock = false;
    bool              animating = false; // a view that changes every frame (loading spinner): re-mirror every frame
    std::string       dynamicWhy;   // last frame-local reason (coverage report)
    bool              dirty = true;   // reported a change since it was last mirrored
    F64               nextRefresh = 0; // staggered safety re-mirror (changes that report nothing)
    LLRect            lastScreen;
    U32               seenFrame = 0, order = 0, checkedFrame = 0;
    F64               unseenSince = 0; // not drawn at all (hidden / closed) since
    U32               drawnFrame = 0;  // last frame the hook saw it (presented or on LLView)
    F64               recheckAt  = 0;  // static blocker: next analysis
};
} // namespace sl_rmlui
using namespace sl_rmlui;

namespace
{
} // namespace



namespace
{
struct State
{
    Rml::Context*                     context  = nullptr;
    Rml::ElementDocument*             document = nullptr;
    Rml::Element*                     body     = nullptr;
    std::unordered_map<LLView*, Root> roots;
    U32                               frame = 0, order = 0, lastDrawFrame = 0;
    bool                              changed = false; // the document changed since the last sync()
    double                            presentMs = 0;   // this frame's presentation time (draw hook)
    static constexpr uint32_t         kMaxRuns = 8;        // = LLRenderVK::kExternalMarks = backend segments
    uint32_t                          runs = 0;            // contiguous presented runs this frame
    std::vector<Rml::Element*>        markers;             // segment markers for runs 1..kMaxRuns-1
    std::vector<int>                  markerZ;
    size_t                            batchesAtLayer = 0;  // LLView batch count at the RmlUi layer's mark
    LLView*                           navBar = nullptr;    // looked up once per frame (singleton)
    // Viewer textures drawn through RmlUi ("llext:<id>-<view>"): held, touched every frame (LRU,
    // fetch priority), re-validated before every capture; bounded.
    struct External
    {
        LLPointer<LLViewerTexture> tex;
        VkImage                    image = VK_NULL_HANDLE;
        VkImageView                view  = VK_NULL_HANDLE;
        VkExtent2D                 extent{};
        F32                        area = 0;
        F64                        lastUsed = 0;
    };
    static constexpr size_t kMaxExternals = 64;
    std::unordered_map<std::string, External> externals;
    LLRmlUiClassic::Stats             stats;
    std::unordered_map<std::string, U32> blockers; // class -> roots it keeps on LLView (coverage)
    F64                               reportAt = 0;
};
State* gState = nullptr;

void set(std::vector<std::pair<const char*, std::string>>& cache, Rml::Element* el, const char* prop, const std::string& value,
         bool& changed)
{
    for (auto& p : cache)
        if (p.first == prop)
        {
            if (p.second != value)
            {
                p.second = value;
                el->SetProperty(prop, value);
                changed = true;
            }
            return;
        }
    cache.emplace_back(prop, value);
    el->SetProperty(prop, value);
    changed = true;
}
void setProp(Node& n, const char* prop, const std::string& value, bool& changed) { set(n.props, n.element, prop, value, changed); }

// The RmlUi source for a viewer texture drawn at `area` UI units^2, or "" when it cannot be
// (not uploaded yet, too many, allocation refused): the caller falls back to LLView.
std::string externalSource(LLViewerTexture* tex, F32 area);

// gl_rect_2d_checkerboard: the "Checker" image repeated every 32 units over `q` (view coords,
// view height h), in a clipping box. "" when the image cannot be atlased.
std::string checkerRml(const LLRect& q, S32 h, F32 a)
{
    if (!LLRmlUiAtlas::instance().find(std::string("Checker")) || q.getWidth() <= 0 || q.getHeight() <= 0)
        return {};
    std::string out = "<div style=\"overflow:hidden;left:" + dp(q.mLeft) + ";top:" + dp(h - q.mTop) + ";width:" + dp(q.getWidth()) +
                      ";height:" + dp(q.getHeight()) + "\">";
    const std::string tint = hex(LLColor4(1.f, 1.f, 1.f, a));
    // bottom-left anchored like the UV tiling (v grows upwards)
    for (S32 y = 0; y < q.getHeight(); y += 32)
        for (S32 x = 0; x < q.getWidth(); x += 32)
            out += "<div style=\"left:" + dp(x) + ";top:" + dp(q.getHeight() - y - 32) + ";width:32dp;height:32dp;decorator:llimage('Checker');image-color:" +
                   tint + "\"></div>";
    return out + "</div>";
}

bool isCandidate(LLView* v)
{
    // The floaters and the chrome (status bar, toolbars). Pointer compares first: the hook
    // sees every view drawn.
    if (v == gStatusBar || v == gToolBarView)
        return v != nullptr;
    if (gState && v == gState->navBar)
        return true;
    return gFloaterView && v->getParent() == gFloaterView && dynamic_cast<LLFloater*>(v);
}

bool analyse(Root& r, LLView* top)
{
    // Rebuild the node list. A view without a presenter is kept as a leaf: it is fine while
    // hidden (a closed combo list, an inactive tab), and blocks the whole top-level view
    // from RmlUi while it is in the visible chain (present() checks that every frame).
    r.nodes.clear();
    r.parents.clear();
    r.blocker.clear();
    std::vector<std::pair<LLView*, int>> stack{ { top, -1 } };
    while (!stack.empty())
    {
        auto [v, parent] = stack.back();
        stack.pop_back();
        std::string why;
        const bool  ok    = LLRmlUiClassicAccess::supportedClass(v, why);
        const int   index = int(r.nodes.size());
        Node        n;
        n.view        = v;
        n.handle      = v->getHandle();
        n.unsupported = !ok;
        n.why         = why;
        r.nodes.push_back(std::move(n));
        r.parents.push_back(parent);
        if (!ok)
        {
            if (v == top || v->isInVisibleChain())
            {
                r.blocker = why;
                return false;
            }
            continue;
        }
        // LLView draws children back to front (reverse child list): keep that DOM order.
        const LLView::child_list_t* children = v->getChildList();
        for (auto it = children->begin(); it != children->end(); ++it)
            stack.emplace_back(*it, index);
    }
    return true;
}

bool structureChanged(const Root& r)
{
    // Same views, same order (cheap pointer walk; unsupported leaves are not descended).
    size_t                     i = 0;
    std::vector<const LLView*> stack{ r.nodes.empty() ? nullptr : r.nodes[0].view };
    while (!stack.empty())
    {
        const LLView* v = stack.back();
        stack.pop_back();
        if (!v || i >= r.nodes.size() || r.nodes[i].view != v || r.nodes[i].handle.isDead())
            return true;
        const bool leaf = r.nodes[i].unsupported;
        ++i;
        if (leaf)
            continue;
        const LLView::child_list_t* children = v->getChildList();
        for (auto it = children->begin(); it != children->end(); ++it)
            stack.push_back(*it);
    }
    return i != r.nodes.size();
}

void destroyElements(Root& r)
{
    if (r.element && gState && gState->body)
    {
        gState->body->RemoveChild(r.element);
        gState->stats.destroyed += U32(r.nodes.size());
    }
    r.element = nullptr;
    for (Node& n : r.nodes)
    {
        n.element = n.content = n.clip = nullptr;
        n.props.clear();
        n.cprops.clear();
        n.inner.clear();
    }
}

bool buildElements(Root& r)
{
    if (!gState || !gState->document || r.nodes.empty())
        return false;
    for (size_t i = 0; i < r.nodes.size(); ++i)
    {
        Rml::ElementPtr el = gState->document->CreateElement("div");
        if (!el)
            return false;
        Rml::Element* parent = r.parents[i] < 0 ? gState->body : r.nodes[size_t(r.parents[i])].element;
        Node&         n      = r.nodes[i];
        if (r.parents[i] >= 0)
        {
            Node& pn = r.nodes[size_t(r.parents[i])];
            if (auto* sc = dynamic_cast<LLScrollContainer*>(pn.view); sc && n.view == LLRmlUiClassicAccess::scrolledView(sc))
            {
                if (!pn.clip)
                {
                    Rml::ElementPtr clip = gState->document->CreateElement("div");
                    if (!clip)
                        return false;
                    clip->SetProperty("overflow", "hidden");
                    pn.clip = pn.element->AppendChild(std::move(clip));
                }
                parent   = pn.clip;
                n.inClip = true;
            }
        }
        // Content first: LLView draws a view's own content, then its children over it.
        n.element            = parent->AppendChild(std::move(el));
        Rml::ElementPtr cont = gState->document->CreateElement("div");
        if (!cont)
            return false;
        cont->SetClass("c", true);
        n.content = n.element->AppendChild(std::move(cont));
        if (i == 0)
            r.element = n.element;
        ++gState->stats.created;
    }
    return true;
}

LLColor4 alpha(const LLColor4& c, F32 a)
{
    LLColor4 out = c;
    out.mV[VALPHA] *= a;
    return out;
}
} // namespace

LLRectf LLRmlUiClassicAccess::scaleRegion(const LLUIImage* image) { return image->mScaleRegion; }
LLView* LLRmlUiClassicAccess::scrolledView(const LLScrollContainer* container) { return container->mScrolledView; }

// ---------------------------------------------------------------------------------------------
bool LLRmlUiClassicAccess::supportedClass(LLView* v, std::string& why)
{
    const std::string name = className(v);
    if (inList(std::begin(kDrawClasses), std::end(kDrawClasses), name) && !inList(std::begin(kPresented), std::end(kPresented), name))
    {
        why = name;
        return false;
    }
    if (auto* t = dynamic_cast<LLTextBase*>(v))
        (void)t; // editable text with focus: presented with its caret (present())
    if (dynamic_cast<LLFloater*>(v) && static_cast<LLFloater*>(v)->isMinimized())
    {
        why = "LLFloater(minimized)";
        return false;
    }
    return true;
}

std::string LLRmlUiClassicAccess::fontStyle(const LLFontGL* font)
{
    // LLFontGL renders points at sVertDPI (96 x UI scale); dp already carries the scale.
    F32 pt = 8.6f;
    std::string family = "DejaVu Sans", weight = "normal", style = "normal";
    if (font)
    {
        const LLFontDescriptor& d = font->getFontDesc();
        F32 size = 0.f;
        if (LLFontGL::sFontRegistry && LLFontGL::sFontRegistry->nameToSize(d.getSize(), size))
            pt = size;
        if (d.getName().find("Mono") != std::string::npos)
            family = "DejaVu Sans Mono";
        if ((d.getStyle() & LLFontGL::BOLD) || d.getName().find("Bold") != std::string::npos)
            weight = "bold";
        if (d.getStyle() & LLFontGL::ITALIC)
            style = "italic";
    }
    const F32 px = pt * 96.f / 72.f;
    return "font-family:" + family + ";font-weight:" + weight + ";font-style:" + style + ";font-size:" + dpf(px) +
           ";line-height:" + dp(font ? font->getLineHeight() : S32(px * 1.2f)) + ";";
}

std::string LLRmlUiClassicAccess::textRml(LLTextBase* t, bool& ok)
{
    // One absolutely positioned div per visible laid-out line (LLTextBase's own wrapping),
    // one span per segment in the segment's style.
    t->reflow();
    std::string out;
    // Document rect in this view's coordinates (with a scroller it lives inside it).
    LLRect doc = t->mDocumentView->getRect();
    if (t->mScroller)
        t->mDocumentView->localRectToOtherView(t->mDocumentView->getLocalRect(), &doc, t);
    const S32    h   = t->getRect().getHeight();
    // Scrolling text is clipped to the scroller's content window (as LLTextBase::draw).
    LLRect window;
    S32    ox = 0, oy = 0;
    if (t->mScroller)
    {
        t->mScroller->localRectToOtherView(t->mScroller->getContentWindowRect(), &window, t);
        ox = window.mLeft;
        oy = h - window.mTop;
    }
    const F32    a   = t->getCurrentTransparency();
    const LLWString& text = t->getWText();
    // The selection box under the text (LLTextBase::drawSelectionBackground), in the same
    // document-to-view mapping as the lines.
    if (t->hasSelection() && !t->mLineInfoList.empty())
    {
        LLTextBase::highlight_list_t highlights;
        highlights.push_back(LLTextBase::range_pair_t(llmin(t->mSelectionStart, t->mSelectionEnd), llmax(t->mSelectionStart, t->mSelectionEnd)));
        LLColor4 c = t->mSelectedBGColor.get();
        c.mV[VALPHA] = (t->hasFocus() ? 0.7f : 0.3f) * t->getDrawContext().mAlpha;
        for (const LLRect& q : t->getSelectionRects(highlights))
            out += "<div style=\"left:" + dp(doc.mLeft + q.mLeft - ox) + ";top:" + dp(h - (doc.mBottom + q.mTop) - oy) + ";width:" +
                   dp(q.getWidth()) + ";height:" + dp(q.getHeight()) + ";background-color:" + hex(c) + "\"></div>";
    }
    auto [first, last] = t->getVisibleLines(t->mClipPartial);
    for (S32 li = first; li < last; ++li)
    {
        const auto& line = t->mLineInfoList[size_t(li)];
        const S32   left = doc.mLeft + line.mRect.mLeft - ox, top = h - (doc.mBottom + line.mRect.mTop) - oy;
        out += "<div style=\"left:" + dp(left) + ";top:" + dp(top) + ";width:" + dp(std::max(0, doc.getWidth() - line.mRect.mLeft)) +
               ";height:" + dp(line.mRect.getHeight()) + "\">";
        const S32 lineEnd = li + 1 < S32(t->mLineInfoList.size()) ? t->mLineInfoList[size_t(li + 1)].mDocIndexStart : S32(text.size());
        S32       pos     = line.mDocIndexStart;
        auto      seg     = t->getSegIterContaining(pos);
        while (pos < lineEnd && seg != t->mSegments.end())
        {
            LLTextSegment* s = *seg;
            const S32      e = std::min(lineEnd, s->getEnd());
            if (dynamic_cast<LLLineBreakTextSegment*>(s))
            {
            }
            else if (auto* n = dynamic_cast<LLNormalTextSegment*>(s); n && !dynamic_cast<LLEmojiTextSegment*>(s))
            {
                LLStyleConstSP st = n->getStyle();
                const LLColor4 c  = alpha(t->mReadOnly ? st->getReadOnlyColor().get() : st->getColor().get(), a * st->getAlpha());
                std::string    str = wstring_to_utf8str(text.substr(size_t(pos), size_t(std::max(0, e - pos))));
                LLStringUtil::replaceChar(str, '\n', ' ');
                out += "<span style=\"color:" + hex(c) + ";" + fontStyle(st->getFont()) +
                       (st->isLink() && !t->getSkipLinkUnderline() ? "text-decoration:underline;" : "") + "\">" +
                       Rml::StringUtilities::EncodeRml(str) + "</span>";
            }
            else
            {
                ok = false; // inline views, images, emoji: the LLView renderer
                return {};
            }
            pos = e;
            ++seg;
        }
        out += "</div>";
    }
    if (t->mScroller)
        out = "<div style=\"overflow:hidden;left:" + dp(window.mLeft) + ";top:" + dp(oy) + ";width:" + dp(window.getWidth()) +
              ";height:" + dp(window.getHeight()) + "\">" + out + "</div>";
    return out;
}

void LLRmlUiClassicAccess::present(Root& root, Node& n, bool& changed)
{
    LLView*    v      = n.view;
    const bool isRoot = &n == &root.nodes[0];
    // Box: LLView rects are y-up in parent space; RmlUi boxes are y-down.
    const LLRect r  = v->getRect();
    S32          ph = 0;
    if (!isRoot && v->getParent())
        ph = v->getParent()->getRect().getHeight();
    if (isRoot)
    {
        const LLRect screen = v->calcScreenRect();
        const S32    rootH  = LLUI::getInstance()->getRootView()->getRect().getHeight();
        setProp(n, "left", dp(screen.mLeft), changed);
        setProp(n, "top", dp(rootH - screen.mTop), changed);
    }
    else
    {
        S32 ox = 0, oy = 0;
        if (n.inClip)
        {
            const Node& pn = root.nodes[size_t(root.parents[size_t(&n - &root.nodes[0])])];
            ox = pn.clipLeft;
            oy = pn.clipTop;
        }
        setProp(n, "left", dp(r.mLeft - ox), changed);
        setProp(n, "top", dp(ph - r.mTop - oy), changed);
    }
    setProp(n, "width", dp(r.getWidth()), changed);
    setProp(n, "height", dp(r.getHeight()), changed);
    if (n.unsupported)
    {
        if (v->isInVisibleChain())
        {
            root.dynamicBlock = true;
            root.blocker      = n.why;
        }
        setProp(n, "display", "none", changed);
        return;
    }
    const bool shown = v->getVisible() && r.isValid();
    if (!isRoot) // the root's display is claimRoot's outcome (presented or not): no flip-flop
        setProp(n, "display", shown ? "block" : "none", changed);
    if (!shown)
        return;

    std::string decorator = "none", imageColor = "#ffffffff", background = "#00000000", content, contentStyle;
    std::string boxShadow = "none", border = "0dp", borderColor = "#00000000", overflow = "visible";
    auto image = [&](LLUIImage* img, const LLColor4& c) -> bool
    {
        if (!img)
            return true;
        if (!LLRmlUiAtlas::instance().find(img))
        {
            root.dynamicBlock = true;
            root.blocker      = "image:" + img->getName();
            return false;
        }
        decorator  = "llimage('" + img->getName() + "')";
        imageColor = hex(c);
        return true;
    };

    if (auto* f = dynamic_cast<LLFloater*>(v))
    {
        // Floater subclasses whose draw() is state work + LLFloater::draw.
        if (auto* imc = dynamic_cast<LLFloaterIMContainer*>(v))
            imc->updateForDraw(); // the title follows the active conversation
        else if (auto* pref = dynamic_cast<LLFloaterPreference*>(v))
            pref->updateForDraw(); // popup enable / disable buttons
        else if (auto* gfx = dynamic_cast<LLFloaterPreferenceGraphicsAdvanced*>(v))
            gfx->updateForDraw(); // VRAM / draw distance text, shader compile status
        if (f->isBackgroundVisible())
        {
            const F32 a   = f->getCurrentTransparency();
            const bool op = f->isBackgroundOpaque();
            LLUIImage* img = op ? f->getBackgroundImage().get() : f->getTransparentImage().get();
            if (img)
                image(img, alpha(op ? f->getBackgroundImageOverlay() : f->getTransparentImageOverlay(), a));
            else
                background = hex(alpha(op ? f->getBackgroundColor() : f->getTransparentColor(), a));
            static LLUIColor shadow = LLUIColorTable::instance().getColor("ColorDropShadow");
            static LLUICachedControl<S32> shadow_px("DropShadowFloater", 5);
            if (shadow_px > 0)
                boxShadow = hex(alpha(shadow.get(), a)) + " " + dp(shadow_px) + " " + dp(shadow_px) + " " + dp(shadow_px);
        }
        f->updateDefaultBtn();
    }
    else if (auto* ls = dynamic_cast<LLLayoutStack*>(v))
    {
        ls->updateLayout();
        overflow = "hidden";
    }
    else if (auto* acc = dynamic_cast<LLAccordionCtrl*>(v))
    {
        // LLAccordionCtrl::draw: auto-scroll bookkeeping, clip, LLPanel::draw.
        if (acc->mAutoScrolling)
        {
            root.dynamicBlock = true; // drag auto-scroll accelerates per drawn frame: LLView
            root.blocker      = "LLAccordionCtrl(auto-scroll)";
        }
        if (acc->mBgVisible)
            background = hex(alpha(acc->mBgOpaque ? acc->mBgOpaqueColor.get() : acc->mBgAlphaColor.get(), acc->getCurrentTransparency()));
        overflow = "hidden";
    }
    else if (auto* tab = dynamic_cast<LLAccordionCtrlTab*>(v))
    {
        // Not fitted: header, scrollbar and the container clipped below the header.
        if (!tab->mFitPanel)
            overflow = "hidden";
    }
    else if (auto* fav = dynamic_cast<LLFavoritesBarCtrl*>(v))
    {
        // LLFavoritesBarCtrl::draw: its buttons, a drag marker while dropping, then bookkeeping.
        if (fav->mShowDragMarker)
        {
            root.dynamicBlock = true; // the drop marker: LLView draws (and clears) it this frame
            root.blocker      = "LLFavoritesBarCtrl(drag)";
        }
        else
            fav->updateAfterDraw();
    }
    else if (auto* chp = dynamic_cast<LLChicletPanel*>(v))
    {
        // LLChicletPanel::draw: its children only (no panel background), the scroll area clipped.
        (void)chp;
    }
    else if (auto* loc = dynamic_cast<LLLocationInputCtrl*>(v))
    {
        // LLLocationInputCtrl::draw: refresh the location / parcel icons, then LLComboBox::draw
        // (which draws nothing of its own).
        loc->updateForDraw();
    }
    else if (auto* nav = dynamic_cast<LLNavigationBar*>(v))
    {
        // LLNavigationBar::draw = a drop shadow (not presented, as for floaters) + LLPanel::draw.
        if (nav->mBgVisible)
            background = hex(alpha(nav->mBgOpaque ? nav->mBgOpaqueColor.get() : nav->mBgAlphaColor.get(), nav->getCurrentTransparency()));
        nav->updateDefaultBtn();
    }
    else if (auto* sbar = dynamic_cast<LLStatusBar*>(v))
    {
        // LLStatusBar::draw = refresh() + LLPanel::draw.
        sbar->refresh();
        if (sbar->mBgVisible)
            background = hex(alpha(sbar->mBgOpaque ? sbar->mBgOpaqueColor.get() : sbar->mBgAlphaColor.get(), sbar->getCurrentTransparency()));
        sbar->updateDefaultBtn();
    }
    else if (auto* mc = dynamic_cast<LLMediaCtrl*>(v))
    {
        // LLMediaCtrl::draw: its state work (updateForDraw), the media texture sampled in place
        // (its used sub-rect, flipped for GL-origin plugins), the panel (forced opaque without media).
        LLMediaCtrl::MediaDraw md;
        if (!mc->updateForDraw(md))
        {
            root.dynamicBlock = true;
            root.blocker      = "LLMediaCtrl(reshape)";
        }
        else if (md.texture)
        {
            const std::string src = externalSource(md.texture, F32(md.width * md.height));
            auto              it  = src.empty() ? gState->externals.end() : gState->externals.find(src);
            if (it == gState->externals.end())
            {
                root.dynamicBlock = true;
                root.blocker      = "LLMediaCtrl(texture not ready or slots full)";
            }
            else
            {
                const U32 tw = U32(md.maxU * it->second.extent.width), th = U32(md.maxV * it->second.extent.height);
                content = "<img src=\"" + src + "\" rect=\"0 0 " + std::to_string(tw) + " " + std::to_string(th) +
                          "\" style=\"display:block;position:absolute;left:" + dp(md.x) + ";top:" + dp(r.getHeight() - (md.y + md.height)) +
                          ";width:" + dp(md.width) + ";height:" + dp(md.height) + ";image-color:" + hex(alpha(LLColor4::white, mc->getDrawContext().mAlpha)) +
                          (md.glCoords ? ";transform:scaleY(-1)" : "") + "\"/>";
            }
        }
        const bool opaque = !md.texture || mc->mBgOpaque;
        if (!md.texture || mc->mBgVisible)
            background = hex(alpha(opaque ? mc->mBgOpaqueColor.get() : mc->mBgAlphaColor.get(), mc->getCurrentTransparency()));
    }
    else if (auto* tc = dynamic_cast<LLTabContainer*>(v))
    {
        // LLTabContainer::draw without scrolling: tab buttons keep the positions and
        // visibility its layout gave them; scrolled tab rows stay on LLView for now.
        const bool arrows = !tc->mHideScrollArrows && !tc->getTabsHidden() && (tc->getMaxScrollPos() > 0 || tc->getScrollPosPixels() > 0);
        if (arrows || tc->getScrollPos() > 0 || tc->getScrollPosPixels() != 0)
        {
            root.dynamicBlock = true;
            root.blocker      = "LLTabContainer(scrolled tabs)";
        }
        for (LLButton* btn : { tc->mPrevArrowBtn, tc->mNextArrowBtn, tc->mJumpPrevArrowBtn, tc->mJumpNextArrowBtn })
            if (btn)
                btn->setVisible(false);
        if (tc->mBgVisible)
        {
            const F32 a = tc->getCurrentTransparency();
            if (tc->mBgOpaque)
                background = hex(alpha(tc->mBgOpaqueColor.get(), a));
            else
                background = hex(alpha(tc->mBgAlphaColor.get(), a));
        }
        overflow = "hidden";
    }
    else if (auto* p = dynamic_cast<LLPanel*>(v))
    {
        if (auto* mi = dynamic_cast<LLPanelMainInventory*>(v))
        {
            mi->updateForDraw();   // filter text, re-sort
            mi->updateAfterDraw(); // item count text, combination view visibility
        }
        else if (auto* ip = dynamic_cast<LLInventoryPanel*>(v))
            ip->updateForDraw(); // the pending selection
        else if (auto* pg = dynamic_cast<LLPanelPreferenceGraphics*>(v))
            pg->updateForDraw(); // the effects row follows its settings
        if (p->mBgVisible)
        {
            const F32 a = p->getCurrentTransparency();
            if (p->mBgOpaque)
            {
                if (p->mBgOpaqueImage.notNull())
                    image(p->mBgOpaqueImage, alpha(p->mBgOpaqueImageOverlay.get(), a));
                else
                    background = hex(alpha(p->mBgOpaqueColor.get(), a));
            }
            else if (p->mBgAlphaImage.notNull())
                image(p->mBgAlphaImage, alpha(p->mBgAlphaImageOverlay.get(), a));
            else
                background = hex(alpha(p->mBgAlphaColor.get(), a));
        }
        if (dynamic_cast<LLLayoutPanel*>(v))
            overflow = "hidden";
        p->updateDefaultBtn();
    }
    else if (auto* se = dynamic_cast<LLSearchEditor*>(v))
    {
        // LLSearchEditor::draw is logic only: clear button and the search field images.
        if (se->mClearButton)
            se->mClearButton->setVisible(!se->mSearchEditor->getWText().empty());
        if (se->mHighlightTextField)
        {
            const bool any = !se->mSearchEditor->getWText().empty();
            se->mSearchEditor->setBgImage(any ? se->mEditorSearchImage : se->mEditorImage);
            se->mSearchEditor->setBgImageFocused(any ? se->mEditorSearchImage : se->mEditorImageFocused);
        }
    }
    else if (auto* pb = dynamic_cast<LLProgressBar*>(v))
    {
        static LLTimer timer;
        F32 a = pb->getDrawContext().mAlpha;
        if (pb->mImageBar)
        {
            LLColor4 c = pb->mColorBackground.get();
            c.setAlpha(a);
            image(pb->mImageBar, c);
        }
        if (pb->mImageFill && LLRmlUiAtlas::instance().find(pb->mImageFill))
        {
            a *= 0.5f + 0.5f * 0.5f * (1.f + (F32)sin(3.f * timer.getElapsedTimeF32())); // the classic pulse
            LLColor4 c = pb->mColorBar.get();
            c.mV[VALPHA] *= a;
            content = "<div style=\"left:0dp;top:0dp;height:" + dp(r.getHeight()) + ";width:" + dp(ll_round(r.getWidth() * (pb->mPercentDone / 100.f))) +
                      ";decorator:llimage('" + pb->mImageFill->getName() + "');image-color:" + hex(c) + "\"></div>";
        }
        else if (pb->mImageFill)
        {
            root.dynamicBlock = true;
            root.blocker      = "image:" + pb->mImageFill->getName();
        }
    }
    else if (auto* tbv = dynamic_cast<LLToolBarView*>(v))
    {
        LLRect rects[LLToolBarEnums::TOOLBAR_COUNT];
        if (tbv->updateForDraw(rects))
        {
            root.dynamicBlock = true; // drop zones while dragging a tool or a pin
            root.blocker      = "LLToolBarView(drag)";
        }
    }
    else if (auto* tb = dynamic_cast<LLToolBar*>(v))
    {
        if (tb->updateForDraw())
        {
            root.dynamicBlock = true; // the drag-and-drop caret
            root.blocker      = "LLToolBar(drop caret)";
        }
        tb->mDragAndDropTarget = false; // as draw(): re-armed by the next drag-over
        if (tb->mCaretIcon)
            tb->mCaretIcon->setVisible(false);
    }
    else if (auto* sc = dynamic_cast<LLScrollContainer*>(v))
    {
        // LLScrollContainer::draw: scroll logic, the inner background, and the scrolled view
        // clipped to the visible area (its scrollbars and border are ordinary children).
        static LLUICachedControl<S32> scrollbar_size_control("UIScrollbarSize", 0);
        const S32 sbs = sc->mSize == -1 ? scrollbar_size_control : sc->mSize;
        if (!sc->mAutoScrolling)
            sc->mAutoScrollRate = sc->mMinAutoScrollRate;
        sc->mAutoScrolling = false;
        if (sc->mIsOpaque)
            content = "<div style=\"left:" + dp(sc->mInnerRect.mLeft) + ";top:" + dp(r.getHeight() - sc->mInnerRect.mTop) + ";width:" +
                      dp(sc->mInnerRect.getWidth()) + ";height:" + dp(sc->mInnerRect.getHeight()) + ";background-color:" +
                      hex(alpha(sc->mBackgroundColor.get(), sc->getCurrentTransparency())) + "\"></div>";
        LLRect clip;
        if (sc->mScrolledView)
        {
            sc->updateScroll();
            S32  vw = 0, vh = 0;
            bool showH = false, showV = false;
            sc->calcVisibleSize(&vw, &vh, &showH, &showV);
            clip = LLRect(sc->mInnerRect.mLeft, sc->mInnerRect.mBottom + (showH ? sbs : 0) + vh, sc->mInnerRect.mRight - (showV ? sbs : 0),
                          sc->mInnerRect.mBottom + (showH ? sbs : 0));
        }
        if (sc->mBorder && sc->mBorder->getVisible())
            sc->mBorder->setKeyboardFocusHighlight(gFocusMgr.childHasKeyboardFocus(sc));
        if (auto* al = dynamic_cast<LLAvatarList*>(v))
            al->updateAfterDraw(); // as draw() did after drawing its items
        if (auto* fl = dynamic_cast<LLFlatListView*>(v); fl && fl->mSelectedItemsBorder && fl->mSelectedItemsBorder->getVisible())
            fl->mSelectedItemsBorder->setKeyboardFocusHighlight(fl->hasFocus()); // LLFlatListView::draw
        n.clipLeft = clip.mLeft;
        n.clipTop  = r.getHeight() - clip.mTop;
        if (n.clip)
        {
            set(n.cprops, n.clip, "left", dp(n.clipLeft), changed);
            set(n.cprops, n.clip, "top", dp(n.clipTop), changed);
            set(n.cprops, n.clip, "width", dp(std::max(0, clip.getWidth())), changed);
            set(n.cprops, n.clip, "height", dp(std::max(0, clip.getHeight())), changed);
        }
    }
    else if (auto* sb = dynamic_cast<LLScrollbar*>(v))
    {
        if (sb->mBGVisible)
            background = hex(sb->mBGColor.get());
        const bool vert = sb->mOrientation == LLScrollbar::VERTICAL;
        LLUIImage* track = vert ? sb->mTrackImageV.get() : sb->mTrackImageH.get();
        LLUIImage* thumb = vert ? sb->mThumbImageV.get() : sb->mThumbImageH.get();
        auto box = [&](const LLRect& q, const std::string& style)
        {
            content += "<div style=\"left:" + dp(q.mLeft) + ";top:" + dp(r.getHeight() - q.mTop) + ";width:" + dp(q.getWidth()) +
                       ";height:" + dp(q.getHeight()) + ";" + style + "\"></div>";
        };
        const S32 t = sb->mThickness;
        const LLRect tr = vert ? LLRect(0, r.getHeight() - t, r.getWidth(), t) : LLRect(t, r.getHeight(), r.getWidth() - t, 0);
        if (!track || !thumb)
        {
            box(tr, "background-color:" + hex(sb->mTrackColor.get()));
            box(sb->mThumbRect, "background-color:" + hex(sb->mThumbColor.get()));
        }
        else if (LLRmlUiAtlas::instance().find(track) && LLRmlUiAtlas::instance().find(thumb))
        {
            box(tr, "decorator:llimage('" + track->getName() + "' solid);image-color:" + hex(sb->mTrackColor.get()));
            box(sb->mThumbRect, "decorator:llimage('" + thumb->getName() + "');image-color:" + hex(sb->mThumbColor.get()));
        }
        else
        {
            root.dynamicBlock = true;
            root.blocker      = "image:" + std::string(track ? track->getName() : "") + "/" + (thumb ? thumb->getName() : "");
        }
    }
    else if (auto* sl = dynamic_cast<LLScrollListCtrl*>(v))
    {
        // draw()'s logic first: sort, scroll-to-selection, column widths, comment visibility.
        sl->updateSort();
        if (sl->mNeedsScroll)
        {
            sl->scrollToShowSelected();
            sl->mNeedsScroll = false;
        }
        sl->updateColumns();
        if (sl->mCommentText)
            sl->mCommentText->setVisible(sl->mItemList.empty());
        if (sl->mBorder)
            sl->mBorder->setKeyboardFocusHighlight(sl->hasFocus());
        overflow = "hidden";
        const F32 a = sl->getCurrentTransparency();
        if (sl->mBackgroundVisible)
            background = hex(alpha(sl->getEnabled() ? sl->mBgWriteableColor.get() : sl->mBgReadOnlyColor.get(), a));
        const F32  da    = sl->getDrawContext().mAlpha;
        const LLRect& lr = sl->mItemListRect;
        content = "<div style=\"overflow:hidden;left:" + dp(lr.mLeft) + ";top:" + dp(r.getHeight() - lr.mTop) + ";width:" + dp(lr.getWidth()) +
                  ";height:" + dp(lr.getHeight()) + "\">";
        const S32 pageLines = sl->getLinesPerPage();
        const S32 first = sl->mScrollLines, last = std::min<S32>(S32(sl->mItemList.size()) - 1, first + pageLines);
        S32       y     = 0, max_columns = 0;
        for (S32 line = first; line <= last && line >= 0; ++line)
        {
            LLScrollListItem* item = sl->mItemList[size_t(line)];
            item->setRect(LLRect(lr.mLeft, lr.mTop - (line - first) * sl->mLineHeight, lr.mRight, lr.mTop - (line - first + 1) * sl->mLineHeight));
            max_columns = std::max(max_columns, item->getNumColumns());
            LLColor4 fg = item->getEnabled() ? sl->mFgUnselectedColor.get() : sl->mFgDisabledColor.get();
            LLColor4 hover(LLColor4::transparent), select(LLColor4::transparent);
            if (item->getSelected() && sl->mCanSelect)
            {
                select = item->getHighlighted() ? lerp(sl->mBgSelectedColor.get(), sl->mHighlightedColor.get(), 0.5f) : sl->mBgSelectedColor.get();
                fg     = item->getEnabled() ? sl->mFgSelectedColor.get() : sl->mFgDisabledColor.get();
            }
            if (sl->mHighlightedItem == line && sl->mCanSelect)
                hover = item->getHighlighted() ? lerp(sl->mHoveredColor.get(), sl->mHighlightedColor.get(), 0.5f) : sl->mHoveredColor.get();
            else if (item->getHighlighted())
                hover = sl->mHighlightedColor.get();
            else if (sl->mDrawStripes && line % 2 == 0 && max_columns > 1)
                hover = sl->mBgStripeColor.get();
            if (!item->getEnabled())
                hover = sl->mBgReadOnlyColor.get();
            const LLColor4 rowBg = item->mSelectedIndex < 0 && item->getSelected() ? select : item->mHoverIndex < 0 ? hover : LLColor4::transparent;
            content += "<div style=\"left:0dp;top:" + dp(y) + ";width:" + dp(lr.getWidth()) + ";height:" + dp(sl->mLineHeight) +
                       ";background-color:" + hex(alpha(rowBg, da)) + "\">";
            S32 x = 0;
            for (S32 col = 0; col < item->getNumColumns(); ++col)
            {
                LLScrollListCell* cell = item->getColumn(col);
                if (!cell || cell->getWidth() < 0 || !cell->getVisible())
                    continue;
                const S32 cw = cell->getWidth(), ch = cell->getHeight();
                std::string cellBg;
                if (item->mSelectedIndex == col)
                    cellBg = "background-color:" + hex(alpha(select, da)) + ";";
                else if (item->mHoverIndex == col)
                    cellBg = "background-color:" + hex(alpha(hover, da)) + ";";
                cell->setUseDefaultColor(item->getSelected() || !item->getEnabled());
                content += "<div style=\"left:" + dp(x) + ";top:" + dp(sl->mLineHeight - ch) + ";width:" + dp(cw) + ";height:" + dp(ch) + ";" + cellBg + "\">";
                if (auto* text = dynamic_cast<LLScrollListText*>(cell); text && typeid(*cell) == typeid(LLScrollListText))
                {
                    if (text->mHighlightCount > 0)
                    {
                        root.dynamicBlock = true; // type-ahead highlight: LLView for now
                        root.blocker      = "LLScrollListText(highlight)";
                    }
                    const LLColor4 c = alpha(text->mUseColor && !text->mUseDefaultColor ? text->mColor : fg, da);
                    const char* align = text->mFontAlignment == LLFontGL::RIGHT ? "right" : text->mFontAlignment == LLFontGL::HCENTER ? "center" : "left";
                    const S32 lh = text->mFont ? text->mFont->getLineHeight() : 12;
                    content += "<div style=\"white-space:pre;overflow:hidden;left:1dp;width:" + dp(std::max(0, cw - 1)) + ";top:" + dp(ch - lh) +
                               ";height:" + dp(lh) + ";text-align:" + align + ";color:" + hex(c) + ";" + fontStyle(text->mFont) + "\">" +
                               Rml::StringUtilities::EncodeRml(text->mText.getString()) + "</div>";
                }
                else if (auto* icon = dynamic_cast<LLScrollListIcon*>(cell); icon && icon->mIcon.notNull() && LLRmlUiAtlas::instance().find(icon->mIcon))
                {
                    const S32 iw = icon->mDrawSize > 0 ? icon->mDrawSize : icon->mIcon->getWidth();
                    const S32 ih = icon->mDrawSize > 0 ? icon->mDrawSize : icon->mIcon->getHeight();
                    content += "<div style=\"left:0dp;top:" + dp(ch - ih) + ";width:" + dp(iw) + ";height:" + dp(ih) + ";decorator:llimage('" + icon->mIcon->getName() + "');image-color:" + hex(alpha(icon->mColor, da)) + "\"></div>";
                }
                else if (dynamic_cast<LLScrollListSpacer*>(cell))
                {
                }
                else
                {
                    root.dynamicBlock = true;
                    root.blocker      = std::string("cell:") + typeid(*cell).name();
                }
                content += "</div>";
                x += cw + sl->mColumnPadding;
            }
            content += "</div>";
            y += sl->mLineHeight;
        }
        content += "</div>";
    }
    else if (LLAccordionCtrlTab::HeaderLook look; LLAccordionCtrlTab::headerLook(v, look))
    {
        background = hex(look.bg);
        auto quad = [&](LLUIImage* img, S32 x, S32 y, S32 w, S32 h, const LLColor4& c)
        {
            if (!img)
                return;
            if (!LLRmlUiAtlas::instance().find(img))
            {
                root.dynamicBlock = true;
                root.blocker      = "image:" + img->getName();
                return;
            }
            content += "<div style=\"left:" + dp(x) + ";top:" + dp(r.getHeight() - (y + h)) + ";width:" + dp(w) + ";height:" + dp(h) +
                       ";decorator:llimage('" + img->getName() + "');image-color:" + hex(c) + "\"></div>";
        };
        quad(look.image, 0, 0, r.getWidth(), r.getHeight(), LLColor4::white);
        quad(look.over, 0, 0, r.getWidth(), r.getHeight(), LLColor4::white);
        if (look.arrow)
            quad(look.arrow, look.arrowX, look.arrowY, look.arrow->getWidth(), look.arrow->getHeight(), LLColor4::white);
        if (look.icon)
            quad(look.icon, look.iconX, look.iconY, look.iconSize, look.iconSize, alpha(UI_VERTEX_COLOR, look.alpha));
    }
    else if (auto* tx = dynamic_cast<LLTextureCtrl*>(v))
    {
        // LLTextureCtrl::draw: its preview logic (updatePreview), outline, then the texture /
        // material preview (a viewer texture sampled in place), locked or fallback image.
        LLViewerTexture* preview = tx->updatePreview();
        const F32        a       = tx->getTransparencyType() == LLUICtrl::TT_ACTIVE ? 1.0f : tx->getCurrentTransparency();
        const LLRect     box(0, r.getHeight(), r.getWidth(), BTN_HEIGHT_SMALL);
        LLRect           in = box;
        in.stretch(-1);
        auto rect = [&](const LLRect& q, const std::string& style)
        {
            content += "<div style=\"left:" + dp(q.mLeft) + ";top:" + dp(r.getHeight() - q.mTop) + ";width:" + dp(std::max(0, q.getWidth())) +
                       ";height:" + dp(std::max(0, q.getHeight())) + ";" + style + "\"></div>";
        };
        auto block = [&](const std::string& why)
        {
            root.dynamicBlock = true;
            root.blocker      = why;
        };
        rect(box, "border-width:1dp;border-color:" + hex(tx->mBorderColor.get()));
        if (preview)
        {
            if (preview->getComponents() == 4)
            {
                const std::string checker = checkerRml(in, r.getHeight(), a);
                if (checker.empty())
                    block("image:Checker");
                content += checker;
            }
            const std::string src = externalSource(preview, F32(in.getWidth() * in.getHeight()));
            if (src.empty())
                block("LLTextureCtrl(texture not ready or slots full)");
            else
                content += "<img src=\"" + src + "\" style=\"display:block;position:absolute;left:" + dp(in.mLeft) + ";top:" +
                           dp(r.getHeight() - in.mTop) + ";width:" + dp(in.getWidth()) + ";height:" + dp(in.getHeight()) +
                           ";image-color:" + hex(alpha(UI_VERTEX_COLOR, a)) + "\"/>";
        }
        else
        {
            LLUIImagePtr img = tx->mLocked ? LLUI::getUIImage("locked_image.j2c") : tx->mFallbackImage;
            if (img.notNull() && LLRmlUiAtlas::instance().find(img.get()))
                rect(in, "decorator:llimage('" + img->getName() + "');image-color:" + hex(alpha(UI_VERTEX_COLOR, a)));
            else if (tx->mLocked && img.isNull())
                rect(in, "background-color:" + hex(alpha(LLColor4::grey, a)));
            else
                block("LLTextureCtrl(no preview)"); // grey + "x", or an image the atlas refused
        }
        if (tx->mTentativeLabel)
            tx->mTentativeLabel->setVisible(tx->getTentative());
        if (tx->mTexturep.notNull() && !tx->mTexturep->isFullyLoaded() && tx->mShowLoadingPlaceholder)
            block("LLTextureCtrl(loading)"); // the loading text with its drop shadow: LLView while loading
        if (tx->ll::ui::SearchableControl::getHighlighted())
            block("LLTextureCtrl(search highlight)");
    }
    else if (auto* li = dynamic_cast<LLLoadingIndicator*>(v))
    {
        // LLLoadingIndicator::draw: advance the frame on its timer, draw the current image.
        if (li->mImageSwitchTimer.getStarted() && li->mImageSwitchTimer.hasExpired())
        {
            if (!li->mImages.empty())
                li->mCurImageIdx = (li->mCurImageIdx + 1) % li->mImages.size();
            li->start();
        }
        root.animating = true;
        if (!li->mImages.empty())
            image(li->mImages[li->mCurImageIdx], alpha(LLColor4::white, li->getDrawContext().mAlpha));
    }
    else if (auto* sw = dynamic_cast<LLColorSwatchCtrl*>(v))
    {
        // LLColorSwatchCtrl::draw: 1-unit outline, then the colour (or the fallback image).
        const F32 a = sw->getTransparencyType() == LLUICtrl::TT_ACTIVE ? 1.0f : sw->getCurrentTransparency();
        if (sw->mBorder)
            sw->mBorder->setKeyboardFocusHighlight(sw->hasFocus());
        const LLRect box(0, r.getHeight(), r.getWidth(), sw->mLabelHeight);
        LLRect       in = box;
        in.stretch(-1);
        auto rect = [&](const LLRect& q, const std::string& style)
        {
            content += "<div style=\"left:" + dp(q.mLeft) + ";top:" + dp(r.getHeight() - q.mTop) + ";width:" + dp(std::max(0, q.getWidth())) +
                       ";height:" + dp(std::max(0, q.getHeight())) + ";" + style + "\"></div>";
        };
        rect(box, "border-width:1dp;border-color:" + hex(sw->mBorderColor.get()));
        if (sw->mValid && sw->mColor.isOpaque())
            rect(in, "background-color:" + hex(alpha(sw->mColor, a)));
        else if (sw->mValid && (sw->mAlphaGradientImage.isNull() || LLRmlUiAtlas::instance().find(sw->mAlphaGradientImage)))
        {
            // Translucent: checkerboard, the colour over it, then the alpha ramp in the opaque colour.
            const std::string checker = checkerRml(in, r.getHeight(), a);
            if (checker.empty())
            {
                root.dynamicBlock = true;
                root.blocker      = "image:Checker";
            }
            content += checker;
            rect(in, "background-color:" + hex(alpha(sw->mColor, a)));
            if (sw->mAlphaGradientImage.notNull())
                rect(in, "decorator:llimage('" + sw->mAlphaGradientImage->getName() + "');image-color:" + hex(alpha(sw->mColor, a)));
        }
        else if (!sw->mValid && sw->mFallbackImage.notNull() && LLRmlUiAtlas::instance().find(sw->mFallbackImage))
            rect(in, "decorator:llimage('" + sw->mFallbackImage->getName() + "');image-color:" + hex(alpha(LLColor4::white, a)));
        else
        {
            root.dynamicBlock = true; // invalid without a fallback image (the "x"): LLView
            root.blocker      = "LLColorSwatchCtrl(invalid)";
        }
        if (sw->ll::ui::SearchableControl::getHighlighted())
        {
            root.dynamicBlock = true;
            root.blocker      = "LLColorSwatchCtrl(search highlight)";
        }
    }
    else if (auto* b = dynamic_cast<LLButton*>(v))
    {
        if (auto* hdr = dynamic_cast<LLScrollColumnHeader*>(v))
        {
            // LLScrollColumnHeader::draw's sort-arrow overlay (flat headers: LLView for now).
            if (hdr->mFlat)
            {
                root.dynamicBlock = true;
                root.blocker      = "LLScrollColumnHeader(flat)";
            }
            else if (hdr->mIcon.isNull() && hdr->mColumn && hdr->mColumn->mParentCtrl)
            {
                LLScrollListCtrl*  list = hdr->mColumn->mParentCtrl;
                const std::string  sort = list->getSortColumnName();
                const bool arrow = !hdr->mColumn->mLabel.empty() && list->isSorted() &&
                                   (sort == hdr->mColumn->mSortingColumn || sort == hdr->mColumn->mName);
                if (arrow)
                    hdr->setImageOverlay(list->getSortAscending() ? "up_arrow.tga" : "down_arrow.tga", LLFontGL::RIGHT, LLColor4::white);
                else
                    hdr->setImageOverlay(LLUUID::null);
            }
        }
        const bool enabled  = b->isInEnabledChain();
        const bool selected = b->getToggleState();
        const bool pressed  = b->mForcePressedState || (b->hasMouseCapture() && b->mDisplayPressedState);
        LLUIImage* img      = nullptr;
        if (pressed && b->mDisplayPressedState)
            img = selected ? b->mImagePressedSelected : b->mImagePressed;
        else if (b->mNeedsHighlight)
            img = selected ? (b->mImageHoverSelected ? b->mImageHoverSelected : b->mImageSelected)
                           : (b->mImageHoverUnselected ? b->mImageHoverUnselected : b->mImageUnselected);
        else
            img = selected ? b->mImageSelected : b->mImageUnselected;
        if (b->mImageDisabledSelected.notNull() && ((enabled && b->getTentative()) || (!enabled && selected)))
            img = b->mImageDisabledSelected;
        else if (b->mImageDisabled.notNull() && !enabled && !selected)
            img = b->mImageDisabled;
        const F32 a = b->mUseDrawContextAlpha ? b->getDrawContext().mAlpha : b->getCurrentTransparency();
        const LLColor4 disabled = b->mFadeWhenDisabled ? alpha(b->mDisabledImageColor.get(), 0.5f) : b->mDisabledImageColor.get();
        if (img)
            image(img, alpha(enabled ? b->mImageColor.get() : disabled, a));
        if (!b->mScaleImage && img)
            contentStyle += ""; // unscaled images: drawn at natural size, top-left (rare in floaters)

        LLColor4 label = enabled ? (selected ? b->mSelectedLabelColor.get() : b->mUnselectedLabelColor.get())
                                 : (selected ? b->mDisabledSelectedLabelColor.get() : b->mDisabledLabelColor.get());
        S32 text_left = b->mLeftHPad, text_width = r.getWidth() - b->mLeftHPad - b->mRightHPad;
        std::string overlay;
        if (b->mImageOverlayEnable && b->mImageOverlay.notNull())
        {
            S32 ow = 0, oh = 0;
            b->getOverlayImageSize(ow, oh);
            S32 cx = r.getWidth() / 2, cy = r.getHeight() / 2 + (b->mImageOverlayBottomPad - b->mImageOverlayTopPad);
            LLColor4 oc = !enabled ? b->mImageOverlayDisabledColor.get() : selected ? b->mImageOverlaySelectedColor.get() : b->mImageOverlayColor.get();
            oc.mV[VALPHA] *= a;
            S32 ox = cx - ow / 2;
            if (b->mImageOverlayAlignment == LLFontGL::LEFT)
            {
                ox = b->mLeftHPad;
                text_left += ow + b->mImgOverlayLabelSpace;
                text_width -= ow + b->mImgOverlayLabelSpace;
            }
            else if (b->mImageOverlayAlignment == LLFontGL::RIGHT)
            {
                ox = r.getWidth() - b->mRightHPad - ow;
                text_width -= ow + b->mImgOverlayLabelSpace;
            }
            if (!LLRmlUiAtlas::instance().find(b->mImageOverlay))
            {
                root.dynamicBlock = true;
                root.blocker      = "image:" + b->mImageOverlay->getName();
            }
            overlay = "<div style=\"left:" + dp(ox) + ";top:" + dp(r.getHeight() - (cy + oh - oh / 2)) + ";width:" + dp(ow) +
                      ";height:" + dp(oh) + ";decorator:llimage('" + b->mImageOverlay->getName() + "');image-color:" + hex(oc) + "\"></div>";
        }
        LLWString lw = b->getCurrentLabel().getWString();
        LLWStringUtil::trim(lw);
        content = overlay;
        if (!lw.empty())
        {
            const char* align = b->mHAlign == LLFontGL::RIGHT ? "right" : b->mHAlign == LLFontGL::HCENTER ? "center" : "left";
            const S32   h2    = r.getHeight() / 2 + b->mBottomVPad; // VCENTER at this y
            const S32   lh    = b->mGLFont ? b->mGLFont->getLineHeight() : 12;
            content += "<div style=\"white-space:pre;overflow:hidden;text-align:" + std::string(align) + ";left:" + dp(text_left) +
                       ";width:" + dp(std::max(0, text_width)) + ";top:" + dp(r.getHeight() - h2 - lh / 2) + ";height:" + dp(lh) +
                       ";color:" + hex(alpha(label, a)) + ";" + fontStyle(b->mGLFont) + "\">" +
                       Rml::StringUtilities::EncodeRml(wstring_to_utf8str(lw)) + "</div>";
        }
    }
    else if (auto* t = dynamic_cast<LLTextBase*>(v))
    {
        auto* te = dynamic_cast<LLTextEditor*>(v);
        if (te && te->mBorder)
            te->mBorder->setKeyboardFocusHighlight(te->hasFocus());
        const bool editing = !t->mReadOnly && t->hasFocus() && gFocusMgr.getAppHasFocus();
        if (editing && ((gKeyboard && gKeyboard->getInsertMode() == LL_KIM_OVERWRITE) || (te && te->hasPreeditString())))
        {
            root.dynamicBlock = true; // overwrite caret / IME pre-edit: LLView for now
            root.blocker      = "LLTextBase(overwrite or IME)";
        }
        // With a scroller the text is drawn over it (after the children).
        set(n.cprops, n.content, "z-index", t->mScroller ? "1" : "auto", changed);
        bool ok = true;
        content = textRml(t, ok);
        if (editing && t->getSegIterContaining(t->mCursorPos) != t->mSegments.end())
        {
            // LLTextBase::drawCursor: 2 units wide, flashing after a second without typing; the
            // IME window follows it.
            LLRect cr = t->getLocalRectFromDocIndex(t->mCursorPos);
            cr.translate(-1, 0);
            cr.mRight = cr.mLeft + 2;
            const F32 elapsed = t->mCursorBlinkTimer.getElapsedTimeF32();
            if (elapsed < 1.0f || (S32(elapsed * 2) & 1))
                content += "<div style=\"left:" + dp(cr.mLeft) + ";top:" + dp(r.getHeight() - cr.mTop) + ";width:2dp;height:" +
                           dp(cr.getHeight()) + ";background-color:" + hex(alpha(t->mCursorColor.get(), t->getDrawContext().mAlpha)) + "\"></div>";
            const LLRect screen = t->calcScreenRect();
            LLCoordGL    ime(screen.mLeft + cr.mLeft, screen.mBottom + cr.mTop);
            ime.mX = S32(ime.mX * LLUI::getScaleFactor().mV[VX]);
            ime.mY = S32(ime.mY * LLUI::getScaleFactor().mV[VY]);
            t->getWindow()->setLanguageTextInput(ime);
        }
        if (!ok)
        {
            root.dynamicBlock = true;
            root.blocker      = "LLTextBase(segments)";
        }
        if (t->mBGVisible)
            background = hex(alpha(t->mReadOnly ? t->mReadOnlyBgColor.get() : t->mWriteableBgColor.get(), t->getCurrentTransparency()));
    }
    else if (auto* ic = dynamic_cast<LLIconCtrl*>(v))
    {
        if (auto* pic = dynamic_cast<LLProfileImageCtrl*>(v); pic && pic->getImage().notNull())
        {
            // LLProfileImageCtrl::draw: pump the texture priority, then LLIconCtrl::draw.
            pic->getImage()->addTextureStats(MAX_IMAGE_AREA);
            pic->getImage()->setKnownDrawSize(LLViewerTexture::MAX_IMAGE_SIZE_DEFAULT, LLViewerTexture::MAX_IMAGE_SIZE_DEFAULT);
        }
        const LLColor4 c = alpha(ic->mColor.get(), ic->mUseDrawContextAlpha ? ic->getDrawContext().mAlpha : ic->getCurrentTransparency());
        if (ic->mImagep.notNull() && !LLRmlUiAtlas::instance().find(ic->mImagep))
        {
            // Not a skin image (an asset: profile picture, avatar / group icon): the viewer
            // texture itself through an external slot, stretched over the box (no scale-9).
            auto* tex = dynamic_cast<LLViewerTexture*>(ic->mImagep->getImage().get());
            const std::string src = externalSource(tex, F32(r.getWidth() * r.getHeight()));
            if (src.empty())
                image(ic->mImagep, c); // records the blocker (not ready / slots full)
            else
                content = "<img src=\"" + src + "\" style=\"display:block;position:absolute;left:0dp;top:0dp;width:" + dp(r.getWidth()) +
                          ";height:" + dp(r.getHeight()) + ";image-color:" + hex(c) + "\"/>";
        }
        else if (ic->mImagep.notNull())
            image(ic->mImagep, c);
    }
    else if (auto* vb = dynamic_cast<LLViewBorder*>(v))
    {
        if (vb->mStyle == LLViewBorder::STYLE_LINE && vb->mBorderWidth > 0)
        {
            LLColor4 topc = vb->mHighlightLight.get(), botc = vb->mHighlightLight.get();
            if (vb->mBevel == LLViewBorder::BEVEL_OUT) { topc = vb->mHighlightLight.get(); botc = vb->mShadowDark.get(); }
            if (vb->mBevel == LLViewBorder::BEVEL_IN)  { topc = vb->mShadowDark.get();     botc = vb->mHighlightLight.get(); }
            if (vb->mHasKeyboardFocus) topc = botc = gFocusMgr.getFocusColor();
            border      = dp(vb->mBorderWidth);
            borderColor = hex(topc) + " " + hex(botc) + " " + hex(botc) + " " + hex(topc);
        }
    }
    else if (auto* rh = dynamic_cast<LLResizeHandle*>(v))
    {
        if (rh->mImage.notNull() && rh->mCorner == LLResizeHandle::RIGHT_BOTTOM)
        {
            // drawn at natural size from the bottom-left corner
            const S32 iw = rh->mImage->getWidth(), ih = rh->mImage->getHeight();
            if (!LLRmlUiAtlas::instance().find(rh->mImage))
            {
                root.dynamicBlock = true;
                root.blocker      = "image:" + rh->mImage->getName();
            }
            content = "<div style=\"left:0dp;top:" + dp(r.getHeight() - ih) + ";width:" + dp(iw) + ";height:" + dp(ih) +
                      ";decorator:llimage('" + rh->mImage->getName() + "')\"></div>";
        }
    }
    else if (auto* dh = dynamic_cast<LLDragHandleTop*>(v))
    {
        if (dh->mTitleBox)
            dh->mTitleBox->setEnabled(dh->getForeground());
    }
    else if (auto* sl = dynamic_cast<LLSlider*>(v))
    {
        sl->updateThumbRect();
        const F32  a = sl->getDrawContext().mAlpha;
        const bool horizontal = sl->mOrientation == LLSlider::HORIZONTAL;
        LLUIImage* track = horizontal ? sl->mTrackImageHorizontal : sl->mTrackImageVertical;
        LLUIImage* hl    = horizontal ? sl->mTrackHighlightHorizontalImage : sl->mTrackHighlightVerticalImage;
        LLRect     tr, hr;
        if (track && sl->mThumbImage)
        {
            if (horizontal)
            {
                tr.set(sl->mThumbImage->getWidth() / 2, r.getHeight() / 2 + track->getHeight() / 2, r.getWidth() - sl->mThumbImage->getWidth() / 2,
                       r.getHeight() / 2 - track->getHeight() / 2);
                hr.set(tr.mLeft, tr.mTop, sl->mThumbRect.getCenterX(), tr.mBottom);
            }
            else
            {
                tr.set(r.getWidth() / 2 - track->getWidth() / 2, r.getHeight(), r.getWidth() / 2 + track->getWidth() / 2, 0);
                hr = tr;
            }
        }
        const LLColor4 tc = sl->isInEnabledChain() ? alpha(LLColor4::white, a) : alpha(LLColor4::white, 0.6f * a);
        auto quad = [&](LLUIImage* img, const LLRect& q, const LLColor4& c)
        {
            if (!img || !q.isValid())
                return;
            if (!LLRmlUiAtlas::instance().find(img))
            {
                root.dynamicBlock = true;
                root.blocker      = "image:" + img->getName();
                return;
            }
            content += "<div style=\"left:" + dp(q.mLeft) + ";top:" + dp(r.getHeight() - q.mTop) + ";width:" + dp(q.getWidth()) +
                       ";height:" + dp(q.getHeight()) + ";decorator:llimage('" + img->getName() + "');image-color:" + hex(c) + "\"></div>";
        };
        quad(track, tr, tc);
        quad(hl, hr, tc);
        LLUIImage* thumb = sl->hasMouseCapture() ? sl->mThumbImagePressed.get()
                           : !sl->isInEnabledChain() ? sl->mThumbImageDisabled.get() : sl->mThumbImage.get();
        quad(thumb, sl->mThumbRect, alpha(sl->hasMouseCapture() ? sl->mThumbOutlineColor.get() : sl->mThumbCenterColor.get(), a));
    }
    else if (auto* le = dynamic_cast<LLLineEditor*>(v))
    {
        const bool focused = le->hasFocus();
        if (le->hasPreeditString() || (focused && le->hasSelection()) || (focused && gKeyboard && gKeyboard->getInsertMode() == LL_KIM_OVERWRITE))
        {
            root.dynamicBlock = true; // selection, IME pre-edit, overwrite caret: LLView for now
            root.blocker      = "LLLineEditor(selection or IME)";
        }
        const F32 a = le->getCurrentTransparency();
        if (le->mUseBgColor)
            background = hex(alpha(le->mBgColor.get(), a));
        else
        {
            LLUIImage* img = le->mReadOnly ? le->mBgImageDisabled.get()
                             : (focused || le->mShowImageFocused) ? le->mBgImageFocused.get() : le->mBgImage.get();
            if (img)
                image(img, alpha(UI_VERTEX_COLOR, a));
        }
        const S32 lh   = le->mGLFont ? le->mGLFont->getLineHeight() : 12;
        const S32 bgh  = r.getHeight() - 2 * le->mBorderThickness;
        const S32 vpad = (bgh - lh) / 2 + (le->mSpellCheck ? 1 : 0);
        const S32 bottom = le->mBorderThickness + vpad;
        std::string str;
        LLColor4    c;
        if (le->mText.length())
        {
            str = le->mDrawAsterixes ? std::string(size_t(le->mText.length()), '*')
                                     : wstring_to_utf8str(le->mText.getWString().substr(size_t(std::max(0, le->mScrollHPos))));
            c   = le->mReadOnly ? le->mReadOnlyFgColor.get() : le->getTentative() ? le->mTentativeFgColor.get() : le->mFgColor.get();
        }
        else if (!le->mLabel.empty())
        {
            str = le->mLabel.getString();
            c   = le->mTentativeFgColor.get();
        }
        c.setAlpha(a);
        if (focused && !le->mReadOnly && gFocusMgr.getAppHasFocus())
        {
            // The caret, flashing every half second after a second without typing (as draw()).
            static LLUICachedControl<S32> thickness("UILineEditorCursorThickness", 0);
            const F32 elapsed = le->mKeystrokeTimer.getElapsedTimeF32();
            if (elapsed < 1.0f || (S32(elapsed * 2) & 1))
            {
                const S32 left = le->findPixelNearestPos() - thickness / 2;
                LLColor4  cc   = le->getTentative() ? le->mTentativeFgColor.get() : le->mFgColor.get();
                cc.setAlpha(a);
                content += "<div style=\"left:" + dp(left) + ";width:" + dp(std::max<S32>(1, thickness)) + ";top:" +
                           dp(r.getHeight() - (r.getHeight() - le->mBorderThickness - 1)) + ";height:" +
                           dp(std::max(0, (r.getHeight() - le->mBorderThickness - 1) - (le->mBorderThickness + 2))) +
                           ";background-color:" + hex(cc) + "\"></div>";
            }
            if (le->mText.length() == 0)
                str.clear(); // the placeholder label is not shown while editing
        }
        if (!str.empty())
            content += "<div style=\"white-space:pre;overflow:hidden;left:" + dp(le->mTextLeftEdge) + ";width:" +
                      dp(std::max(0, le->mTextRightEdge - le->mTextLeftEdge)) + ";top:" + dp(r.getHeight() - bottom - lh) + ";height:" + dp(lh) +
                      ";color:" + hex(c) + ";" + fontStyle(le->mGLFont) + "\">" + Rml::StringUtilities::EncodeRml(str) + "</div>";
    }

    setProp(n, "decorator", decorator, changed);
    setProp(n, "image-color", imageColor, changed);
    setProp(n, "background-color", background, changed);
    setProp(n, "box-shadow", boxShadow, changed);
    setProp(n, "border-width", border, changed);
    setProp(n, "border-color", borderColor, changed);
    if (auto* chp = dynamic_cast<LLChicletPanel*>(v->getParent()); chp && v == chp->mScrollArea)
        overflow = "hidden"; // LLChicletPanel::draw clips its scroll area to its rect
    setProp(n, "overflow", overflow, changed);
    if (content != n.inner)
    {
        n.inner = content;
        n.content->SetInnerRML(content);
        changed = true;
    }
}

// ---------------------------------------------------------------------------------------------
namespace
{
bool claimRoot(State* s, LLView* v, U32 frame)
{
    auto it = s->roots.find(v);
    if (it == s->roots.end())
    {
        try
        {
            it = s->roots.emplace(v, Root{}).first;
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
        it->second.handle = v->getHandle();
    }
    Root& r = it->second;
    if (r.handle.get() != v)
    {
        destroyElements(r);
        r       = Root{};
        r.handle = v->getHandle();
    }
    r.drawnFrame = frame; // drawn this frame (by RmlUi or LLView): its mirror stays
    const F64 now = LLTimer::getTotalSeconds();
    // Re-check what blocked it (classes, focus, images) at most once a second.
    if (!r.blocker.empty() && now < r.recheckAt)
        return false;
    if (r.nodes.empty() || !r.blocker.empty())
    {
        r.checkedFrame = frame;
        r.recheckAt    = now + 1.0;
        destroyElements(r);
        r.dynamicBlock = false;
        if (!analyse(r, v))
        {
            try { ++s->blockers[r.blocker]; } catch (const std::bad_alloc&) {}
            return false;
        }
    }
    // Re-mirror only when something may have changed: a reported change, a move, the mouse
    // over it (hover highlights), keyboard focus or mouse capture inside it (caret, drags),
    // and a staggered refresh for the few changes that report nothing (the retained-UI rules).
    {
        const LLRect screen = v->calcScreenRect();
        LLRect       hover  = screen;
        hover.stretch(8);
        S32 mx = 0, my = 0;
        LLUI::getInstance()->getMousePositionScreen(&mx, &my);
        const bool blocked = !r.dynamicWhy.empty();
        // A root LLView draws for a frame-local reason is re-looked at on a change, a move or
        // twice a second, never every frame (hover / focus do not make it live).
        const bool live = !blocked && (r.animating || hover.pointInRect(mx, my) || gFocusMgr.childHasKeyboardFocus(v) ||
                                       gFocusMgr.childHasMouseCapture(v));
        if (r.element && !r.dirty && !live && screen == r.lastScreen && now < r.nextRefresh)
        {
            if (blocked)
                return false; // still blocked: unchanged since the last look
            r.seenFrame = frame;
            r.order     = ++s->order;
            setProp(r.nodes[0], "display", "block", s->changed);
            setProp(r.nodes[0], "z-index", std::to_string(2 * r.order), s->changed);
            return true;
        }
        r.dirty       = false;
        r.lastScreen  = screen;
        r.nextRefresh = now + (blocked ? 0.5 : 0.125) * (0.75 + 0.5 * ll_frand());
        // Children added / removed: only looked for here (a change, a refresh), never per frame.
        if (structureChanged(r))
        {
            destroyElements(r);
            if (!analyse(r, v))
            {
                r.checkedFrame = frame;
                try { ++s->blockers[r.blocker]; } catch (const std::bad_alloc&) {}
                return false;
            }
        }
    }
    // Mirror this frame's state now (the LLView state is final at draw time). A view that
    // cannot be shown this frame (focused editor, missing image, visible view without a
    // presenter) goes to the LLView renderer for this frame only.
    if (!r.element && !buildElements(r))
    {
        destroyElements(r);
        r.blocker      = "rmlui:element_allocation";
        r.checkedFrame = frame;
        return false;
    }
    r.dynamicBlock = false;
    r.animating    = false;
    r.blocker.clear();
    ++s->stats.represented;
    const auto t0 = std::chrono::steady_clock::now();
    {
        // The draw-time logic the presenters run (layout, sort, overlays) is not a change
        // to report back to ourselves.
        LLView::RetainedDirtyMute mute(true);
        for (Node& n : r.nodes)
            LLRmlUiClassicAccess::present(r, n, s->changed);
    }
    s->presentMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (r.dynamicBlock)
    {
        setProp(r.nodes[0], "display", "none", s->changed);
        r.dynamicWhy   = r.blocker;
        r.blocker.clear();
        return false;
    }
    r.dynamicWhy.clear();
    r.seenFrame = frame;
    r.order     = ++s->order;
    setProp(r.nodes[0], "display", "block", s->changed);
    setProp(r.nodes[0], "z-index", std::to_string(2 * r.order), s->changed);
    return true;
}

// LLView::sPresentHook. RmlUi records before the LLView replay, so presented floaters must
// be the back-most run of the floater stack: drawChildren goes back to front, and once one
// floater stays on LLView this frame, every floater in front of it does too.
// A top-level view that stays on LLView this frame without being analysed (z-order limit):
// still counted, with its reason, for the coverage report.
void noteRefused(State* s, LLView* v, const char* why)
{
    try
    {
        Root& r = s->roots[v];
        if (r.handle.get() != v)
            r.handle = v->getHandle();
        r.dynamicWhy = why;
    }
    catch (const std::bad_alloc&)
    {
    }
}

// LLView::sPresentHook. RmlUi records at marks in the LLView batch stream: each contiguous run
// of presented views (in draw order) gets its own mark and RmlUi segment, so LLView drawing
// between runs (world-anchored UI, the nav bar, a floater left on LLView) stacks exactly.
bool presentHook(LLView* v)
{
    State* s = gState;
    if (!s)
        return false;
    const U32 frame = LLFrameTimer::getFrameCount();
    if (frame != s->lastDrawFrame)
    {
        s->lastDrawFrame = frame;
        s->order         = 0;
        s->runs          = 0;
        s->navBar        = LLNavigationBar::instanceExists() ? LLNavigationBar::getInstance() : nullptr;
    }
    if (!isCandidate(v))
        return false;
    // A new run starts at the first presented view and whenever LLView drew since the last one.
    const bool newRun = s->order == 0 || (gGL.flush(), slUIBatchCount()) != s->batchesAtLayer;
    if (newRun && s->runs >= std::min<size_t>(State::kMaxRuns, s->markers.size() + 1))
    {
        noteRefused(s, v, "z-order: more interleaved layers than the UI layer supports this frame");
        if (auto* f = dynamic_cast<LLFloater*>(v))
            f->onRetainedDirty();
        return false;
    }
    if (claimRoot(s, v, frame))
    {
        Root& r = s->roots[v];
        if (newRun)
        {
            gGL.flush();
            slUIMarkExternalLayer();
            // Runs after the first start with a marker element just below their first view:
            // the backend begins the next segment when RmlUi renders it.
            if (s->runs > 0 && s->runs - 1 < s->markers.size())
            {
                Rml::Element* m = s->markers[s->runs - 1];
                m->SetProperty("z-index", std::to_string(2 * r.order - 1));
                m->SetProperty("display", "block");
                s->changed |= s->markerZ[s->runs - 1] != 2 * r.order - 1;
                s->markerZ[s->runs - 1] = 2 * r.order - 1;
            }
            ++s->runs;
        }
        s->batchesAtLayer = slUIBatchCount();
        return true;
    }
    // Back on LLView after RmlUi drew it: its retained LLView recording may predate changes
    // made while it was presented, so make it re-record.
    if (auto it = s->roots.find(v); it != s->roots.end() && it->second.seenFrame + 1 == frame)
        if (auto* f = dynamic_cast<LLFloater*>(v)) f->onRetainedDirty();
    return false;
}
} // namespace

namespace
{
std::string externalSource(LLViewerTexture* tex, F32 area)
{
    State* s = gState;
    if (!s || !tex)
        return {};
    VkImage     image = VK_NULL_HANDLE;
    VkImageView view  = VK_NULL_HANDLE;
    VkExtent2D  extent{};
    uint32_t    id = 0;
    if (!slUIExternalImage(tex->getTextureData(), image, view, extent, id))
        return {};
    try
    {
        char key[64];
        std::snprintf(key, sizeof(key), "llext:%u-%llx", id, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(view)));
        auto it = s->externals.find(key);
        if (it == s->externals.end())
        {
            if (s->externals.size() >= State::kMaxExternals)
                return {};
            it = s->externals.emplace(key, State::External{ tex, image, view, extent, area, 0 }).first;
        }
        it->second.area     = std::max(it->second.area, area);
        it->second.lastUsed = LLTimer::getTotalSeconds();
        return key;
    }
    catch (const std::bad_alloc&)
    {
        return {};
    }
}

// Before a capture: every held texture still is the image RmlUi samples, else RmlUi drops it
// now (this frame's capture no longer references it) and the floaters re-mirror.
bool validateExternals(State* s)
{
    bool       changed = false;
    const F64  now     = LLTimer::getTotalSeconds();
    for (auto it = s->externals.begin(); it != s->externals.end();)
    {
        State::External& e = it->second;
        VkImage     image = VK_NULL_HANDLE;
        VkImageView view  = VK_NULL_HANDLE;
        VkExtent2D  extent{};
        uint32_t    id = 0;
        const bool alive = e.tex.notNull() && slUIExternalImage(e.tex->getTextureData(), image, view, extent, id) && image == e.image &&
                           view == e.view;
        if (alive && now - e.lastUsed < 30.0)
        {
            e.tex->addTextureStats(e.area); // as LLTextureCtrl::draw: keeps its fetch priority
            ++it;
            continue;
        }
        Rml::ReleaseTexture(it->first);
        it      = s->externals.erase(it);
        changed = true;
    }
    if (changed)
        for (auto& [view, r] : s->roots)
            r.dirty = true;
    return changed;
}

} // namespace

LLRmlUiClassic& LLRmlUiClassic::instance()
{
    static LLRmlUiClassic classic;
    return classic;
}

bool LLRmlUiClassic::active() const { return gState != nullptr; }

bool LLRmlUiClassic::start(Rml::Context* context)
{
    if (gState || !context)
        return gState != nullptr;
    // Both name lists are binary-searched: they must stay strcmp-sorted.
    for (auto list : { std::make_pair(std::begin(kDrawClasses), std::end(kDrawClasses)), std::make_pair(std::begin(kPresented), std::end(kPresented)) })
        if (!std::is_sorted(list.first, list.second, [](const char* x, const char* y) { return std::strcmp(x, y) < 0; }))
        {
            LL_WARNS("RmlUi") << "presenter class lists are not sorted; classic RmlUi disabled" << LL_ENDL;
            return false;
        }
    State* s = new (std::nothrow) State();
    if (!s)
        return false;
    s->context  = context;
    s->document = context->LoadDocumentFromMemory(R"rml(<rml><head><style>
body { position: absolute; left: 0; top: 0; right: 0; bottom: 0; pointer-events: none; font-family: DejaVu Sans; }
div { display: block; position: absolute; box-sizing: border-box; border-width: 0; }
span { display: inline; white-space: pre; }
div.c { left: 0; top: 0; right: 0; bottom: 0; }
</style></head><body></body></rml>)rml", "classic.rml");
    if (!s->document)
    {
        delete s;
        return false;
    }
    s->body = s->document;
    try
    {
        for (uint32_t k = 0; k + 1 < State::kMaxRuns; ++k)
        {
            Rml::ElementPtr marker = s->document->CreateElement("div");
            if (!marker)
                break;
            marker->SetAttribute("style", "left:0dp;top:0dp;width:1dp;height:1dp;decorator:llsegment(0);display:none");
            s->markers.push_back(s->body->AppendChild(std::move(marker)));
            s->markerZ.push_back(-1);
        }
    }
    catch (const std::bad_alloc&)
    {
        // fewer markers: fewer runs (later views stay on LLView)
    }
    s->document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    gState              = s;
    LLView::sPresentHook = &presentHook;
    LLRmlUiAtlas::instance().setExternalSource(
        [](LLRmlUiVK& vk, Rml::Vector2i& size, const std::string& source) -> Rml::TextureHandle
        {
            if (!gState)
                return 0;
            auto it = gState->externals.find(source);
            if (it == gState->externals.end())
                return 0;
            size = { int(it->second.extent.width), int(it->second.extent.height) };
            return vk.externalTexture(it->second.image, it->second.view, it->second.extent.width, it->second.extent.height);
        });
    gFloaterChangeListener = [](LLFloater* f)
    {
        if (gState)
            if (auto it = gState->roots.find(f); it != gState->roots.end())
                it->second.dirty = true;
    };
    LL_INFOS("RmlUi") << "Classic UI on RmlUi: presenting floaters whose views all have presenters" << LL_ENDL;
    return true;
}

void LLRmlUiClassic::stop()
{
    LLView::sPresentHook   = nullptr;
    gFloaterChangeListener = nullptr;
    if (!gState)
        return;
    for (auto& [key, e] : gState->externals)
        Rml::ReleaseTexture(key);
    gState->externals.clear();
    LLRmlUiAtlas::instance().setExternalSource({});
    if (gState->document)
        gState->document->Close();
    delete gState;
    gState = nullptr;
}

bool LLRmlUiClassic::sync()
{
    State* s = gState;
    if (!s)
        return false;
    const auto t0      = std::chrono::steady_clock::now();
    const F64  now     = LLTimer::getTotalSeconds();
    // Segment markers of runs not used this frame are hidden.
    for (size_t k = 0; k < s->markers.size(); ++k)
        if (k + 1 >= s->runs && s->markerZ[k] != -1)
        {
            s->markers[k]->SetProperty("display", "none");
            s->markerZ[k] = -1;
            s->changed    = true;
        }
    bool       changed = s->changed;
    s->changed       = false;
    changed |= validateExternals(s);
    s->stats.claimed = s->stats.nodes = 0;
    for (auto it = s->roots.begin(); it != s->roots.end();)
    {
        Root& r    = it->second;
        LLView* v  = r.handle.get();
        if (!v)
        {
            if (r.element)
                changed = true;
            destroyElements(r);
            it = s->roots.erase(it);
            continue;
        }
        if (r.seenFrame != s->lastDrawFrame || !r.blocker.empty())
        {
            // Not drawn this frame (hidden, closed, or back on the LLView renderer).
            if (r.element)
            {
                bool c = false;
                setProp(r.nodes[0], "display", "none", c);
                changed |= c;
                // Bounded: a mirror is dropped only after 30 s of not being drawn at all
                // (closed / hidden). One LLView draws meanwhile keeps its (hidden) elements.
                if (r.drawnFrame == s->lastDrawFrame)
                    r.unseenSince = 0;
                else if (!r.unseenSince)
                    r.unseenSince = now;
                else if (now - r.unseenSince > 30.0)
                    destroyElements(r);
            }
            ++it;
            continue;
        }
        r.unseenSince = 0;
        ++s->stats.claimed;
        s->stats.nodes += U32(r.nodes.size());
        ++it;
    }
    s->stats.roots = U32(s->roots.size());
    if (changed)
        ++s->stats.changes;
    const double ms   = s->presentMs + std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    s->presentMs      = 0;
    s->stats.syncMs   = ms;
    s->stats.peakSyncMs = std::max(s->stats.peakSyncMs, ms);
    if (now >= s->reportAt)
    {
        s->reportAt = now + 60.0;
        LL_INFOS("RmlUi") << coverageReport() << LL_ENDL;
        s->stats.peakSyncMs = 0;
    }
    return changed;
}

LLRmlUiClassic::Stats LLRmlUiClassic::stats() const { return gState ? gState->stats : Stats{}; }

void LLRmlUiClassic::resetCounters()
{
    if (gState)
        gState->stats.created = gState->stats.destroyed = gState->stats.represented = 0;
}

std::string LLRmlUiClassic::coverageReport() const
{
    if (!gState)
        return "classic RmlUi inactive";
    std::string out = "classic RmlUi: " + std::to_string(gState->stats.claimed) + " presented / " + std::to_string(gState->roots.size()) +
                      " top-level views seen, " + std::to_string(gState->stats.nodes) + " nodes, sync " +
                      std::to_string(gState->stats.syncMs) + " ms (peak " + std::to_string(gState->stats.peakSyncMs) + ")";
    for (const auto& [view, r] : gState->roots)
        if (LLView* v = r.handle.get())
            out += "\n  " + v->getName() + ": " + (!r.blocker.empty() ? "LLView (" + r.blocker + ")" : !r.dynamicWhy.empty() ? "LLView now (" + r.dynamicWhy + ")" : std::string("RmlUi"));
    return out;
}
