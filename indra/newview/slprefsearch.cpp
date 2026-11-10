/**
 * @file slprefsearch.cpp
 * @brief Preferences search (#1348): matching and the section to open.
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

#include "linden_common.h"

#include "slprefsearch.h"

namespace
{
    constexpr size_t SHORT_WORD = 3; // this many characters or fewer: must start a word

    // Ends a word: white space and punctuation (ASCII, general and CJK)
    bool is_separator(llwchar c)
    {
        return LLStringOps::isSpace(c)
            || (c < 128 && !isalnum((int)c))
            || (c >= 0x2000 && c <= 0x206F)
            || (c >= 0x3000 && c <= 0x303F);
    }

    // Thai, Lao and the CJK scripts are written without spaces between words
    bool written_without_spaces(llwchar c)
    {
        return (c >= 0x0E00 && c <= 0x0EFF) || c >= 0x2E80;
    }

    bool contains_word(const LLWString& text, const LLWString& word)
    {
        if (word.size() > SHORT_WORD || written_without_spaces(word[0]))
        {
            return text.find(word) != LLWString::npos;
        }
        for (size_t at = text.find(word); at != LLWString::npos; at = text.find(word, at + 1))
        {
            if (at == 0 || is_separator(text[at - 1]))
            {
                return true;
            }
        }
        return false;
    }
}

bool sl_search_matches(const LLWString& text, const LLWString& filter)
{
    size_t start = 0;
    while (true)
    {
        // the next word: from the next non-space to the next space (or the end)
        while (start < filter.size() && LLStringOps::isSpace(filter[start]))
        {
            ++start;
        }
        if (start == filter.size())
        {
            return true;
        }
        size_t end = start;
        while (end < filter.size() && !LLStringOps::isSpace(filter[end]))
        {
            ++end;
        }
        if (!contains_word(text, filter.substr(start, end - start)))
        {
            return false;
        }
        start = end;
    }
}

bool sl_search_matches_control(const LLWString& text, const LLWString& setting, const LLWString& filter)
{
    return sl_search_matches(text, filter) || (!setting.empty() && filter == setting);
}

S32 sl_section_to_open(const std::vector<bool>& has_match, S32 open_now)
{
    if (open_now >= 0 && (size_t)open_now < has_match.size() && has_match[open_now])
    {
        return open_now;
    }
    for (size_t i = 0; i < has_match.size(); ++i)
    {
        if (has_match[i])
        {
            return (S32)i;
        }
    }
    return open_now;
}
