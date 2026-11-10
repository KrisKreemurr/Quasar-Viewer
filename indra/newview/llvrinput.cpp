/**
 * @file llvrinput.cpp
 * @brief VR milestone M3 (#1076): what the controllers do. See llvrinput.h.
 */
#include "llviewerprecompiledheaders.h"

#include "llvrinput.h"

#include "llagent.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llrootview.h"
#include "llcleancursorpick.h"
#include "llfocusmgr.h"
#include "llkeyboard.h"
#include "llmanip.h"   // #1141 LLManip::vrPointerOnHandle (the beam ends at an edit handle)
#include "llmenugl.h"
#include "llstartup.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"
#include "piemenu.h"
#include "llvoavatarself.h"
#include "llrender2dutils.h"
#include "llrender.h"
#include "llselectmgr.h"
#include "lltool.h"
#include "lltoolmgr.h"
#include "lltoolpie.h"
#include "llui.h"
#include "llviewerobject.h"
#include "llviewerwindow.h"
#include "llvrcamera.h"
#include "llvrdictation.h"
#include "llvrgrab.h"
#include "llvrhands.h"
#include "llvrlobby.h"
#include "llvrteleport.h"
#include "llvrsafety.h"
#include "llvrwrist.h"
#include "llwindow.h"
#include "vkgfx/llxrsession.h"

#include <chrono>
#include <cmath>

namespace
{
// The world pointer's beam when the ray hits nothing the pick reports (sky, past the draw
// distance), metres.
constexpr F32 kWorldBeamMax = 30.f;

struct VRInputState
{
    // UI wall.
    bool wallShown = false;          // in the world: shown by the user (menu button / View menu)
    bool placed = false;             // ui.wallAxis / wallYaw hold a placement
    bool placePending = false;       // a placement waits for a placeable head pose
    uint32_t spaceGeneration = 0;    // the app space's (a runtime recentre makes a placement stale)
    bool toggleRequest = false;      // View > Show UI Wall in VR (#1120: or the wrist panel's wall button)
    bool showRequest = false;        // #1120 the wrist panel asks for the wall (an item opened on it)
    bool prevMenu = false;
    LLXrUiLayers ui;

    // Pointer.
    U32 pointer = LLXR_HAND_RIGHT;   // the hand whose ray drives the mouse
    bool prevSelect[LLXR_HAND_COUNT] = {false, false};
    bool leftDown = false, rightDown = false;   // mouse buttons this pointer holds down
    bool onWall = false;             // the pointer's ray hits the wall this frame
    bool hitLogged[LLXR_HAND_COUNT] = {false, false};
    // Click diagnostics (#1076): the pointer hand's last select, the logged-press count, and
    // whether the matching mouse up of a logged press is logged too.
    bool prevPointerSelect = false;
    U32 clickLogs = 0;
    U32 selectEdgeLogs = 0;
    bool logUp = false;
    // The world pointer (#1076): the pointer hand's ray picks the world (wall hidden, ray off
    // the wall, or on a transparent part of it) through LLCleanCursorPick's VR ray.
    bool worldDown = false;          // a left click on the world is held
    bool worldLogUp = false;
    S32 worldX = 0, worldY = 0;      // the click's (scaled) mouse point
    F32 worldBeam = kWorldBeamMax;   // the beam's last known length (m)
    bool onWristRight = false;       // #1120 the right ray / fingertip is on the wrist panel (LLVRWrist owns that hand)
    bool prevContext[LLXR_HAND_COUNT] = {false, false};
    bool prevWorldSelect = false;    // the pointer hand's select last frame (world press edge)
    S32 px = -1, py = -1;            // the pointer's mouse point (window pixels, top-left origin)
    S32 sentX = -1, sentY = -1;      // ... as last sent as a mouse move
    F32 scrollAcc = 0.f;
    bool smoothValid = false;        // su / sv: the smoothed hit (u, v) of smoothHand
    U32 smoothHand = 0;
    F32 su = 0.f, sv = 0.f;
    std::chrono::steady_clock::time_point holdUntil{};   // the point holds still after a press
    std::chrono::steady_clock::time_point last{};

    // The floating context menu panel (#1134): the open context menu's part of the UI image on a
    // world-locked quad near the pointed spot. The menu itself is the desktop's (LLToolPie).
    bool panelOpen = false;
    uint32_t panelGen = 0;           // the app space generation it was placed in
    float panelP[3] = {0.f, 0.f, 0.f};   // the quad centre at placement (app space)
    float panelYaw = 0.f;            // turned about +Y to face the head
    float panelScale = 0.002f;       // metres per UI image pixel
    float panelCx0 = 0.f, panelCy0 = 0.f;   // the menu rect's centre (image pixels) at placement
    bool ctxEdge[LLXR_HAND_COUNT] = {false, false};   // the context button went down this frame
    bool onPanel[LLXR_HAND_COUNT] = {false, false};   // the hand's ray hits the panel (first)
    // #1142: the panel takes no press until every trigger / context button was let go after it
    // opened, and then only a fresh press edge. The context button that opened the menu is
    // still held when the panel appears under the ray the next frame; as a level it became a
    // right mouse down on the item there, and its release a right mouse up that committed that
    // item (LLContextMenu::handleRightMouseUp) -- or, off an item, a right down the menu did
    // not take, which hid it (LLMenuHolderGL::handleRightMouseDown): "closes at once".
    bool panelAwaitRelease = false;
    bool panelIgnoreLogged = false;
    U32 panelGateLogs = 0;

    // Locomotion.
    bool snapArmed = true;           // the turn stick came back to the centre since the last snap
    bool recenterArmed = true;       // the left stick press may still become a recentre
    F32 recenterHold = 0.f;          // ... held this long (s)
    // #1136: a short click of the left stick (released inside kRunClickMax) toggles always-run.
    bool prevStickClick = false;     // the left stick was pressed last frame
    bool runClickArmed = false;      // the current press may still become a run toggle
    F32 runClickHold = 0.f;          // ... held this long (s)

    // Fly cam and its return blink.
    enum Blink : U8 { BLINK_NONE, BLINK_OUT, BLINK_HOLD, BLINK_IN };
    Blink blink = BLINK_NONE;
    F32 fade = 0.f;                  // 0 .. 1 black (LLXrSession::setFade)
    bool prevFly = false;

