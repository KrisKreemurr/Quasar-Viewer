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

#include "llcharacter.h"
#include "lldir.h"
#include "lldrawable.h"
#include "llfile.h"
#include "llglstates.h"
#include "llimage.h"
#include "llimagepng.h"
#include "llrender.h"
#include "llstartup.h"
#include "lltimer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerdisplay.h"
#include "llviewerobjectlist.h"
#include "llviewershadermgr.h"
#include "llvoavatar.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "pipeline.h"
#include "qlglread.h"

#include <algorithm>

namespace
{
    // A camera that moved further than this between two frames jumped (a teleport, a region crossing's shift of the
    // agent's coordinates, a camera reset): nothing between the two frames would be right.
    constexpr F32 MAX_EYE_JUMP = 8.f;

    // A frame sent at the first point is sent only once this much of the real frames' interval has passed, so that it
    // lands between the two real frames rather than just after the first.
    constexpr F64 EARLIEST_FRACTION = 0.42;

    // Avatars further than this move too little on screen in a frame's interval to be worth a box.
    constexpr F32 MAX_AVATAR_DISTANCE = 96.f;

    // A box this much larger than the avatar's on every side takes in the edge of what it wears, and little floor.
    constexpr F32 AVATAR_BOX_PAD = 0.05f;

    // An avatar that moved further than this between two frames jumped (a teleport, a sit): it is left where it was.
    constexpr F32 MAX_AVATAR_JUMP = 4.f;

    // QuasarFrameWarpDump saves this many sets, this far apart, then turns itself off.
    constexpr U32 DUMP_SETS = 8;
    constexpr F64 DUMP_SPACING = 0.3;

    // The shader's switches, above the box count in qlWarpAvatarCount.
    constexpr S32 WARP_FILL  = 0x100;
    constexpr S32 WARP_DEBUG = 0x200;
    constexpr S32 WARP_BOTH  = 0x400;

    bool frame_warp_wanted()
    {
        static LLCachedControl<bool> frame_warp(gSavedSettings, "QuasarFrameWarp", false);
        return frame_warp();
    }

    // Whether the window can be drawn to between real frames at all.
    bool window_drawable()
    {
        return LLStartUp::getStartupState() == STATE_STARTED && gViewerWindow && gViewerWindow->getWindow() &&
               !gViewerWindow->getWindow()->getMinimized() && !gTeleportDisplay;
    }

    // The camera's view and projection at a point between two cameras.
    LLMatrix4a between_cameras(const LLVector3& from_origin, const LLQuaternion& from_rotation, const LLVector3& to_origin,
                               const LLQuaternion& to_rotation, const LLMatrix4a& projection, F32 alpha)
    {
        LLCamera mid = *LLViewerCamera::getInstance();
        mid.setOrigin(from_origin + (to_origin - from_origin) * alpha);
        mid.setAxes(slerp(alpha, from_rotation, to_rotation));
        LLMatrix4a view_projection;
        view_projection.setMul(mid.frameModelview(), projection);
        return view_projection;
    }
}

QLFrameWarp& QLFrameWarp::instance()
{
    static QLFrameWarp warp;
    return warp;
}

bool QLFrameWarp::twoFrames() const
{
    static LLCachedControl<S32> mode(gSavedSettings, "QuasarFrameWarpMode", 1);
    return mode() >= 1;
}

bool QLFrameWarp::ensureTargets(Kept& kept, U32 width, U32 height, U32 depth_width, U32 depth_height)
{
    for (LLRenderTarget* target : { &kept.mWorld, &kept.mFrame })
    {
        if (target->getWidth() != width || target->getHeight() != height || !target->isComplete())
        {
            target->release();
            if (!target->allocate(width, height, GL_RGBA8))
            {
                return false;
            }
        }
    }
    const LLRenderTarget::eDepthFormat depth_format = LLPipeline::mainDepthFormat();
    if (kept.mDepth.getWidth() != depth_width || kept.mDepth.getHeight() != depth_height || !kept.mDepth.isComplete() ||
        kept.mDepth.getDepthFormat() != depth_format)
    {
        kept.mDepth.release();
        if (!kept.mDepth.allocate(depth_width, depth_height, 0, true, false, ALTextureSlot::TT_TEXTURE,
                                  LLRenderTarget::MIPS_NONE, depth_format))
        {
            return false;
        }
    }
    return true;
}

