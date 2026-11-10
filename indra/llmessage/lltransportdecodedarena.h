/**
 * @file lltransportdecodedarena.h
 * @brief Bounded SPSC storage for actual-sized decoded messages.
 *
 * Copyright (c) 2026, Slipstream Viewer Project
 * SPDX-License-Identifier: LGPL-2.1-only
 */
#ifndef LL_LLTRANSPORTDECODEDARENA_H
#define LL_LLTRANSPORTDECODEDARENA_H

#include "lltransportdecodedpayload.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>

// One producer (transport) and one consumer (main). A separate SPSC event queue
// carries Record values. Initialize before starting either side; destroy only
// after producer join AND clearing all main reader views. No per-message heap,
// mutex, callback or viewer object. Source views come from separate worker
// scratch, never this arena. The caller charges capacityBytes()+sizeof(arena),
// the actual event queue, scratch and schema to its startup memory budget.
class LLTransportDecodedArena
{
public:
    static constexpr std::size_t kMaximumCapacityBytes = 64u * 1024u * 1024u;
    static constexpr std::size_t kMaximumRecordBytes = 512u * 1024u;
    struct Record
    {
        uint64_t serial = 0;
        uint64_t end = 0;
        uint64_t begin = 0; // includes wrap padding owned by this record
        uint32_t offset = 0;
        uint32_t byteCount = 0;
        uint32_t blockCount = 0;
        uint32_t fieldCount = 0;
        bool operator==(const Record&) const = default;
    };
    enum class Result { Ready, Full, Busy, Invalid, Exhausted };

    LLTransportDecodedArena() noexcept = default;
    LLTransportDecodedArena(const LLTransportDecodedArena&) = delete;
    LLTransportDecodedArena& operator=(const LLTransportDecodedArena&) = delete;

    // Same alignment and header calculation as tryCopy. Startup uses this
    // upper bound for each schema before acknowledging any decoded message.
    static uint64_t requiredRecordBytes(uint32_t bytes, uint32_t blocks, uint32_t fields) noexcept
    { return makeLayout(bytes, blocks, fields).total; }

    // Exact requested byte storage; allocator bookkeeping is not included.
    // Reinitialization is refused, so it cannot invalidate a live borrowed view.
    bool initialize(std::size_t capacity, std::size_t maximumRecordBytes) noexcept
    {
        if (mStorage || capacity > kMaximumCapacityBytes
            || capacity % kAlignment || maximumRecordBytes < kHeaderBytes
            || maximumRecordBytes > kMaximumRecordBytes
            || maximumRecordBytes > capacity / 2) return false;
        auto storage = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[capacity]);
        if (!storage) return false;
        mStorage = std::move(storage);
        mCapacity = capacity;
        mMaximumRecordBytes = maximumRecordBytes;
        return true;
    }

    // Producer only. Reserve an event slot as well before committing circuit
    // dedup/ACK state. This copies just the populated arrays and bytes. Failure
    // preserves out and cursor state; a Ready reservation must be commit/cancel'd.
    Result tryCopy(const LLTransportDecodedView& source, Record& out) noexcept
    {
        if (mReserved) return Result::Busy;
        if (!mStorage || (source.byteCount && !source.data)
            || (source.blockCount && !source.blocks) || (source.fieldCount && !source.fields))
            return Result::Invalid;
        const Layout layout = makeLayout(source.byteCount, source.blockCount, source.fieldCount);
        if (layout.total > mMaximumRecordBytes) return Result::Invalid;
        if (!mNextSerial) return Result::Exhausted;
        const uint64_t read = mRead.load(std::memory_order_acquire);
        if (read > mWrite || mWrite - read > mCapacity) return Result::Invalid;
        const uint64_t position = mWrite % mCapacity;
        const uint64_t padding = layout.total > mCapacity - position ? mCapacity - position : 0;
        const uint64_t needed = padding + layout.total;
        if (needed > mCapacity - (mWrite - read)) return Result::Full;
        if (mWrite > (std::numeric_limits<uint64_t>::max)() - needed) return Result::Exhausted;
        const auto offset = static_cast<uint32_t>((position + padding) % mCapacity);
        uint8_t* dest = mStorage.get() + offset;
        mPending = Record{mNextSerial, mWrite + needed, mWrite, offset,
                          source.byteCount, source.blockCount, source.fieldCount};
        // Even a wrapped record has an initialized identity at begin. A bad or
        // canceled descriptor must not direct the consumer into wrap padding.
        const Prefix prefix{mPending.serial, mPending.end};
        std::memcpy(mStorage.get() + position, &prefix, sizeof(prefix));
        std::memcpy(dest, &mPending, sizeof(mPending));
        if (source.byteCount) std::memcpy(dest + kHeaderBytes, source.data, source.byteCount);
        if (source.blockCount) std::memcpy(dest + layout.blocks, source.blocks,
            static_cast<std::size_t>(source.blockCount) * sizeof(LLTransportDecodedBlock));
        if (source.fieldCount) std::memcpy(dest + layout.fields, source.fields,
            static_cast<std::size_t>(source.fieldCount) * sizeof(LLTransportDecodedField));
        mNextSerial = mNextSerial == (std::numeric_limits<uint64_t>::max)() ? 0 : mNextSerial + 1;
        mReserved = true;
        out = mPending;
        return Result::Ready;
    }

    // Producer only. After all fallible admission work succeeds, commit then
    // release-publish the event. The actor must not service ACKs between circuit
    // commit and these guaranteed, nonallocating publication steps.
    bool commit(const Record& record) noexcept
    {
        if (!mReserved || record != mPending) return false;
        mWrite = record.end;
        mReserved = false;
        mPublishedEnd.store(mWrite, std::memory_order_release);
        return true;
    }
    bool cancel(const Record& record) noexcept
    {
        if (!mReserved || record != mPending) return false;
        mReserved = false;
        return true;
    }

    // Consumer only, FIFO. Acquire the event before calling view. Its pointers
    // stay valid until release; clearMessage MUST discard LLMsgData first, not
    // merely return from the handler. Failure preserves out.
    bool view(const Record& record, LLTransportDecodedView& out) const noexcept
    {
        if (!mStorage || !record.serial || record.begin != mRead.load(std::memory_order_relaxed)
            || record.end <= record.begin || record.end - record.begin > mCapacity
            || record.end > mPublishedEnd.load(std::memory_order_acquire)) return false;
        const uint64_t position = record.begin % mCapacity;
        Prefix prefix{};
        std::memcpy(&prefix, mStorage.get() + position, sizeof(prefix));
        if (prefix.serial != record.serial || prefix.end != record.end) return false;
        const Layout layout = makeLayout(record.byteCount, record.blockCount, record.fieldCount);
        if (layout.total > mMaximumRecordBytes) return false;
        const uint64_t padding = layout.total > mCapacity - position ? mCapacity - position : 0;
        if (record.end - record.begin != padding + layout.total
            || record.offset != (position + padding) % mCapacity) return false;
        const uint8_t* data = mStorage.get() + record.offset;
        Record stored{};
        std::memcpy(&stored, data, sizeof(stored));
        if (stored != record) return false;
        out = LLTransportDecodedView{data + kHeaderBytes,
            reinterpret_cast<const LLTransportDecodedBlock*>(data + layout.blocks),
            reinterpret_cast<const LLTransportDecodedField*>(data + layout.fields),
            record.byteCount, record.blockCount, record.fieldCount};
        return true;
    }
    bool release(const Record& record) noexcept
    {
        LLTransportDecodedView ignored;
        if (!view(record, ignored)) return false;
        // The producer cannot reuse these bytes until its acquire sees this.
        mRead.store(record.end, std::memory_order_release);
        return true;
    }

    // Caller has joined the producer and cleared every borrowed reader.
    bool emptyAfterJoinOnMain() const noexcept
    { return !mReserved && mWrite==mRead.load(std::memory_order_acquire); }

    std::size_t capacityBytes() const noexcept { return mCapacity; }
    // Producer only, includes committed wrap padding, not a pending reservation.
    uint64_t producerBytesInUse() const noexcept
    { return mWrite - mRead.load(std::memory_order_acquire); }

