/**
 * @file llfloatergridmanager.h
 * @brief List of known grids: add an OpenSim grid by its login address, refresh, remove.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLFLOATERGRIDMANAGER_H
#define LL_LLFLOATERGRIDMANAGER_H

#include "llfloater.h"

#include <boost/signals2.hpp>

class LLButton;
class LLLineEditor;
class LLScrollListCtrl;
class LLTextBox;

class LLFloaterGridManager : public LLFloater
{
public:
    LLFloaterGridManager(const LLSD& key);
    ~LLFloaterGridManager();

    bool postBuild() override;

private:
    void refreshList();
    void updateButtons();
    void setStatus(const std::string& text, bool is_error);

    void onAdd();
    void onRefresh();
    void onRemove();
    void startFetch(const std::string& uri, bool is_refresh);
    void onFetchDone(bool ok, const std::string& grid_name, const std::string& message, bool is_refresh);

    LLScrollListCtrl* mGridList;
    LLLineEditor* mUriEdit;
    LLButton* mAddBtn;
    LLButton* mRefreshBtn;
    LLButton* mRemoveBtn;
    LLTextBox* mStatusText;

    bool mBusy;
    boost::signals2::scoped_connection mGridsChangedConnection;
};

#endif
