/**
 * @file lltransportcircuitsnapshot.h
 * @brief Nonblocking single-owner circuit snapshot publication.
 *
 * Copyright (c) 2026, Slipstream Viewer Project
 * SPDX-License-Identifier: LGPL-2.1-only
 */
#ifndef LL_LLTRANSPORTCIRCUITSNAPSHOT_H
#define LL_LLTRANSPORTCIRCUITSNAPSHOT_H

#include "lltransportcircuittypes.h"
#include <atomic>
#include <cstdint>
#include <limits>

// One transport-owner writer; any number of main-side readers. Every shared
// field is atomic: a sequence counter around plain fields would be a C++ data
// race. Sequential consistency provides one order for the sequence and fields.
// Reads try a fixed number of times then return Unavailable, never a mixture.
// Writer never waits for a reader. Object lifetime is externally fenced by join.
class LLTransportCircuitSnapshotCell
{
    static_assert(std::atomic<uint64_t>::is_always_lock_free, "64-bit snapshot atomics must be lock-free");
    static_assert(std::atomic<uint32_t>::is_always_lock_free, "32-bit snapshot atomics must be lock-free");
    static_assert(std::atomic<int32_t>::is_always_lock_free, "signed snapshot atomics must be lock-free");

public:
    bool publish(const LLTransportCircuitSnapshot& value) noexcept
    {
        // One writer. Never reuse an even sequence after wrap.
        if (mSequence.load() >= (std::numeric_limits<uint64_t>::max)()-1) return false;
        mSequence.fetch_add(1); // odd: update in progress
        mAddress.store(value.endpoint.addressNetworkOrder);
        mPort.store(value.endpoint.portHostOrder);
        mState.store(static_cast<uint32_t>(value.state));
        mIncarnation.store(value.incarnation);
        mPublication.store(value.publicationSequence);
        mOwnerUsec.store(value.ownerMonotonicUsec);
        mLastPacketUsec.store(value.lastPacketMonotonicUsec);
        mBirthUsec.store(value.birthMonotonicUsec);
        mPacketsIn.store(value.packetsIn);
        mBytesIn.store(value.bytesIn);
        mPacketsOut.store(value.packetsOut);
        mPacketsLost.store(value.packetsLost);
        mLossQuality.store(static_cast<uint32_t>(value.lossQuality));
        mPing.store(value.pingMilliseconds);
        mFlags.store((value.alive ? 1u : 0u) | (value.blocked ? 2u : 0u)
            | (value.trusted ? 4u : 0u) | (value.hasLastPacket ? 8u : 0u));
        mSequence.fetch_add(1); // even: complete coherent publication
        return true;
    }

    LLTransportCircuitSnapshot read() const noexcept
    {
        for (unsigned attempt = 0; attempt < 3; ++attempt)
        {
            const uint64_t before = mSequence.load();
            if (before & 1u) continue;
            LLTransportCircuitSnapshot value;
            value.endpoint.addressNetworkOrder = mAddress.load();
            value.endpoint.portHostOrder = static_cast<uint16_t>(mPort.load());
            value.state = static_cast<LLTransportSnapshotState>(mState.load());
            value.incarnation = mIncarnation.load();
            value.publicationSequence = mPublication.load();
            value.ownerMonotonicUsec = mOwnerUsec.load();
            value.lastPacketMonotonicUsec = mLastPacketUsec.load();
            value.birthMonotonicUsec = mBirthUsec.load();
            value.packetsIn = mPacketsIn.load();
            value.bytesIn = mBytesIn.load();
            value.packetsOut = mPacketsOut.load();
            value.packetsLost = mPacketsLost.load();
            value.lossQuality = static_cast<LLTransportLossQuality>(mLossQuality.load());
            value.pingMilliseconds = mPing.load();
            const uint32_t flags = mFlags.load();
            value.alive = (flags & 1u) != 0;
            value.blocked = (flags & 2u) != 0;
            value.trusted = (flags & 4u) != 0;
            value.hasLastPacket = (flags & 8u) != 0;
            if (before == mSequence.load()) return value;
        }
        return {}; // Unavailable; never fabricate absence or stale presence
    }

private:
    std::atomic<uint64_t> mSequence{0};
    std::atomic<uint32_t> mAddress{0}, mPort{0};
    std::atomic<uint32_t> mState{static_cast<uint32_t>(LLTransportSnapshotState::Unavailable)};
    std::atomic<uint64_t> mIncarnation{0}, mPublication{0}, mOwnerUsec{0}, mLastPacketUsec{0}, mBirthUsec{0};
    std::atomic<uint32_t> mPacketsIn{0};
    std::atomic<int32_t> mBytesIn{0};
    std::atomic<uint32_t> mPacketsOut{0}, mPacketsLost{0}, mLossQuality{0}, mPing{0}, mFlags{0};
};

#endif // LL_LLTRANSPORTCIRCUITSNAPSHOT_H
