/**
 * @file slimchannelchat_test.cpp
 * @brief "/12 text" typed in an IM or group chat (#1291): which lines go to a chat channel.
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

#include "../slimchannelchat.h"

#include "../test/lltut.h"

namespace tut
{
    struct imchannelchat_data {};
    typedef test_group<imchannelchat_data> imchannelchat_test;
    typedef imchannelchat_test::object imchannelchat_object;
    tut::imchannelchat_test imchannelchat("SLIMChannelChat");

    // A slash, the channel number, then a space or the end: chat on that channel.
    template<> template<>
    void imchannelchat_object::test<1>()
    {
        ensure("/12 text", SLIMChannelChat::isChannelChat("/12 some text"));
        ensure("just the channel", SLIMChannelChat::isChannelChat("/12"));
        ensure("one digit", SLIMChannelChat::isChannelChat("/7 hi"));
        ensure("a newline after the number", SLIMChannelChat::isChannelChat("/12\nline two"));
        ensure("a tab after the number", SLIMChannelChat::isChannelChat("/12\ttext"));
        ensure("leading zeros", SLIMChannelChat::isChannelChat("/0012 text"));
        ensure("the largest channel", SLIMChannelChat::isChannelChat("/2147483647 text"));
    }

    // Anything else is a message for the conversation.
    template<> template<>
    void imchannelchat_object::test<2>()
    {
        ensure("plain text", !SLIMChannelChat::isChannelChat("some text"));
        ensure("empty", !SLIMChannelChat::isChannelChat(""));
        ensure("a lone slash", !SLIMChannelChat::isChannelChat("/"));
        ensure("an emote", !SLIMChannelChat::isChannelChat("/me waves"));
        ensure("letters right after the number", !SLIMChannelChat::isChannelChat("/12text"));
        ensure("a number and a dot", !SLIMChannelChat::isChannelChat("/1.5 hours"));
        ensure("a fraction", !SLIMChannelChat::isChannelChat("/2/3 done"));
        ensure("a repeat-channel //", !SLIMChannelChat::isChannelChat("//text"));
        ensure("a negative channel", !SLIMChannelChat::isChannelChat("/-5 text"));
        ensure("a space before the number", !SLIMChannelChat::isChannelChat("/ 12 text"));
        ensure("not at the start", !SLIMChannelChat::isChannelChat("see /12 text"));
    }

    // Never public chat: channel 0, and numbers the channel parser would wrap or cut short.
    template<> template<>
    void imchannelchat_object::test<3>()
    {
        ensure("/0 text", !SLIMChannelChat::isChannelChat("/0 text"));
        ensure("/0", !SLIMChannelChat::isChannelChat("/0"));
        ensure("/000 text", !SLIMChannelChat::isChannelChat("/000 text"));
        ensure("past the largest channel", !SLIMChannelChat::isChannelChat("/2147483648 text"));
        ensure("wraps to 0 as 32 bits", !SLIMChannelChat::isChannelChat("/4294967296 text"));
        ensure("more than ten digits", !SLIMChannelChat::isChannelChat("/00000000012 text"));
    }
}
