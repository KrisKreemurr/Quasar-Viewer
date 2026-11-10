/**
 * @file slwaterinteraction.cpp
 * @brief WATER CONTACT DETECTOR (#1019): see slwaterinteraction.h.
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
 */

#include "llviewerprecompiledheaders.h"

#include "slwaterinteraction.h"

#include "llcharacter.h"
#include "llcleanparticlestore.h"
#include "llframetimer.h"
#include "lljoint.h"
#include "llpartdata.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llvoavatar.h"
#include "llvoavatarself.h"
#include "llworldclock.h"
#include "slwaterevents.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr F32 kGravity = 9.81f;
constexpr F32 kStaleSecs = 2.f;     // a body not seen this long frees its slot

// Ripple amplitudes (m) -- the field's dent per contact. Continuous pushes are per 1/60 s.
constexpr F32 kWadeAmp = 0.010f;    // a leg at walking speed
constexpr F32 kIdleAmp = 0.004f;    // a standing leg's gentle ring
constexpr F32 kSplashAmp = 0.05f;   // a landing at ~5 m/s
constexpr F32 kFallAmp = 0.14f;     // a fall-in

F32 clamp01(F32 x) { return std::clamp(x, 0.f, 1.f); }

LLVector3 liveJointPos(LLJoint* j, U32 stamp)
{
    return j ? j->getWorldMatrixLive(stamp).getTranslation() : LLVector3::zero;
}

// Where a leg (hip -> knee -> foot) crosses the plane z = wh (the foot is under it).
bool legContact(const LLVector3& hip, const LLVector3& knee, const LLVector3& foot, F32 wh, F32& x, F32& y)
{
    const LLVector3* a = &knee;
    const LLVector3* b = &foot;
    if (knee.mV[VZ] < wh) { a = &hip; b = &knee; }   // the knee is under too: the thigh crosses
    if (a->mV[VZ] < wh)
    {
        // the whole leg is under (a swimmer): the hip's own position
        x = hip.mV[VX]; y = hip.mV[VY];
        return false;
    }
    const F32 dz = a->mV[VZ] - b->mV[VZ];
    const F32 t = dz > 1e-4f ? (a->mV[VZ] - wh) / dz : 1.f;
    x = a->mV[VX] + (b->mV[VX] - a->mV[VX]) * t;
    y = a->mV[VY] + (b->mV[VY] - a->mV[VY]) * t;
    return true;
}
} // namespace

SLWaterInteraction& SLWaterInteraction::instance()
{
    static SLWaterInteraction s;
    return s;
}

void SLWaterInteraction::impulse(F32 x, F32 y, F32 radius, F32 amp)
{
    if (mTier < 2 || mImpN >= kMaxImpulses || amp == 0.f) return;   // (no ripple field below MEDIUM)
    float* q = mImp[mImpN++];
    q[0] = x; q[1] = y; q[2] = radius; q[3] = std::clamp(amp, -0.3f, 0.3f);
}

void SLWaterInteraction::splash(ESplash kind, const LLVector3& p, F32 size, F32 intensity, F32 dx, F32 dy)
{
    if (mSplashN >= kMaxSplashes || intensity <= 0.02f) return;
    Splash& s = mSplash[mSplashN++];
    s.pos[0] = p.mV[VX]; s.pos[1] = p.mV[VY]; s.pos[2] = p.mV[VZ];
    s.size = size;
    s.intensity = clamp01(intensity);
    s.dir[0] = dx; s.dir[1] = dy;
    s.kind = kind;
}

SLWaterInteraction::AvState* SLWaterInteraction::avSlot(const LLUUID& id, F64 now)
{
    AvState* stale = &mAv[0];
    for (AvState& s : mAv)
    {
        if (s.seen >= 0.0 && s.id == id) return &s;
        if (s.seen < stale->seen) stale = &s;
    }
    // A new body takes the stalest slot (a crowd past kMaxAvatars drops the farthest-gone).
    *stale = AvState{};
    stale->id = id;
    stale->seen = now;
    return stale;
}

