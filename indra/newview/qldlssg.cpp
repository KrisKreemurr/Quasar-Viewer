/**
 * @file qldlssg.cpp
 * @brief Quasar Viewer: the inputs NVIDIA DLSS Frame Generation reads, made from each frame the viewer draws.
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

#include "qldlssg.h"

#if LL_WINDOWS && !LL_SDL_WINDOW
#include "qlvulkanpresenter.h"
#define QL_VULKAN_PRESENTER 1
#endif

#if QL_VULKAN_PRESENTER

#include "llframetimer.h"
#include "llglslshader.h"
#include "llglstates.h"
#include "llmatrix4a.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llviewercamera.h"
#include "llviewerwindow.h"
#include "pipeline.h"

#include <cmath>
#include <cstring>

extern LLGLSLShader gQLDLSSGInputsProgram;  // llviewershadermgr.cpp

namespace
{
    // How far the camera may move between two frames before the second is taken as a cut (a teleport, a camera jump)
    // rather than motion: frame generation then makes nothing between them.
    constexpr F32 CUT_DISTANCE = 32.f;

    // What the distance to the far plane is given as when the projection has none (the infinite reverse-Z one).
    constexpr F32 INFINITE_FAR = 100000.f;

    struct Inputs
    {
        LLRenderTarget mTarget;  // 0: depth, R32F; 1: motion, RG16F
        LLMatrix4a     mPrevViewProjection;
        LLVector3      mPrevOrigin;
        U32            mPrevFrame = 0;
        bool           mHavePrev = false;
        bool           mWarnedTarget = false;
    };
    Inputs sInputs;

    bool ensure_target(U32 width, U32 height)
    {
        LLRenderTarget& target = sInputs.mTarget;
        if (target.isComplete() && target.getWidth() == width && target.getHeight() == height)
        {
            return true;
        }
        target.release();
        if (!target.allocate(width, height, GL_R32F) || !target.addColorAttachment(GL_RG16F))
        {
            target.release();
            if (!sInputs.mWarnedTarget)
            {
                LL_WARNS("Vulkan") << "DLSS Frame Generation: no " << width << "x" << height
                                   << " target for the depth and motion" << LL_ENDL;
                sInputs.mWarnedTarget = true;
            }
            return false;
        }
        return true;
    }

    void copy_matrix(float* dst, const LLMatrix4a& m)
    {
        LLMatrix4a copy = m;
        memcpy(dst, copy.getF32ptr(), sizeof(F32) * 16);
    }

    void copy_vector(float* dst, const LLVector3& v)
    {
        dst[0] = v.mV[VX];
        dst[1] = v.mV[VY];
        dst[2] = v.mV[VZ];
    }
}

// static
void QLDLSSGInputs::capture(bool normal)
{
    if (!QLVulkanPresenter::frameGenWanted())
    {
        if (sInputs.mTarget.isComplete())
        {
            sInputs.mTarget.release();
        }
        sInputs.mHavePrev = false;
        return;
    }
    // Any other frame breaks the run: the next main view starts again, with no motion from this one.
    if (!normal || !gViewerWindow || gPipeline.mRT != &gPipeline.mMainRT || !gQLDLSSGInputsProgram.isComplete())
    {
        sInputs.mHavePrev = false;
        return;
    }

    const U32 width  = (U32)gViewerWindow->getWindowWidthRaw();
    const U32 height = (U32)gViewerWindow->getWindowHeightRaw();
    if (width == 0 || height == 0 || !ensure_target(width, height))
    {
        sInputs.mHavePrev = false;
        return;
    }

    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("quasar dlss-g inputs");
    LL_PROFILE_GPU_ZONE("quasar dlss-g inputs");

    const LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLMatrix4a projection = camera->getProjection();
    LLMatrix4a view_projection;
    view_projection.setMul(camera->getModelview(), projection);
    LLMatrix4a inverse_view_projection;
    LLMatrix4a inverse_projection;
    if (!inverse_view_projection.setInverse(view_projection) || !inverse_projection.setInverse(projection))
    {
        sInputs.mHavePrev = false;
        return;
    }

    const U32 frame = LLFrameTimer::getFrameCount();
    const LLVector3 origin = camera->getOrigin();
    const bool have_prev = sInputs.mHavePrev && frame == sInputs.mPrevFrame + 1 &&
                           dist_vec(origin, sInputs.mPrevOrigin) < CUT_DISTANCE;
    const LLMatrix4a& prev_view_projection = have_prev ? sInputs.mPrevViewProjection : view_projection;

    // The depth and the camera's motion, over the whole window.
    {
        gGL.flush();
        LLRenderTarget& target = sInputs.mTarget;
        target.bindTarget();
        LLGLDisable blend(GL_BLEND);
        LLGLDisable scissor(GL_SCISSOR_TEST);
        LLGLDisable cull(GL_CULL_FACE);
        LLGLDepthTest depth_test(GL_FALSE, GL_FALSE);
        gGL.setColorMask(true, true);

        const LLRect world = gViewerWindow->getWorldViewRectRaw();
        LLGLSLShader& shader = gQLDLSSGInputsProgram;
        shader.bind();
        const S32 unit = shader.bindDepthTexture(LLShaderMgr::DEFERRED_DEPTH, &gPipeline.mRT->deferredScreen,
                                                 ALSamplers::PointClamp);
        shader.uniformMatrix4fv(LLShaderMgr::QL_WARP_INV_VIEW_PROJ, inverse_view_projection);
        shader.uniformMatrix4fv(LLShaderMgr::QL_WARP_VIEW_PROJ, prev_view_projection);
        shader.uniform4f(LLShaderMgr::QL_WARP_WORLD_RECT, (F32)world.mLeft, (F32)world.mBottom, (F32)world.getWidth(),
                         (F32)world.getHeight());
        shader.uniform2f(LLShaderMgr::QL_WARP_WINDOW_SIZE, (F32)width, (F32)height);
        shader.uniform1f(LLShaderMgr::QL_WARP_ALPHA, have_prev ? 1.f : 0.f);
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        if (unit >= 0)
        {
            gGL.getTextureSlot(unit)->unbind();
        }
        shader.unbind();
        LLVertexBuffer::unbind();
        target.flush();
    }

    // The camera, as Streamline's constants want it: its matrices row-major for row vectors, which is how LLMatrix4a
    // lays GL's out, products taken in LLMatrix4a's order (setMul(a, b) is b after a).
    QLVulkanPresenter::FrameGenCamera info = {};
    LLMatrix4a clip_to_prev;
    clip_to_prev.setMul(inverse_view_projection, prev_view_projection);
    LLMatrix4a inverse_prev;
    if (!inverse_prev.setInverse(prev_view_projection))
    {
        inverse_prev = inverse_view_projection;
    }
    LLMatrix4a prev_to_clip;
    prev_to_clip.setMul(inverse_prev, view_projection);
    copy_matrix(info.viewToClip, projection);
    copy_matrix(info.clipToView, inverse_projection);
    copy_matrix(info.clipToPrevClip, clip_to_prev);
    copy_matrix(info.prevClipToClip, prev_to_clip);
    copy_vector(info.position, origin);
    copy_vector(info.up, camera->getUpAxis());
    copy_vector(info.right, -camera->getLeftAxis());
    copy_vector(info.forward, camera->getAtAxis());
    const F32 far_plane = camera->getProjectionFar();
    info.nearPlane     = camera->getNear();
    info.farPlane      = std::isfinite(far_plane) ? far_plane : INFINITE_FAR;
    info.fovY          = camera->getView();
    info.aspect        = camera->getAspect();
    info.depthInverted = LLRender::sReverseZ;
    info.reset         = !have_prev;

    QLVulkanPresenter::frameGenInputs(sInputs.mTarget.getTexture(0), sInputs.mTarget.getTexture(1), width, height, info);

    sInputs.mPrevViewProjection = view_projection;
    sInputs.mPrevOrigin         = origin;
    sInputs.mPrevFrame          = frame;
    sInputs.mHavePrev           = true;
}

// static
std::string QLDLSSGInputs::statusLine()
{
    return QLVulkanPresenter::frameGenInfo();
}

#else  // no Vulkan presenter on this platform

// static
void QLDLSSGInputs::capture(bool)
{
}

// static
std::string QLDLSSGInputs::statusLine()
{
    return "not on this platform";
}

#endif
