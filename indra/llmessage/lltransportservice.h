/**
 * @file lltransportservice.h
 * @brief Bounded transport-owner service, first transmit-only slice.
 *
 * Copyright (c) 2026, Slipstream Viewer Project
 * SPDX-License-Identifier: LGPL-2.1-only
 */
#ifndef LL_LLTRANSPORTSERVICE_H
#define LL_LLTRANSPORTSERVICE_H

#include "llpacketring.h"
#include "lltransportclock.h"
#include "lltransportspscqueue.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

// Complete value types before instantiating the queue. GCC evaluates the
// queue's no-throw trait while the enclosing service class is incomplete.
enum class LLTransportSendResult { Sent, Failed };
struct LLTransportCompletion
{
    uint64_t operationId = 0;
    LLTransportSendResult result = LLTransportSendResult::Failed;
};
struct LLTransportCommand
{
    uint64_t operationId = 0;
    uint32_t addressNetworkOrder = 0;
    uint32_t size = 0;
    uint16_t portHostOrder = 0;
    std::array<uint8_t, NET_BUFFER_SIZE> bytes{};
};

// First service slice: owns the supplied socket for *outbound unacknowledged*
// preformatted datagrams only. It does not receive, run circuits, generate
// packet IDs, ACK or retry reliable messages. It MUST NOT replace the existing
// message pump or be enabled in the viewer until those owner paths are added.
// Main is the sole producer/consumer of its API; the service thread is the
// opposite side. No callback or viewer pointer is passed to the worker.
class LLTransportService
{
public:
    enum class StartResult { Started, InvalidSocket, AllocationFailed, ThreadFailed };
    enum class OfferResult { Accepted, Full, Stopped, Invalid };
    using SendResult = LLTransportSendResult;
    using Completion = LLTransportCompletion;
    struct ClockAnchor
    {
        uint64_t mainMessageUsec = 0;
        std::chrono::steady_clock::time_point steadyTime{};
    };

    // Main calls totalTime/getMessageTime before passing its value here; no
    // worker calls those shared mutable legacy clock functions.
    static ClockAnchor captureClockOnMain(uint64_t mainMessageUsec) noexcept;

    // Caller transfers exclusive socket use until stopAndJoin(). The socket is
    // already nonblocking and remains caller-owned; this object does not close it.
    // Seed and proxy value are prepared on main, before the worker starts.
    static std::unique_ptr<LLTransportService> start(int socket, const LLPacketRing::UDPProxySnapshot& proxy,
                                                      uint32_t dropSeed, ClockAnchor anchor,
                                                      StartResult& result) noexcept;
    ~LLTransportService();
    LLTransportService(const LLTransportService&) = delete;
    LLTransportService& operator=(const LLTransportService&) = delete;

    // The bytes are copied on admission. Reliable, resent and ACK-bearing
    // datagrams are refused in this slice: their circuit transaction is not
    // implemented. A nonzero operationId receives one terminal completion.
    OfferResult trySend(uint64_t operationId, uint32_t addressNetworkOrder, uint16_t portHostOrder,
                        const uint8_t* bytes, uint32_t size) noexcept;
    bool tryPopCompletion(Completion& result) noexcept;
    std::size_t outstanding() const noexcept { return mOutstanding; } // main only
    bool failed() const noexcept { return mFailed.load() || mPoisoned.load(); }
    // Anchored legacy message-time domain; never pass to circuit state.
    uint64_t ownerMessageTimeUsec() const noexcept { return mOwnerMessageUsec.load(); }
    void requestStop() noexcept;
    // Main only; resolves accepted commands before join. A would-block command
    // becomes a failure on explicit stop rather than preventing shutdown.
    // Retain and drain every completion before destroying the service.
    void stopAndJoin();

private:
    static constexpr std::size_t kCapacity = 64;
    using Command = LLTransportCommand;
    LLTransportService(int socket, const LLPacketRing::UDPProxySnapshot& proxy, ClockAnchor anchor);
    void run() noexcept;
    uint64_t ownerLegacyMessageUsec() const noexcept;

    int mSocket;
    LLPacketRing::UDPProxySnapshot mProxy;
    LLPacketRing mRing;
    ClockAnchor mClock;
    LLTransportSpscQueue<Command, kCapacity> mCommands;
    LLTransportSpscQueue<Completion, kCapacity> mCompletions;
    std::atomic<bool> mStopping{false};
    std::atomic<bool> mFailed{false};
    // Shared with ingress publication; every future ACK (including duplicate),
    // RX/control and send path must check before touching transport state.
    std::atomic<bool> mPoisoned{false};
    std::atomic<uint64_t> mOwnerMessageUsec{0};
    std::size_t mOutstanding = 0; // main thread only; includes undrained completions
    std::thread mThread;
};

#endif // LL_LLTRANSPORTSERVICE_H