SLWaterInteraction::ObjState* SLWaterInteraction::objSlot(const LLUUID& id, F64 now)
{
    ObjState* stale = &mObj[0];
    for (ObjState& s : mObj)
    {
        if (s.seen >= 0.0 && s.id == id) return &s;
        if (s.seen < stale->seen) stale = &s;
    }
    *stale = ObjState{};
    stale->id = id;
    stale->seen = now;
    return stale;
}

void SLWaterInteraction::update(const LLVector3& camAgent, U32 tier)
{
    mImpN = 0;
    // (the particle feed ran since the last update -- idle before display -- or the particles
    // are off: either way the old queue is done)
    mSplashN = 0;
    mTier = tier;
    mCam = camAgent;
    if (tier == 0 || LLWorldClock::isFrozen()) return;
    const F32 dt = std::clamp(LLFrameTimer::getFrameDeltaTimeF32(), 0.f, 0.1f);
    if (dt <= 0.f) return;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const F32 r2 = kRange * kRange;

    // ---- avatars ----
    const U32 stamp = LLJoint::nextLiveStamp();
    for (LLCharacter* ch : LLCharacter::sInstances)
    {
        LLVOAvatar* av = dynamic_cast<LLVOAvatar*>(ch);
        if (!av || av->isDead() || av->isUIAvatar() || av->mDrawable.isNull() || !av->getRegion()) continue;
        const LLVector3 pos = av->getPositionAgent();
        const F32 dx = pos.mV[VX] - camAgent.mV[VX], dy = pos.mV[VY] - camAgent.mV[VY];
        if (dx * dx + dy * dy > r2) continue;
        const F32 wh = av->getRegion()->getWaterHeight();
        // Far above the water (and not falling toward it fast): nothing to track.
        if (pos.mV[VZ] - wh > 12.f) continue;
        AvState* st = avSlot(av->getID(), now);
        st->seen = now;
        updateAvatar(av, *st, wh, dt, stamp);
    }

    // ---- moving objects ----
    U32 tests = 0;
    for (const LLPointer<LLViewerObject>& op : gObjectList.getActiveObjects())
    {
        if (tests >= kMaxObjectTests) break;
        LLViewerObject* obj = op.get();
        if (!obj || obj->isDead() || obj->isAvatar() || obj->isAttachment() || !obj->isRoot() || !obj->getRegion()) continue;
        ++tests;
        const LLVector3 pos = obj->getPositionAgent();
        const F32 dx = pos.mV[VX] - camAgent.mV[VX], dy = pos.mV[VY] - camAgent.mV[VY];
        if (dx * dx + dy * dy > r2) continue;
        const F32 wh = obj->getRegion()->getWaterHeight();
        const F32 rad = 0.5f * obj->getScale().magVec();
        if (pos.mV[VZ] - rad - wh > 12.f || pos.mV[VZ] + rad < wh - 4.f) continue;
        ObjState* st = objSlot(obj->getID(), now);
        st->seen = now;
        updateObject(obj, *st, wh, dt);
    }

    // (stale slots age out on their own: avSlot / objSlot reuse the stalest)
    for (AvState& s : mAv) if (s.seen >= 0.0 && now - s.seen > kStaleSecs) s = AvState{};
    for (ObjState& s : mObj) if (s.seen >= 0.0 && now - s.seen > kStaleSecs) s = ObjState{};
}

