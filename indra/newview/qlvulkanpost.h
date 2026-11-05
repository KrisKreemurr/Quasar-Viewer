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

/// QuasarUILayer: the viewer's 2D interface (windows, chat, menus, toolbars, the debug text) drawn into a layer of its
/// own, which the Vulkan presenter lays over the frame and hands to DLSS Frame Generation as the frame's UI, so the
/// generated frames interpolate the world and leave the interface sharp. HUD attachments, name tags and the 3D overlays
/// stay drawn into the frame with the world. The layer comes out premultiplied: llrender blends its alpha as coverage
/// while it is drawn (gQLUILayerAlpha).
class QLUILayer
{
public:
    /// Before the 2D interface is drawn: true, with the layer bound and cleared, where it goes into the layer this
    /// frame; false, with nothing changed, where it is drawn into the window as before.
    static bool begin();

    /// After it is drawn, when begin() was true: the layer let go and handed to the presenter for this frame's present.
    static void end();

    static std::string statusLine();
};