bool QLFrameWarp::fitsWindow(const Kept& kept) const
{
    return gViewerWindow && (U32)gViewerWindow->getWindowWidthRaw() == kept.mFrame.getWidth() &&
           (U32)gViewerWindow->getWindowHeightRaw() == kept.mFrame.getHeight();
}

void QLFrameWarp::release()
{
    for (Kept& kept : mKept)
    {
        kept.mWorld.release();
        kept.mFrame.release();
        kept.mDepth.release();
        kept.mAvatars.clear();
        kept.mComplete = false;
    }
    mNewest = -1;
    mFilling = -1;
    mHeld = -1;
    mDumpGen.release();
    mDumpTint.release();
}

void QLFrameWarp::invalidate()
{
    for (Kept& kept : mKept)
    {
        kept.mComplete = false;
    }
    mNewest = -1;
    mFilling = -1;
}

void QLFrameWarp::captureWorld(bool normal)
{
    mFilling = -1;

    if (!frame_warp_wanted())
    {
        if (mStatus != OFF)
        {
            release();
            mStatus = OFF;
        }
        return;
    }
    if (!normal || !gViewerWindow || gPipeline.mRT != &gPipeline.mMainRT)
    {
        invalidate();
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

    // Into the slot that is neither the newest frame nor one still held back to be shown.
    const S32 slot = (mNewest == 0) ? 1 : 0;
    if (slot == mHeld)
    {
        return;
    }
    Kept& kept = mKept[slot];
    kept.mComplete = false;

    const U32 width = (U32)gViewerWindow->getWindowWidthRaw();
    const U32 height = (U32)gViewerWindow->getWindowHeightRaw();
    LLRenderTarget& depth = gPipeline.mMainRT.deferredScreen;
    if (width == 0 || height == 0 || !ensureTargets(kept, width, height, depth.getWidth(), depth.getHeight()))
    {
        return;
    }
    kept.mWorld.copyFromWindow(0, 0, width, height);
    // A copy of its own: the next frame clears the G-buffer, and from two frames this one is read after it has.
    gPipeline.copySceneDepth(kept.mDepth);

    const LLViewerCamera* camera = LLViewerCamera::getInstance();
    kept.mProjection = camera->getProjection();
    kept.mViewProjection.setMul(camera->getModelview(), kept.mProjection);
    kept.mOrigin = camera->getOrigin();
    kept.mRotation = camera->getQuaternion();
    keepAvatars(kept);

    mFilling = slot;
}

void QLFrameWarp::keepAvatars(Kept& kept)
{
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("quasar warp keep avatars");
    kept.mAvatars.clear();
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(character);
        // A seated avatar's drawable hangs from its seat's, its box in the seat's space: it is left where it was.
        if (!avatar || avatar->isDead() || avatar->mDrawable.isNull() || !avatar->mDrawable->isRoot() ||
            !avatar->mDrawable->isVisible())
        {
            continue;
        }
        const LLVector3 position = avatar->getPositionAgent();
        const F32 distance = (position - kept.mOrigin).length();
        if (distance > MAX_AVATAR_DISTANCE)
        {
            continue;
        }
        const LLVector4a* extents = avatar->mDrawable->getSpatialExtents();
        KeptAvatar one;
        one.mID = character->getID();
        one.mPosition = position;
        one.mMin.set(extents[0].getF32ptr());
        one.mMax.set(extents[1].getF32ptr());
        one.mMin -= LLVector3(AVATAR_BOX_PAD, AVATAR_BOX_PAD, AVATAR_BOX_PAD);
        one.mMax += LLVector3(AVATAR_BOX_PAD, AVATAR_BOX_PAD, AVATAR_BOX_PAD);
        one.mDistance = distance;
        if (!one.mMin.isFinite() || !one.mMax.isFinite())
        {
            continue;
        }
        kept.mAvatars.push_back(one);
    }

    // The nearest first: they move furthest on screen, and the shader takes the first box a surface is in.
    const size_t keep = llmin(kept.mAvatars.size(), (size_t)MAX_AVATARS);
    std::partial_sort(kept.mAvatars.begin(), kept.mAvatars.begin() + keep, kept.mAvatars.end(),
                      [](const KeptAvatar& a, const KeptAvatar& b) { return a.mDistance < b.mDistance; });
    kept.mAvatars.resize(keep);
}

