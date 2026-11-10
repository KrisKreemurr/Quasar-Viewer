/** Copyright (c) 2026 Slipstream Viewer Project. SPDX-License-Identifier: LGPL-2.1-only */
#include "lltransportdecoder.h"

bool LLTransportSchema::capacityForWireBytes(uint64_t maxWireBytes, LLTransportCapacity& out) const noexcept
{
    if (!mFrozen) return false;
    LLTransportCapacity cap;
    cap.bytes = maxWireBytes;
    auto add = [](uint64_t& target, uint64_t value) noexcept
    { if (value > UINT64_MAX - target) return false; target += value; return true; };
    for (uint32_t i=0; i<mBlockCount; ++i)
    {
        const auto& b=mBlocks[i];
        const uint64_t repeats = b.repeat == LLTransportBlockSchema::Repeat::Variable ? 255 :
            b.repeat == LLTransportBlockSchema::Repeat::Single ? 1 : b.count;
        if (!add(cap.blocks,repeats) || !add(cap.fields,repeats * b.fieldCount)) return false;
        for (uint32_t j=0; j<b.fieldCount; ++j)
        {
            const auto& field=mFields[b.firstField+j];
            if (!field.lengthBytes && !add(cap.bytes,repeats * field.size)) return false;
        }
    }
    out=cap;
    return true;
}
