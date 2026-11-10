/**
 * @file llvrteleport.cpp
 * @brief VR teleport locomotion: aim an arc, release to teleport. See llvrteleport.h.
 */
#include "llviewerprecompiledheaders.h"

#include "llvrteleport.h"

#include "llagent.h"
#include "llcleancursorpick.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "llvrcamera.h"
#include "llvrgrab.h"
#include "llvrhands.h"
#include "llworld.h"
#include "vkgfx/llxrsession.h"

#include <cmath>

namespace
{
constexpr F32 kMaxDistance = 20.f;    // m from the avatar
constexpr F32 kAimOn = 0.7f;          // stick forward past this starts aiming
constexpr F32 kAimRelease = 0.3f;     // ... back under this teleports
constexpr F32 kAimCancel = -0.5f;     // ... pulled back past this cancels
constexpr F32 kWalkableNz = 0.7f;     // a surface normal's up part (~45 degrees)

struct TeleportAim
{
    bool aiming = false;
    bool rearm = true;               // the stick came back to the centre since the last aim
    bool pickPending = false;        // our side pick is in flight
    bool haveTarget = false;
    bool valid = false;
    LLVector3d targetGlobal;
    LLVector3 targetAgent;
    std::string why;                 // why the target is not valid
    U32 logs = 0;
};
TeleportAim sTP;

bool logDue()
{
    ++sTP.logs;
    return sTP.logs <= 20 || (sTP.logs % 25) == 0;
}

bool leftRay(LLVector3& o, LLVector3& d)
{
    LLQuaternion rot;
    if (!LLVRCamera::handPose(LLXR_HAND_LEFT, /*aim=*/true, o, rot)) return false;
    d = LLVector3::x_axis * rot;
    return true;
}

// The pick on the side pick's answer (its own ray, the newest answer: no GPU wait); the world
// pointer's ray is put back after.
bool resolvePick(const LLVector3& o, const LLVector3& d, S32 x, S32 y, LLPickInfo& out)
{
    if (!gViewerWindow) return false;
    LLVector3 keepO, keepD;
    const bool hadRay = LLCleanCursorPick::vrRay(keepO, keepD);
    LLCleanCursorPick::setVRRay(o, d);
    bool ok = false;
    if (LLCleanCursorPick::vrRayOn())
    {
        LLCleanCursorPick::ClickScope scope(true);
        out = gViewerWindow->pickImmediate(x, y, /*pick_transparent=*/false, /*pick_rigged=*/false);
        ok = true;
    }
    if (hadRay) LLCleanCursorPick::setVRRay(keepO, keepD);
    else LLCleanCursorPick::clearVRRay();
    return ok;
}

// Land, or a walkable surface of a world object (not an avatar / attachment / HUD), within
// reach and in a region you see.
void judge(const LLPickInfo& pick, const LLVector3& o, const LLVector3& d)
{
    sTP.haveTarget = true;
    sTP.valid = false;
    sTP.why.clear();
    const LLViewerObject* obj = pick.getObject();
    const bool hit = !pick.mPosGlobal.isExactlyZero() &&
                     (pick.mPickType == LLPickInfo::PICK_LAND || (pick.mPickType == LLPickInfo::PICK_OBJECT && obj));
    if (!hit)
    {
        sTP.targetAgent = o + d * kMaxDistance;
        sTP.targetGlobal = gAgent.getPosGlobalFromAgent(sTP.targetAgent);
        sTP.why = "nothing to stand on";
        return;
    }
    sTP.targetGlobal = pick.mPosGlobal;
    sTP.targetAgent = gAgent.getPosAgentFromGlobal(pick.mPosGlobal);
    if (pick.mPickType == LLPickInfo::PICK_OBJECT)
    {
        if (obj->isAvatar() || obj->getAvatar() || obj->isAttachment() || obj->isHUDAttachment())
        {
            sTP.why = "an avatar";
            return;
        }
        if (pick.mNormal.mV[VZ] < kWalkableNz)
        {
            sTP.why = "not walkable (too steep)";
            return;
        }
    }
    if ((sTP.targetAgent - gAgent.getPositionAgent()).magVec() > kMaxDistance)
    {
        sTP.why = "too far";
        return;
    }
    if (!LLWorld::getInstance()->positionRegionValidGlobal(sTP.targetGlobal))
    {
        sTP.why = "outside the regions you see";
        return;
    }
    sTP.valid = true;
}

void stopAiming(const char* why)
{
    if (sTP.pickPending)
    {
        LLCleanCursorPick::vrSidePickEnd();
        sTP.pickPending = false;
    }
    if (sTP.aiming && why && logDue()) LL_INFOS("VR") << "teleport: aim " << why << LL_ENDL;
    sTP.aiming = false;
    sTP.haveTarget = false;
    sTP.valid = false;
}

void teleportNow()
{
    LLVector3d pos = sTP.targetGlobal;
    if (isAgentAvatarValid()) pos.mdV[VZ] += gAgentAvatarp->getPelvisToFoot();   // (as double-click teleport)
    LL_INFOS("VR") << "teleport: to " << pos << " (" << (sTP.targetAgent - gAgent.getPositionAgent()).magVec()
                   << " m away), the double-click teleport's path" << LL_ENDL;
    gAgent.teleportViaLocationLookAt(pos);
    LLXrSession::instance().haptic(LLXR_HAND_LEFT, 0.4f, 0.05f);
}
} // namespace

