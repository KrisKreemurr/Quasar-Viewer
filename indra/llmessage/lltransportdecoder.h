/** Pure bounded wire-field decode. Copyright (c) 2026 Slipstream Viewer Project.
 * SPDX-License-Identifier: LGPL-2.1-only */
#ifndef LL_LLTRANSPORTDECODER_H
#define LL_LLTRANSPORTDECODER_H

#include "lltransportdecodedpayload.h"
#include "lltransportspscqueue.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

class LLMessageTemplate;
class LLTransportSchema;
enum class LLTransportFreezeStatus : uint8_t { Complete, InvalidSchema, CapacityExceeded, AllocationFailed };

struct LLTransportFieldSchema
{
    uint32_t size = 0;
    uint8_t lengthBytes = 0; // 0 fixed; otherwise little-endian length prefix
};
struct LLTransportBlockSchema
{
    enum class Repeat : uint8_t { Single, Multiple, Variable };
    Repeat repeat = Repeat::Single;
    uint8_t count = 1;
    uint32_t firstField = 0;
    uint32_t fieldCount = 0;
};
struct LLTransportCapacity { uint64_t blocks = 0, fields = 0, bytes = 0; };

// One admission ledger for a queue and all schemas pinned alongside it.
// Include every other retained startup allocation in reservedOtherBytes. The
// limits cover requested object/array storage, not allocator bookkeeping. The
// owner must instantiate this for the actual queue and retain the ledger until
// all of its schema versions drain. Schema charges are monotonic until then.
class LLTransportStartupBudget
{
public:
    LLTransportStartupBudget() = default;
    LLTransportStartupBudget(const LLTransportStartupBudget&) = delete;
    LLTransportStartupBudget& operator=(const LLTransportStartupBudget&) = delete;
    template <std::size_t Bytes, std::size_t Blocks, std::size_t Fields, std::size_t Cells>
    static bool initialize(LLTransportStartupBudget& out, uint64_t maxWireBytes,
                           uint64_t maxSchemaBytes, uint64_t maxTotalBytes,
                           uint64_t reservedOtherBytes = 0) noexcept
    {
        using Payload = LLTransportDecodedPayload<Bytes, Blocks, Fields>;
        using Queue = LLTransportSpscQueue<Payload, Cells>;
        if (out.mInitialized || Bytes > UINT32_MAX || Blocks > UINT32_MAX || Fields > UINT32_MAX ||
            maxWireBytes > UINT32_MAX || reservedOtherBytes > maxTotalBytes ||
            sizeof(Queue) > maxTotalBytes - reservedOtherBytes) return false;
        out.mQueueBytes = sizeof(Queue); out.mOtherBytes = reservedOtherBytes;
        out.mMaxTotal = maxTotalBytes; out.mMaxSchema = maxSchemaBytes;
        out.mMaxWire = maxWireBytes; out.mBytes = Bytes; out.mBlocks = Blocks; out.mFields = Fields;
        out.mCells = Cells; out.mInitialized = true;
        return true;
    }
    uint64_t schemaBytesUsed() const noexcept { return mSchemaUsed; }
    uint64_t queueBytes() const noexcept { return mQueueBytes; }
    uint64_t queueCells() const noexcept { return mCells; }
private:
    friend LLTransportFreezeStatus freezeTransportSchema(const LLMessageTemplate&, uint64_t,
                                                          LLTransportStartupBudget&, LLTransportSchema&) noexcept;
    bool mInitialized = false;
    uint64_t mQueueBytes=0, mOtherBytes=0, mMaxTotal=0, mMaxSchema=0, mSchemaUsed=0;
    uint64_t mMaxWire=0, mBytes=0, mBlocks=0, mFields=0, mCells=0;
};

// Value-only arrays. Publish by const reference and retain through every queued
// payload; generation must change whenever the main-owned template is replaced.
class LLTransportSchema
{
public:
    LLTransportSchema() = default;
    LLTransportSchema(LLTransportSchema&& other) noexcept { *this = std::move(other); }
    LLTransportSchema& operator=(LLTransportSchema&& other) noexcept
    {
        if (this != &other)
        {
            mFrozen=std::exchange(other.mFrozen,false);
            mMessageNumber=other.mMessageNumber; mGeneration=other.mGeneration;
            mBlockCount=std::exchange(other.mBlockCount,0);
            mFieldCount=std::exchange(other.mFieldCount,0);
            mBlocks=std::move(other.mBlocks); mFields=std::move(other.mFields);
        }
        return *this;
    }
    LLTransportSchema(const LLTransportSchema&) = delete;
    LLTransportSchema& operator=(const LLTransportSchema&) = delete;
    bool isFrozen() const noexcept { return mFrozen; }
    uint32_t messageNumber() const noexcept { return mMessageNumber; }
    uint64_t generation() const noexcept { return mGeneration; }
    uint32_t blockCount() const noexcept { return mBlockCount; }
    const LLTransportBlockSchema& block(uint32_t n) const noexcept { return mBlocks[n]; }
    const LLTransportFieldSchema& field(uint32_t n) const noexcept { return mFields[n]; }
    bool capacityForWireBytes(uint64_t maxWireBytes, LLTransportCapacity& out) const noexcept;
private:
    friend LLTransportFreezeStatus freezeTransportSchema(const LLMessageTemplate&, uint64_t,
                                                          LLTransportStartupBudget&, LLTransportSchema&) noexcept;
    bool mFrozen=false;
    uint32_t mMessageNumber=0, mBlockCount=0, mFieldCount=0;
    uint64_t mGeneration=0;
    std::unique_ptr<LLTransportBlockSchema[]> mBlocks;
    std::unique_ptr<LLTransportFieldSchema[]> mFields;
};

