/**
 * @file slfloateranimationexplorer.cpp
 * @brief Animation Explorer floater: the animations played on our avatar, what played
 *        them, and stopping, revoking or blocking them (#1099, after Firestorm's)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * (LGPL v2.1, see slfloateranimationexplorer.h)
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "slfloateranimationexplorer.h"

#include <set>

#include "llagent.h"
#include "llanimationstates.h"
#include "llavatarname.h"
#include "llbutton.h"
#include "llclipboard.h"
#include "llfloaterbvhpreview.h"    // LLPreviewAnimation
#include "llfloaterreg.h"
#include "llkeyframemotion.h"
#include "llrender.h"
#include "llscriptruntimeperms.h"
#include "llscrolllistctrl.h"
#include "llselectmgr.h"
#include "lltextbox.h"
#include "lltoolmgr.h"              // MASK_ORBIT, MASK_PAN
#include "lltrans.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "slanimationlog.h"
#include "slassetblocklist.h"
#include "rlvactions.h"
#include "rlvcommon.h"
#include "rlvhandler.h"

namespace
{
    constexpr F32 PERIODIC_REFRESH_SECONDS = 1.0f;
    const char SEP = '|';

    std::string makeRowKey(const LLUUID& anim, const LLUUID& source)
    {
        return anim.asString() + SEP + source.asString();
    }

    bool parseRowKey(const std::string& key, LLUUID& anim, LLUUID& source)
    {
        const size_t pos = key.find(SEP);
        if (pos == std::string::npos)
        {
            return false;
        }
        anim.set(key.substr(0, pos));
        source.set(key.substr(pos + 1));
        return true;
    }

    void stopAnimation(const LLUUID& anim_id)
    {
        // here and for everyone else: the server stops it (as Stop Animating Me does)
        if (isAgentAvatarValid())
        {
            gAgentAvatarp->stopMotion(anim_id, true);
        }
        gAgent.sendAnimationRequest(anim_id, ANIM_REQUEST_STOP);
    }
}

SLFloaterAnimationExplorer::SLFloaterAnimationExplorer(const LLSD& key)
    : LLFloater(key)
{
}

SLFloaterAnimationExplorer::~SLFloaterAnimationExplorer()
{
    mLogConn.disconnect();
    mBlocklistConn.disconnect();
    mRlvConn.disconnect();
    for (auto& [id, conn] : mNameRequests)
    {
        conn.disconnect();
    }
    mPreview = nullptr;
}

bool SLFloaterAnimationExplorer::postBuild()
{
    mList = getChild<LLScrollListCtrl>("animation_list");
    mCounter = getChild<LLTextBox>("counter");
    mPreviewView = getChild<LLView>("animation_preview");

    mList->setCommitOnSelectionChange(true);
    mList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSelectionChange(); });

    getChild<LLButton>("stop_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onStop(); });
    getChild<LLButton>("revoke_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onStopAndRevoke(); });
    getChild<LLButton>("block_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onBlock(); });
    getChild<LLButton>("unblock_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onUnblock(); });
    getChild<LLButton>("copy_uuid_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onCopyUUID(); });
    getChild<LLButton>("try_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onTryOnMe(); });
    getChild<LLButton>("clear_log_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onClearLog(); });
    getChild<LLButton>("blocklist_btn")->setClickedCallback([](LLUICtrl*, const LLSD&)
    {
        LLFloaterReg::showInstance("blocked", LLSD("asset_tab"));
    });
    getChild<LLUICtrl>("hide_own_check")->setCommitCallback([this](LLUICtrl*, const LLSD&) { mDirty = true; });

    mLogConn = SLAnimationLog::getInstance()->setChangeCallback([this]() { mDirty = true; });
    mBlocklistConn = SLAssetBlocklist::getInstance()->setChangeCallback([this]() { mDirty = true; });
    // A restriction (including a per-avatar exception) can change after names
    // were cached. Reformat before the next draw instead of keeping the old row.
    // Exception-only commands do not emit the behaviour-count signal.
    mRlvConn = gRlvHandler.setCommandCallback([this](const RlvCommand&, ERlvCmdRet, bool) {
        mDirty = true;
        // A retained floater can replay without calling draw(). Revoke its cached
        // name pixels now so the next draw must reformat under the current policy.
        onRetainedDirty();
    });

    if (isAgentAvatarValid())
    {
        const LLRect& r = mPreviewView->getRect();
        mPreview = new LLPreviewAnimation(r.getWidth(), r.getHeight());
        mPreview->setZoom(2.0f);
        previewAnimation(LLUUID::null);
    }

    updateButtons();
    return true;
}

void SLFloaterAnimationExplorer::onClose(bool app_quitting)
{
    stopTrying();
    LLFloater::onClose(app_quitting);
}

void SLFloaterAnimationExplorer::draw()
{
    // a tried animation that does not loop ends by itself (as LLPreviewAnim's local play)
    if (mTryAnim.notNull() && isAgentAvatarValid())
    {
        if (LLMotion* motion = gAgentAvatarp->findMotion(mTryAnim))
        {
            if (mTryStarted && motion->isStopped())
            {
                mTryAnim.setNull();
                mTryStarted = false;
                getChild<LLUICtrl>("try_btn")->setValue(false);
                updateButtons();
            }
            else if (!mTryStarted && gAgentAvatarp->isMotionActive(mTryAnim))
            {
                mTryStarted = true;
            }
        }
    }

    if (mDirty || mRefreshTimer.getElapsedTimeF32() > PERIODIC_REFRESH_SECONDS)
    {
        refresh();
    }
    LLFloater::draw();

    if (mPreview)
    {
        // as the animation upload preview draws its avatar
        mPreview->requestUpdate();
        const LLRect& r = mPreviewView->getRect();
        gGL.color3f(1.f, 1.f, 1.f);
        gGL.getTexUnit(0)->bind(mPreview);
        gGL.begin(LLRender::TRIANGLES);
        {
            gGL.texCoord2f(0.f, 1.f);
            gGL.vertex2i(r.mLeft, r.mTop);
            gGL.texCoord2f(0.f, 0.f);
            gGL.vertex2i(r.mLeft, r.mBottom);
            gGL.texCoord2f(1.f, 0.f);
            gGL.vertex2i(r.mRight, r.mBottom);

            gGL.texCoord2f(0.f, 1.f);
            gGL.vertex2i(r.mLeft, r.mTop);
            gGL.texCoord2f(1.f, 0.f);
            gGL.vertex2i(r.mRight, r.mBottom);
            gGL.texCoord2f(1.f, 1.f);
            gGL.vertex2i(r.mRight, r.mTop);
        }
        gGL.end();
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    }
}

std::vector<SLFloaterAnimationExplorer::Selected> SLFloaterAnimationExplorer::getSelected() const
{
    std::vector<Selected> out;
    for (LLScrollListItem* item : mList->getAllSelected())
    {
        Selected s;
        if (parseRowKey(item->getValue().asString(), s.mAnim, s.mSource))
        {
            out.push_back(s);
        }
    }
    return out;
}

std::string SLFloaterAnimationExplorer::resolveSourceName(const SLAnimationLogEntry& entry)
{
    LLViewerObject* objectp = gObjectList.findObject(entry.mSourceID);
    if (entry.mSourceID == gAgentID || (objectp && objectp->isAvatar()))
    {
        // Check at presentation, not when the request was made. The name callback
        // only dirties the list; every refresh comes back through this policy.
        if (entry.mSourceID != gAgentID && !RlvActions::canShowName(RlvActions::SNC_DEFAULT, entry.mSourceID))
        {
            return RlvStrings::getAnonym(entry.mSourceID.asString());
        }
        LLAvatarName av_name;
        if (LLAvatarNameCache::get(entry.mSourceID, &av_name))
        {
            // RLV @shownames: someone else's name stays hidden
            return entry.mSourceID == gAgentID || RlvActions::canShowName(RlvActions::SNC_DEFAULT, entry.mSourceID)
                ? av_name.getCompleteName() : RlvStrings::getAnonym(av_name);
        }
        if (mNameRequests.find(entry.mSourceID) == mNameRequests.end())
        {
            mNameRequests[entry.mSourceID] = LLAvatarNameCache::get(entry.mSourceID,
                [this](const LLUUID&, const LLAvatarName&) {
                    mDirty = true;
                    onRetainedDirty();
                });
        }
        return LLTrans::getString("LoadingData");
    }
    std::string name = SLAnimationLog::getInstance()->getObjectName(entry.mSourceID);
    if (name.empty() && objectp)
    {
        if (LLSelectNode* node = LLSelectMgr::getInstance()->getSelection()->findNode(objectp))
        {
            name = node->mName;
        }
    }
    // gone out of view before its name came: a short id
    return name.empty() ? entry.mSourceID.asString().substr(0, 8) : name;
}

void SLFloaterAnimationExplorer::refresh()
{
    mDirty = false;
    mRefreshTimer.reset();

    const bool hide_own = gSavedSettings.getBOOL("AnimationExplorerHideOwn");

    // Keep the selection and scroll position across the rebuild.
    std::vector<std::string> selected_keys;
    for (LLScrollListItem* item : mList->getAllSelected())
    {
        selected_keys.push_back(item->getValue().asString());
    }
    const S32 scroll_pos = mList->getScrollPos();

    mList->clearRows();

    const F64 now = LLDate::now().secondsSinceEpoch();
    const SLAnimationLog::entry_list_t& entries = SLAnimationLog::getInstance()->getEntries();
    SLAssetBlocklist& blocklist = *SLAssetBlocklist::getInstance();
    S32 shown = 0;

    for (const SLAnimationLogEntry& e : entries)
    {
        if (hide_own && e.mSourceID == gAgentID)
        {
            continue;
        }

        std::string status;
        if (blocklist.isBlocked(e.mAnimID))
        {
            status = getString("StatusBlocked");
        }
        else if (e.mStopped == 0.0)
        {
            status = getString("StatusPlaying");
        }
        else
        {
            const S64 age = (S64)std::max(0.0, now - e.mStopped);
            LLStringUtil::format_map_t args;
            if (age < 60)        { args["[N]"] = llformat("%lld", (long long)age);          status = getString("StoppedSeconds", args); }
            else if (age < 3600) { args["[N]"] = llformat("%lld", (long long)(age / 60));   status = getString("StoppedMinutes", args); }
            else                 { args["[N]"] = llformat("%lld", (long long)(age / 3600)); status = getString("StoppedHours", args); }
        }

        // known once the animation has loaded on our avatar
        std::string priority = getString("PriorityUnknown");
        if (isAgentAvatarValid())
        {
            if (LLKeyframeMotion* motion = dynamic_cast<LLKeyframeMotion*>(gAgentAvatarp->findMotion(e.mAnimID)))
            {
                priority = llformat("%d", (S32)motion->getPriority());
            }
        }

        LLStringUtil::format_map_t name_args;
        name_args["[ID]"] = e.mAnimID.asString().substr(0, 8);

        LLSD row;
        row["value"] = makeRowKey(e.mAnimID, e.mSourceID);
        LLSD& cols = row["columns"];
        cols[0]["column"] = "played_by"; cols[0]["value"] = resolveSourceName(e);
        cols[1]["column"] = "status";    cols[1]["value"] = status;
        cols[2]["column"] = "priority";  cols[2]["value"] = priority;
        cols[3]["column"] = "animation"; cols[3]["value"] = getString("AnimationName", name_args);
        mList->addElement(row, ADD_BOTTOM);
        ++shown;
    }

    for (const std::string& key : selected_keys)
    {
        mList->setSelectedByValue(LLSD(key), true);
    }
    mList->setScrollPos(scroll_pos);

    LLStringUtil::format_map_t counter_args;
    counter_args["[SHOWN]"] = llformat("%d", shown);
    counter_args["[TOTAL]"] = llformat("%d", (S32)entries.size());
    mCounter->setText(getString("Counter", counter_args));

    updateButtons();
}

void SLFloaterAnimationExplorer::updateButtons()
{
    const std::vector<Selected> sel = getSelected();
    bool any_blocked = false;
    bool any_unblocked = false;
    bool any_object = false;
    for (const Selected& s : sel)
    {
        if (SLAssetBlocklist::getInstance()->isBlocked(s.mAnim)) any_blocked = true; else any_unblocked = true;
        if (s.mSource.notNull() && s.mSource != gAgentID) any_object = true;
    }
    getChildView("stop_btn")->setEnabled(!sel.empty());
    getChildView("revoke_btn")->setEnabled(any_object);
    getChildView("block_btn")->setEnabled(any_unblocked);
    getChildView("unblock_btn")->setEnabled(any_blocked);
    getChildView("copy_uuid_btn")->setEnabled(!sel.empty());

    // Not for one already playing on us: it shows there, and stopping the try would stop it too
    bool can_try = false;
    if (sel.size() == 1 && isAgentAvatarValid() && !SLAssetBlocklist::getInstance()->isBlocked(sel[0].mAnim))
    {
        can_try = gAgentAvatarp->mSignaledAnimations.find(sel[0].mAnim) == gAgentAvatarp->mSignaledAnimations.end();
    }
    getChildView("try_btn")->setEnabled(mTryAnim.notNull() || can_try);
}

void SLFloaterAnimationExplorer::onTryOnMe()
{
    if (mTryAnim.notNull())
    {
        stopTrying();
        return;
    }
    const std::vector<Selected> sel = getSelected();
    if (sel.size() != 1 || !isAgentAvatarValid())
    {
        getChild<LLUICtrl>("try_btn")->setValue(false);
        return;
    }
    // locally only, as the inventory preview's Play Locally: nobody else sees it
    mTryAnim = sel[0].mAnim;
    mTryStarted = false;
    gAgentAvatarp->startMotion(mTryAnim);
    getChild<LLUICtrl>("try_btn")->setValue(true);
    updateButtons();
}

void SLFloaterAnimationExplorer::stopTrying()
{
    // If the sim started the same animation while it was being tried on, it is the sim's now:
    // stopping the preview mustn't stop it
    if (mTryAnim.notNull() && isAgentAvatarValid()
        && !gAgentAvatarp->mSignaledAnimations.count(mTryAnim))
    {
        gAgentAvatarp->stopMotion(mTryAnim);
    }
    mTryAnim.setNull();
    mTryStarted = false;
    if (LLUICtrl* btn = findChild<LLUICtrl>("try_btn"))
    {
        btn->setValue(false);
    }
}

void SLFloaterAnimationExplorer::previewAnimation(const LLUUID& anim_id)
{
    mPreviewAnim = anim_id;
    if (!mPreview)
    {
        return;
    }
    LLVOAvatar* avatarp = mPreview->getDummyAvatar();
    avatarp->deactivateAllMotions();
    avatarp->startMotion(ANIM_AGENT_STAND, 0.0f);
    if (anim_id.notNull())
    {
        avatarp->startMotion(anim_id, 0.0f);
    }
}

void SLFloaterAnimationExplorer::onSelectionChange()
{
    const std::vector<Selected> sel = getSelected();
    const LLUUID anim = sel.size() == 1 ? sel[0].mAnim : LLUUID::null;
    if (anim != mPreviewAnim)
    {
        previewAnimation(anim);
    }
    if (mTryAnim.notNull() && anim != mTryAnim)
    {
        stopTrying();
    }
    updateButtons();
}

void SLFloaterAnimationExplorer::onStop()
{
    std::set<LLUUID> done;
    for (const Selected& s : getSelected())
    {
        if (done.insert(s.mAnim).second)
        {
            stopAnimation(s.mAnim);
        }
    }
}

void SLFloaterAnimationExplorer::onStopAndRevoke()
{
    onStop();
    // The server takes only the animation bits (see LLAgent::sendRevokePermissions)
    const U32 permissions = SCRIPT_PERMISSIONS[SCRIPT_PERMISSION_TRIGGER_ANIMATION].permbit
                          | SCRIPT_PERMISSIONS[SCRIPT_PERMISSION_OVERRIDE_ANIMATIONS].permbit;
    std::set<LLUUID> done;
    for (const Selected& s : getSelected())
    {
        if (s.mSource.notNull() && s.mSource != gAgentID && done.insert(s.mSource).second)
        {
            gAgent.sendRevokePermissions(s.mSource, permissions);
        }
    }
}

void SLFloaterAnimationExplorer::onBlock()
{
    const std::string region = gAgent.getRegion() ? gAgent.getRegion()->getName() : std::string();
    for (const Selected& s : getSelected())
    {
        const LLUUID owner = (s.mSource == gAgentID) ? gAgentID : SLAnimationLog::getInstance()->getObjectOwner(s.mSource);
        // addEntry also stops it on our avatar; process_avatar_animation refuses it from now on
        SLAssetBlocklist::getInstance()->addEntry(s.mAnim, owner, region, LLAssetType::AT_ANIMATION);
    }
    mDirty = true;
}

void SLFloaterAnimationExplorer::onUnblock()
{
    uuid_vec_t ids;
    for (const Selected& s : getSelected())
    {
        ids.push_back(s.mAnim);
    }
    SLAssetBlocklist::getInstance()->removeEntries(ids);
    mDirty = true;
}

void SLFloaterAnimationExplorer::onCopyUUID()
{
    std::string text;
    for (const Selected& s : getSelected())
    {
        if (!text.empty())
        {
            text += "\n";
        }
        text += s.mAnim.asString();
    }
    if (!text.empty())
    {
        LLClipboard::instance().copyToClipboard(utf8str_to_wstring(text), 0, (S32)text.length());
    }
}

void SLFloaterAnimationExplorer::onClearLog()
{
    SLAnimationLog::getInstance()->clear();
}

// Avatar preview mouse handling, as LLFloaterBvhPreview's

bool SLFloaterAnimationExplorer::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (mPreview && mPreviewView->getRect().pointInRect(x, y))
    {
        bringToFront(x, y);
        gFocusMgr.setMouseCapture(this);
        gViewerWindow->hideCursor();
        mLastMouseX = x;
        mLastMouseY = y;
        return true;
    }
    return LLFloater::handleMouseDown(x, y, mask);
}

bool SLFloaterAnimationExplorer::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture())
    {
        gFocusMgr.setMouseCapture(nullptr);
        gViewerWindow->showCursor();
    }
    return LLFloater::handleMouseUp(x, y, mask);
}

bool SLFloaterAnimationExplorer::handleHover(S32 x, S32 y, MASK mask)
{
    if (!mPreview || (!hasMouseCapture() && !mPreviewView->getRect().pointInRect(x, y)))
    {
        return LLFloater::handleHover(x, y, mask);
    }

    const MASK local_mask = mask & ~MASK_ALT;
    if (hasMouseCapture())
    {
        if (local_mask == MASK_PAN)
        {
            mPreview->pan((F32)(x - mLastMouseX) * -0.005f, (F32)(y - mLastMouseY) * -0.005f);
        }
        else if (local_mask == MASK_ORBIT)
        {
            mPreview->rotate((F32)(x - mLastMouseX) * -0.01f, (F32)(y - mLastMouseY) * 0.02f);
        }
        else
        {
            mPreview->rotate((F32)(x - mLastMouseX) * -0.01f, 0.f);
            mPreview->zoom((F32)(y - mLastMouseY) * 0.02f);
        }
        mPreview->requestUpdate();
        LLUI::getInstance()->setMousePositionLocal(this, mLastMouseX, mLastMouseY);
    }
    else if (local_mask == MASK_ORBIT)
    {
        gViewerWindow->setCursor(UI_CURSOR_TOOLCAMERA);
    }
    else if (local_mask == MASK_PAN)
    {
        gViewerWindow->setCursor(UI_CURSOR_TOOLPAN);
    }
    else
    {
        gViewerWindow->setCursor(UI_CURSOR_TOOLZOOMIN);
    }
    return true;
}

bool SLFloaterAnimationExplorer::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    if (mPreview && mPreviewView->getRect().pointInRect(x, y))
    {
        mPreview->zoom((F32)clicks * -0.2f);
        mPreview->requestUpdate();
        return true;
    }
    return LLFloater::handleScrollWheel(x, y, clicks);
}

void SLFloaterAnimationExplorer::onMouseCaptureLost()
{
    gViewerWindow->showCursor();
}