    // #1078 comfort fades (with the blink's, the larger goes to setFade): the world fades in
    // around you when it first shows, and goes dark across a teleport (fade in on arrival).
    F32 worldFade = 0.f;
    F32 worldHold = 0.f;             // seconds the world stays dark before fading in
    F32 worldFadeIn = 0.5f;          // ... and then the fade in's length (s)
    bool prevInWorld = false;
    bool prevTeleport = false;
    // #1078 the comfort vignette (LLXrSession::setVignette), eased.
    F32 vignette = 0.f;
    bool vignetteLogged = false;
    // #1078 haptic hover ticks: the clickable view last under the pointer (compared only).
    const LLView* hoverTarget = nullptr;
    bool flyRequest = false;         // #1120 the wrist panel's fly cam button (a B press)
};
VRInputState sIn;

F32 wrapAngle(F32 a)
{
    while (a > F_PI) a -= F_TWO_PI;
    while (a < -F_PI) a += F_TWO_PI;
    return a;
}

// Fly cam on / off (Index / Touch right B). On: the view leaves the body where it is (no
// motion, no blink needed). Off: a comfort blink -- fade to black (150 ms), jump back onto the
// avatar while dark (one fully black frame), fade in (150 ms); never a fly-back.
void flyCamTick(const LLXrInputState& in, bool inWorld, F32 dt)
{
    const LLXrHandState& rh = in.hand[LLXR_HAND_RIGHT];
    const bool btn = in.focused && rh.active && rh.flyCam;
    const bool wristPress = sIn.flyRequest;   // #1120 (the same press, from the wrist panel)
    sIn.flyRequest = false;
    if (((btn && !sIn.prevFly) || wristPress) && inWorld && sIn.blink == VRInputState::BLINK_NONE)
    {
        if (!LLVRCamera::flyCam()) LLVRCamera::setFlyCam(true);
        else sIn.blink = VRInputState::BLINK_OUT;
        LLXrSession::instance().haptic(LLXR_HAND_RIGHT, 0.3f, 0.03f);
    }
    sIn.prevFly = btn;
    const F32 kBlink = 0.15f;
    switch (sIn.blink)
    {
    case VRInputState::BLINK_OUT:
        sIn.fade = llmin(1.f, sIn.fade + dt / kBlink);
        if (sIn.fade >= 1.f)
        {
            LLVRCamera::setFlyCam(false);   // composed by this frame's camera, rendered black
            sIn.blink = VRInputState::BLINK_HOLD;
        }
        break;
    case VRInputState::BLINK_HOLD:
        sIn.blink = VRInputState::BLINK_IN;
        break;
    case VRInputState::BLINK_IN:
        sIn.fade = llmax(0.f, sIn.fade - dt / kBlink);
        if (sIn.fade <= 0.f) sIn.blink = VRInputState::BLINK_NONE;
        break;
    default:
        sIn.fade = 0.f;
        break;
    }
}

// #1078 comfort fades. Entering the world: dark, held 1 s (the scene streams in), then a 0.8 s
// fade in around you -- not a cut from the login wall to a half-loaded region. A teleport
// (any teleport state, local ones included): fade to dark in 0.2 s and stay dark until it is
// over, then 0.4 s more and a 0.5 s fade in -- the world never jumps under you in full view.
// The fade lies over the world only: the wall and the wrist panel stay readable (the progress
// shows on the wall when it is up). Sent with the blink's fade (the larger).
void comfortFadeTick(bool inWorld, F32 dt)
{
    // (the world renders from STATE_STARTED on; the avatar can be valid a little earlier)
    if (!inWorld || LLStartUp::getStartupState() != STATE_STARTED)
    {
        sIn.prevInWorld = false;
        sIn.prevTeleport = false;
        sIn.worldFade = 0.f;
        sIn.worldHold = 0.f;
        LLXrSession::instance().setFade(sIn.fade);
        return;
    }
    if (!sIn.prevInWorld)
    {
        sIn.prevInWorld = true;
        sIn.worldFade = 1.f;
        sIn.worldHold = 1.f;
        sIn.worldFadeIn = 0.8f;
        LL_INFOS("VR") << "comfort fade: the world fades in around you" << LL_ENDL;
    }
    const bool teleporting = gAgent.getTeleportState() != LLAgent::TELEPORT_NONE;
    if (teleporting != sIn.prevTeleport)
    {
        sIn.prevTeleport = teleporting;
        LL_INFOS("VR") << "comfort fade: " << (teleporting ? "teleport, the world goes dark" : "arrived, the world fades in")
                       << LL_ENDL;
    }
    if (teleporting)
    {
        sIn.worldFade = llmin(1.f, sIn.worldFade + dt / 0.2f);
        sIn.worldHold = 0.4f;
        sIn.worldFadeIn = 0.5f;
    }
    else if (sIn.worldHold > 0.f)
    {
        sIn.worldHold -= dt;   // (dark, or still fading out: hold)
    }
    else
    {
        sIn.worldFade = llmax(0.f, sIn.worldFade - dt / sIn.worldFadeIn);
    }
    LLXrSession::instance().setFade(llmax(sIn.fade, sIn.worldFade));
}

// #1078 the comfort vignette: the edges of the view darken while the view moves without your
// head -- the rig's speed (walking, flying, the fly cam, a vehicle) and smooth turning --
// up to SlipstreamVRComfortVignette (0 = off). Quick in (0.08 s), slower out (0.25 s).
void vignetteTick(bool inWorld, F32 dt)
{
    static LLCachedControl<F32> s_vig(gSavedSettings, "SlipstreamVRComfortVignette", 0.5f);
    const F32 strength = llclamp((F32)s_vig, 0.f, 1.f);
    F32 speed = 0.f, rate = 0.f;
    if (inWorld && strength > 0.f) LLVRCamera::rigMotion(speed, rate);
    const F32 move = llclamp((speed - 0.4f) / 2.6f, 0.f, 1.f);
    const F32 turn = llclamp((rate - 10.f * DEG_TO_RAD) / (60.f * DEG_TO_RAD), 0.f, 1.f);
    const F32 target = strength * llmax(move, turn);
    const F32 tau = target > sIn.vignette ? 0.08f : 0.25f;
    sIn.vignette += (target - sIn.vignette) * (dt > 0.f ? 1.f - std::exp(-dt / tau) : 0.f);
    if (sIn.vignette < 0.005f) sIn.vignette = 0.f;
    if (sIn.vignette > 0.f && !sIn.vignetteLogged)
    {
        sIn.vignetteLogged = true;
        LL_INFOS("VR") << "comfort vignette: first shown (strength " << strength << "; rig " << speed << " m/s, turning "
                       << rate * RAD_TO_DEG << " deg/s)" << LL_ENDL;
    }
    LLXrSession::instance().setVignette(sIn.vignette);
}

// #1135: a script (llTakeControls, i.e. a vehicle) took any of the movement controls.
bool movementControlsTaken()
{
    return gAgent.forwardGrabbed() || gAgent.backwardGrabbed() || gAgent.rotateGrabbed() || gAgent.upGrabbed() ||
           gAgent.downGrabbed() || gAgent.isControlGrabbed(CONTROL_LEFT_POS_INDEX) ||
           gAgent.isControlGrabbed(CONTROL_LEFT_NEG_INDEX);
}

// Thumbstick locomotion while the wall is hidden (frozen while it is up: the sticks belong to
// the UI then; walking around physically still works). Left stick: walk, relative to where
// you look -- the avatar turns to your head's facing while you walk, the view held still
// (LLVRCamera::rigTurnedKeepView), then the stick's forward / sideways parts are the usual
// walk / strafe controls. Right stick: x turns (snap 30 degrees, or smooth 90 degrees a
// second with SlipstreamVRSmoothTurn), y up = jump / fly up, down = crouch / fly down.
// Left stick pressed and held 1 s (not pushed): recentre.
void locomotion(const LLXrInputState& in, bool inWorld, F32 dt)
{
    // #1078: with SlipstreamVRWallFreezesMovement off the sticks also move you while the wall is
    // up -- except the pointer hand's stick while its ray is on the wall (it scrolls the UI).
    static LLCachedControl<bool> s_freeze(gSavedSettings, "SlipstreamVRWallFreezesMovement", true);
    const bool frozen = sIn.ui.wall && s_freeze;
    LLXrHandState lh = in.hand[LLXR_HAND_LEFT];
    LLXrHandState rh = in.hand[LLXR_HAND_RIGHT];
    if (sIn.ui.wall && !frozen && (sIn.onWall || sIn.leftDown || sIn.rightDown))
    {
        LLXrHandState& busy = sIn.pointer == LLXR_HAND_LEFT ? lh : rh;
        busy.stick[0] = busy.stick[1] = 0.f;
    }
    // Recentre: a deliberate gesture -- the left stick pressed and HELD 1 s without being
    // pushed. A plain click recentred 9 times in the first headset run, 4 of them within 6 s
    // while walking with that stick (each one turns the world against the room-locked wall).
    // A press that moves the stick (> half way) is cancelled until released.
    const bool pressed = in.focused && lh.active && lh.stickClick;
    const F32 deflect = std::sqrt(lh.stick[0] * lh.stick[0] + lh.stick[1] * lh.stick[1]);
    // #1136: a short click of the same stick (down and up inside kRunClickMax, pushed or not:
    // pressing it while walking is the natural moment) toggles always-run, as the desktop's
    // toggle_run does. The 1 s recentre hold is a different gesture: a press that lasts past
    // kRunClickMax is never a toggle.
    constexpr F32 kRunClickMax = 0.45f;
    bool runToggle = false;
    if (pressed)
    {
        if (!sIn.prevStickClick)
        {
            sIn.runClickArmed = true;
            sIn.runClickHold = 0.f;
        }
        sIn.runClickHold += dt;
        if (sIn.runClickHold > kRunClickMax) sIn.runClickArmed = false;
    }
    else
    {
        runToggle = sIn.runClickArmed && sIn.prevStickClick;
        sIn.runClickArmed = false;
    }
    sIn.prevStickClick = pressed;
    if (!pressed)
    {
        sIn.recenterHold = 0.f;
        sIn.recenterArmed = true;
    }
    else if (sIn.recenterArmed)
    {
        if (deflect > 0.5f)
        {
            sIn.recenterArmed = false;   // a click while walking: not a recentre
        }
        else
        {
            sIn.recenterHold += dt;
            if (sIn.recenterHold >= 1.f)
            {
                sIn.recenterArmed = false;   // once per press
                LL_INFOS("VR") << "recentre: left stick held " << sIn.recenterHold << " s" << LL_ENDL;
                LLVRCamera::recenter();
                LLXrSession::instance().haptic(LLXR_HAND_LEFT, 0.5f, 0.06f);
            }
        }
    }
    if (!inWorld || !in.focused || frozen || LLStartUp::getStartupState() != STATE_STARTED ||
        !isAgentAvatarValid())
    {
        sIn.snapArmed = true;
        return;
    }

    if (runToggle)
    {
        if (gAgent.getAlwaysRun()) gAgent.clearAlwaysRun();
        else gAgent.setAlwaysRun();
        LL_INFOS("VR") << "run: left stick click -> always run " << (gAgent.getAlwaysRun() ? "on" : "off") << LL_ENDL;
        LLXrSession::instance().haptic(LLXR_HAND_LEFT, 0.35f, 0.04f);
    }

    if (sIn.blink != VRInputState::BLINK_NONE) return;   // (the return blink: hold still)

    // #1135: seated on something that took the controls (a scripted vehicle): the sticks are
    // its steering, as the keyboard's would be. Not while the fly cam is on (the sticks fly the view).
    if (isAgentAvatarValid() && gAgentAvatarp->isSitting() && movementControlsTaken() && !LLVRCamera::flyCam())
    {
        sIn.snapArmed = true;
        // Forward / back: the usual AT flags. Turn: the YAW flags (what a vehicle's CONTROL_ROT_*
        // reads), from either stick's x (the larger push), not the view's snap / smooth turn.
        // Where the script took the slide (strafe) controls but not the turn ones (a hover craft),
        // the left stick's x slides instead. Up / down (boats, aircraft): the right stick's y
        // (no auto-fly, no standing up: only the flags).
        const F32 lx = lh.active ? lh.stick[0] : 0.f, ly = lh.active ? lh.stick[1] : 0.f;
        const F32 rx = rh.active ? rh.stick[0] : 0.f, ry = rh.active ? rh.stick[1] : 0.f;
        if (std::fabs(ly) > 0.3f) gAgent.moveAt(ly > 0.f ? 1 : -1, false);
        const bool slideOnly = !gAgent.rotateGrabbed() &&
                               (gAgent.isControlGrabbed(CONTROL_LEFT_POS_INDEX) || gAgent.isControlGrabbed(CONTROL_LEFT_NEG_INDEX));
        if (slideOnly && std::fabs(lx) > 0.3f) gAgent.moveLeft(lx > 0.f ? -1 : 1);
        const F32 turn = slideOnly ? rx : (std::fabs(lx) >= std::fabs(rx) ? lx : rx);
        if (std::fabs(turn) > 0.3f) gAgent.moveYaw(turn > 0.f ? -1.f : 1.f, false);
        if (ry > 0.3f) gAgent.moveUp(1);
        else if (ry < -0.3f) gAgent.moveUp(-1);
        return;
    }

    // Turn (#1078: the snap angle and the smooth speed are comfort preferences).
    static LLCachedControl<bool> s_smooth(gSavedSettings, "SlipstreamVRSmoothTurn", false);
    static LLCachedControl<F32> s_snap_deg(gSavedSettings, "SlipstreamVRSnapTurnDegrees", 30.f);
    static LLCachedControl<F32> s_smooth_dps(gSavedSettings, "SlipstreamVRSmoothTurnSpeed", 90.f);
    const F32 tx = rh.active ? rh.stick[0] : 0.f;
    if (s_smooth)
    {
        const F32 rate = llclamp((F32)s_smooth_dps, 20.f, 360.f) * DEG_TO_RAD;
        if (std::fabs(tx) > 0.2f) LLVRCamera::turnView(-tx * rate * dt);
    }
    else if (sIn.snapArmed && std::fabs(tx) > 0.7f)
    {
        const F32 snap = llclamp((F32)s_snap_deg, 15.f, 90.f) * DEG_TO_RAD;
        LLVRCamera::turnView(tx > 0.f ? -snap : snap);
        sIn.snapArmed = false;
    }
    else if (std::fabs(tx) < 0.35f)
    {
        sIn.snapArmed = true;
    }

    // Fly cam: the sticks fly the detached view (head-relative, quadratic response for fine
    // control: up to 5 m/s horizontally, 3 m/s up / down); the avatar stays where it is.
    if (LLVRCamera::flyCam())
    {
        const F32 sx = lh.active ? lh.stick[0] : 0.f, sy = lh.active ? lh.stick[1] : 0.f;
        const F32 sz = rh.active ? rh.stick[1] : 0.f;
        const F32 yaw = LLVRCamera::headYaw();
        const LLVector3 fwd(std::cos(yaw), std::sin(yaw), 0.f), right(std::sin(yaw), -std::cos(yaw), 0.f);
        auto curve = [](F32 v) { return std::fabs(v) < 0.15f ? 0.f : v * std::fabs(v); };
        const LLVector3 delta = (fwd * curve(sy) + right * curve(sx)) * (5.f * dt) + LLVector3(0.f, 0.f, curve(sz) * 3.f * dt);
        if (delta.magVecSquared() > 0.f) LLVRCamera::flyMove(delta);
        return;
    }

    // Up / down (as the joystick's fly axis).
    const F32 ty = rh.active ? rh.stick[1] : 0.f;
    if (ty > 0.7f)
    {
        static LLCachedControl<bool> s_auto_fly(gSavedSettings, "AutomaticFly", true);
        if (!gAgent.getFlying() && gAgent.canFly() && !gAgent.upGrabbed() && s_auto_fly) gAgent.setFlying(true);
        gAgent.moveUp(1);
    }
    else if (ty < -0.7f)
    {
        gAgent.moveUp(-1);
    }

    // Walk.
    // (teleport locomotion: the left stick aims the teleport instead, LLVRTeleport)
    const bool walkStick = lh.active && !LLVRTeleport::stickMode();
    const F32 mx = walkStick ? lh.stick[0] : 0.f, my = walkStick ? lh.stick[1] : 0.f;
    const bool walking = std::fabs(mx) > 0.3f || std::fabs(my) > 0.3f;
    // #1121: standing, the body turns to follow the head once it is past the neck's range.
    LLVRCamera::bodyFollowHead(dt, walking);
    if (walking)
    {
        if (LLVRCamera::rigFollowsAgent())
        {
            const LLVector3 at = gAgent.getAtAxis();
            const F32 delta = wrapAngle(LLVRCamera::headYaw() - std::atan2(at.mV[VY], at.mV[VX]));
            if (std::fabs(delta) > 0.02f)
            {
                gAgent.rotate(delta, LLVector3::z_axis);
                LLVRCamera::rigTurnedKeepView(delta);
            }
        }
        if (std::fabs(my) > 0.3f) gAgent.moveAt(my > 0.f ? 1 : -1, false);
        if (std::fabs(mx) > 0.3f) gAgent.moveLeft(mx > 0.f ? -1 : 1);
    }
}

// Standard active rotation of v by the unit quaternion q = (x, y, z, w).
void rotateByQuat(const float q[4], const float v[3], float out[3])
{
    const float ux = q[0], uy = q[1], uz = q[2], w = q[3];
    const float tx = 2.f * (uy * v[2] - uz * v[1]);
    const float ty = 2.f * (uz * v[0] - ux * v[2]);
    const float tz = 2.f * (ux * v[1] - uy * v[0]);
    out[0] = v[0] + w * tx + (uy * tz - uz * ty);
    out[1] = v[1] + w * ty + (uz * tx - ux * tz);
    out[2] = v[2] + w * tz + (ux * ty - uy * tx);
}

// A pose's pointing ray (-Z) in app space.
void aimRay(const LLXrPose& p, float o[3], float d[3])
{
    static const float fwd[3] = {0.f, 0.f, -1.f};
    o[0] = p.position[0];
    o[1] = p.position[1];
    o[2] = p.position[2];
    rotateByQuat(p.orientation, fwd, d);
}

MASK currentMask()
{
    return gKeyboard ? gKeyboard->currentMask(true) : MASK_NONE;
}

// The UI image pixel (top-left origin) as LLViewerWindow's mouse entry points take it.
bool toGL(S32 x, S32 y, LLCoordGL& out)
{
    LLWindow* w = gViewerWindow ? gViewerWindow->getWindow() : nullptr;
    return w && w->convertCoords(LLCoordWindow(x, y), &out);
}

// A press on the wall goes to the UI, or to a HUD attachment. Where the wall is transparent
// (the world shows through) it does nothing: the world behind is not under that window pixel
// in the headset (the mouse's world pick would land somewhere else -- a touch or a walk-to
// you did not aim at). Judged from the last hover (one frame old, at the same point).
bool pressAllowed()
{
    if (!gViewerWindow->getPointerOverWorld()) return true;
    const LLPointer<LLViewerObject> o = LLToolPie::getInstance()->getHoverPick().getObject();
    return o.notNull() && o->isHUDAttachment();
}

// #1078 haptic ticks on hover: a faint tick on the pointing hand when the pointer moves onto a
// different enabled button, menu item or check box on the wall (feel the controls you can
// press). The view under the mouse point (a leaf), or its nearest clickable ancestor; only its
// address is kept for the comparison (never dereferenced).
void hoverHaptic(U32 hand)
{
    LLView* root = gViewerWindow ? gViewerWindow->getRootView() : nullptr;
    if (!root) return;
    LLView* v = root->childFromPoint(gViewerWindow->getCurrentMouseX(), gViewerWindow->getCurrentMouseY(), true);
    LLView* target = nullptr;
    for (int i = 0; v && i < 4 && !target; ++i, v = v->getParent())
    {
        if (dynamic_cast<LLButton*>(v) || dynamic_cast<LLMenuItemGL*>(v) || dynamic_cast<LLCheckBoxCtrl*>(v)) target = v;
    }
    if (target == sIn.hoverTarget) return;
    sIn.hoverTarget = target;
    if (target && target->getEnabled()) LLXrSession::instance().haptic(hand, 0.1f, 0.006f);
}

void releaseButtons()
{
    if (!sIn.leftDown && !sIn.rightDown) return;
    LLCoordGL gl;
    if (gViewerWindow && toGL(llmax(sIn.px, 0), llmax(sIn.py, 0), gl))
    {
        if (sIn.leftDown) gViewerWindow->handleMouseUp(gViewerWindow->getWindow(), gl, currentMask());
        if (sIn.rightDown) gViewerWindow->handleRightMouseUp(gViewerWindow->getWindow(), gl, currentMask());
    }
    sIn.leftDown = sIn.rightDown = false;
}

// #1076 click diagnostics, rate-limited: the first 12, then every 25th.
bool logCountDue(U32& n)
{
    ++n;
    return n <= 12 || (n % 25) == 0;
}

bool clickLogDue()
{
    return logCountDue(sIn.clickLogs);
}

// What the viewer did with an injected mouse down / up: handled, and who holds the mouse now.
void logMouseResult(const char* what, bool took)
{
    LLMouseHandler* captor = gFocusMgr.getMouseCapture();
    LL_INFOS("VR") << "click: mouse " << what << (took ? " handled" : " NOT handled") << ", mouse captor "
                   << (captor ? captor->getName() : std::string("(none)")) << LL_ENDL;
}

// #1076 WORLD POINTER. The pointer hand's aim ray, in agent space, becomes the cursor ray of
// every pick (LLCleanCursorPick::setVRRay) while it aims at the world, and a trigger pull is a
// left click given straight to the current tool (LLToolPie: touch / grab, sit, click actions,
// walk-to; the build tools), skipping the UI views: the UI is the wall, and hidden it must not
// take a click meant for the world. The mouse point stays where it is (the tool's slop / drag
// tests compare it with the click's point); it only keys the pick answers.
bool aimWorldRay(U32 hand)
{
    LLVector3 pos;
    LLQuaternion rot;
    if (!LLVRCamera::handPose(hand, /*aim=*/true, pos, rot))
    {
        LLCleanCursorPick::clearVRRay();
        return false;
    }
    LLCleanCursorPick::setVRRay(pos, LLVector3::x_axis * rot);
    return LLCleanCursorPick::vrRayOn();
}

// A left mouse down / up on the world at the current mouse point (scaled), inside a click
// scope (the pick then waits for the exact answer to this ray). The up goes to whatever holds
// the mouse (the tool, a grab), as LLViewerWindow routes it.
bool worldMouse(bool down, S32 x, S32 y, MASK mask)
{
    LLCleanCursorPick::ClickScope scope(down);
    LLMouseHandler* captor = gFocusMgr.getMouseCapture();
    if (!down && captor)
    {
        S32 lx = x, ly = y;
        captor->screenPointToLocal(x, y, &lx, &ly);
        return captor->handleAnyMouseClick(lx, ly, mask, CLICK_LEFT, false);
    }
    LLTool* tool = LLToolMgr::getInstance()->getCurrentTool();
    return tool && tool->handleAnyMouseClick(x, y, mask, CLICK_LEFT, down);
}

void worldPress(U32 hand, MASK mask, bool logIt)
{
    sIn.worldDown = true;
    sIn.worldX = gViewerWindow->getCurrentMouseX();
    sIn.worldY = gViewerWindow->getCurrentMouseY();
    gViewerWindow->setVRLeftMouseDown(true);   // (LLToolGrab ends a grab without it)
    const bool took = worldMouse(true, sIn.worldX, sIn.worldY, mask);
    LLXrSession::instance().haptic(hand, 0.25f, 0.02f);
    sIn.worldLogUp = logIt;
    if (logIt)
    {
        LLVector3 o, d;
        LLCleanCursorPick::vrRay(o, d);
        const LLPickInfo& pick = LLToolPie::getInstance()->getPick();
        const LLPointer<LLViewerObject> obj = pick.getObject();
        LLTool* tool = LLToolMgr::getInstance()->getCurrentTool();
        LL_INFOS("VR") << "world click: " << (hand == LLXR_HAND_LEFT ? "left" : "right") << " hand, ray ("
                       << o.mV[VX] << ", " << o.mV[VY] << ", " << o.mV[VZ] << ") dir (" << d.mV[VX] << ", " << d.mV[VY]
                       << ", " << d.mV[VZ] << "), tool " << (tool ? tool->getName() : std::string("(none)")) << ", "
                       << (took ? "handled" : "NOT handled") << ", picked "
                       << (obj.notNull() ? obj->getID().asString() : std::string(pick.mPickType == LLPickInfo::PICK_LAND
                                                                                      ? "land" : "nothing"))
                       << LL_ENDL;
    }
}

void worldRelease(MASK mask)
{
    if (!sIn.worldDown) return;
    sIn.worldDown = false;
    gViewerWindow->setVRLeftMouseDown(false);
    const bool took = worldMouse(false, sIn.worldX, sIn.worldY, mask);
    if (sIn.worldLogUp) logMouseResult("up (world)", took);
    sIn.worldLogUp = false;
}

// Place the wall in front of the head's facing, its centre a little below eye level -- only
// on a pose that can anchor it (LLXrHeadPose::placeable: tracked, shown, a plausible height).
// Otherwise the placement waits (placePending) and happens on the first such frame: the first
// headset run placed the wall at a 5 cm "tracked" head before SteamVR settled (the wall lay on
// the floor for a minute and a half).
void placeWallNow(const LLXrHeadPose& hp);
void placeWall(const LLXrHeadPose& hp)
{
    if (hp.placeable())
    {
        placeWallNow(hp);
        return;
    }
    if (!sIn.placePending)
    {
        LL_INFOS("VR") << "UI wall placement waits for head tracking (tracked " << (int)hp.headTracked << ", shown "
                       << (int)hp.shown << ", height " << hp.position[1] << " m)" << LL_ENDL;
    }
    sIn.placePending = true;
}

void placeWallNow(const LLXrHeadPose& hp)
{
    static const float fwd[3] = {0.f, 0.f, -1.f};
    float f[3];
    rotateByQuat(hp.orientation, fwd, f);
    const float len = std::sqrt(f[0] * f[0] + f[2] * f[2]);
    if (len > 1e-3f) sIn.ui.wallYaw = std::atan2(-f[0] / len, -f[2] / len);
    sIn.ui.wallAxis[0] = hp.position[0];
    sIn.ui.wallAxis[1] = hp.position[1] - 0.15f;
    sIn.ui.wallAxis[2] = hp.position[2];
    sIn.placed = true;
    sIn.placePending = false;
    ++sIn.ui.placeSerial;   // (LLXrSession logs the exact layers of the next submit once)
    LL_INFOS("VR") << "UI wall placed: yaw " << sIn.ui.wallYaw * RAD_TO_DEG << " deg, centre height "
                   << sIn.ui.wallAxis[1] << " m (app space)" << LL_ENDL;
}

// ---- #1134 the floating context menu panel. The menu is the desktop's own (LLToolPie opens
// the context / pie menu for the pick as a right click would); the panel only shows the part of
// the UI image it occupies on a quad in the world, and the pointer's ray clicks it like the wall.

// The open context / pie menus' union (this frame), as a rect of the UI image (pixels, top-left
// origin, a small margin). Submenus are menus of their own in the menu holder.
bool popupRect(S32 winW, S32 winH, S32 out[4])
{
    if (!gMenuHolder) return false;
    const LLVector2 sc = LLUI::getScaleFactor();
    LLRect u;
    bool any = false;
    for (LLView* v : *gMenuHolder->getChildList())
    {
        if (!v || !v->getVisible()) continue;
        if (!dynamic_cast<LLContextMenu*>(v) && !dynamic_cast<PieMenu*>(v)) continue;
        const LLRect r = v->calcScreenRect();
        if (r.getWidth() <= 0 || r.getHeight() <= 0) continue;
        if (any) u.unionWith(r);
        else u = r;
        any = true;
    }
    if (!any) return false;
    const S32 m = 6;   // margin (pixels)
    const S32 x0 = llclamp((S32)((F32)u.mLeft * sc.mV[VX]) - m, 0, winW - 1);
    const S32 x1 = llclamp((S32)((F32)u.mRight * sc.mV[VX]) + m, x0 + 1, winW);
    const S32 top = llclamp(winH - (S32)((F32)u.mTop * sc.mV[VY]) - m, 0, winH - 1);
    const S32 bot = llclamp(winH - (S32)((F32)u.mBottom * sc.mV[VY]) + m, top + 1, winH);
    out[0] = x0;
    out[1] = top;
    out[2] = x1 - x0;
    out[3] = bot - top;
    return true;
}

// Closes the panel and the menu behind it (what a click away from a menu does on the desktop).
void closeMenuPanel()
{
    if (!sIn.panelOpen) return;
    sIn.panelOpen = false;
    sIn.panelAwaitRelease = false;
    sIn.ui.panel = false;
    sIn.onPanel[LLXR_HAND_LEFT] = sIn.onPanel[LLXR_HAND_RIGHT] = false;
    if (gMenuHolder) gMenuHolder->hideMenus();
}

// A ray (app space) against the panel quad. dist along the ray (m); u, v across the whole UI
// image (0 .. 1, v down), as wallHit answers, so the wall's pointer path takes it as is.
bool panelHit(const float o[3], const float d[3], S32 winW, S32 winH, float& dist, float& u, float& v)
{
    if (!sIn.panelOpen || !sIn.ui.panel) return false;
    const float s = std::sin(sIn.ui.panelYaw), c = std::cos(sIn.ui.panelYaw);
    const float n[3] = {s, 0.f, c};          // the panel's normal (faces the viewer)
    const float right[3] = {c, 0.f, -s};     // its +X
    const float dn = d[0] * n[0] + d[1] * n[1] + d[2] * n[2];
    if (std::fabs(dn) < 1e-5f) return false;
    const float* ctr = sIn.ui.panelCenter;
    const float t = ((ctr[0] - o[0]) * n[0] + (ctr[1] - o[1]) * n[1] + (ctr[2] - o[2]) * n[2]) / dn;
    if (!(t > 0.f)) return false;
    const float p[3] = {o[0] + t * d[0] - ctr[0], o[1] + t * d[1] - ctr[1], o[2] + t * d[2] - ctr[2]};
    const float lx = p[0] * right[0] + p[2] * right[2];
    const float ly = p[1];
    const float w = sIn.ui.panelSize[0], h = sIn.ui.panelSize[1];
    if (std::fabs(lx) > 0.5f * w || std::fabs(ly) > 0.5f * h) return false;
    const float px = (float)sIn.ui.panelRect[0] + (lx / w + 0.5f) * (float)sIn.ui.panelRect[2];
    const float py = (float)sIn.ui.panelRect[1] + (0.5f - ly / h) * (float)sIn.ui.panelRect[3];
    dist = t;
    u = px / (float)winW;
    v = py / (float)winH;
    return true;
}

// The context button with the world pointer aimed at the world (its ray is the cursor ray,
// aimWorldRay): the desktop's right click for that pick, its menu then shown on the panel,
// near the pointed spot (pulled toward the head, facing it, 0.7 .. 2.2 m away, upright), at a
// fixed angular size per UI pixel so the text reads at that distance.
bool openContextMenu(U32 hand, const LLXrHandState& hs, const LLXrHeadPose& hp, MASK mask, S32 winW, S32 winH)
{
    if (!gMenuHolder || !hs.aim.valid || !hp.valid) return false;
    LLVector3 ro, rd;
    if (!LLCleanCursorPick::vrRay(ro, rd)) return false;
    closeMenuPanel();
    {
        LLCleanCursorPick::ClickScope scope(true);   // (the pick waits for the exact answer to this ray)
        LLToolPie::getInstance()->showContextMenuAt(gViewerWindow->getCurrentMouseX(), gViewerWindow->getCurrentMouseY(), mask);
    }
    S32 r[4];
    if (!popupRect(winW, winH, r))
    {
        LL_INFOS("VR") << "context menu: nothing to open a menu for at the pointed spot" << LL_ENDL;
        return false;
    }
    const LLPickInfo& pick = LLToolPie::getInstance()->getPick();
    F32 dist = sIn.worldBeam;
    if (!pick.mPosGlobal.isExactlyZero()) dist = (gAgent.getPosAgentFromGlobal(pick.mPosGlobal) - ro).length();
    dist = llclamp(dist, 0.3f, kWorldBeamMax);
    float o[3], d[3];
    aimRay(hs.aim, o, d);
    const float* head = hp.position;
    float to[3] = {head[0] - (o[0] + d[0] * dist), head[1] - (o[1] + d[1] * dist), head[2] - (o[2] + d[2] * dist)};
    float len = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
    if (len < 1e-3f)
    {
        to[0] = -d[0];
        to[1] = -d[1];
        to[2] = -d[2];
        len = 1.f;
    }
    float p[3];
    for (int i = 0; i < 3; ++i) p[i] = o[i] + d[i] * dist + to[i] / len * 0.35f;
    p[1] = llclamp(p[1], head[1] - 0.6f, head[1] + 0.4f);
    float v[3] = {p[0] - head[0], p[1] - head[1], p[2] - head[2]};
    float dh = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (dh < 1e-3f)
    {
        v[0] = 0.f;
        v[1] = 0.f;
        v[2] = -1.f;
        dh = 1.f;
    }
    const float dh2 = llclamp(dh, 0.7f, 2.2f);
    for (int i = 0; i < 3; ++i) p[i] = head[i] + v[i] / dh * dh2;
    sIn.panelP[0] = p[0];
    sIn.panelP[1] = p[1];
    sIn.panelP[2] = p[2];
    sIn.panelYaw = std::atan2(head[0] - p[0], head[2] - p[2]);
    sIn.panelScale = llclamp(0.0009f * dh2, 0.0007f, 0.0025f);
    sIn.panelCx0 = (F32)r[0] + 0.5f * (F32)r[2];
    sIn.panelCy0 = (F32)r[1] + 0.5f * (F32)r[3];
    sIn.panelGen = hp.spaceGeneration;
    sIn.panelOpen = true;
    sIn.panelAwaitRelease = true;   // #1142 (the opening press is still held)
    sIn.panelIgnoreLogged = false;
    LLXrSession::instance().haptic(hand, 0.3f, 0.03f);
    LL_INFOS("VR") << "context menu panel: " << r[2] << "x" << r[3] << " px at " << dh2 << " m, " << sIn.panelScale * 1000.f
                   << " mm/px (target " << dist << " m away)" << LL_ENDL;
    return true;
}

// Per frame, before the layers go to the session: keeps the panel's rect on the menu (submenus
// grow it), and closes it when the menu is gone (an item chosen, a click away), the world is not
// shown, the app space was re-centred, or a stick moves the avatar (the panel is room-locked,
// like the wall: walking away would carry it along).
void updateMenuPanel(bool inWorld, bool uiBuilt, S32 winW, S32 winH, const LLXrHeadPose& hp, const LLXrInputState& in)
{
    sIn.ui.panel = false;
    if (!sIn.panelOpen) return;
    const LLXrHandState& lh = in.hand[LLXR_HAND_LEFT];
    const LLXrHandState& rh = in.hand[LLXR_HAND_RIGHT];
    const F32 push = llmax(llmax(std::fabs(lh.stick[0]), std::fabs(lh.stick[1])),
                           llmax(std::fabs(rh.stick[0]), std::fabs(rh.stick[1])));
    S32 r[4];
    if (!inWorld || !uiBuilt || !in.focused || hp.spaceGeneration != sIn.panelGen || push > 0.6f || !popupRect(winW, winH, r))
    {
        closeMenuPanel();
        return;
    }
    const F32 s = sIn.panelScale;
    const float ox = ((F32)r[0] + 0.5f * (F32)r[2] - sIn.panelCx0) * s;    // along the panel's +X
    const float oy = -((F32)r[1] + 0.5f * (F32)r[3] - sIn.panelCy0) * s;   // up
    sIn.ui.panel = true;
    for (int i = 0; i < 4; ++i) sIn.ui.panelRect[i] = r[i];
    sIn.ui.panelYaw = sIn.panelYaw;
    sIn.ui.panelCenter[0] = sIn.panelP[0] + std::cos(sIn.panelYaw) * ox;
    sIn.ui.panelCenter[1] = sIn.panelP[1] + oy;
    sIn.ui.panelCenter[2] = sIn.panelP[2] - std::sin(sIn.panelYaw) * ox;
    sIn.ui.panelSize[0] = (F32)r[2] * s;
    sIn.ui.panelSize[1] = (F32)r[3] * s;
}
} // namespace

