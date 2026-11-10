/**
 * @file llvrdictation.h
 * @brief VR dictation (#1118, epic #1072): push-to-talk speech-to-text into the focused text
 *        field on the UI wall. Engine, capture and model: LLSpeechToText (llspeechtotext.h).
 *
 * The button: the RIGHT THUMBSTICK PRESS, held (Index / Touch; bound and unused until now --
 * the left stick press is the recentre hold, A / B / trigger are taken). A press that moves
 * the stick more than half way is not a dictation press.
 *  - The UI wall is up and a text field has keyboard focus (nearby chat, an IM, search, a
 *    notecard, ...): hold -> "Listening" pill above the field (and a haptic tick), speak,
 *    release -> "Transcribing" -> the text is typed into that field at its cursor, for
 *    editing. Never sent: the user presses Enter / Send as for typed text.
 *  - The wall is up but no text field has focus: a hint ("Select a text field first").
 *  - #1119, the wall is hidden: hold -> "Nearby chat: listening" on the wrist panel (#1120:
 *    LLVRWrist draws wristStatus() over its feed), release -> "Transcribing" -> a PREVIEW
 *    of the line, counting down (2.5 s + 30 ms a character, at most 6 s; counted only while
 *    it is on show) -> said in nearby chat, then "Said: ..." for 3 s. Cancel: press the right
 *    stick while the preview shows (a tap sends nothing; held, it records again and the new
 *    line replaces it), or the session loses focus. Nothing is said for an empty / near-silent
 *    clip. RLVa applies as for typed chat (sendToNearbyChat).
 *  - #1120 the wrist panel's Dictate button: the same #1119 path to nearby chat, wall up or
 *    not, as a toggle (press: listening; press it again or the right stick: transcribe).
 * The speech model is not shipped: the first press asks to download it (size shown;
 * SHA-256 checked); the pill shows the progress.
 * While dictation listens, the voice-chat microphone is muted (audio_update_volume), so
 * dictated speech never reaches voice chat.
 *
 * Setting: SlipstreamVRDictationLanguage ("en" = English model; a language code or "auto"
 * = the multilingual model).
 *
 * Desktop / VR off: nothing here runs. tick() and cancel() are only called from
 * LLVRInput::update past its VR test, draw() and holdsVoiceMic() are one static read.
 */
#pragma once

#include "stdtypes.h"

#include <string>

struct LLXrInputState;

class LLVRDictation
{
public:
    // LLVRInput::update, every frame of a running session (after the controllers were read).
    // wall_up: the UI wall is shown this frame. Also places the on-screen keyboard, the
    // fallback (LLFloaterVRKeyboard::updateVR).
    static void tick(const LLXrInputState& in, bool wall_up);
    // LLVRInput::update while the session is not running (paused, dashboard, lost): stops a
    // capture without transcribing it. Cheap when idle.
    static void cancel();
    // The UI build, after the 2D UI (with the pointer's cursor): the listening / transcribing /
    // download pill and hints. Nothing when idle.
    static void draw();
    // Dictation is listening: voice chat's mic is muted meanwhile (llvieweraudio.cpp).
    static bool holdsVoiceMic();
    // #1119 / #1120, LLVRWrist::update: what the wrist panel shows over its feed for speech
    // going to nearby chat (or anything, with the wall hidden): the status line (listening,
    // transcribing, the preview's countdown, the download, a hint), the preview's transcript
    // ("" if none) and the status dot's colour (RGBA). False when there is nothing (one static
    // read when idle). The panel is shown while this is true.
    static bool wristStatus(bool wall_up, std::string& status, std::string& transcript, F32 dot[4]);
    // The wrist panel shows wristStatus() this frame: the preview counts down while on show.
    static void noteShownOnWrist();
    // #1120 the wrist's Dictate button (the #1119 path, nearby chat): starts a capture that runs
    // until the button or the right stick is pressed again (or the 30 s clip limit), stops such
    // a capture, or cancels a preview (nothing said).
    static void wristToggle();
    // A capture started by wristToggle() is listening (the button reads "Stop").
    static bool wristListening();

    // #1119: say a transcript in nearby chat (channel 0), as typed chat (RLVa redirects /
    // sendchat filters apply in LLFloaterIMNearbyChat::sendChatFromViewer). Empty text is not
    // sent; a leading '/' is dropped (a transcript never becomes a channel / gesture command).
    // Returns false when nothing was sent.
    static bool sendToNearbyChat(const std::string& text);
};
