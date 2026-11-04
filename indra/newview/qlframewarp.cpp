/**
 * @file qlframewarp.cpp
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

#include "llviewerprecompiledheaders.h"

#include "qlframewarp.h"

#include "llglstates.h"
#include "llrender.h"
#include "llstartup.h"
#include "lltimer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerdisplay.h"
#include "llviewershadermgr.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "pipeline.h"

namespace
{
    // A camera that moved further than this between two frames jumped (a teleport, a region crossing's shift of the
    // agent's coordinates, a camera reset): nothing between the two frames would be right.
    constexpr F32 MAX_EYE_JUMP = 8.f;

    // A frame shown at the first point is shown only once this much of the real frames' interval has passed, so that
    // it lands between the two real frames rather than just after the first.
    constexpr F64 EARLIEST_FRACTION = 0.42;

    bool frame_warp_wanted()
    {
        static LLCachedControl<bool> frame_warp(gSavedSettings, "QuasarFrameWarp", false);
        return frame_warp();
    }
}

QLFrameWarp& QLFrameWarp::instance()
{
    static QLFrameWarp warp;
    return warp;
}

bool QLFrameWarp::ensureTargets(U32 width, U32 height)
{
    if (mWorld.getWidth() != width || mWorld.getHeight() != height || !mWorld.isComplete())
    {
        mWorld.release();
        if (!mWorld.allocate(width, height, GL_RGBA8))
        {
            return false;
        }
    }
    if (mFrame.getWidth() != width || mFrame.getHeight() != height || !mFrame.isComplete())
    {
        mFrame.release();
        if (!mFrame.allocate(width, height, GL_RGBA8))
        {
            return false;
        }
    }
    return true;
}

void QLFrameWarp::release()
{
    mWorld.release();
    mFrame.release();
    mValid = false;
    mWorldKept = false;
}

void QLFrameWarp::captureWorld(bool normal)
{
    mWorldKept = false;
    mValid = false;

    if (!frame_warp_wanted())
    {
        if (mStatus != OFF)
        {
            release();
            mStatus = OFF;
        }
        return;
    }
    if (!normal || !gViewerWindow)
    {
        return;
    }
    if (!gQLFrameWarpProgram.isComplete())
    {
        mStatus = NO_PROGRAM;
        return;
    }
    if (mStatus == OFF || mStatus == NO_PROGRAM)
    {
        mStatus = WAITING;
    }

    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("quasar warp keep world");
    LL_PROFILE_GPU_ZONE("quasar warp keep world");

    const U32 width = (U32)gViewerWindow->getWindowWidthRaw();
    const U32 height = (U32)gViewerWindow->getWindowHeightRaw();
    if (width == 0 || height == 0 || !ensureTargets(width, height))
    {
        return;
    }
    mWorld.copyFromWindow(0, 0, width, height);

    const LLViewerCamera* camera = LLViewerCamera::getInstance();
    mProjection = camera->getProjection();
    mViewProjection.setMul(camera->getModelview(), mProjection);
    mOrigin = camera->getOrigin();
    mRotation = camera->getQuaternion();

    const LLRenderTarget& depth = gPipeline.mMainRT.deferredScreen;
    mDepthSerial = depth.getDepthSerial();
    mDepthWidth = depth.getWidth();
    mDepthHeight = depth.getHeight();

    mWorldKept = true;
}

void QLFrameWarp::captureFrame()
{
    if (!mWorldKept)
    {
        return;
    }
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("quasar warp keep frame");
    LL_PROFILE_GPU_ZONE("quasar warp keep frame");
    mFrame.copyFromWindow(0, 0, mFrame.getWidth(), mFrame.getHeight());
    mValid = true;
}

void QLFrameWarp::noteRealSwap(bool presented)
{
    const F64 now = LLTimer::getTotalSeconds();
    if (presented)
    {
        const F64 dt = now - mLastSwap;
        if (dt > 0.0 && dt < 0.25)
        {
            mInterval = mInterval * 0.9 + dt * 0.1;
        }
        mLastSwap = now;
    }
    else
    {
        mValid = false;
    }
    mShown = false;
    mWorldKept = false;

    if (now - mCountStart >= 1.0)
    {
        mLastGenerated = mGenerated;
        mGenerated = 0;
        mCountStart = now;
    }
}

void QLFrameWarp::between(bool last)
{
    if (!mValid || mShown)
    {
        return;
    }
    if (!frame_warp_wanted())
    {
        mValid = false;
        return;
    }
    if (LLStartUp::getStartupState() != STATE_STARTED || !gViewerWindow || !gViewerWindow->getWindow() ||
        gViewerWindow->getWindow()->getMinimized() || gTeleportDisplay)
    {
        mValid = false;
        return;
    }

    // Above this the screen gains little from twice as many frames, and the second swap a frame costs more than it is
    // worth.
    static LLCachedControl<F32> below_fps(gSavedSettings, "QuasarFrameWarpBelowFPS", 170.f);
    if (below_fps() > 0.f && mInterval < 1.0 / (F64)below_fps())
    {
        mStatus = FAST;
        return;
    }

    const F64 elapsed = (F64)LLTimer::getTotalSeconds() - mLastSwap;
    if (!last && elapsed < mInterval * EARLIEST_FRACTION)
    {
        return;
    }

    // What was kept must still be what is there: the window's size, and the G-buffer depth the kept frame drew.
    const LLRenderTarget& depth = gPipeline.mMainRT.deferredScreen;
    if ((U32)gViewerWindow->getWindowWidthRaw() != mFrame.getWidth() ||
        (U32)gViewerWindow->getWindowHeightRaw() != mFrame.getHeight() || depth.getDepthSerial() != mDepthSerial ||
        depth.getWidth() != mDepthWidth || depth.getHeight() != mDepthHeight)
    {
        mValid = false;
        return;
    }

    const LLViewerCamera* camera = LLViewerCamera::getInstance();
    if ((camera->getOrigin() - mOrigin).length() > MAX_EYE_JUMP)
    {
        mValid = false;
        return;
    }

    // Where between the two real frames this one lands, taking the next to come one interval after the last.
    const F32 alpha = (F32)llclamp(elapsed / llmax(mInterval, 1e-4), 0.1, 0.9);
    present(alpha);
}

void QLFrameWarp::present(F32 alpha)
{
    // A target bound here would be drawn into instead of the window.
    if (LLRenderTarget::getCurrentBoundTarget() != nullptr)
    {
        return;
    }

    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("quasar frame warp");
    LL_PROFILE_GPU_ZONE("quasar frame warp");
    const F64 started = LLTimer::getTotalSeconds();

    // The generated frame's camera: part of the way from the kept frame's to the next frame's, under the kept frame's
    // projection, which the depth was stored with.
    const LLViewerCamera* camera = LLViewerCamera::getInstance();
    LLCamera mid = *camera;
    const LLVector3 next_origin = camera->getOrigin();
    mid.setOrigin(mOrigin + (next_origin - mOrigin) * alpha);
    mid.setAxes(slerp(alpha, mRotation, camera->getQuaternion()));

    LLMatrix4a mid_view_projection;
    mid_view_projection.setMul(mid.frameModelview(), mProjection);
    LLMatrix4a kept_inverse;
    if (!kept_inverse.setInverse(mViewProjection))
    {
        mValid = false;
        return;
    }
    // The kept frame's clip space, back to the world and on into the generated camera's.
    LLMatrix4a reproject;
    reproject.setMul(kept_inverse, mid_view_projection);

    const S32 width = (S32)mFrame.getWidth();
    const S32 height = (S32)mFrame.getHeight();
    const LLRect world = gViewerWindow->getWorldViewRectRaw();

    gGL.flush();
    const S32 saved_viewport[4] = { gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3] };
    gGL.setViewport(0, 0, width, height);
    {
        LLGLDisable blend(GL_BLEND);
        LLGLDisable scissor(GL_SCISSOR_TEST);
        LLGLDisable cull(GL_CULL_FACE);
        LLGLDepthTest depth_test(GL_FALSE, GL_FALSE);
        gGL.setColorMask(true, true);

        LLGLSLShader& shader = gQLFrameWarpProgram;
        shader.bind();
        const S32 world_unit = shader.bindTexture(LLShaderMgr::QL_WARP_WORLD_MAP, &mWorld, ALSamplers::BilinearClamp);
        const S32 frame_unit = shader.bindTexture(LLShaderMgr::QL_WARP_FRAME_MAP, &mFrame, ALSamplers::PointClamp);
        const S32 depth_unit = shader.bindDepthTexture(LLShaderMgr::DEFERRED_DEPTH, &gPipeline.mMainRT.deferredScreen);
        shader.uniformMatrix4fv(LLShaderMgr::QL_WARP_REPROJECT, reproject);
        shader.uniform4f(LLShaderMgr::QL_WARP_WORLD_RECT, (F32)world.mLeft, (F32)world.mBottom, (F32)world.getWidth(),
                         (F32)world.getHeight());
        shader.uniform2f(LLShaderMgr::QL_WARP_WINDOW_SIZE, (F32)width, (F32)height);

        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

        for (S32 unit : { world_unit, frame_unit, depth_unit })
        {
            if (unit >= 0)
            {
                gGL.getTextureSlot(unit)->unbind();
            }
        }
        shader.unbind();
        LLVertexBuffer::unbind();
    }

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("quasar frame warp swap");
        gViewerWindow->getWindow()->swapBuffers();
    }
    gGL.setViewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);

    mShown = true;
    ++mGenerated;
    mStatus = ACTIVE;
    mLastWarpMs = (F32)(((F64)LLTimer::getTotalSeconds() - started) * 1000.0);
}
