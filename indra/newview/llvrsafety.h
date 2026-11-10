/**
 * @file llvrsafety.h
 * @brief VR milestone M3 (#1076, epic #1072): the frame-rate safety governor.
 *
 * In VR a frame rate below the headset's refresh, and uneven pacing, are what cause motion
 * sickness. While the session runs and you are in the world, the governor watches xrWaitFrame's
 * pacing (LLXrSession::frameTiming) in half-second windows, judged against the headset's NATIVE
 * period (#1137: the fastest display period the runtime predicted this session -- SteamVR
 * throttles to 1/2 or 1/3 of it when frames are late, and "clean pacing at the throttled rate"
 * made the first version restore quality it could not afford and oscillate), together with our
 * own frame cost (LLXrSession::frameWorkSec, the CPU span of the frame, and the GPU frame time
 * where LLVKAutoTune has one):
 *  - throttled by the runtime while our frame fits the native rate (< 90 % of its period, the
 *    GPU frame time measured -- without it the CPU span alone cannot tell): hold
 *    the level (stepping down would not lift the throttle); give a level back only with clear
 *    headroom (< 70 %) after its dwell;
 *  - otherwise misses of native slots step down, draw distance first -- soft caps through the
 *    same mechanism as "Keep my FPS" (LLVKPerfCaps, merged in LLVulkanContext, most restrictive
 *    wins; settings are never written):
 *  - at once, two levels: a window below half the headset's rate;
 *  - one level: sustained misses -- 2 of the last 3 windows over 10 %, or the last 2 s over 8 %
 *    (a single hitch is not a failure); ignored for 3 s after any level change (its own churn).
 * Nothing counts for 5 s after entering the world or arriving from a teleport (the scene streams
 * in; #1137: level 5 at session start). It restores one level at a time after that level's
 * dwell of clean windows (<= 3 % of native slots missed and, where the cost is known, our frame
 * under 80 % of the native period):
 * 10 s, 30 s for the two restores that raise the render scale (70 -> 85 -> 100 %, at levels 5
 * and 3 only, so the frequent 3 <-> 4 moves never toggle it). A restore undone within 30 s
 * doubles that level's dwell (up to 10 min); one that holds 3 min resets it. Every step is
 * logged (tag "VR"), with a summary every 10 s (native rate, the runtime's rate, throttled
 * share, native slots missed, our CPU span and GPU time against the budget). (#1076 headset run 1: the old 1 % / 5 % thresholds with a 1 s settle
 * flapped 3 <-> 4 every minute at a steady 45 Hz, toggling the render scale each time.)
 *
 * SlipstreamVRSafetyGovernor (Preferences > Safety, on by default) turns it off; off restores
 * everything at once.
 *
 * Desktop (no VR): tick() is only called from LLVRInput::update, which returns first.
 */
#pragma once

class LLVRSafety
{
public:
    // Once per frame (LLVRInput::update, VR only). inWorld: the head-tracked world is shown.
    static void tick(bool inWorld);
    // The session stopped: every cap off at once.
    static void reset();
};
