/** Bounded coherent owner-to-main circuit directory. SPDX-License-Identifier: LGPL-2.1-only */
#ifndef LL_LLTRANSPORTCIRCUITDIRECTORY_H
#define LL_LLTRANSPORTCIRCUITDIRECTORY_H
#include "lltransportcircuitsnapshot.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

// One owner writer, main readers. Slots and all fields remain alive until join
// and main drain. A global atomic sequence brackets only short structural key
// changes; each cell separately publishes coherent last-committed values.
// All concurrently accessed keys and snapshot fields are themselves atomic.
// Lookup is bounded open addressing, not a scan of owner-only Watch storage.
// False means unavailable/contention/corruption; it is never an absent result.
// Fixed capacity and sizeof(directory) must be charged to the startup ledger.
template <std::size_t Capacity>
class LLTransportCircuitDirectory
{
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0);
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
public:
    // Owner calls only after all initial state has been published and before
    // main can interpret a missing endpoint as authoritative absence.
    bool activateOnOwner() noexcept
    {
        if (mFailed.load() || mExhausted.load()) return false;
        mReady.store(true);
        return true;
    }

    // Call when a state mutation succeeded but its corresponding directory
    // update/erase cannot be published. The latch is permanent for this
    // directory instance: main must never interpret old data as authoritative.
    void failClosedOnOwner() noexcept { mFailed.store(true); mReady.store(false); }

    // Bracket owner state mutation and reconciliation. Readers may observe
    // older committed cells during this interval: these are observations,
    // not authorization for destructive actions or a guarantee of liveness.
    // Pending enables must mask absence until their snapshot is published.
    bool beginStateMutationOnOwner() noexcept
    {
        if (mMutationActive || mFailed.load() || mExhausted.load())
        { failClosedOnOwner(); return false; }
        mMutationActive = true;
        return true;
    }
    bool endStateMutationOnOwner() noexcept
    {
        if (!mMutationActive || mFailed.load()) { failClosedOnOwner(); return false; }
        mMutationActive = false;
        return true;
    }

    // Owner-only. A successful upsert publishes the complete coherent value.
    // No mutation on invalid key, capacity exhaustion or sequence exhaustion.
    bool upsertOnOwner(const LLTransportCircuitSnapshot& value) noexcept
    {
        if (mReady.load() && !mMutationActive) { failClosedOnOwner(); return false; }
        if (value.state != LLTransportSnapshotState::Present || !value.incarnation || !value.publicationSequence)
        { failClosedOnOwner(); return false; }
        const uint64_t key = encode(value.endpoint);
        if (!key || mExhausted.load() || mFailed.load()) { failClosedOnOwner(); return false; }
        std::size_t chosen = Capacity, tombstone = Capacity;
        const auto start = hash(key);
        for (std::size_t n=0; n<Capacity; ++n)
        {
            const auto index = (start+n) & (Capacity-1);
            const auto old = mSlots[index].key.load();
            if (old == key)
            {
                const auto current=mSlots[index].cell.read();
                if (current.state != LLTransportSnapshotState::Present ||
                    value.incarnation < current.incarnation ||
                    (value.incarnation == current.incarnation && value.publicationSequence < current.publicationSequence))
                { failClosedOnOwner(); return false; }
                chosen=index; break;
            }
            if (old == 1 && tombstone == Capacity) tombstone=index;
            if (old == 0) { chosen=tombstone == Capacity ? index : tombstone; break; }
        }
        if (chosen == Capacity) chosen=tombstone;
        if (chosen == Capacity) { failClosedOnOwner(); return false; }
        const bool structural = mSlots[chosen].key.load()!=key;
        if (structural && !beginWrite()) { failClosedOnOwner(); return false; }
        if (!mSlots[chosen].cell.publish(value))
        {
            if (structural) endWrite();
            failClosedOnOwner(); return false;
        }
        if (structural) { mSlots[chosen].key.store(key); endWrite(); }
        return true;
    }

    // Owner-only erase by identity prevents a stale close from removing a
    // newer same-endpoint incarnation. Missing/stale leaves the map unchanged.
    // If state already changed and this erase fails, caller MUST immediately
    // call failClosedOnOwner() before servicing any further main lookup.
    bool eraseOnOwner(LLTransportEndpoint endpoint, uint64_t incarnation) noexcept
    {
        if (mReady.load() && !mMutationActive) { failClosedOnOwner(); return false; }
        const uint64_t key=encode(endpoint);
        if (!key || !incarnation || mExhausted.load() || mFailed.load()) return false;
        const auto start=hash(key);
        for (std::size_t n=0; n<Capacity; ++n)
        {
            const auto index=(start+n)&(Capacity-1);
            const auto old=mSlots[index].key.load();
            if (!old) return false;
            if (old!=key) continue;
            const auto value=mSlots[index].cell.read();
            if (value.state != LLTransportSnapshotState::Present || value.incarnation != incarnation) return false;
            if (!beginWrite()) { failClosedOnOwner(); return false; }
            mSlots[index].key.store(1); // tombstone preserves probe chains
            endWrite();
            return true;
        }
        return false;
    }

    // Reconcile ONLY a fresh, authoritative owner-state lookup taken inside
    // the same mutation bracket. A current Absent result supersedes whatever
    // incarnation was previously published for this endpoint. This is not a
    // stale-close operation: callers must not pass cached observations here.
    bool reconcileStateOnOwner(LLTransportEndpoint endpoint, const LLTransportCircuitSnapshot& value) noexcept
    {
        const auto key = encode(endpoint);
        if (!mMutationActive || !key || mFailed.load() || mExhausted.load() ||
            value.endpoint.addressNetworkOrder != endpoint.addressNetworkOrder ||
            value.endpoint.portHostOrder != endpoint.portHostOrder)
        { failClosedOnOwner(); return false; }
        if (value.state == LLTransportSnapshotState::Present) return upsertOnOwner(value);
        if (value.state != LLTransportSnapshotState::Absent)
        { failClosedOnOwner(); return false; }
        const auto start = hash(key);
        for (std::size_t n = 0; n < Capacity; ++n)
        {
            auto& slot = mSlots[(start+n) & (Capacity-1)];
            const auto old = slot.key.load();
            if (!old) return true;
            if (old != key) continue;
            const auto published = slot.cell.read();
            if (published.state != LLTransportSnapshotState::Present || !published.incarnation ||
                published.endpoint.addressNetworkOrder != endpoint.addressNetworkOrder ||
                published.endpoint.portHostOrder != endpoint.portHostOrder)
            { failClosedOnOwner(); return false; }
            if (!beginWrite()) { failClosedOnOwner(); return false; }
            slot.key.store(1); // owner-state absence; preserve probe chains
            endWrite();
            return true;
        }
        return true; // fully probed: endpoint was absent
    }

    bool get(LLTransportEndpoint endpoint, LLTransportCircuitSnapshot& out) const noexcept
    {
        const auto key=encode(endpoint);
        if (!key || !mReady.load() || mExhausted.load() || mFailed.load()) return false;
        for (unsigned attempt=0; attempt<3; ++attempt)
        {
            const auto before=mSequence.load();
            if (before & 1u) continue;
            LLTransportCircuitSnapshot value;
            value.endpoint=endpoint; value.state=LLTransportSnapshotState::Absent;
            bool invalid=false;
            const auto start=hash(key);
            for (std::size_t n=0; n<Capacity; ++n)
            {
                const auto index=(start+n)&(Capacity-1);
                const auto found=mSlots[index].key.load();
                if (!found) break;
                if (found!=key) continue;
                value=mSlots[index].cell.read();
                if (value.state != LLTransportSnapshotState::Present ||
                    value.endpoint.addressNetworkOrder != endpoint.addressNetworkOrder ||
                    value.endpoint.portHostOrder != endpoint.portHostOrder) invalid=true;
                break;
            }
            if (!invalid && before==mSequence.load() && mReady.load() && !mExhausted.load() && !mFailed.load())
            { out=value; return true; }
        }
        return false;
    }

