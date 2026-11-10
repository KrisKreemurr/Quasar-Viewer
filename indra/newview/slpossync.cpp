/**
 * @file slpossync.cpp
 * @brief Avatar position sync between Slipstream viewers over the helper (#1417).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
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

#include "slpossync.h"

#include "llagent.h"
#include "lleventtimer.h"
#include "llframetimer.h"
#include "llmutelist.h"
#include "llrand.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "llworld.h"
#include "rlvactions.h"
#include "slbridge.h"

#include <algorithm>

namespace
{
constexpr char BRIDGE_USER[]        = "possync";
constexpr char LINE_TYPE[]          = "pos/1";      // "#slb:pos/1|<owner>|<fields>" (slpossyncmath.h)
constexpr char SLOT_POS[]           = "pos";        // SLBridge::sendLatest slots
constexpr char SLOT_SEE[]           = "see";
constexpr F32  TICK_PERIOD          = 0.1f;
// Sending: at most four lines a second; one when the drawn position is SEND_MOVE off where the
// last line predicts, the velocity changed by SEND_SPEED_STEP, the turn is SEND_TURN, the state
// (sitting / still / flying) changed, or a keep-alive is due. The helper's own thresholds are a
// little finer, so it never drops what this decided to send.
constexpr F32  SEND_MIN_INTERVAL    = 0.25f;
constexpr F32  KEEP_MOVING          = 1.f;
constexpr F32  KEEP_STILL           = 10.f;     // still or seated
constexpr F32  SEND_MOVE            = 0.05f;    // m
constexpr F32  SEND_SPEED_STEP      = 0.2f;     // m/s
constexpr F32  SEND_TURN            = 5.f * DEG_TO_RAD;
constexpr F32  STILL_SPEED          = 0.05f;    // m/s
// The visible list: avatars within shout range (nothing further is heard) and draw distance,
// nearest to the camera first, at most MAX_SEE (the helper's POS_MAX_SEE; 24 keys fit a line).
constexpr size_t MAX_SEE            = 24;
constexpr F32  SEE_RANGE            = 100.f;
constexpr F32  SEE_CHECK            = 2.f;
constexpr F32  SEE_MIN_INTERVAL     = 3.f;
constexpr F32  SEE_REFRESH          = 60.f;
constexpr F32  STATS_INTERVAL       = 20.f;    // the #PosSync summary line (only while lines arrive)

class SLPosSyncTicker : public LLEventTimer
{
public:
    SLPosSyncTicker(F32 period, std::function<void()> cb) : LLEventTimer(period), mCallback(std::move(cb)) {}
    bool tick() override
    {
        if (mCallback)
            mCallback();
        return false;
    }

private:
    std::function<void()> mCallback;
};

SLPosSyncMath::Vec3 toVec(const LLVector3d& v)
{
    return SLPosSyncMath::Vec3(v.mdV[VX], v.mdV[VY], v.mdV[VZ]);
}

SLPosSyncMath::Quat toQuat(const LLQuaternion& q)
{
    SLPosSyncMath::Quat out;
    out.x = q.mQ[VX];
    out.y = q.mQ[VY];
    out.z = q.mQ[VZ];
    out.w = q.mQ[VW];
    return out;
}

F32 angleBetween(const LLQuaternion& a, const LLQuaternion& b)
{
    F32 dot = llabs(a.mQ[VX] * b.mQ[VX] + a.mQ[VY] * b.mQ[VY] + a.mQ[VZ] * b.mQ[VZ] + a.mQ[VW] * b.mQ[VW]);
    return 2.f * acosf(llmin(dot, 1.f));
}

LLVOAvatar* findOtherAvatar(const LLUUID& id)
{
    LLViewerObject* object = gObjectList.findObject(id);
    if (!object || object->isDead() || !object->isAvatar())
        return nullptr;
    LLVOAvatar* avatar = (LLVOAvatar*)object;
    return (avatar->isSelf() || avatar->isControlAvatar()) ? nullptr : avatar;
}
}

SLPosSync::SLPosSync()
{
}

SLPosSync::~SLPosSync()
{
    mBridgeConnection.disconnect();
    mReadyInRegionConnection.disconnect();
    delete mTicker;
    mTicker = nullptr;
}

void SLPosSync::init()
{
    if (mInitialized)
        return;
    mInitialized = true;
    mSeq = (U16)ll_rand(65536);

    SLBridge& bridge = SLBridge::instance();
    bridge.init();
    bridge.registerHandler(LINE_TYPE, [this](const std::string& rest) { onLine(rest); });
    mBridgeConnection = bridge.setStateCallback([this](SLBridge::EState state)
        {
            if (state == SLBridge::STATE_READY)
            {
                // A fresh (or restarted) script knows nothing: tell it everything again.
                mShareKnown = false;
                mSent = false;
                mSeeForce = true;
            }
        });
    mReadyInRegionConnection = bridge.setReadyInRegionCallback([this]()
        {
            mSent = false;
            mSeeForce = true;
        });
    // Receiving is always on, so the helper is wanted whatever the share option says.
    bridge.addUser(BRIDGE_USER);

    if (!mTicker)
        mTicker = new SLPosSyncTicker(TICK_PERIOD, [this]() { tick(); });
}

// static
bool SLPosSync::isShareOptionOn()
{
    static LLCachedControl<bool> share(gSavedSettings, "SLPosSyncShare", true);
    return share;
}

// static
bool SLPosSync::rlvHidesPosition()
{
    // Any restriction that hides where people are (location, names, nearby lists, maps) also
    // keeps our own precise position to ourselves; so does a block on the channel it uses.
    if (!RlvActions::isRlvEnabled())
        return false;
    return !RlvActions::canShowLocation() || !RlvActions::canShowNearbyAgents()
           || RlvActions::hasBehaviour(RLV_BHVR_SHOWNAMES) || RlvActions::hasBehaviour(RLV_BHVR_SHOWNAMETAGS)
           || RlvActions::hasBehaviour(RLV_BHVR_SHOWMINIMAP) || RlvActions::hasBehaviour(RLV_BHVR_SHOWWORLDMAP)
           || !RlvActions::canSendChannel(SLBridge::POS_CHANNEL);
}

void SLPosSync::tick()
{
    if (!isAgentAvatarValid() || !gAgent.getRegion())
        return;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    mTable.expire(now, [this](const LLUUID& id) { dropAvatar(id); });
    tickSee();
    tickSend();

    if (mStatsTimer.getElapsedTimeF32() > STATS_INTERVAL)
    {
        mStatsTimer.reset();
        // gap: sim-driven placement vs where each sender draws itself (the desync without
        // this feature); residual: what is left on screen after the blend.
        double gap_sum = 0.0, gap_max = 0.0, resid_sum = 0.0, resid_max = 0.0;
        unsigned frames = 0;
        mTable.forEach([&](const LLUUID& id)
            {
                if (LLVOAvatar* avatar = findOtherAvatar(id))
                    avatar->mPosSyncView.takeStats(gap_sum, gap_max, resid_sum, resid_max, frames);
            });
        if (mAccepted || mRejected)
        {
            LL_INFOS("PosSync") << "Followed " << mTable.size() << " avatar(s); lines used " << mAccepted
                                << ", rejected " << mRejected << " in the last " << (S32)STATS_INTERVAL << " s; gap mean "
                                << llformat("%.3f", frames ? gap_sum / frames : 0.0) << " max " << llformat("%.3f", gap_max)
                                << " m, residual mean " << llformat("%.3f", frames ? resid_sum / frames : 0.0) << " max "
                                << llformat("%.3f", resid_max) << " m over " << frames << " frames" << LL_ENDL;
        }
        mAccepted = 0;
        mRejected = 0;
    }
}

void SLPosSync::sendShareState(bool share)
{
    SLBridge& bridge = SLBridge::instance();
    if (!bridge.isReady() || (mShareKnown && share == mShareSent))
        return;
    bridge.sendControl(std::string("pos/share/1|") + (share ? "1" : "0"));
    mShareKnown = true;
    mShareSent = share;
    if (!share)
        mSent = false;
}

void SLPosSync::tickSend()
{
    SLBridge& bridge = SLBridge::instance();
    const bool share = SLPosSyncMath::shouldShare(isShareOptionOn(), rlvHidesPosition(), bridge.isReady());
    sendShareState(share);
    if (!share || gAgent.getTeleportState() != LLAgent::TELEPORT_NONE)
        return;
    LLVOAvatarSelf* self = gAgentAvatarp.get();
    LLViewerRegion* region = self ? self->getRegion() : nullptr;
    if (!self || !region || !self->getRootJoint())
        return;

    // What our own screen shows: the drawn position (the one other viewers compare with their
    // own sim-driven one), the velocity they extrapolate with, the root's rotation.
    const LLVector3 pos = region->getPosRegionFromAgent(self->getRenderPosition());
    const LLQuaternion rot = self->getRootJoint()->getWorldRotation();
    LLVector3 vel = self->getVelocity();
    U8 flags = 0;
    if (self->isSitting())
        flags |= SLPosSyncMath::FLAG_SIT;
    if (gAgent.getFlying())
        flags |= SLPosSyncMath::FLAG_FLY;
    if (vel.length() < STILL_SPEED)
    {
        flags |= SLPosSyncMath::FLAG_STILL;
        vel.clear();
    }

    const F32 since = mSentTimer.getElapsedTimeF32();
    const bool new_region = region->getHandle() != mSentRegion;
    if (mSent && !new_region && since < SEND_MIN_INTERVAL)
        return;
    bool due = !mSent || new_region || flags != mSentFlags;
    if (!due)
    {
        const bool still = flags & (SLPosSyncMath::FLAG_STILL | SLPosSyncMath::FLAG_SIT);
        due = since >= (still ? KEEP_STILL : KEEP_MOVING);
        // Seated: only the state goes out (receivers keep the seat's placement).
        if (!(flags & SLPosSyncMath::FLAG_SIT))
        {
            due = due || dist_vec(pos, mSentPos + mSentVel * since) > SEND_MOVE
                  || dist_vec(vel, mSentVel) > SEND_SPEED_STEP || angleBetween(rot, mSentRot) > SEND_TURN;
        }
    }
    if (!due)
        return;

    std::string flag_text;
    if (flags & SLPosSyncMath::FLAG_SIT)
        flag_text += 'S';
    if (flags & SLPosSyncMath::FLAG_STILL)
        flag_text += 'T';
    if (flags & SLPosSyncMath::FLAG_FLY)
        flag_text += 'F';
    const U16 seq = (U16)(mSeq + 1);
    // Numbers through formatFixed, never printf: a user locale with a decimal comma would
    // break the helper's vector casts.
    const double p[3] = { pos.mV[VX], pos.mV[VY], pos.mV[VZ] };
    const double v[3] = { vel.mV[VX], vel.mV[VY], vel.mV[VZ] };
    const double q[4] = { rot.mQ[VX], rot.mQ[VY], rot.mQ[VZ], rot.mQ[VW] };
    const std::string line = "pos/send/1|" + std::to_string((U32)seq) + "|" + SLPosSyncMath::formatTuple(p, 3, 3) + "|"
                             + SLPosSyncMath::formatTuple(v, 3, 2) + "|" + SLPosSyncMath::formatTuple(q, 4, 4) + "|" + flag_text;
    if (!bridge.sendLatest(SLOT_POS, line))
        return;
    mSeq = seq;
    mSent = true;
    mSentTimer.reset();
    mSentPos = pos;
    mSentVel = vel;
    mSentRot = rot;
    mSentFlags = flags;
    mSentRegion = region->getHandle();
}

void SLPosSync::tickSee()
{
    SLBridge& bridge = SLBridge::instance();
    if (!bridge.isReady())
        return;
    if (!mSeeForce && mSeeCheck.getElapsedTimeF32() < SEE_CHECK)
        return;
    mSeeCheck.reset();

    // Who we can plausibly see: within shout range of us (where their helpers can be heard)
    // and draw distance of the camera, nearest to the camera first.
    static LLCachedControl<F32> far_clip(gSavedSettings, "RenderFarClip", 128.f);
    uuid_vec_t ids;
    std::vector<LLVector3d> positions;
    LLWorld::getInstance()->getAvatars(&ids, &positions, gAgent.getPositionGlobal(), SEE_RANGE);
    const LLVector3d camera = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
    const F64 range = llmax((F64)(F32)far_clip, 1.0);
    std::vector<std::pair<F64, LLUUID>> nearest;
    for (size_t i = 0; i < ids.size() && i < positions.size(); ++i)
    {
        if (ids[i] == gAgentID || LLMuteList::getInstance()->isMuted(ids[i]))
            continue;
        const F64 d = (positions[i] - camera).magVec();
        if (d <= range)
            nearest.emplace_back(d, ids[i]);
    }
    std::sort(nearest.begin(), nearest.end());
    if (nearest.size() > MAX_SEE)
        nearest.resize(MAX_SEE);

    std::vector<LLUUID> members;
    for (const auto& entry : nearest)
        members.push_back(entry.second);
    std::sort(members.begin(), members.end());
    const bool changed = members != mSeeSent;
    if (!mSeeForce && !(changed && mSeeTimer.getElapsedTimeF32() >= SEE_MIN_INTERVAL)
        && mSeeTimer.getElapsedTimeF32() < SEE_REFRESH)
    {
        return;
    }

    std::string line = "pos/see/1|";
    for (size_t i = 0; i < nearest.size(); ++i)
        line += (i ? "," : "") + nearest[i].second.asString();
    if (!bridge.sendLatest(SLOT_SEE, line))
        return;
    mSeeSent = members;
    mSeeTimer.reset();
    mSeeForce = false;
}

void SLPosSync::onLine(const std::string& rest)
{
    // rest = <sim-verified owner of the speaking attachment>|<seq>|<pos>|<vel>|<rot>|<flags>.
    // The owner is the only identity; the fields after it never name anyone (parseFields
    // takes exactly five).
    const std::string::size_type bar = rest.find('|');
    if (bar == std::string::npos)
        return;
    LLUUID sender;
    if (!LLUUID::parseUUID(rest.substr(0, bar), &sender) || sender.isNull() || sender == gAgentID)
        return;
    if (LLMuteList::getInstance()->isMuted(sender))
        return;
    SLPosSyncMath::Line line;
    if (!SLPosSyncMath::parseFields(rest.substr(bar + 1), line))
    {
        ++mRejected;
        return;
    }
    LLVOAvatar* avatar = findOtherAvatar(sender);
    LLViewerRegion* region = avatar ? avatar->getRegion() : nullptr;
    if (!region)
        return;

    // Region-local in the region the sim has the avatar in (a line from a helper that has
    // just crossed lands far off and fails the bound below).
    const SLPosSyncMath::Vec3 shared = toVec(region->getPosGlobalFromRegion(
        LLVector3((F32)line.pos.x, (F32)line.pos.y, (F32)line.pos.z)));
    const SLPosSyncMath::Vec3 sim = toVec(gAgent.getPosGlobalFromAgent(avatar->getRenderPosition()));
    const F64 gap = (shared - sim).length();
    if (!SLPosSyncMath::withinBound(shared, sim))
    {
        // Over 2 m from where the sim puts it: this update is rejected, sim placement stays.
        ++mRejected;
        avatar->mPosSyncView.clearTarget();
        LL_DEBUGS("PosSync") << sender << " seq " << line.seq << " rejected: " << llformat("%.3f", gap) << " m from the sim" << LL_ENDL;
        return;
    }
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (!mTable.accept(sender, line.seq, line.flags, now))
        return;
    LL_DEBUGS("PosSync") << sender << " seq " << line.seq << " flags " << (U32)line.flags << " gap "
                         << llformat("%.3f", gap) << " m (on arrival, before extrapolation)" << LL_ENDL;
    avatar->mPosSyncView.setTarget(line, shared, now);
    ++mAccepted;
}

void SLPosSync::dropAvatar(const LLUUID& id)
{
    // Eased back by the avatar's own view (no snap).
    if (LLVOAvatar* avatar = findOtherAvatar(id))
        avatar->mPosSyncView.clearTarget();
}

// static
LLVector3 SLPosSync::rootOffset(LLVOAvatar& avatar, F32 dt, LLQuaternion* rotation, bool* rot_shown)
{
    SLPosSyncMath::View& view = avatar.mPosSyncView;
    *rot_shown = false;
    if (!view.busy() || avatar.isSelf())
        return LLVector3::zero;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const SLPosSyncMath::Vec3 sim = toVec(gAgent.getPosGlobalFromAgent(avatar.getRenderPosition()));
    const LLVector3 vel = avatar.getVelocity();
    const SLPosSyncMath::Vec3 offset = view.step(sim, toQuat(avatar.getRotation()),
                                                 SLPosSyncMath::Vec3(vel.mV[VX], vel.mV[VY], vel.mV[VZ]), now, dt);
    if (view.rotShown)
    {
        rotation->set((F32)view.shownRot.x, (F32)view.shownRot.y, (F32)view.shownRot.z, (F32)view.shownRot.w);
        *rot_shown = true;
    }
    return LLVector3((F32)offset.x, (F32)offset.y, (F32)offset.z);
}
