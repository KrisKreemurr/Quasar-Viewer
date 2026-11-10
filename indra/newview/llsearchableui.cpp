/**
* @file llsearchableui.cpp
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

#include "llviewerprecompiledheaders.h"
#include "llsearchableui.h"

#include "llview.h"
#include "lltabcontainer.h"
#include "llmenugl.h"
#include "llaccordionctrl.h"
#include "llaccordionctrltab.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llscrollcontainer.h"
#include "llscrolllistcell.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "lluicolortable.h"
#include "slprefsearch.h"

#include <map>
#include <set>

namespace
{
    // #1348: whether the search can show the view. It and the views it sits in must be visible,
    // apart from tab containers and their pages and the bodies of accordion sections, which the
    // search shows itself; a control in a part of a page that is hidden otherwise is no match.
    bool can_reveal( const LLView* view )
    {
        for( const LLView* v = view; v; v = v->getParent() )
        {
            if( v->getVisible() || dynamic_cast< const LLTabContainer* >( v ) )
                continue;

            const LLView* parent = v->getParent();
            if( dynamic_cast< const LLTabContainer* >( parent ) )
                continue; // a page (or its tab) that isn't shown now

            LLAccordionCtrlTab* section = const_cast< LLAccordionCtrlTab* >( dynamic_cast< const LLAccordionCtrlTab* >( parent ) );
            if( section && section->getAccordionView() == v && dynamic_cast< const LLAccordionCtrl* >( section->getParent() ) )
                continue; // a closed section of an accordion, which the search opens

            return false;
        }
        return true;
    }

    bool has_words( const LLWString& text )
    {
        for( llwchar c : text )
        {
            if( !LLStringOps::isSpace( c ) )
                return true;
        }
        return false;
    }

    // #1348: the name of the setting a control changes, lower case. A check box keeps it on its
    // button; that button gets none, so the match lights the check box's label.
    LLWString setting_name( LLView* view )
    {
        if( dynamic_cast< LLCheckBoxCtrl* >( view->getParent() ) )
            return LLWString();

        LLUICtrl* ctrl = dynamic_cast< LLUICtrl* >( view );
        LLControlVariable* control = ctrl ? ctrl->getControlVariable() : nullptr;
        if( !control && dynamic_cast< LLCheckBoxCtrl* >( view ) )
        {
            for( LLView* child : *view->getChildList() )
            {
                LLButton* button = dynamic_cast< LLButton* >( child );
                if( button && button->getControlVariable() )
                {
                    control = button->getControlVariable();
                    break;
                }
            }
        }

        LLWString name = control ? utf8str_to_wstring( control->getName() ) : LLWString();
        LLWStringUtil::toLower( name );
        return name;
    }

    void collect_children( LLView const *aView, ll::prefs::PanelDataPtr aParentPanel, ll::prefs::TabContainerDataPtr aParentTabContainer,
                           ll::prefs::tSearchableItemList &aAll )
    {
        if( !aView )
            return;

        llassert_always( aParentPanel || aParentTabContainer );

        for( LLView* pView : *aView->getChildList() )
        {
            if( !pView )
                continue;

            ll::prefs::PanelDataPtr pCurPanelData = aParentPanel;
            ll::prefs::TabContainerDataPtr pCurTabContainer = aParentTabContainer;

            LLPanel const *pPanel = dynamic_cast< LLPanel const* >( pView );
            LLTabContainer const *pTabContainer = dynamic_cast< LLTabContainer const* >( pView );
            ll::ui::SearchableControl const *pSCtrl = dynamic_cast< ll::ui::SearchableControl const* >( pView );

            if( pTabContainer )
            {
                pCurPanelData.reset();

                pCurTabContainer = ll::prefs::TabContainerDataPtr( new ll::prefs::TabContainerData );
                pCurTabContainer->mTabContainer = const_cast< LLTabContainer* >( pTabContainer );
                pCurTabContainer->mLabel = pTabContainer->getLabel();
                pCurTabContainer->mPanel = 0;

                if( aParentPanel )
                    aParentPanel->mChildPanel.push_back( pCurTabContainer );
                if( aParentTabContainer )
                    aParentTabContainer->mChildPanel.push_back( pCurTabContainer );
            }
            else if( pPanel )
            {
                pCurTabContainer.reset();

                pCurPanelData = ll::prefs::PanelDataPtr( new ll::prefs::PanelData );
                pCurPanelData->mPanel = pPanel;
                pCurPanelData->mLabel = pPanel->getLabel();
                pCurPanelData->mTitle = utf8str_to_wstring( pCurPanelData->mLabel );
                LLWStringUtil::toLower( pCurPanelData->mTitle );

                if( aParentTabContainer )
                    aParentTabContainer->mChildPanel.push_back( pCurPanelData );
                else if( aParentPanel )
                    aParentPanel->mChildPanel.push_back( pCurPanelData );
            }
            else if( pSCtrl || dynamic_cast< LLScrollListCtrl* >( pView ) )
            {
                LLWString text = pSCtrl ? utf8str_to_wstring( pSCtrl->getSearchText() ) : LLWString();
                LLWStringUtil::toLower( text );
                ll::prefs::SearchableItemPtr item = ll::prefs::SearchableItemPtr( new ll::prefs::SearchableItem() );
                item->mView = pView;
                item->mCtrl = pSCtrl;
                item->mList = pSCtrl ? nullptr : dynamic_cast< LLScrollListCtrl* >( pView ); // its rows are read at each search
                item->mLabel = text;
                item->mSettingName = setting_name( pView );
                if( item->mList || has_words( text ) || !item->mSettingName.empty() )
                {
                    if( aParentPanel )
                        aParentPanel->mChildren.push_back( item );
                    if( aParentTabContainer )
                        aParentTabContainer->mChildren.push_back( item );
                    aAll.push_back( item );
                }
            }
            collect_children( pView, pCurPanelData, pCurTabContainer, aAll );
        }
    }

    // #1348: scroll the view into sight in each scroll container and accordion it sits in
    void scroll_into_view( LLView* view )
    {
        LLView* child = view;
        for( LLView* parent = view->getParent(); parent; child = parent, parent = parent->getParent() )
        {
            if( LLScrollContainer* scroller = dynamic_cast< LLScrollContainer* >( parent ) )
            {
                LLRect doc_rect; // in the scrolled panel's coordinates
                view->localRectToOtherView( view->getLocalRect(), &doc_rect, child );
                scroller->scrollToShowRect( doc_rect );
            }
            else if( LLAccordionCtrl* accordion = dynamic_cast< LLAccordionCtrl* >( parent );
                     accordion && !accordion->getFitParent() ) // as LLAccordionCtrlTab::notifyParent (EXT-8285)
            {
                LLRect screen_rect;
                view->localRectToScreen( view->getLocalRect(), &screen_rect );
                accordion->notifyParent( LLSD().with( "scrollToShowRect", screen_rect.getValue() ) );
            }
        }
    }

    // #1348: open the accordion sections that hold a match, in their body or heading (an
    // accordion that opens one at a time keeps the open one if it has a match, else opens the
    // first that does), mark their headings, and scroll the topmost match on the shown page into
    // view. The marked headings go in aMarked, to be cleared by the next search.
    void reveal( const ll::prefs::tSearchableItemList &aItems, std::vector< LLAccordionCtrlTab* > &aMarked )
    {
        std::map< LLAccordionCtrl*, std::set< const LLAccordionCtrlTab* > > matched;
        for( const ll::prefs::SearchableItemPtr &item : aItems )
        {
            if( !item->isHighlighted() )
                continue;

            for( const LLView* parent = item->mView->getParent(); parent; parent = parent->getParent() )
            {
                const LLAccordionCtrlTab* section = dynamic_cast< const LLAccordionCtrlTab* >( parent );
                LLAccordionCtrl* accordion = section ? dynamic_cast< LLAccordionCtrl* >( section->getParent() ) : nullptr;
                if( accordion )
                    matched[ accordion ].insert( section );
            }
        }

        for( const auto &[ accordion, sections ] : matched )
        {
            const std::vector< LLAccordionCtrlTab* > &tabs = accordion->getTabs();
            if( accordion->getSingleExpansion() )
            {
                std::vector< bool > has_match;
                S32 open_now = -1;
                for( size_t i = 0; i < tabs.size(); ++i )
                {
                    has_match.push_back( sections.count( tabs[ i ] ) > 0 && tabs[ i ]->canOpenClose() );
                    if( open_now < 0 && tabs[ i ]->getDisplayChildren() )
                        open_now = (S32)i;
                }
                const S32 open = sl_section_to_open( has_match, open_now );
                if( open >= 0 && open != open_now )
                    tabs[ open ]->changeOpenClose( false ); // the accordion closes the others
            }
            else
            {
                for( LLAccordionCtrlTab* tab : tabs )
                {
                    if( sections.count( tab ) && tab->canOpenClose() && !tab->getDisplayChildren() )
                        tab->changeOpenClose( false );
                }
            }
        }

        // The topmost match now in view, by its place on screen (the items aren't in layout
        // order), chosen before the closed sections' headings are marked. A page's tab is no
        // place to scroll to.
        LLView* target = nullptr;
        LLRect target_rect;
        for( const ll::prefs::SearchableItemPtr &item : aItems )
        {
            if( !item->isHighlighted() || !item->mView->isInVisibleChain()
                || dynamic_cast< const LLTabContainer* >( item->mView->getParent() ) )
                continue;

            LLRect rect;
            item->mView->localRectToScreen( item->mView->getLocalRect(), &rect );
            if( !target || rect.mTop > target_rect.mTop || ( rect.mTop == target_rect.mTop && rect.mLeft < target_rect.mLeft ) )
            {
                target = const_cast< LLView* >( item->mView );
                target_rect = rect;
            }
        }

        for( const auto &[ accordion, sections ] : matched )
        {
            for( LLAccordionCtrlTab* tab : accordion->getTabs() )
            {
                if( sections.count( tab ) )
                {
                    tab->setTitleHighlighted( true );
                    aMarked.push_back( tab );
                }
            }
        }

        if( target )
            scroll_into_view( target );

        // each list with a match shows its first matching row
        for( const ll::prefs::SearchableItemPtr &item : aItems )
        {
            if( !item->mList || !item->mListMatched )
                continue;
            for( LLScrollListItem* row : item->mList->getAllData() )
            {
                if( row->getHighlighted() )
                {
                    item->mList->scrollToShowItem( row );
                    break;
                }
            }
        }
    }
}

ll::prefs::SearchableItem::~SearchableItem()
{}

bool ll::prefs::SearchableItem::isHighlighted() const
{
    return mList ? mListMatched : mCtrl->getHighlighted();
}

void ll::prefs::SearchableItem::setNotHighlighted()
{
    if( mList )
    {
        if( mListMatched )
            mList->clearHighlightedItems();
        mListMatched = false;
        return;
    }
    mCtrl->setHighlighted( false );
}

bool ll::prefs::SearchableItem::hightlightAndHide( LLWString const &aFilter )
{
    if( !mList && mCtrl->getHighlighted() )
        return true; // lit by its spinner or radio group

    if( !can_reveal( mView ) )
        return false;

    if( aFilter.empty() )
    {
        setNotHighlighted();
        return true;
    }

    if( mList )
    {
        // #1348: each row by the text of its cells, as a label
        for( LLScrollListItem* row : mList->getAllData() )
        {
            LLWString text;
            for( S32 i = 0; i < row->getNumColumns(); ++i )
            {
                if( const LLScrollListText* cell = dynamic_cast< const LLScrollListText* >( row->getColumn( i ) ) )
                    text += utf8str_to_wstring( cell->getValue().asString() ) + LLWString( 1, '\n' );
            }
            LLWStringUtil::toLower( text );
            if( sl_search_matches( text, aFilter ) )
            {
                row->setHighlighted( true );
                mListMatched = true;
            }
        }
        if( mListMatched )
            mList->setHighlightedColor( LLUIColorTable::instance().getColor( "SearchableControlHighlightBgColor", LLColor4::red4 ) );
        return mListMatched;
    }

    if( sl_search_matches_control( mLabel, mSettingName, aFilter ) )
    {
        mCtrl->setHighlighted( true );
        return true;
    }

    return false;
}

ll::prefs::PanelData::~PanelData()
{}

bool ll::prefs::PanelData::hightlightAndHide( LLWString const &aFilter, bool show_all )
{
    for( tSearchableItemList::iterator itr = mChildren.begin(); itr  != mChildren.end(); ++itr )
        (*itr)->setNotHighlighted();

    for (tPanelDataList::iterator itr = mChildPanel.begin(); itr != mChildPanel.end(); ++itr)
        (*itr)->setNotHighlighted();

    if (aFilter.empty())
    {
        // Still walk the child panels: a nested tab container (e.g. the Chat or
        // Privacy sub-tabs) must get the empty filter too, or the tabs it hid
        // for the previous term are never shown again.
        for (tPanelDataList::iterator itr = mChildPanel.begin(); itr != mChildPanel.end(); ++itr)
            (*itr)->hightlightAndHide(aFilter, false);
        return true;
    }

    bool bVisible( show_all );
    for( tSearchableItemList::iterator itr = mChildren.begin(); itr  != mChildren.end(); ++itr )
        bVisible |= (*itr)->hightlightAndHide( aFilter );

    for( tPanelDataList::iterator itr = mChildPanel.begin(); itr  != mChildPanel.end(); ++itr )
        bVisible |= (*itr)->hightlightAndHide( aFilter, show_all );

    return bVisible;
}

void ll::prefs::PanelData::setNotHighlighted()
{
    for (tSearchableItemList::iterator itr = mChildren.begin(); itr != mChildren.end(); ++itr)
        (*itr)->setNotHighlighted();

    for (tPanelDataList::iterator itr = mChildPanel.begin(); itr != mChildPanel.end(); ++itr)
        (*itr)->setNotHighlighted();
}

bool ll::prefs::TabContainerData::hightlightAndHide( LLWString const &aFilter, bool show_all )
{
    for( tSearchableItemList::iterator itr = mChildren.begin(); itr  != mChildren.end(); ++itr )
        (*itr)->setNotHighlighted( );

    bool bVisible( show_all );
    for( tSearchableItemList::iterator itr = mChildren.begin(); itr  != mChildren.end(); ++itr )
        bVisible |= (*itr)->hightlightAndHide( aFilter );

    // #1348: the pages' visibility is set together, so the shown page is chosen once
    std::vector< std::pair< LLPanel const*, bool > > pages;
    for( tPanelDataList::iterator itr = mChildPanel.begin(); itr  != mChildPanel.end(); ++itr )
    {
        // #1348: a page whose name matches is shown whole
        const bool named = !aFilter.empty() && (*itr)->mPanel && sl_search_matches( (*itr)->mTitle, aFilter );
        bool bPanelVisible = (*itr)->hightlightAndHide( aFilter, show_all || named ) || named;
        if( (*itr)->mPanel )
            pages.emplace_back( (*itr)->mPanel, bPanelVisible );
        bVisible |= bPanelVisible;
    }
    mTabContainer->setTabsVisibility( pages );

    return bVisible;
}

void ll::prefs::SearchData::collect( LLTabContainer* root )
{
    mItems.clear();
    mRootTab = TabContainerDataPtr( new TabContainerData );
    mRootTab->mTabContainer = root;
    mRootTab->mLabel = root->getLabel();
    mRootTab->mPanel = 0;
    collect_children( root, PanelDataPtr(), mRootTab, mItems );
}

bool ll::prefs::SearchData::filter( LLWString const &aFilter )
{
    mLastFilter = aFilter;
    if( !mRootTab )
        return false;

    for( LLAccordionCtrlTab* tab : mMarked )
        tab->setTitleHighlighted( false );
    mMarked.clear();

    const bool found = mRootTab->hightlightAndHide( aFilter, false );
    if( !aFilter.empty() )
        reveal( mItems, mMarked );
    return found;
}

ll::statusbar::SearchableItem::SearchableItem()
    : mMenu(0)
    , mCtrl(0)
    , mWasHiddenBySearch( false )
{ }

void ll::statusbar::SearchableItem::setNotHighlighted( )
{
    for( tSearchableItemList::iterator itr = mChildren.begin(); itr  != mChildren.end(); ++itr )
        (*itr)->setNotHighlighted( );

    if( mCtrl )
    {
        mCtrl->setHighlighted( false );

        if (mWasHiddenBySearch)
        {
            mMenu->setVisible(true);
            mWasHiddenBySearch = false;
        }
    }
}

bool ll::statusbar::SearchableItem::hightlightAndHide(LLWString const &aFilter, bool hide)
{
    if ((mMenu && !mMenu->getVisible() && !mWasHiddenBySearch) || dynamic_cast<LLMenuItemTearOffGL*>(mMenu))
        return false;

    setNotHighlighted( );

    if( aFilter.empty() )
    {
        if( mCtrl )
            mCtrl->setHighlighted( false );
        return true;
    }

    bool bHighlighted(!hide);
    if( mLabel.find( aFilter ) != LLWString::npos )
    {
        if( mCtrl )
            mCtrl->setHighlighted( true );
        bHighlighted = true;
    }

    bool bVisible(false);
    for (tSearchableItemList::iterator itr = mChildren.begin(); itr != mChildren.end(); ++itr)
        bVisible |= (*itr)->hightlightAndHide(aFilter, !bHighlighted);

    if (mCtrl && !bVisible && !bHighlighted)
    {
        mWasHiddenBySearch = true;
        mMenu->setVisible(false);
    }
    return bVisible || bHighlighted;
}
