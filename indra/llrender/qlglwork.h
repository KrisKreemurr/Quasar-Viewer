/**
 * @file qlglwork.h
 * @brief Quasar: a call before GL does work on the GPU, for Vulkan's runs of GL's draws to be handed over first.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Quasar Viewer Source Code
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

// Newview's Vulkan runs (QuasarVulkanLighting) record GL's draws for Vulkan and hand them to the GPU only when GL next
// needs it: whatever GL draws, clears, copies, blits, reads back or dispatches must come after what Vulkan recorded
// before it. Each place llrender does such work calls qlBeforeGLWork() first; the viewer sets the hook (null: nothing
// to hand over). Binds, uniforms and uploads do not call it: they are what Vulkan's draws read, and run before them.
// The hook is told the function that calls it (for the viewer's count of what hands its runs over).
extern void (*gQLBeforeGLWork)(const char* what);

// And a GL draw of an indexed vertex buffer (LLVertexBuffer::drawRange, drawRangeFast) offered to Vulkan first, with
// the buffer, the mode (LLRender's) and the range: true where Vulkan recorded it, and GL draws nothing.
class LLVertexBuffer;
extern bool (*gQLVulkanDrawRange)(const LLVertexBuffer* vb, unsigned mode, unsigned start, unsigned end, unsigned count,
                                  unsigned indices_offset);

// And a draw with no vertex buffer (LLRender::drawArraysInstanced: the program places its vertices from gl_VertexID, as
// the water's field does), with LLRender's mode and GL's patch size: true where Vulkan recorded it.
extern bool (*gQLVulkanDrawInstanced)(unsigned mode, unsigned first, unsigned count, unsigned instances,
                                      unsigned patch_vertices);

// Why LLImageGL::qlVulkanCopyOf last gave no copy (for the status line); set by llimagegl.cpp.
extern const char* gQLCopyWhy;

// What the engine's uniform-buffer point `binding` holds (ALUniformBuffer::qlBound) stamped: the same stamp, the same
// bytes; new whenever they may have changed; 0 unknown (aluniformbuffer.cpp).
unsigned long long qlUniformBindingStamp(unsigned binding);

inline void qlBeforeGLWork(const char* what = __builtin_FUNCTION())
{
    if (gQLBeforeGLWork)
    {
        gQLBeforeGLWork(what);
    }
}