void QLFrameWarp::captureFrame()
{
    if (mFilling < 0)
    {
        return;
    }
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("quasar warp keep frame");
    LL_PROFILE_GPU_ZONE("quasar warp keep frame");
    Kept& kept = mKept[mFilling];
    kept.mFrame.copyFromWindow(0, 0, kept.mFrame.getWidth(), kept.mFrame.getHeight());
    kept.mComplete = true;
    kept.mSerial = ++mSerial;
    mNewest = mFilling;
    mFilling = -1;
}

bool QLFrameWarp::swapInstead()
{
    // An earlier real frame neither point between frames showed (a snapshot came between) is dropped: shown now it
    // would draw over this frame, which the window holds for the swap to come.
    mHeld = -1;

    if (!frame_warp_wanted() || !twoFrames() || mNewest < 0 || !window_drawable())
    {
        return false;
    }
    const Kept& next = mKept[mNewest];
    const Kept& from = mKept[1 - mNewest];
    if (!next.mComplete || !from.mComplete || from.mSerial + 1 != next.mSerial || !fitsWindow(from) ||
        !fitsWindow(next) || (next.mOrigin - from.mOrigin).length() > MAX_EYE_JUMP)
    {
        return false;
    }

    // Above this the screen gains little from twice as many frames, and the second swap a frame costs more than it is
    // worth.
    static LLCachedControl<F32> below_fps(gSavedSettings, "QuasarFrameWarpBelowFPS", 170.f);
    if (below_fps() > 0.f && mInterval < 1.0 / (F64)below_fps())
    {
        mStatus = FAST;
        return false;
    }

    // Halfway between the two, which is where it lands in time: it goes out now, where the newer frame would have,
    // and the newer goes out half an interval later, as the older did before it.
    const F32 alpha = 0.5f;
    const LLMatrix4a mid = between_cameras(from.mOrigin, from.mRotation, next.mOrigin, next.mRotation, next.mProjection, alpha);
    if (!present(from, &next, mid, alpha))
    {
        return false;
    }
    mHeld = mNewest;
    return true;
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
        invalidate();
    }
    mShown = false;
    mNextKept = false;

    if (now - mCountStart >= 1.0)
    {
        mLastGenerated = mGenerated;
        mGenerated = 0;
        mCountStart = now;
    }
}

void QLFrameWarp::between(bool last)
{
    // The first point is straight after the idle step, before anything in the next frame's display can have moved
    // the main camera about; the camera there is the one the next frame is drawn with.
    if (!mNextKept)
    {
        const LLViewerCamera* camera = LLViewerCamera::getInstance();
        mNextOrigin = camera->getOrigin();
        mNextRotation = camera->getQuaternion();
        mNextKept = true;
    }

    const F64 elapsed = (F64)LLTimer::getTotalSeconds() - mLastSwap;
    const bool due = last || elapsed >= mInterval * EARLIEST_FRACTION;

    // From two frames: the real frame held back goes out halfway to the next generated one.
    if (mHeld >= 0)
    {
        if (due)
        {
            showHeld();
        }
        return;
    }
    if (twoFrames())
    {
        return;
    }

    // From one frame: a generated frame between the last real one and the next.
    if (mNewest < 0 || mShown || !due)
    {
        return;
    }
    if (!frame_warp_wanted() || !window_drawable())
    {
        invalidate();
        return;
    }
    static LLCachedControl<F32> below_fps(gSavedSettings, "QuasarFrameWarpBelowFPS", 170.f);
    if (below_fps() > 0.f && mInterval < 1.0 / (F64)below_fps())
    {
        mStatus = FAST;
        return;
    }
    const Kept& kept = mKept[mNewest];
    if (!kept.mComplete || !fitsWindow(kept) || (mNextOrigin - kept.mOrigin).length() > MAX_EYE_JUMP)
    {
        invalidate();
        return;
    }

    // Where between the two real frames this one lands, taking the next to come one interval after the last.
    const F32 alpha = (F32)llclamp(elapsed / llmax(mInterval, 1e-4), 0.1, 0.9);
    const LLMatrix4a mid = between_cameras(kept.mOrigin, kept.mRotation, mNextOrigin, mNextRotation, kept.mProjection, alpha);
    mShown = present(kept, nullptr, mid, alpha);
}

