#!/usr/bin/env python3
# Author: SkyLine. SPDX-License-Identifier: LGPL-2.1-only
"""Actual producer/consumer functions; stub only existing read-only backend APIs and work."""
import argparse
from pathlib import Path
import re
import shlex
import subprocess


def block(text,start):
    begin=text.index('{',start);depth=0
    for end in range(begin,len(text)):
        if text[end]=='{':depth+=1
        elif text[end]=='}':
            depth-=1
            if depth==0:return text[begin:end+1]
    raise ValueError('source block incomplete')


def function(text,signature):
    start=text.index(signature);return text[start:text.index('{',start)]+block(text,start)


def extract(root):
    source=(root/'indra/newview/llviewertexture.cpp').read_text()
    header=(root/'indra/newview/llviewertexture.h').read_text()
    listing=(root/'indra/newview/llviewertexturelist.cpp').read_text()
    producer=function(source,'void LLViewerFetchedTexture::setVKDemand(')
    request=function(source,'void LLViewerFetchedTexture::vkRequestCpuCopy(')
    modern='bool LLViewerFetchedTexture::vkDirtyNoop() const' in source
    seen=function(source,'void LLViewerFetchedTexture::vkMarkSeen()') if modern else 'void LLViewerFetchedTexture::vkMarkSeen() '+block(header,header.index('void vkMarkSeen()'))
    noop=function(source,'bool LLViewerFetchedTexture::vkDirtyNoop() const') if modern else ''
    begin=listing.index('LLViewerFetchedTexture* imagep = dirty[i].get();')
    end=listing.index('if (timer.getElapsedTimeF32() > max_time * 0.5f)',begin)
    consumer=listing[begin:end]
    stale=re.search(r'constexpr F32 VK_DEMAND_STALE_SEC = [^;]+;',source).group()
    return producer,request,seen,noop,consumer,stale


