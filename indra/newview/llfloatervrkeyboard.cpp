/**
 * @file llfloatervrkeyboard.cpp
 * @brief VR on-screen keyboard (#1118). See llfloatervrkeyboard.h.
 */
#include "llviewerprecompiledheaders.h"

#include "llfloatervrkeyboard.h"

#include "llbutton.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "lluictrl.h"
#include "llviewerwindow.h"
#include "llvraudio.h"

namespace
{
constexpr S32 kKeyW = 40;
constexpr S32 kKeyH = 34;
constexpr S32 kGap = 4;
constexpr S32 kPad = 8;

// The field the keyboard serves (compared only, never dereferenced once it lost focus) and
// the field the user hid it for.
LLFocusableElement* sFor = nullptr;
LLFocusableElement* sHiddenFor = nullptr;
bool sWatching = false;

LLUICtrl* textField(LLFocusableElement* focus)
{
    LLUICtrl* ctrl = dynamic_cast<LLUICtrl*>(focus);
    if (!ctrl || !ctrl->acceptsTextInput() || !ctrl->getEnabled() || !ctrl->isInVisibleChain()) return nullptr;
    return ctrl;
}

LLFloaterVRKeyboard* findKeyboard()
{
    return LLFloaterReg::findTypedInstance<LLFloaterVRKeyboard>("vr_keyboard");
}

void hideKeyboard()
{
    if (LLFloaterVRKeyboard* kb = findKeyboard())
        if (kb->getVisible()) kb->setVisible(false);
    sFor = nullptr;
}
} // namespace

LLFloaterVRKeyboard::LLFloaterVRKeyboard(const LLSD& key)
    : LLFloater(key)
{
}

bool LLFloaterVRKeyboard::postBuild()
{
    buildKeys();
    return true;
}

void LLFloaterVRKeyboard::buildKeys()
{
    // Rows, top first. A key: its label, the character it types (0 = a special key), its width.
    struct K
    {
        const char* label;
        llwchar ch;
        KEY key;
        S32 w;
    };
    const K kSpecialBack = {"Backspace", 0, KEY_BACKSPACE, 84};
    const K kSpecialEnter = {"Enter", 0, KEY_RETURN, 84};
    std::vector<std::vector<K>> rows(5);
    for (const char* c = "1234567890"; *c; ++c) rows[0].push_back({nullptr, (llwchar)*c, 0, kKeyW});
    rows[0].push_back(kSpecialBack);
    for (const char* c = "qwertyuiop"; *c; ++c) rows[1].push_back({nullptr, (llwchar)*c, 0, kKeyW});
    rows[1].push_back({nullptr, (llwchar)'\'', 0, kKeyW});
    for (const char* c = "asdfghjkl"; *c; ++c) rows[2].push_back({nullptr, (llwchar)*c, 0, kKeyW});
    rows[2].push_back(kSpecialEnter);
    rows[3].push_back({"Shift", 0, 0, 64});
    for (const char* c = "zxcvbnm,.?"; *c; ++c) rows[3].push_back({nullptr, (llwchar)*c, 0, kKeyW});
    for (const char* c = "!@-:"; *c; ++c) rows[4].push_back({nullptr, (llwchar)*c, 0, kKeyW});
    rows[4].push_back({"Space", (llwchar)' ', 0, 196});
    rows[4].push_back({"<", 0, KEY_LEFT, kKeyW});
    rows[4].push_back({">", 0, KEY_RIGHT, kKeyW});
    rows[4].push_back({"Hide", 0, 0, 64});

    S32 width = 0;
    for (const auto& row : rows)
    {
        S32 w = 0;
        for (const K& k : row) w += k.w + kGap;
        width = llmax(width, w - kGap);
    }
    const S32 header = getHeaderHeight();
    const S32 height = (S32)rows.size() * (kKeyH + kGap) - kGap + 2 * kPad + header;
    reshape(width + 2 * kPad, height);

    for (size_t r = 0; r < rows.size(); ++r)
    {
        S32 x = kPad;
        const S32 top = height - header - kPad - (S32)r * (kKeyH + kGap);
        for (const K& k : rows[r])
        {
            LLButton::Params p;
            std::string label = k.label ? std::string(k.label) : wstring_to_utf8str(LLWString(1, k.ch));
            p.name(k.label ? std::string(k.label) : llformat("key_%u", (U32)k.ch));
            p.label(label);
            p.rect(LLRect(x, top, x + k.w, top - kKeyH));
            p.tab_stop(false);   // (a key never takes the focus from the field)
            p.follows.flags(FOLLOWS_LEFT | FOLLOWS_TOP);
            LLButton* b = LLUICtrlFactory::create<LLButton>(p);
            if (!b) continue;
            addChild(b);
            if (k.ch)
            {
                const llwchar ch = k.ch;
                b->setClickedCallback([this, ch](LLUICtrl*, const LLSD&) { onChar(ch); });
                if (ch >= 'a' && ch <= 'z') mLetters.push_back({b, ch});
            }
            else if (k.key)
            {
                const KEY key = k.key;
                b->setClickedCallback([this, key](LLUICtrl*, const LLSD&) { onKey(key); });
            }
            else if (std::string(k.label) == "Shift")
            {
                mShiftButton = b;
                b->setClickedCallback([this](LLUICtrl*, const LLSD&) { onShift(); });
            }
            else
            {
                b->setClickedCallback([this](LLUICtrl*, const LLSD&) { onHide(); });
            }
            x += k.w + kGap;
        }
    }
}

