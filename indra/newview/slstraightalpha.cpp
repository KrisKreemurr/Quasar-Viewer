/**
 * @file slstraightalpha.cpp
 * @brief Straight-alpha 8-bit RGBA (#1328). See slstraightalpha.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "slstraightalpha.h"

void slPremultiplyRGBA(U8* rgba, size_t pixels)
{
    for (size_t i = 0; i < pixels; ++i, rgba += 4)
    {
        const U32 a = rgba[3];
        if (a == 255u) continue;
        for (int k = 0; k < 3; ++k) rgba[k] = (U8)((rgba[k] * a + 127u) / 255u);
    }
}

void slUnpremultiplyRGBA(U8* rgba, size_t pixels)
{
    for (size_t i = 0; i < pixels; ++i, rgba += 4)
    {
        const U32 a = rgba[3];
        if (a == 255u) continue;
        if (a == 0u)
        {
            rgba[0] = rgba[1] = rgba[2] = 0;   // the floor: nothing here, no stray colour
            continue;
        }
        for (int k = 0; k < 3; ++k)
        {
            const U32 c = (rgba[k] * 255u + a / 2u) / a;
            rgba[k] = (U8)(c < 255u ? c : 255u);
        }
    }
}
