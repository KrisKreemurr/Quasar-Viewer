/**
 * @file slimchannelchat.h
 * @brief "/12 text" typed in an IM or group chat (#1291): which lines go out on a chat channel
 *        instead of into the conversation. No viewer state, so the rule is unit-tested
 *        (tests/slimchannelchat_test.cpp).
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

#ifndef SL_IMCHANNELCHAT_H
#define SL_IMCHANNELCHAT_H

#include <string>

namespace SLIMChannelChat
{
    // True when a line typed in an IM or group chat is meant for a chat channel: a slash, the
    // channel number (digits only), then a space or the end ("/12 text", "/12"). The caller
    // sends it the way the chat bar does (LLFloaterIMNearbyChat::sendChatFromViewer). Anything
    // else is a message for the conversation: "/12text", "/me", "//", "/-5 text", and channel 0
    // ("/0 text"), which would say a line typed in a private window out loud nearby.
    bool isChannelChat(const std::string& utf8_text);
}

#endif // SL_IMCHANNELCHAT_H
