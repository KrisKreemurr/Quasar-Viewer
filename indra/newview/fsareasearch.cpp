/**
 * @file fsareasearch.cpp
 * @brief Floater to search and list objects in view or is known to the viewer.
 *
 * $LicenseInfo:firstyear=2012&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (c) 2012 Techwolf Lupindo
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
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"
#include <new>

#include "fsareasearch.h"

#include "llavatarnamecache.h"
#include "llscrolllistctrl.h"
#include "lllineeditor.h"
#include "lltextbox.h"
#include "llfloaterreg.h"
#include "llagent.h"
#include "lltracker.h"
#include "llviewerobjectlist.h"
#include "llviewercontrol.h"
#include "llviewerparcelmgr.h"
#include <boost/algorithm/string/find.hpp> //for boost::ifind_first
#include <unordered_set>
#include "llviewerregion.h"
#include "llselectmgr.h"
#include "llcallbacklist.h"
#include "lltoolpie.h"
#include "llsaleinfo.h"
#include "llcheckboxctrl.h"
#include "llviewermenu.h"       // handle_object_touch(), handle_buy()
#include "llradiogroup.h"
#include "llbutton.h"
#include "lluictrlfactory.h"
#include "llmenugl.h"
#include "slparcelownerbar.h"
#include "slparcelsummary.h"
#include "slregionparcels.h"
#include "llparcel.h"
#include "llspinctrl.h"
#include "lltoolgrab.h"
#include "slscriptinfo.h"   // #425: Script Info through the #Slipstream Helper bridge
#include "llcombobox.h"
#include "llnotificationsutil.h"
// #426: FS writes to its asset blacklist here; we use our derender list (llderenderlist.h),
// the one list behind every derender/blacklist action.
#include "llderenderlist.h"
#include "llworld.h"
#include "lltrans.h"    // getString()
#include "llagentcamera.h" // gAgentCamera
#include "llviewerjoystick.h" // For disabling/re-enabling when requested to look at an object.
#include "llmoveview.h" // For LLPanelStandStopFlying::clearStandStopFlyingMode
#include "rlvactions.h"
#include "fsareasearchmenu.h"
#include "llscrolllistctrl.h"
#include "llviewermediafocus.h"
#include "lltoolmgr.h"
#include "rlvhandler.h"
#include "llclipboard.h"

// max number of objects that can be (de-)selected in a single packet.
constexpr S32 MAX_OBJECTS_PER_PACKET = 255;

// time in seconds between refreshes when active
constexpr F32 REFRESH_INTERVAL = 1.0f;

// this is used to prevent refreshing too often and affecting performance.
constexpr F32 MIN_REFRESH_INTERVAL = 0.25f;

// how far the avatar needs to move to trigger a distance update
constexpr F32 MIN_DISTANCE_MOVED = 1.0f;

// typed search text applies after this pause, not on every keystroke
constexpr F32 SEARCH_TYPING_DELAY = 0.3f;

// the parcel summary: how often it is recomputed at most, and how soon after objects
// appear on or vanish from the parcel the server is asked for its count again
constexpr F32 SUMMARY_INTERVAL = 0.5f;
constexpr F32 PARCEL_INFO_INTERVAL = 5.0f;
// lookups retried, PARCEL_INFO_INTERVAL apart, while a parcel's numbers are still unknown
constexpr S32 MAX_PARCEL_INFO_RETRIES = 5;

// timeout to resend object properties request again
constexpr F32 REQUEST_TIMEOUT = 30.0f;

// Land Impact: seconds between re-requests for rows whose cost has not arrived yet,
// how many re-requests per object before giving up (a manual refresh still retries),
// and the cap on how many rows are tracked at once.
constexpr F32 COST_RETRY_INTERVAL = 5.0f;
constexpr U8 MAX_COST_RETRIES = 5;
constexpr size_t MAX_PENDING_COSTS = 8192;

// The window state setting and its scope keys (#1321).
namespace
{
    constexpr const char* STATE_SETTING = "SlipstreamAreaSearchState";

    const char* scope_key(FSAreaSearch::EScope scope)
    {
        switch (scope)
        {
        case FSAreaSearch::EScope::Region: return "region";
        case FSAreaSearch::EScope::Nearby: return "nearby";
        case FSAreaSearch::EScope::Pinned: return "pinned";
        case FSAreaSearch::EScope::Parcel:
        default:                           return "parcel";
        }
    }

    FSAreaSearch::EScope scope_from_key(const std::string& key)
    {
        if (key == "region")
        {
            return FSAreaSearch::EScope::Region;
        }
        if (key == "nearby")
        {
            return FSAreaSearch::EScope::Nearby;
        }
        if (key == "pinned")
        {
            return FSAreaSearch::EScope::Pinned;
        }
        return FSAreaSearch::EScope::Parcel;
    }
}

static std::string RLVa_hideNameIfRestricted(std::string_view name)
{
    if (!gRlvHandler.hasBehaviour(RLV_BHVR_SHOWNAMES))
        return std::string(name);
    else
        return RlvStrings::getAnonym(std::string(name));
}

F32 static calculateObjectDistance(LLVector3d agent_pos, LLViewerObject* object)
{
    if (object->isHUDAttachment())
    {
        return 0.f;
    }
    else
    {
        return (F32)dist_vec(agent_pos, object->getPositionGlobal());
    }
}

class FSAreaSearch::FSParcelChangeObserver : public LLParcelObserver
{
public:
    FSParcelChangeObserver(FSAreaSearch* area_search_floater) : mAreaSearchFloater(area_search_floater) {}

private:
    void changed() override
    {
        if (mAreaSearchFloater)
        {
            mAreaSearchFloater->checkRegion();
        }
    }

    FSAreaSearch* mAreaSearchFloater;
};

class FSAreaSearchTouchTimer : public LLEventTimer
{
public:
    FSAreaSearchTouchTimer(const LLUUID& object_id, F32 timeout) :
        LLEventTimer(timeout),
        mObjectID(object_id)
    {
    }

    bool tick() override
    {
        LLViewerObject* objectp = gObjectList.findObject(mObjectID);
        if (objectp)
        {
            FSPanelAreaSearchList::touchObject(objectp);
        }

        return true;
    }

private:
    LLUUID mObjectID;
};

FSAreaSearch::FSAreaSearch(const LLSD& key) :
    LLFloater(key)
{
    // FS-only LLAgent flag; not present in Alchemy.
    //gAgent.setFSAreaSearchActive(true);
    gAgent.changeInterestListMode(IL_MODE_360);
    mFactoryMap["area_search_list_panel"] = LLCallbackMap(createPanelList, this);

    // Register an idle update callback
    gIdleCallbacks.addFunction(idle, this);

    mParcelChangedObserver = std::make_unique<FSParcelChangeObserver>(this);
    LLViewerParcelMgr::getInstance()->addObserver(mParcelChangedObserver.get());

    mRegionParcels = std::make_unique<SLRegionParcelTracker>([this]() { updateParcelSummary(); });
}

FSAreaSearch::~FSAreaSearch()
{
    //gAgent.setFSAreaSearchActive(false);  // see ctor

    // Tell the Simulator not to send us everything anymore
    // and revert to the regular "keyhole" frustum of interest
    // list updates.
    if (!LLApp::isExiting())
    {
        gAgent.changeInterestListMode(IL_MODE_DEFAULT);
    }

    if (!gIdleCallbacks.deleteFunction(idle, this))
    {
        LL_WARNS("FSAreaSearch") << "FSAreaSearch::~FSAreaSearch() failed to delete callback" << LL_ENDL;
    }

    if (mRlvBehaviorCallbackConnection.connected())
    {
        mRlvBehaviorCallbackConnection.disconnect();
    }

    for (const auto& cb : mNameCacheConnections)
    {
        if (cb.second.connected())
        {
            cb.second.disconnect();
        }
    }
    mNameCacheConnections.clear();

    if (mParcelChangedObserver)
    {
        LLViewerParcelMgr::getInstance()->removeObserver(mParcelChangedObserver.get());
        mParcelChangedObserver = nullptr;
    }
}

bool FSAreaSearch::postBuild()
{
    mScopeRadio = getChild<LLRadioGroup>("scope_radio");
    mScopeRadio->setCommitCallback(boost::bind(&FSAreaSearch::onScopeCommit, this));
    mNearbyRadiusSpin = getChild<LLSpinCtrl>("nearby_radius");
    mNearbyRadiusSpin->setCommitCallback(boost::bind(&FSAreaSearch::onRadiusCommit, this));

    mSearchEditor = getChild<LLLineEditor>("search_text");
    mSearchEditor->setKeystrokeCallback([this](LLLineEditor*, void*) { onSearchKeystroke(); }, nullptr);
    mSearchEditor->setCommitCallback(boost::bind(&FSAreaSearch::applySearch, this));
    mSearchFieldCombo = getChild<LLComboBox>("search_field");
    mSearchFieldCombo->setCommitCallback(boost::bind(&FSAreaSearch::applySearch, this));
    mSearchRegexCheck = getChild<LLCheckBoxCtrl>("search_regex");
    mSearchRegexCheck->setCommitCallback(boost::bind(&FSAreaSearch::applySearch, this));

    for (size_t i = 0; i < mTriStateCombos.size(); ++i)
    {
        const SLAreaSearchProperty property = (SLAreaSearchProperty)i;
        LLComboBox* combo = getChild<LLComboBox>(std::string("tri_") + sl_area_search_property_key(property));
        combo->setCommitCallback(boost::bind(&FSAreaSearch::onTriStateCommit, this, property, combo));
        mTriStateCombos[i] = combo;
    }

    mPriceMinSpin = getChild<LLSpinCtrl>("price_min");
    mPriceMinSpin->setCommitCallback(boost::bind(&FSAreaSearch::onMoreFiltersCommit, this));
    mPriceMaxSpin = getChild<LLSpinCtrl>("price_max");
    mPriceMaxSpin->setCommitCallback(boost::bind(&FSAreaSearch::onMoreFiltersCommit, this));
    mClickActionCombo = getChild<LLComboBox>("click_action");
    mClickActionCombo->setCommitCallback(boost::bind(&FSAreaSearch::onMoreFiltersCommit, this));

    mFiltersPanel = getChild<LLPanel>("filters_panel");
    mMoreFiltersPanel = getChild<LLPanel>("more_filters_panel");
    mFiltersPanelCollapsedHeight = mFiltersPanel->getRect().getHeight() - mMoreFiltersPanel->getRect().getHeight();
    mMoreFiltersButton = getChild<LLButton>("more_filters_btn");
    mMoreFiltersButton->setClickedCallback([this](LLUICtrl*, const LLSD&) { showMoreFilters(!mMoreFiltersShown); saveState(); });

    mParcelNameText = getChild<LLTextBox>("parcel_name");
    mParcelUsedText = getChild<LLTextBox>("parcel_used");
    mParcelPoolText = getChild<LLTextBox>("parcel_pool");
    mOwnerBar = getChild<SLParcelOwnerBar>("owner_bar");
    mOwnerBar->setCommitCallback([this](LLUICtrl*, const LLSD&) { setOwnerFilter(mOwnerBar->getPickedOwner()); });
    mOwnerChipPanel = getChild<LLView>("owner_chip_panel");
    mOwnerChipText = getChild<LLTextBox>("owner_chip_text");
    getChild<LLButton>("owner_chip_clear")->setClickedCallback([this](LLUICtrl*, const LLSD&) { setOwnerFilter(LLUUID::null); });

    // The one-window layout (#1321) is wider than the old tabbed one, so a size saved from that
    // would open it squeezed to the minimum. Keep its size under a name of its own (the saved
    // position stays); LLFloater applies the saved rect after postBuild().
    if (!mRectControl.empty())
    {
        mRectControl = LLFloaterReg::declareRectControl("area_search_v2");
    }

    loadState();
    applyDefaultSort();
    updateParcelSummary(true);

    mRlvBehaviorCallbackConnection = gRlvHandler.setBehaviourCallback(boost::bind(&FSAreaSearch::updateRlvRestrictions, this, _1));

    return LLFloater::postBuild();
}

void FSAreaSearch::onOpen(const LLSD& key)
{
    // Listing starts as soon as the window opens; there is no Search button.
    refreshList(false);
    requestParcelInfo();
}

void FSAreaSearch::draw()
{
    LLFloater::draw();
}

void FSAreaSearch::addBeacons()
{

    static LLCachedControl<S32> beacon_line_width(gSavedSettings, "DebugBeaconLineWidth");
    static LLUIColor mBeaconColor = LLUIColorTable::getInstance()->getColor("AreaSearchBeaconColor");
    static LLUIColor mBeaconTextColor = LLUIColorTable::getInstance()->getColor("PathfindingDefaultBeaconTextColor");

    if (mBeacons)
    {
        std::vector<LLScrollListItem*> items = mPanelList->getResultList()->getAllData();

        for (const auto* item : items)
        {
            if (LLViewerObject* objectp = gObjectList.findObject(item->getUUID()); objectp)
            {
                const std::string& objectName = mObjectDetails[item->getUUID()].description;
                // Object-tracking beacon: drawn at the object's current render pose
                // (follows movers), dropped if the object dies before the draw.
                gObjectList.addDebugBeacon(objectp, objectName, mBeaconColor, mBeaconTextColor, beacon_line_width);
            }
        }
    }
}

//static
void FSAreaSearch::idle(void* user_data)
{
    FSAreaSearch* self = (FSAreaSearch*)user_data;
    if (self->mSearchPending && self->mSearchTypingTimer.getElapsedTimeF32() > SEARCH_TYPING_DELAY)
    {
        self->applySearch();
    }
    if (self->mRadiusPending && self->mRadiusTimer.getElapsedTimeF32() > SEARCH_TYPING_DELAY)
    {
        self->applyRadius();
    }
    if (self->mActive && self->mScope == EScope::Parcel)
    {
        // Walked onto another parcel, or the server pushed the agent's parcel late.
        const LLParcel* parcel = LLViewerParcelMgr::instance().getAgentParcel();
        const LLViewerRegion* region = gAgent.getRegion();
        if (parcel && region && parcel->getLocalID() != INVALID_PARCEL_ID
            && (parcel->getLocalID() != self->mParcelInfoLocalId || region->getRegionID() != self->mParcelInfoRegionId))
        {
            self->requestParcelInfo();
        }
    }
    if (self->mActive && (self->mScope == EScope::Parcel || self->mScope == EScope::Pinned) && !self->mParcelInfo.isMap()
        && self->mParcelInfoRetries < MAX_PARCEL_INFO_RETRIES && self->mParcelInfoTimer.getElapsedTimeF32() > PARCEL_INFO_INTERVAL)
    {
        // The lookup failed or timed out and nothing else has asked again.
        ++self->mParcelInfoRetries;
        self->requestParcelInfo();
    }
    if (self->mActive && self->mScope == EScope::Region)
    {
        self->mRegionParcels->update();
    }
    self->findObjects();
    self->processRequestQueue();
}

// static
void* FSAreaSearch::createPanelList(void* data)
{
    FSAreaSearch* self = (FSAreaSearch*)data;
    self->mPanelList = new FSPanelAreaSearchList(self);
    return self->mPanelList;
}

void FSAreaSearch::updateRlvRestrictions(ERlvBehaviour behavior)
{
    if (behavior == RLV_BHVR_SHOWNAMES)
    {
        refreshList(false);
        updateOwnerChip();
        updateParcelSummary(true);
    }
    else if (behavior == RLV_BHVR_SHOWLOC)
    {
        updateParcelSummary(true); // parcel and region names
    }
}

void FSAreaSearch::checkRegion()
{
    if (mActive)
    {
        // Check if we changed region, and if we did, clear the object details cache.
        if (LLViewerRegion* region = gAgent.getRegion(); region && (region != mLastRegion))
        {
            if (mScope == EScope::Nearby)
            {
                std::vector<LLViewerRegion*> uniqueRegions;
                region->getNeighboringRegions(uniqueRegions);
                if (std::find(uniqueRegions.begin(), uniqueRegions.end(), mLastRegion) != uniqueRegions.end())
                {
                    // Crossed into a neighboring region, no need to clear everything.
                    mLastRegion = region;
                    return;
                }
                // else teleported into a new region
            }
            mLastRegion = region;
            if (mScope == EScope::Pinned)
            {
                // The pinned parcel is in the region left behind.
                mScope = EScope::Parcel;
                mUnpinnedScope = EScope::Parcel;
                unpinParcel();
                mScopeRadio->setValue(scope_key(mScope));
                applyDefaultSort();
                saveState();
            }
            mRequested = 0;
            mObjectDetails.clear();
            mRegionRequests.clear();
            mLastPropertiesReceivedTimer.start();
            mPendingCosts.clear();
            mPanelList->getResultList()->deleteAllItems();
            mPanelList->setCounterText();
            mPanelList->setAgentLastPosition(gAgent.getPositionGlobal());
            mRefresh = true;
        }
    }
}

void FSAreaSearch::refreshList(bool cache_clear)
{
    mActive = true;
    checkRegion();
    if (cache_clear)
    {
        mRequested = 0;
        mObjectDetails.clear();
        mRegionRequests.clear();
        mLastPropertiesReceivedTimer.start();
    }
    else
    {
        for (auto& object_it : mObjectDetails)
        {
             object_it.second.listed = false;
        }
    }
    mPendingCosts.clear();
    mPanelList->getResultList()->deleteAllItems();
    mPanelList->setCounterText();
    mPanelList->setAgentLastPosition(gAgent.getPositionGlobal());
    mRefresh = true;
    findObjects();
}

void FSAreaSearch::findObjects()
{
    // Only loop through the gObjectList every so often. There is a performance hit if done too often.
    if (!(mActive && ((mRefresh && mLastUpdateTimer.getElapsedTimeF32() > MIN_REFRESH_INTERVAL) || mLastUpdateTimer.getElapsedTimeF32() > REFRESH_INTERVAL)))
    {
        return;
    }

    LLViewerRegion* our_region = gAgent.getRegion();
    if (!our_region)
    {
        // Got disconnected or is in the middle of a teleport.
        return;
    }

    LL_DEBUGS("FSAreaSearch_spammy") << "Doing a FSAreaSearch::findObjects" << LL_ENDL;

    mLastUpdateTimer.stop(); // stop sets getElapsedTimeF32() time to zero.
    // Pause processing of requestqueue until done adding new requests.
    mRequestQueuePause = true;
    checkRegion();
    mRefresh = false;
    mSearchableObjects = 0;
    S32 object_count = gObjectList.getNumObjects();

    for (S32 i = 0; i < object_count; i++)
    {
        LLViewerObject* objectp = gObjectList.getObject(i);
        if (!objectp || !isSearchableObject(objectp, our_region))
        {
            continue;
        }

        const LLUUID& object_id = objectp->getID();

        if (object_id.isNull())
        {
            LL_WARNS("FSAreaSearch") << "WTF?! Selectable object with id of NULL!!" << LL_ENDL;
            continue;
        }

        mSearchableObjects++;

        if (mObjectDetails.count(object_id) == 0)
        {
            FSObjectProperties& details = mObjectDetails[object_id];
            details.id = object_id;
            details.local_id = objectp->getLocalID();
            details.region_handle = objectp->getRegion()->getHandle();
            mRequestNeedsSent = true;
            mRequested++;
        }
        else
        {
            FSObjectProperties& details = mObjectDetails[object_id];
            if (details.request == FSObjectProperties::FINISHED)
            {
                matchObject(details, objectp);
            }

            if (details.request == FSObjectProperties::FAILED)
            {
                // object came back into view
                details.request = FSObjectProperties::NEED;
                details.local_id = objectp->getLocalID();
                details.region_handle = objectp->getRegion()->getHandle();
                mRequestNeedsSent = true;
                mRequested++;
            }
        }
    }

    mPanelList->updateScrollList();
    retryPendingCosts();
    updateParcelSummary();

    S32 request_count = 0;
    // requests for non-existent objects will never arrive, check and update the queue.
    for (auto& object_it : mObjectDetails)
    {
        if (object_it.second.request == FSObjectProperties::NEED || object_it.second.request == FSObjectProperties::SENT)
        {
            const LLUUID& id = object_it.second.id;
            if (LLViewerObject* objectp = gObjectList.findObject(id); !objectp)
            {
                object_it.second.request = FSObjectProperties::FAILED;
                mRequested--;
            }
            else
            {
                request_count++;
            }
        }
    }

    if (mRequested != request_count)
    {
        LL_DEBUGS("FSAreaSearch") << "Requested mismatch: " << request_count << " actual vs. " << mRequested << LL_ENDL;
        mRequested = request_count;
    }

    updateCounterText();
    mLastUpdateTimer.start(); // start also reset elapsed time to zero
    mRequestQueuePause = false;
}

bool FSAreaSearch::isSearchableObject(LLViewerObject* objectp, LLViewerRegion* our_region) const
{
    // need to be connected to region object is in.
    if (!objectp->getRegion())
    {
        return false;
    }

    // Land doesn't have object properties
    if (objectp->getPCode() == LLViewerObject::LL_VO_SURFACE_PATCH)
    {
        return false;
    }

    // Object needs to be selectable to get properties from the server
    if (!objectp->mbCanSelect)
    {
        return false;
    }

    // Avatars are spechiel objects that don't have normal properties
    if (objectp->isAvatar())
    {
        return false;
    }

    //-----------------------------------------------------------------------
    // What is fetched at all. Everything else (physical, temporary, ...) is a
    // filter in matchObject(), so changing it re-lists from the cache.
    //-----------------------------------------------------------------------

    if (mFilters.get(SLAreaSearchProperty::ChildPrim) == SLTriState::Hide
        && !(objectp->isRoot() || (objectp->isAttachment() && objectp->isRootEdit())))
    {
        return false;
    }

    // Neighbouring regions only count for Nearby.
    if (mScope != EScope::Nearby && objectp->getRegion() != our_region)
    {
        return false;
    }

    if (mFilters.get(SLAreaSearchProperty::Attachment) == SLTriState::Hide && objectp->isAttachment())
    {
        return false;
    }

    return true;
}

void FSAreaSearch::processRequestQueue()
{
    if (!mActive || mRequestQueuePause)
    {
        return;
    }

    if (mLastPropertiesReceivedTimer.getElapsedTimeF32() > REQUEST_TIMEOUT)
    {
        LL_DEBUGS("FSAreaSearch") << "Timeout reached, resending requests."<< LL_ENDL;
        S32 request_count = 0;
        S32 failed_count = 0;
        for (auto& object_it : mObjectDetails)
        {
            if (object_it.second.request == FSObjectProperties::SENT)
            {
                object_it.second.request = FSObjectProperties::NEED;
                mRequestNeedsSent = true;
                request_count++;
            }

            if (object_it.second.request == FSObjectProperties::FAILED)
            {
                failed_count++;
            }
        }

        mRegionRequests.clear();
        mLastPropertiesReceivedTimer.start();

        if (!mRequestNeedsSent)
        {
            LL_DEBUGS("FSAreaSearch") << "No pending requests found."<< LL_ENDL;
        }
        else
        {
            LL_DEBUGS("FSAreaSearch") << request_count << " pending requests found."<< LL_ENDL;
        }

        LL_DEBUGS("FSAreaSearch") << failed_count << " failed requests found."<< LL_ENDL;
    }

    if (!mRequestNeedsSent)
    {
        return;
    }
    mRequestNeedsSent = false;

    for (const auto regionp : LLWorld::getInstance()->getRegionList())
    {
        U64 region_handle = regionp->getHandle();
        if (mRegionRequests[region_handle] > (MAX_OBJECTS_PER_PACKET + 128))
        {
            mRequestNeedsSent = true;
            return;
        }

        std::vector<U32> request_list;
        bool need_continue = false;

        for (auto& object_it : mObjectDetails)
        {
            if (object_it.second.request == FSObjectProperties::NEED && object_it.second.region_handle == region_handle)
            {
                request_list.push_back(object_it.second.local_id);
                object_it.second.request = FSObjectProperties::SENT;
                mRegionRequests[region_handle]++;
                if (mRegionRequests[region_handle] >= ((MAX_OBJECTS_PER_PACKET * 3) - 3))
                {
                    requestObjectProperties(request_list, true, regionp);
                    requestObjectProperties(request_list, false, regionp);
                    mRequestNeedsSent = true;
                    need_continue = true;
                    break;
                }
            }
        }

        if (need_continue)
        {
            continue;
        }

        if (!request_list.empty())
        {
            requestObjectProperties(request_list, true, regionp);
            requestObjectProperties(request_list, false, regionp);
        }
    }
}

void FSAreaSearch::requestObjectProperties(const std::vector<U32>& request_list, bool select, LLViewerRegion* regionp)
{
    bool start_new_message = true;
    S32 select_count = 0;
    LLMessageSystem* msg = gMessageSystem;

    for (const auto& id : request_list)
    {
        if (start_new_message)
        {
            if (select)
            {
                msg->newMessageFast(_PREHASH_ObjectSelect);
            }
            else
            {
                msg->newMessageFast(_PREHASH_ObjectDeselect);
            }
            msg->nextBlockFast(_PREHASH_AgentData);
            msg->addUUIDFast(_PREHASH_AgentID, gAgentID);
            msg->addUUIDFast(_PREHASH_SessionID, gAgentSessionID);
            select_count++;
            start_new_message = false;
        }

        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_ObjectLocalID, id);
        select_count++;

        if (msg->isSendFull(NULL) || select_count >= MAX_OBJECTS_PER_PACKET)
        {
            LL_DEBUGS("FSAreaSearch") << "Sent one full " << (select ? "ObjectSelect" : "ObjectDeselect") << " message with " << select_count << " object data blocks." << LL_ENDL;
            msg->sendReliable(regionp->getHost());
            select_count = 0;
            start_new_message = true;
        }
    }

    if (!start_new_message)
    {
        LL_DEBUGS("FSAreaSearch") << "Sent one partcial " << (select ? "ObjectSelect" : "ObjectDeselect") << " message with " << select_count << " object data blocks." << LL_ENDL;
        msg->sendReliable(regionp->getHost());
    }
}

void FSAreaSearch::processObjectProperties(LLMessageSystem* msg)
{
    if (!mActive)
    {
        return;
    }

    LLViewerRegion* our_region = gAgent.getRegion();
    bool counter_text_update = false;

    S32 count = msg->getNumberOfBlocksFast(_PREHASH_ObjectData);
    LL_DEBUGS("FSAreaSearch")  << "Got processObjectProperties message with " << count << " object(s)" << LL_ENDL;
    for (S32 i = 0; i < count; i++)
    {
        LLUUID object_id;
        msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_ObjectID, object_id, i);
        if (object_id.isNull())
        {
            LL_WARNS("FSAreaSearch") << "Got Object Properties with NULL id" << LL_ENDL;
            continue;
        }

        LLViewerObject* objectp = gObjectList.findObject(object_id);
        if (!objectp)
        {
            continue;
        }

        FSObjectProperties& details = mObjectDetails[object_id];
        if (details.request != FSObjectProperties::FINISHED)
        {
            // We cache un-requested objects (to avoid having to request them later)
            // and requested objects.

            details.request = FSObjectProperties::FINISHED;
            mLastPropertiesReceivedTimer.start();

            if (details.id.isNull())
            {
                // Recieved object properties without requesting it.
                details.id = object_id;
            }
            else
            {
                if (mRequested > 0)
                {
                    mRequested--;
                }
                mRegionRequests[details.region_handle]--;
                counter_text_update = true;
            }

            msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_CreatorID, details.creator_id, i);
            msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_OwnerID, details.owner_id, i);
            msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_GroupID, details.group_id, i);
            msg->getU64Fast(_PREHASH_ObjectData, _PREHASH_CreationDate, details.creation_date, i);
            msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_BaseMask, details.base_mask, i);
            msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_OwnerMask, details.owner_mask, i);
            msg->getU32Fast(_PREHASH_ObjectData,_PREHASH_GroupMask, details.group_mask, i);
            msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_EveryoneMask, details.everyone_mask, i);
            msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_NextOwnerMask, details.next_owner_mask, i);
            details.sale_info.unpackMultiMessage(msg, _PREHASH_ObjectData, i);
            details.ag_perms.unpackMessage(msg, _PREHASH_ObjectData, _PREHASH_AggregatePerms, i);
            details.ag_texture_perms.unpackMessage(msg, _PREHASH_ObjectData, _PREHASH_AggregatePermTextures, i);
            details.ag_texture_perms_owner.unpackMessage(msg, _PREHASH_ObjectData, _PREHASH_AggregatePermTexturesOwner, i);
            details.category.unpackMultiMessage(msg, _PREHASH_ObjectData, i);
            msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_LastOwnerID, details.last_owner_id, i);
            msg->getStringFast(_PREHASH_ObjectData, _PREHASH_Name, details.name, i);
            msg->getStringFast(_PREHASH_ObjectData, _PREHASH_Description, details.description, i);
            msg->getStringFast(_PREHASH_ObjectData, _PREHASH_TouchName, details.touch_name, i);
            msg->getStringFast(_PREHASH_ObjectData, _PREHASH_SitName, details.sit_name, i);

            S32 size = msg->getSizeFast(_PREHASH_ObjectData, i, _PREHASH_TextureID);
            if (size > 0)
            {
                S8 packed_buffer[SELECT_MAX_TES * UUID_BYTES];
                msg->getBinaryDataFast(_PREHASH_ObjectData, _PREHASH_TextureID, packed_buffer, 0, i, SELECT_MAX_TES * UUID_BYTES);

                for (S32 buf_offset = 0; buf_offset < size; buf_offset += UUID_BYTES)
                {
                    LLUUID tid;
                    memcpy(tid.mData, packed_buffer + buf_offset, UUID_BYTES);      /* Flawfinder: ignore */
                    details.texture_ids.push_back(tid);
                }
            }

            details.permissions.init(details.creator_id, details.owner_id, details.last_owner_id, details.group_id);
            details.permissions.initMasks(details.base_mask, details.owner_mask, details.everyone_mask, details.group_mask, details.next_owner_mask);

            // Sets the group owned bool and real owner id, group or owner depending if object is group owned.
            details.permissions.getOwnership(details.ownership_id, details.group_owned);

            LL_DEBUGS("FSAreaSearch_spammy") << "Got properties for object: " << object_id << LL_ENDL;

            if (isSearchableObject(objectp, our_region))
            {
                matchObject(details, objectp);
            }
        }
    }

    if (counter_text_update)
    {
        updateCounterText();
    }
}

