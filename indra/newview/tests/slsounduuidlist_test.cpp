/**
 * @file slsounduuidlist_test.cpp
 * @brief Split sound uploads (#1313): the clips' UUIDs copied one per line, and the LSL list.
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

#include "../slsounduuidlist.h"

#include "../test/lltut.h"

namespace
{
    const std::vector<LLUUID> CLIPS = {
        LLUUID("0f4e1a2b-3c4d-4e5f-8a6b-7c8d9e0f1a2b"),
        LLUUID("a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d"),
        LLUUID("5d4c3b2a-1f0e-4d9c-8b7a-6f5e4d3c2b1a"),
    };
}

namespace tut
{
    struct sounduuidlist_data {};
    typedef test_group<sounduuidlist_data> sounduuidlist_test;
    typedef sounduuidlist_test::object sounduuidlist_object;
    tut::sounduuidlist_test sounduuidlist("SLSoundUUIDList");

    // The default copy: one UUID per line in clip order; no quotes, commas or variables, and
    // no empty line after the last one.
    template<> template<>
    void sounduuidlist_object::test<1>()
    {
        ensure_equals("one per line, in order", SLSoundUUIDList::asLines(CLIPS),
                      std::string("0f4e1a2b-3c4d-4e5f-8a6b-7c8d9e0f1a2b\n"
                                  "a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d\n"
                                  "5d4c3b2a-1f0e-4d9c-8b7a-6f5e4d3c2b1a"));
        ensure_equals("a single clip is just its UUID", SLSoundUUIDList::asLines({ CLIPS[1] }),
                      std::string("a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d"));
        ensure_equals("no clips, no text", SLSoundUUIDList::asLines({}), std::string());
    }

    // "Copy as LSL list": the clip lengths, then the UUIDs as a quoted, comma-separated list.
    template<> template<>
    void sounduuidlist_object::test<2>()
    {
        ensure_equals("LSL source", SLSoundUUIDList::asLslList(CLIPS, "29.9", "1.94"),
                      std::string("float clip_seconds = 29.9;\n"
                                  "float last_clip_seconds = 1.94;\n"
                                  "list sounds = [\n"
                                  "    \"0f4e1a2b-3c4d-4e5f-8a6b-7c8d9e0f1a2b\",\n"
                                  "    \"a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d\",\n"
                                  "    \"5d4c3b2a-1f0e-4d9c-8b7a-6f5e4d3c2b1a\"\n"
                                  "];\n"));
    }
}
