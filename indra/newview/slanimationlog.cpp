/**
 * @file slanimationlog.cpp
 * @brief Recent animations played on our avatar and what started them, feeding the
 *        Animation Explorer (#1099)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * (LGPL v2.1, see slanimationlog.h)
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "slanimationlog.h"

#include "lldate.h"
#include "message.h"

#include "llagent.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"

namespace
{
    constexpr size_t MAX_ENTRIES = 100;
    constexpr size_t MAX_OBJECTS = 1024;
    constexpr F64 REQUEST_RETRY = 5.0; // seconds without a reply before a name is asked for again
}

void SLAnimationLog::recordStarted(const LLUUID& anim_id, const LLUUID& source_id)
{
    for (auto it = mEntries.begin(); it != mEntries.end(); ++it)
    {
        if (it->mAnimID == anim_id && it->mSourceID == source_id)
        {
            mEntries.erase(it);
            break;
        }
    }

    SLAnimationLogEntry entry;
    entry.mAnimID = anim_id;
    entry.mSourceID = source_id;
    entry.mStarted = LLDate::now().secondsSinceEpoch();
    mEntries.push_front(entry);

    while (mEntries.size() > MAX_ENTRIES)
    {
        // the oldest of our own first, else the oldest
        auto victim = std::prev(mEntries.end());
        for (auto it = mEntries.rbegin(); it != mEntries.rend(); ++it)
        {
            if (it->mSourceID == gAgentID)
            {
                victim = std::prev(it.base());
                break;
            }
        }
        mEntries.erase(victim);
    }

    mChangedSignal();
}

void SLAnimationLog::updatePlaying(const source_map_t& sources)
{
    const F64 now = LLDate::now().secondsSinceEpoch();
    bool changed = false;
    for (SLAnimationLogEntry& entry : mEntries)
    {
        bool playing = false;
        const auto range = sources.equal_range(entry.mSourceID);
        for (auto it = range.first; it != range.second; ++it)
        {
            if (it->second == entry.mAnimID)
            {
                playing = true;
                break;
            }
        }
        if (!playing && entry.mStopped == 0.0)
        {
            entry.mStopped = now;
            changed = true;
        }
    }
    if (changed)
    {
        mChangedSignal();
    }
}

void SLAnimationLog::clear()
{
    mEntries.clear();
    mChangedSignal();
}

std::string SLAnimationLog::getObjectName(const LLUUID& object_id)
{
    if (auto it = mObjects.find(object_id); it != mObjects.end())
    {
        return it->second.mName;
    }
    const F64 now = LLDate::now().secondsSinceEpoch();
    if (auto it = mRequested.find(object_id); it != mRequested.end() && now - it->second < REQUEST_RETRY)
    {
        return std::string();
    }

    LLViewerObject* objectp = gObjectList.findObject(object_id);
    LLViewerRegion* region = objectp && !objectp->isDead() ? objectp->getRegion() : nullptr;
    if (!region || !gMessageSystem)
    {
        return std::string();
    }
    if (mRequested.size() >= MAX_OBJECTS)
    {
        // bounded: forget the requests that went unanswered (they are asked again when needed)
        for (auto it = mRequested.begin(); it != mRequested.end();)
        {
            it = now - it->second >= REQUEST_RETRY ? mRequested.erase(it) : std::next(it);
        }
        if (mRequested.size() >= MAX_OBJECTS)
        {
            mRequested.clear();
        }
    }
    mRequested[object_id] = now;

    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(_PREHASH_RequestObjectPropertiesFamily);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->nextBlockFast(_PREHASH_ObjectData);
    msg->addU32Fast(_PREHASH_RequestFlags, 0);
    msg->addUUIDFast(_PREHASH_ObjectID, object_id);
    msg->sendReliable(region->getHost());
    return std::string();
}

LLUUID SLAnimationLog::getObjectOwner(const LLUUID& object_id) const
{
    auto it = mObjects.find(object_id);
    return it != mObjects.end() ? it->second.mOwnerID : LLUUID::null;
}

// static
bool SLAnimationLog::processPropertiesFamily(const LLUUID& object_id, const LLUUID& owner_id, const std::string& name)
{
    if (!instanceExists())
    {
        return false;
    }
    SLAnimationLog& self = instance();
    if (!self.mRequested.erase(object_id))
    {
        return false;
    }
    if (self.mObjects.size() >= MAX_OBJECTS)
    {
        self.mObjects.clear();
        self.mRequested.clear();
    }
    self.mObjects[object_id] = { name, owner_id };
    self.mChangedSignal();
    return true;
}