void SLWaterInteraction::updateAvatar(LLVOAvatar* av, AvState& st, F32 wh, F32 dt, U32 stamp)
{
    if (!av->mPelvisp || !av->mFootLeftp || !av->mFootRightp || !av->mKneeLeftp || !av->mKneeRightp
        || !av->mHipLeftp || !av->mHipRightp)
        return;
    const bool self = av->isSelf();
    const LLVector3 vel = av->getVelocity();
    const F32 vz = vel.mV[VZ];
    const F32 hspeed = std::sqrt(vel.mV[VX] * vel.mV[VX] + vel.mV[VY] * vel.mV[VY]);
    const LLVector3 pelvis = liveJointPos(av->mPelvisp, stamp);

    if (av->isSitting())
    {
        st.have = false;   // (the seat -- a boat -- is an object; restart cleanly on stand-up)
        return;
    }
    LLVector3 hip[2]  = {liveJointPos(av->mHipLeftp, stamp),  liveJointPos(av->mHipRightp, stamp)};
    LLVector3 knee[2] = {liveJointPos(av->mKneeLeftp, stamp), liveJointPos(av->mKneeRightp, stamp)};
    LLVector3 foot[2] = {liveJointPos(av->mFootLeftp, stamp), liveJointPos(av->mFootRightp, stamp)};
    // (the foot joint is the ankle: the sole is ~8 cm lower)
    for (LLVector3& f : foot) f.mV[VZ] -= 0.08f;
    const F32 legLen = std::max(hip[0].mV[VZ] - foot[0].mV[VZ], 0.5f);

    // FALL-IN: the pelvis crossing the surface fast, into water deeper than the legs.
    if (st.have && st.pelvisZ > wh + 0.15f && pelvis.mV[VZ] <= wh + 0.15f && vz < -4.f)
    {
        const F32 deep = clamp01((wh - std::min(foot[0].mV[VZ], foot[1].mV[VZ])) / legLen);
        const F32 i = clamp01((-vz - 3.f) / 9.f) * (0.5f + 0.5f * deep);
        if (deep > 0.6f)
        {
            const LLVector3 c(pelvis.mV[VX], pelvis.mV[VY], wh);
            impulse(c.mV[VX], c.mV[VY], 0.6f, -kFallAmp * (0.5f + i));
            splash(SPLASH_FALL, c, 0.45f, std::max(i, 0.45f), vel.mV[VX], vel.mV[VY]);
            SLWaterEvents::post(SLWaterEvents::FALL_IN, std::max(i, 0.45f), c, av->getID(), self);
            for (Leg& l : st.leg) { l.in = true; l.haveC = false; }
            st.pelvisZ = pelvis.mV[VZ];
            st.strideT = 0.4f;
            return;
        }
    }

    // SWIMMER / deep wading: the body at the surface.
    const bool body = std::fabs(pelvis.mV[VZ] - wh) < 0.45f;
    if (body)
    {
        const F32 s = std::min(hspeed, 5.f);
        if (s > 0.2f) impulse(pelvis.mV[VX], pelvis.mV[VY], 0.38f, -kWadeAmp * 1.4f * (s / 1.5f) * (dt * 60.f));
    }

    U32 legsIn = 0;
    F32 depth = 0.f;
    for (U32 k = 0; k < 2; ++k)
    {
        Leg& l = st.leg[k];
        const bool in = foot[k].mV[VZ] < wh;
        F32 cx = foot[k].mV[VX], cy = foot[k].mV[VY];
        if (in) legContact(hip[k], knee[k], foot[k], wh, cx, cy);
        // LANDING / JUMPING IN: the foot crossing the plane downward fast.
        if (st.have && !l.in && in && vz < -2.f)
        {
            const F32 i = clamp01((-vz - 1.5f) / 8.f);
            const LLVector3 c(cx, cy, wh);
            impulse(cx, cy, 0.3f, -kSplashAmp * (0.4f + i));
            splash(SPLASH_CROWN, c, 0.25f, std::max(i, 0.2f), vel.mV[VX], vel.mV[VY]);
            if (k == 0 || !st.leg[0].in)   // (one event for both feet landing together)
                SLWaterEvents::post(SLWaterEvents::splashKind(std::max(i, 0.2f)), std::max(i, 0.2f), c, av->getID(), self);
        }
        // JUMPING OUT fast: a small splash where it left.
        else if (st.have && l.in && !in && vz > 2.5f && k == 0)
        {
            const LLVector3 c(l.cx, l.cy, wh);
            splash(SPLASH_SPRAY, c, 0.2f, 0.25f, vel.mV[VX], vel.mV[VY]);
            SLWaterEvents::post(SLWaterEvents::splashKind(0.25f), 0.25f, c, av->getID(), self);
        }
        if (in)
        {
            ++legsIn;
            depth = std::max(depth, wh - foot[k].mV[VZ]);
            // The leg's own motion through the water (the swing), else the body's.
            F32 lspeed = hspeed;
            if (l.haveC)
            {
                const F32 mx = cx - l.cx, my = cy - l.cy;
                lspeed = std::max(lspeed * 0.5f, std::min(std::sqrt(mx * mx + my * my) / dt, 8.f));
            }
            const F32 radius = 0.12f + 0.12f * clamp01((wh - foot[k].mV[VZ]) / legLen);
            if (lspeed > 0.15f)
            {
                impulse(cx, cy, radius, -kWadeAmp * std::min(lspeed / 1.5f, 3.f) * (dt * 60.f));
                l.idleT = 0.6f;
            }
            else if ((l.idleT -= dt) <= 0.f)
            {
                // Standing: a gentle ring now and then (weight shifts, breathing).
                mRng = mRng * 1664525u + 1013904223u;
                l.idleT = 1.2f + 1.2f * (F32)(mRng >> 8) * (1.f / 16777216.f);
                impulse(cx, cy, radius, -kIdleAmp);
            }
            l.cx = cx; l.cy = cy; l.haveC = true;
        }
        else
        {
            l.haveC = false;
        }
        l.in = in;
        l.footZ = foot[k].mV[VZ];
    }

    // WADING STRIDES: one WADE event per stride (alternating legs); spray when running or deep.
    if (legsIn > 0 && hspeed > 0.35f && !body)
    {
        st.strideT -= dt;
        if (st.strideT <= 0.f)
        {
            st.strideT = std::clamp(0.85f / hspeed, 0.26f, 0.7f);
            U32 k = st.nextLeg;
            if (!st.leg[k].in) k ^= 1u;
            st.nextLeg = k ^ 1u;
            const Leg& l = st.leg[k];
            if (l.in && l.haveC)
            {
                const F32 deepK = clamp01(depth / legLen);
                const F32 i = clamp01(0.12f + 0.4f * std::min(hspeed, 6.f) / 6.f + 0.35f * deepK);
                const LLVector3 c(l.cx, l.cy, wh);
                SLWaterEvents::post(SLWaterEvents::WADE, i, c, av->getID(), self);
                if (hspeed > 2.5f || deepK > 0.35f)
                {
                    const F32 n = 1.f / std::max(hspeed, 1e-3f);
                    splash(SPLASH_SPRAY, c, 0.15f, i * (hspeed > 2.5f ? 1.f : 0.6f), vel.mV[VX] * n, vel.mV[VY] * n);
                }
            }
        }
    }
    else if (legsIn == 0)
    {
        st.strideT = 0.f;
    }
    st.pelvisZ = pelvis.mV[VZ];
    st.have = true;
}

