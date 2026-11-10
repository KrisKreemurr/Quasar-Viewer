/**
 * @file sluilanguage_test.cpp
 * @brief #1305: which UI language the Language, InstallLanguage and SystemLanguage settings
 *        give: an explicit choice wins, "default" or empty defers, "en" when nothing is set.
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

#include "../sluilanguage.h"

#include "../test/lltut.h"

namespace tut
{
    struct uilanguage_data {};
    typedef test_group<uilanguage_data> uilanguage_test;
    typedef uilanguage_test::object uilanguage_object;
    tut::uilanguage_test uilanguage("SLUILanguage");

    // An explicit choice wins over the installer's and the system's language.
    template<> template<>
    void uilanguage_object::test<1>()
    {
        ensure_equals("English on a German system", SLUILanguage::resolve("en", "default", "de"), std::string("en"));
        ensure_equals("English over a German install", SLUILanguage::resolve("en", "de", "de"), std::string("en"));
        ensure_equals("Japanese over both", SLUILanguage::resolve("ja", "de", "fr"), std::string("ja"));
    }

    // System default ("default", or never set) takes the installer's language first.
    template<> template<>
    void uilanguage_object::test<2>()
    {
        ensure_equals("default", SLUILanguage::resolve("default", "de", "fr"), std::string("de"));
        ensure_equals("empty", SLUILanguage::resolve("", "de", "fr"), std::string("de"));
    }

    // With no installer language either, the system's.
    template<> template<>
    void uilanguage_object::test<3>()
    {
        ensure_equals("default install", SLUILanguage::resolve("default", "default", "fr"), std::string("fr"));
        ensure_equals("empty install", SLUILanguage::resolve("default", "", "fr"), std::string("fr"));
        ensure_equals("all empty but the system", SLUILanguage::resolve("", "", "pl"), std::string("pl"));
    }

    // Nothing set anywhere: English.
    template<> template<>
    void uilanguage_object::test<4>()
    {
        ensure_equals("all default", SLUILanguage::resolve("default", "default", "default"), std::string("en"));
        ensure_equals("all empty", SLUILanguage::resolve("", "", ""), std::string("en"));
    }
}
