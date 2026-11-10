#!/usr/bin/env python3
# Author: SkyLine. SPDX-License-Identifier: LGPL-2.1-only
"""Compile actual renderer queue bookkeeping, without Vulkan/viewer/GPU execution.

Extract admission, loop gates/consumption guard, both failure branches and tail
retention verbatim. Stub only image upload outcomes and time/resource inputs.
"""
import argparse
from pathlib import Path
import re
import shlex
import subprocess


def block(text, start):
    begin=text.index('{',start);depth=0
    for end in range(begin,len(text)):
        if text[end]=='{': depth+=1
        elif text[end]=='}':
            depth-=1
            if depth==0:return text[start:end+1]
    raise ValueError('unclosed source block')


def extract(source):
    start=source.index('// Upgrades: self/HUD first, then the largest shortfall')
    upgrade=source[start:source.index('if (mResDownsUnsorted)',start)]
    admission=block(source,source.rindex('if (mResUpQueued.insert(kv.first).second)',0,start))
    modern='struct QueuedUpgrade' in upgrade
    guard=block(upgrade,upgrade.index('struct QueuedUpgrade'))+';' if modern else ''
    first=upgrade.index('const Up& u = ups[ui];')
    last=upgrade.index('QueuedUpgrade queued{',first) if modern else upgrade.index('mResUpQueued.erase(u.key);   // consumed',first)
    last=upgrade.index(';',last)+1
    prefix=upgrade[first:last]
    failures=re.findall(r'if \(!base\.image\) \{ [^\n]*?break; \}',upgrade)
    assert len(failures)==2
    pending='queued.keep = true;' if modern else 'mResUpQueued.insert(u.key);'
    assert pending in upgrade[upgrade.index('auto request ='):upgrade.index('bool t2_failed')]
    tail=re.search(r'if \(ui < ups.size\(\)\) ups_left.insert\([^\n]+\);\s*ups.swap\(ups_left\);',upgrade).group()
    return admission,guard,prefix,failures,pending,tail


