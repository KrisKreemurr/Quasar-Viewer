/**
 * @file llfloaterbulkupload.cpp
 * @author Andrey Kleshchev
 * @brief LLFloaterBulkUpload class implementation
 *
 * $LicenseInfo:firstyear=2024&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2024, Linden Research, Inc.
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

#include "llfloaterbulkupload.h"

#include "llagent.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llstatusbar.h"    // can_afford_transaction()
#include "lltextbox.h"
#include "lluploadqueue.h"
#include "llviewercontrol.h"
#include "llviewermenufile.h"

LLFloaterBulkUpload::LLFloaterBulkUpload(const LLSD& key)
:   LLFloater(key)
{
    mDestinationFolderId = key["dest"];
}

LLFloaterBulkUpload::~LLFloaterBulkUpload()
{
}

bool LLFloaterBulkUpload::postBuild()
{
    LLTimer timer;
    childSetAction("upload_btn", [this](void*) { onClickUpload(); }, this);
    childSetAction("cancel_btn", [this](void*) { onClickCancel(); }, this);

    mCountLabel = getChild<LLTextBox>("number_of_items", true);
    mCostLabel = getChild<LLTextBox>("upload_cost", true);
    mStatusLabel = getChild<LLTextBox>("scan_status", true);
    mFileList = getChild<LLScrollListCtrl>("file_list", true);

    mCheckboxPanel = getChild<LLPanel>("checkbox_panel", true);
    mLinkPanel = getChild<LLPanel>("link_panel", true);
    mWarningPanel = getChild<LLPanel>("warning_panel", true);

    mCheckboxUpload2K = getChild<LLUICtrl>("upload_2k");
    mCheckboxUpload2K->setCommitCallback([this](LLUICtrl* ctrl, const LLSD& data) { onUpload2KCheckBox(); });

    mAllow2kTextures = gSavedSettings.getBOOL("BulkUpload2KTextures");
    mCheckboxUpload2K->setValue(!mAllow2kTextures);

    // The list at once, from the names alone; the files are read on the upload pool.
    const LLSD& files = getKey()["files"];
    if (files.isArray())
    {
        for (LLSD::array_const_iterator it = files.beginArray(); it != files.endArray(); ++it)
        {
            const size_t index = mScans.size();
            auto scan = std::make_shared<LLBulkUpload::FileScan>();
            scan->mFile = it->asString();
            const bool needs_reading = LLBulkUpload::classify(*scan);
            mScans.push_back(scan);
            mReady.push_back(needs_reading ? 0 : 1);
            mScanned += needs_reading ? 0 : 1;

            LLSD row;
            row["value"] = (S32)index;
            row["columns"][0]["column"] = "name";
            row["columns"][0]["value"] = gDirUtilp->getBaseFileName(scan->mFile, false);
            row["columns"][1]["column"] = "kind";
            row["columns"][1]["value"] = std::string();
            row["columns"][2]["column"] = "status";
            row["columns"][2]["value"] = getString("status_reading");
            row["columns"][3]["column"] = "cost";
            row["columns"][3]["value"] = std::string();
            mFileList->addElement(row);
            refreshRow(index);
        }
    }

    mScanTimer.reset();
    pumpScans();
    update();

    mScanMainMs = timer.getElapsedTimeF64() * 1000.0;
    LL_INFOS("UploadQueue") << "Bulk upload: list of " << mScans.size() << " file(s) opened in "
                            << llformat("%.2f", mScanMainMs) << " ms (main thread); " << (mScans.size() - mScanned)
                            << " to read on the upload pool" << LL_ENDL;

    return LLFloater::postBuild();
}

void LLFloaterBulkUpload::pumpScans()
{
    while (mScansInFlight < SCANS_IN_FLIGHT && mNextScan < mScans.size())
    {
        const size_t index = mNextScan++;
        if (mReady[index])
        {
            continue;
        }
        std::shared_ptr<LLBulkUpload::FileScan> scan = mScans[index];
        LLHandle<LLFloater> handle = getHandle();
        ++mScansInFlight;
        const bool posted = LLUploadQueue::instance().runOnWorker(
            [scan]() { LLBulkUpload::scanOnWorker(*scan); },
            [handle, index]()
            {
                if (LLFloaterBulkUpload* self = static_cast<LLFloaterBulkUpload*>(handle.get()))
                {
                    self->onScanned(index);
                }
            });
        if (!posted)
        {
            // no worker (quitting): the file is not read here
            --mScansInFlight;
            scan->mKind = LLBulkUpload::KIND_ERROR;
            scan->mError = "the viewer is shutting down";
            mReady[index] = 1;
            ++mScanned;
            refreshRow(index);
        }
    }
}

void LLFloaterBulkUpload::onScanned(size_t index)
{
    LLTimer timer;
    mScansInFlight = llmax(mScansInFlight - 1, 0);
    if (index < mReady.size() && !mReady[index])
    {
        mReady[index] = 1;
        ++mScanned;
        LL_INFOS("UploadQueue") << "Bulk upload: read " << mScans[index]->mFile << " on the upload pool in "
                                << llformat("%.1f", mScans[index]->mWorkerMs) << " ms" << LL_ENDL;
        refreshRow(index);
    }
    pumpScans();
    update();
    mScanMainMs += timer.getElapsedTimeF64() * 1000.0;

    if (scanComplete())
    {
        LL_INFOS("UploadQueue") << "Bulk upload: read " << mScans.size() << " file(s) in "
                                << llformat("%.2f", mScanTimer.getElapsedTimeF64()) << " s; main thread "
                                << llformat("%.2f", mScanMainMs) << " ms in total for the list; " << mUploadCount
                                << " item(s), L$" << mUploadCost << LL_ENDL;
        static LLCachedControl<bool> sPowerfulWizard(gSavedSettings, "SlipstreamPowerfulWizard", false);
        if (sPowerfulWizard && mUploadCost == 0 && mUploadCount > 0)
        {
            onClickUpload();   // nothing to charge: no confirmation needed
        }
    }
}

void LLFloaterBulkUpload::refreshRow(size_t index)
{
    LLScrollListItem* item = mFileList ? mFileList->getItem(LLSD((S32)index)) : nullptr;
    if (!item || index >= mScans.size() || !mReady[index])
    {
        return;
    }
    const LLBulkUpload::FileScan& scan = *mScans[index];
    const LLBulkUpload::Quote q = LLBulkUpload::quote(scan, mAllow2kTextures);

    std::string kind;
    std::string status;
    switch (scan.mKind)
    {
    case LLBulkUpload::KIND_TEXTURE:
        kind = getString("kind_texture");
        status = llformat("%d x %d", scan.mWidth, scan.mHeight);
        break;
    case LLBulkUpload::KIND_SOUND:
        kind = getString("kind_sound");
        status = getString("status_ready");
        break;
    case LLBulkUpload::KIND_ANIMATION:
        kind = getString("kind_animation");
        status = getString("status_ready");
        break;
    case LLBulkUpload::KIND_MATERIAL:
    {
        kind = getString("kind_material");
        LLStringUtil::format_map_t args;
        args["[COUNT]"] = llformat("%d", (S32)scan.mMaterials.size());
        status = getString("status_materials", args);
        break;
    }
    case LLBulkUpload::KIND_ERROR:
        status = scan.mError;
        break;
    default:
        status = getString("status_unsupported");
        break;
    }
    if (q.mItems == 0 && scan.mKind != LLBulkUpload::KIND_ERROR && scan.mKind != LLBulkUpload::KIND_UNSUPPORTED)
    {
        status = getString("status_unsupported");
    }

    item->getColumn(1)->setValue(kind);
    item->getColumn(2)->setValue(status);
    item->getColumn(3)->setValue(q.mItems > 0 ? llformat("%d", q.mCost) : std::string());
}

void LLFloaterBulkUpload::update()
{
    S32 cost = 0;
    S32 count = 0;
    S32 skipped = 0;
    bool has_2k = false;
    for (size_t i = 0; i < mScans.size(); ++i)
    {
        if (!mReady[i])
        {
            continue;
        }
        const LLBulkUpload::Quote q = LLBulkUpload::quote(*mScans[i], mAllow2kTextures);
        cost += q.mCost;
        count += q.mItems;
        skipped += q.mItems ? 0 : 1;
        has_2k |= LLBulkUpload::quote(*mScans[i], true).mHas2K;
    }
    mUploadCost = cost;
    mUploadCount = count;
    mHas2kTextures = has_2k;

    mCountLabel->setTextArg("[COUNT]", llformat("%d", mUploadCount));
    mCostLabel->setTextArg("[COST]", llformat("%d", mUploadCost));

    mCheckboxPanel->setVisible(mHas2kTextures);
    mLinkPanel->setVisible(mHas2kTextures);
    mWarningPanel->setVisible(mHas2kTextures);

    std::string status;
    LLStringUtil::format_map_t args;
    if (!scanComplete())
    {
        args["[DONE]"] = llformat("%d", (S32)mScanned);
        args["[TOTAL]"] = llformat("%d", (S32)mScans.size());
        status = getString("scan_progress", args);
    }
    else if (mUploadCount == 0)
    {
        status = getString("nothing_to_upload");
    }
    else if (skipped > 0)
    {
        args["[SKIPPED]"] = llformat("%d", skipped);
        status = getString("scan_skipped", args);
    }
    mStatusLabel->setValue(status);

    getChildView("upload_btn")->setEnabled(scanComplete() && mUploadCount > 0 && !mStarted);
}

void LLFloaterBulkUpload::onUpload2KCheckBox()
{
    mAllow2kTextures = !mCheckboxUpload2K->getValue().asBoolean();
    gSavedSettings.setBOOL("BulkUpload2KTextures", mAllow2kTextures);

    // the sizes are known: only the quote changes
    for (size_t i = 0; i < mScans.size(); ++i)
    {
        refreshRow(i);
    }
    update();
}

void LLFloaterBulkUpload::onClickUpload()
{
    if (!scanComplete() || mStarted || mUploadCount <= 0)
    {
        return;
    }
    if (gAgent.getRegionCapability("NewFileAgentInventory").empty())
    {
        mStatusLabel->setValue(getString("no_upload_capability"));
        return;
    }
    if (!can_afford_transaction(mUploadCost))
    {
        LLStringUtil::format_map_t args;
        args["[COST]"] = llformat("%d", mUploadCost);
        mStatusLabel->setValue(getString("cannot_afford", args));
        return;
    }
    mStarted = true;
    // The confirmed uploads go to the background pipeline; this window is done.
    LLBulkUpload::start(mScans, mAllow2kTextures, mDestinationFolderId);
    closeFloater();
}


void LLFloaterBulkUpload::onClickCancel()
{
    closeFloater();
}
