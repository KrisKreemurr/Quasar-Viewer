/**
 * @file slregionparcels.cpp
 * @brief Area Search (#1321): the agent region's parcels and what the server counts on each,
 *        kept current for the region summary.
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

#include "llviewerprecompiledheaders.h"

#include "slregionparcels.h"

#include "llagent.h"
#include "llparcel.h"
#include "llviewerparcelmgr.h"
#include "llviewerparceloverlay.h"
#include "llviewerregion.h"

namespace
{
constexpr size_t MAX_LOOKUPS = 4;        // under way at once
constexpr F32 RETRY_DELAY = 5.f;         // after a failed lookup
constexpr F32 SLOW_RETRY_DELAY = 30.f;   // after several in a row
constexpr S32 QUICK_RETRIES = 5;
constexpr F32 ROOTS_SETTLE_TIME = 2.f;   // the region's root count holds still this long

S32 cells_per_edge(const LLViewerRegion* region)
{
    return region ? (S32)(region->getWidth() / PARCEL_GRID_STEP_METERS) : 0;
}

// The property lines from the region's parcel overlay, one entry per cell (x + y * edge), or
// none without an overlay.
std::vector<U8> line_flags(LLViewerRegion* region, S32 edge)
{
    std::vector<U8> flags;
    const LLViewerParcelOverlay* overlay = region->getParcelOverlay();
    if (!overlay)
    {
        return flags;
    }
    flags.resize((size_t)edge * edge);
    for (S32 y = 0; y < edge; ++y)
    {
        for (S32 x = 0; x < edge; ++x)
        {
            flags[x + y * edge] = overlay->parcelLineFlags(y, x);
        }
    }
    return flags;
}
}

SLRegionParcelTracker::SLRegionParcelTracker(std::function<void()> on_change)
:   mOnChange(std::move(on_change))
{
}

void SLRegionParcelTracker::reset()
{
    ++mGeneration;
    LLViewerRegion* region = gAgent.getRegion();
    mRegionId = region ? region->getRegionID() : LLUUID::null;
    mParcels.reset(cells_per_edge(region));
    mScanDue = true;
    mAskingCells.clear();
    mAskingParcels.clear();
    mStale.clear();
    mFailures = 0;
    mRegionRoots = -1;
}

void SLRegionParcelTracker::update()
{
    LLViewerRegion* region = gAgent.getRegion();
    if (!region)
    {
        return;
    }
    if (region->getRegionID() != mRegionId)
    {
        reset();
    }
    if (mAskingCells.size() + mAskingParcels.size() >= MAX_LOOKUPS
        || (mFailures > 0 && mFailureTimer.getElapsedTimeF32() < (mFailures < QUICK_RETRIES ? RETRY_DELAY : SLOW_RETRY_DELAY)))
    {
        return;
    }

    // Parcels not found yet come first: until every cell is known there is no region total.
    if (!mParcels.complete() && mScanDue)
    {
        mScanDue = false;
        for (S32 cell : mParcels.cellsToAsk(line_flags(region, mParcels.cellsPerEdge()), mAskingCells))
        {
            if (mAskingCells.size() + mAskingParcels.size() >= MAX_LOOKUPS)
            {
                mScanDue = true; // more groups than free lookups: go on when one answers
                break;
            }
            askCell(region, cell);
        }
    }
    while (!mStale.empty() && mAskingCells.size() + mAskingParcels.size() < MAX_LOOKUPS)
    {
        const S32 local_id = *mStale.begin();
        mStale.erase(mStale.begin());
        askParcel(region, local_id);
    }
}

void SLRegionParcelTracker::noteRoots(const std::map<S32, S32>& roots_by_parcel, S32 region_roots)
{
    // While objects are still streaming in, the counts grow without the server's moving; only
    // a count that changes once they have settled means objects were rezzed or removed.
    if (region_roots != mRegionRoots)
    {
        mRegionRoots = region_roots;
        mRootsSteadyTimer.reset();
        return;
    }
    if (mRootsSteadyTimer.getElapsedTimeF32() < ROOTS_SETTLE_TIME)
    {
        return;
    }
    for (S32 local_id : mParcels.noteRoots(roots_by_parcel))
    {
        if (!mAskingParcels.contains(local_id))
        {
            mStale.insert(local_id);
        }
    }
}

void SLRegionParcelTracker::askCell(LLViewerRegion* region, S32 cell)
{
    const S32 edge = mParcels.cellsPerEdge();
    const LLVector3 center(((F32)(cell % edge) + 0.5f) * PARCEL_GRID_STEP_METERS,
                           ((F32)(cell / edge) + 0.5f) * PARCEL_GRID_STEP_METERS, 0.f);
    mAskingCells.insert(cell);
    LLHandle<SLRegionParcelTracker> handle = getHandle();
    const U32 generation = mGeneration;
    LLViewerParcelMgr::instance().requestParcelContentsAt(region, center, [handle, generation, cell](const LLSD& info)
    {
        if (SLRegionParcelTracker* self = handle.get())
        {
            self->onAnswer(info, generation, cell, INVALID_PARCEL_ID);
        }
    });
}

void SLRegionParcelTracker::askParcel(LLViewerRegion* region, S32 local_id)
{
    mAskingParcels.insert(local_id);
    LLHandle<SLRegionParcelTracker> handle = getHandle();
    const U32 generation = mGeneration;
    LLViewerParcelMgr::instance().requestParcelContents(region, local_id, [handle, generation, local_id](const LLSD& info)
    {
        if (SLRegionParcelTracker* self = handle.get())
        {
            self->onAnswer(info, generation, -1, local_id);
        }
    });
}

void SLRegionParcelTracker::onAnswer(const LLSD& info, U32 generation, S32 cell, S32 local_id)
{
    if (generation != mGeneration)
    {
        return;
    }
    mAskingCells.erase(cell);
    mAskingParcels.erase(local_id);
    mScanDue = true;

    // An answer by cell must cover that cell, or the same cell would be asked again at once.
    if (!info.isMap() || !mParcels.setParcel(info["local_id"].asInteger(), info["bitmap"].asBinary(), info["total_prims"].asInteger())
        || (cell >= 0 && mParcels.parcelAt(cell) == INVALID_PARCEL_ID))
    {
        ++mFailures;
        mFailureTimer.reset();
        if (local_id != INVALID_PARCEL_ID)
        {
            // Asked by id and no answer: the parcel may be gone (joined into another). Forget
            // its cells so they are looked up again by position.
            const S32 edge = mParcels.cellsPerEdge();
            mParcels.setParcel(local_id, std::vector<U8>((size_t)edge * edge / 8, 0), 0);
        }
        return;
    }
    mFailures = 0;
    if (mOnChange)
    {
        mOnChange();
    }
}