CPP=r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <vector>
bool failAllocation=false;
void* operator new(std::size_t bytes) {
    if(failAllocation)throw std::bad_alloc();
    if(void* memory=std::malloc(bytes?bytes:1))return memory;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
struct Up {uint32_t prio;float ratio;uint64_t key;};
struct SharedTex {bool pinned=false;uint32_t made_pass=0;};
struct GpuImage {bool image;};
using F32=float;
enum class Mode {Accept,Refuse,ReadPending,UploadThrows,FailureHandlerThrows};
struct Harness {
    std::vector<Up> mResUps;
    std::unordered_set<uint64_t> mResUpQueued;
    std::unordered_map<uint64_t,SharedTex> mSharedTex;
    bool mResUpsUnsorted=false;
    uint32_t mTexPass=1;
    Harness(){mResUps.reserve(64);mResUpQueued.reserve(64);mSharedTex.reserve(64);}
    void scan(uint64_t key,bool priority=false) {
        auto& ups=mResUps;
        const std::pair<uint64_t,int> kv{key,0};
        const bool prio=priority;const unsigned T=1024,R=128;
        @ADMISSION@
    }
    unsigned step(Mode mode,unsigned failurePath=0,bool pressure=false,bool budget=true) {
        auto& ups=mResUps;
        const bool mTexPressure=pressure;
        const unsigned kPrioImgCap=16,kMaxImgOps=4;
        unsigned prio_img_ops=0,n_up=0;
        const F32 s_img_budget=budget?1.f:0.f;
        const auto res_ms=[]{return 0.5;};
        bool img_starved=false;
        auto note_oom=[&]{img_starved=true;if(mode==Mode::FailureHandlerThrows)throw std::bad_alloc();};
        std::vector<Up> ups_left;ups_left.reserve(64); // Test avoids unrelated tail allocations.
        @GUARD@
        size_t ui=0;
        for(;ui<ups.size();++ui) {
            @PREFIX@
            if(mode==Mode::ReadPending){@PENDING@ ups_left.push_back(u);continue;}
            // Allocation injection is confined to upload + actual refusal bookkeeping.
            failAllocation=true;
            if(mode==Mode::UploadThrows)throw std::bad_alloc();
            GpuImage base{mode==Mode::Accept};
            if(failurePath==0){@FAILURE0@}else{@FAILURE1@}
            failAllocation=false;
            ++n_up;if(u.prio)++prio_img_ops;
        }
        failAllocation=false;
        @TAIL@
        (void)img_starved;
        return n_up;
    }
    void live(uint64_t key){mSharedTex.emplace(key,SharedTex{});}
};
int main(){
    for(unsigned failurePath:{0u,1u}) {
        Harness h;h.live(1);
        for(unsigned i=0;i<32;++i){h.scan(1);h.step(Mode::Refuse,failurePath);}
        std::cout<<"pending="<<h.mResUps.size()<<" membership="<<h.mResUpQueued.size()<<std::endl;
        if(h.mResUps.size()!=1 || h.mResUpQueued.size()!=1)return 17;
        // Refusal performs no replacement insertion/allocation; acceptance makes progress.
        assert(h.step(Mode::Accept,failurePath)==1);
        assert(h.mResUps.empty()&&h.mResUpQueued.empty());
    }
    for(Mode mode:{Mode::ReadPending,Mode::UploadThrows,Mode::FailureHandlerThrows}) {
        Harness h;h.live(2);h.scan(2);
        try{h.step(mode);}catch(const std::bad_alloc&){}
        failAllocation=false;
        assert(h.mResUps.size()==1&&h.mResUpQueued.count(2)==1);
        assert(h.step(Mode::Accept)==1&&h.mResUps.empty()&&h.mResUpQueued.empty());
    }
    Harness h;h.live(3);h.scan(3);
    assert(h.step(Mode::Accept,0,true)==0&&h.mResUpQueued.count(3)); // Pressure gate unchanged.
    assert(h.step(Mode::Accept,0,false,false)==0&&h.mResUpQueued.count(3)); // Time budget.
    h.mSharedTex[3].made_pass=1;
    assert(h.step(Mode::Accept)==0&&h.mResUpQueued.count(3)); // Pending same-pass image.
    h.mSharedTex[3].made_pass=0;h.mSharedTex[3].pinned=true;
    assert(h.step(Mode::Accept)==0&&h.mResUps.empty()&&h.mResUpQueued.empty());
    h.scan(4); // Deleted/stale shared image: discard, never retain membership.
    assert(h.step(Mode::Accept)==0&&h.mResUps.empty()&&h.mResUpQueued.empty());
    h.live(5);h.scan(5,true);
    assert(h.step(Mode::Accept,0,true)==1); // Existing priority allowance unchanged.
    h.live(6);h.scan(6);h.mResUps.clear();h.mResUpQueued.clear();
    h.scan(6);assert(h.step(Mode::Accept)==1); // Whole-queue teardown/restart.
    std::cout<<"PASS actual queue counts/progress, both refusals, pending, exceptions, gates, drops"<<std::endl;
}
'''


def run(source,out,label,regression):
    admission,guard,prefix,failures,pending,tail=extract(source.read_text())
    code=CPP
    for token,value in {'ADMISSION':admission,'GUARD':guard,'PREFIX':prefix,'PENDING':pending,'FAILURE0':failures[0],'FAILURE1':failures[1],'TAIL':tail}.items():
        code=code.replace('@'+token+'@',value)
    cpp=out/(label+'.cpp');exe=out/label;cpp.write_text(code);cpp.chmod(0o600)
    command=['g++','-std=c++17','-Wall','-Wextra','-Werror','-pedantic',str(cpp),'-o',str(exe)]
    print(shlex.join(command),flush=True);subprocess.run(command,check=True)
    result=subprocess.run([str(exe)],text=True,capture_output=True)
    print(label,result.stdout.strip(),flush=True)
    if regression:
        assert result.returncode==17 and 'pending=32 membership=0' in result.stdout
        print('PASS baseline reproduces repeated-refusal growth and lost membership',flush=True)
    else:
        if result.returncode:print(result.stderr,flush=True)
        assert result.returncode==0


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,required=True)
    p.add_argument('--baseline',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True);a.output.chmod(0o700)
    if "TexStageDemand" in a.source.read_text():
        # Upgrades now become bounded shared-service tickets rather than creating
        # images inside this historical synchronous queue block.
        import sys
        root = Path(__file__).resolve().parents[3]
        for name in ("texture_prep_priority_checks.py", "texture_prep_budget_gate_checks.py"):
            command = [sys.executable, str(Path(__file__).with_name(name)),
                       "--source-root", str(root), "--out", str(a.output / name.removesuffix(".py"))]
            if name == "texture_prep_priority_checks.py":
                command.append("--run")
            subprocess.run(command, check=True)
        print("PASS unified ticket admission/priority bounds (legacy synchronous block replaced)")
        return
    run(a.baseline,a.output,'baseline',True)
    run(a.source,a.output,'candidate',False)

if __name__=='__main__':main()
