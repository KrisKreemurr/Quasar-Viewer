/**
 * @file slanimationlog.h
 * @brief Recent animations played on our avatar and what started them, feeding the
 *        Animation Explorer (#1099)
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

#ifndef SL_ANIMATIONLOG_H
#define SL_ANIMATIONLOG_H

#include "llsingleton.h"
#include "lluuid.h"

#include <list>
#include <map>
#include <set>
#include <string>
#include <boost/signals2.hpp>

struct SLAnimationLogEntry
{
    LLUUID  mAnimID;
    LLUUID  mSourceID;      // the object (or avatar: our own gestures, AO) that started it
    F64     mStarted = 0.0; // unix seconds
    F64     mStopped = 0.0; // unix seconds, 0 while playing
};

// The animations objects and avatars started on our avatar (process_avatar_animation), most
// recent first. Bounded: our own animations (gestures, AO) are dropped first, so an AO does
// not push out the furniture that animated us. A restart of the same animation by the same
// source moves its entry to the front.
class SLAnimationLog : public LLSingleton<SLAnimationLog>
{
    LLSINGLETON_EMPTY_CTOR(SLAnimationLog);

public:
    using entry_list_t = std::list<SLAnimationLogEntry>;
    using source_map_t = std::multimap<LLUUID, LLUUID>; // LLVOAvatar::AnimationSourceMap
    using change_signal_t = boost::signals2::signal<void()>;

    // A source started an animation on our avatar
    void recordStarted(const LLUUID& anim_id, const LLUUID& source_id);
    // Our avatar's animation sources after a change: the entries no longer among them stopped
    void updatePlaying(const source_map_t& sources);

    const entry_list_t& getEntries() const { return mEntries; }
    void clear();

    // An object's name and owner, fetched with RequestObjectPropertiesFamily the first time
    // (the name is empty until the reply; the change signal fires then). A request with no
    // reply is sent again after a few seconds.
    std::string getObjectName(const LLUUID& object_id);
    LLUUID getObjectOwner(const LLUUID& object_id) const;
    // LLSelectMgr::processObjectPropertiesFamily: true when the reply was ours
    static bool processPropertiesFamily(const LLUUID& object_id, const LLUUID& owner_id, const std::string& name);

    boost::signals2::connection setChangeCallback(const change_signal_t::slot_type& cb) { return mChangedSignal.connect(cb); }

private:
    struct ObjectInfo
    {
        std::string mName;
        LLUUID mOwnerID;
    };

    entry_list_t mEntries;
    std::map<LLUUID, ObjectInfo> mObjects;
    std::map<LLUUID, F64> mRequested; // asked for, and when: asked again once REQUEST_RETRY passes
    change_signal_t mChangedSignal;
};

#endif // SL_ANIMATIONLOG_H
