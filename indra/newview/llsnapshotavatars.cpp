/**
 * @file llsnapshotavatars.cpp
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

// See llsnapshotavatars.h. Ported from GunMetal-DX (squeedledorf, LGPL 2.1), v0.3.0 cf5f1f4
// (gmdxsnapshotavatars.cpp), from SoapStorm (Soap Frenzy).

#include "llviewerprecompiledheaders.h"

#include "llsnapshotavatars.h"
#include "vkgfx/vk_snapshot_readback.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llavatarnamecache.h"
#include "llbutton.h"
#include "llcachename.h"
#include "llcharacter.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llfiltereditor.h"
#include "llfloaterreg.h"
#include "llfloatersnapshot.h"
#include "llimage.h"
#include "llnotificationsutil.h"
#include "llpanelsnapshot.h"
#include "llscrolllistcell.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llsnapshotlivepreview.h"
#include "llspinctrl.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llvoavatar.h"
#include "llvoavatarself.h"
#include "pipeline.h"
#include "rlvactions.h"
#include "rlvcommon.h"
#include "rlveffects.h"
#include "slstraightalpha.h"

#include <algorithm>
#include <cstring>

std::set<LLUUID> gSnapshotSelectedAvatars;
bool gSnapshotSelectedSelf = true;

// The avatar layer's offscreen render and its sizing (llappviewer.cpp).
bool vkRenderOffscreenAvatarLayer(S32 w, S32 h, std::vector<U8>& rgba, U32 aa_frames,
                                  std::vector<U8>* hdrHalf, F32* hdrWhiteNits);
U32 vkSnapshotAAFrames();
U32 vkSnapshotFitAvatarLayer(S32& w, S32& h, U32 aa_frames, bool companionHdr);

namespace
{
// What the snapshot being rendered applies (main thread; between offscreen renders it is off).
struct SnapScope
{
    bool active = false;        // a snapshot's offscreen render is running
    U32  scope = SNAPSHOT_AVATARS_ALL;
    bool self = true;           // "Selected": you are ticked
    bool avatar_only = false;   // ... and it is an avatar layer
};
SnapScope s_scope;

U32 scopeSetting()
{
    static LLCachedControl<U32> s_setting(gSavedSettings, "SLSnapshotAvatarScope", SNAPSHOT_AVATARS_ALL);
    return slSnapshotScopeClamp(s_setting);
}

// The scope as the snapshot being rendered took it (the picker's ticks: the session's).
SLSnapshotScopePick scopePick()
{
    SLSnapshotScopePick pick;
    pick.scope = s_scope.scope;
    pick.self = s_scope.self;
    pick.selected = &gSnapshotSelectedAvatars;
    return pick;
}

// A new snapshot render: the scope setting and your tick, held for its whole sequence.
void scopeBegin(bool avatar_only)
{
    s_scope = SnapScope{};
    s_scope.active = true;
    s_scope.scope = scopeSetting();
    s_scope.self = gSnapshotSelectedSelf;
    s_scope.avatar_only = avatar_only;
}

// The avatar an avatar / animesh counts as: an animesh worn by someone is that wearer's.
LLVOAvatar* snapshotOwner(LLVOAvatar* av)
{
    if (av && av->isControlAvatar())
    {
        LLVOAvatar* wearer = av->getAttachedAvatar();
        if (wearer && !wearer->isDead()) return wearer;
    }
    return av;
}

// RLVa @setsphere: a vision sphere is on (as buildVulkanWorldFrame collects them).
bool rlvSphereActive()
{
    if (!RlvActions::hasBehaviour(RLV_BHVR_SETSPHERE)) return false;
    RlvSphereRenderParams spheres;
    LLVfxManager::instance().runEffect(EVisualEffect::RlvSphere, &spheres);
    return spheres.count > 0;
}

SLSnapshotAvatarKind snapshotKind(LLVOAvatar* owner)
{
    if (owner->isSelf()) return SLSnapshotAvatarKind::SELF;
    return owner->isControlAvatar() ? SLSnapshotAvatarKind::ANIMESH : SLSnapshotAvatarKind::AVATAR;
}

// The scope takes this avatar's owner in.
bool scopeTakes(LLVOAvatar* owner)
{
    return slSnapshotScopeTakes(scopePick(), snapshotKind(owner), owner->getID());
}

} // namespace

bool snapshotIsAvatarLayer(LLSnapshotModel::ESnapshotLayerType type)
{
    return type == LLSnapshotModel::SNAPSHOT_TYPE_AVATAR_TRANSPARENT
        || type == LLSnapshotModel::SNAPSHOT_TYPE_AVATAR_MASK_BLACK
        || type == LLSnapshotModel::SNAPSHOT_TYPE_AVATAR_MASK_WHITE;
}

bool snapshotHidesAvatar(LLVOAvatar* av)
{
    // (an avatar layer leaves the main view only: snapshotLayerHidesAvatar)
    if (!s_scope.active || s_scope.avatar_only || !av || av->isDead() || av->isUIAvatar()) return false;
    LLVOAvatar* owner = snapshotOwner(av);
    if (owner->isSelf()) return false;   // (your draws also serve the shadow / mirror passes: snapshotHidesSelf)
    return !scopeTakes(owner);
}

bool snapshotHidesSelf()
{
    return s_scope.active && !s_scope.avatar_only
        && !slSnapshotScopeTakes(scopePick(), SLSnapshotAvatarKind::SELF, gAgentID);
}

bool snapshotLayerHidesAvatar(LLVOAvatar* av)
{
    if (!s_scope.active || !s_scope.avatar_only || !av || av->isDead() || av->isUIAvatar()) return false;
    LLVOAvatar* owner = snapshotOwner(av);
    if (owner->isSelf()) return false;   // (snapshotLayerHidesSelf)
    return !scopeTakes(owner);
}

bool snapshotLayerHidesSelf()
{
    return s_scope.active && s_scope.avatar_only
        && !slSnapshotScopeTakes(scopePick(), SLSnapshotAvatarKind::SELF, gAgentID);
}


bool snapshotWantsOffscreen(LLSnapshotModel::ESnapshotLayerType type)
{
    if (snapshotIsAvatarLayer(type)) return true;
    if (scopeSetting() != SNAPSHOT_AVATARS_ALL) return true;
    static LLCachedControl<bool> s_mattes(gSavedSettings, "SLSnapshotSaveAvatarMattes", false);
    return type == LLSnapshotModel::SNAPSHOT_TYPE_COLOR && s_mattes;
}

LLSnapshotScopeHold::LLSnapshotScopeHold(bool on) : mOn(on && !s_scope.active)
{
    if (mOn) scopeBegin(false);
}

LLSnapshotScopeHold::~LLSnapshotScopeHold()
{
    if (mOn) s_scope = SnapScope{};
}

bool snapshotRawAvatar(LLImageRaw* raw, S32 image_width, S32 image_height,
                           LLSnapshotModel::ESnapshotLayerType type, S32 max_size, std::vector<U8>* pairedHdr)
{
    if (pairedHdr) pairedHdr->clear();
    if (!raw || image_width <= 0 || image_height <= 0 || !snapshotIsAvatarLayer(type) || s_scope.active) return false;
    // #1328: an RLVa vision sphere must stay on every snapshot, and its passes write opaque pixels
    // (they work on the finished picture, not on coverage): no avatar layer while one is on. Told
    // at most every 20 s -- the floater re-renders its preview often.
    if (rlvSphereActive())
    {
        static F64 s_told_at = -1.0e9;
        const F64 now = LLFrameTimer::getTotalSeconds();
        if (now - s_told_at >= 20.0)
        {
            s_told_at = now;
            LLNotificationsUtil::add("SnapshotAvatarLayerRlvSphere");
        }
        LL_INFOS("Snapshot") << "avatar snapshot refused: an RLVa vision sphere is on" << LL_ENDL;
        return false;
    }
    // The size the device / video memory / system memory allow (as the offscreen snapshot); the
    // rest is a scale at the end.
    S32 rw = image_width, rh = image_height;
    bool wantHdr = pairedHdr && type == LLSnapshotModel::SNAPSHOT_TYPE_AVATAR_TRANSPARENT;
    U32 aa = vkSnapshotFitAvatarLayer(rw, rh, vkSnapshotAAFrames(), wantHdr);
    if ((rw <= 0 || rh <= 0) && wantHdr)
    {
        // The optional companion can exceed its limits when the normal image fits.
        wantHdr = false;
        rw = image_width; rh = image_height;
        aa = vkSnapshotFitAvatarLayer(rw, rh, vkSnapshotAAFrames(), false);
    }
    if (rw <= 0 || rh <= 0) return false;

    scopeBegin(true);
    // No sky (the background is cleared black) and no particles in front of the avatars.
    gPipeline.pushRenderTypeMask();
    gPipeline.clearRenderTypeMask(LLPipeline::RENDER_TYPE_SKY, LLPipeline::RENDER_TYPE_WL_SKY,
                                  LLPipeline::RENDER_TYPE_PARTICLES, LLPipeline::END_RENDER_TYPES);
    // #1328: straight colour + coverage, unpremultiplied and tonemapped on the GPU (the
    // renderer's transparent avatar layer, vkgfx/llcleansnapmattevk.cpp); transparent pixels are
    // transparent black.
    std::vector<U8> rgba;
    std::vector<U8> hdrHalf;
    F32 hdrWhiteNits = 0.f;
    bool got = vkRenderOffscreenAvatarLayer(rw, rh, rgba, aa, wantHdr ? &hdrHalf : nullptr,
                                            wantHdr ? &hdrWhiteNits : nullptr);
    if (!got && wantHdr)
    {
        hdrHalf.clear();
        got = vkRenderOffscreenAvatarLayer(rw, rh, rgba, aa, nullptr, nullptr);
    }
    gPipeline.popRenderTypeMask();
    s_scope = SnapScope{};
    if (!got || rgba.size() < (size_t)rw * rh * 4)
    {
        LL_WARNS("Snapshot") << "avatar snapshot " << rw << "x" << rh << " failed (render " << got << ", "
                             << rgba.size() << " bytes)" << LL_ENDL;
        return false;
    }
    LL_INFOS("Snapshot") << "avatar snapshot " << rw << "x" << rh << " (requested " << image_width << "x" << image_height
                         << ", layer " << (S32)type << ", antialias frames " << aa << ")" << LL_ENDL;

    const bool transparent = (type == LLSnapshotModel::SNAPSHOT_TYPE_AVATAR_TRANSPARENT);
    {
        LLImageDataLock lock(raw);
        raw->resize(rw, rh, transparent ? 4 : 3);
        if (raw->isBufferInvalid()) return false;
        U8* out = raw->getData();
        const size_t n = (size_t)rw * rh;
        if (transparent)
        {
            std::memcpy(out, rgba.data(), n * 4);
        }
        else
        {
            // The mattes: the layer's alpha, avatar black on white or white on black.
            const bool inverted = (type == LLSnapshotModel::SNAPSHOT_TYPE_AVATAR_MASK_BLACK);
            for (size_t p = 0; p < n; ++p)
            {
                const U8 a = rgba[p * 4 + 3];
                U8* o = out + p * 3;
                o[0] = o[1] = o[2] = inverted ? (U8)(255 - a) : a;
            }
        }
    }
    // Pad the line length to a multiple of 4 bytes (BMP, 3 components only) and honour max_size,
    // as rawSnapshot does.
    if (!transparent) image_width += (image_width * 3) % 4;
    if (image_width > max_size || image_height > max_size)
    {
        const F32 f = llmin((F32)max_size / image_width, (F32)max_size / image_height);
        image_width = llmax(1, (S32)(image_width * f));
        image_height = llmax(1, (S32)(image_height * f));
    }
    if (wantHdr)
    {
        try
        {
            if (!vksnapshot::avatarToRgbe(hdrHalf.data(), hdrHalf.size(), rw, rh,
                                          image_width, image_height, hdrWhiteNits, *pairedHdr)) pairedHdr->clear();
        }
        catch (const std::bad_alloc&) { pairedHdr->clear(); }
    }
    if (image_width == rw && image_height == rh) return true;
    if (!transparent) return raw->scale(image_width, image_height, true);
    // #1328: the cut-out resamples in premultiplied space (slstraightalpha.h): averaged
    // straight, the transparent black would darken every edge pixel of the result.
    {
        LLImageDataLock lock(raw);
        slPremultiplyRGBA(raw->getData(), (size_t)rw * rh);
    }
    if (!raw->scale(image_width, image_height, true)) return false;
    LLImageDataLock lock(raw);
    slUnpremultiplyRGBA(raw->getData(), (size_t)image_width * image_height);
    return true;
}

void snapshotSaveMattes(LLImageFormatted* image, const std::string& filepath, const std::string& extension)
{
    static LLCachedControl<bool> s_mattes(gSavedSettings, "SLSnapshotSaveAvatarMattes", false);
    if (!s_mattes || !image || !gViewerWindow || filepath.size() <= extension.size()) return;
    // A saved avatar layer is its own matte.
    if (LLFloaterSnapshot* floater = LLFloaterSnapshot::findInstance())
    {
        LLUICtrl* layers = floater->findChild<LLUICtrl>("layer_types");
        if (layers && layers->getValue().asString() != "colors") return;
    }
    const std::string base = filepath.substr(0, filepath.length() - extension.length());
    LLPointer<LLImageRaw> avatar_raw = new LLImageRaw;
    if (!gViewerWindow->rawSnapshot(avatar_raw, image->getWidth(), image->getHeight(), true, false, false, false, false,
                                    false, false, LLSnapshotModel::SNAPSHOT_TYPE_AVATAR_TRANSPARENT)
        || avatar_raw->getComponents() != 4)
    {
        LL_WARNS("Snapshot") << "holdout mattes for " << filepath << " not saved: the avatar render failed" << LL_ENDL;
        return;
    }
    const S32 w = avatar_raw->getWidth(), h = avatar_raw->getHeight();
    LLPointer<LLImageRaw> matte_black = new LLImageRaw(w, h, 3);   // avatar black on white
    LLPointer<LLImageRaw> matte_white = new LLImageRaw(w, h, 3);   // avatar white on black
    if (matte_black->isBufferInvalid() || matte_white->isBufferInvalid()) return;
    const U8* src = avatar_raw->getData();
    U8* dst_b = matte_black->getData();
    U8* dst_w = matte_white->getData();
    const size_t n = (size_t)w * h;
    for (size_t p = 0; p < n; ++p)
    {
        const U8 a = src[p * 4 + 3];
        dst_b[p * 3 + 0] = dst_b[p * 3 + 1] = dst_b[p * 3 + 2] = (U8)(255 - a);
        dst_w[p * 3 + 0] = dst_w[p * 3 + 1] = dst_w[p * 3 + 2] = a;
    }
    LLPointer<LLImageFormatted> form_b = LLImageFormatted::createFromType(image->getCodec());
    if (form_b && form_b->encode(matte_black, 0)) form_b->save(base + "_matte" + extension);
    LLPointer<LLImageFormatted> form_w = LLImageFormatted::createFromType(image->getCodec());
    if (form_w && form_w->encode(matte_white, 0)) form_w->save(base + "_matte_inv" + extension);
}

// ---------------------------------------------------------------------------
// Snapshot floater helpers
// ---------------------------------------------------------------------------

void snapshotSwapOrientation(LLFloaterSnapshotBase* floater)
{
    if (!floater || !floater->impl) return;
    LLSnapshotLivePreview* previewp = floater->getPreviewView();
    if (!previewp) return;
    S32 cur_w = 0, cur_h = 0;
    previewp->getSize(cur_w, cur_h);
    if ((cur_w <= 0 || cur_h <= 0) && gViewerWindow)
    {
        cur_w = gViewerWindow->getWindowWidthRaw();
        cur_h = gViewerWindow->getWindowHeightRaw();
    }
    if (cur_w <= 0 || cur_h <= 0) return;
    const S32 new_w = cur_h, new_h = cur_w;

    LLPanelSnapshot* panel = floater->impl->getActivePanel(floater);
    if (panel)
    {
        // A preset with the swapped size, else Custom with it.
        LLComboBox* combo = panel->findChild<LLComboBox>(panel->getImageSizeComboName());
        const std::string target = llformat("[i%d,i%d]", new_w, new_h);
        if (combo) combo->setValue(target);
        if (combo && combo->getValue().asString() == target)
        {
            combo->onCommit();
        }
        else
        {
            if (combo) combo->setValue("[i-1,i-1]");
            panel->enableAspectRatioCheckbox(true);
            LLSpinCtrl* wsp = panel->getWidthSpinner();
            LLSpinCtrl* hsp = panel->getHeightSpinner();
            if (wsp) { wsp->setEnabled(true); wsp->setValue(new_w); }
            if (hsp) { hsp->setEnabled(true); hsp->setValue(new_h); }
            LLSD info;
            info["w"] = new_w;
            info["h"] = new_h;
            floater->notify(LLSD().with("custom-res-change", info));
        }
    }
    else
    {
        previewp->setSize(new_w, new_h);
        floater->impl->setStatus(LLFloaterSnapshotBase::ImplBase::STATUS_READY);
        previewp->mForceUpdateSnapshot = true;
        previewp->updateSnapshot(true);
    }
    floater->impl->updateControls(floater);
}

void snapshotUpdateControls(LLFloaterSnapshotBase* floater)
{
    if (!floater) return;
    // The orientation buttons show the current one (click: the other).
    S32 w = 0, h = 0;
    if (LLSnapshotLivePreview* previewp = floater->getPreviewView()) previewp->getSize(w, h);
    if ((w <= 0 || h <= 0) && gViewerWindow)
    {
        w = gViewerWindow->getWindowWidthRaw();
        h = gViewerWindow->getWindowHeightRaw();
    }
    const bool portrait = h > w;
    const std::string icon = portrait ? "Icon_Orientation_Portrait" : "Icon_Orientation_Landscape";
    const std::string tip = floater->getString(portrait ? "orientation_portrait_tip" : "orientation_landscape_tip");
    for (const char* name : { "swap_portrait_btn", "swap_resolution_btn" })
    {
        if (LLButton* btn = floater->findChild<LLButton>(name))
        {
            btn->setImageOverlay(icon, LLFontGL::HCENTER);
            btn->setToolTip(tip);
        }
    }
    if (LLView* popout = floater->findChild<LLView>("avatar_scope_popout_btn"))
    {
        popout->setEnabled(scopeSetting() == SNAPSHOT_AVATARS_SELECTED);
    }
}

void snapshotShowPicker(LLFloaterSnapshotBase* floater)
{
    if (!floater) return;
    LLFloaterSnapshotAvatarSelect* picker = LLFloaterReg::showTypedInstance<LLFloaterSnapshotAvatarSelect>("snapshot_avatar_select");
    if (!picker) return;
    floater->addDependentFloater(picker);   // docked beside the Snapshot floater
    picker->refreshList();
}

void snapshotScopeChanged(LLFloaterSnapshotBase* floater)
{
    if (!floater) return;
    // (the combo, not the setting: its control may not be written yet at commit time)
    U32 scope = scopeSetting();
    if (LLComboBox* combo = floater->findChild<LLComboBox>("avatar_scope_combo"))
        scope = (U32)llclamp(combo->getValue().asInteger(), (S32)SNAPSHOT_AVATARS_SELF, (S32)SNAPSHOT_AVATARS_ALL);
    if (scope == SNAPSHOT_AVATARS_SELECTED)
    {
        snapshotShowPicker(floater);
    }
    else if (LLFloaterSnapshotAvatarSelect* picker = LLFloaterReg::findTypedInstance<LLFloaterSnapshotAvatarSelect>("snapshot_avatar_select"))
    {
        if (picker->isInVisibleChain()) picker->closeFloater();
    }
}

// ---------------------------------------------------------------------------
// LLFloaterSnapshotAvatarSelect
// ---------------------------------------------------------------------------

LLFloaterSnapshotAvatarSelect::LLFloaterSnapshotAvatarSelect(const LLSD& key)
:   LLFloater(key)
{
}

// static
void LLFloaterSnapshotAvatarSelect::onAvatarNameCacheCallback(LLHandle<LLFloaterSnapshotAvatarSelect> handle, const LLUUID& agent_id, const LLAvatarName& av_name)
{
    if (LLFloaterSnapshotAvatarSelect* floater = handle.get()) floater->onAvatarNameCache(agent_id, av_name);
}

// static
void LLFloaterSnapshotAvatarSelect::onCheckItemCallback(LLHandle<LLFloaterSnapshotAvatarSelect> handle, LLUICtrl* ctrl, const LLUUID& id)
{
    if (LLFloaterSnapshotAvatarSelect* floater = handle.get()) floater->onCheckItem(ctrl, id);
}

bool LLFloaterSnapshotAvatarSelect::postBuild()
{
    mAvatarList = getChild<LLScrollListCtrl>("avatar_list");
    mFilterEditor = getChild<LLFilterEditor>("avatar_filter");
    mFilterEditor->setCommitCallback(boost::bind(&LLFloaterSnapshotAvatarSelect::onFilterEdit, this, _2));
    childSetAction("select_all_btn", boost::bind(&LLFloaterSnapshotAvatarSelect::onSelectAll, this));
    childSetAction("clear_btn", boost::bind(&LLFloaterSnapshotAvatarSelect::onDeselectAll, this));
    childSetAction("refresh_btn", boost::bind(&LLFloaterSnapshotAvatarSelect::refreshList, this));
    return LLFloater::postBuild();
}

void LLFloaterSnapshotAvatarSelect::onOpen(const LLSD& key)
{
    refreshList();
}

// A display name for the list (RLVa @shownames: anonymized).
static std::string pickerName(const LLUUID& id, const std::string& display, const std::string& user)
{
    if (!RlvActions::canShowName(RlvActions::SNC_DEFAULT, id)) return RlvStrings::getAnonym(display);
    std::string full = display;
    if (!user.empty() && user != display) full += " (" + user + ")";
    return full;
}

std::string LLFloaterSnapshotAvatarSelect::listName(const NearbyAvatarInfo& info) const
{
    if (!info.self) return pickerName(info.id, info.displayName, info.userName);
    std::string full = info.displayName;
    if (!info.userName.empty() && info.userName != info.displayName) full += " (" + info.userName + ")";
    LLStringUtil::format_map_t args;
    args["[NAME]"] = full;
    return getString("self_name", args);
}

void LLFloaterSnapshotAvatarSelect::refreshList()
{
    mNearbyAvatars.clear();
    const LLVector3d camera_pos = gAgentCamera.getCameraPositionGlobal();
    // You, first ("Selected" can leave you out; ticked to begin with).
    if (isAgentAvatarValid())
    {
        NearbyAvatarInfo me;
        me.id = gAgentID;
        me.self = true;
        me.distance = (F32)(gAgentAvatarp->getPositionGlobal() - camera_pos).magVec();
        LLAvatarName av_name;
        if (LLAvatarNameCache::get(gAgentID, &av_name))
        {
            me.displayName = av_name.getDisplayName();
            me.userName = av_name.getUserName();
        }
        else
        {
            me.displayName = gCacheName ? gCacheName->getDefaultName() : std::string("Loading...");
            LLAvatarNameCache::get(gAgentID, boost::bind(&LLFloaterSnapshotAvatarSelect::onAvatarNameCacheCallback,
                                                         getDerivedHandle<LLFloaterSnapshotAvatarSelect>(), _1, _2));
        }
        mNearbyAvatars.push_back(me);
    }
    // RLVa @shownearby: nobody else to list.
    if (RlvActions::canShowNearbyAgents())
    {
        std::set<LLUUID> seen_ids;
        for (LLCharacter* character : LLCharacter::sInstances)
        {
            LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(character);
            if (!avatar || avatar->isDead() || avatar->mIsDummy || avatar->isOrphaned() || avatar->isControlAvatar()
                || avatar->isSelf() || avatar->isUIAvatar())
                continue;
            const LLUUID av_id = avatar->getID();
            if (!seen_ids.insert(av_id).second) continue;
            NearbyAvatarInfo info;
            info.id = av_id;
            info.distance = (F32)(avatar->getPositionGlobal() - camera_pos).magVec();
            LLAvatarName av_name;
            if (LLAvatarNameCache::get(av_id, &av_name))
            {
                info.displayName = av_name.getDisplayName();
                info.userName = av_name.getUserName();
            }
            else
            {
                info.displayName = gCacheName ? gCacheName->getDefaultName() : std::string("Loading...");
                LLAvatarNameCache::get(av_id, boost::bind(&LLFloaterSnapshotAvatarSelect::onAvatarNameCacheCallback,
                                                          getDerivedHandle<LLFloaterSnapshotAvatarSelect>(), _1, _2));
            }
            mNearbyAvatars.push_back(info);
        }
    }
    // You, then the others closest to the camera first.
    std::sort(mNearbyAvatars.begin(), mNearbyAvatars.end(),
              [](const NearbyAvatarInfo& a, const NearbyAvatarInfo& b)
              { return a.self != b.self ? a.self : a.distance < b.distance; });

    if (!mAvatarList) return;
    mAvatarList->deleteAllItems();
    std::string filter_lower = mFilterString;
    LLStringUtil::toLower(filter_lower);
    for (const NearbyAvatarInfo& info : mNearbyAvatars)
    {
        const std::string name = listName(info);
        if (!filter_lower.empty())
        {
            std::string name_lower = name;
            LLStringUtil::toLower(name_lower);
            if (name_lower.find(filter_lower) == std::string::npos) continue;
        }
        LLSD row;
        row["id"] = info.id;
        LLSD& columns = row["columns"];
        columns[0]["column"] = "check";
        columns[0]["type"] = "checkbox";
        columns[0]["value"] = info.self ? gSnapshotSelectedSelf : gSnapshotSelectedAvatars.count(info.id) > 0;
        columns[1]["column"] = "name";
        columns[1]["type"] = "text";
        columns[1]["value"] = name;
        columns[2]["column"] = "distance";
        columns[2]["type"] = "text";
        columns[2]["value"] = llformat("%.1fm", info.distance);
        LLScrollListItem* item = mAvatarList->addElement(row);
        if (!item) continue;
        LLScrollListCheck* check_cell = dynamic_cast<LLScrollListCheck*>(item->getColumn(0));
        if (check_cell && check_cell->getCheckBox())
        {
            check_cell->getCheckBox()->setCommitCallback(boost::bind(&LLFloaterSnapshotAvatarSelect::onCheckItemCallback,
                                                                     getDerivedHandle<LLFloaterSnapshotAvatarSelect>(), _1, info.id));
        }
    }
}

void LLFloaterSnapshotAvatarSelect::onCheckItem(LLUICtrl* ctrl, const LLUUID& id)
{
    LLCheckBoxCtrl* checkbox = dynamic_cast<LLCheckBoxCtrl*>(ctrl);
    if (!checkbox) return;
    if (id == gAgentID) gSnapshotSelectedSelf = checkbox->get();
    else if (checkbox->get()) gSnapshotSelectedAvatars.insert(id);
    else gSnapshotSelectedAvatars.erase(id);
    updatePreview();
}

void LLFloaterSnapshotAvatarSelect::onSelectAll()
{
    for (const NearbyAvatarInfo& info : mNearbyAvatars)
    {
        if (info.self) gSnapshotSelectedSelf = true;
        else gSnapshotSelectedAvatars.insert(info.id);
    }
    refreshList();
    updatePreview();
}

void LLFloaterSnapshotAvatarSelect::onDeselectAll()
{
    gSnapshotSelectedAvatars.clear();
    gSnapshotSelectedSelf = false;
    refreshList();
    updatePreview();
}

void LLFloaterSnapshotAvatarSelect::onFilterEdit(const LLSD& value)
{
    mFilterString = value.asString();
    refreshList();
}

void LLFloaterSnapshotAvatarSelect::onAvatarNameCache(const LLUUID& agent_id, const LLAvatarName& av_name)
{
    const NearbyAvatarInfo* named = nullptr;
    for (NearbyAvatarInfo& info : mNearbyAvatars)
    {
        if (info.id != agent_id) continue;
        info.displayName = av_name.getDisplayName();
        info.userName = av_name.getUserName();
        named = &info;
        break;
    }
    if (!mAvatarList || !named) return;
    if (LLScrollListItem* item = mAvatarList->getItem(agent_id))
    {
        if (LLScrollListCell* name_cell = item->getColumn(1)) name_cell->setValue(listName(*named));
    }
}

void LLFloaterSnapshotAvatarSelect::updatePreview()
{
    LLFloaterSnapshot* snap_floater = LLFloaterSnapshot::findInstance();
    LLSnapshotLivePreview* previewp = snap_floater ? snap_floater->getPreviewView() : nullptr;
    if (previewp) previewp->updateSnapshot(true, true);
}
