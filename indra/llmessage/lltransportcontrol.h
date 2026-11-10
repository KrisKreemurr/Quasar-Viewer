/** Owner-only extraction of built-in control message values. SPDX-License-Identifier: LGPL-2.1-only */
#ifndef LL_LLTRANSPORTCONTROL_H
#define LL_LLTRANSPORTCONTROL_H
#include "lltransportdecodedpayload.h"
#include "lltransportcircuittypes.h"
#include <array>
#include <cstdint>
#include <cstring>

// Value-only warning for an internal control decoded with legacy zero-fill.
// The worker never invokes viewer callbacks or retains a template pointer.
struct LLTransportControlWarning
{
    LLTransportEndpoint endpoint{};
    uint64_t loginGeneration=0,incarnation=0,observation=0;
    uint64_t firstOffset=0,firstWanted=0;
    uint32_t messageNumber=0,packetId=0,truncations=0;
};

// Inputs are decoded against the matching immutable, already authorized
// template. This function never invokes viewer callbacks or emits ACKs. The
// caller retains diagnostics and enforces their ordering before handling.
struct LLTransportControlValues
{
    enum class Kind : uint8_t { Unknown, PacketAck, StartPing, CompletePing };
    Kind kind = Kind::Unknown;
    uint8_t pingId = 0;
    uint32_t oldestUnacked = 0;
    uint32_t ackCount = 0;
    std::array<uint32_t, 255> acknowledgements{};
};

inline bool llTransportControlField(const LLTransportDecodedView& view, uint32_t block,
                                     uint32_t field, void* output, uint32_t bytes) noexcept
{
    if (!output || !view.blocks || !view.fields || block >= view.blockCount) return false;
    const auto& b = view.blocks[block];
    if (b.schemaOrdinal != 0 || field >= b.fieldCount || b.firstField > view.fieldCount ||
        field >= view.fieldCount - b.firstField) return false;
    const auto& f = view.fields[b.firstField + field];
    if (f.schemaOrdinal != field || f.size != bytes || f.offset > view.byteCount || bytes > view.byteCount - f.offset ||
        (bytes && !view.data)) return false;
    std::memcpy(output, view.data + f.offset, bytes);
    return true;
}

inline bool llTransportDecodeControl(uint32_t messageNumber, const LLTransportDecodedView& view,
                                     LLTransportControlValues& output) noexcept
{
    LLTransportControlValues value;
    if (messageNumber == 1 || messageNumber == 2)
    {
        if (view.blockCount != 1 || !llTransportControlField(view, 0, 0, &value.pingId, 1)) return false;
        if (messageNumber == 1)
        {
            if (!llTransportControlField(view, 0, 1, &value.oldestUnacked, 4)) return false;
            value.kind = LLTransportControlValues::Kind::StartPing;
        }
        else value.kind = LLTransportControlValues::Kind::CompletePing;
    }
    else if (messageNumber == 0xfffffffbu)
    {
        if (view.blockCount > value.acknowledgements.size()) return false;
        value.kind = LLTransportControlValues::Kind::PacketAck;
        value.ackCount = view.blockCount;
        for (uint32_t i = 0; i < value.ackCount; ++i)
            if (!llTransportControlField(view, i, 0, &value.acknowledgements[i], 4)) return false;
    }
    else return false;
    output = value;
    return true;
}
#endif
