/**
 * @file qlframewarp.h
 * @brief Quasar Viewer: native frame generation by camera reprojection.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Quasar Viewer Source Code (based on Alchemy Viewer)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#pragma once

#include "llmatrix4a.h"
#include "llquaternion.h"
#include "llrendertarget.h"
#include "lluuid.h"
#include "v3math.h"

#include <vector>

/// One generated frame between each two real ones (QuasarFrameWarp).
///
/// Each real frame is kept as it is shown: its world as the final blit left it, before the interface, the whole frame,
/// a copy of its depth, its camera, and the nearest avatars with their boxes and positions. A generated frame is made
/// of kept frames by reprojecting their world by their depth to a camera between two real frames' cameras, the
/// interface laid over it as the newest frame drew it, and swapped in on its own, so the screen shows two frames for
/// each one the viewer renders. A surface in an avatar's box moves with that avatar, so one walking, flying or riding
/// moves as smoothly as the camera; its limbs keep a real frame's pose, and anything else that moves (a door, a vehicle)
/// moves at the rendered rate.
///
/// QuasarFrameWarpMode picks how:
///   0, from one frame: once the idle step has moved the camera on, the last real frame is warped part of the way
///      toward it. No delay, but what the last frame did not see (behind an edge the camera moves past) is guessed.
///   1, from two: each real frame, once drawn, is held back; the generated frame between it and the one before goes
///      out in its place, made of both, and the held frame goes out halfway to the next. What one of the two did not
///      see the other usually did. Every frame reaches the screen about half a rendered frame later.
///
/// A frame goes out between two real frames at the first of two points that is far enough from the last swap to land
/// halfway: after the idle step, or, failing that, once the next frame has culled and sorted and is about to draw.
class QLFrameWarp
{
public:
    enum EStatus
    {
        OFF,            // the setting is off
        NO_PROGRAM,     // the warp program did not build
        WAITING,        // no frame kept yet
        ACTIVE,         // generating
        FAST,           // the real frame rate is already above QuasarFrameWarpBelowFPS
    };

    static QLFrameWarp& instance();

    /// render_ui, just after renderFinalize: keeps the world as the final blit left it, its depth, and the camera it
    /// was drawn with. `normal` is false for a frame that is not the main view's as the window shows it (a snapshot, a
    /// zoomed or tiled render, a cutout), which drops what was kept.
    void captureWorld(bool normal);

    /// display(), after the interface and just before the real frame's swap: keeps the frame as it will be shown.
    void captureFrame();

    /// display(), in place of the real frame's swap. From two frames, shows the generated frame between the last real
    /// frame and this one and holds this one back for later, and answers true: the caller does not swap. False where
    /// nothing was generated, for the caller to swap this frame as it would.
    bool swapInstead();

    /// display(), just after the real frame's swap (or swapInstead's).
    void noteRealSwap(bool presented);

    /// Drops the kept frames: a snapshot drew, the world shifted, the window changed. A held real frame is still shown.
    void invalidate();

    /// A point between two real frames where a frame may go out (a generated one from one frame, the held real one
    /// from two); `last` is the latest such point, which sends it if no earlier point did.
    void between(bool last);

    /// Frees the targets (shutdown, a context lost, or the setting switched off).
    void release();

    EStatus status() const { return mStatus; }
    bool twoFrames() const;
    U32 lastGenerated() const { return mLastGenerated; }
    F32 lastWarpMs() const { return mLastWarpMs; }
    F32 baseIntervalMs() const { return (F32)(mInterval * 1000.0); }
    U32 lastAvatars() const { return mLastAvatars; }

    /// The most avatars that move on their own in a generated frame; the shader's MAX_WARP_AVATARS.
    static constexpr U32 MAX_AVATARS = 32;

private:
    QLFrameWarp() = default;

    // An avatar as a kept frame drew it.
    struct KeptAvatar
    {
        LLUUID    mID;
        LLVector3 mPosition;    // the agent's space
        LLVector3 mMin;         // its box, padded
        LLVector3 mMax;
        F32       mDistance;
    };

    // A real frame as it was shown.
    struct Kept
    {
        LLRenderTarget mWorld;          // the world before the interface, window sized
        LLRenderTarget mFrame;          // the frame as shown, window sized
        LLRenderTarget mDepth;          // the G-buffer's depth, at its size
        LLMatrix4a     mProjection;
        LLMatrix4a     mViewProjection;
        LLVector3      mOrigin;
        LLQuaternion   mRotation;
        std::vector<KeptAvatar> mAvatars;   // the nearest, nearest first
        U64            mSerial = 0;     // which real frame, counted from 1
        bool           mComplete = false;
    };

    bool ensureTargets(Kept& kept, U32 width, U32 height, U32 depth_width, U32 depth_height);
    void keepAvatars(Kept& kept);
    // Whether a kept frame still fits the window.
    bool fitsWindow(const Kept& kept) const;
    // Draws the generated frame from `from` (and `next`, from two) for the camera `mid_view_projection` and swaps it.
    bool present(const Kept& from, const Kept* next, const LLMatrix4a& mid_view_projection, F32 alpha);
    // Shows the held-back real frame.
    void showHeld();
    // QuasarFrameWarpDump: whether this generated frame is one to save, and saving it.
    bool dumpDue() const;
    bool ensureDumpTarget(LLRenderTarget& target, S32 width, S32 height);
    void writeDump(const Kept& from, const Kept* next, F32 alpha, S32 avatars, U32 moving);

    Kept mKept[2];
    S32  mNewest  = -1;             // the newest complete kept frame
    S32  mFilling = -1;             // the one this frame is being kept into
    S32  mHeld    = -1;             // from two frames, the real frame drawn but not yet shown
    U64  mSerial  = 0;

    // The next frame's camera, taken once the idle step has moved it and before the frame's display can move the
    // main camera about for its own passes (the mirrors, the probes), as the next frame will be drawn with it.
    LLVector3    mNextOrigin;
    LLQuaternion mNextRotation;
    bool         mNextKept = false;

    bool mShown = false;            // from one frame, a generated frame went out since the last real swap

    F64  mLastSwap  = 0.0;          // seconds, the last real frame's swap (from two, its generated frame's)
    F64  mInterval  = 1.0 / 60.0;   // seconds between real swaps, smoothed

    EStatus mStatus = OFF;
    U32     mGenerated = 0;
    U32     mLastGenerated = 0;     // generated frames over the last second
    F64     mCountStart = 0.0;
    F32     mLastWarpMs = 0.f;
    U32     mLastAvatars = 0;       // avatars moving in the last generated frame

    // QuasarFrameWarpDump: the generated frame shown and tinted, the sets saved, and when the next is due.
    LLRenderTarget mDumpGen;
    LLRenderTarget mDumpTint;
    U32            mDumpSets = 0;
    F64            mNextDump = 0.0;
};
