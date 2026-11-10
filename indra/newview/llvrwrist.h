/**
 * @file llvrwrist.h
 * @brief VR wrist panel (#1120, M3 #1076, epic #1072): IMs, nearby chat, notifications and
 *        dialogs, and the quick actions, on a panel of its own that follows the left hand.
 *
 * The panel is NOT a crop of the desktop UI any more (that was an 18 cm quad showing the
 * toasts at desktop text size). It is its own small image, drawn here with large text into a
 * facade render target (an offscreen segment of the UI build), copied by LLXrSession into a
 * small swapchain of its own and shown as one opaque quad layer in the left aim pose's space.
 *
 * Everything about its look and behaviour is data: app_settings/vr_wrist_panel.xml (LLSD) --
 * size in millimetres, text line height, mount on the controller, when it shows, the touch
 * distances, colours, the feed rows and the quick actions (id, label string, action). The
 * built-in defaults below match that file, so a missing or broken file still gives the panel.
 * The drawing is the only part tied to the current UI path (gGL + LLFontGL into a render
 * target); the model (feed), layout (rects in mm) and placement do not depend on it.
 *
 *  - Shows when the left controller is turned toward the face (its mount normal toward the
 *    head, hysteresis + a short delay), in the world, with or without the UI wall. It tilts
 *    about its own X axis toward the eyes (clamped) so it is read face-on.
 *  - The right hand works it two ways: the LASER (its aim ray; trigger = press) and TOUCH
 *    (the right controller's tip pushed onto the panel; press on entry, release on backing
 *    off). Both give a haptic tick. The cursor / fingertip is drawn on the panel (a quad
 *    layer hides the hand behind it).
 *  - Feed: the latest IMs, nearby chat lines and notifications (fixed ring). A dialog that
 *    needs an answer shows its (up to 3) buttons plus "Open on wall". Clicking a feed row
 *    brings up the wall with that conversation / the notifications focused.
 *  - Quick actions (layout): wall toggle, dictation (#1119 straight to nearby chat: press to
 *    listen, press again to stop; LLVRDictation::wristToggle), recentre, fly cam.
 *  - #1119 dictation to nearby chat (listening, transcribing, the preview and its countdown,
 *    hints) is drawn over the feed (LLVRDictation::wristStatus) and keeps the panel shown.
 *
 * VR off / desktop: nothing here runs. update() is called only from LLVRInput::update past its
 * VR test, render() returns on one static read (LLXrSession::active()), onNearbyChat() is one
 * static read per chat line. The render target, swapchain, font and signal connections are
 * made on the first VR frame that shows the panel.
 *
 * Log tag "VRWrist".
 */
#pragma once

#include "stdtypes.h"

class LLChat;
struct LLXrHeadPose;
struct LLXrInputState;
struct LLXrUiLayers;

class LLVRWrist
{
public:
    // What the panel asks LLVRInput to do (it owns the wall and the fly cam's blink).
    struct Requests
    {
        bool wallShow = false;     // bring the wall up (placed in front of you) if it is hidden
        bool wallToggle = false;   // the wall button
        bool flyCam = false;       // the fly cam button (as the B button: on, or the blink return)
    };

    // LLVRInput::update, every frame of a running session, after the head and controllers were
    // read. enabled: the world is shown and the UI is built (no panel otherwise). wallUp: the UI
    // wall is shown. Sets ui.wrist* (the layer) and fills req. dt: seconds since last frame.
    static void update(const LLXrInputState& in, const LLXrHeadPose& hp, bool enabled, bool wallUp, F32 dt,
                       LLXrUiLayers& ui, Requests& req);
    // The right hand's ray or fingertip is on the panel this frame: it owns the trigger, and the
    // wall / world pointer must ignore that hand. beam: the laser's length to the panel (m).
    static bool rightOnPanel(F32& beam);
    // Session not running (paused, dashboard, lost): drop hover / press state. Cheap.
    static void reset();
    // displayVulkanUI, at the top of the UI build (the offscreen-segment slot, beside the
    // dynamic textures): redraws the panel's image when it changed and hands it to the session.
    static void render();
    // LLNotificationManager::onChat: a nearby chat line (VR only; one static read otherwise).
    static void onNearbyChat(const LLChat& chat);
};
