/**
 * @file sluilanguage.h
 * @brief The UI language from the language settings (#1305): one rule for LLUI::getLanguage()
 *        and for the skin search path set before LLUI exists. Pure, so it is unit-tested
 *        (tests/sluilanguage_test.cpp).
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

#ifndef SL_SLUILANGUAGE_H
#define SL_SLUILANGUAGE_H

#include <string>

namespace SLUILanguage
{
    // The language code the UI uses, from the settings in order of authority: Language (the
    // Preferences choice), InstallLanguage (passed by an installer's shortcut), SystemLanguage
    // (the operating system's, set at startup). An empty value or "default" defers to the
    // next; "en" when none is set.
    std::string resolve(const std::string& language, const std::string& install_language,
                        const std::string& system_language);
}

#endif // SL_SLUILANGUAGE_H
