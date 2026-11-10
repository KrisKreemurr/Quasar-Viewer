/**
 * @file llfloatergridmanager.cpp
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

#include "llviewerprecompiledheaders.h"

#include "llfloatergridmanager.h"

#include "llbutton.h"
#include "llgridinfo.h"
#include "lllineeditor.h"
#include "llpanellogin.h"
#include "llscrolllistctrl.h"
#include "llstartup.h"
#include "lltextbox.h"
#include "lltrans.h"
#include "lluicolortable.h"
#include "llviewernetwork.h"

LLFloaterGridManager::LLFloaterGridManager(const LLSD& key)
:   LLFloater(key),
    mGridList(nullptr),
    mUriEdit(nullptr),
    mAddBtn(nullptr),
    mRefreshBtn(nullptr),
    mRemoveBtn(nullptr),
    mStatusText(nullptr),
    mBusy(false)
{
}

LLFloaterGridManager::~LLFloaterGridManager()
{
}

bool LLFloaterGridManager::postBuild()
{
    mGridList = getChild<LLScrollListCtrl>("grid_list");
    mUriEdit = getChild<LLLineEditor>("uri_edit");
    mAddBtn = getChild<LLButton>("add_btn");
    mRefreshBtn = getChild<LLButton>("refresh_btn");
    mRemoveBtn = getChild<LLButton>("remove_btn");
    mStatusText = getChild<LLTextBox>("status_text");

    mGridList->setCommitCallback(boost::bind(&LLFloaterGridManager::updateButtons, this));
    mUriEdit->setCommitCallback(boost::bind(&LLFloaterGridManager::onAdd, this));
    mUriEdit->setKeystrokeCallback([this](LLLineEditor*, void*) { updateButtons(); }, nullptr);
    mAddBtn->setCommitCallback(boost::bind(&LLFloaterGridManager::onAdd, this));
    mRefreshBtn->setCommitCallback(boost::bind(&LLFloaterGridManager::onRefresh, this));
    mRemoveBtn->setCommitCallback(boost::bind(&LLFloaterGridManager::onRemove, this));

    mGridsChangedConnection = LLGridManager::getInstance()->setGridsChangedCallback(boost::bind(&LLFloaterGridManager::refreshList, this));

    refreshList();
    return true;
}

void LLFloaterGridManager::refreshList()
{
    LLGridManager* grids = LLGridManager::getInstance();
    const std::string selected = mGridList->getSelectedValue().asString();

    mGridList->deleteAllItems();
    std::map<std::string, std::string> known = grids->getKnownGrids();
    for (std::map<std::string, std::string>::const_iterator it = known.begin(); it != known.end(); ++it)
    {
        std::vector<std::string> uris;
        grids->getLoginURIs(it->first, uris);

        LLSD row;
        row["value"] = it->first;
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = it->second;
        row["columns"][1]["column"] = "platform";
        row["columns"][1]["value"] = grids->getPlatform(it->first);
        row["columns"][2]["column"] = "login";
        row["columns"][2]["value"] = uris.empty() ? std::string() : uris.front();
        mGridList->addElement(row);
    }
    if (selected.empty() || !mGridList->setSelectedByValue(selected, true))
    {
        mGridList->selectFirstItem();
    }
    updateButtons();
}

void LLFloaterGridManager::updateButtons()
{
    LLGridManager* grids = LLGridManager::getInstance();
    const std::string selected = mGridList->getSelectedValue().asString();
    const bool user_grid = !selected.empty() && !grids->isSystemGrid(selected);

    // switching the grid under a running session is not possible
    const bool in_use = selected == grids->getGrid() && !LLPanelLogin::isShown();

    mAddBtn->setEnabled(!mBusy && !mUriEdit->getText().empty());
    mRefreshBtn->setEnabled(!mBusy && user_grid && !grids->getGridBaseURI(selected).empty());
    mRemoveBtn->setEnabled(!mBusy && user_grid && !in_use);
}

void LLFloaterGridManager::setStatus(const std::string& text, bool is_error)
{
    mStatusText->setColor(LLUIColorTable::instance().getColor(is_error ? "Red" : "LabelTextColor"));
    mStatusText->setText(text);
}

void LLFloaterGridManager::onAdd()
{
    if (mBusy || mUriEdit->getText().empty())
    {
        return;
    }
    startFetch(mUriEdit->getText(), false);
}

void LLFloaterGridManager::onRefresh()
{
    const std::string base = LLGridManager::getInstance()->getGridBaseURI(mGridList->getSelectedValue().asString());
    if (!mBusy && !base.empty())
    {
        startFetch(base, true);
    }
}

void LLFloaterGridManager::onRemove()
{
    if (mBusy)
    {
        return;
    }
    const std::string selected = mGridList->getSelectedValue().asString();
    LLGridManager* grids = LLGridManager::getInstance();
    if (selected.empty() || grids->isSystemGrid(selected) || (selected == grids->getGrid() && !LLPanelLogin::isShown()))
    {
        return;
    }
    const bool was_current = selected == grids->getGrid();
    grids->removeGrid(selected);
    if (was_current)
    {
        // removeGrid fell back to Second Life; bring the login screen along
        LLPanelLogin::selectGrid(grids->getGrid());
    }
    setStatus(std::string(), false);
}

void LLFloaterGridManager::startFetch(const std::string& uri, bool is_refresh)
{
    mBusy = true;
    updateButtons();
    setStatus(LLTrans::getString("GridManagerAdding"), false);

    // The floater may be closed before the answer (or the timeout) arrives
    LLHandle<LLFloater> handle = getHandle();
    LLGridInfo::addGridFromURI(uri, [handle, is_refresh](bool ok, const std::string& grid_name, const std::string& message)
    {
        if (LLFloaterGridManager* floater = dynamic_cast<LLFloaterGridManager*>(handle.get()))
        {
            floater->onFetchDone(ok, grid_name, message, is_refresh);
        }
        else if (ok && !is_refresh && LLPanelLogin::isShown())
        {
            LLPanelLogin::selectGrid(grid_name);
        }
    });
}

void LLFloaterGridManager::onFetchDone(bool ok, const std::string& grid_name, const std::string& message, bool is_refresh)
{
    mBusy = false;

    if (!ok)
    {
        setStatus(message, true);
        updateButtons();
        return;
    }

    LLStringUtil::format_map_t args;
    args["[GRID]"] = LLGridManager::getInstance()->getGridLabel(grid_name);
    setStatus(message.empty() ? LLTrans::getString(is_refresh ? "GridManagerRefreshed" : "GridManagerAdded", args) : message,
              !message.empty());

    if (!is_refresh)
    {
        mUriEdit->clear();
        // On the login screen a freshly added grid becomes the selected one
        if (LLPanelLogin::isShown())
        {
            LLPanelLogin::selectGrid(grid_name);
        }
    }
    refreshList();
    mGridList->setSelectedByValue(grid_name, true);
    updateButtons();
}
