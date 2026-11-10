/**
 * @file slfloaterquickprefs.h
 * @brief Quick Preferences: the hover height floater plus the settings people change
 *        mid-session (#1256: avatar count, object detail, name tags, sky and water)
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

#ifndef SL_FLOATERQUICKPREFS_H
#define SL_FLOATERQUICKPREFS_H

#include <string>

#include "llfloaterhoverheight.h"

class LLComboBox;

class SLFloaterQuickPrefs : public LLFloaterHoverHeight
{
public:
    SLFloaterQuickPrefs(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

private:
    void onMaxAvatarsCommit();
    void updateMaxAvatarsText();
    void updateUpscalerAvailability();

    // Sky and water: "Region default", then the sky / water settings in the inventory
    // (and the Library), applied as the local environment
    void loadEnvironmentPresets();
    void selectEnvironmentPresets();
    void onEnvironmentCommit(LLComboBox* combo);

    LLComboBox* mSkyCombo = nullptr;
    LLComboBox* mWaterCombo = nullptr;
    LLComboBox* mUpscaleCombo = nullptr;
    std::string mUpscaleToolTip;
    boost::signals2::scoped_connection mMaxAvatarsConn;
    boost::signals2::scoped_connection mEnvChangedConn;
};

#endif // SL_FLOATERQUICKPREFS_H
