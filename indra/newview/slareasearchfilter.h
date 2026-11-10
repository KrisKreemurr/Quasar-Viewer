/**
 * @file slareasearchfilter.h
 * @brief Area Search (#1321): which objects the list shows. Each filter is one Any / Only /
 *        Hide control over a property, and one search box matches a substring or a
 *        whole-field regex in one field or any of them. No viewer objects here (FSAreaSearch
 *        fills the facts and strings), so the rules are unit-tested
 *        (tests/slareasearchfilter_test.cpp).
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

#ifndef SL_SLAREASEARCHFILTER_H
#define SL_SLAREASEARCHFILTER_H

#include "llsd.h"

#include <boost/regex.hpp>

#include <array>
#include <bitset>
#include <string>

// One filter's setting: list objects whatever they are, only those with the property, or
// only those without it.
enum class SLTriState : U8 { Any = 0, Only = 1, Hide = 2 };

bool sl_tri_state_passes(SLTriState state, bool has_property);

enum class SLAreaSearchProperty : U8
{
    Temporary, Attachment, Physical, Phantom, Locked, Scripted, SharedMedia, ForSale,
    Copy, Modify, Transfer, ReflectionProbe, ChildPrim,
    Count
};

// Stable key per property, used in the saved state and as the XUI control suffix (tri_<key>).
const char* sl_area_search_property_key(SLAreaSearchProperty property);

// What the filters look at for one object.
struct SLAreaSearchFacts
{
    std::bitset<(size_t)SLAreaSearchProperty::Count> has;
    S32 price = 0;              // sale price, when ForSale is set
    bool handles_touch = false; // the object has a touch handler
    U8 click_action = 0;        // the object's click action (CLICK_ACTION_*)
};

struct SLAreaSearchFilters
{
    std::array<SLTriState, (size_t)SLAreaSearchProperty::Count> states{};
    S32 price_min = 0;          // the sale price range applies when ForSale is Only
    S32 price_max = 999999;
    U8 click_action_filter = 0; // 0 none, 1 any click action, 2 touch, n >= 3 click action n - 2

    // Everything Any, except attachments and child prims, which are hidden.
    static SLAreaSearchFilters defaults();

    bool passes(const SLAreaSearchFacts& facts) const;

    SLTriState get(SLAreaSearchProperty property) const { return states[(size_t)property]; }
    void set(SLAreaSearchProperty property, SLTriState state) { states[(size_t)property] = state; }

    // { states: { <key>: 0|1|2, ... }, price_min, price_max, click_action }. Missing keys and
    // out-of-range values read back as the defaults.
    LLSD asLLSD() const;
    static SLAreaSearchFilters fromLLSD(const LLSD& sd);

    bool operator==(const SLAreaSearchFilters&) const = default;
};

enum class SLAreaSearchField : U8 { All, Name, Description, Owner, Group, Creator, LastOwner };

// One-time conversion of compatible, saved pre-#1321 preferences. Undefined when
// there are no legacy values. Callers retain any already-authored new state.
LLSD sl_area_search_migrate_legacy(const LLSD& legacy);

// The text an object can be found by (names already resolved, and hidden where RLV says so).
struct SLAreaSearchStrings
{
    std::string name;
    std::string description;
    std::string owner;
    std::string group;
    std::string creator;
    std::string last_owner;
};

// The search box. A plain query is a case-insensitive substring; a regex has to match a
// whole field, case-insensitively. With All, any one field matching is enough.
class SLAreaSearchText
{
public:
    // False for an invalid regex, which leaves the previous query in force.
    bool set(const std::string& query, SLAreaSearchField field, bool regex);

    bool empty() const { return mQuery.empty(); }
    bool matches(const SLAreaSearchStrings& strings) const;

private:
    bool matchesField(const std::string& text) const;

    std::string mQuery;
    SLAreaSearchField mField = SLAreaSearchField::All;
    bool mRegex = false;
    boost::regex mPattern;
};

#endif // SL_SLAREASEARCHFILTER_H
