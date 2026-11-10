/**
 * @file slsounduuidlist.cpp
 * @brief Split sound uploads: the clips' asset UUIDs as clipboard text (see slsounduuidlist.h)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * (LGPL v2.1, see slsounduuidlist.h)
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "slsounduuidlist.h"

std::string SLSoundUUIDList::asLines(const std::vector<LLUUID>& ids)
{
    std::string out;
    for (size_t i = 0; i < ids.size(); ++i)
    {
        if (i) out += '\n';
        out += ids[i].asString();
    }
    return out;
}

std::string SLSoundUUIDList::asLslList(const std::vector<LLUUID>& ids,
                                       const std::string& clip_seconds, const std::string& last_clip_seconds)
{
    std::string out = "float clip_seconds = " + clip_seconds + ";\n";
    out += "float last_clip_seconds = " + last_clip_seconds + ";\n";
    out += "list sounds = [\n";
    for (size_t i = 0; i < ids.size(); ++i)
    {
        out += "    \"" + ids[i].asString() + "\"";
        out += (i + 1 < ids.size()) ? ",\n" : "\n";
    }
    out += "];\n";
    return out;
}
