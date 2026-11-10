/**
 * @file llvrlobby.cpp
 * @brief VR milestone M3 (#1076): the login room. See llvrlobby.h.
 */
#include "llviewerprecompiledheaders.h"

#include "llvrlobby.h"

#include "llevents.h"
#include "llsdutil.h"
#include "llstartup.h"
#include "llviewercontrol.h"
#include "vkgfx/llxrsession.h"

#include <cmath>

namespace
{
constexpr float kDoorAngle = 62.f * 3.14159265f / 180.f;   // right of the wall's centre
constexpr float kDoorDist = 2.3f;                          // from the wall's axis (where you stood)

struct LobbyState
{
    bool placed = false;
    uint32_t placeSerial = 0;           // the wall placement the room was placed from
    uint32_t spaceGeneration = 0;
    float floorY = 0.f;
    LLXrLobby layers;
    float normal[3] = {0.f, 0.f, 1.f};  // the doorway's front (towards the room)
    float right[3] = {1.f, 0.f, 0.f};   // its +X
    bool prevSelect[LLXR_HAND_COUNT] = {false, false};
    bool hover = false;
    bool haveSide = false;
    float headSide = 0.f;               // the head's signed distance to the doorway plane, last frame
    bool requested = false;             // a login was asked for from this login screen
    bool logged = false;
};
LobbyState sL;

// Rotation about +Y by a (right-handed, OpenXR), as LLXrSession's wall geometry.
void rotY(float a, const float v[3], float out[3])
{
    const float c = std::cos(a), s = std::sin(a);
    out[0] = v[0] * c + v[2] * s;
    out[1] = v[1];
    out[2] = -v[0] * s + v[2] * c;
}

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

float dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void place(const LLXrUiLayers& ui, const LLXrHeadPose& hp)
{
    static LLCachedControl<F32> s_eye_height(gSavedSettings, "SlipstreamVREyeHeight", 1.6f);
    sL.floorY = hp.floorSpace ? 0.f : -llclamp((F32)s_eye_height, 0.5f, 2.5f);   // (LOCAL: y = 0 at the start head height)
    const float local[3] = {std::sin(kDoorAngle) * kDoorDist, 0.f, -std::cos(kDoorAngle) * kDoorDist};
    float off[3];
    rotY(ui.wallYaw, local, off);
    LLXrLobby& l = sL.layers;
    l.doorCenter[0] = ui.wallAxis[0] + off[0];
    l.doorCenter[1] = sL.floorY + 0.5f * LLXrSession::kDoorHeight;
    l.doorCenter[2] = ui.wallAxis[2] + off[2];
    l.doorYaw = ui.wallYaw - kDoorAngle;
    l.floorCenter[0] = ui.wallAxis[0];
    l.floorCenter[1] = sL.floorY;
    l.floorCenter[2] = ui.wallAxis[2];
    static const float ez[3] = {0.f, 0.f, 1.f}, ex[3] = {1.f, 0.f, 0.f};
    rotY(l.doorYaw, ez, sL.normal);
    rotY(l.doorYaw, ex, sL.right);
    sL.placed = true;
    sL.placeSerial = ui.placeSerial;
    sL.spaceGeneration = hp.spaceGeneration;
    sL.haveSide = false;
    LL_INFOS("VR") << "login room: doorway at (" << l.doorCenter[0] << ", " << l.doorCenter[1] << ", " << l.doorCenter[2]
                   << "), floor at " << sL.floorY << " m (app space)" << LL_ENDL;
}

// A ray (app space) against the doorway's opening, from its front. dist: along the ray (m).
bool doorHit(const float o[3], const float d[3], float& dist)
{
    const float dn = dot3(d, sL.normal);
    if (dn >= -1e-4f) return false;   // parallel or from behind
    const float* c = sL.layers.doorCenter;
    const float rel[3] = {c[0] - o[0], c[1] - o[1], c[2] - o[2]};
    const float t = dot3(rel, sL.normal) / dn;
    if (!(t > 0.f)) return false;
    const float p[3] = {o[0] + t * d[0] - c[0], o[1] + t * d[1] - c[1], o[2] + t * d[2] - c[2]};
    if (std::fabs(dot3(p, sL.right)) > 0.5f * LLXrSession::kDoorWidth || std::fabs(p[1]) > 0.5f * LLXrSession::kDoorHeight)
        return false;
    dist = t;
    return true;
}

void logIn(const char* how)
{
    if (sL.requested) return;
    sL.requested = true;
    LL_INFOS("VR") << "login room: " << how << " -- logging in (as the Log In button)" << LL_ENDL;
    LLEventPumps::instance().obtain("LLPanelLogin").post(llsd::map("op", "onClickConnect"));
}
} // namespace

