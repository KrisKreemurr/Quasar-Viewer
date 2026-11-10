/**
 * @file slparcelownerbar.cpp
 * @brief Area Search (#1321): the parcel's land impact bar, split by owner.
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

#include "slparcelownerbar.h"
#include <string_view>

#include "llfontgl.h"
#include "llrender2dutils.h"
#include "lltrans.h"
#include "lluicolortable.h"
#include "llwindow.h"

static LLDefaultChildRegistry::Register<SLParcelOwnerBar> r("sl_parcel_owner_bar");

namespace
{
constexpr S32 BAR_HEIGHT = 10;
constexpr S32 LEGEND_GAP = 4;   // between the bar and the legend line
constexpr S32 SWATCH = 8;
constexpr S32 ENTRY_GAP = 12;   // between legend entries

LLColor4 ownerColor(size_t i)
{
    static const char* names[] = { "AreaSearchOwner1", "AreaSearchOwner2", "AreaSearchOwner3", "AreaSearchOwner4", "AreaSearchOwner5" };
    return LLUIColorTable::instance().getColor(names[i % LL_ARRAY_SIZE(names)]).get();
}
}

SLParcelOwnerBar::Params::Params()
{
    mouse_opaque = true;
}

SLParcelOwnerBar::SLParcelOwnerBar(const Params& p)
:   LLUICtrl(p)
{
}

void SLParcelOwnerBar::setSummary(const SLParcelSummary& summary, const std::vector<std::string>& owner_labels)
{
    bool same_labels = mLabels.size() == summary.owners.size();
    for (size_t i = 0; same_labels && i < mLabels.size(); ++i)
    {
        const std::string_view label = i < owner_labels.size() ? std::string_view(owner_labels[i]) : std::string_view();
        same_labels = std::string_view(mLabels[i]) == label;
    }
    if (mSummary == summary && same_labels)
    {
        return;
    }
    mSummary = summary;
    mLabels = owner_labels;
    mLabels.resize(mSummary.owners.size());
    onRetainedDirty();
}

void SLParcelOwnerBar::setPickedOwner(const LLUUID& owner)
{
    if (mPicked == owner)
    {
        return;
    }
    mPicked = owner;
    onRetainedDirty();
}

const SLParcelOwnerBar::Hit* SLParcelOwnerBar::hitAt(S32 x, S32 y) const
{
    for (const Hit& hit : mHits)
    {
        if (hit.rect.pointInRect(x, y))
        {
            return &hit;
        }
    }
    return nullptr;
}

bool SLParcelOwnerBar::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (const Hit* hit = hitAt(x, y))
    {
        setPickedOwner(mPicked == hit->owner ? LLUUID::null : hit->owner);
        onCommit();
        return true;
    }
    return LLUICtrl::handleMouseDown(x, y, mask);
}

bool SLParcelOwnerBar::handleHover(S32 x, S32 y, MASK mask)
{
    if (hitAt(x, y))
    {
        getWindow()->setCursor(UI_CURSOR_HAND);
        return true;
    }
    return LLUICtrl::handleHover(x, y, mask);
}

void SLParcelOwnerBar::draw()
{
    mHits.clear();

    const S32 width = getRect().getWidth();
    const S32 height = getRect().getHeight();
    const LLRect bar(0, height, width, height - BAR_HEIGHT);
    const LLColor4 others_color = LLUIColorTable::instance().getColor("AreaSearchOwnersOther").get();
    const LLColor4 not_loaded_color = LLUIColorTable::instance().getColor("AreaSearchNotLoaded").get();
    const LLColor4 free_color = LLUIColorTable::instance().getColor("AreaSearchFree").get();
    const LLColor4 text_color = LLUIColorTable::instance().getColor("LabelTextColor").get();
    const LLColor4 picked_color = LLUIColorTable::instance().getColor("LabelSelectedColor").get();

    S32 summed = mSummary.others + mSummary.not_loaded;
    for (const SLOwnerShare& share : mSummary.owners)
    {
        summed += share.land_impact;
    }
    const S32 free_li = llmax(0, mSummary.capacity - summed);
    // The scale is the capacity, or what is counted when a parcel is over its capacity.
    const S32 scale = llmax(mSummary.capacity, summed);

    gl_rect_2d(bar, free_color, true);
    if (scale > 0)
    {
        S32 left = bar.mLeft;
        auto segment = [&](S32 land_impact, const LLColor4& color) -> LLRect
        {
            const S32 right = llmin(bar.mRight, left + ll_round((F32)land_impact * (F32)width / (F32)scale));
            const LLRect rect(left, bar.mTop, right, bar.mBottom);
            if (right > left)
            {
                gl_rect_2d(rect, color, true);
            }
            left = right;
            return rect;
        };
        for (size_t i = 0; i < mSummary.owners.size(); ++i)
        {
            const LLRect rect = segment(mSummary.owners[i].land_impact, ownerColor(i));
            mHits.push_back({ rect, mSummary.owners[i].owner });
            if (mSummary.owners[i].owner == mPicked && rect.getWidth() > 0)
            {
                gl_rect_2d(rect, picked_color, false);
            }
        }
        segment(mSummary.others, others_color);
        segment(mSummary.not_loaded, not_loaded_color);
    }

    // Legend: "■ name LI" per owner, then others, not loaded yet and free. Those last three
    // always show; owner names shorten, and the smallest owners drop out, to leave them room.
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const S32 line_top = bar.mBottom - LEGEND_GAP;
    const S32 baseline = line_top - (S32)font->getLineHeight() + 2;
    S32 x = 0;
    auto entry = [&](const LLColor4& swatch_color, const std::string& text, const LLColor4& color) -> LLRect
    {
        const S32 text_width = font->getWidth(text);
        if (x + SWATCH + 3 + text_width > width)
        {
            return LLRect();
        }
        const S32 swatch_top = baseline + SWATCH + 1;
        gl_rect_2d(x, swatch_top, x + SWATCH, swatch_top - SWATCH, swatch_color, true);
        font->renderUTF8(text, 0, (F32)(x + SWATCH + 3), (F32)baseline, color, LLFontGL::LEFT, LLFontGL::BASELINE, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
        const LLRect rect(x, line_top, x + SWATCH + 3 + text_width, line_top - (S32)font->getLineHeight());
        x = rect.mRight + ENTRY_GAP;
        return rect;
    };

    std::vector<std::pair<LLColor4, std::string>> totals;
    auto total = [&totals](const LLColor4& color, const char* string_name, S32 land_impact)
    {
        LLStringUtil::format_map_t args;
        args["[LI]"] = llformat("%d", land_impact);
        totals.emplace_back(color, LLTrans::getString(string_name, args));
    };
    if (mSummary.others > 0)
    {
        total(others_color, "AreaSearchOwnersOther", mSummary.others);
    }
    if (mSummary.not_loaded > 0)
    {
        total(not_loaded_color, "AreaSearchNotLoaded", mSummary.not_loaded);
    }
    if (mSummary.capacity > 0)
    {
        total(free_color, "AreaSearchFree", free_li);
    }
    S32 totals_width = 0;
    for (const auto& [color, text] : totals)
    {
        totals_width += SWATCH + 3 + font->getWidth(text) + ENTRY_GAP;
    }

    std::vector<std::string> suffixes;
    for (const SLOwnerShare& share : mSummary.owners)
    {
        suffixes.push_back(llformat("%d", share.land_impact));
    }
    const std::vector<std::string> owner_entries = sl_fit_owner_labels(mLabels, suffixes, width - totals_width, SWATCH + 3 + ENTRY_GAP,
                                                                       [font](const std::string& text) { return font->getWidth(text); });
    for (size_t i = 0; i < owner_entries.size(); ++i)
    {
        const bool picked = mSummary.owners[i].owner == mPicked;
        const LLRect rect = entry(ownerColor(i), owner_entries[i], picked ? picked_color : text_color);
        if (rect.notEmpty())
        {
            mHits.push_back({ rect, mSummary.owners[i].owner });
        }
    }
    for (const auto& [color, text] : totals)
    {
        entry(color, text, text_color);
    }

    LLUICtrl::draw();
}
