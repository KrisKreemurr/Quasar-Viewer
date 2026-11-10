/**
 * @file slsettingsmigration_test.cpp
 * @brief Settings migration (#1263): the shared new-IM sound carried over to the per-type IM
 *        sounds once, without overriding a sound already chosen.
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

#include "../slsettingsmigration.h"

#include "llcontrol.h"

#include "../test/lltut.h"

namespace
{
    const char* const DEFAULT_SOUND = "67cc2844-00f3-2b3c-b991-6418d01e1bb7";
    const char* const CUSTOM_SOUND = "11111111-2222-3333-4444-555555555555";
    const char* const PER_TYPE[] = { "UISndNewIncomingGroupIMSession", "UISndNewIncomingConfIMSession",
                                     "UISndNearbyChat", "UISndObjectIM" };

    // A settings group with the IM sounds at their defaults and the marker unset
    void declare(LLControlGroup& group)
    {
        group.declareString("UISndNewIncomingIMSession", DEFAULT_SOUND, "");
        for (const char* name : PER_TYPE)
        {
            group.declareString(name, DEFAULT_SOUND, "");
        }
        group.declareBOOL("SharedIMSoundMigrated", false, "");
    }
}

namespace tut
{
    struct slsettingsmigration_data {};
    typedef test_group<slsettingsmigration_data> slsettingsmigration_group;
    typedef slsettingsmigration_group::object slsettingsmigration_object;
    tut::slsettingsmigration_group slsettingsmigration_tests("slsettingsmigration");

    // A chosen shared sound carries over to every per-type sound, and the marker is set
    template<> template<>
    void slsettingsmigration_object::test<1>()
    {
        LLControlGroup group("slsm_test_1");
        declare(group);
        group.setString("UISndNewIncomingIMSession", CUSTOM_SOUND);

        SLSettingsMigration::migrateSharedIMSound(group);

        for (const char* name : PER_TYPE)
        {
            ensure_equals(name, group.getString(name), std::string(CUSTOM_SOUND));
        }
        ensure("marked", group.getBOOL("SharedIMSoundMigrated"));
    }

    // The shared sound at its default: nothing to carry over
    template<> template<>
    void slsettingsmigration_object::test<2>()
    {
        LLControlGroup group("slsm_test_2");
        declare(group);

        SLSettingsMigration::migrateSharedIMSound(group);

        for (const char* name : PER_TYPE)
        {
            ensure_equals(name, group.getString(name), std::string(DEFAULT_SOUND));
        }
        ensure("marked", group.getBOOL("SharedIMSoundMigrated"));
    }

    // A per-type sound already chosen is kept; the others take the shared one
    template<> template<>
    void slsettingsmigration_object::test<3>()
    {
        LLControlGroup group("slsm_test_3");
        declare(group);
        group.setString("UISndNewIncomingIMSession", CUSTOM_SOUND);
        group.setString("UISndNearbyChat", "");

        SLSettingsMigration::migrateSharedIMSound(group);

        ensure_equals("chosen kept", group.getString("UISndNearbyChat"), std::string());
        ensure_equals("group", group.getString("UISndNewIncomingGroupIMSession"), std::string(CUSTOM_SOUND));
        ensure_equals("object", group.getString("UISndObjectIM"), std::string(CUSTOM_SOUND));
    }

    // Only once: after the marker, a changed shared sound stays its own
    template<> template<>
    void slsettingsmigration_object::test<4>()
    {
        LLControlGroup group("slsm_test_4");
        declare(group);
        group.setBOOL("SharedIMSoundMigrated", true);
        group.setString("UISndNewIncomingIMSession", CUSTOM_SOUND);

        SLSettingsMigration::migrateSharedIMSound(group);

        for (const char* name : PER_TYPE)
        {
            ensure_equals(name, group.getString(name), std::string(DEFAULT_SOUND));
        }
    }
}
