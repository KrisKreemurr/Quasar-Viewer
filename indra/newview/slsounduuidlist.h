/**
 * @file slsounduuidlist.h
 * @brief Split sound uploads (#807, #1313): the clips' asset UUIDs as clipboard text. No
 *        viewer state, so the formats are unit-tested (tests/slsounduuidlist_test.cpp).
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

#ifndef SL_SOUNDUUIDLIST_H
#define SL_SOUNDUUIDLIST_H

#include "lluuid.h"

#include <string>
#include <vector>

// Both take the UUIDs in clip order.
namespace SLSoundUUIDList
{
    // One UUID per line and nothing else, what notecards and sound players' web forms take.
    // No newline after the last one, so a paste does not add an empty line.
    std::string asLines(const std::vector<LLUUID>& ids);

    // A ready-made LSL list for scripts that play the clips back to back. Every clip is
    // clip_seconds long except the last one; both as the upload dialogs show them ("29.9").
    std::string asLslList(const std::vector<LLUUID>& ids,
                          const std::string& clip_seconds, const std::string& last_clip_seconds);
}

#endif // SL_SOUNDUUIDLIST_H
