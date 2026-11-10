/**
 * @file llpreviewsound.cpp
 * @brief LLPreviewSound class implementation
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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

#include "llaudioengine.h"
#include "llbutton.h"
#include "llinventory.h"
#include "lllineeditor.h"
#include "llpreviewsound.h"
#include "llresmgr.h"
#include "llviewercontrol.h"
#include "llviewermessage.h"  // send_guid_sound_trigger
#include "lluictrlfactory.h"
#include "slpanelsoundplayer.h"

extern LLAudioEngine* gAudiop;

const F32 SOUND_GAIN = 1.0f;

LLPreviewSound::LLPreviewSound(const LLSD& key)
  : LLPreview( key )
{
}

// virtual
bool    LLPreviewSound::postBuild()
{
    const LLInventoryItem* item = getItem();
    if (item)
    {
        getChild<LLUICtrl>("desc")->setValue(item->getDescription());
    }

    // #1311: Play Locally is the player's Play / Pause (it fetches and decodes the sound).
    mPlayer = findChild<SLPanelSoundPlayer>("player");

    childSetAction("Sound play btn",&LLPreviewSound::playSound,this);

    LLButton* button = getChild<LLButton>("Sound play btn");
    button->setSoundFlags(LLView::SILENT);

    childSetCommitCallback("desc", LLPreview::onText, this);
    getChild<LLLineEditor>("desc")->setPrevalidate(&LLTextValidate::validateASCIIPrintableNoPipe);

    return LLPreview::postBuild();
}

// static
void LLPreviewSound::playSound( void *userdata )
{
    LLPreviewSound* self = (LLPreviewSound*) userdata;
    const LLInventoryItem *item = self->getItem();

    if(item && gAudiop)
    {
        send_sound_trigger(item->getAssetUUID(), SOUND_GAIN);
    }
}

// virtual
void LLPreviewSound::refreshFromItem()
{
    LLPreview::refreshFromItem();
    const LLInventoryItem* item = getItem();
    if (item && mPlayer)
    {
        mPlayer->setSound(item->getAssetUUID());
    }
}