void FSAreaSearch::matchObject(FSObjectProperties& details, LLViewerObject* objectp)
{
    if (details.listed)
    {
        // object allready listed on the scroll list.
        return;
    }

    if (!isInScope(objectp) || (mOwnerFilter.notNull() && details.ownership_id != mOwnerFilter)
        || !mFilters.passes(buildFacts(details, objectp)))
    {
        return;
    }

    //-----------------------------------------------------------------------
    // Search text
    //-----------------------------------------------------------------------

    LLUUID object_id = details.id;
    std::string creator_name;
    std::string owner_name;
    std::string last_owner_name;
    std::string group_name;

    details.name_requested = false;
    getNameFromUUID(details.ownership_id, owner_name, details.group_owned, details.name_requested);
    getNameFromUUID(details.creator_id, creator_name, false, details.name_requested);
    getNameFromUUID(details.last_owner_id, last_owner_name, false, details.name_requested);
    getNameFromUUID(details.group_id, group_name, true, details.name_requested);

    // @shownames hides avatar names; a group owner's name stays, as in the Group column.
    if (!details.group_owned)
    {
        owner_name = RLVa_hideNameIfRestricted(owner_name);
    }
    last_owner_name = RLVa_hideNameIfRestricted(last_owner_name);

    if (!mSearch.matches({ details.name, details.description, owner_name, group_name, creator_name, last_owner_name }))
    {
        return;
    }

    //-----------------------------------------------------------------------
    // Object passed all above tests, add it to the list.
    //-----------------------------------------------------------------------

    details.listed = true;

    LLScrollListCell::Params cell_params;
    cell_params.font = LLFontGL::getFontSansSerif();

    LLScrollListItem::Params row_params;
    row_params.value = object_id.asString();

    cell_params.column = "distance";
    cell_params.value = llformat("%1.0f m", calculateObjectDistance(mPanelList->getAgentLastPosition(), objectp)); // used mAgentLastPosition instead of gAgent->getPositionGlobal for performace
    row_params.columns.add(cell_params);

    cell_params.column = "name";
    cell_params.value = details.name;
    row_params.columns.add(cell_params);

    cell_params.column = "description";
    cell_params.value = details.description;
    row_params.columns.add(cell_params);

    cell_params.column = "price";
    if (details.sale_info.isForSale())
    {
        LLStringUtil::format_map_t args;
        args["COST"] = llformat("%d", details.sale_info.getSalePrice());
        std::string cost_label = LLTrans::getString("FSAreaSearch_Cost_Label", args);
        cell_params.value = cost_label;
    }
    else
    {
        cell_params.value = " ";
    }
    row_params.columns.add(cell_params);

    cell_params.column = "land_impact";
    if (F32 cost = objectp->getLinksetCost(); cost > F_ALMOST_ZERO)
    {
        cell_params.value = sl_land_impact(cost);
    }
    else
    {
        // Not known yet: getLinksetCost() queued a GetObjectCost request; the reply
        // fills the cell via updateObjectCosts(), retryPendingCosts() re-asks if not.
        cell_params.value = "...";
        trackPendingCost(object_id);
    }
    row_params.columns.add(cell_params);

    cell_params.column = "prim_count";
    cell_params.value = objectp->numChildren() + 1;
    row_params.columns.add(cell_params);

    cell_params.column = "owner";
    cell_params.value = owner_name;
    row_params.columns.add(cell_params);

    cell_params.column = "group";
    cell_params.value = group_name;
    row_params.columns.add(cell_params);

    cell_params.column = "creator";
    cell_params.value = creator_name;
    row_params.columns.add(cell_params);

    cell_params.column = "last_owner";
    cell_params.value = last_owner_name;
    row_params.columns.add(cell_params);

    LLScrollListItem* list_row = mPanelList->getResultList()->addRow(row_params);

    if (objectp->flagTemporaryOnRez() || objectp->flagUsePhysics())
    {
        U8 font_style = LLFontGL::NORMAL;
        if (objectp->flagTemporaryOnRez())
        {
            font_style |= LLFontGL::ITALIC;
        }
        if (objectp->flagUsePhysics())
        {
            font_style |= LLFontGL::BOLD;
        }

        S32 num_colums = list_row->getNumColumns();
        for (S32 i = 0; i < num_colums; i++)
        {
            LLScrollListText* list_cell = (LLScrollListText*)list_row->getColumn(i);
            list_cell->setFontStyle(font_style);
        }
    }

    //mPanelList->getResultList()->refreshLineHeight(); // FS-only method
}