void LLVRLobby::update(const LLXrInputState& in, const LLXrHeadPose& hp, bool inWorld)
{
    LLXrSession& xr = LLXrSession::instance();
    const EStartupState state = LLStartUp::getStartupState();
    const LLXrUiLayers& ui = xr.uiLayers();
    if (inWorld || state >= STATE_STARTED || !hp.valid)
    {
        if (sL.layers.on) LL_INFOS("VR") << "login room: off (the world shows)" << LL_ENDL;
        sL.layers.on = false;
        sL.hover = false;
        xr.setLobby(sL.layers);
        return;
    }
    // Placed from the wall's placement (the wall is always up before the world); again when the
    // wall is re-placed (menu button) or the runtime re-centres the space.
    if (ui.wall && (!sL.placed || ui.placeSerial != sL.placeSerial || hp.spaceGeneration != sL.spaceGeneration)) place(ui, hp);
    if (!sL.placed)
    {
        xr.setLobby(LLXrLobby{});
        return;
    }
    if (!sL.logged)
    {
        sL.logged = true;
        LL_INFOS("VR") << "login room: on" << LL_ENDL;
    }
    sL.layers.on = true;
    const bool loginWait = state == STATE_LOGIN_WAIT;
    if (!loginWait) sL.requested = false;   // (a failed login comes back to the login screen: ask again)

    // Pointing: either hand's aim ray on the doorway (nearer than the wall); the trigger logs in.
    bool hover = false;
    for (U32 h = 0; h < LLXR_HAND_COUNT; ++h)
    {
        const LLXrHandState& hs = in.hand[h];
        const bool sel = in.focused && hs.active && hs.select;
        bool onDoor = false;
        if (in.focused && hs.active && hs.aim.valid)
        {
            static const float fwd[3] = {0.f, 0.f, -1.f};
            float d[3];
            rotateByQuat(hs.aim.orientation, fwd, d);
            float dd = 0.f, u = 0.f, v = 0.f, wd = 0.f;
            onDoor = doorHit(hs.aim.position, d, dd) && !(xr.wallHit(hs.aim.position, d, u, v, wd) && wd < dd);
        }
        if (onDoor && loginWait)
        {
            hover = true;
            if (sel && !sL.prevSelect[h])
            {
                xr.haptic(h, 0.5f, 0.05f);
                logIn("the doorway was selected");
            }
        }
        sL.prevSelect[h] = sel;
    }
    if (hover && !sL.hover)
    {
        for (U32 h = 0; h < LLXR_HAND_COUNT; ++h)
            if (in.hand[h].active) xr.haptic(h, 0.15f, 0.01f);
    }
    sL.hover = hover;
    sL.layers.doorHover = hover || sL.requested;

    // Walking through: the head crosses the doorway's plane from the room's side, inside its
    // width and below its top.
    if (hp.tracked)
    {
        const float* c = sL.layers.doorCenter;
        const float rel[3] = {hp.position[0] - c[0], hp.position[1] - c[1], hp.position[2] - c[2]};
        const float side = dot3(rel, sL.normal);
        const bool within = std::fabs(dot3(rel, sL.right)) < 0.5f * LLXrSession::kDoorWidth &&
                            hp.position[1] < sL.floorY + LLXrSession::kDoorHeight;
        if (sL.haveSide && loginWait && within && sL.headSide > 0.f && side <= 0.f) logIn("you walked through the doorway");
        sL.headSide = side;
        sL.haveSide = true;
    }
    xr.setLobby(sL.layers);
}
