/**
 * @file slregionparcels.h
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

#ifndef SL_SLREGIONPARCELS_H
#define SL_SLREGIONPARCELS_H

#include "llframetimer.h"
#include "llhandle.h"
#include "slparcelsummary.h"

#include <functional>

class LLSD;
class LLViewerRegion;

// No message reports a region's used land impact, so it is added up from its parcels: each is
// looked up once through one of its cells, a few lookups at a time, and asked again by id when
// the root objects the viewer sees on it change. Lookups only run while update() is called.
class SLRegionParcelTracker : public LLHandleProvider<SLRegionParcelTracker>
{
public:
    // on_change runs when an answer changes what is known.
    explicit SLRegionParcelTracker(std::function<void()> on_change);

    // Forget everything and look the parcels up again (Refresh).
    void reset();
    // Idle: follow the agent region and start the lookups that are due.
    void update();
    // Root objects seen on each parcel and in the whole region, counted by the summary.
    void noteRoots(const std::map<S32, S32>& roots_by_parcel, S32 region_roots);

    const SLRegionParcelMap& parcels() const { return mParcels; }

private:
    void askCell(LLViewerRegion* region, S32 cell);
    void askParcel(LLViewerRegion* region, S32 local_id);
    void onAnswer(const LLSD& info, U32 generation, S32 cell, S32 local_id);

    std::function<void()> mOnChange;
    SLRegionParcelMap mParcels;
    LLUUID mRegionId;
    U32 mGeneration{ 0 };         // answers to lookups from before a reset are dropped
    bool mScanDue{ true };        // something changed since the unknown cells were last grouped
    std::set<S32> mAskingCells;   // lookups by cell under way
    std::set<S32> mAskingParcels; // lookups by id under way
    std::set<S32> mStale;         // parcels to ask again
    S32 mFailures{ 0 };           // failed lookups since the last answer
    LLFrameTimer mFailureTimer;
    S32 mRegionRoots{ -1 };
    LLFrameTimer mRootsSteadyTimer; // since the region's root count last moved
};

#endif // SL_SLREGIONPARCELS_H
