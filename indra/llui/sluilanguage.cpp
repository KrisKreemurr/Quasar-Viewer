/**
 * @file sluilanguage.cpp
 * @brief The UI language from the language settings (see sluilanguage.h)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * (LGPL v2.1, see sluilanguage.h)
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "sluilanguage.h"

namespace
{
    bool isSet(const std::string& language)
    {
        return !language.empty() && language != "default";
    }
}

std::string SLUILanguage::resolve(const std::string& language, const std::string& install_language,
                                  const std::string& system_language)
{
    if (isSet(language))
    {
        return language;
    }
    if (isSet(install_language))
    {
        return install_language;
    }
    if (isSet(system_language))
    {
        return system_language;
    }
    return "en";
}
