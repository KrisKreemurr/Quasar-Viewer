/**
 * @file slsnapshotbufferlimits_test.cpp
 * @brief Snapshot SSBO device-range and integer-boundary regressions; no Vulkan device.
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
#include "linden_common.h"
#include "../vkgfx/llsnapshotbufferlimits.h"
#include "../test/lltut.h"

#include <limits>

namespace tut
{
    struct snapshotbufferlimits_data {};
    typedef test_group<snapshotbufferlimits_data> snapshotbufferlimits_test;
    typedef snapshotbufferlimits_test::object snapshotbufferlimits_object;
    tut::snapshotbufferlimits_test snapshotbufferlimits("SLSnapshotBufferLimits");

    template<> template<>
    void snapshotbufferlimits_object::test<1>()
    {
        const uint64_t device_range = 128ull << 20;
        uint64_t bytes = 0;
        ensure("8192-square matte refuses a 128 MiB descriptor device",
               !LLSnapshotBufferLimits::imageBytes(8192, 8192, 4, device_range, bytes));
        ensure_equals("refusal clears bytes", bytes, uint64_t(0));
        ensure("4-byte copy at exact device limit",
               LLSnapshotBufferLimits::imageBytes(8192, 4096, 4, device_range, bytes));
        ensure_equals("exact range", bytes, device_range);
        ensure("one row beyond device limit",
               !LLSnapshotBufferLimits::imageBytes(8192, 4097, 4, device_range, bytes));
    }

    template<> template<>
    void snapshotbufferlimits_object::test<2>()
    {
        const uint64_t device_range = 128ull << 20;
        uint64_t bytes = 0;
        ensure("8-byte work/accumulator at limit",
               LLSnapshotBufferLimits::imageBytes(4096, 4096, 8, device_range, bytes));
        ensure_equals("work range", bytes, device_range);
        ensure("work can fail when the 4-byte source fits",
               !LLSnapshotBufferLimits::imageBytes(4096, 4097, 8, device_range, bytes));
        ensure("source still fits",
               LLSnapshotBufferLimits::imageBytes(4096, 4097, 4, device_range, bytes));
    }

    template<> template<>
    void snapshotbufferlimits_object::test<3>()
    {
        uint64_t bytes = 0;
        ensure("one pixel still needs a complete 12-byte packing group",
               !LLSnapshotBufferLimits::packedRgbBytes(1, 1, 11, bytes));
        ensure("one partial group fits",
               LLSnapshotBufferLimits::packedRgbBytes(1, 1, 12, bytes));
        ensure_equals("packed group", bytes, uint64_t(12));
        ensure("two complete groups fit",
               LLSnapshotBufferLimits::packedRgbBytes(8, 1, 24, bytes));
        ensure_equals("two groups", bytes, uint64_t(24));
        ensure("ninth pixel requires a third group",
               !LLSnapshotBufferLimits::packedRgbBytes(9, 1, 24, bytes));
        ensure("8192-square packed output exceeds 128 MiB",
               !LLSnapshotBufferLimits::packedRgbBytes(8192, 8192, 128ull << 20, bytes));
    }

    template<> template<>
    void snapshotbufferlimits_object::test<4>()
    {
        const uint32_t extent_limit = std::numeric_limits<uint32_t>::max();
        const uint64_t byte_limit = std::numeric_limits<uint64_t>::max();
        uint64_t bytes = 123;
        ensure("image multiplication cannot wrap to a small accepted range",
               !LLSnapshotBufferLimits::imageBytes(extent_limit, extent_limit, 4, byte_limit, bytes));
        ensure_equals("overflow clears bytes", bytes, uint64_t(0));
        ensure("packed multiplication cannot wrap",
               !LLSnapshotBufferLimits::packedRgbBytes(extent_limit, extent_limit, byte_limit, bytes));
        ensure("one-byte pixels fit the uint64 product",
               LLSnapshotBufferLimits::imageBytes(extent_limit, extent_limit, 1, byte_limit, bytes));
        ensure_equals("wide product preserved", bytes, uint64_t(extent_limit) * extent_limit);
        ensure("zero dimension rejected", !LLSnapshotBufferLimits::imageBytes(0, 1, 4, byte_limit, bytes));
        ensure("zero pixel stride rejected", !LLSnapshotBufferLimits::imageBytes(1, 1, 0, byte_limit, bytes));
        ensure("zero device range rejected", !LLSnapshotBufferLimits::imageBytes(1, 1, 4, 0, bytes));
        ensure("empty packed image rejected", !LLSnapshotBufferLimits::packedRgbBytes(1, 0, byte_limit, bytes));
    }
}