void FSAreaSearch::updateObjectCosts(const LLUUID& object_id, F32 object_cost, F32 link_cost, F32 physics_cost, F32 link_physics_cost)
{
    if (!mActive)
    {
        return;
    }

    // Called for every GetObjectCost reply; only listed objects have a row to update, but any
    // cached object in scope counts in the summary.
    if (auto it = mObjectDetails.find(object_id); it == mObjectDetails.end() || !it->second.listed)
    {
        if (mObjectDetails.count(object_id))
        {
            updateParcelSummary();
        }
        return;
    }
    updateParcelSummary();
    mPendingCosts.erase(object_id);

    if (LLScrollListCtrl* result_list = mPanelList->getResultList(); result_list)
    {
        if (LLScrollListItem* list_row = result_list->getItem(LLSD(object_id)); list_row)
        {
            if (LLScrollListColumn* list_column = result_list->getColumn("land_impact"); list_column)
            {
                if (LLScrollListCell* linkset_cost_cell = list_row->getColumn(list_column->mIndex); linkset_cost_cell)
                {
                    linkset_cost_cell->setValue(LLSD(sl_land_impact(link_cost)));
                    result_list->setNeedsSort(); // re-sort if needed.
                }
            }
        }
    }
}

void FSAreaSearch::trackPendingCost(const LLUUID& id)
{
    // Bounded: past the cap the row just keeps "..." until the list is refreshed.
    if (mPendingCosts.size() < MAX_PENDING_COSTS)
    {
        mPendingCosts.emplace(id, 0);
    }
}

