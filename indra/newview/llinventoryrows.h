// #703 The inventory as a flat list of visible rows, UI-independent: a folder tree (open folders
// expanded, folders first, by name) or, with a filter, the items whose name contains it. Any view
// virtualises over it (the new interface materialises only the rows on screen from a fixed pool).
// Rebuilt only when the inventory or the open folders / filter change; bounded (kMaxRows).
#pragma once

#include "llinventoryobserver.h"
#include "lluuid.h"
#include <set>
#include <string>
#include <vector>

class LLInventoryRows : public LLInventoryObserver
{
public:
    struct Row
    {
        LLUUID      id;
        std::string name;
        std::string icon;  // skin image name (LLInventoryIcon)
        int         depth  = 0;
        bool        folder = false, open = false, worn = false;
    };
    static constexpr size_t kMaxRows = 20000;

    LLInventoryRows();
    ~LLInventoryRows() override;
    void changed(U32 mask) override { mDirty = true; }

    // Rebuilds when needed; true when the rows changed.
    bool refresh();
    const std::vector<Row>& rows() const { return mRows; }

    void toggleFolder(const LLUUID& id);
    void setFilter(const std::string& filter);
    const std::string& filter() const { return mFilter; }
    // Opens / wears / rezzes it as the inventory does on a double click.
    static void activate(const LLUUID& id);

private:
    void addFolder(const LLUUID& id, int depth);
    void addMatches(const LLUUID& id, const std::string& folded, int depth);

    std::vector<Row> mRows;
    std::set<LLUUID> mOpen;
    std::string      mFilter;
    bool             mDirty = true;
};
