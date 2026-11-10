/**
 * @file llsnapshotbufferlimits.h
 * @brief Checked byte ranges for full-image snapshot storage buffers.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */
#pragma once

#include <cstdint>

namespace LLSnapshotBufferLimits
{
// maxStorageBufferRange is a per-descriptor bound, independent of available VRAM.
// Divide before multiplying: even UINT32_MAX squared times four can overflow uint64_t.
inline bool imageBytes(uint32_t width, uint32_t height, uint32_t bytes_per_pixel,
                       uint64_t range_limit, uint64_t& bytes)
{
    bytes = 0;
    const uint64_t pixels = uint64_t(width) * height;
    if (!pixels || !bytes_per_pixel || pixels > range_limit / bytes_per_pixel) return false;
    bytes = pixels * bytes_per_pixel;
    return true;
}

// clean_snap_live.comp packs four RGB pixels into three words, including the final partial group.
inline bool packedRgbBytes(uint32_t width, uint32_t height, uint64_t range_limit, uint64_t& bytes)
{
    bytes = 0;
    const uint64_t pixels = uint64_t(width) * height;
    const uint64_t groups = pixels / 4 + (pixels % 4 != 0);
    if (!groups || groups > range_limit / 12) return false;
    bytes = groups * 12;
    return true;
}

inline bool packedRgbeBytes(uint32_t width, uint32_t height, uint64_t range_limit, uint64_t& bytes)
{
    return imageBytes(width, height, 4, range_limit, bytes);
}

inline bool snapshotPeakSystemBytes(uint32_t outW, uint32_t outH, uint32_t bytesPerOutputPixel,
                                    uint32_t outputCopies, uint32_t sourceW, uint32_t sourceH,
                                    uint32_t bytesPerSourcePixel, uint64_t& bytes)
{
    bytes = 0;
    constexpr uint64_t max = ~uint64_t(0);
    const uint64_t outPixels = uint64_t(outW) * outH;
    const uint64_t srcPixels = uint64_t(sourceW) * sourceH;
    if (!outPixels || !srcPixels || !bytesPerOutputPixel || !bytesPerSourcePixel
        || outPixels > max / bytesPerOutputPixel || srcPixels > max / bytesPerSourcePixel)
        return false;
    const uint64_t outBytes = outPixels * bytesPerOutputPixel;
    const uint64_t srcBytes = srcPixels * bytesPerSourcePixel;
    if (outputCopies && outBytes > (max - srcBytes) / outputCopies) return false;
    bytes = outBytes * outputCopies + srcBytes;
    return true;
}
} // namespace LLSnapshotBufferLimits