void FSAreaSearch::retryPendingCosts()
{
    if (mPendingCosts.empty() || mCostRetryTimer.getElapsedTimeF32() < COST_RETRY_INTERVAL)
    {
        return;
    }
    mCostRetryTimer.reset();

    for (auto it = mPendingCosts.begin(); it != mPendingCosts.end(); )
    {
        LLViewerObject* objectp = gObjectList.findObject(it->first);
        if (!objectp)
        {
            it = mPendingCosts.erase(it);
            continue;
        }

        // getLinksetCost() re-queues the GetObjectCost request while the cost is still
        // stale (LLViewerObjectList dedupes ids already queued or in flight).
        if (F32 cost = objectp->getLinksetCost(); cost > F_ALMOST_ZERO)
        {
            // Arrived without reaching us; fill the cell (also drops the entry).
            const LLUUID id = it->first;
            ++it;
            updateObjectCosts(id, objectp->getObjectCost(), cost, objectp->getPhysicsCost(), objectp->getLinksetPhysicsCost());
            mPendingCosts.erase(id);
            continue;
        }

        if (++it->second > MAX_COST_RETRIES)
        {
            it = mPendingCosts.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void FSAreaSearch::getNameFromUUID(const LLUUID& id, std::string& name, bool group, bool& name_requested)
{
    static const std::string unknown_name = LLTrans::getString("AvatarNameWaiting");

    if (group)
    {
        // LLCacheName lacks getIfThere(); emulate by trying group then full name.
        bool found = gCacheName->getGroupName(id, name);
        if (!found) { found = gCacheName->getFullName(id, name); }
        if (!found)
        {
            name = unknown_name;
            if (!mNameCacheConnections.contains(id))
            {
                mNameCacheConnections.emplace(id, gCacheName->get(id, group, boost::bind(&FSAreaSearch::callbackLoadFullName, this, _1, _2)));
            }
            name_requested = true;
        }
    }
    else
    {
        if (LLAvatarName av_name; !LLAvatarNameCache::get(id, &av_name))
        {
            name = unknown_name;
            if (!mNameCacheConnections.contains(id))
            {
                mNameCacheConnections.emplace(id, LLAvatarNameCache::get(id, boost::bind(&FSAreaSearch::avatarNameCacheCallback, this, _1, _2)));
            }
            name_requested = true;
        }
        else
        {
            name = av_name.getCompleteName();
        }
    }
}

void FSAreaSearch::avatarNameCacheCallback(const LLUUID& id, const LLAvatarName& av_name)
{
    callbackLoadFullName(id, av_name.getCompleteName());
}

void FSAreaSearch::callbackLoadFullName(const LLUUID& id, const std::string& full_name)
{
    if (auto iter = mNameCacheConnections.find(id); iter != mNameCacheConnections.end())
    {
        if (iter->second.connected())
        {
            iter->second.disconnect();
        }
        mNameCacheConnections.erase(iter);
    }

    LLViewerRegion* our_region = gAgent.getRegion();

    for (auto& entry : mObjectDetails)
    {
        if (entry.second.name_requested && !entry.second.listed)
        {
            const LLUUID& object_id = entry.second.id;
            LLViewerObject* objectp = gObjectList.findObject(object_id);
            if (objectp && isSearchableObject(objectp, our_region))
            {
                matchObject(entry.second, objectp);
            }
        }
    }

    mPanelList->updateName(id, full_name);
    updateParcelSummary();
    updateOwnerChip();
}

void FSAreaSearch::updateCounterText()
{
    LLStringUtil::format_map_t args;
    args["[LISTED]"] = llformat("%d", mPanelList->getResultList()->getItemCount());
    args["[PENDING]"] = llformat("%d", mRequested);
    mPanelList->setCounterText(args);
}

//---------------------------------------------------------------------------
// Filter panel (#1321): where to look, the search box, one Any / Only / Hide
// control per property. Every change re-lists from the cache and is saved.
//---------------------------------------------------------------------------

void FSAreaSearch::loadState()
{
    LLSD state = gSavedSettings.getLLSD(STATE_SETTING);
    if (!state.isMap() || state.size() == 0)
    {
        try
        {
            LLSD legacy;
            static constexpr const char* names[] = {
                "OnlyTemporary", "ExcludeTemporary", "OnlyAttachments", "ExcludeAttachments",
                "OnlyPhysical", "ExcludePhysical", "OnlyPhantom", "OnlyLocked", "OnlyMOAP",
                "FilterForSale", "OnlyCopiable", "OnlyModifiable", "OnlyTransferable",
                "OnlyReflectionProbes", "ExcludeReflectionProbes", "ExcludeChildPrims",
                "MinimumPrice", "MaximumPrice", "ClickAction", "OnlyCurrentParcel",
                "FilterDistance", "MaximumDistance"
            };
            for (const char* name : names)
            {
                // User settings preserve unknown old controls; do not recreate their
                // obsolete debug settings or infer a legacy profile on fresh installs.
                if (const auto control = gSavedSettings.getControl(std::string("FSAreaSearch_") + name))
                {
                    legacy[name] = control->getValue();
                }
            }
            state = sl_area_search_migrate_legacy(legacy);
            if (state.isMap()) gSavedSettings.setLLSD(STATE_SETTING, state);
        }
        catch (const std::bad_alloc&)
        {
            state = LLSD();
            LL_WARNS("FSAreaSearch") << "Legacy filter migration deferred: allocation refused" << LL_ENDL;
        }
    }

    mScope = scope_from_key(state["scope"].asString());
    if (mScope == EScope::Pinned)
    {
        mScope = EScope::Parcel; // a pin is not remembered across sessions
    }
    mUnpinnedScope = mScope;
    mNearbyRadius = state.has("radius") ? llclamp(state["radius"].asInteger(), 1, 4096) : 64;
    mFilters = SLAreaSearchFilters::fromLLSD(state["filters"]);
    mMoreFiltersShown = state["more_open"].asBoolean();
    mDoubleClick.touch = state["dbl_touch"].asBoolean();
    mDoubleClick.buy = state["dbl_buy"].asBoolean();
    mDoubleClick.sit = state["dbl_sit"].asBoolean();

    mScopeRadio->setValue(scope_key(mScope));
    mNearbyRadiusSpin->set((F32)mNearbyRadius);
    mNearbyRadiusSpin->setEnabled(mScope == EScope::Nearby);
    const S32 field = state["field"].asInteger();
    mSearchFieldCombo->setCurrentByIndex(field >= 0 && field <= (S32)SLAreaSearchField::LastOwner ? field : 0);
    mSearchRegexCheck->set(state["regex"].asBoolean());
    for (size_t i = 0; i < mTriStateCombos.size(); ++i)
    {
        mTriStateCombos[i]->setValue((S32)mFilters.states[i]);
    }
    mPriceMinSpin->set((F32)mFilters.price_min);
    mPriceMaxSpin->set((F32)mFilters.price_max);
    mClickActionCombo->setCurrentByIndex(mFilters.click_action_filter);
    showMoreFilters(mMoreFiltersShown);
}

void FSAreaSearch::saveState() const
{
    LLSD state;
    state["scope"] = scope_key(mScope == EScope::Pinned ? mUnpinnedScope : mScope);
    state["radius"] = mNearbyRadius;
    state["field"] = mSearchFieldCombo ? mSearchFieldCombo->getCurrentIndex() : 0;
    state["regex"] = mSearchRegexCheck && mSearchRegexCheck->get();
    state["more_open"] = mMoreFiltersShown;
    state["filters"] = mFilters.asLLSD();
    state["dbl_touch"] = mDoubleClick.touch;
    state["dbl_buy"] = mDoubleClick.buy;
    state["dbl_sit"] = mDoubleClick.sit;
    gSavedSettings.setLLSD(STATE_SETTING, state);
}

bool FSAreaSearch::isInScope(LLViewerObject* objectp) const
{
    // A linkset stands where its root does, which is how the server counts it on a parcel.
    LLViewerObject* root = objectp->getRootEdit();
    if (!root)
    {
        root = objectp;
    }

    switch (mScope)
    {
    case EScope::Parcel:
        return LLViewerParcelMgr::instance().inAgentParcel(root->getPositionGlobal());
    case EScope::Pinned:
    {
        const LLVector3 pos = root->getPositionRegion();
        return root->getRegion() && root->getRegion()->getRegionID() == mPinnedRegionId && mPinnedCells.contains(pos.mV[VX], pos.mV[VY]);
    }
    case EScope::Region:
        return root->getRegion() && root->getRegion() == gAgent.getRegion();
    case EScope::Nearby:
    default:
        return calculateObjectDistance(mPanelList->getAgentLastPosition(), root) <= (F32)mNearbyRadius;
    }
}

void FSAreaSearch::setScope(EScope scope)
{
    if (scope != EScope::Pinned)
    {
        mUnpinnedScope = scope;
        unpinParcel();
    }
    // Only a new scope resets the sort; a radius change keeps the user's column choice.
    const bool changed = scope != mScope;
    mScope = scope;
    if (mScopeRadio)
    {
        mScopeRadio->setValue(scope_key(scope));
    }
    if (mNearbyRadiusSpin)
    {
        mNearbyRadiusSpin->setEnabled(scope == EScope::Nearby);
    }
    if (changed)
    {
        applyDefaultSort();
    }
    saveState();
    // Objects the new scope needs that were never fetched (neighbouring regions for
    // Nearby) are picked up by findObjects() like any newly seen object.
    relist();
    requestParcelInfo();
    updateParcelSummary(true);
}

void FSAreaSearch::applyDefaultSort()
{
    // On a parcel the question is what uses its land impact; elsewhere, what is near.
    if (LLScrollListCtrl* list = mPanelList ? mPanelList->getResultList() : nullptr)
    {
        if (mScope == EScope::Parcel || mScope == EScope::Pinned)
        {
            list->sortByColumn("land_impact", false);
        }
        else
        {
            list->sortByColumn("distance", true);
        }
    }
}

void FSAreaSearch::onScopeCommit()
{
    mRadiusPending = false;
    mNearbyRadius = llclamp((S32)mNearbyRadiusSpin->get(), 1, 4096);
    const EScope scope = scope_from_key(mScopeRadio->getValue().asString());
    setScope(scope == EScope::Pinned && mPinnedLocalId == INVALID_PARCEL_ID ? EScope::Parcel : scope);
}

void FSAreaSearch::onRadiusCommit()
{
    // A held arrow commits every step; re-list once the value settles, like typing a search.
    mRadiusPending = true;
    mRadiusTimer.reset();
}

void FSAreaSearch::applyRadius()
{
    mRadiusPending = false;
    const S32 radius = llclamp((S32)mNearbyRadiusSpin->get(), 1, 4096);
    if (radius == mNearbyRadius)
    {
        return;
    }
    mNearbyRadius = radius;
    saveState();
    relist();
    updateParcelSummary(true);
}

void FSAreaSearch::onSearchKeystroke()
{
    // Typing applies after a short pause (idle()), not on every key.
    mSearchPending = true;
    mSearchTypingTimer.reset();
}

void FSAreaSearch::applySearch()
{
    mSearchPending = false;
    // The editor also commits when it loses focus, e.g. on the first click into the list after
    // typing; re-listing then would reset the scroll and move the row under the click.
    mSearchEditor->resetDirty();
    const std::string text = mSearchEditor->getText();
    const S32 field = mSearchFieldCombo->getCurrentIndex();
    const bool regex = mSearchRegexCheck->get();
    if (text == mAppliedSearchText && field == mAppliedSearchField && regex == mAppliedSearchRegex)
    {
        return;
    }

    const bool valid = mSearch.set(text, (SLAreaSearchField)field, regex);
    // An invalid regex keeps the previous query; say so on the box instead of a popup per key.
    mSearchEditor->setTentative(!valid);
    mSearchEditor->setToolTip(valid ? LLStringUtil::null : getString("invalid_regex"));
    mAppliedSearchText = text;
    mAppliedSearchField = field;
    mAppliedSearchRegex = regex;
    saveState();
    if (valid)
    {
        relist();
    }
}

void FSAreaSearch::onTriStateCommit(SLAreaSearchProperty property, LLComboBox* combo)
{
    const S32 value = combo->getValue().asInteger();
    mFilters.set(property, (value >= (S32)SLTriState::Any && value <= (S32)SLTriState::Hide) ? (SLTriState)value : SLTriState::Any);
    saveState();
    // Child prims and attachments no longer hidden are fetched by findObjects() as they are seen.
    relist();
}

void FSAreaSearch::setOwnerFilter(const LLUUID& owner)
{
    mOwnerFilter = owner;
    mOwnerFilterIsGroup = false;
    for (const auto& [id, details] : mObjectDetails)
    {
        if (details.ownership_id == owner)
        {
            mOwnerFilterIsGroup = details.group_owned;
            break;
        }
    }
    mOwnerBar->setPickedOwner(owner);
    updateOwnerChip();
    relist();
}

void FSAreaSearch::updateOwnerChip()
{
    mOwnerChipPanel->setVisible(mOwnerFilter.notNull());
    if (mOwnerFilter.isNull())
    {
        return;
    }
    std::string name;
    bool requested = false;
    getNameFromUUID(mOwnerFilter, name, mOwnerFilterIsGroup, requested);
    LLStringUtil::format_map_t args;
    args["[NAME]"] = mOwnerFilterIsGroup ? name : RLVa_hideNameIfRestricted(name);
    mOwnerChipText->setText(getString("owner_chip", args));
}

void FSAreaSearch::requestParcelInfo()
{
    LLViewerRegion* region = gAgent.getRegion();
    S32 local_id = INVALID_PARCEL_ID;
    if (mScope == EScope::Pinned)
    {
        local_id = region && region->getRegionID() == mPinnedRegionId ? mPinnedLocalId : INVALID_PARCEL_ID;
    }
    else if (mScope == EScope::Parcel)
    {
        const LLParcel* parcel = LLViewerParcelMgr::instance().getAgentParcel();
        local_id = parcel ? parcel->getLocalID() : INVALID_PARCEL_ID;
    }
    if (!region || local_id == INVALID_PARCEL_ID)
    {
        return;
    }

    const LLUUID region_id = region->getRegionID();
    if (local_id != mParcelInfoLocalId || region_id != mParcelInfoRegionId)
    {
        // Another parcel: its numbers start unknown.
        mParcelInfo.clear();
        mParcelRootCount = -1;
        mParcelInfoRetries = 0;
        mParcelInfoLocalId = local_id;
        mParcelInfoRegionId = region_id;
    }
    mParcelCountChanged = false;
    mParcelInfoTimer.reset();

    LLHandle<FSAreaSearch> handle = getDerivedHandle<FSAreaSearch>();
    LLViewerParcelMgr::instance().requestParcelContents(region, local_id, [handle, local_id, region_id](const LLSD& info)
    {
        if (FSAreaSearch* self = handle.get())
        {
            self->onParcelInfo(info, local_id, region_id);
        }
    });
}

void FSAreaSearch::onParcelInfo(const LLSD& info, S32 local_id, const LLUUID& region_id)
{
    if (local_id != mParcelInfoLocalId || region_id != mParcelInfoRegionId)
    {
        return; // an earlier parcel's answer
    }
    // A failed lookup keeps the last numbers ("..." when there are none); it is retried a few
    // times, and the next change, Refresh or parcel crossing asks again.
    LLViewerRegion* region = gAgent.getRegion();
    if (info.isMap())
    {
        mParcelInfo = info;
        if (mScope == EScope::Pinned && local_id == mPinnedLocalId && region && region->getRegionID() == mPinnedRegionId)
        {
            // Until the cells arrive nothing is on the parcel; list it now.
            const bool first_cells = mPinnedCells.empty();
            const LLSD::Binary& bitmap = info["bitmap"].asBinary();
            mPinnedCells.setFromBitmap(bitmap, (S32)(region->getWidth() / PARCEL_GRID_STEP_METERS));
            if (first_cells && !mPinnedCells.empty())
            {
                relist();
            }
        }
    }
    updateParcelSummary(true);
}

void FSAreaSearch::pinParcel(S32 local_id)
{
    LLViewerRegion* region = gAgent.getRegion();
    if (!region || local_id == INVALID_PARCEL_ID)
    {
        return;
    }
    mPinnedLocalId = local_id;
    mPinnedRegionId = region->getRegionID();
    mPinnedCells = SLParcelCells();
    mScopeRadio->setIndexEnabled(mScopeRadio->getItemCount() - 1, true);
    mScopeRadio->getChild<LLCheckBoxCtrl>("pinned")->setLabel(getString("parcel_loading"));
    setScope(EScope::Pinned);
}

void FSAreaSearch::unpinParcel()
{
    if (mPinnedLocalId == INVALID_PARCEL_ID)
    {
        return;
    }
    mPinnedLocalId = INVALID_PARCEL_ID;
    mPinnedRegionId.setNull();
    mPinnedCells = SLParcelCells();
    mScopeRadio->getChild<LLCheckBoxCtrl>("pinned")->setLabel(getString("pinned_default"));
    mScopeRadio->setIndexEnabled(mScopeRadio->getItemCount() - 1, false);
}

void FSAreaSearch::refreshParcelNumbers()
{
    mRegionParcels->reset();
    requestParcelInfo();
}

void FSAreaSearch::updateParcelSummary(bool force)
{
    if (!force && mSummaryTimer.getElapsedTimeF32() < SUMMARY_INTERVAL)
    {
        return;
    }
    mSummaryTimer.reset();

    // Every cached root object in scope that counts against land: not attachments, not
    // temporary, whatever the search and filters show. A root counts as soon as it is seen; its
    // share also needs its properties (the owner) and its cost.
    const bool on_parcel = mScope == EScope::Parcel || mScope == EScope::Pinned;
    const SLRegionParcelMap& region_parcels = mRegionParcels->parcels();
    std::vector<SLParcelObject> objects;
    std::map<LLUUID, bool> owner_is_group;
    std::map<S32, S32> roots_by_parcel;
    S32 roots = 0;
    S32 loaded = 0;
    for (const auto& [id, details] : mObjectDetails)
    {
        LLViewerObject* objectp = gObjectList.findObject(id);
        if (!objectp || !objectp->isRoot() || objectp->isAttachment() || objectp->flagTemporaryOnRez() || !isInScope(objectp))
        {
            continue;
        }
        ++roots;
        if (mScope == EScope::Region)
        {
            const LLVector3 pos = objectp->getPositionRegion();
            if (const S32 local_id = region_parcels.parcelAt(region_parcels.cellAt(pos.mV[VX], pos.mV[VY])); local_id != INVALID_PARCEL_ID)
            {
                ++roots_by_parcel[local_id];
            }
        }
        if (details.request != FSObjectProperties::FINISHED)
        {
            continue;
        }
        if (const F32 cost = objectp->getLinksetCost(); cost > F_ALMOST_ZERO)
        {
            objects.push_back({ details.ownership_id, sl_land_impact(cost) });
            owner_is_group[details.ownership_id] = details.group_owned;
            loaded += objects.back().land_impact;
        }
    }

    // Objects appeared or vanished: the server's count moved too.
    if (on_parcel)
    {
        if (mParcelRootCount >= 0 && roots != mParcelRootCount)
        {
            mParcelCountChanged = true;
        }
        mParcelRootCount = roots;
        if (mParcelCountChanged && mParcelInfoTimer.getElapsedTimeF32() > PARCEL_INFO_INTERVAL)
        {
            requestParcelInfo();
        }
    }
    else if (mScope == EScope::Region)
    {
        mRegionParcels->noteRoots(roots_by_parcel, roots);
    }

    // What the server counts and allows: a parcel's numbers, the region's parcels added up, or
    // none for Nearby, where the bar is just what has loaded in range.
    LLViewerRegion* region = gAgent.getRegion();
    const S32 max_tasks = region ? (S32)region->getMaxTasks() : 0;
    bool known = false;
    S32 used = 0;
    S32 capacity = 0;
    if (on_parcel && mParcelInfo.isMap())
    {
        known = true;
        used = mParcelInfo["total_prims"].asInteger();
        capacity = sl_parcel_capacity(mParcelInfo["max_prims"].asInteger(), (F32)mParcelInfo["prim_bonus"].asReal(), max_tasks);
    }
    else if (mScope == EScope::Region && region_parcels.complete())
    {
        known = true;
        used = region_parcels.used();
        capacity = max_tasks;
    }
    const SLParcelSummary summary = sl_summarize_parcel(objects, used, capacity);

    std::vector<std::string> labels;
    for (const SLOwnerShare& share : summary.owners)
    {
        std::string name;
        bool requested = false;
        const bool group = owner_is_group[share.owner];
        getNameFromUUID(share.owner, name, group, requested);
        labels.push_back(group ? name : RLVa_hideNameIfRestricted(name));
    }
    mOwnerBar->setSummary(summary, labels);

    // The header: what is summed, then its land impact against what it allows.
    const bool hide_location = gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC);
    LLStringUtil::format_map_t args;
    std::string name_text = getString("parcel_loading");
    std::string used_text = getString("parcel_loading");
    std::string pool_text;
    switch (mScope)
    {
    case EScope::Parcel:
    case EScope::Pinned:
    {
        mParcelUsedText->setToolTip(getString("parcel_used_tip"));
        if (!known)
        {
            break;
        }
        const std::string parcel_name = hide_location ? RlvStrings::getString(RlvStringKeys::Hidden::Parcel)
                                      : mParcelInfo["name"].asString().empty() ? getString("unnamed_parcel") : mParcelInfo["name"].asString();
        args["[NAME]"] = parcel_name;
        args["[AREA]"] = llformat("%d", mParcelInfo["area"].asInteger());
        name_text = getString("parcel_name", args);
        if (mScope == EScope::Pinned)
        {
            mScopeRadio->getChild<LLCheckBoxCtrl>("pinned")->setLabel(parcel_name);
        }
        args["[USED]"] = llformat("%d", used);
        args["[CAPACITY]"] = llformat("%d", capacity);
        used_text = getString("parcel_used", args);

        // Parcels of one owner in a region can share one pool; show it when it differs.
        const S32 pool_used = mParcelInfo["sim_wide_total_prims"].asInteger();
        const S32 pool_max = max_tasks > 0 ? llmin(mParcelInfo["sim_wide_max_prims"].asInteger(), max_tasks) : mParcelInfo["sim_wide_max_prims"].asInteger();
        if (pool_max > 0 && (pool_used != used || pool_max != capacity))
        {
            args["[USED]"] = llformat("%d", pool_used);
            args["[MAX]"] = llformat("%d", pool_max);
            pool_text = getString("parcel_pool", args);
        }
        break;
    }
    case EScope::Region:
        mParcelUsedText->setToolTip(getString("region_used_tip"));
        args["[REGION]"] = hide_location ? RlvStrings::getString(RlvStringKeys::Hidden::Region) : region ? region->getName() : LLStringUtil::null;
        args["[PARCELS]"] = llformat("%d", (S32)region_parcels.size());
        name_text = getString(!known ? "region_name_loading" : region_parcels.size() == 1 ? "region_name_one" : "region_name", args);
        if (known)
        {
            args["[USED]"] = llformat("%d", used);
            args["[CAPACITY]"] = llformat("%d", capacity);
            used_text = getString("parcel_used", args);
        }
        break;
    case EScope::Nearby:
    default:
        mParcelUsedText->setToolTip(getString("nearby_used_tip"));
        args["[RADIUS]"] = llformat("%d", mNearbyRadius);
        name_text = getString("nearby_name", args);
        args["[USED]"] = llformat("%d", loaded);
        used_text = getString("nearby_used", args);
        break;
    }
    mParcelNameText->setText(name_text);
    mParcelUsedText->setText(used_text);
    mParcelPoolText->setVisible(!pool_text.empty());
    mParcelPoolText->setText(pool_text);
}

void FSAreaSearch::onMoreFiltersCommit()
{
    mFilters.price_min = (S32)mPriceMinSpin->get();
    mFilters.price_max = (S32)mPriceMaxSpin->get();
    mFilters.click_action_filter = (U8)llmax(0, mClickActionCombo->getCurrentIndex());
    saveState();
    relist();
}

void FSAreaSearch::showMoreFilters(bool show)
{
    mMoreFiltersShown = show;
    mMoreFiltersPanel->setVisible(show);
    mMoreFiltersButton->setToggleState(show);
    // The filter panel scrolls; give it the height of what is showing.
    const S32 height = mFiltersPanelCollapsedHeight + (show ? mMoreFiltersPanel->getRect().getHeight() : 0);
    mFiltersPanel->reshape(mFiltersPanel->getRect().getWidth(), height);
}

SLAreaSearchFacts FSAreaSearch::buildFacts(const FSObjectProperties& details, LLViewerObject* objectp) const
{
    SLAreaSearchFacts facts;
    auto set = [&facts](SLAreaSearchProperty property, bool has) { facts.has.set((size_t)property, has); };

    bool shared_media = false;
    for (U8 i = 0; i < objectp->getNumTEs() && !shared_media; ++i)
    {
        shared_media = objectp->getTE(i) && objectp->getTE(i)->hasMedia();
    }

    set(SLAreaSearchProperty::Temporary, objectp->flagTemporaryOnRez());
    set(SLAreaSearchProperty::Attachment, objectp->isAttachment());
    set(SLAreaSearchProperty::Physical, objectp->flagUsePhysics());
    set(SLAreaSearchProperty::Phantom, objectp->flagPhantom());
    set(SLAreaSearchProperty::Locked, !(details.owner_mask & PERM_MOVE));
    set(SLAreaSearchProperty::Scripted, objectp->flagScripted());
    set(SLAreaSearchProperty::SharedMedia, shared_media);
    set(SLAreaSearchProperty::ForSale, details.sale_info.isForSale());
    set(SLAreaSearchProperty::Copy, details.owner_mask & PERM_COPY);
    set(SLAreaSearchProperty::Modify, details.owner_mask & PERM_MODIFY);
    set(SLAreaSearchProperty::Transfer, details.owner_mask & PERM_TRANSFER);
    set(SLAreaSearchProperty::ReflectionProbe, objectp->mIsReflectionProbe);
    set(SLAreaSearchProperty::ChildPrim, !(objectp->isRoot() || (objectp->isAttachment() && objectp->isRootEdit())));

    facts.price = details.sale_info.getSalePrice();
    facts.handles_touch = objectp->flagHandleTouch();
    facts.click_action = objectp->getClickAction();
    return facts;
}


//---------------------------------------------------------------------------
// List panel
//---------------------------------------------------------------------------

FSPanelAreaSearchList::FSPanelAreaSearchList(FSAreaSearch* pointer)
:   LLPanel(),
    mFSAreaSearch(pointer)
{
    mColumnBits["distance"] = 1;
    mColumnBits["name"] = 2;
    mColumnBits["description"] = 4;
    mColumnBits["price"] = 8;
    mColumnBits["land_impact"] = 16;
    mColumnBits["prim_count"] = 32;
    mColumnBits["owner"] = 64;
    mColumnBits["group"] = 128;
    mColumnBits["creator"] = 256;
    mColumnBits["last_owner"] = 512;

    // The gear menu (menu_area_search_gear.xml) is built with this panel.
    mCommitCallbackRegistrar.add("AreaSearch.Gear.Toggle", boost::bind(&FSPanelAreaSearchList::onGearToggle, this, _2));
    mEnableCallbackRegistrar.add("AreaSearch.Gear.Check", boost::bind(&FSPanelAreaSearchList::onGearCheck, this, _2));
}

bool FSPanelAreaSearchList::postBuild()
{
    mResultList = getChild<LLScrollListCtrl>("result_list");
    mResultList->setDoubleClickCallback(boost::bind(&FSPanelAreaSearchList::onDoubleClick, this));
    mResultList->sortByColumn("name", true);
    // Stock LLScrollListCtrl::setContextMenu only takes a ContextMenuType
    // enum, not a custom menu pointer like FSScrollListCtrl did. Wire the
    // right-click via LLUICtrl::setRightMouseDownCallback instead, then
    // hand off to gFSAreaSearchMenu (LLListContextMenu::show).
    mResultList->setRightMouseDownCallback(boost::bind(&FSPanelAreaSearchList::onRightMouseDown, this, _2, _3, _4));

    mCounterText = getChild<LLTextBox>("counter");

    mRefreshButton = getChild<LLButton>("Refresh");
    mRefreshButton->setClickedCallback(boost::bind(&FSPanelAreaSearchList::onClickRefresh, this));

    mCheckboxBeacons = getChild<LLCheckBoxCtrl>("beacons");
    mCheckboxBeacons->setCommitCallback(boost::bind(&FSPanelAreaSearchList::onCommitCheckboxBeacons, this));

    mAgentLastPosition = gAgent.getPositionGlobal();

    updateResultListColumns();
    mFSAreaSearchColumnConfigConnection = gSavedSettings.getControl("FSAreaSearchColumnConfig")->getSignal()->connect(boost::bind(&FSPanelAreaSearchList::updateResultListColumns, this));

    return LLPanel::postBuild();
}

// virtual
FSPanelAreaSearchList::~FSPanelAreaSearchList()
{
    if (LLContextMenu* menu = mColumnMenuHandle.get())
    {
        menu->die();
    }
    if (mFSAreaSearchColumnConfigConnection.connected())
    {
        mFSAreaSearchColumnConfigConnection.disconnect();
    }
}

void FSPanelAreaSearchList::onClickRefresh()
{
    mFSAreaSearch->refreshList(true);
    mFSAreaSearch->refreshParcelNumbers();
}

// Handle keyboard shortcuts for the Area Search results list
// CTRL+C: Copy selected rows to clipboard in tab-separated format
// CTRL+A: Select all items in the list
// - Neremyn
bool FSPanelAreaSearchList::handleKeyHere(KEY key, MASK mask)
{
    if (key == 'C' && mask == MASK_CONTROL)
    {
        onCopyToClipboard();
        return true;
    }

    if (key == 'A' && mask == MASK_CONTROL)
    {
        if (mResultList)
        {
            mResultList->selectAll();
            return true;
        }
    }
	
    return LLPanel::handleKeyHere(key, mask);
}

// Copy selected rows from the Area Search results to the system clipboard
// Output format: tab-separated values with header row, suitable for Excel/Spreadsheets
// Column order: Distance, Name, Description, Price, Land Impact, Prim Count, Owner, Group, Creator, Last Owner
// - Neremyn
void FSPanelAreaSearchList::onCopyToClipboard()
{
    if (!mResultList)
    {
        LL_WARNS("FSAreaSearch") << "Result list control not found" << LL_ENDL;
        return;
    }

    // Get all selected items
    std::vector<LLScrollListItem*> selected_items = mResultList->getAllSelected();

    if (selected_items.empty())
    {
        return;
    }

    std::string clipboard_text;

    clipboard_text = "Distance\tName\tDescription\tPrice\tLand Impact\tPrim Count\tOwner\tGroup\tCreator\tLast Owner\n";

	for (const auto& item : selected_items)
	{
		if (!item)
			continue;

		auto getColumnValue = [&](S32 column_idx) {
			return item->getColumn(column_idx) ? item->getColumn(column_idx)->getValue().asString() : "";
		};

		const S32 column_count =  item->getNumColumns();
		for (S32 i = 0; i < column_count; ++i)
		{
			clipboard_text += getColumnValue(i);
			clipboard_text += (i < column_count - 1) ? "\t" : "\n";
		}
	}

    // Remove trailing newline
    if (!clipboard_text.empty() && clipboard_text.back() == '\n')
    {
        clipboard_text.pop_back();
    }

    // Copy to system clipboard
	LLWString wstr = utf8str_to_wstring(clipboard_text);
	LLClipboard::instance().copyToClipboard(wstr, 0, static_cast<S32>(wstr.length()));
}

void FSPanelAreaSearchList::onCommitCheckboxBeacons()
{
    mFSAreaSearch->setBeacons(mCheckboxBeacons->get());
}

void FSPanelAreaSearchList::setCounterText()
{
    mCounterText->setText(getString("counter_blank"));
}

void FSPanelAreaSearchList::setCounterText(LLStringUtil::format_map_t args)
{
    mCounterText->setText(getString("counter_format", args));
}

void FSPanelAreaSearchList::onDoubleClick()
{
    LLScrollListItem* item = mResultList->getFirstSelected();
    if (!item)
    {
        return;
    }

    const LLUUID& object_id = item->getUUID();
    if (LLViewerObject* objectp = gObjectList.findObject(object_id); objectp)
    {
        FSObjectProperties& details = mFSAreaSearch->mObjectDetails[object_id];
        LLTracker::trackLocation(objectp->getPositionGlobal(), details.name, "", LLTracker::LOCATION_ITEM);

        // Focus the camera on the picked object. This is the implicit
        // "where is it?" behavior expected on double-click; the optional
        // buy/touch/sit extras from the gear menu layer on top.
        gAgentCamera.setFocusOnAvatar(false, ANIMATE);
        gAgentCamera.setFocusGlobal(objectp->getPositionGlobal(), object_id);

        const FSAreaSearch::DoubleClickExtras& extras = mFSAreaSearch->getDoubleClickExtras();
        if (extras.buy)
        {
            buyObject(details, objectp);
        }

        if (extras.touch)
        {
            touchObject(objectp);
        }

        if (extras.sit)
        {
            sitOnObject(details, objectp);
        }
    }
}

bool FSPanelAreaSearchList::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    if (LLPanel::handleRightMouseDown(x, y, mask))
    {
        return true;
    }

    // The list only handles right-clicks on rows (LLScrollListCtrl::handleRightMouseDown), so a
    // click on its column headers falls through to here: which columns to show.
    S32 list_x = 0;
    S32 list_y = 0;
    localPointToOtherView(x, y, &list_x, &list_y, mResultList);
    if (mResultList->pointInView(list_x, list_y) && list_y > mResultList->getItemListRect().mTop)
    {
        showColumnMenu(list_x, list_y);
        return true;
    }
    return false;
}

void FSPanelAreaSearchList::onRightMouseDown(S32 x, S32 y, MASK /*mask*/)
{
    // The context menu actions operate on the list selection. Make the
    // right-clicked row the target: if it is already part of the selection
    // (single or multi), keep the selection as is; otherwise select just
    // that row, like standard list behaviour.
    if (LLScrollListItem* hit_item = mResultList->hitItem(x, y); hit_item && !hit_item->getSelected())
    {
        mResultList->selectItemAt(x, y, MASK_NONE);
    }

    uuid_vec_t ids;
    for (LLScrollListItem* item : mResultList->getAllSelected())
    {
        if (item)
        {
            ids.push_back(item->getUUID());
        }
    }
    if (ids.empty())
    {
        return;
    }
    gFSAreaSearchMenu.show(mResultList, ids, x, y);
}

void FSPanelAreaSearchList::updateScrollList()
{
    bool agent_moved = false;
    const LLVector3d current_agent_position = gAgent.getPositionGlobal();

    if (dist_vec(mAgentLastPosition, current_agent_position) > MIN_DISTANCE_MOVED)
    {
        agent_moved = true;
        mAgentLastPosition = current_agent_position;
    }

    LLScrollListColumn* distance_column = mResultList->getColumn("distance");
    LLViewerRegion* our_region = gAgent.getRegion();

    // Rows whose object has gone away or left the scope are removed together afterwards: a
    // parcel crossing can drop thousands at once.
    std::unordered_set<const LLScrollListItem*> gone;
    for (const LLScrollListItem* item : mResultList->getAllData())
    {
        const LLUUID& row_id = item->getUUID();
        LLViewerObject* objectp = gObjectList.findObject(row_id);

        if (!objectp || !mFSAreaSearch->isSearchableObject(objectp, our_region) || !mFSAreaSearch->isInScope(objectp))
        {
            gone.insert(item);
            mFSAreaSearch->mObjectDetails[row_id].listed = false;
            mFSAreaSearch->forgetPendingCost(row_id);
        }
        else if (agent_moved && distance_column)
        {
            item->getColumn(distance_column->mIndex)->setValue(LLSD(llformat("%1.0f m", calculateObjectDistance(current_agent_position, objectp))));
        }
    }
    const bool deleted = !gone.empty();
    if (deleted)
    {
        mResultList->deleteItemsIf([&gone](const LLScrollListItem* item) { return gone.contains(item); });
    }

    if (deleted || agent_moved)
    {
        mResultList->updateLayout();
    }
}

void FSPanelAreaSearchList::updateResultListColumns()
{
    // Rebuild the column set from the params declared in floater_fs_area_search.xml, giving
    // every column switched off in FSAreaSearchColumnConfig a width of -1 (LLScrollListCtrl
    // treats that as hidden: no header, cells skipped). Rebuilding from the declared params
    // rather than resizing in place also restores the declared widths, which updateColumns()
    // otherwise drifts by stretching whichever column is last visible to fill the list.
    if (!mResultList)
    {
        return;
    }

    const std::vector<LLScrollListColumn::Params> column_params = mResultList->getColumnInitParams();
    if (column_params.empty())
    {
        return;
    }

    const U32 column_config = gSavedSettings.getU32("FSAreaSearchColumnConfig");
    const std::string sort_column = mResultList->getSortColumnName();
    const bool sort_ascending = mResultList->getSortAscending();

    // The rows are kept: their cells are indexed by column position, and the columns are
    // re-added in the declared order, so only the cell widths need refreshing afterwards.
    mResultList->clearColumns();
    for (const LLScrollListColumn::Params& p : column_params)
    {
        LLScrollListColumn::Params params(p);
        auto bit = mColumnBits.find(p.name.getValue());
        if (bit != mColumnBits.end() && !(column_config & bit->second))
        {
            params.width.pixel_width.set(-1, true);
        }
        mResultList->addColumn(params);
    }

    // clearColumns() dropped the sort order; sorting reads cell data, so a hidden column
    // still sorts correctly.
    mResultList->sortByColumn(sort_column.empty() ? std::string("name") : sort_column, sort_ascending);

    // Push the new widths (including the -1 "hidden" width) down to the existing cells.
    mResultList->updateLayout();
    mResultList->updateColumns(true);
}

bool FSPanelAreaSearchList::isColumnVisible(std::string_view column) const
{
    auto bit = mColumnBits.find(column);
    if (bit == mColumnBits.end())
    {
        return false;
    }
    return (gSavedSettings.getU32("FSAreaSearchColumnConfig") & bit->second) != 0;
}

void FSPanelAreaSearchList::setColumnVisible(std::string_view column, bool visible)
{
    auto bit = mColumnBits.find(column);
    if (bit == mColumnBits.end())
    {
        LL_WARNS("FSAreaSearch") << "Unknown result list column: " << column << LL_ENDL;
        return;
    }

    const U32 column_config = gSavedSettings.getU32("FSAreaSearchColumnConfig");
    const U32 new_value = visible ? (column_config | bit->second) : (column_config & ~bit->second);
    if (new_value != column_config)
    {
        // The FSAreaSearchColumnConfig signal (connected in postBuild) rebuilds the columns.
        gSavedSettings.setU32("FSAreaSearchColumnConfig", new_value);
    }
}

void FSPanelAreaSearchList::showColumnMenu(S32 x, S32 y)
{
    if (LLContextMenu* old_menu = mColumnMenuHandle.get())
    {
        old_menu->die();
        mColumnMenuHandle.markDead();
    }

    LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
    registrar.add("AreaSearch.Column.Toggle", [this](LLUICtrl*, const LLSD& column)
        { setColumnVisible(column.asStringRef(), !isColumnVisible(column.asStringRef())); });
    LLUICtrl::EnableCallbackRegistry::ScopedRegistrar enable_registrar;
    enable_registrar.add("AreaSearch.Column.Visible", [this](LLUICtrl*, const LLSD& column)
        { return isColumnVisible(column.asStringRef()); });

    llassert(LLMenuGL::sMenuContainer != nullptr);
    LLContextMenu* menu = LLUICtrlFactory::getInstance()->createFromFile<LLContextMenu>(
        "menu_area_search_columns.xml", LLMenuGL::sMenuContainer, LLViewerMenuHolderGL::child_registry_t::instance());
    if (!menu)
    {
        return;
    }
    mColumnMenuHandle = menu->getHandle();
    menu->show(x, y);
    LLMenuGL::showPopup(mResultList, menu, x, y);
}

void FSPanelAreaSearchList::onGearToggle(const LLSD& userdata)
{
    FSAreaSearch::DoubleClickExtras& extras = mFSAreaSearch->getDoubleClickExtras();
    const std::string& action = userdata.asStringRef();
    if (action == "touch")
    {
        extras.touch = !extras.touch;
    }
    else if (action == "buy")
    {
        extras.buy = !extras.buy;
    }
    else if (action == "sit")
    {
        extras.sit = !extras.sit;
    }
    mFSAreaSearch->saveState();
}

bool FSPanelAreaSearchList::onGearCheck(const LLSD& userdata)
{
    const FSAreaSearch::DoubleClickExtras& extras = mFSAreaSearch->getDoubleClickExtras();
    const std::string& action = userdata.asStringRef();
    return (action == "touch" && extras.touch) || (action == "buy" && extras.buy) || (action == "sit" && extras.sit);
}

void FSPanelAreaSearchList::updateName(const LLUUID& id, const std::string& name)
{
    LLScrollListColumn* creator_column = mResultList->getColumn("creator");
    LLScrollListColumn* owner_column = mResultList->getColumn("owner");
    LLScrollListColumn* group_column = mResultList->getColumn("group");
    LLScrollListColumn* last_owner_column = mResultList->getColumn("last_owner");

    // Iterate over the rows in the list, updating the ones with matching id.
    std::vector<LLScrollListItem*> items = mResultList->getAllData();

    for (const auto* item : items)
    {
        const LLUUID& row_id = item->getUUID();
        FSObjectProperties& details = mFSAreaSearch->mObjectDetails[row_id];

        if (creator_column && (id == details.creator_id))
        {
            LLScrollListText* creator_text = (LLScrollListText*)item->getColumn(creator_column->mIndex);
            creator_text->setText(name);
            mResultList->setNeedsSort();
        }

        if (owner_column && (id == details.owner_id))
        {
            LLScrollListText* owner_text = (LLScrollListText*)item->getColumn(owner_column->mIndex);
            owner_text->setText(details.group_owned ? name : RLVa_hideNameIfRestricted(name));
            mResultList->setNeedsSort();
        }

        if (group_column && (id == details.group_id))
        {
            LLScrollListText* group_text = (LLScrollListText*)item->getColumn(group_column->mIndex);
            group_text->setText(name);
            mResultList->setNeedsSort();
        }

        if (last_owner_column && (id == details.last_owner_id))
        {
            LLScrollListText* last_owner_text = (LLScrollListText*)item->getColumn(last_owner_column->mIndex);
            last_owner_text->setText(RLVa_hideNameIfRestricted(name));
            mResultList->setNeedsSort();
        }
    }
}

bool FSPanelAreaSearchList::onContextMenuItemEnable(const LLSD& userdata)
{
    const std::string& parameter = userdata.asStringRef();
    if (parameter == "one")
    {
        // return true if just one item is selected.
        return (mResultList->getNumSelected() == 1);
    }
    else if (parameter == "in_dd")
    {
        // return true if the object is within the draw distance.
        if (mResultList->getNumSelected() == 1)
        {
            const LLUUID& object_id = mResultList->getFirstSelected()->getUUID();
            LLViewerObject* objectp = gObjectList.findObject(object_id);
            return (objectp && calculateObjectDistance(gAgent.getPositionGlobal(), objectp) < gAgentCamera.mDrawDistance);
        }
        else
        {
            return false;
        }
    }
    else if (parameter == "script")
    {
        // #425: needs the #Slipstream Helper; off (with the reason as tooltip, see
        // FSAreaSearchMenu::show) while it cannot run here.
        return mResultList->getNumSelected() > 0 && SLScriptInfo::instance().canRequest();
    }
    else
    {
        // return true if more then one is selected, but not just one.
        return (mResultList->getNumSelected() > 1);
    }
}

bool FSPanelAreaSearchList::onContextMenuItemVisibleRLV(const LLSD& userdata)
{
    if (!RlvActions::isRlvEnabled())
    {
        return true;
    }

    if (RlvActions::hasBehaviour(RLV_BHVR_INTERACT))
    {
        return false;
    }

    const std::string& parameter = userdata.asStringRef();
    std::vector<std::string> behavs;
    LLStringUtil::getTokens(parameter, behavs, "|");

    std::vector<LLScrollListItem*> selected = mResultList->getAllSelected();
    for (const auto* item : selected)
    {
        const LLUUID& object_id = item->getUUID();
        LLViewerObject* objectp = gObjectList.findObject(object_id);
        if (!objectp)
        {
            return false;
        }

        if (
            std::find(behavs.begin(), behavs.end(), "touch") != behavs.end()
            && !RlvActions::canTouch(objectp)
        )
        {
            return false;
        }
        else if (
            std::find(behavs.begin(), behavs.end(), "edit") != behavs.end()
            && !RlvActions::canEdit(objectp)
        )
        {
            return false;
        }
        else if (
            std::find(behavs.begin(), behavs.end(), "delete") != behavs.end()
            && !rlvCanDeleteOrReturn(objectp)
        )
        {
            return false;
        }
        else if (
            std::find(behavs.begin(), behavs.end(), "sit") != behavs.end()
            && !RlvActions::canSit(objectp)
        )
        {
            return false;
        }
        else if (
            std::find(behavs.begin(), behavs.end(), "buy") != behavs.end()
            && !RlvActions::canBuyObject(object_id)
        )
        {
            return false;
        }
        else if (
            std::find(behavs.begin(), behavs.end(), "teleport") != behavs.end()
            && !RlvActions::canTeleportToLocal(objectp->getPositionGlobal())
        )
        {
            return false;
        }
    }

    return true;
}

bool FSPanelAreaSearchList::onContextMenuItemClick(const LLSD& userdata)
{
    const std::string& action = userdata.asStringRef();
    LL_DEBUGS("FSAreaSearch") << "Right click menu " << action << " was selected." << LL_ENDL;

    if (action == "select_all")
    {
        std::vector<LLScrollListItem*> result_items = mResultList->getAllData();
        std::for_each(result_items.begin(), result_items.end(), [](LLScrollListItem* item) { item->setSelected(true); });
        return true;
    }
    if (action == "clear_selection")
    {
        std::vector<LLScrollListItem*> selected_items = mResultList->getAllSelected();
        std::for_each(selected_items.begin(), selected_items.end(), [](LLScrollListItem* item) { item->setSelected(false); });
        return true;
    }
    if (action == "filter_my_objects")
    {
        mFSAreaSearch->setOwnerFilter(gAgent.getID());
        return true;
    }

    if (action == "script")
    {
        // #425: one request for the whole selection; the helper answers in batches and
        // the results go to nearby chat.
        std::vector<std::pair<LLUUID, std::string>> objects;
        for (const auto* item : mResultList->getAllSelected())
        {
            const LLUUID& object_id = item->getUUID();
            auto it_details = mFSAreaSearch->mObjectDetails.find(object_id);
            const std::string name = (it_details != mFSAreaSearch->mObjectDetails.end() && !it_details->second.name.empty())
                                         ? it_details->second.name
                                         : object_id.asString();
            objects.emplace_back(object_id, name);
        }
        SLScriptInfo::instance().request(objects);
        return true;
    }

    // NOTE that each action command MUST begin with a different letter.
    char c = action.at(0);
    switch(c)
    {
    case 't': // touch
    case 'l': // blacklist (permanent derender)
    case 'h': // h_derender (this session only)
    {
        std::vector<LLScrollListItem*> selected = mResultList->getAllSelected();
        S32 cnt = 0;

        if ( ('l' == c) || ('h' == c) )
        {
            // #426: same rules as right-click > Derender (LLDerenderList::canAdd: never our own
            // attachments or avatar, never the object we sit on, linkset roots / other residents'
            // attachment roots only); "blacklist" persists across sessions like FS's.
            // Ids first: derendering kills objects, which can drop result rows.
            uuid_vec_t object_ids;
            for (const auto* item : selected)
            {
                object_ids.push_back(item->getUUID());
            }
            for (const LLUUID& object_id : object_ids)
            {
                LLViewerObject* objectp = gObjectList.findObject(object_id);
                if (objectp)
                {
                    objectp = objectp->getRootEdit();
                }
                if (!objectp || !LLDerenderList::canAdd(objectp))
                {
                    continue;
                }
                std::string name;
                auto it_details = mFSAreaSearch->mObjectDetails.find(object_id);
                if (it_details != mFSAreaSearch->mObjectDetails.end())
                {
                    name = it_details->second.name;
                }
                if (LLDerenderList::instance().addObject(objectp, 'l' == c, name))
                {
                    mFSAreaSearch->mObjectDetails.erase(object_id);
                }
            }
            break;
        }

        for (const auto* item : selected)
        {
            switch (c)
            {
            case 't': // touch
            {
                new FSAreaSearchTouchTimer(item->getUUID(), cnt * 0.2f);
                cnt++;
            }
                break;
            default:
                break;
            }
        }
    }
        break;

    case 'b': // buy
    case 'p': // p_teleport
    case 'u': // sit
    case 'q': // q_zoom
    {
        if (mResultList->getNumSelected() == 1)
        {
            const LLUUID& object_id = mResultList->getFirstSelected()->getUUID();
            LLViewerObject* objectp = gObjectList.findObject(object_id);
            if (objectp)
            {
                switch (c)
                {
                case 'b': // buy
                    buyObject(mFSAreaSearch->mObjectDetails[object_id], objectp);
                    break;
                case 'p': // p_teleport
                    gAgent.teleportViaLocation(objectp->getPositionGlobal());
                    break;
                case 'u': // sit
                    sitOnObject(mFSAreaSearch->mObjectDetails[object_id], objectp);
                    break;
                case 'q': // q_zoom
                {
                    // Disable flycam if active.  Without this, the requested look-at doesn't happen because the flycam code overrides all other camera motion.
                    // Ported from GunMetal-DX (squeedledorf, LGPL 2.1), v0.3.0 cf5f1f4: the flycam lives on LLAgentCamera
                    bool fly_cam_status(gAgentCamera.isUsingFlycam());
                    if (fly_cam_status)
                    {
                        gAgentCamera.toggleFlycam();
                        // FS-only SSFM_FLYCAM enum value not in upstream; SSFM_STAND
                        // is the closest fallback for clearing the post-flycam state.
                        LLPanelStandStopFlying::clearStandStopFlyingMode(LLPanelStandStopFlying::SSFM_STAND);
                        // *NOTE: Above may not be the proper way to disable flycam.  What I really want to do is just be able to move the camera and then leave the flycam in the the same state it was in, just moved to the new location. ~Cron
                    }

                    LLViewerJoystick::getInstance()->setCameraNeedsUpdate(true); // Fixes an edge case where if the user has JUST disabled flycam themselves, the camera gets stuck waiting for input.

                    gAgentCamera.setFocusOnAvatar(false, ANIMATE);

                    gAgentCamera.setLookAt(LOOKAT_TARGET_SELECT, objectp);

                    // Place the camera looking at the object, along the line from the camera to the object,
                    //  and sufficiently far enough away for the object to fill 3/4 of the screen,
                    //  but not so close that the bbox's nearest possible vertex goes inside the near clip.
                    // Logic C&P'd from LLViewerMediaFocus::setCameraZoom() and then edited as needed

                    LLBBox bbox = objectp->getBoundingBoxAgent();
                    LLVector3d center(gAgent.getPosGlobalFromAgent(bbox.getCenterAgent()));
                    F32 height;
                    F32 width;
                    F32 depth;
                    F32 angle_of_view;
                    F32 distance;

                    LLVector3d target_pos(center);
                    LLVector3d camera_dir(gAgentCamera.getCameraPositionGlobal() - target_pos);
                    camera_dir.normalize();

                    // We need the aspect ratio, and the 3 components of the bbox as height, width, and depth.
                    F32 aspect_ratio(LLViewerMediaFocus::getBBoxAspectRatio(bbox, LLVector3(camera_dir), &height, &width, &depth));
                    F32 camera_aspect(LLViewerCamera::getInstance()->getAspect());

                    // We will normally use the side of the volume aligned with the short side of the screen (i.e. the height for
                    // a screen in a landscape aspect ratio), however there is an edge case where the aspect ratio of the object is
                    // more extreme than the screen.  In this case we invert the logic, using the longer component of both the object
                    // and the screen.
                    bool invert((camera_aspect > 1.0f && aspect_ratio > camera_aspect) || (camera_aspect < 1.0f && aspect_ratio < camera_aspect));

                    // To calculate the optimum viewing distance we will need the angle of the shorter side of the view rectangle.
                    // In portrait mode this is the width, and in landscape it is the height.
                    // We then calculate the distance based on the corresponding side of the object bbox (width for portrait, height for landscape)
                    // We will add half the depth of the bounding box, as the distance projection uses the center point of the bbox.
                    if (camera_aspect < 1.0f || invert)
                    {
                        angle_of_view = llmax(0.1f, LLViewerCamera::getInstance()->getView() * LLViewerCamera::getInstance()->getAspect());
                        distance = width * 0.5f * 1.1f / tanf(angle_of_view * 0.5f);
                    }
                    else
                    {
                        angle_of_view = llmax(0.1f, LLViewerCamera::getInstance()->getView());
                        distance = height * 0.5f * 1.1f / tanf(angle_of_view * 0.5f);
                    }

                    distance += depth * 0.5f;

                    // Verify that the bounding box isn't inside the near clip.  Using OBB-plane intersection to check if the
                    // near-clip plane intersects with the bounding box, and if it does, adjust the distance such that the
                    // object doesn't clip.
                    LLVector3d bbox_extents(bbox.getExtentLocal());
                    LLVector3d axis_x = LLVector3d(1, 0, 0) * bbox.getRotation();
                    LLVector3d axis_y = LLVector3d(0, 1, 0) * bbox.getRotation();
                    LLVector3d axis_z = LLVector3d(0, 0, 1) * bbox.getRotation();
                    //Normal of nearclip plane is camera_dir.
                    F32 min_near_clip_dist = (F32)(bbox_extents.mdV[0] * (camera_dir * axis_x) + bbox_extents.mdV[1] * (camera_dir * axis_y) + bbox_extents.mdV[2] * (camera_dir * axis_z)); // http://www.gamasutra.com/view/feature/131790/simple_intersection_tests_for_games.php?page=7
                    F32 camera_to_near_clip_dist(LLViewerCamera::getInstance()->getNear());
                    F32 min_camera_dist(min_near_clip_dist + camera_to_near_clip_dist);
                    if (distance < min_camera_dist)
                    {
                        // Camera is too close to object, some parts MIGHT clip.  Move camera away to the position where clipping barely doesn't happen.
                        distance = min_camera_dist;
                    }

                    LLVector3d camera_pos(target_pos + camera_dir * distance);

                    if (camera_dir == LLVector3d::z_axis || camera_dir == LLVector3d::z_axis_neg)
                    {
                        // If the direction points directly up, the camera will "flip" around.
                        // We try to avoid this by adjusting the target camera position a
                        // smidge towards current camera position
                        // *NOTE: this solution is not perfect.  All it attempts to solve is the
                        // "looking down" problem where the camera flips around when it animates
                        // to that position.  You still are not guaranteed to be looking at the
                        // object in the correct orientation.  What this solution does is it will
                        // put the camera into position keeping as best it can the current
                        // orientation with respect to the direction wanted.  In other words, if
                        // before zoom the object appears "upside down" from the camera, after
                        /// zooming it will still be upside down, but at least it will not flip.
                        LLVector3d cur_camera_pos = LLVector3d(gAgentCamera.getCameraPositionGlobal());
                        LLVector3d delta = (cur_camera_pos - camera_pos);
                        F64 len = delta.length();
                        delta.normalize();
                        // Move 1% of the distance towards original camera location
                        camera_pos += 0.01 * len * delta;
                    }

                    gAgentCamera.setCameraPosAndFocusGlobal(camera_pos, target_pos, objectp->getID());

                    // *TODO: Re-enable joystick flycam if we disabled it earlier...  Have to find some form of callback as re-enabling at this point causes the camera motion to not happen. ~Cron
                    //if (fly_cam_status)
                    //{
                    //  LLViewerJoystick::getInstance()->toggleFlycam();
                    //}
                }
                break;
                default:
                    break;
                }
            }
        }
    }
        break;

    case 'i': // inspect
    case 'e': // edit
    case 'd': // delete
    case 'r': // return
    {
        // select the objects first
        LLSelectMgr::getInstance()->deselectAll();
        std::vector<LLScrollListItem*> selected = mResultList->getAllSelected();

        for (const auto* item : selected)
        {
            const LLUUID& object_id = item->getUUID();
            LLViewerObject* objectp = gObjectList.findObject(object_id);
            if (objectp)
            {
                LLSelectMgr::getInstance()->selectObjectAndFamily(objectp);
                if (c == 'r')
                {
                    // need to set permissions for object return
                    LLSelectNode* node = LLSelectMgr::getInstance()->getSelection()->findNode(objectp);
                    if (!node)
                    {
                        break;
                    }

                    if (!mFSAreaSearch || mFSAreaSearch->mObjectDetails.end() == mFSAreaSearch->mObjectDetails.find(object_id))
                    {
                        break;
                    }

                    FSObjectProperties& details = mFSAreaSearch->mObjectDetails[object_id];
                    node->mValid = true;
                    node->mPermissions->init(details.creator_id, details.owner_id, details.last_owner_id, details.group_id);
                    node->mPermissions->initMasks(details.base_mask, details.owner_mask, details.everyone_mask, details.group_mask, details.next_owner_mask);
                    node->mAggregatePerm = details.ag_perms;
                }
            }
        }

        // now act on those selected objects
        switch (c)
        {
        case 'i': // inspect
            LLFloaterReg::showInstance("inspect");
            break;
        case 'e': // edit
            handle_object_edit();
            break;
        case 'd': // delete
            handle_object_delete();
            break;
        case 'r': // return
            if (RlvActions::isRlvEnabled() && !rlvCanDeleteOrReturn())
            {
                break;
            }
            handle_object_return();
            break;
        default:
            break;
        }
    }
        break;
    default:
        break;
    }

    return true;
}

// static
void FSPanelAreaSearchList::touchObject(LLViewerObject* objectp)
{
    // *NOTE: Hope the packets arrive safely and in order or else
    // there will be some problems.
    if ( (!RlvActions::isRlvEnabled()) || (RlvActions::canTouch(objectp)) )
    {
        LLPickInfo pick; // default constructor will set sane values.
        send_ObjectGrab_message(objectp, pick, LLVector3::zero);
        send_ObjectDeGrab_message(objectp, pick);
    }
}

void FSPanelAreaSearchList::buyObject(FSObjectProperties& details, LLViewerObject* objectp)
{
    LLSelectMgr::getInstance()->deselectAll();
    LLSelectMgr::getInstance()->selectObjectAndFamily(objectp);

    if (LLSelectNode* node = LLSelectMgr::getInstance()->getSelection()->findNode(objectp))
    {
        node->mValid = true;
        node->mPermissions->init(details.creator_id, details.owner_id, details.last_owner_id, details.group_id);
        node->mPermissions->initMasks(details.base_mask, details.owner_mask, details.everyone_mask, details.group_mask, details.next_owner_mask);
        node->mSaleInfo = details.sale_info;
        node->mAggregatePerm = details.ag_perms;
        node->mCategory = details.category;
        node->mName.assign(details.name);
        node->mDescription.assign(details.description);

        handle_buy();
    }
    else
    {
        LL_WARNS("FSAreaSearch") << "No LLSelectNode node" << LL_ENDL;
    }
}

void FSPanelAreaSearchList::sitOnObject(FSObjectProperties& details, LLViewerObject* objectp)
{
    if ( (!RlvActions::isRlvEnabled()) || (RlvActions::canSit(objectp)) )
    {
        LLSelectMgr::getInstance()->deselectAll();
        LLSelectMgr::getInstance()->selectObjectAndFamily(objectp);
        LLSelectNode* node = LLSelectMgr::getInstance()->getSelection()->findNode(objectp);
        if (node)
        {
            gMessageSystem->newMessageFast(_PREHASH_AgentRequestSit);
            gMessageSystem->nextBlockFast(_PREHASH_AgentData);
            gMessageSystem->addUUIDFast(_PREHASH_AgentID, gAgentID);
            gMessageSystem->addUUIDFast(_PREHASH_SessionID, gAgentSessionID);
            gMessageSystem->nextBlockFast(_PREHASH_TargetObject);
            gMessageSystem->addUUIDFast(_PREHASH_TargetID, objectp->mID);
            gMessageSystem->addVector3Fast(_PREHASH_Offset, LLVector3::zero);
            objectp->getRegion()->sendReliableMessage();
        }
        else
        {
            LL_WARNS("FSAreaSearch") << "No LLSelectNode node" << LL_ENDL;
        }
    }
}
