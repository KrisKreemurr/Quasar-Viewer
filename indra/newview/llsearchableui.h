/**
* @file llsearchableui.h
*
* $LicenseInfo:firstyear=2019&license=viewerlgpl$
* Second Life Viewer Source Code
* Copyright (C) 2019, Linden Research, Inc.
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

#ifndef LL_SEARCHABLE_UI_H
#define LL_SEARCHABLE_UI_H

class LLMenuItemGL;
class LLView;
class LLPanel;
class LLTabContainer;
class LLAccordionCtrlTab;
class LLScrollListCtrl;


#include "llsearchablecontrol.h"

namespace ll
{
    namespace prefs
    {
        struct SearchableItem;
        struct PanelData;
        struct TabContainerData;

        typedef std::shared_ptr< SearchableItem > SearchableItemPtr;
        typedef std::shared_ptr< PanelData > PanelDataPtr;
        typedef std::shared_ptr< TabContainerData > TabContainerDataPtr;

        typedef std::vector< TabContainerData > tTabContainerDataList;
        typedef std::vector< SearchableItemPtr > tSearchableItemList;
        typedef std::vector< PanelDataPtr > tPanelDataList;

        struct SearchableItem
        {
            LLWString mLabel;        // what the control shows and its tool tip, one per line, lower case
            LLWString mSettingName;  // the setting it changes, lower case (#1348)
            LLView const *mView;
            ll::ui::SearchableControl const *mCtrl = nullptr;
            // #1348: a list instead of a control: its rows are matched one by one, and the
            // matching ones lit
            LLScrollListCtrl *mList = nullptr;
            bool mListMatched = false;

            std::vector< std::shared_ptr< SearchableItem >  > mChildren;

            virtual ~SearchableItem();

            bool isHighlighted() const;
            void setNotHighlighted();
            virtual bool hightlightAndHide( LLWString const &aFilter );
        };

        struct PanelData
        {
            LLPanel const *mPanel;
            std::string mLabel;
            LLWString mTitle; // mLabel in lower case: a page's name, as its tab shows it (#1348)

            std::vector< std::shared_ptr< SearchableItem > > mChildren;
            std::vector< std::shared_ptr< PanelData > > mChildPanel;

            virtual ~PanelData();

            void setNotHighlighted();
            // Highlights the matches and hides the tabs that have none; true when something here
            // can be shown. show_all: this is in a page whose name matched, shown whole (#1348).
            virtual bool hightlightAndHide( LLWString const &aFilter, bool show_all );
        };

        struct TabContainerData: public PanelData
        {
            LLTabContainer *mTabContainer;
            bool hightlightAndHide( LLWString const &aFilter, bool show_all ) override;
        };

        struct SearchData
        {
            TabContainerDataPtr mRootTab;
            LLWString mLastFilter;
            tSearchableItemList mItems; // every item, in child-list order (not layout order) (#1348)
            std::vector< LLAccordionCtrlTab* > mMarked; // the section headings the last search marked

            // Gathers what the search looks at: the pages of root and everything on them
            void collect( LLTabContainer* root );

            // Highlights the matches (the matching rows of lists too) and hides the pages without
            // any. With a filter, also opens the accordion sections that hold a match, scrolls
            // each list to its first matching row and the topmost match into view. True when
            // something matched, or the filter is empty.
            bool filter( LLWString const &aFilter );
        };
    }

    namespace statusbar
    {
        struct SearchableItem;

        typedef std::shared_ptr< SearchableItem > SearchableItemPtr;

        typedef std::vector< SearchableItemPtr > tSearchableItemList;

        struct SearchableItem
        {
            LLWString mLabel;
            LLMenuItemGL *mMenu;
            tSearchableItemList mChildren;
            ll::ui::SearchableControl const *mCtrl;
            bool mWasHiddenBySearch;

            SearchableItem();

            void setNotHighlighted( );
            bool hightlightAndHide( LLWString const &aFilter, bool hide = true );
        };

        struct SearchData
        {
            SearchableItemPtr mRootMenu;
            LLWString mLastFilter;
        };
    }
}

#endif
