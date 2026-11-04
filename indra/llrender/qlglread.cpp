/**
 * @file qlglread.cpp
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

#include "linden_common.h"

#include "qlglread.h"

#include "llgl.h"
#include "llrender.h"
#include "llrendertarget.h"

bool ql_read_target_rgba(LLRenderTarget& target, std::vector<U8>& out)
{
    out.clear();
    const U32 width = target.getWidth();
    const U32 height = target.getHeight();
    if (width == 0 || height == 0 || target.getTexture() == 0)
    {
        return false;
    }

    gGL.flush();
    ALTextureSlot* slot = gGL.getTextureSlot(0);
    slot->unbind();
    if (!slot->bindManual(ALTextureSlot::TT_TEXTURE, target.getTexture()))
    {
        return false;
    }

    out.resize((size_t)width * height * 4);
    GLint pack_alignment = 4;
    GLint pack_buffer = 0;
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack_alignment);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
    // Into client memory: no pack buffer may be bound.
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    glPixelStorei(GL_PACK_ALIGNMENT, pack_alignment);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack_buffer);
    stop_glerror();

    slot->unbind();
    return true;
}
