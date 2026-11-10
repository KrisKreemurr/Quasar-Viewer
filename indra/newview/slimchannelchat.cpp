/**
 * @file slimchannelchat.cpp
 * @brief "/12 text" typed in an IM or group chat (see slimchannelchat.h)
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

#include "slimchannelchat.h"

#include "llstring.h"

bool SLIMChannelChat::isChannelChat(const std::string& utf8_text)
{
    if (utf8_text.empty() || utf8_text[0] != '/')
    {
        return false;
    }

    // At most the ten digits of the largest channel, so the number the chat bar's channel
    // parser reads (LLFloaterIMNearbyChat::stripChannelNumber) is exactly this one: it can't
    // wrap or be cut short into channel 0.
    constexpr size_t MAX_DIGITS = 10;
    U64 channel = 0;
    size_t pos = 1;
    while (pos < utf8_text.size() && LLStringOps::isDigit(utf8_text[pos]))
    {
        if (pos > MAX_DIGITS)
        {
            return false;
        }
        channel = channel * 10 + (utf8_text[pos] - '0');
        ++pos;
    }
    if (pos == 1 || channel == 0 || channel > (U64)S32_MAX)
    {
        return false;
    }
    return pos == utf8_text.size() || LLStringOps::isSpace(utf8_text[pos]);
}
