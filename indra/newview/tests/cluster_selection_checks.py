#!/usr/bin/env python3
"""CPU execution of the production legacy fixup and CLOD overlay filter bodies.
No Vulkan driver, viewer, geometry upload, or generated SPIR-V header is needed.
Usage: python3 cluster_selection_checks.py <external-output-directory>
"""
from pathlib import Path
import re
import subprocess
import sys
root = Path(__file__).resolve().parents[3]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
shaders = root / 'indra/vkclean/shaders'
legacy = (shaders / 'clean_sel_rig.comp').read_text()
legacy = legacy[legacy.index('bool riggedBucket'):].replace('void main()', 'void legacyFixup()')
cluster = (shaders / 'clean_cluster_selection.comp').read_text()
cluster = cluster[cluster.index('void main()'):].replace('void main()', 'void clusterFilter()')
hidden = (shaders/'clean_cluster_select_common.glsl').read_text()
hidden = hidden[hidden.index('bool clRowHidden('):].split('#endif')[0].replace('.length()', '.size()')
fixture = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>
using uint=uint32_t; using uvec4=uint;
using std::min;
struct XYZ {uint x=0,y=0,z=0;};
XYZ gl_GlobalInvocationID,gl_WorkGroupID; XYZ gl_NumWorkGroups{1,1,1};
struct DrawCmd {uint indexCount,instanceCount,firstIndex; int vertexOffset; uint firstInstance;};
DrawCmd sel[2]{},cand[2]{}; uint cand_meta[2]{};
struct Push {uint first=0,count=0,instance_count=0,pad=0; uint64_t address=0;uint view=0,cap=0,wire=0;} pc;
'''+legacy+r'''
constexpr uint ROWS=8192,COMMANDS=4+ROWS*2,CL_MAX_VIEWS=8;
constexpr uint CLC_VIEW_WORDS=96,CLC_SEL_CLAMPED=0,CLI_RIGGED=1,CLI1B_DRAW=1024,CLV_HIDE_SELF=2,CLI_SELF=4,CL_NONE=~0u,CLV_AVATARS_ONLY=16;
struct ClSelected {uint instance,cluster_ref;}; ClSelected cl_sel[3]{};
struct ClInstance {uint flags,render_instance;}; ClInstance cl_instances[3]{};
struct ClView {uint sel_base,flags;}; ClView cl_views[8]{};
struct ClRecord {uint triangles;};
uint cl_cnt[8*96]{},mapSlot[3]{0,0,0}; bool policy=false;
bool clAvatarPolicyOff(uint){return policy;}
bool clAvatarOffMain(uint){return false;}
uint clSkinnedCluster(uint i){return mapSlot[i];}
uint clRefSlot(uint){return 0;} uint clRefIndex(uint r){return r;}
ClRecord clLoadRecord(uint,uint r){return ClRecord{r+1};}
uint clRecTriCount(ClRecord r){return r.triangles;}
uint clFirstIndexByte(uint,ClRecord r){return r.triangles*12;}
std::vector<uint> words(COMMANDS+3*5,0xeeeeeeee);
struct Overlay {std::vector<uint>& words; explicit Overlay(uint64_t):words(::words){}};
uvec4 subgroupBallot(bool b){return b?1:0;} uint subgroupBallotBitCount(uvec4 b){return b;}
bool subgroupElect(){return true;} uint subgroupBroadcastFirst(uint x){return x;}
uint subgroupBallotExclusiveBitCount(uvec4){return 0;}
uint atomicAdd(uint& v,uint n){uint old=v;v+=n;return old;}
'''+cluster+r'''
struct HiddenAux {XYZ hide;}; HiddenAux cl_aux; std::vector<uint> cl_hide_rows;
'''+hidden+r'''
int main(){
 cl_aux.hide={0,1,0};cl_hide_rows={3,5,42,900};
 assert(clRowHidden(5)&&clRowHidden(42)&&clRowHidden(900)&&!clRowHidden(6));
 cl_aux.hide.y=0;assert(!clRowHidden(42));cl_aux.hide.y=1;
 cl_hide_rows[0]=2048;assert(!clRowHidden(42));cl_hide_rows[0]=3;

 // With CLOD off, the real legacy fixup finds a skinned candidate. Owned CLOD
 // intentionally has no legacy candidate, so that same production fixup empties it.
 pc.count=1;pc.instance_count=1;sel[0]={0,0,0,0,0};cand[0]={6,1,9,12,0};cand_meta[0]=3u<<24;
 legacyFixup();assert(sel[0].indexCount==6);
 cand_meta[0]=~0u;legacyFixup();assert(sel[0].indexCount==0&&sel[0].instanceCount==0);
 // The real new filter emits the current cut independently of that empty legacy slot.
 pc.count=1;pc.cap=2;pc.view=0;words[0]=0;words[4]=42;words[5]=1;
 cl_cnt[0]=3;for(uint i=0;i<3;++i){cl_sel[i]={i,i};cl_instances[i]={CLI_RIGGED|CLI1B_DRAW,42+i};}
 clusterFilter();assert(words[0]==1&&words[COMMANDS]==3&&words[COMMANDS+2]==12);
 assert(words[COMMANDS+3]==(1u<<30));
 words[0]=0;mapSlot[0]=CL_NONE;clusterFilter();assert(words[0]==0);mapSlot[0]=0;
 policy=true;clusterFilter();assert(words[0]==0);policy=false;
 cl_views[0].flags=CLV_HIDE_SELF;cl_instances[0].flags|=CLI_SELF;clusterFilter();assert(words[0]==0);
 cl_views[0].flags=0;cl_instances[0].flags&=~CLI_SELF;
 // All rigged cut entries in wireframe; exact bounded indirect range, no overwrite.
 pc.wire=1;pc.count=0;for(uint i=0;i<3;++i){gl_GlobalInvocationID.x=i;clusterFilter();}
 assert(words[0]==3&&words[COMMANDS+5]==6&&words[COMMANDS+10]==0xeeeeeeee);
 // Nothing selected and wireframe off emits no command; sorted-input miss stays absent.
 words[0]=0;pc.wire=0;gl_GlobalInvocationID.x=0;clusterFilter();assert(words[0]==0);
 std::cout<<"PASS legacy on/off reproduction; cut command, pose-map, policy/self, wire and cap checks\n";
}
'''
# Constants are read from the production ABI rather than maintained in the fixture.
abi = (shaders/'clean_cluster_common.glsl').read_text() + (shaders/'clean_cluster_select_common.glsl').read_text()
for key in ['CLC_SEL_CLAMPED','CLI_RIGGED','CLI1B_DRAW','CLV_HIDE_SELF','CLI_SELF']:
    value = re.search(r'^#define\s+'+key+r'\s+(\d+)u', abi, re.M).group(1)
    fixture = re.sub(r'\b'+key+r'=\d+', key+'='+value, fixture)
fixture = fixture.replace('cl_cnt[0]=3;', 'cl_cnt[CLC_SEL_CLAMPED]=3;')
src = out / 'cluster_selection_checks.cpp'
src.write_text(fixture)
subprocess.run(['c++','-std=c++20','-O2','-Wall','-Wextra','-Werror',str(src),'-o',str(out/'cluster_selection_checks')],check=True)
subprocess.run([str(out/'cluster_selection_checks')],check=True)