CPP=r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>
using U32=uint32_t;using U16=uint16_t;using U8=uint8_t;using S32=int32_t;using S8=int8_t;using F32=float;
template<class T>T llmax(T a,T b){return std::max(a,b);}
template<class T>T llmin(T a,T b){return std::min(a,b);}
template<class T>T llclamp(T a,T b,T c){return std::clamp(a,b,c);}
constexpr S32 MAX_DISCARD_LEVEL=6;constexpr S8 S8_MAX=127;
@STALE@
struct LLImageRaw{};
template<class T>struct Pointer {
    T* value=nullptr;T* get()const{return value;}T* operator->()const{return value;}
    bool notNull()const{return value!=nullptr;}bool isNull()const{return value==nullptr;}
};
struct LLTextureData {
    bool dropped=true,ui=false,mask=false;U8 gradient=0;U16 side=512;S32 generation=7;
    U32 lease=0;LLImageRaw* raw=nullptr;
    bool isVKCpuDropped()const{return dropped;}LLImageRaw* getVKCpuData()const{return raw;}
    S32 getVKGeneration()const{return generation;}
    void getVKAlphaFacts(bool& m,U8& g,U16& s)const{m=mask;g=gradient;s=side;}
};
class LLViewerFetchedTexture;
struct Ref {LLViewerFetchedTexture* value;Ref(LLViewerFetchedTexture* p):value(p){}LLViewerFetchedTexture* get()const{return value;}};
class LLViewerFetchedTexture {
public:
    LLTextureData image;Pointer<LLTextureData> mTextureData{&image};Pointer<LLImageRaw> mRawImage;
    static std::vector<Ref> sVKDirty;static F32 sCurrentTime;static U32 sVKResourced;
    struct VKDecodeDemand{U32 texels=0,pass=0;F32 time=0;};VKDecodeDemand mVKDecodeDemand[2];
    U32 mVKDemand=0,mVKDemandSeen=~0u,mVKDemandPass=0,mVKGpuTexels=0;
    F32 mVKDemandTime=-1.e9f,mVKResourceSince=-1;
    bool mVKT2Safe=false,mVKDirtyQueued=false,mVKDirtyForced=false,mFullyLoaded=false;
    S8 mVKResourceDiscard=-1,mDesiredDiscardLevel=6;
    S8 mMinDesiredDiscardLevel=MAX_DISCARD_LEVEL+1,mLoadedCallbackDesiredDiscardLevel=S8_MAX;
    bool mIsFetching=false,mHasFetcher=false,mNeedsCreateTexture=false,mIsMissingAsset=false;
    std::vector<int> mLoadedCallbackList;
    const LLTextureData* mVKImageSeen=nullptr;S32 mVKGenerationSeen=-1;
    bool mVKAlphaMaskSeen=false;U8 mVKAlphaGradientSeen=255;U16 mVKAlphaSideSeen=0;
    bool active=true,resourceable=true,gpuCovered=true,forceDuringWork=false;
    unsigned priorityCalls=0,fetchCalls=0;
    bool vkDemandActive()const{return active;}
    bool vkGpuCoversDemand()const{return gpuCovered&&mVKGpuTexels>=mVKDemand;}
    // The canonical production lease implementation is unchanged; these are API inputs.
    U32 vkCpuLeaseMask()const{return image.lease|(image.ui?2u:0u)|(resourceable?0u:4u);}
    bool vkCpuResourceable()const{return resourceable;}
    S32 getDiscardLevel()const{return 0;}
    bool isInImageList()const{return true;}int getNumRefs()const{return 3;}
    LLTextureData* getTextureData()const{return mTextureData.get();}
    void setVKDemand(U32,U32,U32,bool,bool,bool=false);
    void vkRequestCpuCopy(S32);void vkMarkSeen();bool vkDirtyNoop()const;
    void updateFetch(){++fetchCalls;if(forceDuringWork){forceDuringWork=false;vkRequestCpuCopy(0);}}
};
std::vector<Ref> LLViewerFetchedTexture::sVKDirty;
F32 LLViewerFetchedTexture::sCurrentTime=1;
U32 LLViewerFetchedTexture::sVKResourced=0;
@PRODUCER@
@REQUEST@
@SEEN@
@NOOP@
void updateImageDecodePriority(LLViewerFetchedTexture* t){++t->priorityCalls;}
void consume(){
    std::vector<Ref> dirty;dirty.swap(LLViewerFetchedTexture::sVKDirty);unsigned n_dirty=0;
    for(std::size_t i=0;i<dirty.size();++i){@CONSUMER@}
    (void)n_dirty;
}
unsigned pass=1;
void draw(LLViewerFetchedTexture& t,bool decode=false){
    LLViewerFetchedTexture::sCurrentTime=1.f+pass*.01f;
    t.setVKDemand(128,++pass,2048,true,false);
    t.setVKDemand(1024,pass,2048,true,decode);
}
void bootstrap(LLViewerFetchedTexture& t){
    LLViewerFetchedTexture::sVKDirty.clear();
    t.setVKDemand(1024,++pass,2048,true,false);consume();
    t.mFullyLoaded=true; // Existing backend load completed before stable-frame fixtures.
    t.priorityCalls=t.fetchCalls=0;
}
template<class Change>void mustWork(Change change){
    LLViewerFetchedTexture t;bootstrap(t);draw(t);change(t);consume();
    assert(t.priorityCalls==1&&t.fetchCalls==1);
    LLViewerFetchedTexture::sVKDirty.clear();
}
int main(){
    LLViewerFetchedTexture stable;bootstrap(stable);
    for(unsigned i=0;i<100;++i){draw(stable);consume();}
    std::cout<<"stable final-demand heavy calls priority="<<stable.priorityCalls<<" fetch="<<stable.fetchCalls<<std::endl;
    if(stable.priorityCalls||stable.fetchCalls)return 17;
    mustWork([](auto& t){t.mVKDemand=2048;});
    mustWork([](auto& t){t.mFullyLoaded=false;});
    mustWork([](auto& t){t.mMinDesiredDiscardLevel=0;});
    mustWork([](auto& t){t.mLoadedCallbackDesiredDiscardLevel=0;});
    mustWork([](auto& t){t.mIsFetching=true;});
    mustWork([](auto& t){t.mHasFetcher=true;});
    mustWork([](auto& t){t.mNeedsCreateTexture=true;});
    mustWork([](auto& t){t.mIsMissingAsset=true;});
    mustWork([](auto& t){t.mLoadedCallbackList.push_back(1);});
    mustWork([](auto& t){t.mVKResourceDiscard=0;});
    mustWork([](auto& t){t.active=false;});
    mustWork([](auto& t){t.image.dropped=false;});
    mustWork([](auto& t){static LLImageRaw raw;t.image.raw=&raw;});
    mustWork([](auto& t){static LLImageRaw raw;t.mRawImage.value=&raw;});
    mustWork([](auto& t){t.image.lease=1;});
    mustWork([](auto& t){t.image.ui=true;});
    mustWork([](auto& t){t.resourceable=false;});
    mustWork([](auto& t){t.gpuCovered=false;});
    mustWork([](auto& t){t.mVKGpuTexels=0;});
    mustWork([](auto& t){++t.image.generation;});
    mustWork([](auto& t){static LLTextureData replacement;t.mTextureData.value=&replacement;});
    mustWork([](auto& t){t.image.mask=true;});
    mustWork([](auto& t){++t.image.gradient;});
    mustWork([](auto& t){++t.image.side;});
    LLViewerFetchedTexture decode;bootstrap(decode);draw(decode,true);
    assert(decode.mVKDirtyForced&&decode.mVKDirtyQueued);
    // A deferred forced entry survives into a pass whose final demand and GPU coverage match.
    decode.setVKDemand(1024,++pass,2048,true,false);
    assert(decode.mVKDirtyForced&&decode.mVKGpuTexels==2048);
    consume();assert(decode.fetchCalls==1&&!decode.mVKDirtyForced);
    LLViewerFetchedTexture cpu;bootstrap(cpu);draw(cpu);cpu.vkRequestCpuCopy(0);
    assert(cpu.mVKDirtyQueued&&cpu.mVKDirtyForced);consume();assert(cpu.fetchCalls==1);
    LLViewerFetchedTexture during;bootstrap(during);draw(during);during.mVKDemand=2048;during.forceDuringWork=true;
    consume();assert(during.mVKDirtyForced&&during.mVKDirtyQueued&&LLViewerFetchedTexture::sVKDirty.size()==1);
    consume();assert(during.fetchCalls==2&&!during.mVKDirtyForced);
    std::cout<<"PASS changed-final, forced decode/copy, deferred/reentrant force, CPU/raw/asset/gen/alpha/lease/callback preservation"<<std::endl;
}
'''


def run(root,out,label,regression):
    producer,request,seen,noop,consumer,stale=extract(root);code=CPP
    for token,value in {'PRODUCER':producer,'REQUEST':request,'SEEN':seen,'NOOP':noop,'CONSUMER':consumer,'STALE':stale}.items():
        code=code.replace('@'+token+'@',value)
    cpp=out/(label+'.cpp');exe=out/label;cpp.write_text(code);cpp.chmod(0o600)
    command=['g++','-std=c++17','-Wall','-Wextra','-Werror','-pedantic',str(cpp),'-o',str(exe)]
    print(shlex.join(command),flush=True);subprocess.run(command,check=True)
    result=subprocess.run([str(exe)],text=True,capture_output=True)
    print(label,result.stdout.strip(),flush=True)
    if regression:assert result.returncode==17 and 'priority=100 fetch=100' in result.stdout
    else:
        if result.returncode:print(result.stderr,flush=True)
        assert result.returncode==0


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root',type=Path,required=True);p.add_argument('--baseline-root',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    a.output.mkdir(parents=True,exist_ok=True);a.output.chmod(0o700)
    run(a.baseline_root,a.output,'baseline',True);run(a.source_root,a.output,'candidate',False)

if __name__=='__main__':main()
