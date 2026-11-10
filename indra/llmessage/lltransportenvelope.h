/** Bounded owner-side UDP envelope parsing. Copyright (c) 2026 Slipstream Viewer Project.
 * SPDX-License-Identifier: LGPL-2.1-only */
#ifndef LL_LLTRANSPORTENVELOPE_H
#define LL_LLTRANSPORTENVELOPE_H

#include <cstddef>
#include <cstdint>
#include <cstring>

// Input and output buffers must not overlap. This handles only the packet
// envelope, not schema/trust validation or reliable admission/ACK policy.
// On failure neither appended ACKs nor packet data may be acted upon.
struct LLTransportEnvelope
{
    enum class Status : uint8_t { Complete, TooShort, MalformedAck, MalformedZeroCode, CapacityExceeded, Invalid };
    Status status = Status::Invalid;
    uint32_t packetId = 0;
    uint32_t messageNumber = 0;
    std::size_t bodyOffset = 0;
    std::size_t expandedSize = 0;
    std::size_t wireMessageSize = 0;
    uint8_t ackCount = 0;
    bool reliable = false;
    bool resent = false;
    bool compressed = false;
};

inline uint32_t llTransportReadNetworkU32(const uint8_t* p) noexcept
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// Legacy checkMessages processes appended ACKs in reverse wire order. The
// caller can retrieve each only after Complete and independently validate the
// current circuit incarnation before applying it.
inline bool llTransportAppendedAck(const uint8_t* wire, std::size_t size,
                                   const LLTransportEnvelope& envelope, uint8_t index,
                                   uint32_t& packetId) noexcept
{
    if (!wire || envelope.status != LLTransportEnvelope::Status::Complete ||
        !(wire[0] & 0x10u) || index >= envelope.ackCount || size < 1 ||
        std::size_t(envelope.ackCount) * 4 > size - 1) return false;
    packetId = llTransportReadNetworkU32(wire + size - 1 - (std::size_t(index) + 1) * 4);
    return true;
}

inline LLTransportEnvelope llTransportParseEnvelope(const uint8_t* wire, std::size_t size,
                                                     uint8_t* out, std::size_t capacity) noexcept
{
    LLTransportEnvelope r;
    if (!wire || !out) return r;
    if (size < 7) { r.status = LLTransportEnvelope::Status::TooShort; return r; }
    std::size_t messageSize = size;
    if (wire[0] & 0x10u)
    {
        r.ackCount = wire[size - 1];
        --messageSize;
        const std::size_t ackBytes = std::size_t(r.ackCount) * 4;
        if (messageSize < 7 || ackBytes > messageSize - 7)
        { r.status = LLTransportEnvelope::Status::MalformedAck; return r; }
        messageSize -= ackBytes;
    }
    r.wireMessageSize = messageSize;
    r.packetId = llTransportReadNetworkU32(wire + 1);
    r.reliable = (wire[0] & 0x40u) != 0;
    r.resent = (wire[0] & 0x20u) != 0;
    r.compressed = (wire[0] & 0x80u) != 0;
    if (capacity < 6) { r.status = LLTransportEnvelope::Status::CapacityExceeded; return r; }
    std::memcpy(out, wire, 6);
    if (r.compressed) out[0] &= uint8_t(~0x80u);
    std::size_t in = 6, used = 6;
    while (in < messageSize)
    {
        const uint8_t byte = wire[in++];
        if (!r.compressed || byte != 0)
        {
            if (used == capacity) { r.status = LLTransportEnvelope::Status::CapacityExceeded; return r; }
            out[used++] = byte;
            continue;
        }
        // Each 0,0 pair adds 256 zero bytes; final nonzero count adds that
        // many more. A missing terminal count is a malformed compressed packet.
        std::size_t zeros = 0;
        while (in < messageSize && wire[in] == 0)
        {
            if (zeros > capacity || capacity - zeros < 256)
            { r.status = LLTransportEnvelope::Status::CapacityExceeded; return r; }
            zeros += 256;
            ++in;
        }
        if (in == messageSize) { r.status = LLTransportEnvelope::Status::MalformedZeroCode; return r; }
        const std::size_t tail = wire[in++];
        if (zeros > capacity - used || tail > capacity - used - zeros)
        { r.status = LLTransportEnvelope::Status::CapacityExceeded; return r; }
        zeros += tail;
        std::memset(out + used, 0, zeros);
        used += zeros;
    }
    if (used < 7) { r.status = LLTransportEnvelope::Status::TooShort; return r; }
    std::size_t frequency = 1;
    r.messageNumber = out[6];
    if (out[6] == 0xff)
    {
        if (used < 8) { r.status = LLTransportEnvelope::Status::TooShort; return r; }
        frequency = 2;
        r.messageNumber = 0xff00u | out[7];
        if (out[7] == 0xff)
        {
            if (used < 10) { r.status = LLTransportEnvelope::Status::TooShort; return r; }
            frequency = 4;
            r.messageNumber = 0xffff0000u | (uint32_t(out[8]) << 8) | out[9];
        }
    }
    r.bodyOffset = 6 + frequency + out[5];
    r.expandedSize = used;
    r.status = LLTransportEnvelope::Status::Complete;
    return r;
}
#endif
