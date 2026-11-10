/** Disabled owner-side deferred datagrams. SPDX-License-Identifier: LGPL-2.1-only */
#ifndef LL_LLTRANSPORTDEFERREDSEND_H
#define LL_LLTRANSPORTDEFERREDSEND_H
#include "llpacketring.h"
#include "lltransportcircuittypes.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#if LL_WINDOWS
#include <winsock2.h>
#else
#include <cerrno>
#endif

// Owner-only bounded encoded control/data retention. Completion capacity and
// the state-action reservation are held by the caller before enqueue. This is
// NOT a main-command queue and does not replace ordering of circuit commands.
// Never enqueue the same state action twice; reissued state tokens must be
// reconciled by the scheduler before retry. The owner remains responsible for
// RX/control fairness and invoking state commitAttempt/defer exactly once.
template <std::size_t ControlCapacity, std::size_t DataCapacity>
class LLTransportDeferredSend
{
    static_assert(ControlCapacity && DataCapacity);
public:
    enum class Lane : uint8_t { Control, Data };
    enum class Offer : uint8_t { Accepted, Full, Invalid };
    enum class Status : uint8_t { Empty, Deferred, Sent, Failed, Cancelled };
    struct Result { Status status = Status::Empty; Lane lane = Lane::Control; uint64_t token = 0; uint32_t wireBytes = 0; int osError = 0; };

    Offer enqueue(Lane lane, uint64_t token, LLTransportEndpoint endpoint,
                  const uint8_t* encoded, uint32_t size) noexcept
    {
        if (!token || !endpoint.addressNetworkOrder || !endpoint.portHostOrder ||
            !encoded || !size || size > NET_BUFFER_SIZE) return Offer::Invalid;
        return lane == Lane::Control ? push(mControl, token, endpoint, encoded, size)
                                     : push(mData, token, endpoint, encoded, size);
    }

    // Exactly one socket attempt per call. Returned Deferred retains identical
    // bytes, token and packet ID. Windows WSAECONNRESET preserves the legacy
    // assumed-success behavior; POSIX preserves its three-attempt convention.
    // Control/data alternate whenever both have queued work. This permits RX
    // between calls and prevents a blocked data head from monopolizing attempts.
    Result attemptOne(LLPacketRing& ring, int socket, const LLPacketRing::UDPProxySnapshot& proxy) noexcept
    {
        if (!mControl.count && !mData.count) return {};
        Lane lane = mPreferControl ? Lane::Control : Lane::Data;
        if (lane == Lane::Control && !mControl.count) lane = Lane::Data;
        if (lane == Lane::Data && !mData.count) lane = Lane::Control;
        mPreferControl = lane == Lane::Data;
        return lane == Lane::Control ? attempt(mControl, lane, ring, socket, proxy)
                                     : attempt(mData, lane, ring, socket, proxy);
    }

    // Shutdown never waits for socket writability. Caller resolves each
    // returned token using its already-reserved terminal completion capacity.
    Result cancelOne() noexcept
    {
        if (mControl.count) return cancel(mControl, Lane::Control);
        if (mData.count) return cancel(mData, Lane::Data);
        return {};
    }

private:
    struct Entry
    {
        LLTransportEndpoint endpoint{};
        uint64_t token = 0;
        uint32_t size = 0, attempts = 0;
        std::array<uint8_t, NET_BUFFER_SIZE> bytes{};
    };
    template <std::size_t Capacity> struct Queue
    {
        std::array<Entry, Capacity> items{};
        std::size_t head = 0, count = 0;
    };
    template <std::size_t Capacity>
    static Offer push(Queue<Capacity>& queue, uint64_t token, LLTransportEndpoint endpoint,
                      const uint8_t* encoded, uint32_t size) noexcept
    {
        if (queue.count == Capacity) return Offer::Full;
        Entry& entry = queue.items[(queue.head + queue.count) % Capacity];
        entry.endpoint = endpoint; entry.token = token; entry.size = size; entry.attempts = 0;
        std::memcpy(entry.bytes.data(), encoded, size);
        ++queue.count;
        return Offer::Accepted;
    }
    template <std::size_t Capacity>
    static Result attempt(Queue<Capacity>& queue, Lane lane, LLPacketRing& ring, int socket,
                          const LLPacketRing::UDPProxySnapshot& proxy) noexcept
    {
        Entry& entry = queue.items[queue.head];
        NetSendResult result{NetSendStatus::Failed, 0};
        try
        {
            const LLHost target(entry.endpoint.addressNetworkOrder, entry.endpoint.portHostOrder);
            result = ring.sendPacketOnce(socket, reinterpret_cast<const char*>(entry.bytes.data()),
                                         static_cast<S32>(entry.size), target, proxy, entry.attempts == 0);
        }
        catch (...) { result = {NetSendStatus::Failed, 0}; }
        // Saturation keeps the first-attempt/statistics flag false after any
        // arbitrarily long Windows backpressure period.
        if (entry.attempts != UINT32_MAX) ++entry.attempts;
        bool success = result.status == NetSendStatus::Sent;
        bool retry = false;
#if LL_WINDOWS
        retry = result.status == NetSendStatus::WouldBlock;
        if (result.status == NetSendStatus::Refused && result.osError == WSAECONNRESET) success = true;
#else
        retry = (result.osError == EAGAIN || result.osError == ECONNREFUSED) && entry.attempts < 3;
        if (entry.attempts >= 3) success = false;
#endif
        const Result out{retry ? Status::Deferred : success ? Status::Sent : Status::Failed,
                         lane, entry.token, entry.size, result.osError};
        if (!retry) { entry.token = 0; queue.head = (queue.head + 1) % Capacity; --queue.count; }
        return out;
    }
    template <std::size_t Capacity>
    static Result cancel(Queue<Capacity>& queue, Lane lane) noexcept
    {
        Entry& entry = queue.items[queue.head];
        const Result out{Status::Cancelled, lane, entry.token, entry.size, 0};
        entry.token = 0; queue.head = (queue.head + 1) % Capacity; --queue.count;
        return out;
    }
    Queue<ControlCapacity> mControl;
    Queue<DataCapacity> mData;
    bool mPreferControl = true;
};
#endif
