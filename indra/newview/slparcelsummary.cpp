/**
 * @file slparcelsummary.cpp
 * @brief Area Search (#1321): parcel cells, land impact, capacity and the owner split.
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

#include "linden_common.h"

#include "slparcelsummary.h"

#include "llmath.h"
#include "llparcel.h"
#include "llstring.h"

#include <algorithm>
#include <cmath>
#include <map>

bool SLParcelCells::setFromBitmap(const std::vector<U8>& bitmap, S32 cells_per_edge)
{
    if (cells_per_edge <= 0 || bitmap.size() != (size_t)cells_per_edge * cells_per_edge / 8)
    {
        mCellsPerEdge = 0;
        mBitmap.clear();
        return false;
    }
    mCellsPerEdge = cells_per_edge;
    mBitmap = bitmap;
    return true;
}

bool SLParcelCells::contains(F32 region_x, F32 region_y) const
{
    if (mBitmap.empty() || region_x < 0.f || region_y < 0.f)
    {
        return false;
    }
    const S32 x = (S32)std::floor(region_x / PARCEL_GRID_STEP_METERS);
    const S32 y = (S32)std::floor(region_y / PARCEL_GRID_STEP_METERS);
    if (x >= mCellsPerEdge || y >= mCellsPerEdge)
    {
        return false;
    }
    const S32 i = x + y * mCellsPerEdge;
    return (mBitmap[i / 8] & (1 << (i % 8))) != 0;
}

S32 sl_land_impact(F32 linkset_cost)
{
    return (S32)linkset_cost;
}

S32 sl_parcel_capacity(S32 max_prims, F32 prim_bonus, S32 region_max_tasks)
{
    const S32 capacity = ll_round((F32)max_prims * prim_bonus);
    return region_max_tasks > 0 ? llmin(capacity, region_max_tasks) : capacity;
}

SLParcelSummary sl_summarize_parcel(const std::vector<SLParcelObject>& objects, S32 server_used, S32 capacity,
                                    size_t max_owners)
{
    std::map<LLUUID, S32> by_owner;
    S32 total = 0;
    for (const SLParcelObject& object : objects)
    {
        by_owner[object.owner] += object.land_impact;
        total += object.land_impact;
    }

    std::vector<SLOwnerShare> shares;
    shares.reserve(by_owner.size());
    for (const auto& [owner, land_impact] : by_owner)
    {
        shares.push_back({ owner, land_impact });
    }
    // The map already orders by owner id, so a stable sort keeps ties in id order.
    std::stable_sort(shares.begin(), shares.end(),
                     [](const SLOwnerShare& a, const SLOwnerShare& b) { return a.land_impact > b.land_impact; });

    SLParcelSummary summary;
    summary.used = server_used;
    summary.capacity = capacity;
    summary.not_loaded = llmax(0, server_used - total);
    for (size_t i = 0; i < shares.size(); ++i)
    {
        if (i < max_owners)
        {
            summary.owners.push_back(shares[i]);
        }
        else
        {
            summary.others += shares[i].land_impact;
        }
    }
    return summary;
}

std::vector<std::string> sl_fit_owner_labels(const std::vector<std::string>& names, const std::vector<std::string>& suffixes,
                                             S32 available, S32 per_entry, const std::function<S32(const std::string&)>& width,
                                             size_t max_chars, size_t min_chars)
{
    auto entries_at = [&](size_t cap)
    {
        std::vector<std::string> entries;
        for (size_t i = 0; i < names.size(); ++i)
        {
            std::string name = names[i];
            if (name.size() > cap)
            {
                // Names are UTF-8: cut on a character boundary.
                name = utf8str_truncate(name, (S32)cap - 3) + "...";
            }
            entries.push_back(i < suffixes.size() ? name + " " + suffixes[i] : name);
        }
        return entries;
    };
    auto total = [&](const std::vector<std::string>& entries)
    {
        S32 sum = 0;
        for (const std::string& entry : entries)
        {
            sum += per_entry + width(entry);
        }
        return sum;
    };

    std::vector<std::string> entries;
    for (size_t cap = max_chars; cap >= min_chars; --cap)
    {
        entries = entries_at(cap);
        if (total(entries) <= available)
        {
            return entries;
        }
    }
    while (!entries.empty() && total(entries) > available)
    {
        entries.pop_back();
    }
    return entries;
}

void SLRegionParcelMap::reset(S32 cells_per_edge)
{
    mCellsPerEdge = llmax(0, cells_per_edge);
    mCellParcel.assign((size_t)mCellsPerEdge * mCellsPerEdge, INVALID_PARCEL_ID);
    mUnknownCells = mCellParcel.size();
    mParcels.clear();
}

std::vector<S32> SLRegionParcelMap::cellsToAsk(const std::vector<U8>& line_flags, const std::set<S32>& asking) const
{
    const S32 edge = mCellsPerEdge;
    const bool lines = line_flags.size() == mCellParcel.size();
    auto line = [&](S32 cell, U8 flag) { return lines && (line_flags[cell] & flag) != 0; };

    std::vector<S32> cells;
    std::vector<bool> seen(mCellParcel.size(), false);
    std::vector<S32> stack;
    for (S32 start = 0; start < (S32)mCellParcel.size(); ++start)
    {
        if (seen[start] || mCellParcel[start] != INVALID_PARCEL_ID)
        {
            continue;
        }
        // Flood the unknown cells reachable without crossing a property line.
        bool being_asked = false;
        seen[start] = true;
        stack.push_back(start);
        while (!stack.empty())
        {
            const S32 cell = stack.back();
            stack.pop_back();
            being_asked = being_asked || asking.contains(cell);
            const S32 x = cell % edge;
            const S32 y = cell / edge;
            auto visit = [&](S32 next, bool open)
            {
                if (open && !seen[next] && mCellParcel[next] == INVALID_PARCEL_ID)
                {
                    seen[next] = true;
                    stack.push_back(next);
                }
            };
            // A cell's own flags mark lines on its west and south edges.
            visit(cell - 1, x > 0 && !line(cell, PARCEL_WEST_LINE));
            visit(cell + 1, x + 1 < edge && !line(cell + 1, PARCEL_WEST_LINE));
            visit(cell - edge, y > 0 && !line(cell, PARCEL_SOUTH_LINE));
            visit(cell + edge, y + 1 < edge && !line(cell + edge, PARCEL_SOUTH_LINE));
        }
        if (!being_asked)
        {
            cells.push_back(start);
        }
    }
    return cells;
}

bool SLRegionParcelMap::setParcel(S32 local_id, const std::vector<U8>& bitmap, S32 used)
{
    if (mCellsPerEdge <= 0 || bitmap.size() != mCellParcel.size() / 8)
    {
        return false;
    }
    for (size_t i = 0; i < mCellParcel.size(); ++i)
    {
        const bool mine = (bitmap[i / 8] & (1 << (i % 8))) != 0;
        if (mine && mCellParcel[i] != local_id)
        {
            mUnknownCells -= mCellParcel[i] == INVALID_PARCEL_ID ? 1 : 0;
            mCellParcel[i] = local_id;
        }
        else if (!mine && mCellParcel[i] == local_id)
        {
            // The parcel shrank: whose the cell is now takes another lookup.
            mCellParcel[i] = INVALID_PARCEL_ID;
            ++mUnknownCells;
        }
    }
    mParcels[local_id] = Parcel{ used };

    // Parcels whose cells all went to this one (joined, or redrawn) are gone.
    std::set<S32> present(mCellParcel.begin(), mCellParcel.end());
    std::erase_if(mParcels, [&present](const auto& parcel) { return !present.contains(parcel.first); });
    return true;
}

S32 SLRegionParcelMap::parcelAt(S32 cell) const
{
    return cell >= 0 && cell < (S32)mCellParcel.size() ? mCellParcel[cell] : INVALID_PARCEL_ID;
}

S32 SLRegionParcelMap::cellAt(F32 region_x, F32 region_y) const
{
    if (region_x < 0.f || region_y < 0.f)
    {
        return -1;
    }
    const S32 x = (S32)std::floor(region_x / PARCEL_GRID_STEP_METERS);
    const S32 y = (S32)std::floor(region_y / PARCEL_GRID_STEP_METERS);
    return x < mCellsPerEdge && y < mCellsPerEdge ? x + y * mCellsPerEdge : -1;
}

S32 SLRegionParcelMap::used() const
{
    S32 used = 0;
    for (const auto& [local_id, parcel] : mParcels)
    {
        used += parcel.used;
    }
    return used;
}

std::vector<S32> SLRegionParcelMap::noteRoots(const std::map<S32, S32>& roots_by_parcel)
{
    std::vector<S32> changed;
    for (auto& [local_id, parcel] : mParcels)
    {
        const auto it = roots_by_parcel.find(local_id);
        const S32 roots = it != roots_by_parcel.end() ? it->second : 0;
        if (parcel.roots < 0)
        {
            parcel.roots = roots;
        }
        else if (roots != parcel.roots)
        {
            changed.push_back(local_id);
        }
    }
    return changed;
}
