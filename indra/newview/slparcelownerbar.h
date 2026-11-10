/**
 * @file slparcelownerbar.h
 * @brief Area Search (#1321): a parcel's used land impact as one bar split by owner, with a
 *        legend line. Clicking an owner's segment or legend entry picks that owner (commit);
 *        clicking the picked owner again clears it.
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

#ifndef SL_SLPARCELOWNERBAR_H
#define SL_SLPARCELOWNERBAR_H

#include "lluictrl.h"
#include "slparcelsummary.h"

class SLParcelOwnerBar final : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params();
    };

    SLParcelOwnerBar(const Params& p);

    // owner_labels runs parallel to summary.owners (names already resolved and RLV-hidden).
    void setSummary(const SLParcelSummary& summary, const std::vector<std::string>& owner_labels);

    const LLUUID& getPickedOwner() const { return mPicked; }
    void setPickedOwner(const LLUUID& owner);

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;

private:
    // Where each owner was drawn (bar segment and legend entry), rebuilt every draw.
    struct Hit
    {
        LLRect rect;
        LLUUID owner;
    };
    const Hit* hitAt(S32 x, S32 y) const;

    SLParcelSummary mSummary;
    std::vector<std::string> mLabels;
    LLUUID mPicked;
    std::vector<Hit> mHits;
};

#endif // SL_SLPARCELOWNERBAR_H
