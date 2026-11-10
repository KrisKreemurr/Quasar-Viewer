/**
 * @file slparcelsummary.h
 * @brief Area Search (#1321): which 4 m cells belong to a parcel (from the parcel's
 *        properties bitmap), land impact as the viewer shows it, the parcel's capacity as
 *        About Land computes it, and the used land impact split by owner. No viewer objects
 *        here, so it is unit-tested (tests/slparcelsummary_test.cpp).
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

#ifndef SL_SLPARCELSUMMARY_H
#define SL_SLPARCELSUMMARY_H

#include "lluuid.h"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

// The cells of one parcel, from a ParcelProperties bitmap: one bit per 4 m cell, cell
// x + y * cells_per_edge, least significant bit first (as the agent parcel overlay reads it).
class SLParcelCells
{
public:
    // False for a bitmap of the wrong size, which leaves the cells empty.
    bool setFromBitmap(const std::vector<U8>& bitmap, S32 cells_per_edge);

    bool contains(F32 region_x, F32 region_y) const;
    bool empty() const { return mBitmap.empty(); }

private:
    S32 mCellsPerEdge = 0;
    std::vector<U8> mBitmap;
};

// Land impact as the Build floater and Object Weights show it: the server's linkset cost,
// truncated (#781).
S32 sl_land_impact(F32 linkset_cost);

// The parcel's capacity as About Land shows it: max objects times the object bonus, capped at
// the region's object limit when that is known (> 0).
S32 sl_parcel_capacity(S32 max_prims, F32 prim_bonus, S32 region_max_tasks);

struct SLParcelObject
{
    LLUUID owner;       // the ownership id: the group for group-owned objects
    S32 land_impact = 0;
};

struct SLOwnerShare
{
    LLUUID owner;
    S32 land_impact = 0;
    bool operator==(const SLOwnerShare&) const = default;
};

struct SLParcelSummary
{
    std::vector<SLOwnerShare> owners; // largest first, at most max_owners
    S32 others = 0;                   // the owners past max_owners, together
    S32 not_loaded = 0;               // what the server counts beyond the objects summed here
    S32 used = 0;                     // the server's count
    S32 capacity = 0;
    bool operator==(const SLParcelSummary&) const = default;
};

SLParcelSummary sl_summarize_parcel(const std::vector<SLParcelObject>& objects, S32 server_used, S32 capacity,
                                    size_t max_owners = 5);

// The owner bar's legend entries ("name LI"), shortened to fit `available` pixels together:
// every name is cut to the same number of bytes (on a UTF-8 boundary, with "..."), from
// max_chars down to min_chars, and if even that is too wide the last (smallest) owners are left
// out. per_entry is the room each entry takes besides its text; width measures a text.
std::vector<std::string> sl_fit_owner_labels(const std::vector<std::string>& names, const std::vector<std::string>& suffixes,
                                             S32 available, S32 per_entry, const std::function<S32(const std::string&)>& width,
                                             size_t max_chars = 24, size_t min_chars = 6);

// A region's parcels as Area Search learns them, one lookup per parcel: which parcel each 4 m
// cell (x + y * cells_per_edge) is in and what the server counts on each, so the region's used
// land impact adds up from its parcels. It also keeps how many root objects the viewer saw on
// each parcel since its answer, to tell when a parcel should be asked again.
class SLRegionParcelMap
{
public:
    void reset(S32 cells_per_edge);
    S32 cellsPerEdge() const { return mCellsPerEdge; }

    // The cells to look up next: one per group of unknown cells the property lines enclose,
    // skipping groups that contain a cell in `asking`. line_flags holds each cell's
    // PARCEL_WEST_LINE / PARCEL_SOUTH_LINE bits from the parcel overlay; with no lines (no
    // overlay yet) all unknown cells are one group, so parcels are found one at a time.
    std::vector<S32> cellsToAsk(const std::vector<U8>& line_flags, const std::set<S32>& asking) const;

    // A lookup's answer: the parcel's cells (its ParcelProperties bitmap) and the server's count.
    // Cells another parcel had are this one's now, and a parcel left with no cells is gone.
    // False, changing nothing, for a bitmap of the wrong size.
    bool setParcel(S32 local_id, const std::vector<U8>& bitmap, S32 used);

    S32 parcelAt(S32 cell) const;               // INVALID_PARCEL_ID when not known
    S32 cellAt(F32 region_x, F32 region_y) const; // -1 outside the region
    bool complete() const { return mCellsPerEdge > 0 && mUnknownCells == 0; }
    S32 used() const;
    size_t size() const { return mParcels.size(); }

    // Root objects seen per parcel now (parcels left out have none). The first count after a
    // parcel's answer is its baseline; returns the parcels whose count has moved from theirs.
    std::vector<S32> noteRoots(const std::map<S32, S32>& roots_by_parcel);

private:
    struct Parcel
    {
        S32 used = 0;
        S32 roots = -1; // the baseline; -1 until counted after the answer
    };

    S32 mCellsPerEdge = 0;
    std::vector<S32> mCellParcel; // local id per cell
    size_t mUnknownCells = 0;
    std::map<S32, Parcel> mParcels;
};

#endif // SL_SLPARCELSUMMARY_H
