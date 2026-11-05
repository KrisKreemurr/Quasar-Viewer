/**
 * @file qldlssg.h
 * @brief Quasar Viewer: the inputs NVIDIA DLSS Frame Generation reads, made from each frame the viewer draws.
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

#include <string>

/// QuasarDLSSFrameGen. DLSS Frame Generation (Streamline's sl.dlss_g, under the Vulkan presenter) makes a frame between
/// every two the viewer presents, from the frames' colour without the interface, their depth and their motion. Once the
/// final blit has drawn the world and before the interface is drawn over it, capture() draws the depth and the motion
/// (the camera's: see qlDLSSGInputsF.glsl) into a target of the window's size and hands them, with the back buffer as
/// it is then and the frame's camera, to the presenter, which copies them into images Vulkan shares and tags them for
/// Streamline when it presents the frame.
///
/// Nothing is drawn or kept while frame generation is not wanted or cannot run (no Vulkan presentation, no Streamline,
/// a GPU before the RTX 40 series).
class QLDLSSGInputs
{
public:
    /// After LLPipeline::renderFinalize, on the main thread. normal: the frame is the main view, at the window's size,
    /// with no zoom (no snapshot, no cube capture, no cutout); any other frame hands over nothing.
    static void capture(bool normal);

    /// For the status line.
    static std::string statusLine();
};
