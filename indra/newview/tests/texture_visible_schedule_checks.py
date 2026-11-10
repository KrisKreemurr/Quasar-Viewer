#!/usr/bin/env python3
"""Exercise production bounded visible selection, renderer admission, and record order."""
import argparse,pathlib,subprocess
ap=argparse.ArgumentParser();ap.add_argument('--source-root',type=pathlib.Path,required=True);ap.add_argument('--out',type=pathlib.Path,required=True);a=ap.parse_args();root=a.source_root.resolve();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
s=(root/'indra/newview/vkgfx/llcleanrenderervk.cpp').read_text()
def between(start,end):
 i=s.index(start);return s[i:s.index(end,i)]
select=between('        // Fixed-size top-k:', '    phase("vk_texP_liveScan");')
# Include the already-open if(mat_scan) body, excluding only its closing brace.
select=select[:select.rfind('    }')]
admit=between('    const uint32_t end = priority == Priority::SelfOrHud', '    if (!ticket) { ++mTexStageInterval.refuseTickets;')
order=between('    std::array<uint32_t, kTexStageTicketCap> order{};', '    for (uint32_t step = 0; step < count && recorded < kMaxPerFrame')
code=r'''#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>
enum class Priority:uint8_t{Background,Visible,SelfOrHud};
struct TexStageTicket{enum class Life{Free,Worker};Life life=Life::Free;Priority priority=Priority::Background;};
constexpr uint32_t kTexStageTicketCap=128;
struct H{std::array<TexStageTicket,128>mTexStageTickets{};uint32_t mTexStageRecordCursor=0;
std::array<uint32_t,256>mVisibleMats{};uint32_t mVisibleMatCount=0;
std::vector<uint32_t>mat_demand;std::vector<uint8_t>live_mat,mMatSettled;
void select(){SELECT}
bool admit(Priority priority){TexStageTicket*ticket=nullptr;ADMIT if(!ticket){return false;}ticket->life=TexStageTicket::Life::Worker;ticket->priority=priority;return true;}
auto order(){const uint32_t count=128;ORDER return order;}};
int main(){H h;h.mat_demand.assign(8000,0);h.live_mat.assign(8000,1);h.mMatSettled.assign(8000,0);
for(uint32_t i=7600;i<8000;++i)h.mat_demand[i]=i-7599;
h.mMatSettled[7999]=1;h.select();assert(h.mVisibleMatCount==256);assert(h.mVisibleMats[0]==7998);assert(h.mVisibleMats[255]==7743);
for(unsigned i=0;i<96;++i){assert(h.admit(Priority::Background));}assert(!h.admit(Priority::Background));
for(unsigned i=0;i<24;++i){assert(h.admit(Priority::Visible));}assert(!h.admit(Priority::Visible));
for(unsigned i=0;i<8;++i){assert(h.admit(Priority::SelfOrHud));}assert(!h.admit(Priority::SelfOrHud));
auto order=h.order();for(unsigned i=0;i<8;++i)assert(h.mTexStageTickets[order[i]].priority==Priority::SelfOrHud);
for(unsigned i=8;i<32;++i)assert(h.mTexStageTickets[order[i]].priority==Priority::Visible);
h.mTexStageRecordCursor=119;order=h.order();assert(order[8]==119);assert(order[9]==96);
std::cout<<"production visible top256, BG96/Visible24/Self8 admission and recording priority PASS\n";}
'''
code=code.replace('SELECT',select).replace('ADMIT',admit).replace('ORDER',order)
cpp=out/'schedule.cpp';cpp.write_text(code);cmd=['/usr/bin/c++','-std=c++20','-O0','-Wall','-Wextra','-Werror',str(cpp),'-o',str(out/'schedule')];subprocess.run(cmd,check=True,cwd=out);subprocess.run([str(out/'schedule')],check=True,cwd=out)
