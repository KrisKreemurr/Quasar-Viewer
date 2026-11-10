/**
 * @file llgridinfo.h
 * @brief Fetches an OpenSim grid's get_grid_info answer and adds the grid to LLGridManager.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLGRIDINFO_H
#define LL_LLGRIDINFO_H

#include <functional>
#include <string>

class LLSD;

namespace LLGridInfo
{
    /// Result of an add: ok, the grid's key in LLGridManager (when ok) and a
    /// message for the user (the reason when not ok, a warning when ok).
    typedef std::function<void(bool ok, const std::string& grid_name, const std::string& message)> result_fn_t;

    /// Fetch <uri>/get_grid_info without blocking, then add the grid to (or refresh it in)
    /// LLGridManager.  uri is what the user typed: with or without scheme (https is tried
    /// before http), with or without a trailing slash.  The callback always runs, on the
    /// main thread, once the request succeeded, failed or timed out.
    void addGridFromURI(const std::string& uri, result_fn_t callback);

    /// Convert a get_grid_info document (root <gridinfo>) to a map of its element names to
    /// text.  Returns false if the text is not such a document.
    bool parseGridInfo(const std::string& body, LLSD& info);
}

#endif
