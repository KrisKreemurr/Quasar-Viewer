/**
 * @file slpossync.h
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

#ifndef SL_POS_SYNC_H
#define SL_POS_SYNC_H

#include <string>
#include <vector>

#include <boost/signals2.hpp>

#include "llsingleton.h"
#include "lltimer.h"
#include "lluuid.h"
#include "v3math.h"
#include "llquaternion.h"
#include "slpossyncmath.h"

class LLEventTimer;
class LLVOAvatar;

/**
 * Each Slipstream user shares their own avatar's drawn position; other Slipstream viewers
 * draw that avatar there instead of where their own reading of the sim's updates puts it.
 * See doc/SLBRIDGE_POSITION_SYNC.md.
 *
 * Sending (option SLPosSyncShare, default on): the drawn position, velocity and root rotation
 * of our own avatar go to the helper ("pos/send/1|...") when they change beyond a threshold,
 * at most POS_SEND_RATE a second, with a keep-alive. Never while the option is off, while
 * RLV hides where we are, or without a ready bridge; the helper is told ("pos/share/1|0")
 * and then says nothing for us either.
 *
 * Receiving (always on): the helper forwards lines only for the avatars we list as
 * plausibly visible ("pos/see/1|<keys>"), identified by the sim. A line is used only if it
 * is newer than the last one, the table has room (SLPosSyncMath::MAX_ENTRIES), and it lies
 * within 2 m of the sim-driven position; the avatar's root is then eased towards it
 * (SLPosSyncMath::View, a member of LLVOAvatar) and eased back when updates stop or the
 * bound breaks. Visual only: the drawable, physics, sits, picking and the camera keep the
 * sim's position.
 */
class SLPosSync : public LLSingleton<SLPosSync>
{
    LLSINGLETON(SLPosSync);
    ~SLPosSync() override;
    LOG_CLASS(SLPosSync);

public:
    void init();

    // The option and the gates on it (RLV, bridge).
    static bool isShareOptionOn();
    static bool rlvHidesPosition();

    // From LLVOAvatar::updateRootPositionAndRotation for another avatar (not seated) whose
    // mPosSyncView is busy(): the offset to add to the sim-driven root this frame. While a
    // shared rotation is shown, *rotation is set to it and *rot_shown to true.
    static LLVector3 rootOffset(LLVOAvatar& avatar, F32 dt, LLQuaternion* rotation, bool* rot_shown);

    // Status for the log / a debug view.
    size_t getFollowed() const { return mTable.size(); }

private:
    void tick();
    void tickSend();
    void tickSee();
    void onLine(const std::string& rest);
    void sendShareState(bool share);   // tells the helper when it changes (or it never heard)
    void dropAvatar(const LLUUID& id);

    bool mInitialized = false;
    LLEventTimer* mTicker = nullptr;
    boost::signals2::connection mBridgeConnection;
    boost::signals2::connection mReadyInRegionConnection;

    // sending
    bool mShareSent = false;        // the helper was last told: share
    bool mShareKnown = false;       // ... told at all since the bridge became ready
    U16 mSeq = 0;
    bool mSent = false;             // a line went out since the bridge became ready
    LLTimer mSentTimer;
    LLVector3 mSentPos;             // region-local
    LLVector3 mSentVel;
    LLQuaternion mSentRot;
    U8 mSentFlags = 0;
    U64 mSentRegion = 0;

    // receiving
    SLPosSyncMath::Table<LLUUID> mTable;
    std::vector<LLUUID> mSeeSent;   // the list the helper has
    LLTimer mSeeTimer;              // since it was sent
    LLTimer mSeeCheck;
    bool mSeeForce = true;
    LLTimer mStatsTimer;
    U32 mAccepted = 0;
    U32 mRejected = 0;
};

#endif // SL_POS_SYNC_H
