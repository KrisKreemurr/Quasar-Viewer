/**
 * @file slfloaterquickprefs.cpp
 * @brief Quick Preferences: the hover height floater plus the settings people change
 *        mid-session (#1256: avatar count, object detail, name tags, sky and water)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * (LGPL v2.1, see slfloaterquickprefs.h)
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "slfloaterquickprefs.h"

#include <set>

#include "llcombobox.h"
#include "llenvironment.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorysettings.h"
#include "lltrans.h"
#include "llviewercontrol.h"
#include "llvoavatar.h"
#if defined(LL_HAS_CLEAN_PIPELINE)
#include "vkgfx/llvkneuralrecon.h"
#endif

namespace
{
    const std::string REGION_DEFAULT("region");

    // Environment settings in the inventory and the Library, each asset once, not the ones
    // listed on the Marketplace
    class EnvironmentCollector : public LLInventoryCollectFunctor
    {
    public:
        bool operator()(LLInventoryCategory*, LLInventoryItem* item) override
        {
            if (!item || item->getType() != LLAssetType::AT_SETTINGS
                || (mMarketplace.notNull() && gInventory.isObjectDescendentOf(item->getUUID(), mMarketplace)))
            {
                return false;
            }
            return mSeen.insert(item->getAssetUUID()).second;
        }

    private:
        const LLUUID mMarketplace = gInventory.findCategoryUUIDForType(LLFolderType::FT_MARKETPLACE_LISTINGS);
        std::set<LLUUID> mSeen;
    };

    using preset_list_t = std::vector<std::pair<std::string, LLUUID>>;

    void fillPresets(LLComboBox* combo, preset_list_t& presets, const std::string& region_label)
    {
        std::sort(presets.begin(), presets.end(), [](const auto& a, const auto& b)
            {
                return LLStringUtil::compareDict(a.first, b.first) < 0;
            });
        combo->removeall();
        combo->add(region_label, LLSD(REGION_DEFAULT));
        if (!presets.empty())
        {
            combo->addSeparator();
        }
        for (const auto& [name, asset_id] : presets)
        {
            combo->add(name, LLSD(asset_id));
        }
    }

    // The local environment's sky / water if it is one of the listed presets; "Region default"
    // without a local environment; for any other local one, nothing selected and other_label
    void selectPreset(LLComboBox* combo, bool local, const LLUUID& asset_id, const std::string& other_label)
    {
        if (!local)
        {
            combo->selectByValue(LLSD(REGION_DEFAULT));
        }
        else if (asset_id.isNull() || !combo->selectByValue(LLSD(asset_id)))
        {
            combo->clear();
            combo->setLabel(other_label);
        }
    }
}

SLFloaterQuickPrefs::SLFloaterQuickPrefs(const LLSD& key)
:   LLFloaterHoverHeight(key)
{
}

bool SLFloaterQuickPrefs::postBuild()
{
    if (!LLFloaterHoverHeight::postBuild())
    {
        return false;
    }

    getChild<LLUICtrl>("qp_max_avatars")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onMaxAvatarsCommit(); });
    mUpscaleCombo = getChild<LLComboBox>("qp_upscale");
    mUpscaleToolTip = mUpscaleCombo->getToolTip();
    updateUpscalerAvailability();
    // Preferences > Graphics sets it too
    if (LLControlVariablePtr control = gSavedSettings.getControl("RenderAvatarMaxNonImpostors"))
    {
        mMaxAvatarsConn = control->getSignal()->connect([this](LLControlVariable*, const LLSD&, const LLSD&)
            {
                updateMaxAvatarsText();
            });
    }

    mSkyCombo = getChild<LLComboBox>("qp_sky");
    mWaterCombo = getChild<LLComboBox>("qp_water");
    mSkyCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onEnvironmentCommit(mSkyCombo); });
    mWaterCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onEnvironmentCommit(mWaterCombo); });
    // also World > Environment, My Environments and a preset that finished loading
    mEnvChangedConn = LLEnvironment::instance().setEnvironmentChanged([this](LLEnvironment::EnvSelection_t, S32)
        {
            selectEnvironmentPresets();
        });

    return true;
}

void SLFloaterQuickPrefs::onOpen(const LLSD& key)
{
    LLFloaterHoverHeight::onOpen(key);
    updateMaxAvatarsText();
    updateUpscalerAvailability();
    // the inventory may have new presets since the last time
    loadEnvironmentPresets();
}

void SLFloaterQuickPrefs::updateUpscalerAvailability()
{
#if defined(LL_HAS_CLEAN_PIPELINE)
    const bool dlssAvailable = LLVKNeuralRecon::instance().available();
    for (S32 mode = 7; mode <= 11; ++mode)
    {
        mUpscaleCombo->setEnabledByValue(LLSD(mode), dlssAvailable);
    }
    std::string tip = mUpscaleToolTip;
    if (!dlssAvailable)
    {
        LLStringUtil::format_map_t args;
        const std::string& reason = LLVKNeuralRecon::instance().reason();
        args["[REASON]"] = reason.empty() ? getString("dlss_not_loaded") : reason;
        tip += "\n\n" + getString("qp_dlss_unavailable", args);
    }
    mUpscaleCombo->setToolTip(tip);
#endif
}

void SLFloaterQuickPrefs::onMaxAvatarsCommit()
{
    // As Preferences > Graphics: IndirectMaxNonImpostors (the slider) at its far right is
    // unlimited (0)
    U32 value = getChild<LLUICtrl>("qp_max_avatars")->getValue().asInteger();
    if (value >= LLVOAvatar::NON_IMPOSTORS_MAX_SLIDER)
    {
        value = 0;
    }
    gSavedSettings.setU32("RenderAvatarMaxNonImpostors", value);
    LLVOAvatar::updateImpostorRendering(value);   // now, not at the next avatar update
}

void SLFloaterQuickPrefs::updateMaxAvatarsText()
{
    const U32 value = gSavedSettings.getU32("RenderAvatarMaxNonImpostors");
    getChild<LLUICtrl>("qp_max_avatars_text")->setValue(value == 0 ? LLTrans::getString("no_limit") : llformat("%u", value));
}

void SLFloaterQuickPrefs::loadEnvironmentPresets()
{
    LLInventoryModel::cat_array_t cats;
    LLInventoryModel::item_array_t items;
    EnvironmentCollector collector;
    gInventory.collectDescendentsIf(gInventory.getRootFolderID(), cats, items, LLInventoryModel::EXCLUDE_TRASH, collector);
    gInventory.collectDescendentsIf(gInventory.getLibraryRootFolderID(), cats, items, LLInventoryModel::EXCLUDE_TRASH, collector);

    preset_list_t skies;
    preset_list_t waters;
    for (const LLPointer<LLViewerInventoryItem>& item : items)
    {
        switch (LLSettingsType::fromInventoryFlags(item->getFlags()))
        {
            case LLSettingsType::ST_SKY:   skies.emplace_back(item->getName(), item->getAssetUUID());  break;
            case LLSettingsType::ST_WATER: waters.emplace_back(item->getName(), item->getAssetUUID()); break;
            default: break;
        }
    }

    const std::string region_label = getString("region_default");
    fillPresets(mSkyCombo, skies, region_label);
    fillPresets(mWaterCombo, waters, region_label);
    selectEnvironmentPresets();
}

void SLFloaterQuickPrefs::selectEnvironmentPresets()
{
    LLEnvironment& env = LLEnvironment::instance();
    const bool local = env.getSelectedEnvironment() == LLEnvironment::ENV_LOCAL && env.hasEnvironment(LLEnvironment::ENV_LOCAL);
    LLSettingsSky::ptr_t sky = local ? env.getEnvironmentFixedSky(LLEnvironment::ENV_LOCAL) : LLSettingsSky::ptr_t();
    LLSettingsWater::ptr_t water = local ? env.getEnvironmentFixedWater(LLEnvironment::ENV_LOCAL) : LLSettingsWater::ptr_t();
    const std::string other_label = getString("other_environment");
    selectPreset(mSkyCombo, local, sky ? sky->getAssetId() : LLUUID::null, other_label);
    selectPreset(mWaterCombo, local, water ? water->getAssetId() : LLUUID::null, other_label);
}

void SLFloaterQuickPrefs::onEnvironmentCommit(LLComboBox* combo)
{
    const LLSD value = combo->getValue();
    LLEnvironment& env = LLEnvironment::instance();
    if (value.asString() == REGION_DEFAULT)
    {
        // As World > Environment > Use Shared Environment: the whole local environment goes
        env.clearEnvironment(LLEnvironment::ENV_LOCAL);
        env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
    }
    else if (value.isUUID() && value.asUUID().notNull())
    {
        // As My Environments' Apply Locally. A sky keeps the local water (or takes the one
        // showing), a water the local sky.
        env.setEnvironment(LLEnvironment::ENV_LOCAL, value.asUUID());
        env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL);
    }
}
