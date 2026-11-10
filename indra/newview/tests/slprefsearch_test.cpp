/**
 * @file slprefsearch_test.cpp
 * @brief Preferences search (#1348): words in any order, white space, an empty search, and
 *        which accordion section opens.
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

#include "../slprefsearch.h"

#include "../test/lltut.h"

namespace
{
    bool matches(const char* text, const char* filter)
    {
        return sl_search_matches(utf8str_to_wstring(text), utf8str_to_wstring(filter));
    }

    bool matchesControl(const char* text, const char* setting, const char* filter)
    {
        return sl_search_matches_control(utf8str_to_wstring(text), utf8str_to_wstring(setting), utf8str_to_wstring(filter));
    }
}

namespace tut
{
    struct slprefsearch_data {};
    typedef test_group<slprefsearch_data> slprefsearch_group;
    typedef slprefsearch_group::object slprefsearch_object;
    tut::slprefsearch_group slprefsearch_tests("slprefsearch");

    // One word: anywhere in the text, as part of a longer word too
    template<> template<>
    void slprefsearch_object::test<1>()
    {
        ensure("whole word", matches("play typing sound", "typing"));
        ensure("part of a word", matches("play typing sound", "typ"));
        ensure("not there", !matches("play typing sound", "typo"));
    }

    // Several words: each must be there, in any order
    template<> template<>
    void slprefsearch_object::test<2>()
    {
        ensure("reversed", matches("play typing sound", "sound typing"));
        ensure("in order", matches("play typing sound", "typing sound"));
        ensure("one missing", !matches("play typing sound", "sound typing loud"));
        ensure("a word twice", matches("play typing sound", "sound sound"));
    }

    // Extra white space between, before and after words, tabs included
    template<> template<>
    void slprefsearch_object::test<3>()
    {
        ensure("spaces", matches("play typing sound", "  sound    typing  "));
        ensure("tab", matches("play typing sound", "sound\ttyping"));
    }

    // A search with no words matches everything
    template<> template<>
    void slprefsearch_object::test<4>()
    {
        ensure("empty", matches("play typing sound", ""));
        ensure("only spaces", matches("play typing sound", "   "));
        ensure("empty text, empty search", matches("", ""));
        ensure("empty text", !matches("", "sound"));
    }

    // Label and tool tip are joined by a line break: a word can come from either, but no word
    // spans both
    template<> template<>
    void slprefsearch_object::test<5>()
    {
        const char* text = "play typing sound\nwhen people type";
        ensure("label and tool tip", matches(text, "people typing"));
        ensure("across the join", !matches(text, "soundwhen"));
    }

    // Non-ASCII text
    template<> template<>
    void slprefsearch_object::test<6>()
    {
        ensure("umlaut", matches("schriftgröße", "größe"));
        ensure("cjk", matches("音量 設定", "設定"));
    }

    // The section to open: the open one while it has a match, else the first that has one
    template<> template<>
    void slprefsearch_object::test<7>()
    {
        ensure_equals("open one matches", sl_section_to_open({ true, false, true }, 2), 2);
        ensure_equals("open one doesn't", sl_section_to_open({ false, true, true }, 0), 1);
        ensure_equals("none open", sl_section_to_open({ false, false, true }, -1), 2);
    }

    // No section matches: leave it as it is
    template<> template<>
    void slprefsearch_object::test<8>()
    {
        ensure_equals("one open", sl_section_to_open({ false, false }, 1), 1);
        ensure_equals("none open", sl_section_to_open({ false, false }, -1), -1);
        ensure_equals("no sections", sl_section_to_open({}, -1), -1);
        ensure_equals("open index past the end", sl_section_to_open({ false, true }, 5), 1);
    }

    // A word of three characters or fewer must start a word of the text: "sit" finds Sit/Stand
    // and sitting, not position or visit
    template<> template<>
    void slprefsearch_object::test<9>()
    {
        ensure("at the start", matches("sit/stand", "sit"));
        ensure("start of a later word", matches("allow sitting", "sit"));
        ensure("inside a word", !matches("camera position", "sit"));
        ensure("end of a word", !matches("visit", "sit"));
        ensure("after punctuation", matches("sit/stand", "sta"));
        ensure("after a bracket", matches("view (fov)", "fov"));
        ensure("after a digit is inside a word", !matches("262144mb", "mb"));
    }

    // Longer words match anywhere, so a part of a compound word is found
    template<> template<>
    void slprefsearch_object::test<10>()
    {
        ensure("inside a word", matches("camera position", "sition"));
        ensure("compound", matches("schriftgröße", "größe"));
    }

    // Scripts written without spaces have no word starts to look for: short words match anywhere
    template<> template<>
    void slprefsearch_object::test<11>()
    {
        ensure("cjk short word", matches("音量設定", "設定"));
        ensure("kana", matches("チャットの音", "音"));
    }

    // A setting name counts only when it is the whole search, so ordinary words don't light up
    // every control whose setting happens to contain them
    template<> template<>
    void slprefsearch_object::test<12>()
    {
        ensure("whole name", matchesControl("draw distance", "renderfarclip", "renderfarclip"));
        ensure("part of the name", !matchesControl("draw distance", "renderfarclip", "render"));
        ensure("name and another word", !matchesControl("draw distance", "renderfarclip", "renderfarclip draw"));
        ensure("label still counts", matchesControl("draw distance", "renderfarclip", "draw"));
        ensure("no setting", !matchesControl("draw distance", "", "renderfarclip"));
        ensure("no setting, empty search", matchesControl("draw distance", "", ""));
    }
}
