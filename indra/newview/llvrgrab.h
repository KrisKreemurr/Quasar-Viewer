/**
 * @file llvrgrab.h
 * @brief VR #1143 (epic #1072): grab and manipulate objects with the controller's grip.
 *
 * A grip squeeze (LLXrHandState::squeeze) on an object the hand's ray is on picks it: an
 * asynchronous pick along that hand's aim ray (LLCleanCursorPick's side pick: the per-frame
 * request asks for it, ~2 frames, never a GPU wait); let go before it answers = cancelled. It
 * is held when it is a linkset of
 * volumes, not an avatar / attachment / HUD / pathfinding character / permanent object, no one
 * sits on it, its bounding box (all prims, in the root's frame) is under kMaxVolume, you may
 * move it (permMove; the RLVa edit / touch rules as the desktop's) and the other hand does not
 * hold it. Then it follows the hand's grip pose rigidly -- position AND rotation, from where it
 * was relative to the hand at the press -- until the squeeze is let go:
 *
 *  - not physical: the edit path (what the build tools send). The linkset is selected
 *    (LLSelectMgr, as an edit selection: no damping, no velocity interpolation), moved LOCALLY
 *    every frame (smooth in the hand), and its root's position + rotation go to the sim as
 *    MultipleObjectUpdate (linked set) at most every kSendInterval while it moves, plus one
 *    exact update on release. The drag-start transform is saved as the build tools save it,
 *    and the linkset stays selected after release (as the build tools leave it), so Ctrl+Z
 *    (LLSelectMgr::undo, the sim's undo) applies to it; the next grab deselects it.
 *  - physical: the grab path (what Ctrl+drag sends): ObjectGrab, then ObjectGrabUpdate (the
 *    root's target position) + ObjectSpinUpdate (its target rotation) at most every
 *    kSendInterval, an exact final pair on release, then ObjectSpinStop + ObjectDeGrab. The sim
 *    simulates it (no local move): it is pulled to the hand and keeps its speed when let go
 *    (thrown / dropped).
 *
 * Only those standard messages; nothing touches inventory (no take, copy, derez or rez).
 *
 * Desktop (no VR): tick() is only called from LLVRInput::update past its VR test; follow() is
 * one static read (nothing held).
 */
#pragma once

#include "stdtypes.h"

struct LLXrInputState;

class LLVRGrab
{
public:
    // LLVRInput::update, every frame of a running session, after the pointer: grip press / release
    // edges per hand. busy[h]: hand h's ray is on the UI (wall, menu panel, wrist) or its trigger
    // holds a world click -- its grip does not start a grab then.
    static void tick(const LLXrInputState& in, bool inWorld, const bool busy[2]);
    // idle(), right after LLVRCamera::apply() (this frame's rig mapping, after the network
    // and the object updates): the held objects follow the hands; the sim updates go out.
    static void follow()
    {
        if (sHeld) followHeld();
    }
    // The session stopped / lost focus: let go of everything (final updates sent).
    static void releaseAll();
    static bool holding(U32 hand);
    // A grip press waits for a side pick (LLCleanCursorPick): other side-pick users yield.
    static bool wantsPick();

private:
    static void followHeld();
    static bool sHeld;   // any hand holds an object
};