bool LLVRInput::wallVisible()
{
    return LLXrSession::active() && sIn.ui.wall;
}

void LLVRInput::toggleWall()
{
    if (LLXrSession::active()) sIn.toggleRequest = true;
}

bool LLVRInput::endFlyCam()
{
    if (!LLXrSession::active() || !LLVRCamera::flyCam()) return false;
    if (sIn.blink == VRInputState::BLINK_NONE)
    {
        sIn.blink = VRInputState::BLINK_OUT;   // flyCamTick runs the blink and the jump back
        LL_INFOS("VR") << "fly cam: reset view (Esc) -- back to the avatar with a blink" << LL_ENDL;
    }
    return true;
}

void LLVRInput::update()
{
    if (!LLXrSession::active())   // desktop: one static read
    {
        return;
    }
    LLXrSession& xr = LLXrSession::instance();
    LLXrInputState in;
    LLXrHeadPose hp;
    const bool running = xr.inputState(in) && xr.headPose(hp);
    const auto now = std::chrono::steady_clock::now();
    const F32 dt = sIn.last == std::chrono::steady_clock::time_point{}
                       ? 0.f
                       : llclamp(std::chrono::duration<F32>(now - sIn.last).count(), 0.f, 0.1f);
    sIn.last = now;
    if (!running || !gViewerWindow)
    {
        if (gViewerWindow) worldRelease(currentMask());
        sIn.worldDown = false;
        sIn.panelOpen = sIn.ui.panel = false;   // (#1134: a left-over menu hides itself with the headset)
        LLCleanCursorPick::clearVRRay();
        releaseButtons();
        sIn.onWall = false;
        if (gViewerWindow) gViewerWindow->setVRPointerInWindow(false);
        LLVRSafety::reset();   // (no-op when nothing is capped)
        LLVRGrab::releaseAll();   // #1143 (no-op unless something is held)
        LLVRTeleport::cancel();   // (no-op unless aiming)
        LLVRDictation::cancel();   // #1118 (no-op unless listening)
        LLVRWrist::reset();        // #1120
        sIn.onWristRight = false;
        return;
    }
    const bool inWorld = LLVRCamera::active();
    // ---- The frame-rate safety governor (pacing from this frame's xrWaitFrame).
    LLVRSafety::tick(inWorld);

    // ---- The UI wall. Before the world shows (login, loading) it is always up.
    bool menu = false;
    for (const LLXrHandState& h : in.hand) menu |= h.active && h.menu;
    const bool menuPressed = menu && !sIn.prevMenu;
    sIn.prevMenu = menu;
    if (menuPressed || sIn.toggleRequest)
    {
        if (inWorld)
        {
            sIn.wallShown = !sIn.wallShown;
            if (sIn.wallShown) placeWall(hp);   // summoned in front of you, then world-locked
        }
        else
        {
            placeWall(hp);   // (always up before the world: the button brings it in front of you)
        }
        for (U32 h = 0; h < LLXR_HAND_COUNT; ++h)
            if (in.hand[h].menu) xr.haptic(h, 0.4f, 0.03f);
    }
    sIn.toggleRequest = false;
    // #1120 the wrist panel asked for the wall last frame (an item / dialog opened on it).
    if (sIn.showRequest && inWorld && !sIn.wallShown)
    {
        sIn.wallShown = true;
        placeWall(hp);
    }
    sIn.showRequest = false;
    const bool visible = !inWorld || sIn.wallShown;
    // The runtime re-centred the app space (dashboard "reset view"): the placement's
    // coordinates are stale -- place again in front of you.
    if (hp.spaceGeneration != sIn.spaceGeneration)
    {
        sIn.spaceGeneration = hp.spaceGeneration;
        if (sIn.placed) LL_INFOS("VR") << "the runtime re-centred the app space: the UI wall is placed again" << LL_ENDL;
        sIn.placed = false;
    }
    if (visible && !sIn.placed) placeWall(hp);
    else if (sIn.placePending && hp.placeable()) placeWallNow(hp);   // a summon that waited for tracking
    const S32 winW = llmax(1, gViewerWindow->getWindowWidthRaw()), winH = llmax(1, gViewerWindow->getWindowHeightRaw());
    // No UI is built while the mirror window is hidden or minimised (displayVulkanUI's test):
    // then there is no wall / wrist image, so neither is shown nor takes clicks (the wall
    // comes back as it was when the window does).
    LLWindow* win = gViewerWindow->getWindow();
    const bool uiBuilt = win && gViewerWindow->getActive() && win->getVisible() && !win->getMinimized();
    sIn.ui.wall = uiBuilt && visible && sIn.placed;
    sIn.ui.wallAspect = (F32)winW / (F32)winH;
    static LLCachedControl<U32> s_debug_layers(gSavedSettings, "SlipstreamVRDebugLayers", 0);
    sIn.ui.debugLayers = llmin((U32)s_debug_layers, 3u);
    // #1120 the wrist panel (its own image; LLVRWrist): in the world, wall up or not, while the
    // left controller is turned toward the face. Its wall / fly cam requests apply next frame.
    {
        LLVRWrist::Requests wr;
        LLVRWrist::update(in, hp, uiBuilt && inWorld, sIn.ui.wall, dt, sIn.ui, wr);
        F32 wristBeam = 0.f;
        sIn.onWristRight = LLVRWrist::rightOnPanel(wristBeam);
        if (sIn.onWristRight) LLVRHands::setBeam(LLXR_HAND_RIGHT, wristBeam);
        if (wr.wallToggle) sIn.toggleRequest = true;
        else if (wr.wallShow) sIn.showRequest = true;
        if (wr.flyCam) sIn.flyRequest = true;
    }
    updateMenuPanel(inWorld, uiBuilt, winW, winH, hp, in);   // #1134 (the floating context menu)
    xr.setUiLayers(sIn.ui);   // (wallHit below uses this geometry)
    LLVRLobby::update(in, hp, inWorld);   // #1076 the login room (before the world)

    // ---- The pointer: the aim rays against the wall. A trigger pull on the other hand while
    // its ray is on the wall makes that hand the pointer.
    float hitU[LLXR_HAND_COUNT] = {0.f, 0.f}, hitV[LLXR_HAND_COUNT] = {0.f, 0.f}, hitD[LLXR_HAND_COUNT] = {0.f, 0.f};
    bool hit[LLXR_HAND_COUNT] = {false, false};
    for (U32 h = 0; h < LLXR_HAND_COUNT; ++h)
    {
        const LLXrHandState& hs = in.hand[h];
        sIn.onPanel[h] = false;
        if ((!sIn.ui.wall && !sIn.ui.panel) || !in.focused || !hs.active || !hs.aim.valid) continue;
        if (h == LLXR_HAND_RIGHT && sIn.onWristRight) continue;   // #1120 (the wrist panel is in front)
        float o[3], d[3];
        aimRay(hs.aim, o, d);
        if (sIn.ui.wall) hit[h] = xr.wallHit(o, d, hitU[h], hitV[h], hitD[h]);
        // #1134: the floating menu panel, where it is in front of the wall (or the wall is hidden).
        {
            float pd = 0.f, pu = 0.f, pv = 0.f;
            if (panelHit(o, d, winW, winH, pd, pu, pv) && (!hit[h] || pd < hitD[h]))
            {
                hit[h] = true;
                sIn.onPanel[h] = true;
                hitU[h] = pu;
                hitV[h] = pv;
                hitD[h] = pd;
            }
        }
        if (hit[h]) LLVRHands::setBeam(h, hitD[h]);
        if (hit[h] && !sIn.hitLogged[h])
        {
            sIn.hitLogged[h] = true;   // (once per hand: proves the ray reaches the wall)
            LL_INFOS("VR") << "pointer: " << (h == LLXR_HAND_LEFT ? "left" : "right") << " ray first on the wall at u "
                           << hitU[h] << " v " << hitV[h] << ", " << hitD[h] << " m" << LL_ENDL;
        }
    }
    bool selEdge[LLXR_HAND_COUNT] = {false, false};   // this frame's trigger press edges
    for (U32 h = 0; h < LLXR_HAND_COUNT; ++h)
    {
        const bool sel = in.hand[h].active && in.hand[h].select;
        selEdge[h] = sel && !sIn.prevSelect[h];
        if (sel && !sIn.prevSelect[h] && logCountDue(sIn.selectEdgeLogs))
        {
            LL_INFOS("VR") << "select: " << (h == LLXR_HAND_LEFT ? "left" : "right") << " hand pressed (pointer hand "
                           << (h == sIn.pointer ? "yes" : "no") << ", wall up " << (int)sIn.ui.wall << ", focused "
                           << (int)in.focused << ", ray on wall " << (int)hit[h] << ")" << LL_ENDL;
        }
        // A trigger pull on the other hand makes it the pointer (on the wall or at the world).
        if (sel && !sIn.prevSelect[h] && h != sIn.pointer && !sIn.leftDown && !sIn.rightDown && !sIn.worldDown)
            sIn.pointer = h;
        sIn.prevSelect[h] = sel;
        // (A / X: right click on the wall; logged so a run shows which button the owner used.)
        const bool ctx = in.hand[h].active && in.hand[h].context;
        if (ctx && !sIn.prevContext[h] && logCountDue(sIn.selectEdgeLogs))
        {
            LL_INFOS("VR") << "context (A): " << (h == LLXR_HAND_LEFT ? "left" : "right") << " hand pressed (pointer hand "
                           << (h == sIn.pointer ? "yes" : "no") << ", wall up " << (int)sIn.ui.wall << ", ray on wall "
                           << (int)hit[h] << ")" << LL_ENDL;
        }
        sIn.ctxEdge[h] = ctx && !sIn.prevContext[h];
        // The context button on the other hand makes it the pointer too (it aims the menu).
        if (sIn.ctxEdge[h] && h != sIn.pointer && !sIn.leftDown && !sIn.rightDown && !sIn.worldDown) sIn.pointer = h;
        sIn.prevContext[h] = ctx;
    }
    // #1142: the menu panel's press gate opens once every trigger and context button is up.
    if (sIn.panelOpen && sIn.panelAwaitRelease)
    {
        bool anyHeld = false;
        for (const LLXrHandState& hs : in.hand) anyHeld |= hs.active && (hs.select || hs.context);
        if (!anyHeld)
        {
            sIn.panelAwaitRelease = false;
            if (logCountDue(sIn.panelGateLogs))
                LL_INFOS("VR") << "context menu panel: buttons released, a fresh press now selects" << LL_ENDL;
        }
    }
    const U32 p = sIn.pointer;
    const LLXrHandState& ph = in.hand[p];
    LLWindow* window = gViewerWindow->getWindow();
    const MASK mask = currentMask();
    sIn.onWall = hit[p];
    const bool held = sIn.leftDown || sIn.rightDown;
    if ((!sIn.ui.wall && !sIn.ui.panel) || !in.focused || !ph.active)
    {
        releaseButtons();   // hidden / dashboard / controller gone: let go of a held click
        sIn.onWall = false;
    }
    else if ((hit[p] || held) && !sIn.worldDown)
    {
        // The hit pixel; while a button is held off the wall (a drag) the last point stays.
        // Lightly smoothed against hand tremor (~3 px at this distance), and held still for a
        // moment after a press (pulling the trigger jerks the controller: no accidental drag).
        if (hit[p])
        {
            const bool fresh = !sIn.smoothValid || sIn.smoothHand != p;
            const F32 a = fresh ? 1.f : 1.f - std::exp(-dt / 0.035f);
            sIn.su = fresh ? hitU[p] : sIn.su + (hitU[p] - sIn.su) * a;
            sIn.sv = fresh ? hitV[p] : sIn.sv + (hitV[p] - sIn.sv) * a;
            sIn.smoothValid = true;
            sIn.smoothHand = p;
            if (now >= sIn.holdUntil)
            {
                sIn.px = llclamp((S32)(sIn.su * (F32)winW), 0, winW - 1);
                sIn.py = llclamp((S32)(sIn.sv * (F32)winH), 0, winH - 1);
            }
        }
        else
        {
            sIn.smoothValid = false;
        }
        LLCoordGL gl;
        if (sIn.px >= 0 && toGL(sIn.px, sIn.py, gl))
        {
            if (hit[p] && (sIn.px != sIn.sentX || sIn.py != sIn.sentY))
            {
                gViewerWindow->handleMouseMove(window, gl, mask);
                sIn.sentX = sIn.px;
                sIn.sentY = sIn.py;
                if (!held) hoverHaptic(p);   // #1078
            }
            const bool canPress = hit[p] && pressAllowed();
            // #1076 click diagnostics: each select press edge of the pointer hand, whether it
            // became a mouse press (and why not), where, and who took the down / up.
            const bool selectEdge = ph.select && !sIn.prevPointerSelect;
            sIn.prevPointerSelect = ph.select;
            const bool logClick = selectEdge && clickLogDue();
            // On a transparent part of the wall (no UI there, the world shows through): the
            // press goes to the world along the controller's ray.
            const bool throughWall = selectEdge && hit[p] && !canPress && !sIn.leftDown && inWorld && !sIn.onPanel[p];
            // #1142: on the menu panel only a fresh press edge, after every button that was held
            // when it opened was let go, becomes a mouse down (levels elsewhere, as before).
            const bool onPanel = sIn.onPanel[p];
            const bool panelBlocked = onPanel && sIn.panelAwaitRelease;
            const bool leftMay = !onPanel || (!panelBlocked && selEdge[p]);
            const bool rightMay = !onPanel || (!panelBlocked && sIn.ctxEdge[p]);
            if (panelBlocked && ((ph.select && !sIn.leftDown) || (ph.context && !sIn.rightDown)) &&
                !sIn.panelIgnoreLogged && logCountDue(sIn.panelGateLogs))
            {
                sIn.panelIgnoreLogged = true;   // (once per opened menu)
                LL_INFOS("VR") << "context menu panel: held " << (ph.select ? "trigger" : "context button")
                               << " ignored on the panel until released" << LL_ENDL;
            }
            // #1134: a trigger pull on a part of the menu panel that holds no menu is a click away.
            if (selEdge[p] && onPanel && !panelBlocked && !canPress && !sIn.leftDown) closeMenuPanel();
            if (logClick)
            {
                const LLPointer<LLViewerObject> hover = LLToolPie::getInstance()->getHoverPick().getObject();
                LL_INFOS("VR") << "click: select edge (" << (p == LLXR_HAND_LEFT ? "left" : "right") << " hand, click "
                               << (int)ph.selectClick << ", value " << ph.selectValue << ") at window (" << sIn.px << ", "
                               << sIn.py << ") gl (" << gl.mX << ", " << gl.mY << "): on wall " << (int)hit[p]
                               << ", pointer over world " << (int)gViewerWindow->getPointerOverWorld() << ", hover object "
                               << (hover.notNull() ? (hover->isHUDAttachment() ? "HUD" : "world") : "none") << ", button held "
                               << (int)sIn.leftDown << " -> "
                               << (canPress && !sIn.leftDown ? "mouse down" : throughWall ? "world click" : "NO press")
                               << LL_ENDL;
            }
            if (throughWall && aimWorldRay(p)) worldPress(p, mask, logClick);
            // (the hold starts at the press itself: a trigger held over a transparent part of
            // the wall used to re-arm it every frame and freeze the pointer)
            if ((ph.select && !sIn.leftDown && canPress && leftMay) || (ph.context && !sIn.rightDown && canPress && rightMay))
                sIn.holdUntil = now + std::chrono::milliseconds(120);
            if (ph.select && !sIn.leftDown && canPress && leftMay)
            {
                sIn.leftDown = true;
                const bool took = gViewerWindow->handleMouseDown(window, gl, mask);
                xr.haptic(p, 0.25f, 0.02f);
                sIn.logUp = logClick;
                if (logClick) logMouseResult("down", took);
            }
            else if (!ph.select && sIn.leftDown)
            {
                sIn.leftDown = false;
                const bool took = gViewerWindow->handleMouseUp(window, gl, mask);
                if (sIn.logUp) logMouseResult("up", took);
                sIn.logUp = false;
            }
            if (ph.context && !sIn.rightDown && canPress && rightMay)
            {
                sIn.rightDown = true;
                gViewerWindow->handleRightMouseDown(window, gl, mask);
                xr.haptic(p, 0.25f, 0.02f);
            }
            else if (!ph.context && sIn.rightDown)
            {
                sIn.rightDown = false;
                gViewerWindow->handleRightMouseUp(window, gl, mask);
            }
            // Thumbstick forward / back scrolls (as the wheel: forward = up), smoothly where
            // smooth scrolling is on; whole notches otherwise.
            const F32 sy = ph.stick[1];
            if (hit[p] && !held && std::fabs(sy) > 0.2f)
            {
                const F32 notches = -sy * 10.f * dt;   // up to 10 notches a second
                sIn.scrollAcc += notches;
                const S32 whole = (S32)sIn.scrollAcc;
                sIn.scrollAcc -= (F32)whole;
                if (!gViewerWindow->handleScrollWheelSmooth(window, notches, 0.f, whole, 0) && whole)
                    gViewerWindow->handleScrollWheel(whole);
            }
            else
            {
                sIn.scrollAcc = 0.f;
            }
        }
    }
    // ---- The world pointer: the pointer hand aimed at the world (the wall hidden, or its ray
    // off the wall; not on the wrist panel, which takes that hand's trigger). Its ray is the
    // cursor ray of every pick (LLCleanCursorPick::setVRRay), the beam reaches what it hits (or
    // kWorldBeamMax), and the trigger left-clicks the world there. A click held keeps the ray
    // (a grab follows the hand) until the trigger is let go.
    const bool aimAtWorld = inWorld && in.focused && ph.active && !hit[p] && !sIn.leftDown && !sIn.rightDown &&
                            !(p == LLXR_HAND_RIGHT && sIn.onWristRight);
    if (sIn.worldDown && (!ph.select || !in.focused || !ph.active))
    {
        worldRelease(mask);
    }
    if (sIn.worldDown || aimAtWorld)
    {
        if (aimWorldRay(p))
        {
            LLVector3 o, d, at;
            LLCleanCursorPick::vrRay(o, d);
            const LLCleanCursorPick::Focus f = LLCleanCursorPick::vrRayHit(at);
            if (f == LLCleanCursorPick::FOCUS_HIT) sIn.worldBeam = llclamp((at - o).length(), 0.02f, kWorldBeamMax);
            else if (f == LLCleanCursorPick::FOCUS_NONE) sIn.worldBeam = kWorldBeamMax;
            if (F32 hd; LLManip::vrPointerOnHandle(&hd)) sIn.worldBeam = llclamp(hd, 0.02f, sIn.worldBeam);   // #1141: ends at an edit handle
            if (!hit[p]) LLVRHands::setBeam(p, sIn.worldBeam);
            const bool edge = ph.select && !sIn.prevWorldSelect;
            if (edge && aimAtWorld && !sIn.worldDown)
            {
                // A trigger pull away from an open context menu panel only closes it.
                if (sIn.panelOpen) closeMenuPanel();
                else worldPress(p, mask, clickLogDue());
            }
            // #1134: the context button at the world opens that target's context menu.
            if (sIn.ctxEdge[p] && aimAtWorld && !sIn.worldDown && ph.aim.valid && !sIn.leftDown)
            {
                if (sIn.panelOpen) closeMenuPanel();
                openContextMenu(p, ph, hp, mask, winW, winH);
            }
        }
    }
    else
    {
        LLCleanCursorPick::clearVRRay();
        sIn.worldBeam = kWorldBeamMax;
    }
    sIn.prevWorldSelect = ph.select;

    gViewerWindow->setVRPointerInWindow(sIn.onWall || sIn.leftDown || sIn.rightDown);

    // ---- #1143 the grip grabs what its hand's ray is on (not on the UI, not while that hand's
    // trigger / context button holds a click).
    {
        bool busy[LLXR_HAND_COUNT] = {hit[LLXR_HAND_LEFT], hit[LLXR_HAND_RIGHT] || sIn.onWristRight};
        busy[p] = busy[p] || sIn.worldDown || sIn.leftDown || sIn.rightDown;
        LLVRGrab::tick(in, inWorld, busy);
    }

    // ---- Fly cam (and its return blink), then locomotion: the thumbsticks while the wall is
    // hidden.
    flyCamTick(in, inWorld, dt);
    comfortFadeTick(inWorld, dt);   // #1078 (sends the blink's fade too)
    locomotion(in, inWorld, dt);
    // Teleport locomotion (SlipstreamVRTeleportMove): not with the wall up, in the fly cam or its
    // blink, or steering a vehicle that took the controls.
    {
        const bool vehicle = isAgentAvatarValid() && gAgentAvatarp->isSitting() && movementControlsTaken();
        const bool allowed = inWorld && in.focused && !sIn.ui.wall && !LLVRCamera::flyCam() &&
                             sIn.blink == VRInputState::BLINK_NONE && !vehicle &&
                             LLStartUp::getStartupState() == STATE_STARTED;
        LLVRTeleport::tick(in, allowed);
    }
    vignetteTick(inWorld, dt);      // #1078 (last frame's rig motion: the camera runs in idle)

    // ---- #1118 dictation: the right stick press, held, while a text field on the wall has focus;
    // #1119 with the wall hidden: to nearby chat, after a preview on the wrist.
    LLVRDictation::tick(in, sIn.ui.wall);
}

