/**
 * @file slprefsearch.h
 * @brief Preferences search (#1348): whether a control matches what was typed, and which
 *        section of a one-open-at-a-time accordion to open while searching. No viewer objects
 *        here, so it is unit-tested (tests/slprefsearch_test.cpp).
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

#ifndef SL_SLPREFSEARCH_H
#define SL_SLPREFSEARCH_H

#include "llstring.h"

#include <vector>

// Whether every word of `filter` (split on white space) is in `text`, in any order. A word of
// three characters or fewer must start a word of the text (after a space or punctuation), so
// "sit" finds "Sit/Stand" but not "position"; longer words match anywhere, and so do words in
// scripts written without spaces. Both are lower case already. A filter with no words matches
// everything.
bool sl_search_matches(const LLWString& text, const LLWString& filter);

// A control: its label and tool tip (`text`) match as above, or the whole filter is the name of
// the setting it changes. A setting name only counts whole, so ordinary words don't light up
// every control whose setting contains them.
bool sl_search_matches_control(const LLWString& text, const LLWString& setting, const LLWString& filter);

// The section of a one-open-at-a-time accordion to open while searching: the open one if it
// holds a match, else the first that does. With no match anywhere, open_now (-1: none open).
S32 sl_section_to_open(const std::vector<bool>& has_match, S32 open_now);

#endif // SL_SLPREFSEARCH_H
