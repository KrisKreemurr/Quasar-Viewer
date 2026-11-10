/**
 * @file slfloateranimationexplorer.h
 * @brief Animation Explorer floater: the animations played on our avatar, what played
 *        them, and stopping, revoking or blocking them (#1099, after Firestorm's)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2013, Zi Ree @ Second Life (Firestorm's Animation Explorer)
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

#ifndef SL_FLOATERANIMATIONEXPLORER_H
#define SL_FLOATERANIMATIONEXPLORER_H

#include "llfloater.h"
#include "llavatarnamecache.h"
#include "llframetimer.h"

#include <map>

class LLPreviewAnimation;
class LLScrollListCtrl;
class LLTextBox;
class LLView;
struct SLAnimationLogEntry;

class SLFloaterAnimationExplorer : public LLFloater
{
    friend class LLFloaterReg;
public:
    bool postBuild() override;
    void draw() override;
    void onClose(bool app_quitting) override;

    // the avatar preview turns, pans and zooms like the animation upload preview's
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    void onMouseCaptureLost() override;

private:
    SLFloaterAnimationExplorer(const LLSD& key);
    ~SLFloaterAnimationExplorer() override;

    struct Selected
    {
        LLUUID mAnim;
        LLUUID mSource;
    };

    void refresh();
    void updateButtons();
    std::vector<Selected> getSelected() const;
    std::string resolveSourceName(const SLAnimationLogEntry& entry);
    void previewAnimation(const LLUUID& anim_id);
    // Try on me: the selected animation on our own avatar, seen only by us
    void stopTrying();

    void onSelectionChange();
    void onStop();
    void onStopAndRevoke();
    void onBlock();
    void onUnblock();
    void onCopyUUID();
    void onTryOnMe();
    void onClearLog();

    LLScrollListCtrl* mList = nullptr;
    LLTextBox* mCounter = nullptr;
    LLView* mPreviewView = nullptr;
    LLPointer<LLPreviewAnimation> mPreview;
    LLUUID mPreviewAnim;
    LLUUID mTryAnim;            // playing locally on our avatar (Try on me)
    bool mTryStarted = false;   // its motion became active, so a stop means it ended
    S32 mLastMouseX = 0;
    S32 mLastMouseY = 0;

    bool mDirty = true;
    LLFrameTimer mRefreshTimer;
    boost::signals2::connection mLogConn;
    boost::signals2::connection mBlocklistConn;
    boost::signals2::connection mRlvConn;
    std::map<LLUUID, LLAvatarNameCache::callback_connection_t> mNameRequests;
};

#endif // SL_FLOATERANIMATIONEXPLORER_H