void LLVRInput::drawCursor()
{
    if (!LLXrSession::active() || !sIn.onWall || !gViewerWindow) return;
    // The mouse point is in SCALED UI coordinates (raw / display scale), and this runs after
    // setup2DRender (a raw-pixel ortho) outside any UI scale: scale as the UI draws
    // (LLViewerWindow::drawDebugText). Unscaled, the cursor was drawn at 1 / display scale of
    // where the clicks went (#1076 third headset run, display scale 1.25: "the pointer
    // location does not match where UI clicks are going").
    const F32 x = (F32)gViewerWindow->getCurrentMouseX(), y = (F32)gViewerWindow->getCurrentMouseY();
    const LLVector2& ds = gViewerWindow->getDisplayScale();
    gGL.pushUIMatrix();
    gGL.scaleUI(ds.mV[VX], ds.mV[VY], 1.f);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.color4f(0.f, 0.f, 0.f, 0.55f);
    gl_circle_2d(x, y, 9.f, 20, true);
    gGL.color4f(0.35f, 0.85f, 1.f, 1.f);
    gl_circle_2d(x, y, 6.f, 20, true);
    gGL.color4f(1.f, 1.f, 1.f, 1.f);
    gl_circle_2d(x, y, 2.f, 10, true);
    gGL.flush();
    gGL.popUIMatrix();
}
