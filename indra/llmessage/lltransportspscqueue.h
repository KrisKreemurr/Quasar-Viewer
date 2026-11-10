/**
 * @file lltransportspscqueue.h
 * @brief Fixed-capacity single-producer/single-consumer transport handoff.
 *
 * Copyright (c) 2026, Slipstream Viewer Project
 * SPDX-License-Identifier: LGPL-2.1-only
 */
#ifndef LL_LLTRANSPORTSPSCQUEUE_H
#define LL_LLTRANSPORTSPSCQUEUE_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

// Exactly one producer and one consumer, each with at most one outstanding
// reservation. Slots are allocated with the queue; neither side waits or
// allocates. The consumer owns a published slot until releaseConsumer().
// Construct and destroy the queue only when both threads are stopped.
// T must own any payload inline; pointers to mutable or shorter-lived storage
// do not become safe merely by passing through this queue.
// This is a handoff primitive, not a reliable-packet ACK policy: the caller
// must reserve its dedup/ACK state before publishing a reliable message.
template <typename T, std::size_t Capacity>
class LLTransportSpscQueue
{
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "transport queue capacity must be a power of two");
    static_assert(std::is_nothrow_default_constructible<T>::value,
                  "transport queue slots must construct without throwing");
    static_assert(std::is_trivially_copyable<T>::value,
                  "transport queue slots must use bounded value storage");
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
                  "transport queue indices must be lock-free");

public:
    LLTransportSpscQueue() noexcept = default;
    LLTransportSpscQueue(const LLTransportSpscQueue&) = delete;
    LLTransportSpscQueue& operator=(const LLTransportSpscQueue&) = delete;

    // Producer only. A null result means full or already reserved.
    T* reserveProducer() noexcept
    {
        if (mProducerReserved) return nullptr;
        const uint64_t write = mWrite.load(std::memory_order_relaxed);
        const uint64_t read = mRead.load(std::memory_order_acquire);
        if (write - read >= Capacity) return nullptr;
        mProducerReserved = true;
        return &mSlots[write & (Capacity - 1)];
    }

    // Only fully constructed slot contents may be published. Commit itself
    // cannot allocate or throw, so it can be the last step of a transaction.
    bool publishProducer() noexcept
    {
        if (!mProducerReserved) return false;
        const uint64_t write = mWrite.load(std::memory_order_relaxed);
        mProducerReserved = false;
        mWrite.store(write + 1, std::memory_order_release);
        return true;
    }

    void cancelProducer() noexcept { mProducerReserved = false; }

    // Consumer only. The pointer stays valid until releaseConsumer().
    const T* peekConsumer() noexcept
    {
        const uint64_t read = mRead.load(std::memory_order_relaxed);
        if (read == mWrite.load(std::memory_order_acquire)) return nullptr;
        mConsumerReserved = true;
        return &mSlots[read & (Capacity - 1)];
    }

    bool releaseConsumer() noexcept
    {
        if (!mConsumerReserved) return false;
        const uint64_t read = mRead.load(std::memory_order_relaxed);
        mConsumerReserved = false;
        mRead.store(read + 1, std::memory_order_release);
        return true;
    }

    // Consumer only. A momentary published-queue observation, not proof that
    // the producer or socket has no work in flight. A held slot is pending.
    // Failure leaves out unchanged.
    bool pendingOnConsumer(bool& out) const noexcept
    {
        const auto read=mRead.load(std::memory_order_relaxed);
        const auto write=mWrite.load(std::memory_order_acquire);
        const auto count=write-read;
        if (count>Capacity) return false;
        out=count!=0;
        return true;
    }

    // Only after producer join and with the main consumer quiescent.
    bool emptyAfterJoinOnMain() const noexcept
    {
        return !mProducerReserved && !mConsumerReserved &&
            mRead.load(std::memory_order_acquire)==mWrite.load(std::memory_order_acquire);
    }

private:
    std::array<T, Capacity> mSlots{};
    std::atomic<uint64_t> mWrite{0};
    std::atomic<uint64_t> mRead{0};
    bool mProducerReserved = false; // touched only by producer
    bool mConsumerReserved = false; // touched only by consumer
};

#endif // LL_LLTRANSPORTSPSCQUEUE_H
