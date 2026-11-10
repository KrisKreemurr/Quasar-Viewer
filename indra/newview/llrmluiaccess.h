// #703 UI on RmlUi: the one friend of the widget classes (declared in their headers).
// It reads their draw state and runs the logic their draw() would have run (layout,
// reflow, thumb rects) for the classic presenter; it never draws.
#pragma once

#include "llrect.h"
#include <string>

class LLFontGL;
class LLTextBase;
class LLUIImage;
class LLScrollContainer;
class LLView;
namespace sl_rmlui
{
struct Node;
struct Root;
}

struct LLRmlUiClassicAccess
{
    static LLRectf     scaleRegion(const LLUIImage* image);
    static LLView*     scrolledView(const LLScrollContainer* container);
    static bool        supportedClass(LLView* view, std::string& why);
    static void        present(sl_rmlui::Root& root, sl_rmlui::Node& node, bool& changed);
    static std::string fontStyle(const LLFontGL* font);
    static std::string textRml(LLTextBase* text, bool& ok);
};
