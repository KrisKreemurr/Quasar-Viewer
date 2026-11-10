#!/usr/bin/env python3
"""Exercise the production per-pass saturation gate; no viewer or Vulkan driver."""
import argparse,pathlib,subprocess
ap=argparse.ArgumentParser();ap.add_argument('--source-root',type=pathlib.Path,required=True);ap.add_argument('--out',type=pathlib.Path,required=True);a=ap.parse_args();root=a.source_root.resolve();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
s=(root/'indra/newview/vkgfx/llcleanrenderervk.cpp').read_text();start=s.index('LLCleanRendererVK::TexStageStartResult LLCleanRendererVK::startImmutableTexStage(');brace=s.index('{',start);prefix=s[brace+1:s.index('    TexStageCpuTimer prepareCpu',brace)]
marker='    const bool inputBlocked =';b=s.find(marker,start)
if b<0:b=s.index('    if (usage.inputBytes > inputLimit',start)
block=s[b:s.index('    LLVKCpuLease mainLease(',b)]
code=r'''#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
enum class Priority:uint8_t{Background,Visible,SelfOrHud};
enum class TexStageStartResult{Deferred,Accepted};
struct Usage{uint64_t inputBytes=0,stageBytes=0,scratchBytes=0;};
struct Harness{uint32_t mTexPass=1;std::array<uint32_t,3>mTexStageBudgetBlockPass{{UINT32_MAX,UINT32_MAX,UINT32_MAX}};
struct{uint64_t budgetDeferredFast=0,refuseBudget=0,refuseInputBudget=0,refuseStageBudget=0,refuseScratchBudget=0,blockedInputPeak=0,blockedStagePeak=0,blockedScratchPeak=0;}mTexStageInterval;
uint64_t queries=0,captures=0;
TexStageStartResult run(Priority priority,Usage usage,uint64_t inputLimit,uint64_t stageLimit,uint64_t scratchLimit,uint64_t knownInput,uint64_t outputEstimate,uint64_t scratchEstimate){(void)priority;PREFIX ++queries;BLOCK ++captures;return TexStageStartResult::Accepted;}};
constexpr uint64_t M=1024*1024;
int main(){Harness h;
for(unsigned i=0;i<250000;++i){assert(h.run(Priority::Visible,{0,160*M,0},64*M,128*M,64*M,4*M,4*M,20*M)==TexStageStartResult::Deferred);}
assert(h.queries==1&&h.captures==0&&h.mTexStageInterval.refuseBudget==1&&h.mTexStageInterval.budgetDeferredFast==249999);
assert(h.mTexStageInterval.refuseStageBudget==1&&h.mTexStageInterval.blockedStagePeak==160*M);
assert(h.run(Priority::SelfOrHud,{0,160*M,0},192*M,256*M,192*M,4*M,4*M,20*M)==TexStageStartResult::Accepted);
++h.mTexPass;assert(h.run(Priority::Visible,{},64*M,128*M,64*M,4*M,4*M,20*M)==TexStageStartResult::Accepted);
++h.mTexPass;auto q=h.queries;
assert(h.run(Priority::Visible,{},64*M,128*M,64*M,4*M,4*M,80*M)==TexStageStartResult::Deferred);
assert(h.run(Priority::Visible,{},64*M,128*M,64*M,4*M,4*M,20*M)==TexStageStartResult::Accepted);assert(h.queries==q+2);
++h.mTexPass;assert(h.run(Priority::Visible,{64*M,0,0},64*M,128*M,64*M,1,1,1)==TexStageStartResult::Deferred);
++h.mTexPass;assert(h.run(Priority::Visible,{},64*M,128*M,64*M,1,1,1)==TexStageStartResult::Accepted);
std::cout<<"PASS production budget gate: saturation checked once/pass, priority isolation, next-pass recovery, oversized request cannot poison smaller work\n";}
''';code=code.replace('PREFIX',prefix).replace('BLOCK',block);cpp=out/'budget.cpp';cpp.write_text(code);exe=out/'budget';subprocess.run(['/usr/bin/c++','-std=c++20','-O0','-Wall','-Wextra','-Werror',str(cpp),'-o',str(exe)],cwd=out,check=True);subprocess.run([str(exe)],cwd=out,check=True)
