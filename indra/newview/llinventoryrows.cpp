// #703 The inventory as a flat list of visible rows (see llinventoryrows.h).
#include "llviewerprecompiledheaders.h"
#include "llinventoryrows.h"

#include "llappearancemgr.h"
#include "llinventorybridge.h"
#include "llinventoryfunctions.h"
#include "llinventoryicon.h"
#include "llinventorymodel.h"
#include "llviewerinventory.h"

#include <algorithm>
#include <cctype>
#include <new>

namespace
{
std::string fold(const std::string& s)
{
    std::string out = s;
    for (char& c : out)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Folders first, then by name (case-insensitive), like the classic inventory's default sort.
template <class T> void sortByName(std::vector<T*>& v)
{
    std::sort(v.begin(), v.end(), [](const T* a, const T* b) { return LLStringUtil::compareDict(a->getName(), b->getName()) < 0; });
}
} // namespace

LLInventoryRows::LLInventoryRows()
{
    gInventory.addObserver(this);
}

LLInventoryRows::~LLInventoryRows()
{
    if (gInventory.containsObserver(this))
        gInventory.removeObserver(this);
}

void LLInventoryRows::toggleFolder(const LLUUID& id)
{
    if (!mOpen.erase(id))
    {
        mOpen.insert(id);
        gInventory.fetchDescendentsOf(id); // opening a folder loads it, as in the classic tree
    }
    mDirty = true;
}

void LLInventoryRows::setFilter(const std::string& filter)
{
    if (filter == mFilter)
        return;
    mFilter = filter;
    mDirty  = true;
}

void LLInventoryRows::addFolder(const LLUUID& id, int depth)
{
    LLInventoryModel::cat_array_t*  cats  = nullptr;
    LLInventoryModel::item_array_t* items = nullptr;
    gInventory.getDirectDescendentsOf(id, cats, items);
    std::vector<LLViewerInventoryCategory*> folders;
    std::vector<LLViewerInventoryItem*>     things;
    if (cats)
        for (const auto& c : *cats)
            folders.push_back(c.get());
    if (items)
        for (const auto& i : *items)
            things.push_back(i.get());
    sortByName(folders);
    sortByName(things);
    for (LLViewerInventoryCategory* cat : folders)
    {
        if (mRows.size() >= kMaxRows)
            return;
        Row r;
        r.id     = cat->getUUID();
        r.name   = cat->getName();
        r.icon   = LLInventoryIcon::getIconName(LLAssetType::AT_CATEGORY);
        r.depth  = depth;
        r.folder = true;
        r.open   = mOpen.count(r.id) != 0;
        mRows.push_back(std::move(r));
        if (mRows.back().open)
            addFolder(cat->getUUID(), depth + 1);
    }
    for (LLViewerInventoryItem* item : things)
    {
        if (mRows.size() >= kMaxRows)
            return;
        Row r;
        r.id    = item->getUUID();
        r.name  = item->getName();
        r.icon  = LLInventoryIcon::getIconName(item->getType(), item->getInventoryType(), item->getFlags());
        r.depth = depth;
        r.worn  = get_is_item_worn(item);
        mRows.push_back(std::move(r));
    }
}

void LLInventoryRows::addMatches(const LLUUID& id, const std::string& folded, int depth)
{
    if (depth > 64)
        return;
    LLInventoryModel::cat_array_t*  cats  = nullptr;
    LLInventoryModel::item_array_t* items = nullptr;
    gInventory.getDirectDescendentsOf(id, cats, items);
    if (items)
        for (const auto& item : *items)
        {
            if (mRows.size() >= kMaxRows / 10) // a search shows at most 2000 matches
                return;
            if (fold(item->getName()).find(folded) == std::string::npos)
                continue;
            Row r;
            r.id   = item->getUUID();
            r.name = item->getName();
            r.icon = LLInventoryIcon::getIconName(item->getType(), item->getInventoryType(), item->getFlags());
            r.worn = get_is_item_worn(item.get());
            mRows.push_back(std::move(r));
        }
    if (cats)
        for (const auto& cat : *cats)
            addMatches(cat->getUUID(), folded, depth + 1);
}

bool LLInventoryRows::refresh()
{
    if (!mDirty)
        return false;
    mDirty = false;
    try
    {
        mRows.clear();
        const LLUUID& root = gInventory.getRootFolderID();
        if (root.isNull())
            return true;
        if (mFilter.empty())
            addFolder(root, 0);
        else
            addMatches(root, fold(mFilter), 0);
    }
    catch (const std::bad_alloc&)
    {
        mRows.clear(); // retried on the next change
        mDirty = true;
    }
    return true;
}

void LLInventoryRows::activate(const LLUUID& id)
{
    if (gInventory.getCategory(id))
        return; // folders open in place (toggleFolder)
    if (get_can_item_be_worn(id))
    {
        LLAppearanceMgr::instance().wearItemOnAvatar(id, true, false);
        return;
    }
    LLInvFVBridgeAction::doAction(id, &gInventory); // notecards, textures, scripts, landmarks ...
}
