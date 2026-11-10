/**
 * @file llvrlobby.h
 * @brief VR milestone M3 (#1076, epic #1072): the login room.
 *
 * Before the world shows (the login screen, then the login's progress), the headset shows a
 * dark room: the eyes' dark backdrop, low mist along the floor and a single glowing doorway,
 * with the login screen on the UI wall (as before). The doorway stands to the right of the wall
 * (62 degrees off its centre, 2.3 m from where you stood when the wall was placed), facing you.
 * On the login screen, pointing a controller at it lights it up (a haptic tick), and the
 * trigger -- or physically walking through it -- logs in, exactly as the login panel's Log In
 * button (the LLPanelLogin event API's onClickConnect). When the world is ready it fades in
 * around you (LLVRInput's comfort fade) and the room is gone.
 *
 * Composition layers only (LLXrSession::setLobby): no renderer work, no world. The images are
 * made once per session on the first room frame.
 *
 * Desktop (no VR): update() is only called from LLVRInput::update, which returns first.
 */
#pragma once

struct LLXrInputState;
struct LLXrHeadPose;

class LLVRLobby
{
public:
    // LLVRInput::update, after the UI wall's layers were set (VR only). inWorld: the
    // head-tracked world is shown (the room is then off).
    static void update(const LLXrInputState& in, const LLXrHeadPose& hp, bool inWorld);
};