// Main-thread only. Preflights all schema and queue limits before either array
// allocation. Failure changes neither result nor the cumulative budget.
LLTransportFreezeStatus freezeTransportSchema(const LLMessageTemplate& source, uint64_t generation,
                                               LLTransportStartupBudget& budget, LLTransportSchema& result) noexcept;

enum class LLTransportDecodeStatus : uint8_t { Complete, EmptyMessage, CapacityExceeded, InvalidInput };
struct LLTransportDecodeResult
{
    LLTransportDecodeStatus status = LLTransportDecodeStatus::InvalidInput;
    uint64_t endOffset = 0;
    uint64_t firstTruncationOffset = 0;
    uint64_t firstTruncationWanted = 0;
    uint32_t truncations = 0; // saturates; value diagnostics, not exact log replay
};

// Packet is already validated, expanded and matched to this immutable schema.
// bodyOffset is legacy LL_PACKET_ID_SIZE + frequency + PHL_OFFSET. This does
// not validate trust, packet identity or ACK, dispatch, log, or invoke a callback.
// On any failure the owning payload is reset and must not be handed off.
template <std::size_t Bytes, std::size_t Blocks, std::size_t Fields>
LLTransportDecodeResult decodeTransportFields(const LLTransportSchema& schema,
    const uint8_t* packet, std::size_t packetSize, uint64_t bodyOffset,
    LLTransportDecodedPayload<Bytes, Blocks, Fields>& out) noexcept
{
    LLTransportDecodeResult r;
    out.reset();
    if (!schema.isFrozen() || (!packet && packetSize) || packetSize > UINT32_MAX || bodyOffset > UINT32_MAX) return r;
    uint64_t pos = bodyOffset;
    auto truncated = [&](uint64_t at, uint64_t wanted) noexcept
    {
        if (!r.truncations) { r.firstTruncationOffset = at; r.firstTruncationWanted = wanted; }
        if (r.truncations != UINT32_MAX) ++r.truncations;
    };
    auto available = [&](uint64_t n) noexcept { return pos <= packetSize && n <= packetSize - pos; };
    auto fail = [&]() noexcept { out.reset(); r.status = LLTransportDecodeStatus::CapacityExceeded; r.endOffset = pos; return r; };
    for (uint32_t blockOrdinal=0; blockOrdinal<schema.blockCount(); ++blockOrdinal)
    {
        const LLTransportBlockSchema& block = schema.block(blockOrdinal);
        uint32_t repeat = block.repeat == LLTransportBlockSchema::Repeat::Single ? 1 : block.count;
        if (block.repeat == LLTransportBlockSchema::Repeat::Variable)
            repeat = available(1) ? packet[pos++] : 0; // absent count is legal legacy zero
        for (uint32_t i = 0; i < repeat; ++i)
        {
            if (!out.appendBlock(blockOrdinal, i)) return fail();
            for (uint32_t fieldOrdinal=0; fieldOrdinal<block.fieldCount; ++fieldOrdinal)
            {
                const LLTransportFieldSchema& field = schema.field(block.firstField + fieldOrdinal);
                if (field.lengthBytes)
                {
                    uint32_t length = 0;
                    if (available(field.lengthBytes))
                        for (uint32_t n = 0; n < field.lengthBytes; ++n)
                            length |= uint32_t(packet[pos + n]) << (8 * n);
                    else truncated(pos, field.lengthBytes);
                    pos += field.lengthBytes;
                    if (!available(length) && length)
                    { truncated(pos, length); pos = packetSize; length = 0; }
                    const uint8_t* data = length ? packet + pos : nullptr;
                    if (!out.appendField(fieldOrdinal, data, length)) return fail();
                    pos += length;
                }
                else
                {
                    if (!available(field.size))
                    {
                        truncated(pos, field.size);
                        if (!out.appendZeroField(fieldOrdinal, field.size)) return fail();
                    }
                    else if (!out.appendField(fieldOrdinal, field.size ? packet + pos : nullptr, field.size)) return fail();
                    pos += field.size;
                }
            }
        }
    }
    r.endOffset = pos;
    r.status = !out.blockCount && schema.blockCount() ? LLTransportDecodeStatus::EmptyMessage : LLTransportDecodeStatus::Complete;
    if (r.status != LLTransportDecodeStatus::Complete) out.reset();
    return r;
}
#endif
