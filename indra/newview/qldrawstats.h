/**
 * @file qldrawstats.h
 * @brief Quasar Viewer: per-frame counts of the draw pools' draws, for planning draw batching.
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

/// What LLRenderPass's batch draws (pushBatch, pushUntexturedBatch) did in a frame, every pass and view counted. Plotted
/// in Tracy at the start of each main frame (LLPipeline::renderGeomDeferred), then cleared. They tell which batching
/// would pay: draws that share a vertex buffer can be merged into a multi-draw as they are, while draws that each move
/// the model matrix need it moved into a per-draw buffer the shaders read first.
struct QLDrawStats
{
    U32 mDraws          = 0;  // batch draws issued
    U32 mBufferSwitches = 0;  // draws whose vertex buffer differed from the draw before
    U32 mMatrixChanges  = 0;  // model matrix changes (LLRenderPass::applyModelMatrix)

    const void* mLastBuffer = nullptr;

    void noteDraw(const void* buffer)
    {
        ++mDraws;
        if (buffer != mLastBuffer)
        {
            ++mBufferSwitches;
            mLastBuffer = buffer;
        }
    }

    void clear() { *this = QLDrawStats(); }
};

extern QLDrawStats gQLDrawStats;
