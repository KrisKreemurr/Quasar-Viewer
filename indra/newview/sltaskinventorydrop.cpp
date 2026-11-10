/**
 * @file sltaskinventorydrop.cpp
 * @brief #1466: paced, verified drops of inventory items into a prim's contents.
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

// Why items went missing (LL behaviour this replaces): a multi-item drop sent one
// reliable UpdateTaskInventory / RezScript per item in a single frame, all at once,
// and never looked at the result. A reliable-message ack only means the sim's
// message layer received the packet, not that the task-inventory write was
// applied; writes the sim dropped (or whose packets exhausted their resends) left
// no trace on the viewer. No-copy items were also deleted from the local inventory
// model at send time, so a dropped no-copy write made the item look gone.
//
// What this does instead, per drop (one tracker per target object):
//  1. Reads the prim's contents from the server (RequestTaskInventory cap) BEFORE
//     sending anything: the baseline set of task item ids and the serial.
//  2. Sends kBatchSize items, waits for every ack (or kAckTimeout), waits a settle
//     time that grows when the sim falls behind, re-reads the contents and matches
//     entries that are new since the baseline to what was sent.
//  3. When nothing is left to send, waits until two reads kQuietInterval apart show
//     the same serial and contents (the sim has finished applying writes).
//  4. Retries a missing item only if ALL hold:
//       - no server alert arrived while this drop had writes in flight;
//       - the serial advanced by fewer writes than were sent (a write was lost);
//       - per (type, creator) group, the count of new entries is short by at least
//         the number of items the name/asset match calls missing;
//       - a fresh AIS read shows the item still in the user's inventory, same id,
//         owned by the user, not in Trash (the read also restores a no-copy item
//         in the local model, since the send deleted it locally);
//       - the object still accepts it and it has had fewer than kMaxAttempts sends.
//  5. Reports "N of M added" with each failure's reason (non-modal toast), and logs
//     one line per batch, per retry and a summary.
//
// A drop whose contents can't be read (no cap, the read fails, or someone else's
// prim with "allow drop") is sent paced but never retried, and the toast says the
// result could not be checked.

#include "llviewerprecompiledheaders.h"

#include "sltaskinventorydrop.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lleventcoro.h"
#include "llhttpconstants.h"
#include "llinventorydefines.h"
#include "llinventorymodel.h"
#include "llnotificationsutil.h"
#include "lltrans.h"
#include "llviewerinventory.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "message.h"

#include <cctype>
#include <cstdint>
#include <map>
#include <memory>
#include <unordered_set>
#include <vector>

namespace
{
    // Starting values; the alt test (research/PRIM_DROP_TEST_2026-11-05.md) tunes them.
    constexpr size_t kMaxItemsPerDrop  = 1024; // one drop's fixed record capacity
    constexpr size_t kMaxTrackers      = 8;    // concurrent drops (one per object)
    constexpr size_t kBatchSize        = 20;   // writes per ack-gated batch
    constexpr U8     kMaxAttempts      = 3;    // sends per item, including the first
    constexpr F32    kAckTimeout       = 20.f; // seconds to wait for a batch's acks
    constexpr F32    kSettleBase       = 1.f;  // seconds after acks before a re-read
    constexpr F32    kSettleMax        = 8.f;
    constexpr F32    kQuietInterval    = 3.f;  // seconds between the two quiet reads
    constexpr S32    kQuietChecksMax   = 8;
    constexpr S32    kReadTries        = 3;    // idempotent reads only
    constexpr F32    kReadRetryDelay   = 2.f;
    constexpr F32    kPollInterval     = 0.1f;
    constexpr size_t kMaxNameLen       = 63;   // DB_INV_ITEM_NAME_STR_LEN
    constexpr size_t kMaxAlertLen      = 160;
    constexpr size_t kToastFailLines   = 8;

    enum class EState : U8 { Queued, Sent, Arrived, Failed };

    enum class EReason : U8
    {
        NoReason,
        ItemGone,       // not in your inventory any more when its turn came; not sent
        NotAccepted,    // the object no longer accepts it; not sent
        ObjectGone,     // the object left view (teleport, out of range, deleted)
        ServerRefused,  // the server sent an alert during the drop
        RefusedNotSent, // queued when the server refused something; not sent
        GaveUp,         // still missing after kMaxAttempts sends
        NotInInventory, // missing from the prim and from your inventory
        Uncertain,      // contents changed in a way that can't be matched; not retried
        Unverified,     // sent, but the contents could not be read back
        InventoryCheck, // your inventory could not be read to prove it's still there
        TooMany,        // over the drop's capacity; not sent
        Aborted,        // disconnected or quitting
    };

    struct Record
    {
        LLUUID item_id;
        LLUUID creator_id;
        LLUUID asset_id;
        LLUUID task_item_id; // the prim entry this matched, once Arrived
        LLUUID parent_id;    // the folder the item was in when sent
        std::string name;    // at most kMaxNameLen
        LLAssetType::EType type = LLAssetType::AT_NONE;
        LLToolDragAndDrop::ESource source = LLToolDragAndDrop::SOURCE_AGENT;
        U8 attempts = 0;
        EState state = EState::Queued;
        EReason reason = EReason::NoReason;
        bool is_script = false;
        bool script_active = true;
        bool no_copy = false;
        LLPointer<LLViewerInventoryItem> retry_item; // server-read copy, only while a retry is queued
    };

    struct TaskEntry
    {
        LLUUID id;
        LLUUID asset_id;
        LLUUID creator_id;
        std::string name;
        LLAssetType::EType type = LLAssetType::AT_NONE;
    };

    struct TaskSnapshot
    {
        bool ok = false;
        S16 serial = 0;
        std::vector<TaskEntry> entries;
    };

    struct Tracker
    {
        U32 id = 0;
        LLUUID object_id;
        std::vector<Record> records;
        std::unordered_set<LLUUID> baseline_ids;
        S16 baseline_serial = 0;
        bool verifiable = false;
        bool finishing = false;
        bool refused = false;
        std::string alert;
        S32 sends = 0;
        S32 acks_pending = 0;
        S32 ack_failures = 0;
        S32 overflow = 0;
        S32 batches = 0;
        S32 retries = 0;
        S32 possible_duplicates = 0;
        F32 settle = kSettleBase;
    };

    std::map<U32, std::shared_ptr<Tracker>> sTrackers;
    U32 sNextTrackerId = 1;

    std::shared_ptr<Tracker> find_tracker(U32 id)
    {
        auto it = sTrackers.find(id);
        return it == sTrackers.end() ? std::shared_ptr<Tracker>() : it->second;
    }

    bool exiting()
    {
        return gDisconnected || LLApp::isExiting();
    }

    std::string bounded(const std::string& s, size_t len)
    {
        return s.size() <= len ? s : s.substr(0, len);
    }

    // The sim gives a duplicate name a numeric suffix ("Name 1").
    bool name_matches(const std::string& entry_name, const std::string& sent_name)
    {
        if (entry_name == sent_name)
        {
            return true;
        }
        if (entry_name.size() < sent_name.size() + 2
            || entry_name.compare(0, sent_name.size(), sent_name) != 0
            || entry_name[sent_name.size()] != ' ')
        {
            return false;
        }
        for (size_t i = sent_name.size() + 1; i < entry_name.size(); ++i)
        {
            if (!isdigit((unsigned char)entry_name[i]))
            {
                return false;
            }
        }
        return true;
    }

    bool in_trash(const LLUUID& parent_id)
    {
        const LLUUID trash_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
        return trash_id.notNull()
            && (parent_id == trash_id || gInventory.isObjectDescendentOf(parent_id, trash_id));
    }

    LLViewerObject* live_object(const Tracker& t)
    {
        LLViewerObject* obj = gObjectList.findObject(t.object_id);
        if (!obj || obj->isDead() || !obj->getRegion())
        {
            return nullptr;
        }
        return obj;
    }

    // The ack token carries the tracker id and the batch, so a late ack from a
    // timed-out batch can't end the next batch's wait early.
    static_assert(sizeof(uintptr_t) >= 8, "ack token packs two 32-bit values");

    void** pack_token(U32 id, U32 batch)
    {
        return reinterpret_cast<void**>((static_cast<uintptr_t>(batch) << 32) | id);
    }

    void on_write_ack(void** data, S32 result)
    {
        const uintptr_t token = reinterpret_cast<uintptr_t>(data);
        const U32 id = static_cast<U32>(token & 0xffffffffu);
        const U32 batch = static_cast<U32>(token >> 32);
        std::shared_ptr<Tracker> t = find_tracker(id);
        if (!t || batch != (U32)t->batches)
        {
            return;
        }
        t->acks_pending = llmax(0, t->acks_pending - 1);
        if (result != LL_ERR_NOERR)
        {
            ++t->ack_failures;
        }
    }

    // Idempotent read of the prim's contents from the server. Never the cached list.
    TaskSnapshot read_task_inventory(const LLUUID& object_id)
    {
        TaskSnapshot snap;
        for (S32 attempt = 0; attempt < kReadTries && !exiting(); ++attempt)
        {
            if (attempt > 0)
            {
                llcoro::suspendUntilTimeout(kReadRetryDelay);
            }
            LLViewerObject* obj = gObjectList.findObject(object_id);
            if (!obj || obj->isDead() || !obj->getRegion())
            {
                return snap;
            }
            const std::string cap = obj->getRegion()->getCapability("RequestTaskInventory");
            if (cap.empty())
            {
                return snap;
            }
            LLCore::HttpRequest::policy_t policy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
            LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter(
                new LLCoreHttpUtil::HttpCoroutineAdapter("TaskInvDropVerify", policy));
            LLCore::HttpRequest::ptr_t request(new LLCore::HttpRequest);
            LLSD result = adapter->getAndSuspend(request, cap + "?task_id=" + object_id.asString());
            LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(
                result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
            if (!status || !result.has("contents") || !result.has("inventory_serial"))
            {
                LL_WARNS("TaskInvDrop") << "contents read for " << object_id << " failed: "
                                        << status.toString() << LL_ENDL;
                continue;
            }
            snap.serial = (S16)result["inventory_serial"].asInteger();
            const LLSD& contents = result["contents"];
            snap.entries.reserve(contents.size());
            for (const auto& entry : llsd::inArray(contents))
            {
                if (!entry.has("item_id"))
                {
                    continue;
                }
                LLPointer<LLViewerInventoryItem> inv = new LLViewerInventoryItem;
                if (!inv->unpackMessage(entry))
                {
                    continue;
                }
                TaskEntry e;
                e.id = inv->getUUID();
                e.asset_id = inv->getAssetUUID();
                e.creator_id = inv->getPermissions().getCreator();
                e.name = inv->getName();
                e.type = inv->getType();
                snap.entries.push_back(std::move(e));
            }
            snap.ok = true;
            return snap;
        }
        return snap;
    }

    enum class EPresence : U8 { Present, Absent, InTrash, Unknown };

    // Idempotent AIS read of one item of the user's inventory. On success the item is
    // also applied to the local model, which restores a no-copy item the send deleted.
    EPresence read_agent_item(const LLUUID& item_id, LLPointer<LLViewerInventoryItem>& out)
    {
        for (S32 attempt = 0; attempt < kReadTries && !exiting(); ++attempt)
        {
            if (attempt > 0)
            {
                llcoro::suspendUntilTimeout(kReadRetryDelay);
            }
            LLViewerRegion* region = gAgent.getRegion();
            const std::string cap = region ? region->getCapability("InventoryAPIv3") : std::string();
            if (cap.empty())
            {
                return EPresence::Unknown;
            }
            LLCore::HttpRequest::policy_t policy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
            LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter(
                new LLCoreHttpUtil::HttpCoroutineAdapter("TaskInvDropItemCheck", policy));
            LLCore::HttpRequest::ptr_t request(new LLCore::HttpRequest);
            LLSD result = adapter->getAndSuspend(request, cap + "/item/" + item_id.asString());
            LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(
                result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
            const S32 http = (S32)status.getType();
            if (http == HTTP_NOT_FOUND || http == HTTP_GONE)
            {
                return EPresence::Absent;
            }
            if (!status || !result.isMap())
            {
                continue;
            }
            if (result["item_id"].asUUID() != item_id || result.has("linked_id"))
            {
                return EPresence::Unknown;
            }
            LLPointer<LLViewerInventoryItem> item = new LLViewerInventoryItem;
            if (!item->unpackMessage(result) || item->getUUID() != item_id
                || item->getParentUUID().isNull()
                || item->getPermissions().getOwner() != gAgent.getID())
            {
                return EPresence::Unknown;
            }
            item->setComplete(true);
            if (in_trash(item->getParentUUID()))
            {
                return EPresence::InTrash;
            }
            // Mirror the server's copy into the local model (a no-copy send deleted it).
            LLPointer<LLViewerInventoryItem> local = new LLViewerInventoryItem(item);
            gInventory.updateItem(local);
            gInventory.notifyObservers();
            out = item;
            return EPresence::Present;
        }
        return EPresence::Unknown;
    }

    // Recomputes Sent -> Arrived from a fresh snapshot. An entry matches at most one
    // record; arrived records keep their entry.
    void match_arrivals(Tracker& t, const TaskSnapshot& snap)
    {
        std::unordered_set<LLUUID> used;
        for (const Record& r : t.records)
        {
            if (r.state == EState::Arrived)
            {
                used.insert(r.task_item_id);
            }
        }
        std::vector<const TaskEntry*> fresh;
        fresh.reserve(snap.entries.size());
        for (const TaskEntry& e : snap.entries)
        {
            if (t.baseline_ids.count(e.id) == 0 && used.count(e.id) == 0)
            {
                fresh.push_back(&e);
            }
        }

        // Strongest key first: same item id, same asset, exact name, suffixed name.
        for (int pass = 0; pass < 4; ++pass)
        {
            for (Record& r : t.records)
            {
                if (r.state != EState::Sent)
                {
                    continue;
                }
                for (const TaskEntry*& e : fresh)
                {
                    if (!e)
                    {
                        continue;
                    }
                    bool hit = false;
                    switch (pass)
                    {
                    case 0: hit = e->id == r.item_id; break;
                    case 1: hit = r.asset_id.notNull() && e->asset_id == r.asset_id && e->type == r.type; break;
                    case 2: hit = e->type == r.type && e->creator_id == r.creator_id && e->name == r.name; break;
                    default: hit = e->type == r.type && e->creator_id == r.creator_id && name_matches(e->name, r.name); break;
                    }
                    if (hit)
                    {
                        r.state = EState::Arrived;
                        r.task_item_id = e->id;
                        r.retry_item = nullptr;
                        e = nullptr;
                        break;
                    }
                }
            }
        }
    }

    S32 count_state(const Tracker& t, EState state)
    {
        S32 n = 0;
        for (const Record& r : t.records)
        {
            n += (r.state == state) ? 1 : 0;
        }
        return n;
    }

    void fail(Record& r, EReason reason)
    {
        r.state = EState::Failed;
        r.reason = reason;
        r.retry_item = nullptr;
    }

    // One write. Returns false when the record failed before sending.
    bool send_record(Tracker& t, Record& r, LLViewerObject* obj)
    {
        LLPointer<LLViewerInventoryItem> item = r.retry_item.notNull()
            ? r.retry_item
            : LLPointer<LLViewerInventoryItem>(gInventory.getItem(r.item_id));
        if (item.isNull())
        {
            fail(r, EReason::ItemGone);
            return false;
        }
        if (obj->isLocalOnly() || !LLToolDragAndDrop::isInventoryDropAcceptable(obj, item))
        {
            fail(r, EReason::NotAccepted);
            return false;
        }

        // Copy before any local delete below drops the model's reference.
        LLPointer<LLViewerInventoryItem> payload = new LLViewerInventoryItem(item);
        payload->setCreationDate(time_corrected());
        r.parent_id = item->getParentUUID();
        r.no_copy = !item->getPermissions().allowCopyBy(gAgent.getID());

        if (r.no_copy && r.source != LLToolDragAndDrop::SOURCE_LIBRARY)
        {
            // The server moves a no-copy item. Hide it locally (as LL did, so it can't be
            // dropped twice), and drop the folder's cached version so a viewer that quits
            // before this drop is verified re-reads the folder at the next login instead
            // of trusting a cache that lacks the item.
            if (LLViewerInventoryCategory* cat = gInventory.getCategory(r.parent_id))
            {
                cat->setVersion(LLViewerInventoryCategory::VERSION_UNKNOWN);
            }
            if (gInventory.getItem(r.item_id))
            {
                gInventory.deleteObject(r.item_id);
                gInventory.notifyObservers();
            }
        }

        ++t.acks_pending;
        if (r.is_script)
        {
            obj->saveScript(payload, r.script_active, true, &on_write_ack, pack_token(t.id, (U32)t.batches));
        }
        else
        {
            obj->updateInventory(payload, TASK_INVENTORY_ITEM_KEY, true, &on_write_ack, pack_token(t.id, (U32)t.batches));
        }
        ++r.attempts;
        ++t.sends;
        r.state = EState::Sent;
        r.retry_item = nullptr;
        return true;
    }

    void wait_for_acks(Tracker& t)
    {
        F32 waited = 0.f;
        while (t.acks_pending > 0 && waited < kAckTimeout && !exiting())
        {
            llcoro::suspendUntilTimeout(kPollInterval);
            waited += kPollInterval;
        }
        if (t.acks_pending > 0)
        {
            LL_WARNS("TaskInvDrop") << "drop " << t.id << ": " << t.acks_pending
                                    << " write(s) unacknowledged after " << kAckTimeout << " s" << LL_ENDL;
            t.acks_pending = 0; // late acks are clamped at zero
        }
    }

    // Everything still queued or sent is closed out with reason.
    void close_out(Tracker& t, EReason queued_reason, EReason sent_reason)
    {
        for (Record& r : t.records)
        {
            if (r.state == EState::Queued)
            {
                fail(r, queued_reason);
            }
            else if (r.state == EState::Sent)
            {
                fail(r, sent_reason);
            }
        }
    }

    void close_sent(Tracker& t, EReason reason)
    {
        for (Record& r : t.records)
        {
            if (r.state == EState::Sent)
            {
                fail(r, reason);
            }
        }
    }

    // Sends everything queued, batch by batch. Returns false when the drop must stop.
    bool send_phase(Tracker& t)
    {
        for (;;)
        {
            if (exiting())
            {
                close_out(t, EReason::Aborted, EReason::Unverified);
                return false;
            }
            LLViewerObject* obj = live_object(t);
            if (!obj)
            {
                close_out(t, EReason::ObjectGone, EReason::ObjectGone);
                return false;
            }

            size_t sent = 0;
            S32 arrived_before = count_state(t, EState::Arrived);
            ++t.batches; // the acks of this batch carry this number
            t.acks_pending = 0;
            for (Record& r : t.records)
            {
                if (sent >= kBatchSize)
                {
                    break;
                }
                if (r.state == EState::Queued && send_record(t, r, obj))
                {
                    ++sent;
                }
            }
            if (sent == 0)
            {
                --t.batches;
                return true;
            }

            const S32 failures_before = t.ack_failures;
            wait_for_acks(t);
            llcoro::suspendUntilTimeout(t.settle);
            if (exiting())
            {
                close_out(t, EReason::Aborted, EReason::Unverified);
                return false;
            }

            S32 arrived_now = arrived_before;
            bool checked = false;
            S32 serial_delta = 0;
            if (t.verifiable)
            {
                TaskSnapshot snap = read_task_inventory(t.object_id);
                if (snap.ok)
                {
                    match_arrivals(t, snap);
                    arrived_now = count_state(t, EState::Arrived);
                    serial_delta = (S16)(snap.serial - t.baseline_serial);
                    checked = true;
                }
            }
            const S32 seen = arrived_now - arrived_before;
            LL_INFOS("TaskInvDrop") << "drop " << t.id << " batch " << t.batches << ": sent " << sent
                                    << ", ack failures " << (t.ack_failures - failures_before)
                                    << (checked ? ", arrived " + std::to_string(seen) + ", serial +" + std::to_string(serial_delta)
                                                : std::string(", not checked"))
                                    << " for " << t.sends << " writes so far, settle " << t.settle << " s" << LL_ENDL;
            // Pace to the sim: back off while it lags, recover slowly when it keeps up.
            if (checked && seen < (S32)sent)
            {
                t.settle = llmin(t.settle * 2.f, kSettleMax);
            }
            else
            {
                t.settle = llmax(t.settle * 0.75f, kSettleBase);
            }
        }
    }

    // Waits until the contents stop changing; returns the last snapshot (ok=false if
    // they never settled or couldn't be read).
    TaskSnapshot quiet_snapshot(Tracker& t)
    {
        TaskSnapshot prev = read_task_inventory(t.object_id);
        for (S32 i = 0; i < kQuietChecksMax && prev.ok && !exiting(); ++i)
        {
            llcoro::suspendUntilTimeout(kQuietInterval);
            TaskSnapshot next = read_task_inventory(t.object_id);
            if (!next.ok)
            {
                break;
            }
            if (next.serial == prev.serial && next.entries.size() == prev.entries.size())
            {
                return next;
            }
            prev = std::move(next);
        }
        TaskSnapshot none;
        return none;
    }

    struct GroupKey
    {
        LLAssetType::EType type;
        LLUUID creator;
        bool operator<(const GroupKey& o) const
        {
            return type != o.type ? type < o.type : creator < o.creator;
        }
    };

    // Decides what to retry after a quiet snapshot. Returns true if anything was queued.
    bool plan_retries(Tracker& t, const TaskSnapshot& snap)
    {
        match_arrivals(t, snap);

        // Per (type, creator) group: items sent at least once, entries new since the
        // baseline, and items the matcher calls missing.
        std::map<GroupKey, S32> sent_in_group, fresh_in_group, missing_in_group;
        for (const Record& r : t.records)
        {
            if (r.attempts > 0)
            {
                ++sent_in_group[{ r.type, r.creator_id }];
            }
            if (r.state == EState::Sent)
            {
                ++missing_in_group[{ r.type, r.creator_id }];
            }
        }
        for (const TaskEntry& e : snap.entries)
        {
            if (t.baseline_ids.count(e.id) == 0)
            {
                ++fresh_in_group[{ e.type, e.creator_id }];
            }
        }
        // More new entries than items sent in a group: a late write landed after a
        // retry (or someone else added the same kind of item). Reported, never "fixed".
        S32 extra = 0;
        for (const auto& [key, fresh] : fresh_in_group)
        {
            auto it = sent_in_group.find(key);
            if (it != sent_in_group.end() && fresh > it->second)
            {
                extra += fresh - it->second;
            }
        }
        if (extra > 0 && extra != t.possible_duplicates)
        {
            LL_WARNS("TaskInvDrop") << "drop " << t.id << ": " << extra
                                    << " more new entries than items sent (possible duplicate)" << LL_ENDL;
        }
        t.possible_duplicates = extra;

        const S32 missing = count_state(t, EState::Sent);
        if (missing == 0)
        {
            return false;
        }
        if (t.refused)
        {
            close_out(t, EReason::RefusedNotSent, EReason::ServerRefused);
            return false;
        }
        const S32 serial_delta = (S16)(snap.serial - t.baseline_serial);
        if (serial_delta >= t.sends)
        {
            // The server counted a write for every send: whatever didn't match by
            // name is there under some other name. Never resend.
            LL_INFOS("TaskInvDrop") << "drop " << t.id << ": " << missing << " unmatched but serial +"
                                    << serial_delta << " covers all " << t.sends << " writes; not retrying" << LL_ENDL;
            close_sent(t, EReason::Uncertain);
            return false;
        }

        LLViewerObject* obj = live_object(t);
        bool queued = false;
        for (Record& r : t.records)
        {
            if (r.state != EState::Sent)
            {
                continue;
            }
            const GroupKey key{ r.type, r.creator_id };
            const S32 deficit = sent_in_group[key] - fresh_in_group[key];
            if (deficit < missing_in_group[key])
            {
                fail(r, EReason::Uncertain);
                continue;
            }
            if (!obj && r.attempts < kMaxAttempts)
            {
                fail(r, EReason::ObjectGone);
                continue;
            }

            // Read even when out of attempts: the report says where the item is, and
            // the read restores a no-copy item in the local model.
            LLPointer<LLViewerInventoryItem> verified;
            EPresence presence = EPresence::Unknown;
            if (r.source == LLToolDragAndDrop::SOURCE_LIBRARY)
            {
                // The library is read-only and always copyable.
                verified = gInventory.getItem(r.item_id);
                presence = verified.notNull() ? EPresence::Present : EPresence::Absent;
            }
            else
            {
                presence = read_agent_item(r.item_id, verified);
            }
            obj = live_object(t); // the read suspended
            if (t.refused)
            {
                fail(r, EReason::ServerRefused);
                continue;
            }
            switch (presence)
            {
            case EPresence::Present:
                if (r.attempts >= kMaxAttempts)
                {
                    fail(r, EReason::GaveUp);
                    break;
                }
                if (!obj)
                {
                    fail(r, EReason::ObjectGone);
                    break;
                }
                r.retry_item = verified;
                r.state = EState::Queued;
                ++t.retries;
                queued = true;
                LL_INFOS("TaskInvDrop") << "drop " << t.id << " retry: item " << r.item_id << " type "
                                        << LLAssetType::lookup(r.type) << (r.no_copy ? " (no-copy)" : "")
                                        << " attempt " << (S32)(r.attempts + 1) << " of " << (S32)kMaxAttempts
                                        << ": absent from the prim, present in inventory" << LL_ENDL;
                break;
            case EPresence::Absent:
                fail(r, EReason::NotInInventory);
                break;
            case EPresence::InTrash:
                fail(r, EReason::ItemGone);
                break;
            default:
                fail(r, EReason::InventoryCheck);
                break;
            }
        }
        return queued;
    }

    std::string reason_text(EReason reason, const Tracker& t)
    {
        switch (reason)
        {
        case EReason::ItemGone:       return LLTrans::getString("TaskInvDropItemGone");
        case EReason::NotAccepted:    return LLTrans::getString("TaskInvDropNotAccepted");
        case EReason::ObjectGone:     return LLTrans::getString("TaskInvDropObjectGone");
        case EReason::ServerRefused:
        {
            LLStringUtil::format_map_t args;
            args["[ALERT]"] = t.alert;
            return LLTrans::getString("TaskInvDropRefused", args);
        }
        case EReason::RefusedNotSent:
        {
            LLStringUtil::format_map_t args;
            args["[ALERT]"] = t.alert;
            return LLTrans::getString("TaskInvDropRefusedNotSent", args);
        }
        case EReason::GaveUp:         return LLTrans::getString("TaskInvDropGaveUp");
        case EReason::NotInInventory: return LLTrans::getString("TaskInvDropNotInInventory");
        case EReason::Uncertain:      return LLTrans::getString("TaskInvDropUncertain");
        case EReason::Unverified:     return LLTrans::getString("TaskInvDropUnverified");
        case EReason::InventoryCheck: return LLTrans::getString("TaskInvDropInventoryCheck");
        case EReason::TooMany:        return LLTrans::getString("TaskInvDropTooMany");
        case EReason::Aborted:        return LLTrans::getString("TaskInvDropAborted");
        default:                      return std::string();
        }
    }

    void finish(Tracker& t)
    {
        t.finishing = true;
        close_out(t, EReason::Aborted, t.verifiable ? EReason::GaveUp : EReason::Unverified);

        const S32 total = (S32)t.records.size() + t.overflow;
        const S32 added = count_state(t, EState::Arrived);
        S32 unverified = 0;
        S32 failed = 0;
        for (const Record& r : t.records)
        {
            if (r.state == EState::Failed)
            {
                (r.reason == EReason::Unverified ? unverified : failed) += 1;
            }
        }
        failed += t.overflow;

        LL_INFOS("TaskInvDrop") << "drop " << t.id << " done: " << added << " of " << total << " added, "
                                << failed << " failed, " << unverified << " unverified, " << t.retries
                                << " retries, " << t.sends << " writes in " << t.batches << " batches, "
                                << t.ack_failures << " ack failures" << (t.refused ? ", server alert" : "")
                                << (t.possible_duplicates ? ", possible duplicates " + std::to_string(t.possible_duplicates) : std::string())
                                << LL_ENDL;
        for (const Record& r : t.records)
        {
            if (r.state == EState::Failed && r.reason != EReason::Unverified)
            {
                LL_INFOS("TaskInvDrop") << "drop " << t.id << " not added: item " << r.item_id << " type "
                                        << LLAssetType::lookup(r.type) << " sends " << (S32)r.attempts
                                        << " reason " << (S32)r.reason << LL_ENDL;
            }
        }

        // Re-read the folders whose cached version a no-copy send dropped: restores the
        // local view of anything the server still has.
        std::unordered_set<LLUUID> folders;
        for (const Record& r : t.records)
        {
            if (r.no_copy && r.parent_id.notNull() && folders.insert(r.parent_id).second)
            {
                if (LLViewerInventoryCategory* cat = gInventory.getCategory(r.parent_id))
                {
                    if (cat->getVersion() == LLViewerInventoryCategory::VERSION_UNKNOWN)
                    {
                        cat->fetch();
                    }
                }
            }
        }

        // Let an open Contents panel show the server's list.
        if (LLViewerObject* obj = live_object(t))
        {
            if (obj->hasInventoryListeners())
            {
                obj->dirtyInventory();
                obj->requestInventory();
            }
        }

        if (exiting() || (total <= 1 && failed == 0 && t.possible_duplicates == 0))
        {
            return;
        }

        LLStringUtil::format_map_t args;
        args["[ADDED]"] = std::to_string(added);
        args["[TOTAL]"] = std::to_string(total);
        std::string text = LLTrans::getString("TaskInvDropSummary", args);
        if (unverified > 0)
        {
            LLStringUtil::format_map_t uargs;
            uargs["[COUNT]"] = std::to_string(unverified);
            text += "\n" + LLTrans::getString("TaskInvDropUnverifiedCount", uargs);
        }
        if (t.possible_duplicates > 0)
        {
            LLStringUtil::format_map_t dargs;
            dargs["[COUNT]"] = std::to_string(t.possible_duplicates);
            text += "\n" + LLTrans::getString("TaskInvDropPossibleDuplicates", dargs);
        }
        size_t lines = 0;
        for (const Record& r : t.records)
        {
            if (r.state != EState::Failed || r.reason == EReason::Unverified)
            {
                continue;
            }
            if (lines++ < kToastFailLines)
            {
                text += "\n" + r.name + ": " + reason_text(r.reason, t);
            }
        }
        if (t.overflow > 0 && lines++ < kToastFailLines)
        {
            LLStringUtil::format_map_t oargs;
            oargs["[COUNT]"] = std::to_string(t.overflow);
            text += "\n" + LLTrans::getString("TaskInvDropOverflow", oargs) + ": " + reason_text(EReason::TooMany, t);
        }
        if (lines > kToastFailLines)
        {
            LLStringUtil::format_map_t margs;
            margs["[COUNT]"] = std::to_string(lines - kToastFailLines);
            text += "\n" + LLTrans::getString("TaskInvDropMore", margs);
        }
        LLSD nargs;
        nargs["MESSAGE"] = text;
        LLNotificationsUtil::add("TaskInventoryDropResult", nargs);
    }

    void run_tracker(U32 id)
    {
        std::shared_ptr<Tracker> t = find_tracker(id);
        if (!t)
        {
            return;
        }

        // Baseline before any write. Yield first so the drop loop that queued us has
        // queued every item before the first batch goes out.
        llcoro::suspendUntilTimeout(kPollInterval);

        // Someone else's prim that only allows drops hides its contents from you:
        // don't spend reads on it.
        LLViewerObject* obj = live_object(*t);
        const bool can_see = obj && (obj->permModify() || obj->permYouOwner());
        TaskSnapshot base;
        if (can_see)
        {
            base = read_task_inventory(t->object_id);
        }
        t->verifiable = base.ok && can_see && live_object(*t);
        if (base.ok)
        {
            t->baseline_serial = base.serial;
            t->baseline_ids.reserve(base.entries.size());
            for (const TaskEntry& e : base.entries)
            {
                t->baseline_ids.insert(e.id);
            }
        }
        LL_INFOS("TaskInvDrop") << "drop " << t->id << " into " << t->object_id << ": " << t->records.size()
                                << " items, baseline " << (base.ok ? std::to_string(base.entries.size()) + " entries serial " + std::to_string(base.serial) : std::string("unreadable"))
                                << (t->verifiable ? "" : ", not verifiable: sending paced, no retries") << LL_ENDL;

        while (send_phase(*t))
        {
            if (!t->verifiable)
            {
                // Items queued during the last settle are picked up by send_phase; only
                // stop once nothing is queued.
                if (count_state(*t, EState::Queued) == 0)
                {
                    break;
                }
                continue;
            }
            TaskSnapshot quiet = quiet_snapshot(*t);
            if (!quiet.ok)
            {
                LL_WARNS("TaskInvDrop") << "drop " << t->id << ": contents never settled or unreadable; not retrying" << LL_ENDL;
                close_sent(*t, EReason::Uncertain);
            }
            else
            {
                plan_retries(*t, quiet);
            }
            // Retries, and items another drop merged in meanwhile, are queued.
            if (count_state(*t, EState::Queued) == 0)
            {
                break;
            }
        }

        finish(*t);
        sTrackers.erase(id);
    }
}

namespace SLTaskInvDrop
{
    void queueItem(LLViewerObject* target,
                   LLInventoryItem* item,
                   LLToolDragAndDrop::ESource source,
                   bool is_script,
                   bool script_active)
    {
        if (!target || !item)
        {
            return;
        }

        std::shared_ptr<Tracker> t;
        for (auto& [id, tracker] : sTrackers)
        {
            if (tracker->object_id == target->getID() && !tracker->finishing)
            {
                t = tracker;
                break;
            }
        }
        bool launch = false;
        if (!t)
        {
            if (sTrackers.size() >= kMaxTrackers)
            {
                LL_WARNS("TaskInvDrop") << "too many drops in flight; item " << item->getUUID() << " not sent" << LL_ENDL;
                LLNotificationsUtil::add("TaskInventoryDropBusy");
                return;
            }
            t = std::make_shared<Tracker>();
            t->id = sNextTrackerId++;
            if (sNextTrackerId == 0)
            {
                sNextTrackerId = 1;
            }
            t->object_id = target->getID();
            // Fixed capacity: the vector never reallocates, so records stay put while
            // the tracker coroutine holds references across a suspension.
            t->records.reserve(kMaxItemsPerDrop);
            sTrackers[t->id] = t;
            launch = true;
        }

        if (t->records.size() >= kMaxItemsPerDrop)
        {
            ++t->overflow;
        }
        else
        {
            Record r;
            r.item_id = item->getUUID();
            r.creator_id = item->getPermissions().getCreator();
            r.asset_id = item->getAssetUUID();
            r.name = bounded(item->getName(), kMaxNameLen);
            r.type = item->getType();
            r.source = source;
            r.is_script = is_script;
            r.script_active = script_active;
            t->records.push_back(std::move(r));
        }

        if (launch)
        {
            const U32 id = t->id;
            LLCoros::instance().launch("SLTaskInvDrop", [id]() { run_tracker(id); });
        }
    }

    void onServerAlert(const std::string& notification, const std::string& message)
    {
        if (message.find("Autopilot canceled") != std::string::npos)
        {
            return;
        }
        const std::string text = bounded(message.empty() ? notification : message, kMaxAlertLen);
        for (auto& [id, t] : sTrackers)
        {
            if (t->sends > 0 && !t->finishing && !t->refused)
            {
                t->refused = true;
                t->alert = text;
                LL_INFOS("TaskInvDrop") << "drop " << id << ": server alert during the drop ("
                                        << (notification.empty() ? std::string("text") : notification)
                                        << "); retries off" << LL_ENDL;
            }
        }
    }
}
