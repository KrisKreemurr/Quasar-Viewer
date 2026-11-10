/**
 * @file vk_allocation_limit.h
 * @brief Single-allocation limits and checked aggregate requirements (#1081).
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
#include <limits>

inline bool vkAllocationSizeFits(uint64_t bytes, uint64_t limit)
{
    return limit == 0 || bytes <= limit; // zero means the device limit is unknown
}

struct VkAllocationPlan
{
    uint64_t total = 0;
    bool fits = true;
    void add(uint64_t requirement, uint64_t limit)
    {
        fits = fits && requirement != 0 && vkAllocationSizeFits(requirement, limit);
        if (requirement > std::numeric_limits<uint64_t>::max() - total)
        {
            total = std::numeric_limits<uint64_t>::max();
            fits = false;
        }
        else total += requirement;
    }
};