void LLFloaterVRKeyboard::onChar(llwchar c)
{
    LLFocusableElement* focus = gFocusMgr.getKeyboardFocus();
    if (!textField(focus)) return;
    if (mShift && c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    focus->handleUnicodeChar(c, false);   // (as LLViewerWindow::handleUnicodeChar delivers typed text)
    if (mShift)
    {
        mShift = false;   // one-shot
        relabel();
    }
}

void LLFloaterVRKeyboard::onKey(KEY key)
{
    if (!gViewerWindow || !textField(gFocusMgr.getKeyboardFocus())) return;
    gViewerWindow->handleKey(key, MASK_NONE);
}

void LLFloaterVRKeyboard::onShift()
{
    mShift = !mShift;
    relabel();
}

void LLFloaterVRKeyboard::onHide()
{
    sHiddenFor = sFor;
    hideKeyboard();
    LL_INFOS("VR") << "on-screen keyboard hidden for this field" << LL_ENDL;
}

void LLFloaterVRKeyboard::relabel()
{
    for (const LetterKey& k : mLetters)
    {
        const llwchar c = mShift ? k.lower - 'a' + 'A' : k.lower;
        k.button->setLabel(wstring_to_utf8str(LLWString(1, c)));
    }
    if (mShiftButton) mShiftButton->setToggleState(mShift);
}

void LLFloaterVRKeyboard::placeFor(const LLRect& field_screen)
{
    if (!gFloaterView) return;
    LLRect field;
    gFloaterView->screenRectToLocal(field_screen, &field);
    const LLRect area = gFloaterView->getLocalRect();
    const S32 w = getRect().getWidth(), h = getRect().getHeight();
    // Below the field when it fits, else above it; centred on the field, kept on screen.
    S32 bottom = field.mBottom - kGap - h;
    if (bottom < area.mBottom) bottom = field.mTop + kGap;
    bottom = llclamp(bottom, area.mBottom, llmax(area.mBottom, area.mTop - h));
    const S32 left = llclamp(field.getCenterX() - w / 2, area.mLeft, llmax(area.mLeft, area.mRight - w));
    setOrigin(left, bottom);
}

// static
void LLFloaterVRKeyboard::updateVR(bool wall_up)
{
    LLFocusableElement* focus = gFocusMgr.getKeyboardFocus();
    LLUICtrl* field = wall_up ? textField(focus) : nullptr;
    // Hide is over once ANOTHER text field takes the focus (a click on the world in between
    // does not count: the chat bar re-takes the focus often).
    if (field && focus != sHiddenFor) sHiddenFor = nullptr;
    if (!field || focus == sHiddenFor)
    {
        if (sFor) hideKeyboard();   // (one compare when it is not shown)
        return;
    }
    if (focus == sFor) return;   // shown for this field already
    LLFloaterVRKeyboard* kb = LLFloaterReg::getTypedInstance<LLFloaterVRKeyboard>("vr_keyboard");
    if (!kb) return;
    if (!sWatching)
    {
        // The VR session's end hides it (the desktop never shows it). Connected on first use.
        sWatching = true;
        LLVRAudio::addChangedCallback([]()
        {
            if (!LLVRAudio::active()) hideKeyboard();
        });
    }
    kb->placeFor(field->calcScreenRect());
    if (!kb->getVisible()) kb->openFloater();   // (chrome: opening it does not take the focus)
    if (gFloaterView) gFloaterView->bringToFront(kb, false);
    sFor = focus;
    LL_INFOS("VR") << "on-screen keyboard shown for the focused text field" << LL_ENDL;
}
