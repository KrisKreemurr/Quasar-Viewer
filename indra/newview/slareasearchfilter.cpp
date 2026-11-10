/**
 * @file slareasearchfilter.cpp
 * @brief Area Search (#1321): the Any / Only / Hide filters and the search box.
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

#include "slareasearchfilter.h"
#include "llmath.h"

#include <boost/algorithm/string/find.hpp>

bool sl_tri_state_passes(SLTriState state, bool has_property)
{
    switch (state)
    {
    case SLTriState::Only:
        return has_property;
    case SLTriState::Hide:
        return !has_property;
    case SLTriState::Any:
    default:
        return true;
    }
}

const char* sl_area_search_property_key(SLAreaSearchProperty property)
{
    switch (property)
    {
    case SLAreaSearchProperty::Temporary:       return "temporary";
    case SLAreaSearchProperty::Attachment:      return "attachment";
    case SLAreaSearchProperty::Physical:        return "physical";
    case SLAreaSearchProperty::Phantom:         return "phantom";
    case SLAreaSearchProperty::Locked:          return "locked";
    case SLAreaSearchProperty::Scripted:        return "scripted";
    case SLAreaSearchProperty::SharedMedia:     return "shared_media";
    case SLAreaSearchProperty::ForSale:         return "for_sale";
    case SLAreaSearchProperty::Copy:            return "copy";
    case SLAreaSearchProperty::Modify:          return "modify";
    case SLAreaSearchProperty::Transfer:        return "transfer";
    case SLAreaSearchProperty::ReflectionProbe: return "reflection_probe";
    case SLAreaSearchProperty::ChildPrim:       return "child_prim";
    case SLAreaSearchProperty::Count:           break;
    }
    return "";
}

// static
SLAreaSearchFilters SLAreaSearchFilters::defaults()
{
    SLAreaSearchFilters filters;
    filters.states.fill(SLTriState::Any);
    filters.set(SLAreaSearchProperty::Attachment, SLTriState::Hide);
    filters.set(SLAreaSearchProperty::ChildPrim, SLTriState::Hide);
    return filters;
}

bool SLAreaSearchFilters::passes(const SLAreaSearchFacts& facts) const
{
    for (size_t i = 0; i < states.size(); ++i)
    {
        if (!sl_tri_state_passes(states[i], facts.has.test(i)))
        {
            return false;
        }
    }

    if (get(SLAreaSearchProperty::ForSale) == SLTriState::Only && (facts.price < price_min || facts.price > price_max))
    {
        return false;
    }

    switch (click_action_filter)
    {
    case 0:
        return true;
    case 1:
        return facts.handles_touch || facts.click_action != 0;
    case 2:
        return facts.handles_touch;
    default:
        return facts.click_action == click_action_filter - 2;
    }
}

LLSD SLAreaSearchFilters::asLLSD() const
{
    LLSD sd;
    LLSD& state_map = sd["states"];
    for (size_t i = 0; i < states.size(); ++i)
    {
        state_map[sl_area_search_property_key((SLAreaSearchProperty)i)] = (S32)states[i];
    }
    sd["price_min"] = price_min;
    sd["price_max"] = price_max;
    sd["click_action"] = (S32)click_action_filter;
    return sd;
}

// static
LLSD sl_area_search_migrate_legacy(const LLSD& legacy)
{
    if (!legacy.isMap() || legacy.size() == 0)
    {
        return LLSD();
    }
    SLAreaSearchFilters filters = SLAreaSearchFilters::defaults();
    struct Mapping { SLAreaSearchProperty property; const char* only; const char* hide; };
    static constexpr Mapping mappings[] = {
        {SLAreaSearchProperty::Temporary, "OnlyTemporary", "ExcludeTemporary"},
        {SLAreaSearchProperty::Attachment, "OnlyAttachments", "ExcludeAttachments"},
        {SLAreaSearchProperty::Physical, "OnlyPhysical", "ExcludePhysical"},
        {SLAreaSearchProperty::Phantom, "OnlyPhantom", nullptr},
        {SLAreaSearchProperty::Locked, "OnlyLocked", nullptr},
        {SLAreaSearchProperty::SharedMedia, "OnlyMOAP", nullptr},
        {SLAreaSearchProperty::ForSale, "FilterForSale", nullptr},
        {SLAreaSearchProperty::Copy, "OnlyCopiable", nullptr},
        {SLAreaSearchProperty::Modify, "OnlyModifiable", nullptr},
        {SLAreaSearchProperty::Transfer, "OnlyTransferable", nullptr},
        {SLAreaSearchProperty::ReflectionProbe, "OnlyReflectionProbes", "ExcludeReflectionProbes"},
        {SLAreaSearchProperty::ChildPrim, nullptr, "ExcludeChildPrims"}
    };
    for (const auto& mapping : mappings)
    {
        if ((!mapping.only || !legacy.has(mapping.only)) && (!mapping.hide || !legacy.has(mapping.hide)))
        {
            continue;
        }
        const bool only = mapping.only && legacy[mapping.only].asBoolean();
        const bool hide = mapping.hide && (legacy.has(mapping.hide) ? legacy[mapping.hide].asBoolean() :
            filters.get(mapping.property) == SLTriState::Hide);
        // The old UI disabled Only when Exclude was checked.
        filters.set(mapping.property, hide ? SLTriState::Hide : only ? SLTriState::Only : SLTriState::Any);
    }
    if (legacy.has("MinimumPrice")) filters.price_min = legacy["MinimumPrice"].asInteger();
    if (legacy.has("MaximumPrice")) filters.price_max = legacy["MaximumPrice"].asInteger();
    static constexpr const char* clicks[] = {"blank", "any", "touch", "sit", "buy", "pay", "open", "play", "open_media", "zoom"};
    for (size_t i = 0; i < sizeof(clicks) / sizeof(clicks[0]); ++i)
    {
        if (legacy["ClickAction"].asString() == clicks[i]) filters.click_action_filter = static_cast<U8>(i);
    }
    LLSD state;
    state["filters"] = filters.asLLSD();
    // A minimum-distance annulus and neighbour-region inclusion have no exact new
    // equivalent; migrate the compatible maximum radius and current-parcel choice.
    state["scope"] = legacy["OnlyCurrentParcel"].asBoolean() ? "parcel" :
        legacy["FilterDistance"].asBoolean() ? "nearby" : "region";
    state["radius"] = legacy.has("MaximumDistance") ? llclamp(legacy["MaximumDistance"].asInteger(), 1, 4096) :
        legacy["FilterDistance"].asBoolean() ? 4096 : 64;
    return state;
}

SLAreaSearchFilters SLAreaSearchFilters::fromLLSD(const LLSD& sd)
{
    SLAreaSearchFilters filters = defaults();
    if (!sd.isMap())
    {
        return filters;
    }

    const LLSD& state_map = sd["states"];
    for (size_t i = 0; i < filters.states.size(); ++i)
    {
        const LLSD& value = state_map[sl_area_search_property_key((SLAreaSearchProperty)i)];
        if (value.isInteger() && value.asInteger() >= (S32)SLTriState::Any && value.asInteger() <= (S32)SLTriState::Hide)
        {
            filters.states[i] = (SLTriState)value.asInteger();
        }
    }
    if (sd.has("price_min"))
    {
        filters.price_min = sd["price_min"].asInteger();
    }
    if (sd.has("price_max"))
    {
        filters.price_max = sd["price_max"].asInteger();
    }
    if (const S32 click = sd["click_action"].asInteger(); click >= 0 && click <= 255)
    {
        filters.click_action_filter = (U8)click;
    }
    return filters;
}

bool SLAreaSearchText::set(const std::string& query, SLAreaSearchField field, bool regex)
{
    if (regex && !query.empty())
    {
        try
        {
            mPattern.assign(query, boost::regex::perl | boost::regex::icase);
        }
        catch (const std::exception&)
        {
            return false;
        }
    }
    mQuery = query;
    mField = field;
    mRegex = regex;
    return true;
}

bool SLAreaSearchText::matchesField(const std::string& text) const
{
    if (!mRegex)
    {
        return !boost::ifind_first(text, mQuery).empty();
    }

    try
    {
        return boost::regex_match(text, mPattern);
    }
    catch (const std::exception& e)
    {
        // A pattern can be valid and still too complex for one input (boost throws on
        // runaway backtracking). Keep the object listed rather than hide it on an error.
        LL_WARNS_ONCE("AreaSearch") << "Regex failed on an object's text: " << e.what() << LL_ENDL;
        return true;
    }
}

bool SLAreaSearchText::matches(const SLAreaSearchStrings& strings) const
{
    if (mQuery.empty())
    {
        return true;
    }

    switch (mField)
    {
    case SLAreaSearchField::Name:        return matchesField(strings.name);
    case SLAreaSearchField::Description: return matchesField(strings.description);
    case SLAreaSearchField::Owner:       return matchesField(strings.owner);
    case SLAreaSearchField::Group:       return matchesField(strings.group);
    case SLAreaSearchField::Creator:     return matchesField(strings.creator);
    case SLAreaSearchField::LastOwner:   return matchesField(strings.last_owner);
    case SLAreaSearchField::All:
    default:
        return matchesField(strings.name) || matchesField(strings.description) || matchesField(strings.owner)
            || matchesField(strings.group) || matchesField(strings.creator) || matchesField(strings.last_owner);
    }
}
