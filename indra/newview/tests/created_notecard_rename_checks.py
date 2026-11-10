#!/usr/bin/env python3
"""Source-bound #1286 regression. Default prepares only; --run requires CPU slot.

Tests actual production creation/response lambdas with a small UI/model fixture.
It is not a viewer/network test. Existing bridge permissions remain production code.
"""
import argparse
import pathlib
import subprocess

parser=argparse.ArgumentParser()
parser.add_argument('--output',type=pathlib.Path,required=True)
parser.add_argument('--run',action='store_true')
a=parser.parse_args()
viewer=pathlib.Path(__file__).resolve().parents[1]
cpp=(viewer/'llpanelmaininventory.cpp').read_text()
inv=(viewer/'llviewerinventory.cpp').read_text()
header=(viewer/'llviewerinventory.h').read_text()
model=(viewer/'llinventorymodel.cpp').read_text()
ais=(viewer/'llaisapi.cpp').read_text()
bridge=(viewer/'llinventorybridge.cpp').read_text()

def block(text,signature):
    start=text.index(signature);opening=text.index('{',start);depth=1;end=opening+1
    while depth:
        depth += (text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]

method=block(cpp,'void LLPanelMainInventory::createNotecardWithRename(')
do_create=block(cpp,'void LLPanelMainInventory::doCreate(')
# Check the production dispatch precedes both context-changing legacy operations.
assert do_create.index('createNotecardWithRename(request)') < do_create.index('reset_inventory_filter()')
assert do_create.index('createNotecardWithRename(request)') < do_create.index('selectAllItemsPanel()')
assert 'menu_create_inventory_item(nullptr, parent_id' in method
assert all(x not in method for x in ['setSelection','setNeedsAutoRename','reset_inventory_filter','selectAllItemsPanel'])
# Real callback chain appends the confirmed-ID callback after default permissions.
create=block(inv,'void create_new_item(')
assert create.index('LLBoostFuncInventoryCallback(create_notecard_cb)') < create.index('cb->addOnFireFunc(created_cb)') < create.index('create_inventory_item(')
notecard=block(inv,'void create_notecard_cb(')
assert notecard.index('set_default_permissions(') < notecard.index('gInventory.notifyObservers()')
fire=block(header,'    void fire(const LLUUID& item_id)')
add=block(header,'    void addOnFireFunc(')
assert 'for (inventory_func_type &func: mFireFuncs)' in fire and 'func(item_id)' in fire
assert 'mFireFuncs.push_back(fire_func)' in add
udp=block(model,'void LLInventoryModel::processUpdateCreateInventoryItem(')
assert udp.index('messageUpdateCore(') < udp.index('gInventoryCallbacks.fire(')
# AIS update application is synchronous before its completion callback dispatch.
assert ais.index('    onUpdateReceived(result, type, body);') < ais.index('    if (callback && !callback.empty())')
assert 'ais_update.doUpdate();' in ais
rename=block(bridge,'bool LLItemBridge::renameItem(')
assert rename.index('if (!isItemRenameable())') < rename.index('update_inventory_item(')
print('PASS source ordering/selection/filter contract; no compiler started.',flush=True)
# Compile the actual production early-dispatch prefix, not the large legacy branches.
prefix=do_create[:do_create.index('    reset_inventory_filter();')]+'    ++legacy_calls;\n}'
fixture=r'''
#include <cassert>
#include <cctype>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>
using U64=unsigned long long;
struct LLUUID {int value=0; LLUUID()=default; LLUUID(int v):value(v){} bool isNull()const{return value==0;} bool operator!=(LLUUID b)const{return value!=b.value;} static const LLUUID null;};
const LLUUID LLUUID::null{};
struct LLSD {std::string text;std::map<std::string,LLSD> fields;LLSD()=default;LLSD(const char* s):text(s){};LLSD& operator[](const char* k){return fields[k];}const LLSD& operator[](const char* k)const{return fields.at(k);}std::string asString()const{return text;}LLSD& operator=(const std::string& s){text=s;return *this;}};
template<class T> struct LLHandle {std::weak_ptr<T> p;LLHandle()=default;LLHandle(std::shared_ptr<T> q):p(q){}T* get()const{auto q=p.lock();return q.get();}};
struct LLPanel:std::enable_shared_from_this<LLPanel>{virtual ~LLPanel()=default;LLHandle<LLPanel> getHandle(){return {shared_from_this()};}};
struct LLInventoryPanel:LLPanel {bool visible=true,root=true;std::string filter="typed search",tab="Favorites";int selection=99;void* getRootFolder(){return root?this:nullptr;}bool isInVisibleChain(){return visible;}LLHandle<LLInventoryPanel> getInventoryPanelHandle(){return {std::static_pointer_cast<LLInventoryPanel>(shared_from_this())};}};
namespace LLAssetType {enum {AT_NOTECARD=1,AT_LINK=2};}
namespace LLFolderType {enum {FT_NOTECARD=1};}
struct LLViewerInventoryItem {LLUUID id,parent{7};std::string name="New Note";int type=LLAssetType::AT_NOTECARD;bool allowed=true,defaults=false,trash=false;int getActualType()const{return type;}LLUUID getParentUUID()const{return parent;}std::string getName()const{return name;}};
struct Inventory {std::map<int,LLViewerInventoryItem> items;LLUUID findCategoryUUIDForType(int){return {7};}LLViewerInventoryItem* getItem(LLUUID id){auto it=items.find(id.value);return it==items.end()?nullptr:&it->second;}}gInventory;
std::vector<int> renamed;
struct LLItemBridge {LLUUID id;LLItemBridge(LLInventoryPanel*,void*,LLUUID i):id(i){}bool isItemRenameable(){auto p=gInventory.getItem(id);return p && p->allowed && p->defaults;}bool isItemInTrash(){auto p=gInventory.getItem(id);return !p||p->trash;}bool renameItem(const std::string& s){if(isItemRenameable()){gInventory.getItem(id)->name=s;renamed.push_back(id.value);}return false;}};
struct LLStringUtil {static void trim(std::string& s){auto first=s.find_first_not_of(" \t\r\n");if(first==std::string::npos){s.clear();return;}s=s.substr(first,s.find_last_not_of(" \t\r\n")-first+1);}};
struct LLInventoryObject {static void correctInventoryName(std::string&) {}};
using Response=std::function<void(const LLSD&,const LLSD&)>;
namespace LLNotificationsUtil {std::vector<Response> responses;void add(const char*,const LLSD&,const LLSD&,Response f){responses.push_back(f);}int getSelectedOption(const LLSD&,const LLSD& r){return r["choice"].asString()=="ok"?0:1;}}
using inventory_func_type=std::function<void(const LLUUID&)>;
struct OrderedCallbacks {std::list<inventory_func_type> mFireFuncs;
'''+add+'\n'+fire+r'''
};
std::vector<inventory_func_type> pending;
void menu_create_inventory_item(LLInventoryPanel* panel,LLUUID parent,const LLSD&,LLUUID,inventory_func_type cb){assert(!panel&&parent.value==7);pending.push_back(cb);}
struct LLPanelMainInventory:LLPanel {U64 mNotecardRenameRequest=0;bool mNotecardRenamePrompted=false,mSingleFolderMode=false;LLInventoryPanel* active=nullptr;int legacy_calls=0;LLInventoryPanel* getActivePanel(){return active;}void createNotecardWithRename(U64);void doCreate(const LLSD&);};
'''+method+'\n'+prefix+r'''
struct Case {
 std::shared_ptr<LLPanelMainInventory> owner=std::make_shared<LLPanelMainInventory>();
 std::shared_ptr<LLInventoryPanel> panel=std::make_shared<LLInventoryPanel>();
 Case(){pending.clear();LLNotificationsUtil::responses.clear();gInventory.items.clear();renamed.clear();owner->active=panel.get();gInventory.items[99].name="Original";}
 void start(){owner->doCreate(LLSD("notecard"));assert(owner->legacy_calls==0);assert(panel->filter=="typed search"&&panel->tab=="Favorites"&&panel->selection==99);}
 void complete(int request,int id){gInventory.items[id].id={id};OrderedCallbacks ordered;ordered.mFireFuncs.push_back([](LLUUID i){gInventory.items[i.value].defaults=true;});ordered.addOnFireFunc(pending.at(request));ordered.fire(LLUUID(id));}
 void answer(int dialog=0,const char* choice="ok",const char* name=" Named "){LLSD r;r["choice"]=std::string(choice);r["new_name"]=std::string(name);auto cb=LLNotificationsUtil::responses.at(dialog);cb(LLSD(),r);}
 void unchanged(){assert(gInventory.items[99].name=="Original");assert(panel->selection==99&&panel->filter=="typed search"&&panel->tab=="Favorites");}
};
int main(){
 {Case c;c.start();assert(LLNotificationsUtil::responses.empty());c.complete(0,1);assert(LLNotificationsUtil::responses.size()==1);c.answer();assert((renamed==std::vector<int>{1}));c.answer();pending[0](LLUUID(1));assert(renamed.size()==1&&LLNotificationsUtil::responses.size()==1);c.unchanged();}
 {Case c;c.start();c.start();c.complete(1,2);c.complete(0,1);assert(LLNotificationsUtil::responses.size()==1);c.answer();assert((renamed==std::vector<int>{2}));c.unchanged();}
 {Case c;c.start();c.complete(0,1);c.start();c.answer();assert(renamed.empty());c.unchanged();}
 {Case c;c.start();auto other=std::make_shared<LLInventoryPanel>();c.owner->active=other.get();c.complete(0,1);assert(LLNotificationsUtil::responses.empty());}
 {Case c;c.start();c.owner.reset();c.complete(0,1);assert(LLNotificationsUtil::responses.empty());}
 {Case c;c.start();c.owner->active=nullptr;c.panel.reset();c.complete(0,1);assert(LLNotificationsUtil::responses.empty());}
 {Case c;c.start();pending[0](LLUUID());assert(LLNotificationsUtil::responses.empty());}
 {Case c;c.start();c.complete(0,1);c.answer(0,"cancel");assert(renamed.empty());c.unchanged();}
 {Case c;c.start();c.complete(0,1);gInventory.items[1].allowed=false;c.answer();assert(renamed.empty());}
 {Case c;c.start();c.complete(0,1);gInventory.items[1].name="Manual";c.answer();assert(renamed.empty()&&gInventory.items[1].name=="Manual");}
 {Case c;c.start();c.complete(0,1);gInventory.items[1].parent={8};c.answer();assert(renamed.empty());}
 {Case c;c.start();c.complete(0,1);gInventory.items[1].trash=true;c.answer();assert(renamed.empty());}
 {Case c;c.start();c.complete(0,1);gInventory.items.erase(1);c.answer();assert(renamed.empty());}
 {Case c;c.start();c.complete(0,1);c.answer(0,"ok","  ");assert(renamed.empty());}
 {Case c;c.start();gInventory.items[1].type=LLAssetType::AT_LINK;c.complete(0,1);assert(LLNotificationsUtil::responses.empty());}
 {Case c;c.start();c.panel->visible=false;c.complete(0,1);assert(LLNotificationsUtil::responses.empty());}
 {Case c;c.start();gInventory.items[1].id={1};pending[0](LLUUID(1));assert(LLNotificationsUtil::responses.empty()); /* reversed/defaults-not-applied order refuses */}
}
'''
a.output.mkdir(parents=True,exist_ok=True)
source=a.output/'created_notecard_rename.cpp';source.write_text(fixture)
print('Prepared actual-source callback/selection/filter regression; no network/viewer actions.',flush=True)
if a.run:
    executable=a.output/'created_notecard_rename'
    subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',str(source),'-o',str(executable)],check=True)
    subprocess.run([str(executable)],check=True)
    print('PASS17 actual-source fixture scenarios.',flush=True)