void QLFrameWarp::showHeld()
{
    const S32 held = mHeld;
    mHeld = -1;
    if (held < 0 || !gViewerWindow || !gViewerWindow->getWindow() || !fitsWindow(mKept[held]) ||
        LLRenderTarget::getCurrentBoundTarget() != nullptr)
    {
        return;
    }
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("quasar warp show held frame");
    LL_PROFILE_GPU_ZONE("quasar warp show held frame");
    LLRenderTarget& frame = mKept[held].mFrame;
    {
        LLGLDisable scissor(GL_SCISSOR_TEST);
        LLRenderTarget::copyContentsToFramebuffer(frame, 0, 0, frame.getWidth(), frame.getHeight(), 0, 0, frame.getWidth(),
                                                  frame.getHeight(), GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    gViewerWindow->getWindow()->swapBuffers();
}

bool QLFrameWarp::present(const Kept& from, const Kept* next, const LLMatrix4a& mid_view_projection, F32 alpha)
{
    // A target bound here would be drawn into instead of the window.
    if (LLRenderTarget::getCurrentBoundTarget() != nullptr)
    {
        return false;
    }

    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("quasar frame warp");
    LL_PROFILE_GPU_ZONE("quasar frame warp");
    const F64 started = LLTimer::getTotalSeconds();

    // Each frame's clip space back to the agent's, where the avatars move.
    LLMatrix4a from_inverse;
    LLMatrix4a next_inverse;
    if (!from_inverse.setInverse(from.mViewProjection) || (next && !next_inverse.setInverse(next->mViewProjection)))
    {
        invalidate();
        return false;
    }
    if (!next)
    {
        next_inverse = from_inverse;
    }

    // QuasarFrameWarpAvatars: 0 leaves every avatar where the frames drew them, 1 moves them, 2 also holds the hair
    // of the near ones to them. QuasarFrameWarpFill fills uncovered strips from the background.
    static LLCachedControl<S32> avatar_mode(gSavedSettings, "QuasarFrameWarpAvatars", 2);
    static LLCachedControl<bool> fill(gSavedSettings, "QuasarFrameWarpFill", true);
    static LLCachedControl<F32> hair_distance(gSavedSettings, "QuasarFrameWarpHairDistance", 10.f);
    static LLCachedControl<bool> debug_tint(gSavedSettings, "QuasarFrameWarpDebug", false);

    // Each kept avatar's box as the earlier frame drew it, and how far it moved since: to the later frame from two
    // (which has just been drawn, so where it is now), or to the next frame from one (which the idle step has moved it
    // on to). The shader moves it by its share of that.
    F32 avatar_min[MAX_AVATARS * 4] = {};
    F32 avatar_max[MAX_AVATARS * 4] = {};
    F32 avatar_move[MAX_AVATARS * 4] = {};
    S32 avatars = 0;
    U32 moving = 0;
    if (avatar_mode() > 0)
    {
        for (const KeptAvatar& kept : from.mAvatars)
        {
            LLViewerObject* object = gObjectList.findObject(kept.mID);
            if (!object || object->isDead() || !object->asAvatar())
            {
                continue;
            }
            // An avatar standing still keeps its box too, with no move: its see-through hair is held to it as the
            // camera orbits (the shader's hair rule).
            LLVector3 moved = object->getPositionAgent() - kept.mPosition;
            const F32 length = moved.length();
            if (length > MAX_AVATAR_JUMP || !moved.isFinite())
            {
                moved.clear();
            }
            else if (length >= 1e-4f)
            {
                ++moving;
            }
            for (S32 i = 0; i < 3; ++i)
            {
                avatar_min[avatars * 4 + i] = kept.mMin.mV[i];
                avatar_max[avatars * 4 + i] = kept.mMax.mV[i];
                avatar_move[avatars * 4 + i] = moved.mV[i];
            }
            avatar_move[avatars * 4 + 3] = (avatar_mode() >= 2 && kept.mDistance < hair_distance()) ? 1.f : 0.f;
            ++avatars;
        }
    }
    mLastAvatars = moving;

    const Kept& newest = next ? *next : from;
    const S32 width = (S32)newest.mFrame.getWidth();
    const S32 height = (S32)newest.mFrame.getHeight();
    const LLRect world = gViewerWindow->getWorldViewRectRaw();
    const S32 flags = avatars | (fill() ? WARP_FILL : 0) | (debug_tint() ? WARP_DEBUG : 0) | (next ? WARP_BOTH : 0);

    gGL.flush();
    const S32 saved_viewport[4] = { gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3] };
    gGL.setViewport(0, 0, width, height);
    {
        LLGLDisable blend(GL_BLEND);
        LLGLDisable scissor(GL_SCISSOR_TEST);
        LLGLDisable cull(GL_CULL_FACE);
        LLGLDepthTest depth_test(GL_FALSE, GL_FALSE);
        gGL.setColorMask(true, true);

        auto draw = [&](S32 draw_flags)
        {
            const Kept& later = next ? *next : from;
            LLGLSLShader& shader = gQLFrameWarpProgram;
            shader.bind();
            const S32 units[] = {
                shader.bindTexture(LLShaderMgr::QL_WARP_WORLD_MAP, const_cast<LLRenderTarget*>(&from.mWorld), ALSamplers::BilinearClamp),
                shader.bindDepthTexture(LLShaderMgr::DEFERRED_DEPTH, const_cast<LLRenderTarget*>(&from.mDepth)),
                shader.bindTexture(LLShaderMgr::QL_WARP_NEXT_WORLD_MAP, const_cast<LLRenderTarget*>(&later.mWorld), ALSamplers::BilinearClamp),
                shader.bindDepthTexture(LLShaderMgr::QL_WARP_NEXT_DEPTH_MAP, const_cast<LLRenderTarget*>(&later.mDepth)),
                shader.bindTexture(LLShaderMgr::QL_WARP_FRAME_MAP, const_cast<LLRenderTarget*>(&newest.mFrame), ALSamplers::PointClamp),
            };
            shader.uniformMatrix4fv(LLShaderMgr::QL_WARP_INV_VIEW_PROJ, from_inverse);
            shader.uniformMatrix4fv(LLShaderMgr::QL_WARP_NEXT_INV_VIEW_PROJ, next_inverse);
            shader.uniformMatrix4fv(LLShaderMgr::QL_WARP_VIEW_PROJ, mid_view_projection);
            shader.uniform1f(LLShaderMgr::QL_WARP_ALPHA, alpha);
            shader.uniform1i(LLShaderMgr::QL_WARP_AVATAR_COUNT, draw_flags);
            if (avatars > 0)
            {
                shader.uniform4fv(LLShaderMgr::QL_WARP_AVATAR_MIN, (U32)avatars, avatar_min);
                shader.uniform4fv(LLShaderMgr::QL_WARP_AVATAR_MAX, (U32)avatars, avatar_max);
                shader.uniform4fv(LLShaderMgr::QL_WARP_AVATAR_MOVE, (U32)avatars, avatar_move);
            }
            shader.uniform4f(LLShaderMgr::QL_WARP_WORLD_RECT, (F32)world.mLeft, (F32)world.mBottom, (F32)world.getWidth(),
                             (F32)world.getHeight());
            shader.uniform2f(LLShaderMgr::QL_WARP_WINDOW_SIZE, (F32)width, (F32)height);

            gPipeline.mScreenTriangleVB->setBuffer();
            gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

            for (S32 unit : units)
            {
                if (unit >= 0)
                {
                    gGL.getTextureSlot(unit)->unbind();
                }
            }
            shader.unbind();
            LLVertexBuffer::unbind();
        };

        // QuasarFrameWarpDump: a set of pictures of this generated frame and what it was made of, the tinted frame
        // drawn and kept first, then the frame shown over it.
        const bool dumping = dumpDue();
        if (dumping)
        {
            draw(flags | WARP_DEBUG);
            if (ensureDumpTarget(mDumpTint, width, height))
            {
                mDumpTint.copyFromWindow(0, 0, width, height);
            }
        }
        draw(flags);
        if (dumping)
        {
            if (ensureDumpTarget(mDumpGen, width, height))
            {
                mDumpGen.copyFromWindow(0, 0, width, height);
            }
            writeDump(from, next, alpha, avatars, moving);
        }
    }

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("quasar frame warp swap");
        gViewerWindow->getWindow()->swapBuffers();
    }
    gGL.setViewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);

    ++mGenerated;
    mStatus = ACTIVE;
    mLastWarpMs = (F32)(((F64)LLTimer::getTotalSeconds() - started) * 1000.0);
    return true;
}

