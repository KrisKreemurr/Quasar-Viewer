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
#include "v3math.h"

/// One generated frame between each two real ones (QuasarFrameWarp).
///
/// A real frame is kept twice as it is shown: the world as the final blit left it, before the interface, and the
/// whole frame. Between that frame and the next, once the next frame's camera is known (the idle step has moved it),
/// a frame is made of the kept one: the world reprojected by the G-buffer's depth, which the next frame has not yet
/// cleared, to a camera part of the way from the kept frame's camera to the next one's, with the interface laid over
/// it as it was. It is swapped in on its own, so the screen shows two frames for each one the viewer renders.
///
/// It moves the camera only: what moves in the world (avatars walking, a door) moves at the rendered rate, where the
/// kept frame put it. Surfaces the kept frame did not see (behind an edge the camera moves past) take the nearest
/// surface it did, the stretch every reprojection shows at an edge.
///
/// It is shown at the first of two points that is far enough from the last real frame's swap to land between it and
/// the next: after the idle step, or, failing that, once the next frame has culled and sorted and is about to draw.
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

    /// render_ui, just after renderFinalize: keeps the world as the final blit left it, and the camera it was drawn
    /// with. `normal` is false for a frame that is not the main view's as the window shows it (a snapshot, a zoomed or
    /// tiled render, a cutout), which drops what was kept.
    void captureWorld(bool normal);

    /// display(), after the interface and just before the real frame's swap: keeps the frame as it will be shown.
    void captureFrame();

    /// display(), just after the real frame's swap.
    void noteRealSwap(bool presented);

    /// Drops what is kept: a snapshot drew over the depth, the world shifted, the window changed size.
    void invalidate() { mValid = false; }

    /// A point between two real frames where a generated one may be shown; `last` is the latest such point, which
    /// shows it if no earlier point did.
    void between(bool last);

    /// Frees the targets (shutdown, or the setting switched off).
    void release();

    EStatus status() const { return mStatus; }
    U32 lastGenerated() const { return mLastGenerated; }
    F32 lastWarpMs() const { return mLastWarpMs; }
    F32 baseIntervalMs() const { return (F32)(mInterval * 1000.0); }

private:
    QLFrameWarp() = default;

    bool ensureTargets(U32 width, U32 height);
    void present(F32 alpha);

    LLRenderTarget mWorld;          // the world before the interface, window sized
    LLRenderTarget mFrame;          // the frame as shown, window sized

    // The kept frame's camera.
    LLMatrix4a   mProjection;
    LLMatrix4a   mViewProjection;
    LLVector3    mOrigin;
    LLQuaternion mRotation;
    U64          mDepthSerial = 0;  // the G-buffer depth's build the kept frame was drawn into
    U32          mDepthWidth  = 0;
    U32          mDepthHeight = 0;

    bool mWorldKept = false;        // captureWorld ran for the frame being shown
    bool mValid     = false;        // a whole frame is kept and its depth stands
    bool mShown     = false;        // a generated frame was shown since the last real swap

    F64  mLastSwap  = 0.0;          // seconds, the last real frame's swap
    F64  mInterval  = 1.0 / 60.0;   // seconds between real swaps, smoothed

    EStatus mStatus = OFF;
    U32     mGenerated = 0;
    U32     mLastGenerated = 0;     // generated frames over the last second
    F64     mCountStart = 0.0;
    F32     mLastWarpMs = 0.f;
};
