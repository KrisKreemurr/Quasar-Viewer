/**
 * @file fsareasearch.h
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

#ifndef FS_AREASEARCH_H
#define FS_AREASEARCH_H

#include "llcategory.h"
#include "llfloater.h"
#include "llframetimer.h"
#include "llpermissions.h"
#include "llsaleinfo.h"
#include "llscrolllistcolumn.h"
#include "llviewerobject.h"
#include "rlvdefines.h"
#include "slareasearchfilter.h"
#include "slparcelsummary.h"

class LLAvatarName;
class LLTextBox;
class LLViewerRegion;
class LLCheckBoxCtrl;
class LLLineEditor;
class LLContextMenu;
class LLSpinCtrl;
class LLComboBox;
class LLButton;
class LLRadioGroup;
class LLPanel;

class FSPanelAreaSearchList;
class LLScrollListCtrl;
class SLParcelOwnerBar;
class SLRegionParcelTracker;

struct FSObjectProperties
{
    LLUUID id;
    bool listed{ false };
    std::string name;
    std::string description;
    std::string touch_name;
    std::string sit_name;
    LLUUID creator_id;
    LLUUID owner_id;
    LLUUID group_id;
    LLUUID ownership_id;
    bool group_owned{ false };
    U64 creation_date{ 0 };
    U32 base_mask{ 0 };
    U32 owner_mask{ 0 };
    U32 group_mask{ 0 };
    U32 everyone_mask{ 0 };
    U32 next_owner_mask{ 0 };
    LLSaleInfo sale_info;
    LLCategory category;
    LLUUID last_owner_id;
    LLAggregatePermissions ag_perms;
    LLAggregatePermissions ag_texture_perms;
    LLAggregatePermissions ag_texture_perms_owner;
    LLPermissions permissions;
    uuid_vec_t texture_ids;
    bool name_requested{ false };
    U32 local_id{ 0 };
    U64 region_handle{ 0 };

    typedef enum e_object_properties_request
    {
        NEED,
        SENT,
        FINISHED,
        FAILED
    } EObjectPropertiesRequest;
    EObjectPropertiesRequest request{ NEED };
};


//---------------------------------------------------------------------
// Main class for area search
// holds the search engine and main floater: the filter panel on the
// left, the list panel on the right (#1321).
// --------------------------------------------------------------------
class FSAreaSearch : public LLFloater
{
    LOG_CLASS(FSAreaSearch);
public:
    // Where to look: the agent's parcel, the agent's region, within a radius of the agent
    // (neighbouring regions included), or a parcel picked in About Land (pinned).
    enum class EScope : U8 { Parcel, Region, Nearby, Pinned };

    // What a double-click does besides camming to the object and tracking it.
    struct DoubleClickExtras
    {
        bool touch{ false };
        bool buy{ false };
        bool sit{ false };
    };

    FSAreaSearch(const LLSD &);
    virtual ~FSAreaSearch();

    bool postBuild() override;
    void draw() override;
    // Queue this frame's result beacons (called every frame from the world-overlay
    // pass, NOT from draw(): retained UI replays idle floaters without calling draw()).
    void addBeacons();
    void onOpen(const LLSD& key) override;

    void avatarNameCacheCallback(const LLUUID& id, const LLAvatarName& av_name);
    void callbackLoadFullName(const LLUUID& id, const std::string& full_name);
    void processObjectProperties(LLMessageSystem* msg);
    void updateObjectCosts(const LLUUID& object_id, F32 object_cost, F32 link_cost, F32 physics_cost, F32 link_physics_cost);
    static void idle(void *user_data);

    void checkRegion();
    void refreshList(bool cache_clear);
    // Re-list from the cached object details after a filter, search or scope change.
    void relist() { refreshList(false); }
    bool isSearchableObject(LLViewerObject* objectp, LLViewerRegion* our_region) const;
    bool isInScope(LLViewerObject* objectp) const;

    void setScope(EScope scope);
    EScope getScope() const { return mScope; }
    const SLAreaSearchFilters& getFilters() const { return mFilters; }

    DoubleClickExtras& getDoubleClickExtras() { return mDoubleClick; }
    // List only one owner's objects (the group for group-owned ones); null lists everyone's.
    // Set from the parcel summary's owner bar or "Filter My Objects".
    void setOwnerFilter(const LLUUID& owner);
    // Ask the server for the parcel's name, counts and capacity again (Parcel or Pinned scope).
    void requestParcelInfo();
    // Refresh: ask again for every number the summary shows, the region's parcels included.
    void refreshParcelNumbers();
    // List one parcel of the agent's region (About Land > Objects > List objects). It stays
    // put while the agent walks, until another scope is picked or the agent leaves the region.
    void pinParcel(S32 local_id);
    // Persist the window's state (scope, search, filters, double-click extras).
    void saveState() const;

    std::map<LLUUID, FSObjectProperties> mObjectDetails;

    // Listed rows whose Land Impact is still unknown ("..."), with the number of
    // cost re-requests made so far. Bounded (MAX_PENDING_COSTS); entries leave when
    // the cost arrives, the row goes away, or the retries run out.
    std::map<LLUUID, U8> mPendingCosts;
    void forgetPendingCost(const LLUUID& id) { mPendingCosts.erase(id); }

    FSPanelAreaSearchList* getPanelList() { return mPanelList; }

    void setBeacons(bool b) { mBeacons = b; }

    bool isActive() const { return mActive; }

private:
    void requestObjectProperties(const std::vector<U32>& request_list, bool select, LLViewerRegion* regionp);
    void matchObject(FSObjectProperties& details, LLViewerObject* objectp);
    SLAreaSearchFacts buildFacts(const FSObjectProperties& details, LLViewerObject* objectp) const;
    void getNameFromUUID(const LLUUID& id, std::string& name, bool group, bool& name_requested);

    void updateCounterText();
    void findObjects();
    void processRequestQueue();
    void trackPendingCost(const LLUUID& id);
    void retryPendingCosts();

    // The filter panel.
    void loadState();
    void onScopeCommit();
    void onRadiusCommit();
    void applyRadius();
    void onSearchKeystroke();
    void applySearch();
    void onTriStateCommit(SLAreaSearchProperty property, LLComboBox* combo);
    void onMoreFiltersCommit();
    void showMoreFilters(bool show);
    void showMoreFields(bool show);
    void resizeFiltersPanel();
    void applyDefaultSort();

    // The parcel summary above the list (#1321).
    void onParcelInfo(const LLSD& info, S32 local_id, const LLUUID& region_id);
    void updateParcelSummary(bool force = false);
    void updateOwnerChip();
    void unpinParcel();

    boost::signals2::connection mRlvBehaviorCallbackConnection{};
    void updateRlvRestrictions(ERlvBehaviour behavior);

    S32 mRequested{ 0 };
    bool mRefresh{ false };
    S32 mSearchableObjects{ 0 };
    bool mActive{ false };
    bool mRequestQueuePause{ false };
    bool mRequestNeedsSent{ false };
    std::map<U64,S32> mRegionRequests;

    SLAreaSearchFilters mFilters{ SLAreaSearchFilters::defaults() };
    SLAreaSearchText mSearch;
    EScope mScope{ EScope::Parcel };
    S32 mNearbyRadius{ 64 };
    bool mMoreFiltersShown{ false };
    bool mMoreFieldsShown{ false };   // the per-field search boxes (#1404)
    bool mMoreFieldsApplied{ false }; // what the filter panel layout currently shows
    DoubleClickExtras mDoubleClick;
    bool mSearchPending{ false }; // typed text waiting for the keystroke pause
    LLFrameTimer mSearchTypingTimer;
    bool mRadiusPending{ false }; // a radius change waiting for the spinner to settle
    LLFrameTimer mRadiusTimer;
    // The search last applied, so a commit without a change does not re-list.
    std::string mAppliedSearchText;
    S32 mAppliedSearchField{ 0 };
    bool mAppliedSearchRegex{ false };
    SLAreaSearchFieldQueries mAppliedFieldQueries;

    LLUUID mOwnerFilter;
    bool mOwnerFilterIsGroup{ false };

    // The parcel the summary describes, and the server's answer for it (undefined until one).
    LLSD mParcelInfo;

    // The pinned parcel: its region, local id and cells (empty until its bitmap arrives), and
    // the scope saved meanwhile (a pin is not remembered across sessions).
    S32 mPinnedLocalId{ -1 };
    LLUUID mPinnedRegionId;
    SLParcelCells mPinnedCells;
    EScope mUnpinnedScope{ EScope::Parcel };

    S32 mParcelInfoLocalId{ -1 };
    LLUUID mParcelInfoRegionId;
    LLFrameTimer mParcelInfoTimer;   // since the last lookup
    S32 mParcelInfoRetries{ 0 };     // lookups retried while the numbers are still unknown
    S32 mParcelRootCount{ -1 };      // root objects seen on the parcel at the last summary
    bool mParcelCountChanged{ false };
    LLFrameTimer mSummaryTimer;

    // The region's parcels and their counts, for the Region scope's summary.
    std::unique_ptr<SLRegionParcelTracker> mRegionParcels;

    LLFrameTimer mLastUpdateTimer;
    LLFrameTimer mLastPropertiesReceivedTimer;
    LLFrameTimer mCostRetryTimer;


    using name_cache_connection_map_t = std::map<LLUUID, boost::signals2::connection>;
    name_cache_connection_map_t mNameCacheConnections;

    LLViewerRegion* mLastRegion{ nullptr };

    class FSParcelChangeObserver;
    friend class FSParcelChangeObserver;
    std::unique_ptr<FSParcelChangeObserver> mParcelChangedObserver;

    FSPanelAreaSearchList* mPanelList{ nullptr };

    LLRadioGroup* mScopeRadio{ nullptr };
    LLSpinCtrl* mNearbyRadiusSpin{ nullptr };
    LLLineEditor* mSearchEditor{ nullptr };
    LLComboBox* mSearchFieldCombo{ nullptr };
    LLCheckBoxCtrl* mSearchRegexCheck{ nullptr };
    std::array<LLLineEditor*, 6> mFieldEditors{};
    LLButton* mMoreFieldsButton{ nullptr };
    LLPanel* mMoreFieldsPanel{ nullptr };
    std::array<LLComboBox*, (size_t)SLAreaSearchProperty::Count> mTriStateCombos{};
    LLSpinCtrl* mPriceMinSpin{ nullptr };
    LLSpinCtrl* mPriceMaxSpin{ nullptr };
    LLComboBox* mClickActionCombo{ nullptr };
    LLButton* mMoreFiltersButton{ nullptr };
    LLPanel* mMoreFiltersPanel{ nullptr };
    LLPanel* mFiltersPanel{ nullptr };
    S32 mFiltersPanelCollapsedHeight{ 0 };

    LLTextBox* mParcelNameText{ nullptr };
    LLTextBox* mParcelUsedText{ nullptr };
    LLTextBox* mParcelPoolText{ nullptr };
    SLParcelOwnerBar* mOwnerBar{ nullptr };
    LLView* mOwnerChipPanel{ nullptr };
    LLTextBox* mOwnerChipText{ nullptr };

    bool mBeacons{ false };

protected:
    static void* createPanelList(void* data);
};


//------------------------------------------------------------
// List panel
// displays the found objects, with the counter, beacons,
// refresh and the gear menu below
//------------------------------------------------------------
class FSPanelAreaSearchList
:   public LLPanel
{
    LOG_CLASS(FSPanelAreaSearchList);
    friend class FSAreaSearchMenu;

public:
    FSPanelAreaSearchList(FSAreaSearch* pointer);
    virtual ~FSPanelAreaSearchList();

    bool postBuild() override;

    void setCounterText();
    void setCounterText(LLStringUtil::format_map_t args);
    void updateScrollList();
    void updateName(const LLUUID& id, const std::string& name);
    static void touchObject(LLViewerObject* objectp);

    LLScrollListCtrl* getResultList() { return mResultList; }
    void updateResultListColumns();

    void setAgentLastPosition(LLVector3d d) { mAgentLastPosition = d; }
    LLVector3d getAgentLastPosition() { return mAgentLastPosition; }

private:
    void onDoubleClick();
    void onRightMouseDown(S32 x, S32 y, MASK mask);
    void showColumnMenu(S32 x, S32 y);
    void onClickRefresh();
    void buyObject(FSObjectProperties& details, LLViewerObject* objectp);
    void sitOnObject(FSObjectProperties& details, LLViewerObject* objectp);
    void onCommitCheckboxBeacons();
	
	void onCopyToClipboard();
	bool handleKeyHere(KEY key, MASK mask) override;
    bool handleRightMouseDown(S32 x, S32 y, MASK mask) override;

    bool onContextMenuItemClick(const LLSD& userdata);
    bool onContextMenuItemEnable(const LLSD& userdata);
    bool onContextMenuItemVisibleRLV(const LLSD& userdata);

    bool isColumnVisible(std::string_view column) const;
    void setColumnVisible(std::string_view column, bool visible);

    // The gear menu: what a double-click does besides camming to the object.
    void onGearToggle(const LLSD& userdata);
    bool onGearCheck(const LLSD& userdata);

    LLVector3d mAgentLastPosition;

    FSAreaSearch* mFSAreaSearch{ nullptr };
    LLButton* mRefreshButton{ nullptr };
    LLScrollListCtrl* mResultList{ nullptr };
    LLCheckBoxCtrl* mCheckboxBeacons{ nullptr };
    LLTextBox* mCounterText{ nullptr };
    LLHandle<LLContextMenu> mColumnMenuHandle;

    std::map<std::string, U32, std::less<>> mColumnBits;
    boost::signals2::connection mFSAreaSearchColumnConfigConnection{};
};

#endif // FS_AREASEARCH_H