bool QLFrameWarp::dumpDue() const
{
    static LLCachedControl<bool> dump(gSavedSettings, "QuasarFrameWarpDump", false);
    return dump() && (F64)LLTimer::getTotalSeconds() >= mNextDump;
}

bool QLFrameWarp::ensureDumpTarget(LLRenderTarget& target, S32 width, S32 height)
{
    if ((S32)target.getWidth() == width && (S32)target.getHeight() == height && target.isComplete())
    {
        return true;
    }
    target.release();
    return target.allocate((U32)width, (U32)height, GL_RGBA8);
}

namespace
{
    // A window-sized copy saved as a PNG in the logs folder, its alpha dropped (the window's is not the picture's).
    void save_target_png(LLRenderTarget& target, const std::string& name)
    {
        std::vector<U8> rgba;
        if (!ql_read_target_rgba(target, rgba))
        {
            return;
        }
        const U32 width = target.getWidth();
        const U32 height = target.getHeight();
        LLPointer<LLImageRaw> raw = new LLImageRaw((U16)width, (U16)height, 3);
        U8* rgb = raw->getData();
        if (!rgb)
        {
            return;
        }
        const size_t pixels = (size_t)width * height;
        for (size_t i = 0; i < pixels; ++i)
        {
            rgb[i * 3 + 0] = rgba[i * 4 + 0];
            rgb[i * 3 + 1] = rgba[i * 4 + 1];
            rgb[i * 3 + 2] = rgba[i * 4 + 2];
        }
        LLPointer<LLImagePNG> png = new LLImagePNG();
        if (png->encode(raw, 0.f))
        {
            png->save(gDirUtilp->getExpandedFilename(LL_PATH_LOGS, name));
        }
    }
}

