/**
 * @file qlhizcull.cpp
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

#include "llviewerprecompiledheaders.h"

#include "qlhizcull.h"

#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llviewercontrol.h"

namespace
{
    // The level read back is the finest whose width is at most this many texels: a few thousand floats a read, each
    // texel the farthest depth of a block of pixels 2^(level + 1) on a side (32 at 1080p).
    constexpr U32 MAX_READ_WIDTH = 160;
    // Never finer than this level, whatever the window's size.
    constexpr U32 MIN_READ_LEVEL = 2;

    // Reads in flight at once: the frame pacing lets the GPU run up to three frames behind, so a read is collected three
    // or four frames after it was asked for.
    constexpr U32 READ_SLOTS = 5;

    // How far the camera may have moved, in metres, and turned (the cosine of the angle) since the frame a read was
    // taken of before the read is no longer trusted.
    constexpr F32 MAX_EYE_MOVE   = 0.5f;
    constexpr F32 MIN_AT_COSINE  = 0.99905f;  // about 2.5 degrees

    // A box wider than this many texels of the level read each way covers much of the screen, and is kept rather than
    // read texel by texel.
    constexpr S32 MOST_READ = 8;

    bool enabled()
    {
        static LLCachedControl<bool> hiz_cull(gSavedSettings, "QuasarRenderHiZCull", true);
        return hiz_cull;
    }
}

QLHiZCull::QLHiZCull()
{
    mPyramid.getTarget().setLabel("quasar.hiz.pyramid");
}

void QLHiZCull::capture(LLGLSLShader& program, LLRenderTarget& depth, const LLMatrix4a& modelview,
                        const LLMatrix4a& projection, const LLVector3& eye, const LLVector3& at_axis, bool reverse_z)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;

    const EStatus off = !enabled() ? STATUS_OFF
                      : !LLRender::sCompute ? STATUS_NO_COMPUTE
                      : !ALGPUReadback::canReadTextureImages() ? STATUS_NO_TEXTURE_READ
                      : STATUS_ACTIVE;
    if (off != STATUS_ACTIVE)
    {
        if (mPyramid.isAllocated() || mReadback.isAllocated() || mValid)
        {
            release();
        }
        mStatus = off;
        return;
    }

    collect();

    // Every read still in flight: building a pyramid now would have nowhere to go.
    if (mReadback.isAllocated() && mReadback.getPending() >= READ_SLOTS)
    {
        return;
    }

    if (!mPyramid.build(program, depth))
    {
        if (!mValid)
        {
            mStatus = STATUS_NO_PYRAMID;
        }
        return;
    }

    const LLRenderTarget& target = mPyramid.getTarget();
    U32 level = MIN_READ_LEVEL;
    while (level + 1 < mPyramid.getLevels() && (target.getWidth() >> level) > MAX_READ_WIDTH)
    {
        ++level;
    }
    const U32 width  = llmax(target.getWidth() >> level, 1u);
    const U32 height = llmax(target.getHeight() >> level, 1u);
    const size_t bytes = (size_t)width * height * sizeof(F32);

    if (!mReadback.isAllocated() || bytes > mSlotBytes)
    {
        // A new store drops whatever was in flight in the old one.
        mPending.clear();
        mReadback.release();
        if (!mReadback.allocate(bytes, READ_SLOTS, "quasar.hiz.readback"))
        {
            mSlotBytes = 0;
            return;
        }
        mSlotBytes = bytes;
    }

    if (!mReadback.requestTextureImage(target.getTexture(), (S32)level, GL_RED, GL_FLOAT, bytes))
    {
        return;
    }

    Frame frame;
    frame.mModelviewProjection.setMul(modelview, projection);
    frame.mEye         = eye;
    frame.mAtAxis      = at_axis;
    frame.mDepthWidth  = mPyramid.getDepthWidth();
    frame.mDepthHeight = mPyramid.getDepthHeight();
    frame.mWidth       = width;
    frame.mHeight      = height;
    frame.mShift       = level + 1;
    frame.mReverseZ    = reverse_z;
    mPending.push_back(frame);
    if (!mValid)
    {
        mStatus = STATUS_WAITING;
    }
}

void QLHiZCull::collect()
{
    while (!mPending.empty() && mReadback.getPending() > 0)
    {
        const Frame& oldest = mPending.front();
        const size_t bytes  = (size_t)oldest.mWidth * oldest.mHeight * sizeof(F32);
        mScratch.resize(oldest.mWidth * oldest.mHeight);
        if (!mReadback.collect(mScratch.data(), bytes))
        {
            break;
        }
        mDepth.swap(mScratch);
        mFrame = oldest;
        mValid = true;
        mStatus = STATUS_ACTIVE;
        mPending.pop_front();
    }
}

bool QLHiZCull::isHidden(const LLVector4a& center, const LLVector4a& half, const LLVector3& eye,
                         const LLVector3& at_axis) const
{
    if (!mValid || mDepth.empty() || !enabled())
    {
        return false;
    }
    ++mTestedThisFrame;

    // Too far from the frame the read was taken of to trust it.
    if ((eye - mFrame.mEye).lengthSquared() > MAX_EYE_MOVE * MAX_EYE_MOVE || (at_axis * mFrame.mAtAxis) < MIN_AT_COSINE)
    {
        mMovingThisFrame = true;
        return false;
    }

    const F32 cx = center[0], cy = center[1], cz = center[2];
    const F32 hx = half[0], hy = half[1], hz = half[2];

    // A box the camera now stands in, or near enough to have stepped into since, is in view.
    if (fabsf(eye.mV[VX] - cx) <= hx + MAX_EYE_MOVE && fabsf(eye.mV[VY] - cy) <= hy + MAX_EYE_MOVE &&
        fabsf(eye.mV[VZ] - cz) <= hz + MAX_EYE_MOVE)
    {
        return false;
    }

    // As depth_pyramid_box_visible (depthPyramidUtil.glsl), against the one level read.
    F32 ndc_lo_x = 1.f, ndc_lo_y = 1.f, ndc_hi_x = -1.f, ndc_hi_y = -1.f;
    F32 nearest = mFrame.mReverseZ ? 0.f : 1.f;
    for (S32 i = 0; i < 8; ++i)
    {
        LLVector4a corner;
        corner.set((i & 1) ? cx + hx : cx - hx, (i & 2) ? cy + hy : cy - hy, (i & 4) ? cz + hz : cz - hz, 1.f);
        LLVector4a clip;
        mFrame.mModelviewProjection.transform4(corner, clip);
        const F32 w = clip[3];
        if (!(w > 0.f))
        {
            // Reaches the eye's plane, or is not a number.
            return false;
        }
        const F32 x = clip[0] / w;
        const F32 y = clip[1] / w;
        const F32 z = clip[2] / w;
        ndc_lo_x = llmin(ndc_lo_x, x);
        ndc_lo_y = llmin(ndc_lo_y, y);
        ndc_hi_x = llmax(ndc_hi_x, x);
        ndc_hi_y = llmax(ndc_hi_y, y);
        if (mFrame.mReverseZ)
        {
            nearest = llmax(nearest, z);
        }
        else
        {
            nearest = llmin(nearest, z * 0.5f + 0.5f);
        }
    }

    // Past the edge of the frame the read was taken of: what lay off that screen was never drawn into its depth, and the
    // camera may have turned to bring it in since, so only a box wholly on that screen is judged.
    if (ndc_lo_x < -1.f || ndc_hi_x > 1.f || ndc_lo_y < -1.f || ndc_hi_y > 1.f)
    {
        return false;
    }

    const S32 size_x = (S32)mFrame.mDepthWidth;
    const S32 size_y = (S32)mFrame.mDepthHeight;
    const S32 px_lo = llclamp((S32)floorf((ndc_lo_x * 0.5f + 0.5f) * size_x), 0, size_x - 1);
    const S32 py_lo = llclamp((S32)floorf((ndc_lo_y * 0.5f + 0.5f) * size_y), 0, size_y - 1);
    const S32 px_hi = llclamp((S32)floorf((ndc_hi_x * 0.5f + 0.5f) * size_x), 0, size_x - 1);
    const S32 py_hi = llclamp((S32)floorf((ndc_hi_y * 0.5f + 0.5f) * size_y), 0, size_y - 1);

    const S32 tx_lo = llmin(px_lo >> mFrame.mShift, (S32)mFrame.mWidth - 1);
    const S32 ty_lo = llmin(py_lo >> mFrame.mShift, (S32)mFrame.mHeight - 1);
    const S32 tx_hi = llmin(px_hi >> mFrame.mShift, (S32)mFrame.mWidth - 1);
    const S32 ty_hi = llmin(py_hi >> mFrame.mShift, (S32)mFrame.mHeight - 1);
    if (tx_hi - tx_lo >= MOST_READ || ty_hi - ty_lo >= MOST_READ)
    {
        return false;
    }

    F32 farthest = mFrame.mReverseZ ? 1.f : 0.f;
    for (S32 ty = ty_lo; ty <= ty_hi; ++ty)
    {
        const F32* row = mDepth.data() + (size_t)ty * mFrame.mWidth;
        for (S32 tx = tx_lo; tx <= tx_hi; ++tx)
        {
            farthest = mFrame.mReverseZ ? llmin(farthest, row[tx]) : llmax(farthest, row[tx]);
        }
    }

    // Hidden only where the box's nearest point is strictly behind the farthest of what covered it; a NaN on either
    // side compares false, and the box is kept.
    return mFrame.mReverseZ ? (nearest < farthest) : (nearest > farthest);
}

// static
const char* QLHiZCull::statusName(EStatus status)
{
    switch (status)
    {
    case STATUS_OFF:             return "off (QuasarRenderHiZCull)";
    case STATUS_NO_COMPUTE:      return "unavailable: no compute shaders";
    case STATUS_NO_TEXTURE_READ: return "unavailable: no OpenGL 4.5 texture reads";
    case STATUS_NO_PYRAMID:      return "unavailable: depth pyramid did not build";
    case STATUS_WAITING:         return "starting";
    case STATUS_ACTIVE:          return "active";
    }
    return "unknown";
}

void QLHiZCull::release()
{
    mPyramid.release();
    mReadback.release();
    mSlotBytes = 0;
    mPending.clear();
    mDepth.clear();
    mScratch.clear();
    mValid = false;
    mStatus = STATUS_OFF;
}