void SLWaterInteraction::updateObject(LLViewerObject* obj, ObjState& st, F32 wh, F32 dt)
{
    const LLVector3 pos = obj->getPositionAgent();
    const LLVector3 vel = obj->getVelocity();
    const LLVector3 scale = obj->getScale();
    const F32 rad = std::clamp(0.5f * scale.magVec(), 0.05f, 8.f);
    // The body's vertical half extent, roughly (rotation ignored: SL hulls are mostly level).
    const F32 hz = std::clamp(0.5f * scale.mV[VZ], 0.05f, rad);
    const F32 zmin = pos.mV[VZ] - hz, zmax = pos.mV[VZ] + hz;
    const F32 vz = vel.mV[VZ];
    const F32 hspeed = std::sqrt(vel.mV[VX] * vel.mV[VX] + vel.mV[VY] * vel.mV[VY]);
    const LLVector3 c(pos.mV[VX], pos.mV[VY], wh);
    const F32 size = std::clamp(rad, 0.1f, 4.f);

    // HITTING the water: the bottom crossing the plane downward fast.
    if (st.have && st.zmin > wh && zmin <= wh && vz < -1.5f)
    {
        const F32 i = clamp01((-vz / 10.f) * std::sqrt(size) * 1.2f);
        impulse(c.mV[VX], c.mV[VY], std::clamp(rad * 0.6f, 0.15f, 1.5f), -std::min(kSplashAmp * 0.6f * -vz * 0.2f * size, 0.18f));
        splash(size > 1.f && i > 0.6f ? SPLASH_FALL : SPLASH_CROWN, c, size * 0.6f, std::max(i, 0.1f), vel.mV[VX], vel.mV[VY]);
        SLWaterEvents::post(SLWaterEvents::splashKind(std::max(i, 0.1f)), std::max(i, 0.1f), c, obj->getID(), false);
    }
    // CUTTING the surface while moving: a wake, and bow spray when fast.
    else if (zmin < wh && zmax > wh && hspeed > 0.4f)
    {
        const F32 s = std::min(hspeed, 15.f);
        impulse(c.mV[VX], c.mV[VY], std::clamp(rad * 0.45f, 0.12f, 1.2f), -kWadeAmp * std::min(s / 1.5f, 4.f) * std::sqrt(size) * (dt * 60.f));
        if (s > 6.f && (st.sprayT -= dt) <= 0.f)
        {
            st.sprayT = 0.15f;
            const F32 n = 1.f / s;
            const LLVector3 bow(c.mV[VX] + vel.mV[VX] * n * rad * 0.8f, c.mV[VY] + vel.mV[VY] * n * rad * 0.8f, wh);
            splash(SPLASH_SPRAY, bow, std::min(size * 0.4f, 1.f), clamp01(s / 20.f), vel.mV[VX] * n, vel.mV[VY] * n);
        }
    }
    st.zmin = zmin;
    st.have = true;
}

