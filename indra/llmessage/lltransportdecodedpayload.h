/**
 * @file lltransportdecodedpayload.h
 * @brief Bounded, owning decoded-field storage for transport handoff.
 *
 * Copyright (c) 2026, Slipstream Viewer Project
 * SPDX-License-Identifier: LGPL-2.1-only
 */
#ifndef LL_LLTRANSPORTDECODEDPAYLOAD_H
#define LL_LLTRANSPORTDECODEDPAYLOAD_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

struct LLTransportDecodedBlock
{
    uint32_t schemaOrdinal = 0;
    uint32_t repeatOrdinal = 0;
    uint32_t firstField = 0;
    uint32_t fieldCount = 0;
};

struct LLTransportDecodedField
{
    uint32_t schemaOrdinal = 0;
    uint32_t offset = 0;
    uint32_t size = 0;
};

// A main-thread reader may borrow this view only while the queue consumer
// retains the owning slot. The view itself must never outlive that slot.
struct LLTransportDecodedView
{
    const uint8_t* data = nullptr;
    const LLTransportDecodedBlock* blocks = nullptr;
    const LLTransportDecodedField* fields = nullptr;
    uint32_t byteCount = 0;
    uint32_t blockCount = 0;
    uint32_t fieldCount = 0;
};

// Ordinals refer to a separately owned immutable schema for the lifetime of a
// queued message. The parser supplies decoded field bytes, including any
// protocol-required zero filling; no field borrows the socket receive buffer.
// A failed operation leaves all counters and existing fields unchanged. The
// caller must reject an incomplete message without committing a reliable ACK.
template <std::size_t Bytes, std::size_t Blocks, std::size_t Fields>
struct LLTransportDecodedPayload
{
    static_assert(Bytes <= (std::numeric_limits<uint32_t>::max)(), "byte arena too large");
    static_assert(Blocks <= (std::numeric_limits<uint32_t>::max)(), "block arena too large");
    static_assert(Fields <= (std::numeric_limits<uint32_t>::max)(), "field arena too large");

    using Block = LLTransportDecodedBlock;
    using Field = LLTransportDecodedField;

    std::array<uint8_t, Bytes> data{};
    std::array<Block, Blocks> blocks{};
    std::array<Field, Fields> fields{};
    uint32_t byteCount = 0;
    uint32_t blockCount = 0;
    uint32_t fieldCount = 0;

    void reset() noexcept { byteCount = blockCount = fieldCount = 0; }

    LLTransportDecodedView view() const noexcept
    {
        return {data.data(), blocks.data(), fields.data(), byteCount, blockCount, fieldCount};
    }

    bool appendBlock(uint32_t schemaOrdinal, uint32_t repeatOrdinal) noexcept
    {
        if (blockCount >= Blocks) return false;
        blocks[blockCount++] = Block{schemaOrdinal, repeatOrdinal, fieldCount, 0};
        return true;
    }

    bool appendField(uint32_t schemaOrdinal, const void* source, uint32_t size) noexcept
    {
        if (!blockCount || blockCount > Blocks || fieldCount >= Fields || byteCount > Bytes
            || size > Bytes - byteCount || (size && !source)) return false;
        if (size) std::memcpy(data.data() + byteCount, source, size);
        fields[fieldCount++] = Field{schemaOrdinal, byteCount, size};
        byteCount += size;
        ++blocks[blockCount - 1].fieldCount;
        return true;
    }

    // The legacy parser reports a truncated fixed field and supplies zeros.
    // This writes them directly into the bounded owning arena without a temp.
    bool appendZeroField(uint32_t schemaOrdinal, uint32_t size) noexcept
    {
        if (!blockCount || blockCount > Blocks || fieldCount >= Fields || byteCount > Bytes
            || size > Bytes - byteCount) return false;
        if (size) std::memset(data.data() + byteCount, 0, size);
        fields[fieldCount++] = Field{schemaOrdinal, byteCount, size};
        byteCount += size;
        ++blocks[blockCount - 1].fieldCount;
        return true;
    }
};

#endif // LL_LLTRANSPORTDECODEDPAYLOAD_H
