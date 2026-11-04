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

#include <chrono>

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

    // The last whole frame's counts, kept by clear() for the on-screen status line.
    U32 mLastDraws          = 0;
    U32 mLastBufferSwitches = 0;
    U32 mLastMatrixChanges  = 0;
    // The last frame's vertex buffer setups: by binding the buffer under formats already given, and by setting every
    // attribute pointer (LLVertexBuffer, QuasarVertexAttribBinding).
    U32 mLastFastSwitches   = 0;
    U32 mLastFullSetups     = 0;

    // The time culling and sorting took, by the kind of view (LLPipeline::updateCull and stateSort, postSort
    // included), in milliseconds: the main view, the sun's shadow, the spot lights' shadows, reflection probes and
    // mirrors, the rest.
    enum EView { VIEW_WORLD, VIEW_SHADOW, VIEW_SPOT, VIEW_PROBE, VIEW_OTHER, NUM_VIEWS };
    F32 mCullMs[NUM_VIEWS]     = {};
    F32 mLastCullMs[NUM_VIEWS] = {};

    // What shared vertex storage would let multi-draws merge, in the main view's opaque passes (QuasarBatchMergeStats,
    // LLPipeline::countBatchMerges): the draws counted, those whose state (vertex format, textures, material, matrix,
    // skin, every other thing a draw binds) equals the draw's before them in the pass as ordered now, and as many once
    // each pass is sorted by that state; and once sorted ignoring the model matrix, what a per-draw matrix buffer
    // would add. Filled during postSort, before the clear that ends the frame, so they land in the mLast* copies.
    U32 mMergeDraws        = 0;
    U32 mMergeAsOrdered    = 0;
    U32 mMergeSorted       = 0;
    U32 mMergeSortedNoMatrix = 0;
    U32 mLastMergeDraws        = 0;
    U32 mLastMergeAsOrdered    = 0;
    U32 mLastMergeSorted       = 0;
    U32 mLastMergeSortedNoMatrix = 0;

    // Batch draws by the kind of view they were drawn for (the same kinds as mCullMs).
    U32 mViewDraws[NUM_VIEWS]     = {};
    U32 mLastViewDraws[NUM_VIEWS] = {};

    void noteDraw(const void* buffer, U32 view)
    {
        ++mDraws;
        ++mViewDraws[view];
        if (buffer != mLastBuffer)
        {
            ++mBufferSwitches;
            mLastBuffer = buffer;
        }
    }

    void clear()
    {
        mLastDraws          = mDraws;
        mLastBufferSwitches = mBufferSwitches;
        mLastMatrixChanges  = mMatrixChanges;
        mDraws = mBufferSwitches = mMatrixChanges = 0;
        mLastBuffer = nullptr;
        mLastMergeDraws          = mMergeDraws;
        mLastMergeAsOrdered      = mMergeAsOrdered;
        mLastMergeSorted         = mMergeSorted;
        mLastMergeSortedNoMatrix = mMergeSortedNoMatrix;
        mMergeDraws = mMergeAsOrdered = mMergeSorted = mMergeSortedNoMatrix = 0;
        for (U32 i = 0; i < NUM_VIEWS; ++i)
        {
            mLastCullMs[i] = mCullMs[i];
            mCullMs[i] = 0.f;
            mLastViewDraws[i] = mViewDraws[i];
            mViewDraws[i] = 0;
        }
    }
};

/// Adds the time from its making to its end to a kind of view's culling and sorting.
class QLCullTimer
{
public:
    explicit QLCullTimer(F32& total) : mTotal(total), mStart(std::chrono::steady_clock::now()) {}
    ~QLCullTimer()
    {
        mTotal += std::chrono::duration<F32, std::milli>(std::chrono::steady_clock::now() - mStart).count();
    }
    QLCullTimer(const QLCullTimer&) = delete;
    QLCullTimer& operator=(const QLCullTimer&) = delete;

private:
    F32& mTotal;
    std::chrono::steady_clock::time_point mStart;
};

extern QLDrawStats gQLDrawStats;

/// Which kind of view the cull, sort or draw under way is for (QLDrawStats::EView); defined in pipeline.cpp.
U32 ql_cull_view();