#if defined(LL_TRANSPORT_DIRECTORY_TESTING)
    // Owner-only, before concurrent use; exercises fail-closed exhaustion.
    void testSetSequence(uint64_t value) noexcept { mSequence.store(value); }
#endif
private:
    struct Slot { std::atomic<uint64_t> key{0}; LLTransportCircuitSnapshotCell cell; };
    static uint64_t encode(LLTransportEndpoint endpoint) noexcept
    {
        if (!endpoint.addressNetworkOrder || !endpoint.portHostOrder) return 0;
        return ((uint64_t(endpoint.addressNetworkOrder)<<16) | endpoint.portHostOrder) + 2;
    }
    static std::size_t hash(uint64_t key) noexcept
    {
        key ^= key >> 30; key *= 0xbf58476d1ce4e5b9ULL;
        key ^= key >> 27; key *= 0x94d049bb133111ebULL;
        key ^= key >> 31;
        return static_cast<std::size_t>(key) & (Capacity-1);
    }
    bool beginWrite() noexcept
    {
        const uint64_t current=mSequence.load();
        if ((current & 1u) || current >= (std::numeric_limits<uint64_t>::max)()-1)
        { mExhausted.store(true); return false; }
        mSequence.store(current+1);
        return true;
    }
    void endWrite() noexcept { mSequence.fetch_add(1); }
    std::array<Slot,Capacity> mSlots{};
    std::atomic<uint64_t> mSequence{0};
    std::atomic<bool> mReady{false}, mExhausted{false}, mFailed{false};
    bool mMutationActive = false; // owner only
};
using LLTransportMainCircuitDirectory = LLTransportCircuitDirectory<2048>;
#endif
