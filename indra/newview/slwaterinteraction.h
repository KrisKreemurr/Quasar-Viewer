/**
 * @file slwaterinteraction.h
 * @brief WATER CONTACT DETECTOR (#1019): avatars and moving objects against the region water
 *        height -> ripple impulses (GPU), splash particles (the GPU particle store) and the
 *        splash / wade / fall-in events (sounds, slwaterevents.h). CPU only, once per frame.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
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
 *
 * Second Life water is a flat plane per region (LLViewerRegion::getWaterHeight); the server
 * knows nothing of waves, so contact is judged against that plane, never against the drawn
 * (displaced) surface. Visual only: no movement, physics or server state is touched.
 *
 * AVATARS (within kRange of the camera; live joint transforms, LLJoint::getWorldMatrixLive):
 *   each LEG whose foot is under the surface touches it where the shin (or the thigh, when the
 *   knee is under too) crosses the plane. Moving legs push a ripple every frame (a trail
 *   behind a wader); every stride (~0.6 s walking, ~0.26 s running) posts a WADE event and,
 *   running or in deeper water, throws a spray. Standing legs ring gently every ~1.2-2.4 s.
 *   A swimmer (pelvis at the surface) pushes a body-wide ring.
 *   LANDING / JUMPING IN: a foot crossing the plane downward faster than 2 m/s -> SPLASH, sized
 *   by the speed; leaving it upward fast -> a small SPLASH.
 *   FALLING IN: the pelvis crossing downward faster than 4 m/s into water deeper than the legs
 *   -> FALL_IN: the big crown, a big ring.
 *   Sitting avatars are their seat's business (a boat is an object below).
 * OBJECTS (gObjectList's active -- moving -- roots, non-avatar, non-attachment; at most
 *   kMaxObjectTests per frame): a body crossing the plane downward faster than 1.5 m/s ->
 *   SPLASH scaled by speed and size; a body cutting the surface while moving (a hull, a
 *   wading animesh) -> a wake ripple each frame and, fast (> 6 m/s), bow spray.
 * Per-body state is kept in FIXED tables (kMaxAvatars / kMaxObjects slots, keyed by id, the
 * stalest slot reused): nothing grows with the crowd.
 *
 * OUTPUTS, each frame:
 *   impulses()  <= kMaxImpulses contacts (xy world, radius m, amplitude m) for the ripple field
 *               (LLCleanWaterFxVK filters them to its window);
 *   splashes    queued here; feedSplashEmitters() (LLViewerPartSim::updateSimulation, the GPU
 *               particle path, the frame after) turns them into one-shot emitters of the
 *               particle store under reserved source ids -- lit, sorted and capped by
 *               RenderMaxPartCount like every other particle;
 *   events      SLWaterEvents::post (the sounds' one-shots).
 */

#ifndef SL_WATER_INTERACTION_H
#define SL_WATER_INTERACTION_H

#include "stdtypes.h"
#include "lluuid.h"
#include "v3math.h"

class LLVOAvatar;
class LLViewerObject;

class SLWaterInteraction
{
public:
    static constexpr U32 kMaxImpulses = 64;    // = LLCleanWaterFxVK::kMaxImpulses
    static constexpr U32 kMaxSplashes = 12;    // per frame
    static constexpr U32 kMaxAvatars = 48;
    static constexpr U32 kMaxObjects = 64;
    static constexpr U32 kMaxObjectTests = 512;
    static constexpr F32 kRange = 64.f;        // m from the camera (= SLWaterEvents::kMaxDistance)
    // Splash emitters use the particle store's source ids from here up (LLViewerPartSource ids
    // count up from 1 and never get near).
    static constexpr U32 kSplashSourceBase = 0xFFFFF000u;
    static constexpr U32 kSplashSlots = 24;

    static SLWaterInteraction& instance();

    // Once per live frame (the renderer's ensure step). `tier`: LLCleanWaterFxVK's
    // (0 = off: nothing is detected or queued); camAgent: the main eye.
    void update(const LLVector3& camAgent, U32 tier);
    const float (*impulses() const)[4] { return mImp; }
    U32 impulseCount() const { return mImpN; }
    // The particle feed (GPU particle path, after LLCleanParticleStore::beginFrame).
    void feedSplashEmitters();

private:
    SLWaterInteraction() = default;

    struct Leg
    {
        bool  in = false;          // the foot is under the surface
        F32   footZ = 0.f;         // last frame's
        F32   cx = 0.f, cy = 0.f;  // last contact point
        bool  haveC = false;
        F32   idleT = 0.f;         // standing: time to the next gentle ring
    };
    struct AvState
    {
        LLUUID id;
        F64   seen = -1.0;
        F32   pelvisZ = 0.f;
        bool  have = false;
        F32   strideT = 0.f;
        U32   nextLeg = 0;
        Leg   leg[2];
    };
    struct ObjState
    {
        LLUUID id;
        F64   seen = -1.0;
        F32   zmin = 0.f;
        bool  have = false;
        F32   sprayT = 0.f;
    };
    enum ESplash : U32 { SPLASH_CROWN = 0, SPLASH_SPRAY = 1, SPLASH_FALL = 2 };
    struct Splash
    {
        F32 pos[3] = {};
        F32 size = 0.f;            // m (the body's radius)
        F32 intensity = 0.f;       // 0..1
        F32 dir[2] = {};           // horizontal motion (spray leans with it)
        ESplash kind = SPLASH_CROWN;
    };

    void updateAvatar(LLVOAvatar* av, AvState& st, F32 wh, F32 dt, U32 stamp);
    void updateObject(LLViewerObject* obj, ObjState& st, F32 wh, F32 dt);
    AvState* avSlot(const LLUUID& id, F64 now);
    ObjState* objSlot(const LLUUID& id, F64 now);
    void impulse(F32 x, F32 y, F32 radius, F32 amp);
    void splash(ESplash kind, const LLVector3& p, F32 size, F32 intensity, F32 dx, F32 dy);

    AvState  mAv[kMaxAvatars];
    ObjState mObj[kMaxObjects];
    float    mImp[kMaxImpulses][4] = {};
    U32      mImpN = 0;
    Splash   mSplash[kMaxSplashes];
    U32      mSplashN = 0;
    U32      mSplashSlot = 0;      // round-robin over the reserved emitter ids
    U32      mTier = 0;
    U32      mRng = 0x2545F491u;
    LLVector3 mCam;
};

#endif
