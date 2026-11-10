/**
 * @file llsnapshotavatars.h
 * @brief Snapshot avatar layers (transparent avatar, holdout mattes) and the snapshot avatar scope.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Copyright (C) 2026, squeedledorf (GunMetal-DX); SoapStorm (Soap Frenzy).
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

// Ported from GunMetal-DX (squeedledorf, LGPL 2.1), v0.3.0 cf5f1f4 (gmdxsnapshotavatars.h), which
// ported it from SoapStorm (Soap Frenzy): transparent avatar snapshots, holdout alpha mattes
// (SLSnapshotSaveAvatarMattes), the snapshot avatar scope (SLSnapshotAvatarScope: all, selected,
// self only; the rule is slsnapshotscope.h) with its docked picker
// (floater_snapshot_avatar_select.xml), and the portrait / landscape swap. SoapStorm did the
// rendering in GL; here it is the Vulkan renderer:
//  - AVATAR LAYERS ("Avatars only (transparent background)", "Avatar Matte (Black on White /
//    White on Black)"):
//    an offscreen render (vkRenderOffscreenAvatarLayer) whose main view keeps avatars only (the
//    world still lights and shadows them; no sky, no particles, no bloom / glow / DoF / lens
//    effects). Its alpha is the avatars' coverage carried through the colour chain on the GPU --
//    soft for hair and lace, antialiased by the supersampling, cut where the world is in front as
//    the same view WITH the world (its warm-up recording) shows it, so nobody shows through a
//    wall -- and it comes back as straight-alpha RGBA (vkgfx/llcleansnapmattevk.cpp, #1328); the
//    mattes are that alpha.
//  - SCOPE: avatars outside SLSnapshotAvatarScope leave every pass of a world snapshot's offscreen
//    render (the renderer's avatar render policy, snapshotHidesAvatar); a filtered world snapshot
//    therefore always renders offscreen. Your own avatar, when "Selected" leaves you out, leaves
//    the main view only (snapshotHidesSelf, as "Show me in Mouselook" off: its draws also serve
//    the shadow and mirror passes). In an avatar layer the avatars it leaves out stay in the
//    world -- shadows, reflections, in front of the others -- and leave only its avatar-only main
//    view (snapshotLayerHidesAvatar, #1328). Snapshots with the interface are the presented frame
//    and are not filtered.

#ifndef LL_SNAPSHOTAVATARS_H
#define LL_SNAPSHOTAVATARS_H

#include "llfloater.h"
#include "llsnapshotmodel.h"
#include "lluuid.h"
#include "slsnapshotscope.h"

#include <set>
#include <string>
#include <vector>

class LLAvatarName;
class LLFilterEditor;
class LLFloaterSnapshotBase;
class LLImageFormatted;
class LLImageRaw;
class LLScrollListCtrl;
class LLVOAvatar;

// What "Selected" takes in (session only; the picker edits both): the other avatars ticked,
// and you while ticked (you are, to begin with).
extern std::set<LLUUID> gSnapshotSelectedAvatars;
extern bool gSnapshotSelectedSelf;

bool snapshotIsAvatarLayer(LLSnapshotModel::ESnapshotLayerType type);

// While a world snapshot renders: true = leave this avatar (or animesh) out of every pass. The
// Vulkan renderer's avatar render policy asks (llcleanrenderervk.cpp avatarHiddenByPolicy). Never
// your own avatar or what it wears (snapshotHidesSelf); never in an avatar layer
// (snapshotLayerHidesAvatar).
bool snapshotHidesAvatar(LLVOAvatar* av);
// While a world snapshot renders: true = the scope leaves you out ("Selected" with you unticked).
// The offscreen render then hides your avatar in its main view (LLVKWorldFrame::hide_self).
bool snapshotHidesSelf();
// While an avatar layer renders (#1328): the avatars outside the scope stay in the world. They
// cast shadows, are reflected and stand in front of the layer's avatars like any wall; only the
// main view of the layer's avatar-only recordings leaves them out (the renderer's
// computeAvatarVis asks). You: snapshotLayerHidesSelf (LLVKWorldFrame::avatar_hide_self).
bool snapshotLayerHidesAvatar(LLVOAvatar* av);
bool snapshotLayerHidesSelf();

// LLViewerWindow::rawSnapshot. A world (non-UI) snapshot of `type` must render offscreen: an
// avatar layer, a scope that leaves avatars out, or mattes saved with it (they come from an
// offscreen render: the snapshot must frame exactly as they do).
bool snapshotWantsOffscreen(LLSnapshotModel::ESnapshotLayerType type);
// Applies SLSnapshotAvatarScope to the offscreen renders while it lives (rawSnapshot).
class LLSnapshotScopeHold
{
public:
    explicit LLSnapshotScopeHold(bool on);
    ~LLSnapshotScopeHold();
private:
    bool mOn;
};
// An avatar layer at image_width x image_height (rows bottom-up, as rawSnapshot): RGBA for the
// transparent avatar, grey for the mattes.
bool snapshotRawAvatar(LLImageRaw* raw, S32 image_width, S32 image_height,
                           LLSnapshotModel::ESnapshotLayerType type, S32 max_size, std::vector<U8>* pairedHdr = nullptr);
// LLViewerWindow::saveImageLocal, after `image` was written to `filepath`: with
// SLSnapshotSaveAvatarMattes, <name>_matte and <name>_matte_inv beside it.
void snapshotSaveMattes(LLImageFormatted* image, const std::string& filepath, const std::string& extension);

// Snapshot floater helpers.
void snapshotSwapOrientation(LLFloaterSnapshotBase* floater);   // landscape <-> portrait
void snapshotUpdateControls(LLFloaterSnapshotBase* floater);    // orientation icons, picker button
void snapshotScopeChanged(LLFloaterSnapshotBase* floater);      // picker shown / closed
void snapshotShowPicker(LLFloaterSnapshotBase* floater);

// "Select Avatars": you and the nearby avatars "Selected" may take in, docked beside the Snapshot floater.
class LLFloaterSnapshotAvatarSelect : public LLFloater
{
public:
    LLFloaterSnapshotAvatarSelect(const LLSD& key);
    ~LLFloaterSnapshotAvatarSelect() override = default;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

    void refreshList();
    void updatePreview();

private:
    static void onAvatarNameCacheCallback(LLHandle<LLFloaterSnapshotAvatarSelect> handle, const LLUUID& agent_id, const LLAvatarName& av_name);
    static void onCheckItemCallback(LLHandle<LLFloaterSnapshotAvatarSelect> handle, LLUICtrl* ctrl, const LLUUID& id);

    void onCheckItem(LLUICtrl* ctrl, const LLUUID& id);
    void onSelectAll();
    void onDeselectAll();
    void onFilterEdit(const LLSD& value);
    void onAvatarNameCache(const LLUUID& agent_id, const LLAvatarName& av_name);

    struct NearbyAvatarInfo
    {
        LLUUID id;
        std::string displayName;
        std::string userName;
        F32 distance = 0.f;
        bool self = false;      // your own avatar (listed first)
    };
    std::string listName(const NearbyAvatarInfo& info) const;

    LLScrollListCtrl* mAvatarList = nullptr;
    LLFilterEditor*   mFilterEditor = nullptr;
    std::string       mFilterString;
    std::vector<NearbyAvatarInfo> mNearbyAvatars;
};

#endif // LL_SNAPSHOTAVATARS_H
