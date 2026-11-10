/** Bounded command admission with nonblocking owner close. SPDX-License-Identifier: LGPL-2.1-only */
#ifndef LL_LLTRANSPORTCOMMANDQUEUE_H
#define LL_LLTRANSPORTCOMMANDQUEUE_H
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

// One main producer and one owner consumer. The caller reserves all completion
// credits before reserveProducer and releases them if publication fails. A
// command becomes accepted only when publishProducer's CAS succeeds.
// closeOnConsumer and publishProducer linearize on the SAME atomic word: a
// published command is in the prefix the owner drains, while an unpublished
// reservation is refused without the owner waiting for a paused producer.
// Main must beginOfferOnMain BEFORE reserving credits and endOfferOnMain only
// after publication or complete rollback and its last graph access. Teardown
// may destroy the graph only after owner join AND !offerActiveOnMain().
template<class T, std::size_t Capacity>
class LLTransportCommandQueue
{
    static_assert(Capacity > 0 && (Capacity & (Capacity-1)) == 0);
    static_assert(std::is_nothrow_default_constructible_v<T> && std::is_trivially_copyable_v<T>);
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    static constexpr uint64_t Closed = uint64_t{1} << 63;
    static constexpr uint64_t InFlight = uint64_t{1} << 62;
    static constexpr uint64_t CursorMask = InFlight - 1;
public:
    LLTransportCommandQueue() noexcept = default;
    LLTransportCommandQueue(const LLTransportCommandQueue&) = delete;
    LLTransportCommandQueue& operator=(const LLTransportCommandQueue&) = delete;

    // Main producer only. This outer lifetime guard covers credit reservation
    // and rollback as well as slot publication; owner close never waits on it.
    bool beginOfferOnMain() noexcept
    {
        bool expected=false;
        if (!mOfferActive.compare_exchange_strong(expected,true,std::memory_order_acq_rel)) return false;
        if (isClosed()) { mOfferActive.store(false,std::memory_order_release); return false; }
        return true;
    }
    void endOfferOnMain() noexcept
    {
        cancelProducer();
        mOfferActive.store(false,std::memory_order_release);
    }
    bool offerActiveOnMain() const noexcept { return mOfferActive.load(std::memory_order_acquire); }

    // Producer only, inside the outer offer guard. nullptr means closed, full
    // or another slot reservation exists.
    T* reserveProducer() noexcept
    {
        if (!mOfferActive.load(std::memory_order_acquire) || mProducerReserved) return nullptr;
        uint64_t state=mState.load(std::memory_order_acquire);
        if (state & (Closed | InFlight)) return nullptr;
        const uint64_t write=state & CursorMask;
        const uint64_t read=mRead.load(std::memory_order_acquire);
        if (write==CursorMask || read>write)
        { mState.fetch_or(Closed,std::memory_order_acq_rel); return nullptr; }
        if (write-read>=Capacity) return nullptr;
        if (!mState.compare_exchange_strong(state,write | InFlight,
                std::memory_order_acq_rel,std::memory_order_acquire)) return nullptr;
        mProducerWrite=write;
        mProducerReserved=true;
        return &mSlots[write & (Capacity-1)];
    }

    // Producer only. False is not an accepted command: the caller must release
    // its previously reserved terminal credit and any other unaccepted state.
    bool publishProducer() noexcept
    {
        if (!mProducerReserved) return false;
        uint64_t expected=mProducerWrite | InFlight;
        const bool published=mState.compare_exchange_strong(expected,mProducerWrite+1,
            std::memory_order_acq_rel,std::memory_order_acquire);
        mProducerReserved=false;
        if (!published)
        {
            // A close may race the producer while it is filling the slot.
            // Unexpected state also fails closed; never publish partial data.
            mState.fetch_or(Closed,std::memory_order_acq_rel);
            mState.fetch_and(~InFlight,std::memory_order_release);
        }
        return published;
    }
    void cancelProducer() noexcept
    {
        if (!mProducerReserved) return;
        mProducerReserved=false;
        mState.fetch_and(~InFlight,std::memory_order_release);
    }

    // Consumer only. No producer wait, even if main is suspended mid-offer.
    void closeOnConsumer() noexcept { mState.fetch_or(Closed,std::memory_order_acq_rel); }
    bool isClosed() const noexcept { return (mState.load(std::memory_order_acquire) & Closed)!=0; }
    // This reports the slot reservation only. It does NOT cover the outer
    // credit-reservation/rollback lifetime and is not a destruction fence.
    bool producerInFlight() const noexcept { return (mState.load(std::memory_order_acquire) & InFlight)!=0; }

    const T* peekConsumer() noexcept
    {
        const auto read=mRead.load(std::memory_order_relaxed);
        const auto write=mState.load(std::memory_order_acquire) & CursorMask;
        if (read==write) return nullptr;
        mConsumerReserved=true;
        return &mSlots[read & (Capacity-1)];
    }
    bool releaseConsumer() noexcept
    {
        if (!mConsumerReserved) return false;
        const auto read=mRead.load(std::memory_order_relaxed);
        mConsumerReserved=false;
        mRead.store(read+1,std::memory_order_release);
        return true;
    }
    // Owner joined and no main offer may run while this is inspected.
    bool emptyAfterJoinOnMain() const noexcept
    {
        const auto state=mState.load(std::memory_order_acquire);
        return !mOfferActive.load(std::memory_order_acquire) && !mProducerReserved &&
            !mConsumerReserved && !(state & InFlight) &&
            mRead.load(std::memory_order_acquire)==(state & CursorMask);
    }
#if defined(LL_TRANSPORT_COMMAND_QUEUE_TESTING)
    // Owner and producer stopped, queue empty: deterministic exhaustion check.
    void testSetEmptyCursor(uint64_t value) noexcept
    { mRead.store(value); mState.store(value); }
#endif
private:
    std::array<T,Capacity> mSlots{};
    std::atomic<uint64_t> mState{0},mRead{0};
    std::atomic<bool> mOfferActive{false};
    uint64_t mProducerWrite=0; // producer only
    bool mProducerReserved=false; // producer only
    bool mConsumerReserved=false; // consumer only
};
#endif
