/**
 * @file slareasearchfilter_test.cpp
 * @brief Area Search (#1321): the Any / Only / Hide filters, their saved form, and the one
 *        search box (substring or whole-field regex, one field or all).
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

#include "../slareasearchfilter.h"

#include "../test/lltut.h"

namespace
{
    using P = SLAreaSearchProperty;

    SLAreaSearchFacts plain()
    {
        return SLAreaSearchFacts{};
    }

    SLAreaSearchFacts with(P property)
    {
        SLAreaSearchFacts facts;
        facts.has.set((size_t)property);
        return facts;
    }

    SLAreaSearchFacts forSale(S32 price)
    {
        SLAreaSearchFacts facts = with(P::ForSale);
        facts.price = price;
        return facts;
    }

    SLAreaSearchStrings strings()
    {
        SLAreaSearchStrings s;
        s.name = "Coffee table";
        s.description = "Oak, stacked";
        s.owner = "Juno Vex";
        s.group = "Moonlit Cove";
        s.creator = "Maple Rowe";
        s.last_owner = "Maple Rowe";
        return s;
    }
}

namespace tut
{
    struct areasearchfilter_data {};
    typedef test_group<areasearchfilter_data> areasearchfilter_test;
    typedef areasearchfilter_test::object areasearchfilter_object;
    tut::areasearchfilter_test areasearchfilter("SLAreaSearchFilter");

    // Any passes everything; Only needs the property; Hide needs it absent.
    template<> template<>
    void areasearchfilter_object::test<1>()
    {
        ensure("any, has", sl_tri_state_passes(SLTriState::Any, true));
        ensure("any, has not", sl_tri_state_passes(SLTriState::Any, false));
        ensure("only, has", sl_tri_state_passes(SLTriState::Only, true));
        ensure("only, has not", !sl_tri_state_passes(SLTriState::Only, false));
        ensure("hide, has", !sl_tri_state_passes(SLTriState::Hide, true));
        ensure("hide, has not", sl_tri_state_passes(SLTriState::Hide, false));
    }

    // Defaults: attachments and child prims hidden, everything else Any.
    template<> template<>
    void areasearchfilter_object::test<2>()
    {
        const SLAreaSearchFilters filters = SLAreaSearchFilters::defaults();
        for (size_t i = 0; i < (size_t)P::Count; ++i)
        {
            const P p = (P)i;
            const SLTriState expected = (p == P::Attachment || p == P::ChildPrim) ? SLTriState::Hide : SLTriState::Any;
            ensure(std::string("default of ") + sl_area_search_property_key(p), filters.get(p) == expected);
        }
        ensure("an attachment is hidden", !filters.passes(with(P::Attachment)));
        ensure("a child prim is hidden", !filters.passes(with(P::ChildPrim)));
        ensure("a plain object passes", filters.passes(plain()));
        ensure("a physical object passes", filters.passes(with(P::Physical)));
    }

    // The price range applies only when For sale is Only.
    template<> template<>
    void areasearchfilter_object::test<3>()
    {
        SLAreaSearchFilters filters = SLAreaSearchFilters::defaults();
        filters.price_min = 10;
        filters.price_max = 100;

        filters.set(P::ForSale, SLTriState::Only);
        ensure("only: in range", filters.passes(forSale(50)));
        ensure("only: below", !filters.passes(forSale(5)));
        ensure("only: above", !filters.passes(forSale(500)));
        ensure("only: not for sale", !filters.passes(plain()));

        filters.set(P::ForSale, SLTriState::Hide);
        ensure("hide: not for sale passes", filters.passes(plain()));
        ensure("hide: for sale fails whatever the price", !filters.passes(forSale(50)));

        filters.set(P::ForSale, SLTriState::Any);
        ensure("any: price out of range still passes", filters.passes(forSale(500)));
        ensure("any: not for sale passes", filters.passes(plain()));
    }

    // Click action filter, in the combo's encoding: 1 any, 2 touch, n >= 3 action n - 2.
    template<> template<>
    void areasearchfilter_object::test<4>()
    {
        SLAreaSearchFilters filters = SLAreaSearchFilters::defaults();
        SLAreaSearchFacts touch = plain();
        touch.handles_touch = true;
        SLAreaSearchFacts sit = plain();
        sit.click_action = 1;
        SLAreaSearchFacts buy = plain();
        buy.click_action = 2;

        ensure("none: no click action passes", filters.passes(plain()));

        filters.click_action_filter = 1;
        ensure("any: touch", filters.passes(touch));
        ensure("any: sit", filters.passes(sit));
        ensure("any: nothing", !filters.passes(plain()));

        filters.click_action_filter = 2;
        ensure("touch: touch", filters.passes(touch));
        ensure("touch: sit", !filters.passes(sit));

        filters.click_action_filter = 4;
        ensure("action 2: buy", filters.passes(buy));
        ensure("action 2: sit", !filters.passes(sit));
        ensure("action 2: touch", !filters.passes(touch));
    }

    // The saved form round-trips, and missing or bad values fall back to the defaults.
    template<> template<>
    void areasearchfilter_object::test<5>()
    {
        SLAreaSearchFilters filters = SLAreaSearchFilters::defaults();
        filters.set(P::Temporary, SLTriState::Only);
        filters.set(P::Attachment, SLTriState::Any);
        filters.set(P::Scripted, SLTriState::Hide);
        filters.price_min = 5;
        filters.price_max = 250;
        filters.click_action_filter = 3;

        const SLAreaSearchFilters back = SLAreaSearchFilters::fromLLSD(filters.asLLSD());
        ensure("round trip", back == filters);

        ensure("empty LLSD gives the defaults", SLAreaSearchFilters::fromLLSD(LLSD()) == SLAreaSearchFilters::defaults());

        LLSD bad = SLAreaSearchFilters::defaults().asLLSD();
        bad["states"]["attachment"] = 7;
        bad["states"]["physical"] = 1;
        const SLAreaSearchFilters read = SLAreaSearchFilters::fromLLSD(bad);
        ensure("out-of-range state falls back to its default", read.get(P::Attachment) == SLTriState::Hide);
        ensure("valid state is read", read.get(P::Physical) == SLTriState::Only);
    }

    // Substring search: case-insensitive, one field or any of them.
    template<> template<>
    void areasearchfilter_object::test<6>()
    {
        SLAreaSearchText text;
        ensure("valid", text.set("TaBlE", SLAreaSearchField::Name, false));
        ensure("name matches", text.matches(strings()));

        text.set("TaBlE", SLAreaSearchField::Owner, false);
        ensure("owner does not", !text.matches(strings()));

        text.set("TaBlE", SLAreaSearchField::All, false);
        ensure("all fields matches", text.matches(strings()));

        text.set("maple", SLAreaSearchField::All, false);
        ensure("all fields finds the creator", text.matches(strings()));

        text.set("maple", SLAreaSearchField::Group, false);
        ensure("group does not have it", !text.matches(strings()));
    }

    // A regex has to match the whole field, case-insensitively.
    template<> template<>
    void areasearchfilter_object::test<7>()
    {
        SLAreaSearchText text;
        ensure("valid", text.set("table", SLAreaSearchField::Name, true));
        ensure("part of the field is not enough", !text.matches(strings()));

        text.set(".*TABLE", SLAreaSearchField::Name, true);
        ensure("whole field matches", text.matches(strings()));

        text.set("juno.*", SLAreaSearchField::All, true);
        ensure("any field may match", text.matches(strings()));
    }

    // An invalid regex is refused and the previous query stays in force.
    template<> template<>
    void areasearchfilter_object::test<8>()
    {
        SLAreaSearchText text;
        ensure("valid", text.set("sofa", SLAreaSearchField::Name, false));
        ensure("sofa does not match", !text.matches(strings()));

        ensure("invalid regex refused", !text.set("(", SLAreaSearchField::Name, true));
        ensure("still the sofa query", !text.matches(strings()));
        ensure("not empty", !text.empty());
    }

    template<> template<>
    void areasearchfilter_object::test<10>()
    {
        ensure("no legacy profile for a new install", sl_area_search_migrate_legacy(LLSD()).isUndefined());
        LLSD old;
        old["OnlyPhysical"] = true;
        old["OnlyAttachments"] = true;
        old["ExcludeAttachments"] = false;
        old["OnlyCopiable"] = true;
        old["FilterForSale"] = true;
        old["MinimumPrice"] = 20;
        old["MaximumPrice"] = 70;
        old["ClickAction"] = "pay";
        old["OnlyCurrentParcel"] = true;
        const LLSD migrated = sl_area_search_migrate_legacy(old);
        const auto filters = SLAreaSearchFilters::fromLLSD(migrated["filters"]);
        ensure("physical selection preserved", filters.get(P::Physical) == SLTriState::Only);
        ensure("attachment selection preserved", filters.get(P::Attachment) == SLTriState::Only);
        ensure("copy selection preserved", filters.get(P::Copy) == SLTriState::Only);
        ensure("sale selection preserved", filters.get(P::ForSale) == SLTriState::Only);
        ensure_equals("minimum price", filters.price_min, 20);
        ensure_equals("maximum price", filters.price_max, 70);
        ensure_equals("pay action", (U32)filters.click_action_filter, 5u);
        ensure_equals("parcel scope", migrated["scope"].asString(), std::string("parcel"));
        old["ExcludePhysical"] = true;
        old["OnlyCurrentParcel"] = false;
        old["FilterDistance"] = true;
        old["MaximumDistance"] = 9999;
        const LLSD nearby = sl_area_search_migrate_legacy(old);
        ensure("old exclusion wins", SLAreaSearchFilters::fromLLSD(nearby["filters"]).get(P::Physical) == SLTriState::Hide);
        ensure_equals("nearby scope", nearby["scope"].asString(), std::string("nearby"));
        ensure_equals("bounded radius", nearby["radius"].asInteger(), 4096);
    }

    // An empty query matches everything.
    template<> template<>
    void areasearchfilter_object::test<9>()
    {
        SLAreaSearchText text;
        ensure("starts empty", text.empty());
        ensure("matches", text.matches(strings()));

        text.set("", SLAreaSearchField::Owner, true);
        ensure("empty regex is empty", text.empty());
        ensure("still matches", text.matches(strings()));
    }

    // Per-field texts (#1404) are ANDed with each other and with the box.
    template<> template<>
    void areasearchfilter_object::test<11>()
    {
        using F = SLAreaSearchField;
        SLAreaSearchFieldQueries fields;
        fields[0] = "table";   // name
        fields[4] = "MAPLE";   // creator
        SLAreaSearchText text;
        ensure("valid", text.set("", F::All, false, fields));
        ensure("fields alone are a query", !text.empty());
        ensure("name and creator both match", text.matches(strings()));

        fields[4] = "linden";
        text.set("", F::All, false, fields);
        ensure("one field failing rejects the object", !text.matches(strings()));

        fields[4] = "maple";
        fields[2] = "juno";    // owner
        text.set("oak", F::Description, false, fields);
        ensure("box and three fields all match", text.matches(strings()));

        text.set("sofa", F::Description, false, fields);
        ensure("the box is ANDed with the fields", !text.matches(strings()));

        text.set("", F::All, false, {});
        ensure("clearing the fields lists everything", text.empty() && text.matches(strings()));
    }

    // Field texts are whole-field regexes in regex mode; any invalid one refuses the lot.
    template<> template<>
    void areasearchfilter_object::test<12>()
    {
        using F = SLAreaSearchField;
        SLAreaSearchFieldQueries fields;
        fields[0] = "coffee.*";
        fields[3] = "moon.*";  // group
        SLAreaSearchText text;
        ensure("valid", text.set("", F::All, true, fields));
        ensure("both regexes match whole fields", text.matches(strings()));

        fields[0] = "coffee";
        text.set("", F::All, true, fields);
        ensure("a partial field is not enough", !text.matches(strings()));

        fields[0] = "coffee.*";
        ensure("invalid regex in a field is refused", !text.set("", F::All, true, { "(" }));
        text.set("", F::All, true, fields);
        fields[1] = "(";
        ensure("invalid field regex refused", !text.set("x", F::Name, true, fields));
        ensure("previous query stays", text.matches(strings()));
    }
}