void SLWaterInteraction::feedSplashEmitters()
{
    LLCleanParticleStore& ps = LLCleanParticleStore::instance();
    if (!ps.ready() || mSplashN == 0)
    {
        mSplashN = 0;
        return;
    }
    LLViewerFetchedTexture* img = LLViewerFetchedTexture::sDefaultParticleImagep.get();
    const LLUUID tex = img ? img->getID() : LLUUID::null;
    if (img)
    {
        img->setBoostLevel(LLGLTexture::BOOST_HIGH);
        img->addTextureStats(256.f * 256.f);
    }
    // LOW tier: half the droplets.
    const F32 budget = mTier >= 2 ? 1.f : 0.5f;
    for (U32 i = 0; i < mSplashN; ++i)
    {
        const Splash& s = mSplash[i];
        // (a FALL is two emitters: the column and the crown)
        const U32 parts = s.kind == SPLASH_FALL ? 2u : 1u;
        for (U32 part = 0; part < parts; ++part)
        {
            const U32 src = kSplashSourceBase + (mSplashSlot++ % kSplashSlots);
            const U32 slot = ps.getOrCreateEmitter(src);
            if (slot == LLCleanParticleStore::INVALID) continue;
            CleanEmitter e{};
            const F32 I = s.intensity;
            F32 vmin, vmax, outer, inner, age, s0, s1, radius;
            U32 count;
            if (s.kind == SPLASH_FALL && part == 0)
            {
                // the column: fast, narrow, tall
                vmin = 3.f + 2.f * I; vmax = 5.5f + 3.5f * I; inner = 0.f; outer = 0.22f;
                count = (U32)(40.f + 120.f * I); s0 = 0.09f; s1 = 0.16f; radius = 0.15f + 0.2f * s.size;
            }
            else if (s.kind == SPLASH_SPRAY)
            {
                vmin = 0.8f + 0.6f * I; vmax = 1.8f + 2.2f * I; inner = 0.35f; outer = 1.1f;
                count = (U32)(6.f + 30.f * I * std::sqrt(std::max(s.size, 0.1f) / 0.15f)); s0 = 0.035f; s1 = 0.07f;
                radius = 0.05f + 0.3f * s.size;
            }
            else
            {
                // the crown: a ring of droplets thrown up and out
                vmin = 1.2f + 1.2f * I; vmax = 2.6f + 3.f * I; inner = 0.25f; outer = 0.75f;
                count = (U32)(12.f + 90.f * I * std::sqrt(std::max(s.size, 0.1f) / 0.25f)); s0 = 0.05f; s1 = 0.1f;
                radius = 0.08f + 0.5f * s.size;
            }
            count = std::min((U32)((F32)count * budget), 220u);
            age = std::clamp(2.f * vmax / kGravity, 0.4f, 1.4f);
            e.pos[0] = s.pos[0]; e.pos[1] = s.pos[1]; e.pos[2] = s.pos[2] + 0.02f; e.pos[3] = radius;
            // Lean the spray with the motion (a small tilt of the cone toward it).
            const F32 lean = s.kind == SPLASH_SPRAY ? 0.35f : 0.12f;
            {
                // rotation about the axis perpendicular to the motion: +Z leans toward dir
                const F32 ax = -s.dir[1], ay = s.dir[0];
                const F32 al = std::sqrt(ax * ax + ay * ay);
                if (al > 1e-3f)
                {
                    const F32 h = 0.5f * lean;
                    e.rot[0] = ax / al * std::sin(h); e.rot[1] = ay / al * std::sin(h); e.rot[2] = 0.f; e.rot[3] = std::cos(h);
                }
                else { e.rot[0] = e.rot[1] = e.rot[2] = 0.f; e.rot[3] = 1.f; }
            }
            for (U32 k = 0; k < 4; ++k) { e.prev_rot[k] = e.rot[k]; e.src_rot[k] = k == 3 ? 1.f : 0.f; }
            e.accel[0] = 0.f; e.accel[1] = 0.f; e.accel[2] = -kGravity; e.accel[3] = age;
            e.start_color[0] = 0.86f; e.start_color[1] = 0.9f; e.start_color[2] = 0.94f; e.start_color[3] = 0.65f;
            e.end_color[0] = 0.92f; e.end_color[1] = 0.94f; e.end_color[2] = 0.97f; e.end_color[3] = 0.f;
            e.scale[0] = e.scale[1] = s0 * (0.8f + 0.6f * I);
            e.scale[2] = e.scale[3] = s1 * (0.8f + 0.6f * I);
            e.speed_angle[0] = vmin; e.speed_angle[1] = vmax; e.speed_angle[2] = inner; e.speed_angle[3] = outer;
            e.target[0] = e.pos[0]; e.target[1] = e.pos[1]; e.target[2] = e.pos[2]; e.target[3] = 0.f;
            e.misc[0] = LLPartSysData::LL_PART_SRC_PATTERN_ANGLE_CONE;
            e.misc[1] = LLPartData::LL_PART_INTERP_COLOR_MASK | LLPartData::LL_PART_INTERP_SCALE_MASK;
            e.misc[2] = count;
            e.misc[3] = src;
            e.blend[0] = LLCleanParticleStore::BLEND_PAIR_ALPHA;
            e.blend[1] = LLCleanParticleStore::MOTION_NORMAL;
            for (U32 k = 0; k < 3; ++k) { e.prev_pos[k] = e.pos[k]; e.prev_pos3[k] = e.pos[k]; }
            ps.setEmitter(slot, e);
            ps.setEmitterTexture(slot, tex);
        }
    }
    mSplashN = 0;
}
