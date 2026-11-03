/**
 * @file qlhizcull.h
 * @brief Quasar Viewer: culls octree groups the main view's last depth hides, on the CPU, from a read-back depth pyramid.
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

#include "aldepthpyramid.h"
#include "algpubuffer.h"
#include "llmatrix4a.h"
#include "v3math.h"

#include <deque>
#include <vector>

class LLGLSLShader;
class LLRenderTarget;

/// Hierarchical-Z occlusion culling for objects and avatars (QuasarRenderHiZCull).
///
/// After the main view's G-buffer is drawn, capture() builds a depth pyramid of its own (ALDepthPyramid, by compute)
/// and reads one coarse level of it back to the CPU without waiting (ALGPUReadback). A few frames later, once the GPU
/// has finished it, the level arrives with the view and projection it was drawn with. The cull then asks isHidden() of
/// each octree group's box before walking into it: a box whose nearest point lies behind the farthest depth under all
/// the pixels it would cover was hidden in that frame, and is skipped with everything below it.
///
/// It complements the occlusion queries (LLOcclusionCullingGroup), which need a query drawn per group and answer only
/// for groups already asked about; this answers for every group from one read.
///
/// What it trusts is a frame or few old, so it stands down when the camera has moved or turned more than a little
/// since that frame, which keeps a fast turn or a step round a corner from hiding what has just come into sight.
/// Something that moves out from behind cover can still appear a few frames late, as with the queries.
class QLHiZCull
{
public:
    QLHiZCull();

    /// Collects any finished read, then builds a pyramid from `depth` (the main view's G-buffer, final for opaque
    /// geometry and bound) and reads its coarse level back, with the modelview and projection the view was drawn with.
    /// Does nothing where the setting is off, without compute or direct state access, or while every read is pending.
    void capture(LLGLSLShader& program, LLRenderTarget& depth, const LLMatrix4a& modelview, const LLMatrix4a& projection,
                 const LLVector3& eye, const LLVector3& at_axis, bool reverse_z);

    /// Whether the box at `center` with half extents `half` (agent space) was entirely hidden in the frame the last
    /// read came from, judged from where the camera now stands at `eye` looking along `at_axis`. False whenever there is
    /// no read to trust.
    bool isHidden(const LLVector4a& center, const LLVector4a& half, const LLVector3& eye, const LLVector3& at_axis) const;

    /// Forgets the last read, as when agent space has shifted under it.
    void invalidate() { mValid = false; }

    /// Lets go of the pyramid and the reads.
    void release();

    /// Whether a read is held to cull against.
    bool isValid() const { return mValid; }

    /// What the cull is doing, for the on-screen status line (QuasarShowHiZInfo).
    enum EStatus
    {
        STATUS_OFF,             // QuasarRenderHiZCull is off
        STATUS_NO_COMPUTE,      // the session does not compute
        STATUS_NO_TEXTURE_READ, // no glGetTextureImage (GL 4.5 direct state access)
        STATUS_NO_PYRAMID,      // the pyramid did not build: its program is missing, or the target is another size
        STATUS_WAITING,         // reads are in flight, none collected yet
        STATUS_ACTIVE,          // a read is held and the cull uses it
    };
    EStatus getStatus() const { return mStatus; }
    static const char* statusName(EStatus status);

    /// The groups the last main-view cull asked about and skipped, and whether it stood down for the camera's motion.
    U32  getTestedLastFrame() const { return mTestedLastFrame; }
    U32  getCulledLastFrame() const { return mCulledLastFrame; }
    bool getMovingLastFrame() const { return mMovingLastFrame; }
    void noteCulled() const { ++mCulledThisFrame; }
    void endCull() const
    {
        mTestedLastFrame = mTestedThisFrame;
        mCulledLastFrame = mCulledThisFrame;
        mMovingLastFrame = mMovingThisFrame;
        mTestedThisFrame = 0;
        mCulledThisFrame = 0;
        mMovingThisFrame = false;
    }

private:
    // What one read was taken of.
    struct Frame
    {
        LLMatrix4a mModelviewProjection;
        LLVector3  mEye;
        LLVector3  mAtAxis;
        U32        mDepthWidth  = 0;  // the view's size in pixels
        U32        mDepthHeight = 0;
        U32        mWidth       = 0;  // the level read, in texels
        U32        mHeight      = 0;
        U32        mShift       = 0;  // depth pixel p lies under texel p >> mShift
        bool       mReverseZ    = true;
    };

    /// Collects the newest finished read into mDepth and mFrame.
    void collect();

    ALDepthPyramid     mPyramid;
    ALGPUReadback      mReadback;
    size_t             mSlotBytes = 0;
    std::deque<Frame>  mPending;  // what each read in flight was taken of, oldest first

    Frame              mFrame;    // what mDepth was taken of
    std::vector<F32>   mDepth;    // the level read, row by row from the bottom
    std::vector<F32>   mScratch;
    bool               mValid = false;

    EStatus            mStatus = STATUS_OFF;

    mutable U32        mTestedThisFrame = 0;
    mutable U32        mTestedLastFrame = 0;
    mutable U32        mCulledThisFrame = 0;
    mutable U32        mCulledLastFrame = 0;
    mutable bool       mMovingThisFrame = false;
    mutable bool       mMovingLastFrame = false;
};
