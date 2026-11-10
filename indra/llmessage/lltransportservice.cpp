/**
 * @file lltransportservice.cpp
 * @brief Bounded transport-owner service, first transmit-only slice.
 *
 * Copyright (c) 2026, Slipstream Viewer Project
 * SPDX-License-Identifier: LGPL-2.1-only
 */
#include "linden_common.h"
#include "lltransportservice.h"

#include <chrono>
#include <cstring>
#include <new>
#include <system_error>
#include <limits>
#if LL_WINDOWS
#include <winsock2.h>
#else
#include <cerrno>
#endif

LLTransportService::ClockAnchor LLTransportService::captureClockOnMain(uint64_t mainMessageUsec) noexcept
{
    return {mainMessageUsec, std::chrono::steady_clock::now()};
}

LLTransportService::LLTransportService(int socket, const LLPacketRing::UDPProxySnapshot& proxy, ClockAnchor anchor)
    : mSocket(socket), mProxy(proxy), mClock(anchor), mOwnerMessageUsec(anchor.mainMessageUsec)
{
}

std::unique_ptr<LLTransportService> LLTransportService::start(int socket,
    const LLPacketRing::UDPProxySnapshot& proxy, uint32_t dropSeed, ClockAnchor anchor, StartResult& result) noexcept
{
    if (socket < 0) { result = StartResult::InvalidSocket; return {}; }
    try
    {
        std::unique_ptr<LLTransportService> service(new LLTransportService(socket, proxy, anchor));
        if (!service->mRing.prepareOwnerDropRandom(dropSeed))
        {
            result = StartResult::AllocationFailed;
            return {};
        }
        try { service->mThread = std::thread(&LLTransportService::run, service.get()); }
        catch (const std::system_error&) { result = StartResult::ThreadFailed; return {}; }
        result = StartResult::Started;
        return service;
    }
    catch (...) { result = StartResult::AllocationFailed; return {}; }
}

LLTransportService::~LLTransportService()
{
    // Destruction must occur on main, never from the owner thread.
    stopAndJoin();
}

LLTransportService::OfferResult LLTransportService::trySend(uint64_t operationId,
    uint32_t addressNetworkOrder, uint16_t portHostOrder, const uint8_t* bytes, uint32_t size) noexcept
{
    if (mStopping.load() || mFailed.load() || mPoisoned.load()) return OfferResult::Stopped;
    // LL packet flags: reliable 0x40, resent 0x20, appended ACK 0x10.
    if (!operationId || !addressNetworkOrder || !portHostOrder || !bytes || size < 7
        || size > NET_BUFFER_SIZE || (bytes[0] & 0x70u)) return OfferResult::Invalid;
    // Each admitted command owns a completion slot until main consumes it.
    if (mOutstanding >= kCapacity) return OfferResult::Full;
    Command* command = mCommands.reserveProducer();
    if (!command) return OfferResult::Full;
    command->operationId = operationId;
    command->addressNetworkOrder = addressNetworkOrder;
    command->portHostOrder = portHostOrder;
    command->size = size;
    std::memcpy(command->bytes.data(), bytes, size);
    if (!mCommands.publishProducer()) { mCommands.cancelProducer(); return OfferResult::Stopped; }
    ++mOutstanding;
    return OfferResult::Accepted;
}

bool LLTransportService::tryPopCompletion(Completion& result) noexcept
{
    const Completion* completion = mCompletions.peekConsumer();
    if (!completion)
    {
        // A terminal internal failure leaves any unprocessed commands owned by
        // their bounded queue. Once the worker is joined, main may return them
        // as failures rather than silently discarding accepted operations.
        if (!mFailed.load() || mThread.joinable()) return false;
        const Command* command = mCommands.peekConsumer();
        if (!command) return false;
        result = {command->operationId, SendResult::Failed};
        if (!mCommands.releaseConsumer()) return false;
        if (mOutstanding) --mOutstanding;
        return true;
    }
    result = *completion;
    if (!mCompletions.releaseConsumer()) return false;
    if (mOutstanding) --mOutstanding;
    return true;
}

void LLTransportService::requestStop() noexcept { mStopping.store(true); }

void LLTransportService::stopAndJoin()
{
    requestStop();
    if (mThread.joinable()) mThread.join();
}

uint64_t LLTransportService::ownerLegacyMessageUsec() const noexcept
{
    const auto anchorTicks = std::chrono::duration_cast<std::chrono::microseconds>(
        mClock.steadyTime.time_since_epoch()).count();
    const LLTransportSteadyUsec anchor(anchorTicks > 0 ? static_cast<uint64_t>(anchorTicks) : 0);
    return llTransportLegacyAt(LLTransportLegacyUsec(mClock.mainMessageUsec), anchor, llTransportSteadyNow()).value;
}

void LLTransportService::run() noexcept
{
    try
    {
        unsigned attempts = 0; // owner-only; reset when the current datagram resolves
        for (;;)
        {
            if (mPoisoned.load()) { mFailed.store(true); return; }
            mOwnerMessageUsec.store(ownerLegacyMessageUsec());
            const Command* command = mCommands.peekConsumer();
            if (!command)
            {
                if (!mStopping.load())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                // The producer can publish after the first empty observation
                // but before its stop store. Re-read after observing stop so an
                // already accepted command cannot be stranded during shutdown.
                command = mCommands.peekConsumer();
                if (!command) return;
            }
            NetSendResult sent{NetSendStatus::Failed, 0};
            try
            {
                const LLHost destination(command->addressNetworkOrder, command->portHostOrder);
                sent = mRing.sendPacketOnce(mSocket, reinterpret_cast<const char*>(command->bytes.data()),
                                            (S32)command->size, destination, mProxy, attempts == 0);
            }
            catch (...) { sent = {NetSendStatus::Failed, 0}; }
            ++attempts;

            bool retry = false;
            bool success = sent.status == NetSendStatus::Sent;
#if LL_WINDOWS
            retry = sent.status == NetSendStatus::WouldBlock;
            // The old Windows wrapper treats this ICMP indication as success.
            if (sent.status == NetSendStatus::Refused && sent.osError == WSAECONNRESET) success = true;
#else
            // Preserve the POSIX wrapper's bounded retryable-error set and its
            // return convention (the third attempt reports failure even if sent).
            retry = (sent.osError == EAGAIN || sent.osError == ECONNREFUSED) && attempts < 3;
            if (attempts >= 3) success = false;
#endif
            if (retry && !mStopping.load())
            {
                // Keep the owned command queued. Future owner receive/control
                // service runs at this boundary before another socket attempt.
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            Completion* completion = mCompletions.reserveProducer();
            if (!completion)
            {
                mFailed.store(true); // invariant failure; command remains owned
                return;
            }
            completion->operationId = command->operationId;
            completion->result = success ? SendResult::Sent : SendResult::Failed;
            if (!mCompletions.publishProducer() || !mCommands.releaseConsumer())
            {
                mFailed.store(true);
                return;
            }
            attempts = 0;
        }
    }
    catch (...)
    {
        mCompletions.cancelProducer();
        mFailed.store(true);
    }
}