private:
    static constexpr uint64_t kAlignment = 16;
    static constexpr uint64_t kHeaderBytes = (sizeof(Record) + kAlignment - 1) & ~(kAlignment - 1);
    struct Prefix { uint64_t serial; uint64_t end; };
    static_assert(sizeof(Prefix) == kAlignment);
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    static_assert(alignof(LLTransportDecodedBlock) <= kAlignment);
    static_assert(alignof(LLTransportDecodedField) <= kAlignment);
    static_assert(std::is_trivially_copyable_v<LLTransportDecodedBlock>);
    static_assert(std::is_trivially_copyable_v<LLTransportDecodedField>);
    struct Layout { uint64_t blocks, fields, total; };
    static uint64_t align(uint64_t value, uint64_t alignment) noexcept
    { return (value + alignment - 1) & ~(alignment - 1); }
    static Layout makeLayout(uint32_t bytes, uint32_t blocks, uint32_t fields) noexcept
    {
        // Three u32 counts times these fixed struct sizes fit in u64; no
        // unchecked narrowing happens until total is admitted below capacity.
        const uint64_t blockOffset = align(kHeaderBytes + bytes, alignof(LLTransportDecodedBlock));
        const uint64_t fieldOffset = align(blockOffset + uint64_t{blocks} * sizeof(LLTransportDecodedBlock),
                                           alignof(LLTransportDecodedField));
        const uint64_t used = fieldOffset + uint64_t{fields} * sizeof(LLTransportDecodedField);
        return Layout{blockOffset, fieldOffset, align(used, kAlignment)};
    }

    std::unique_ptr<uint8_t[]> mStorage;
    std::size_t mCapacity = 0;
    std::size_t mMaximumRecordBytes = 0;
    uint64_t mWrite = 0; // producer only
    uint64_t mNextSerial = 1; // producer only, never reused after wrap
    Record mPending{};
    bool mReserved = false; // producer only
    std::atomic<uint64_t> mPublishedEnd{0};
    mutable std::atomic<uint64_t> mRead{0}; // consumer stores, producer acquires
};

#endif // LL_LLTRANSPORTDECODEDARENA_H
