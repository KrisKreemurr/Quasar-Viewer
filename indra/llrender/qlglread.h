/**
 * @file qlglread.h
 * @brief Quasar Viewer: a render target's pixels read back at once, for debugging dumps.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Quasar Viewer Source Code (based on Alchemy Viewer)
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

#pragma once

#include "stdtypes.h"

#include <vector>

class LLRenderTarget;

/// The first colour attachment of `target`, level 0, as RGBA8, rows from the bottom up. It waits for the GPU to finish
/// everything before it, so it is for a dump taken on request, never for a frame's own work; ALTextureReadback is the
/// way that does not wait. False, with `out` empty, where the target has no colour.
bool ql_read_target_rgba(LLRenderTarget& target, std::vector<U8>& out);
