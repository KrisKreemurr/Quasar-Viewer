/**
 * @file slparcelsummary_test.cpp
 * @brief Area Search (#1321): which 4 m cells belong to a parcel, land impact as the viewer
 *        shows it, the parcel's capacity, the used land impact split by owner, and a region's
 *        parcels as they are learned cell by cell.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "../slparcelsummary.h"

#include "llparcel.h"

#include "../test/lltut.h"

namespace
{
    LLUUID owner(U8 n)
    {
        LLUUID id;
        id.mData[0] = n;
        id.mData[15] = 0x3c;
        return id;
    }

    // A 64-cells-per-edge (256 m region) bitmap with only cell (x, y) set.
    std::vector<U8> oneCell(S32 x, S32 y)
    {
        std::vector<U8> bitmap(64 * 64 / 8, 0);
        const S32 i = x + y * 64;
        bitmap[i / 8] = (U8)(1 << (i % 8));
        return bitmap;
    }

    // A small region for the region map tests: 8 cells per edge (32 m).
    constexpr S32 EDGE = 8;

    template<typename Pred>
    std::vector<U8> cellsWhere(Pred pred)
    {
        std::vector<U8> bitmap(EDGE * EDGE / 8, 0);
        for (S32 y = 0; y < EDGE; ++y)
        {
            for (S32 x = 0; x < EDGE; ++x)
            {
                if (pred(x, y))
                {
                    const S32 i = x + y * EDGE;
                    bitmap[i / 8] |= (U8)(1 << (i % 8));
                }
            }
        }
        return bitmap;
    }
}

namespace tut
{
    struct parcelsummary_data {};
    typedef test_group<parcelsummary_data> parcelsummary_test;
    typedef parcelsummary_test::object parcelsummary_object;
    tut::parcelsummary_test parcelsummary("SLParcelSummary");

    // Bit order matches the agent parcel overlay: cell x + y * edge, LSB first, 4 m cells.
    template<> template<>
    void parcelsummary_object::test<1>()
    {
        SLParcelCells cells;
        ensure("starts empty", cells.empty());
        ensure("accepts a full bitmap", cells.setFromBitmap(oneCell(3, 2), 64));
        ensure("not empty", !cells.empty());
        ensure("inside the cell", cells.contains(13.9f, 9.0f));
        ensure("cell to the east", !cells.contains(16.0f, 9.0f));
        ensure("cell to the north", !cells.contains(13.9f, 12.0f));
        ensure("cell's south-west corner", cells.contains(12.0f, 8.0f));
    }

    // A bitmap of the wrong size is refused and leaves the cells empty.
    template<> template<>
    void parcelsummary_object::test<2>()
    {
        SLParcelCells cells;
        cells.setFromBitmap(oneCell(0, 0), 64);
        ensure("refused", !cells.setFromBitmap(std::vector<U8>(511, 0xff), 64));
        ensure("now empty", cells.empty());
        ensure("contains nothing", !cells.contains(1.0f, 1.0f));
    }

    // Positions outside the region are never inside.
    template<> template<>
    void parcelsummary_object::test<3>()
    {
        SLParcelCells cells;
        cells.setFromBitmap(std::vector<U8>(64 * 64 / 8, 0xff), 64);
        ensure("inside", cells.contains(0.0f, 0.0f));
        ensure("west of the region", !cells.contains(-0.1f, 0.0f));
        ensure("east of the region", !cells.contains(256.0f, 0.0f));
        ensure("north of the region", !cells.contains(0.0f, 256.0f));
    }

    // Capacity as About Land shows it: max objects times the bonus, capped at the region's.
    template<> template<>
    void parcelsummary_object::test<4>()
    {
        ensure_equals("plain", sl_parcel_capacity(351, 1.0f, 15000), 351);
        ensure_equals("bonus", sl_parcel_capacity(351, 2.0f, 15000), 702);
        ensure_equals("region cap", sl_parcel_capacity(10000, 2.0f, 15000), 15000);
        ensure_equals("no region limit known", sl_parcel_capacity(10000, 2.0f, 0), 20000);
    }

    // Land impact is the server's linkset cost truncated, as the Build floater shows it.
    template<> template<>
    void parcelsummary_object::test<5>()
    {
        ensure_equals("12.9", sl_land_impact(12.9f), 12);
        ensure_equals("0.4", sl_land_impact(0.4f), 0);
        ensure_equals("3", sl_land_impact(3.0f), 3);
    }

    // Owners are summed, ordered largest first (ties by owner id), and capped; the rest are
    // summed into others.
    template<> template<>
    void parcelsummary_object::test<6>()
    {
        std::vector<SLParcelObject> objects = {
            { owner(1), 40 }, { owner(2), 96 }, { owner(1), 30 }, { owner(3), 18 },
        };
        for (U8 n = 8; n >= 4; --n)
        {
            objects.push_back({ owner(n), 1 });
        }

        const SLParcelSummary summary = sl_summarize_parcel(objects, 189, 351);
        ensure_equals("five owners", summary.owners.size(), (size_t)5);
        ensure("largest first", summary.owners[0].owner == owner(2) && summary.owners[0].land_impact == 96);
        ensure("summed", summary.owners[1].owner == owner(1) && summary.owners[1].land_impact == 70);
        ensure("third", summary.owners[2].owner == owner(3) && summary.owners[2].land_impact == 18);
        ensure("ties by id", summary.owners[3].owner == owner(4) && summary.owners[4].owner == owner(5));
        ensure_equals("others", summary.others, 3);
        ensure_equals("used", summary.used, 189);
        ensure_equals("capacity", summary.capacity, 351);
        ensure_equals("all loaded", summary.not_loaded, 0);
    }

    // What the server counts beyond the viewer's sum is "not loaded yet", never negative.
    template<> template<>
    void parcelsummary_object::test<7>()
    {
        const std::vector<SLParcelObject> objects = { { owner(1), 200 }, { owner(2), 100 } };
        ensure_equals("server counts more", sl_summarize_parcel(objects, 312, 351).not_loaded, 12);
        ensure_equals("server counts less", sl_summarize_parcel(objects, 290, 351).not_loaded, 0);
        ensure("no objects", sl_summarize_parcel({}, 0, 351).owners.empty());
    }

    // Without property lines (no overlay yet) the unknown cells are one group: one lookup at a
    // time. A group with a lookup under way is not asked again.
    template<> template<>
    void parcelsummary_object::test<8>()
    {
        SLRegionParcelMap map;
        map.reset(EDGE);
        const std::vector<U8> no_lines(EDGE * EDGE, 0);
        ensure("nothing known", !map.complete() && map.size() == 0 && map.used() == 0);
        ensure("unknown cell", map.parcelAt(5) == INVALID_PARCEL_ID);
        ensure("one cell to ask", map.cellsToAsk(no_lines, {}) == std::vector<S32>{ 0 });
        ensure("already asking", map.cellsToAsk(no_lines, { 9 }).empty());

        ensure("west half answered", map.setParcel(7, cellsWhere([](S32 x, S32) { return x < 4; }), 120));
        ensure("the east half next", map.cellsToAsk(no_lines, {}) == std::vector<S32>{ 4 });
        ensure("cell of the answer", map.parcelAt(3 + 6 * EDGE) == 7 && map.parcelAt(4) == INVALID_PARCEL_ID);
    }

    // Property lines split the unknown cells into groups, one lookup each; a parcel in two
    // pieces is asked once, since its answer covers both.
    template<> template<>
    void parcelsummary_object::test<9>()
    {
        SLRegionParcelMap map;
        map.reset(EDGE);
        // A line between x = 3 and 4 (cells at x = 4 have it on their west edge), and one
        // between y = 5 and 6 on the east side only.
        std::vector<U8> lines(EDGE * EDGE, 0);
        for (S32 y = 0; y < EDGE; ++y)
        {
            lines[4 + y * EDGE] |= PARCEL_WEST_LINE;
        }
        for (S32 x = 4; x < EDGE; ++x)
        {
            lines[x + 6 * EDGE] |= PARCEL_SOUTH_LINE;
        }
        ensure("three groups", map.cellsToAsk(lines, {}) == std::vector<S32>{ 0, 4, 4 + 6 * EDGE });
        ensure("skips the group being asked", map.cellsToAsk(lines, { 5 + 2 * EDGE }) == std::vector<S32>{ 0, 4 + 6 * EDGE });

        // Parcel 2 is the west half and the north-east corner, not touching.
        ensure("two pieces", map.setParcel(2, cellsWhere([](S32 x, S32 y) { return x < 4 || y >= 6; }), 50));
        ensure("both pieces covered", map.cellsToAsk(lines, {}) == std::vector<S32>{ 4 });
    }

    // The region adds up from its parcels once every cell is known. A parcel that shrinks leaves
    // cells unknown; one whose cells all went to another parcel is gone.
    template<> template<>
    void parcelsummary_object::test<10>()
    {
        SLRegionParcelMap map;
        map.reset(EDGE);
        map.setParcel(1, cellsWhere([](S32 x, S32) { return x < 4; }), 100);
        ensure("half known", !map.complete());
        map.setParcel(2, cellsWhere([](S32 x, S32) { return x >= 4; }), 40);
        ensure("complete", map.complete());
        ensure_equals("parcels", map.size(), (size_t)2);
        ensure_equals("used", map.used(), 140);

        map.setParcel(2, cellsWhere([](S32 x, S32) { return x >= 6; }), 30);
        ensure("shrunk: cells unknown", !map.complete() && map.parcelAt(4) == INVALID_PARCEL_ID);
        ensure_equals("new count", map.used(), 130);

        map.setParcel(3, cellsWhere([](S32 x, S32) { return x >= 4; }), 45);
        ensure("parcel 2 joined into 3", map.complete() && map.size() == 2 && map.parcelAt(7) == 3);
        ensure_equals("used without parcel 2", map.used(), 145);
    }

    // A bitmap of the wrong size changes nothing; positions map to cells, nothing outside.
    template<> template<>
    void parcelsummary_object::test<11>()
    {
        SLRegionParcelMap map;
        map.reset(EDGE);
        ensure("bad bitmap refused", !map.setParcel(1, std::vector<U8>(3, 0xff), 10));
        ensure("nothing known", map.size() == 0 && map.used() == 0);
        ensure_equals("cell of a position", map.cellAt(13.9f, 9.0f), 3 + 2 * EDGE);
        ensure_equals("west of the region", map.cellAt(-0.5f, 9.0f), -1);
        ensure_equals("north of the region", map.cellAt(9.0f, (F32)(EDGE * 4)), -1);
    }

    // Root objects per parcel: the first count after an answer is the baseline; a parcel whose
    // count moves from it is reported until it is answered again.
    template<> template<>
    void parcelsummary_object::test<12>()
    {
        SLRegionParcelMap map;
        map.reset(EDGE);
        map.setParcel(1, cellsWhere([](S32 x, S32) { return x < 4; }), 100);
        map.setParcel(2, cellsWhere([](S32 x, S32) { return x >= 4; }), 40);
        ensure("baseline", map.noteRoots({ { 1, 12 }, { 2, 3 } }).empty());
        ensure("unchanged", map.noteRoots({ { 1, 12 }, { 2, 3 } }).empty());
        ensure("parcel 2 changed", map.noteRoots({ { 1, 12 }, { 2, 4 } }) == std::vector<S32>{ 2 });
        ensure("no roots counts as 0", map.noteRoots({ { 2, 4 } }) == std::vector<S32>{ 1, 2 });
        map.setParcel(2, cellsWhere([](S32 x, S32) { return x >= 4; }), 41);
        ensure("answered: new baseline", map.noteRoots({ { 1, 12 }, { 2, 4 } }).empty());
    }

    // Owner legend entries shorten their names, all to the same cap, until they fit the room
    // left; when even the shortest cap does not fit, the smallest owners are left out.
    template<> template<>
    void parcelsummary_object::test<13>()
    {
        // 6 px per byte, 20 px per entry for the swatch and gaps.
        const auto width = [](const std::string& text) { return (S32)text.size() * 6; };
        const std::vector<std::string> names = { "Alexandra Longname", "Bo", "Group of Builders" };
        const std::vector<std::string> suffixes = { "96", "70", "18" };

        const std::vector<std::string> roomy = sl_fit_owner_labels(names, suffixes, 1000, 20, width);
        ensure("all fit as they are", roomy == std::vector<std::string>{ "Alexandra Longname 96", "Bo 70", "Group of Builders 18" });

        // Full: 21 + 5 + 20 bytes = 276 px + 60. At 300 px a cap of 14 fits (14 + 5 + 14 + 6 = 39
        // bytes = 234 + 60 = 294), 15 does not (15 + 5 + 15 + 6 = 41 = 306).
        const std::vector<std::string> tight = sl_fit_owner_labels(names, suffixes, 300, 20, width);
        ensure("shortened to fit", tight == std::vector<std::string>{ "Alexandra L... 96", "Bo 70", "Group of Bu... 18" });

        // At the shortest cap (6: "Ale... 96" + "Bo 70" + "Gro... 18" = 9 + 5 + 9 bytes = 138
        // + 60 = 198) 120 px is too little; without the last owner, 9 + 5 bytes = 84 + 40 = 124
        // is too; one owner, 54 + 20 = 74, fits.
        const std::vector<std::string> cramped = sl_fit_owner_labels(names, suffixes, 120, 20, width);
        ensure("smallest owners left out", cramped == std::vector<std::string>{ "Ale... 96" });
        ensure("no room at all", sl_fit_owner_labels(names, suffixes, 10, 20, width).empty());
    }
}
