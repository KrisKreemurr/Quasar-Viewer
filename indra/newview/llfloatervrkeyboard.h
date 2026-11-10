/**
 * @file llfloatervrkeyboard.h
 * @brief VR on-screen keyboard (#1118, epic #1072): the FALLBACK for text input on the UI wall
 *        (dictation, LLVRDictation, is the main way): corrections, and users without a mic.
 *
 * Shown on the wall while a text field has keyboard focus (below the field, or above it when
 * there is no room), hidden when the focus leaves text fields or the wall is hidden. Its Hide
 * key hides it for that field until the focus moves to another one.
 * Keys type into the focused field the way the physical keyboard does: characters go to the
 * keyboard focus as text input (LLFocusableElement::handleUnicodeChar), Enter / Backspace /
 * arrows through LLViewerWindow::handleKey. The floater is chrome and its keys have no tab
 * stop, so a click on it never takes the focus from the field.
 *
 * Desktop / VR off: nothing. updateVR() is only called from the VR input path; the floater
 * is built on its first show in VR and closed when the VR session ends.
 */
#pragma once

#include "llfloater.h"

#include <vector>

class LLButton;
class LLFocusableElement;

class LLFloaterVRKeyboard : public LLFloater
{
public:
    LLFloaterVRKeyboard(const LLSD& key);
    bool postBuild() override;

    // LLVRDictation::tick (VR session running, every frame): shows / hides / places the
    // keyboard for the current keyboard focus. wall_up: the UI wall is shown this frame.
    static void updateVR(bool wall_up);

private:
    void buildKeys();
    void onChar(llwchar c);
    void onKey(KEY key);
    void onShift();
    void onHide();
    void relabel();
    void placeFor(const LLRect& field_screen);

    struct LetterKey
    {
        LLButton* button;
        llwchar lower;
    };
    std::vector<LetterKey> mLetters;
    LLButton* mShiftButton = nullptr;
    bool mShift = false;
};
