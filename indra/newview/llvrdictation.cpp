/**
 * @file llvrdictation.cpp
 * @brief VR dictation (#1118). See llvrdictation.h.
 */
#include "llviewerprecompiledheaders.h"

#include "llvrdictation.h"

#include "llagent.h"
#include "llavatarname.h"
#include "llavatarnamecache.h"
#include "llfloaterimnearbychat.h"
#include "llfloatervrkeyboard.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llframetimer.h"
#include "llnotificationsutil.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llspeechtotext.h"
#include "lluictrl.h"
#include "llviewercontrol.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llvraudio.h"
#include "llworld.h"
#include "rlvactions.h"
#include "vkgfx/llxrsession.h"

#include <algorithm>
#include <cmath>

namespace
{
enum class State : U8
{
    IDLE,
    LISTENING,      // the button is held, the mic captures
    TRANSCRIBING,   // released: the clip is with the worker
    PREVIEW,        // #1119: the transcript is shown, counting down to nearby chat
};

enum class Target : U8
{
    FIELD,          // the text field that had keyboard focus at the press
    NEARBY_CHAT,    // #1119: said in nearby chat
};

constexpr F32 kMaxClipSec = 29.5f;      // whisper's window is 30 s: released for the user there
constexpr F32 kStickPushed = 0.5f;      // a press that moves the stick this far is not dictation
constexpr F32 kTapSec = 0.35f;          // a press released sooner is a tap: send the field (Enter)
constexpr F64 kHintSec = 3.0;
constexpr F32 kNearbyRadius = 20.f;     // normal chat range (m): whose names may be spoken
constexpr size_t kMaxUserTerms = 48;
constexpr size_t kMaxNearbyPeople = 16;
constexpr size_t kMaxTermChars = 48;    // longer is not a name (parcel names are often slogans)
// #1119 preview: how long the transcript shows before it is said (time to read it, and to
// cancel), counted only while it is on show (the wrist panel or the wall).
constexpr F32 kPreviewBaseSec = 2.5f;
constexpr F32 kPreviewPerCharSec = 0.03f;
constexpr F32 kPreviewMaxSec = 6.f;
constexpr F64 kPreviewStaleSec = 20.0;  // never on show this long (left controller off): dropped
// The pill when it is not above a field (direct chat, hints): a fixed width (the wrist panel's
// shape stays put while the text changes), the text wrapped onto at most this many lines.
constexpr S32 kWidePillTextWidth = 340;
constexpr size_t kMaxPillLines = 6;

struct DictationState
{
    State state = State::IDLE;
    Target target = Target::FIELD;
    // The field at the press. Only compared with the current keyboard focus, and only used
    // while it still is the focus (it may be gone by the time the text is ready).
    LLFocusableElement* field = nullptr;
    U32 serial = 0;                     // the clip being transcribed
    F64 releasedAt = 0.0;
    F32 clipSec = 0.f;
    bool prevButton = false;
    bool consentOpen = false;           // the download question is up
    const LLSpeechToText::Model* model = nullptr;   // the model for the clip being recorded
    bool upgradeOffered = false;       // the bigger model was offered this session (once)
    bool watching = false;              // connected to LLVRAudio's changed signal
    std::string hint;
    F64 hintUntil = 0.0;
    // #1119 preview
    std::string preview;                // what will be said (the user's speech: never logged)
    F32 previewLeft = 0.f;              // seconds on show still to go
    F64 previewSince = 0.0;
    bool pressCancelledPreview = false; // this press began by cancelling a preview
    bool shownThisFrame = false;        // the wrist panel showed wristStatus() this frame
    F64 lastTick = 0.0;
    bool toggled = false;               // #1120 this capture was started by the wrist's button
};
DictationState sD;

F64 now()
{
    return LLFrameTimer::getElapsedSeconds();
}

std::string dictationLanguage()
{
    static LLCachedControl<std::string> s_lang(gSavedSettings, "SlipstreamVRDictationLanguage", "en");
    return s_lang;
}

void addTerm(std::vector<std::string>& out, std::string term)
{
    LLStringUtil::trim(term);
    if (term.empty() || term.size() > kMaxTermChars) return;
    for (unsigned char c : term)
        if (c < 0x20 || c == 0x7f) return;
    out.push_back(std::move(term));
}

// Words the speaker is likely to say, most important first, for the speech model's prompt
// (LLSpeechToText::transcribe): the user's own list (SlipstreamDictationVocabulary,
// comma-separated), the display / legacy names of avatars within chat range (closest first),
// then the region and parcel names. RLVa: no names or places the restrictions hide. Built per
// dictation on the main thread; never logged.
std::vector<std::string> dictationVocabulary()
{
    std::vector<std::string> v;

    static LLCachedControl<std::string> s_vocab(gSavedSettings, "SlipstreamDictationVocabulary", "");
    const std::string list = s_vocab;
    size_t from = 0, user_terms = 0;
    while (from <= list.size() && user_terms < kMaxUserTerms)
    {
        size_t to = list.find_first_of(",;\n", from);
        if (to == std::string::npos) to = list.size();
        const size_t before = v.size();
        addTerm(v, list.substr(from, to - from));
        user_terms += v.size() - before;
        from = to + 1;
    }

    if (!RlvActions::isRlvEnabled() || RlvActions::canShowNearbyAgents())
    {
        uuid_vec_t ids;
        std::vector<LLVector3d> positions;
        const LLVector3d here = gAgent.getPositionGlobal();
        LLWorld::getInstance()->getAvatars(&ids, &positions, here, kNearbyRadius);
        std::vector<std::pair<F64, LLUUID>> by_distance;
        for (size_t i = 0; i < ids.size() && i < positions.size(); ++i)
        {
            if (ids[i] != gAgentID) by_distance.emplace_back(dist_vec_squared(positions[i], here), ids[i]);
        }
        std::sort(by_distance.begin(), by_distance.end(),
                  [](const std::pair<F64, LLUUID>& a, const std::pair<F64, LLUUID>& b) { return a.first < b.first; });
        if (by_distance.size() > kMaxNearbyPeople) by_distance.resize(kMaxNearbyPeople);
        for (const auto& p : by_distance)
        {
            LLAvatarName name;
            if (!LLAvatarNameCache::get(p.second, &name)) continue;
            if (RlvActions::isRlvEnabled() && !RlvActions::canShowName(RlvActions::SNC_DEFAULT, p.second)) continue;
            addTerm(v, name.getDisplayName());
            std::string legacy = name.getLegacyName();
            const std::string resident = " Resident";
            if (legacy.size() > resident.size() && legacy.compare(legacy.size() - resident.size(), resident.size(), resident) == 0)
                legacy.resize(legacy.size() - resident.size());
            addTerm(v, legacy);
        }
    }

    if (!RlvActions::isRlvEnabled() || RlvActions::canShowLocation())
    {
        if (LLViewerRegion* region = gAgent.getRegion()) addTerm(v, region->getName());
        LLViewerParcelMgr* parcels = LLViewerParcelMgr::getInstance();
        if (parcels->getAgentParcel()) addTerm(v, parcels->getAgentParcelName());
    }
    return v;
}

void haptic(F32 amplitude, F32 seconds)
{
    if (LLXrSession::active()) LLXrSession::instance().haptic(LLXR_HAND_RIGHT, amplitude, seconds);
}

void showHint(const std::string& text)
{
    sD.hint = text;
    sD.hintUntil = now() + kHintSec;
}

// Everything back to idle; the model is freed and the worker stopped (the VR session ended).
void releaseAll()
{
    sD.state = State::IDLE;
    sD.toggled = false;
    sD.field = nullptr;
    sD.serial = 0;
    sD.hint.clear();
    sD.preview.clear();
    LLSpeechToText::release();
}

// #1119: the transcript as it would be said (sendToNearbyChat's rules), "" = nothing to say.
std::string chatLine(const std::string& text)
{
    std::string utf8 = text;
    LLStringUtil::trim(utf8);
    while (!utf8.empty() && utf8.front() == '/') utf8.erase(0, 1);   // never a channel / gesture command
    LLStringUtil::trim(utf8);
    return utf8;
}

void cancelPreview(const char* why)
{
    if (sD.state != State::PREVIEW) return;
    sD.state = State::IDLE;
    sD.preview.clear();
    LL_INFOS("SpeechToText") << "#1119 dictation: preview cancelled (" << why << "), nothing sent" << LL_ENDL;
}

void startPreview(const std::string& text)
{
    const std::string line = chatLine(text);
    if (line.empty())
    {
        sD.state = State::IDLE;
        showHint("Nothing recognised");
        return;
    }
    sD.state = State::PREVIEW;
    sD.preview = line;
    sD.previewLeft = llclamp(kPreviewBaseSec + kPreviewPerCharSec * (F32)line.size(), kPreviewBaseSec, kPreviewMaxSec);
    sD.previewSince = now();
    sD.hint.clear();
    sD.hintUntil = 0.0;
    haptic(0.2f, 0.02f);
    LL_INFOS("SpeechToText") << "#1119 dictation: preview shown (" << line.size() << " chars), says it in nearby chat in "
                             << sD.previewLeft << " s unless the right stick is pressed" << LL_ENDL;
}

void sendPreview()
{
    const std::string line = sD.preview;
    sD.state = State::IDLE;
    sD.preview.clear();
    const bool sent = LLVRDictation::sendToNearbyChat(line);
    LL_INFOS("SpeechToText") << "#1119 dictation: " << (sent ? "sent to nearby chat" : "nothing sent") << " ("
                             << line.size() << " chars, " << (S32)((now() - sD.previewSince) * 1000.0)
                             << " ms after the preview showed)" << LL_ENDL;
    if (sent)
    {
        haptic(0.3f, 0.04f);
        showHint("Said: " + line);
    }
}

// The session's end frees the model (free on liveness; nothing stays loaded with VR off).
// Connected on the first dictation press, never before.
void watchSession()
{
    if (sD.watching) return;
    sD.watching = true;
    LLVRAudio::addChangedCallback([]()
    {
        if (!LLVRAudio::active()) releaseAll();
    });
}

LLUICtrl* textTarget(LLFocusableElement* focus)
{
    LLUICtrl* ctrl = dynamic_cast<LLUICtrl*>(focus);
    if (!ctrl || !ctrl->acceptsTextInput() || !ctrl->getEnabled() || !ctrl->isInVisibleChain()) return nullptr;
    return ctrl;
}

// Types the text into the field at its cursor, as keyboard text input arrives
// (LLViewerWindow::handleUnicodeChar -> the keyboard focus), so every text control takes it
// the same way (line / text editors, the chat bar, combo box entries; undo works).
void typeInto(LLUICtrl* ctrl, const std::string& text)
{
    std::string utf8 = text;
    const std::string have = ctrl->getValue().asString();
    if (!have.empty() && have.back() != ' ' && have.back() != '\n') utf8 = " " + utf8;
    const LLWString w = utf8str_to_wstring(utf8);
    for (llwchar c : w)
    {
        if (c < 0x20 || c == 0x7f) continue;   // (no Enter: nothing is ever sent)
        ctrl->handleUnicodeChar(c, false);
    }
}

void askToDownload(const LLSpeechToText::Model& m)
{
    if (sD.consentOpen) return;
    sD.consentOpen = true;
    const LLSpeechToText::Model* model = &m;   // (static table)
    LLSD args;
    args["SIZE"] = llformat("%.0f", (F64)m.bytes / 1.0e6);
    // (a model with a fallback is the bigger, more accurate one)
    args["KIND"] = m.multilingual ? "multilingual" : LLSpeechToText::fallbackFor(m) ? "English, more accurate" : "English";
    LLNotificationsUtil::add("VRDictationDownloadModel", args, LLSD(),
        [model](const LLSD& notification, const LLSD& response)
        {
            sD.consentOpen = false;
            if (LLNotificationsUtil::getSelectedOption(notification, response) != 0) return;
            LL_INFOS("SpeechToText") << "The user agreed to download the speech model " << model->file << LL_ENDL;
            LLSpeechToText::downloadModel(*model, [](bool ok)
            {
                LLNotificationsUtil::add(ok ? "VRDictationModelReady" : "VRDictationModelFailed");
            });
        });
}

void onPress(bool wall_up)
{
    // #1119: a press while the preview shows cancels it (a tap: nothing is said; held: a new
    // recording, which replaces it).
    sD.pressCancelledPreview = sD.state == State::PREVIEW;
    if (sD.pressCancelledPreview)
    {
        cancelPreview("right stick pressed");
        haptic(0.15f, 0.02f);
        showHint("Not sent");
    }
    if (!LLSpeechToText::available())
    {
        showHint("Dictation is not available here (it needs a CPU with AVX2)");
        return;
    }
    Target target = Target::FIELD;
    LLFocusableElement* field = nullptr;
    if (wall_up)
    {
        field = gFocusMgr.getKeyboardFocus();
        if (!textTarget(field))
        {
            showHint("Select a text field first, then hold the right stick to dictate");
            haptic(0.15f, 0.02f);
            return;
        }
    }
    else
    {
        target = Target::NEARBY_CHAT;   // #1119: the wall is hidden: speech goes to nearby chat (after the preview)
    }
    if (sD.state == State::TRANSCRIBING)
    {
        showHint("Still transcribing...");
        return;
    }
    const std::string language = dictationLanguage();
    const LLSpeechToText::Model* use = &LLSpeechToText::modelFor(language);
    if (LLSpeechToText::downloading()) return;   // (the pill shows the progress)
    if (!LLSpeechToText::modelPresent(*use))
    {
        // The preferred (bigger) model is not here yet: dictate with the smaller one that is, and
        // offer the download once per session. With none on disk, the download is asked for first.
        const LLSpeechToText::Model* fallback = LLSpeechToText::fallbackFor(*use);
        if (!fallback || !LLSpeechToText::modelPresent(*fallback))
        {
            askToDownload(*use);
            return;
        }
        if (!sD.upgradeOffered)
        {
            sD.upgradeOffered = true;
            askToDownload(*use);
        }
        use = fallback;
    }
    sD.model = use;
    watchSession();
    if (!LLSpeechToText::startCapture(LLVRAudio::headsetMicName()))
    {
        showHint("No microphone found for dictation");
        return;
    }
    sD.state = State::LISTENING;
    sD.target = target;
    sD.field = field;
    sD.hint.clear();
    haptic(0.35f, 0.03f);
}

void onRelease()
{
    const bool toggled = sD.toggled;   // (#1120: started by the wrist's button, stopped by a press)
    sD.toggled = false;
    std::vector<S16> pcm = LLSpeechToText::stopCapture();
    haptic(0.2f, 0.02f);
    sD.clipSec = (F32)pcm.size() / 16000.f;
    // A short TAP (not dictation) sends the focused field: Enter through the keyboard path, so the
    // field commits exactly as the Enter key would (nearby chat says it; RLVa applies there).
    if (sD.target == Target::FIELD && sD.clipSec < kTapSec)
    {
        sD.state = State::IDLE;
        LLFocusableElement* focus = gFocusMgr.getKeyboardFocus();
        LLUICtrl* ctrl = (focus && focus == sD.field) ? textTarget(focus) : nullptr;
        sD.field = nullptr;
        if (ctrl && !ctrl->getValue().asString().empty() && gViewerWindow)
        {
            gViewerWindow->handleKey(KEY_RETURN, MASK_NONE);
            LL_INFOS("SpeechToText") << "dictation: tap -> Enter on the focused field" << LL_ENDL;
        }
        else
        {
            showHint("Hold the right stick to dictate, tap it to send");
        }
        return;
    }
    // #1119: a tap with the wall hidden is no dictation: it cancelled the preview (if one showed).
    if (sD.target == Target::NEARBY_CHAT && sD.clipSec < kTapSec)
    {
        sD.state = State::IDLE;
        showHint(toggled ? "Nothing heard"
                         : sD.pressCancelledPreview ? "Not sent" : "Hold the right stick and speak to say it in nearby chat");
        return;
    }
    if (!LLSpeechToText::trimSilence(pcm))
    {
        LL_INFOS("SpeechToText") << "dictation: " << sD.clipSec << " s clip held no speech; nothing transcribed" << LL_ENDL;
        sD.state = State::IDLE;
        showHint("Nothing heard");
        return;
    }
    if (!sD.model) sD.model = &LLSpeechToText::modelFor(dictationLanguage());
    sD.serial = LLSpeechToText::transcribe(std::move(pcm), dictationLanguage(), *sD.model, dictationVocabulary());
    if (!sD.serial)
    {
        sD.state = State::IDLE;
        showHint("Still transcribing the last one; try again");
        return;
    }
    sD.state = State::TRANSCRIBING;
    sD.releasedAt = now();
}

void deliver(const LLSpeechToText::Result& r)
{
    sD.state = State::IDLE;
    const F64 latency = now() - sD.releasedAt;
    // (The transcript itself is the user's speech: never logged.)
    LL_INFOS("SpeechToText") << "dictation: " << sD.clipSec << " s held, " << r.audioSec << " s of speech, inference "
                             << (S32)r.inferMs << " ms, release -> text " << (S32)(latency * 1000.0) << " ms, "
                             << r.text.size() << " chars" << (r.ok ? "" : ", failed: " + r.error) << LL_ENDL;
    if (!r.ok)
    {
        showHint("Dictation failed: " + r.error);
        return;
    }
    if (r.text.empty())
    {
        showHint("Nothing recognised");
        return;
    }
    if (sD.target == Target::NEARBY_CHAT)
    {
        startPreview(r.text);   // (said when the preview's time is up, unless cancelled)
        return;
    }
    LLFocusableElement* focus = gFocusMgr.getKeyboardFocus();
    LLUICtrl* ctrl = (focus && focus == sD.field) ? textTarget(focus) : nullptr;
    sD.field = nullptr;
    if (!ctrl)
    {
        showHint("The text field lost focus; nothing was typed");
        return;
    }
    // A new dictation REPLACES what the field holds (owner: so a bad result can just be tried
    // again); the field's own value setter clears it, then the text arrives as typed input.
    ctrl->setValue(LLSD(std::string()));
    typeInto(ctrl, r.text);
}

// `text` word-wrapped to `max_px` onto at most kMaxPillLines lines (the last one ends in "..."
// when it does not all fit), appended to `out`.
void wrapLines(const LLFontGL* font, const std::string& text, F32 max_px, std::vector<std::string>& out)
{
    const LLWString w = utf8str_to_wstring(text);
    size_t off = 0;
    while (off < w.size() && out.size() < kMaxPillLines)
    {
        while (off < w.size() && w[off] == ' ') ++off;
        if (off >= w.size()) break;
        S32 n = font->maxDrawableChars(w.c_str() + off, max_px, (S32)(w.size() - off), LLFontGL::WORD_BOUNDARY_IF_POSSIBLE);
        if (n <= 0) n = 1;
        LLWString line = w.substr(off, (size_t)n);
        off += (size_t)n;
        if (out.size() + 1 == kMaxPillLines && off < w.size())
        {
            const LLWString dots = utf8str_to_wstring("...");
            while (!line.empty() && font->getWidth((line + dots).c_str()) > (S32)max_px) line.pop_back();
            line += dots;
        }
        out.push_back(wstring_to_utf8str(line));
    }
}

// The pill: a dark box with a status dot and its lines of text (top first), in scaled UI
// coordinates (the caller set the UI scale); text_w > 0 = that fixed text width.
void drawPill(S32 x, S32 y, const std::vector<std::string>& lines, const LLColor4& dot, S32 text_w)
{
    const LLFontGL* font = LLFontGL::getFontSansSerif();
    if (!font || lines.empty()) return;
    const S32 pad = 6, dot_w = 16;
    const S32 line_h = font->getLineHeight();
    if (text_w <= 0)
        for (const std::string& l : lines) text_w = llmax(text_w, font->getWidth(l));
    const S32 w = text_w + 2 * pad + dot_w;
    const S32 h = line_h * (S32)lines.size() + 2 * pad;
    const S32 win_w = gViewerWindow->getWindowWidthScaled(), win_h = gViewerWindow->getWindowHeightScaled();
    x = llclamp(x, 4, llmax(4, win_w - w - 4));
    y = llclamp(y, 4, llmax(4, win_h - h - 4));
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gl_rect_2d(x, y + h, x + w, y, LLColor4(0.f, 0.f, 0.f, 0.78f), true);
    gGL.color4fv(dot.mV);
    gl_circle_2d((F32)(x + pad + 5), (F32)(y + h - pad - line_h / 2), 5.f, 16, true);
    gGL.flush();
    for (size_t i = 0; i < lines.size(); ++i)
    {
        const F32 ly = (F32)(y + pad + line_h * (S32)(lines.size() - 1 - i));
        font->renderUTF8(lines[i], 0, (F32)(x + pad + dot_w), ly, LLColor4::white, LLFontGL::LEFT, LLFontGL::BOTTOM,
                         LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
    }
}

// What the pill says now: the status line, the preview's transcript ("" if none) and the dot.
// False: nothing to show (an expired hint is cleared here). pulse: the listening dot pulses
// (the wall's pill, redrawn every frame; the wrist panel's image keeps it steady).
bool statusContent(std::string& text, std::string& quote, LLColor4& dot, bool pulse)
{
    const bool downloading = LLSpeechToText::downloading();
    if (sD.state == State::IDLE && !downloading && sD.hintUntil == 0.0) return false;
    const bool direct = sD.target == Target::NEARBY_CHAT;
    quote.clear();
    dot = LLColor4(0.6f, 0.6f, 0.6f, 1.f);
    if (sD.state == State::LISTENING)
    {
        text = sD.toggled ? llformat("Nearby chat: listening... %.1f s (press Stop to send)", LLSpeechToText::captureSeconds())
             : direct     ? llformat("Nearby chat: listening... %.1f s (release to stop)", LLSpeechToText::captureSeconds())
                          : llformat("Listening... %.1f s  (release to transcribe)", LLSpeechToText::captureSeconds());
        // A slow red pulse: the mic is live.
        const F32 a = pulse ? 0.65f + 0.35f * std::sin((F32)now() * 6.f) : 1.f;
        dot = LLColor4(1.f, 0.2f, 0.2f, a);
    }
    else if (sD.state == State::TRANSCRIBING)
    {
        text = "Transcribing...";
        dot = LLColor4(1.f, 0.8f, 0.2f, 1.f);
    }
    else if (sD.state == State::PREVIEW)
    {
        text = llformat("Saying in nearby chat in %.0f s - press the right stick to cancel",
                        std::ceil(llmax(0.f, sD.previewLeft)));
        quote = "\"" + sD.preview + "\"";
        dot = LLColor4(0.3f, 0.9f, 0.4f, 1.f);
    }
    else if (downloading)
    {
        text = llformat("Downloading the speech model... %d%%", (S32)(LLSpeechToText::downloadProgress() * 100.f));
        dot = LLColor4(0.35f, 0.85f, 1.f, 1.f);
    }
    else if (now() < sD.hintUntil)
    {
        text = sD.hint;
    }
    else
    {
        sD.hintUntil = 0.0;
        sD.hint.clear();
        return false;
    }
    return true;
}
} // namespace

void LLVRDictation::tick(const LLXrInputState& in, bool wall_up)
{
    // Results first (a release last frame may have its text by now).
    if (sD.state == State::TRANSCRIBING || LLSpeechToText::busy())
    {
        LLSpeechToText::Result r;
        while (LLSpeechToText::pollResult(r))
        {
            if (sD.state == State::TRANSCRIBING && r.serial == sD.serial) deliver(r);
        }
    }

    // The on-screen keyboard (the fallback): on the wall while a text field has focus.
    LLFloaterVRKeyboard::updateVR(wall_up);

    // #1119: the preview counts down only while it is on show (the wrist panel took the pill
    // this frame, or the wall is up), then the line is said.
    const F64 t = now();
    const F32 dt = sD.lastTick > 0.0 ? (F32)llclamp(t - sD.lastTick, 0.0, 0.1) : 0.f;
    sD.lastTick = t;
    // (not while the runtime's dashboard covers it: the session is running but not focused)
    const bool shown = in.focused && (sD.shownThisFrame || wall_up);
    sD.shownThisFrame = false;
    if (sD.state == State::PREVIEW)
    {
        if (shown) sD.previewLeft -= dt;
        if (sD.previewLeft <= 0.f) sendPreview();
        else if (t - sD.previewSince > kPreviewStaleSec) cancelPreview("never on show (no left controller?)");
    }

    const LLXrHandState& rh = in.hand[LLXR_HAND_RIGHT];
    const bool button = in.focused && rh.active && rh.stickClick;
    const F32 pushed = std::sqrt(rh.stick[0] * rh.stick[0] + rh.stick[1] * rh.stick[1]);
    if (button && !sD.prevButton && pushed <= kStickPushed)
    {
        // #1120: a capture the wrist's button started ends at the next press (transcribed).
        if (sD.toggled && sD.state == State::LISTENING) onRelease();
        else onPress(wall_up);
    }
    sD.prevButton = button;

    if (sD.state != State::LISTENING) return;
    if (!in.focused || (!rh.active && !sD.toggled))
    {
        cancel();   // dashboard / controller gone: nothing is transcribed
        return;
    }
    LLSpeechToText::pollCapture();
    if ((!sD.toggled && !button) || LLSpeechToText::captureSeconds() >= kMaxClipSec) onRelease();
}

void LLVRDictation::cancel()
{
    // A preview is not said while the session cannot show it (dashboard, headset off).
    cancelPreview("the session lost focus");
    if (sD.state != State::LISTENING) return;
    LLSpeechToText::stopCapture();   // (discarded)
    sD.state = State::IDLE;
    sD.toggled = false;
    sD.field = nullptr;
    LL_INFOS("SpeechToText") << "dictation cancelled (session lost focus)" << LL_ENDL;
}

bool LLVRDictation::wristStatus(bool wall_up, std::string& status, std::string& transcript, F32 dot[4])
{
    if (sD.state == State::IDLE && sD.hintUntil == 0.0 && !LLSpeechToText::downloading()) return false;   // (idle)
    // With the wall up a field's dictation shows above its field (draw()); the wrist shows what
    // goes to nearby chat, and everything while the wall is hidden.
    if (wall_up && sD.target != Target::NEARBY_CHAT) return false;
    LLColor4 c;
    if (!statusContent(status, transcript, c, false)) return false;
    for (int i = 0; i < 4; ++i) dot[i] = c.mV[i];
    return true;
}

void LLVRDictation::noteShownOnWrist()
{
    sD.shownThisFrame = true;
}

void LLVRDictation::wristToggle()
{
    if (sD.state == State::LISTENING)
    {
        if (sD.toggled) onRelease();   // Stop: transcribe it (then the preview)
        return;                        // (a right-stick hold owns this capture)
    }
    if (sD.state == State::PREVIEW)
    {
        cancelPreview("the wrist's button pressed");
        haptic(0.15f, 0.02f);
        showHint("Not sent");
        return;
    }
    onPress(false);   // the #1119 path: nearby chat, after the preview
    if (sD.state == State::LISTENING)
    {
        sD.toggled = true;
        LL_INFOS("SpeechToText") << "#1120 dictation: started from the wrist panel (press again to stop)" << LL_ENDL;
    }
}

bool LLVRDictation::wristListening()
{
    return sD.toggled && sD.state == State::LISTENING;
}

bool LLVRDictation::holdsVoiceMic()
{
    return sD.state == State::LISTENING;
}

void LLVRDictation::draw()
{
    if (sD.state == State::IDLE && !LLSpeechToText::downloading() && sD.hintUntil == 0.0) return;
    if (!LLXrSession::active() || !gViewerWindow) return;
    const LLFontGL* font = LLFontGL::getFontSansSerif();
    if (!font) return;
    std::string text, quote;
    LLColor4 dot;
    if (!statusContent(text, quote, dot, true)) return;
    std::vector<std::string> extra;   // (the preview's transcript, wrapped)
    if (!quote.empty()) wrapLines(font, quote, (F32)kWidePillTextWidth, extra);

    // Above the field being dictated into (while it still has focus): one line. Else low in the
    // middle at a fixed width, wrapped (the wrist panel draws the same content: wristStatus()).
    std::vector<std::string> lines;
    S32 text_w = 0;
    S32 x = 0, y = 60;
    LLFocusableElement* focus = gFocusMgr.getKeyboardFocus();
    LLUICtrl* above = (sD.state != State::IDLE && sD.target == Target::FIELD && focus && focus == sD.field)
                          ? textTarget(focus) : nullptr;
    if (above)
    {
        const LLRect r = above->calcScreenRect();
        x = r.mLeft;
        y = r.mTop + 4;
        lines.push_back(text);
    }
    else
    {
        text_w = kWidePillTextWidth;
        wrapLines(font, text, (F32)text_w, lines);
        lines.insert(lines.end(), extra.begin(), extra.end());
        if (lines.size() > kMaxPillLines + 1) lines.resize(kMaxPillLines + 1);
        x = gViewerWindow->getWindowWidthScaled() / 2 - text_w / 2;
    }
    // Scaled UI coordinates, as LLVRInput::drawCursor (this runs outside any UI scale).
    const LLVector2& ds = gViewerWindow->getDisplayScale();
    gGL.pushUIMatrix();
    gGL.scaleUI(ds.mV[VX], ds.mV[VY], 1.f);
    drawPill(x, y, lines, dot, text_w);
    gGL.flush();
    gGL.popUIMatrix();
}

bool LLVRDictation::sendToNearbyChat(const std::string& text)
{
    const std::string utf8 = chatLine(text);
    if (utf8.empty()) return false;
    static LLCachedControl<bool> s_anim(gSavedSettings, "PlayChatAnim", true);
    LLFloaterIMNearbyChat::sendChatFromViewer(utf8, CHAT_TYPE_NORMAL, s_anim);
    return true;
}
