/**
 * @file llvrteleport.h
 * @brief VR teleport locomotion (epic #1072, M3 locomotion): aim an arc, release to teleport.
 *
 * With SlipstreamVRTeleportMove on, the LEFT stick is the teleport stick instead of the walk
 * stick (the right stick keeps snap / smooth turn and up / down): push it forward and hold to
 * aim -- an arc from the left controller to a landing marker -- and let it go back to the
 * centre to teleport there. Pull it back while aiming to cancel. The marker is blue on a valid
 * target -- land, or a walkable surface (normal within ~45 degrees of up) of an object that is
 * not an avatar or attachment, within kMaxDistance of you -- and red otherwise (no teleport).
 *
 * The target is the asynchronous VR side pick along the left aim ray (LLCleanCursorPick: the
 * per-frame request asks for it, ~2 frames, never a GPU wait), asked again as each answer comes
 * in. The teleport is the double-click teleport's own call (gAgent.teleportViaLocationLookAt,
 * the foot offset as LLToolPie adds it): the server applies the parcel / region rules. The
 * comfort fade across a teleport (#1078) covers it. Aiming is cancelled while the UI wall is
 * up, in the fly cam, seated on a vehicle that took the controls, and when the left hand is
 * lost.
 *
 * Desktop: tick() is only called from LLVRInput::update past its VR test.
 */
#pragma once

#include "stdtypes.h"

struct LLXrInputState;

class LLVRTeleport
{
public:
    // The left stick is the teleport stick (SlipstreamVRTeleportMove): locomotion does not walk with it.
    static bool stickMode();
    // LLVRInput::update, every frame of a running session. allowed: in the world, focused,
    // the wall hidden, not in the fly cam / its blink, not steering a vehicle.
    static void tick(const LLXrInputState& in, bool allowed);
    // The session stopped: stop aiming (no teleport).
    static void cancel();
};
