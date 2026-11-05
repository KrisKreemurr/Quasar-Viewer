/**
 * @file qlvulkanpost.h
 * @brief Quasar Viewer: post passes run in Vulkan (QuasarVulkanPost), the first step of moving rendering into Vulkan.
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

#include <string>

class LLRenderTarget;

/// The viewer's side of the presenter's post passes (QLVulkanPresenter::sharpen), kept apart so pipeline.cpp needs no
/// Windows or Vulkan header. Everything is false or empty where the Vulkan presenter does not run.
class QLVulkanPost
{
public:
    /// QuasarVulkanPost is on and Vulkan can run the passes.
    static bool wanted();

    /// CAS sharpening in Vulkan, src into dst (the same size). False where it did not run, dst untouched.
    static bool sharpen(LLRenderTarget* src, LLRenderTarget* dst, const U32 cas_const0[4], const U32 cas_const1[4]);

    static std::string statusLine();
};