bool LLVRTeleport::stickMode()
{
    static LLCachedControl<bool> s_on(gSavedSettings, "SlipstreamVRTeleportMove", false);
    return s_on;
}

void LLVRTeleport::tick(const LLXrInputState& in, bool allowed)
{
    const LLXrHandState& lh = in.hand[LLXR_HAND_LEFT];
    const bool ok = stickMode() && allowed && lh.active && isAgentAvatarValid() &&
                    gAgent.getTeleportState() == LLAgent::TELEPORT_NONE;
    if (!ok)
    {
        stopAiming("cancelled (wall up, fly cam, vehicle, teleporting or hand lost)");
        sTP.rearm = !lh.active || std::fabs(lh.stick[1]) < kAimRelease;
        return;
    }
    const F32 y = lh.stick[1];
    if (!sTP.aiming)
    {
        if (std::fabs(y) < kAimRelease) sTP.rearm = true;
        if (!sTP.rearm || y < kAimOn) return;
        sTP.aiming = true;
        sTP.rearm = false;
        sTP.haveTarget = sTP.valid = false;
        if (logDue()) LL_INFOS("VR") << "teleport: aiming (left stick forward)" << LL_ENDL;
    }
    if (y < kAimCancel)
    {
        stopAiming("cancelled (stick pulled back)");
        return;
    }
    if (y < kAimRelease)
    {
        const bool go = sTP.haveTarget && sTP.valid;
        if (!go && logDue())
            LL_INFOS("VR") << "teleport: released on no valid target (" << (sTP.haveTarget ? sTP.why : std::string("no answer yet"))
                           << "): no teleport" << LL_ENDL;
        stopAiming(nullptr);
        sTP.rearm = true;
        if (go) teleportNow();
        return;
    }

    // Aiming: the target from the side pick along the left ray, asked again as answers come.
    LLVector3 o, d;
    const bool ray = leftRay(o, d);
    if (sTP.pickPending)
    {
        LLVector3 ao, ad;
        S32 x = 0, yy = 0;
        const LLCleanCursorPick::SidePickState st = LLCleanCursorPick::vrSidePickPoll(ao, ad, x, yy);
        if (st == LLCleanCursorPick::SIDE_READY)
        {
            LLPickInfo pick;
            if (resolvePick(ao, ad, x, yy, pick))
            {
                const bool was = sTP.haveTarget && sTP.valid;
                judge(pick, ao, ad);
                if (was != sTP.valid && logDue())
                    LL_INFOS("VR") << "teleport: target " << (sTP.valid ? "valid" : "not valid: ") << sTP.why << LL_ENDL;
            }
            LLCleanCursorPick::vrSidePickEnd();
            sTP.pickPending = false;
        }
        else if (st == LLCleanCursorPick::SIDE_PENDING)
        {
            if (ray) LLCleanCursorPick::vrSidePickBegin(o, d, 0, 0);   // (the ray follows the hand)
        }
        else
        {
            sTP.pickPending = false;
        }
    }
    if (!sTP.pickPending && ray && gViewerWindow && !LLVRGrab::wantsPick() && !LLCleanCursorPick::vrSidePickActive())
    {
        LLCleanCursorPick::vrSidePickBegin(o, d, gViewerWindow->getCurrentMouseX(), gViewerWindow->getCurrentMouseY());
        sTP.pickPending = true;
    }
    if (sTP.haveTarget) LLVRHands::setTeleportArc(LLXR_HAND_LEFT, sTP.targetAgent, sTP.valid);
}

void LLVRTeleport::cancel()
{
    if (sTP.aiming || sTP.pickPending) stopAiming("cancelled (the session stopped)");
}
