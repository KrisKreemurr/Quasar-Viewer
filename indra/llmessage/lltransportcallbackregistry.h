/**
 * @file lltransportcallbackregistry.h
 * @brief Bounded, main-thread reliable-send callback ownership.
 *
 * Copyright (c) 2026, Slipstream Viewer Project
 * SPDX-License-Identifier: LGPL-2.1-only
 */
#ifndef LL_LLTRANSPORTCALLBACKREGISTRY_H
#define LL_LLTRANSPORTCALLBACKREGISTRY_H

#include <array>
#include <cstddef>
#include <cstdint>

// Only main may access this registry. The transport receives Token values, never
// callback/user-data pointers. Reserve this slot AND transport completion space
// before accepting a reliable operation. A reservation is not a successful send.
// Entries borrow the original callback data: its existing owner must keep it
// alive until rejection or terminal completion, and destroy it by the original
// callback contract. This class neither dereferences nor deletes callback data.
template <std::size_t Capacity>
class LLTransportCallbackRegistry
{
    static_assert(Capacity > 0 && Capacity <= 65536,
                  "callback registry uses a 16-bit slot index");

public:
    using Token = uint64_t;
    using Callback = void (*)(void**, int32_t);

    struct Context
    {
        uint64_t loginGeneration = 0;
        uint64_t circuitIncarnation = 0;
        uint32_t addressNetworkOrder = 0;
        uint16_t portHostOrder = 0;
    };

    struct Record
    {
        Callback callback = nullptr;
        void** userData = nullptr;
        Context context{};
    };

    LLTransportCallbackRegistry() noexcept
    {
        for (std::size_t i = 0; i < Capacity; ++i)
            mFreeSlots[i] = static_cast<uint16_t>(Capacity - 1 - i);
    }
    LLTransportCallbackRegistry(const LLTransportCallbackRegistry&) = delete;
    LLTransportCallbackRegistry& operator=(const LLTransportCallbackRegistry&) = delete;

    // Zero means full or lifetime generation exhaustion. Do not reuse tokens
    // after generation wrap: a very late completion must never match a new send.
    Token reserve(Callback callback, void** userData, Context context) noexcept
    {
        if (!mFreeCount || !mNextGeneration) return 0;
        const uint16_t index = mFreeSlots[--mFreeCount];
        const Token token = (mNextGeneration << 16) | index;
        mNextGeneration = mNextGeneration == kLastGeneration ? 0 : mNextGeneration + 1;
        mSlots[index] = Slot{Record{callback, userData, context}, token, false};
        return token;
    }

    // Called on main after enqueue admission. No callback may be delivered
    // synchronously from the producer/transport path before this returns.
    bool markAccepted(Token token) noexcept
    {
        Slot* slot = find(token);
        if (!slot || slot->accepted) return false;
        slot->accepted = true;
        return true;
    }

    // Failed enqueue returns ownership to the original caller without inventing
    // a callback, timeout or success. An accepted operation cannot use this path.
    bool releaseUnaccepted(Token token) noexcept
    {
        Slot* slot = find(token);
        if (!slot || slot->accepted) return false;
        release(token);
        return true;
    }

    // Main consumes a terminal event and removes the entry BEFORE invoking its
    // callback. A callback can reenter messaging and reserve the same slot safely.
    // The caller validates the event's login/circuit context and chooses the
    // existing terminal status; teardown is CIRCUIT_GONE, not a fabricated ACK.
    // Returned context remains available for stale-login cleanup. Never discard
    // accepted callback data just because the current login has changed.
    // False (including duplicate/stale token) leaves out unchanged.
    bool takeAccepted(Token token, const Context& expected, Record& out) noexcept
    {
        Slot* slot = find(token);
        if (!slot || !slot->accepted || !sameContext(slot->record.context, expected)) return false;
        out = slot->record;
        release(token);
        return true;
    }

    std::size_t size() const noexcept { return Capacity - mFreeCount; }
    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    static bool sameContext(const Context& a, const Context& b) noexcept
    {
        return a.loginGeneration == b.loginGeneration
            && a.circuitIncarnation == b.circuitIncarnation
            && a.addressNetworkOrder == b.addressNetworkOrder
            && a.portHostOrder == b.portHostOrder;
    }

    struct Slot
    {
        Record record{};
        Token token = 0;
        bool accepted = false;
    };
    static constexpr uint64_t kLastGeneration = (uint64_t{1} << 48) - 1;

    Slot* find(Token token) noexcept
    {
        if (!token) return nullptr;
        const std::size_t index = token & 0xffffu;
        if (index >= Capacity || mSlots[index].token != token) return nullptr;
        return &mSlots[index];
    }

    void release(Token token) noexcept
    {
        const std::size_t index = token & 0xffffu;
        mSlots[index] = Slot{};
        mFreeSlots[mFreeCount++] = static_cast<uint16_t>(index);
    }

    std::array<Slot, Capacity> mSlots{};
    std::array<uint16_t, Capacity> mFreeSlots{};
    std::size_t mFreeCount = Capacity;
    uint64_t mNextGeneration = 1;
};

#endif // LL_LLTRANSPORTCALLBACKREGISTRY_H