void QLFrameWarp::writeDump(const Kept& from, const Kept* next, F32 alpha, S32 avatars, U32 moving)
{
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("quasar warp dump");
    const U32 set = mDumpSets + 1;
    const std::string stem = llformat("QuasarWarp_%02u_", set);

    // From the earliest in time to the latest, so that the names sort as the frames went by.
    save_target_png(const_cast<LLRenderTarget&>(from.mWorld), stem + "1_earlier.png");
    save_target_png(mDumpGen, stem + "2_generated.png");
    save_target_png(mDumpTint, stem + "2_generated_tinted.png");
    if (next)
    {
        save_target_png(const_cast<LLRenderTarget&>(next->mWorld), stem + "3_later.png");
    }

    // What the set was made from, a line a set.
    const std::string info_path = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "QuasarWarp_info.txt");
    llofstream info(info_path.c_str(), set == 1 ? std::ios::trunc : std::ios::app);
    if (info.is_open())
    {
        const LLVector3& eye_a = from.mOrigin;
        const LLVector3& eye_b = next ? next->mOrigin : mNextOrigin;
        info << llformat("set %u: %s, alpha %.3f, base frame %.2f ms, camera moved %.3f m, %d avatar boxes (%u moving), "
                         "frames %llu%s\n",
                         set, next ? "from two frames" : "from one frame", alpha, mInterval * 1000.0,
                         (eye_b - eye_a).length(), avatars, moving, (unsigned long long)from.mSerial,
                         next ? llformat(" and %llu", (unsigned long long)next->mSerial).c_str() : "");
    }

    mNextDump = (F64)LLTimer::getTotalSeconds() + DUMP_SPACING;
    mDumpSets = set;
    if (mDumpSets >= DUMP_SETS)
    {
        LL_INFOS("Quasar") << "Frame warp dump: " << mDumpSets << " sets saved to " << gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "")
                           << LL_ENDL;
        mDumpSets = 0;
        gSavedSettings.setBOOL("QuasarFrameWarpDump", false);
    }
}
