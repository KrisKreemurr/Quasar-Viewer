#!/usr/bin/env python3
# $LicenseInfo:firstyear=2026&license=viewerlgpl$
# Copyright (C) 2026, Slipstream Viewer contributors.
# This file is licensed under LGPL version 2.1 only.
# $/LicenseInfo$
"""Extract real #1081 planner/central gate bodies into a no-GPU fixture.
Only dependency responses are synthetic. This script prepares source, never compiles.
"""
from pathlib import Path
import argparse
p=argparse.ArgumentParser();p.add_argument('tree',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
r=(a.tree/'indra/newview/vkgfx/llcleanrenderervk.cpp').read_text();d=(a.tree/'indra/newview/vkgfx/vk_device.cpp').read_text()
def span(s,start,end):return s[s.index(start):s.index(end,s.index(start))]
fmt=span(r,'namespace\n{\nconstexpr VkFormat kProbeHdr','// #565: can this device')
plan=span(r,'uint64_t LLCleanRendererVK::probeBytes(','uint64_t LLCleanRendererVK::probeHeldBytes()')
configure=span(r,'void LLCleanRendererVK::configureProbes(bool live)','// #599: draw set 0 bindings')
resize=span(r,'void LLCleanRendererVK::resizeProbesIfNeeded()','extern std::vector<float> gVKProbeSpheres')
pick=span(r,'int LLCleanRendererVK::vramPick(','bool LLCleanRendererVK::vramDriverFits(')
gate=span(d,'bool VkDeviceContext::allocationSizeAllowed(','bool VkDeviceContext::heapBudgetPreflight(')
# Guard the parts not executed by this fixture: gate before budget work/relief;
# allocation defense before any live probe member write; no format pre-destroy.
heap=d[d.index('bool VkDeviceContext::heapBudgetPreflight('):]
assert heap.index('if (!allocationSizeAllowed(bytes, what)) return false;')<heap.index('const bool soft = t_softAlloc;')
alloc=r[r.index('bool LLCleanRendererVK::allocProbes('):]
assert alloc.index('probeBytes(r, n, format)')<alloc.index('mProbeRes = r;')
assert 'destroyProbes();' not in resize
assert r.count('need != std::numeric_limits<uint64_t>::max() &&')>=2
header=(a.tree/'indra/newview/vkgfx/vk_allocation_limit.h').resolve()
prefix=r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <tuple>
#include "@HEADER@"
using U32=uint32_t;using S32=int32_t;
struct Sink { template<class T> Sink& operator<<(const T&){return *this;} }; Sink sink;
#define LL_INFOS(x) sink
#define LL_WARNS(x) sink
#define LL_WARNS_ONCE(x) sink
#define LL_ENDL 0
#define VK_HITCH_SCOPE(x) ((void)0)
#define LL_HAS_CLEAN_PIPELINE 1
struct LLVKHitch { static void armWatch(int){} };
int waits=0,driverCalls=0;
VkResult vkDeviceWaitIdle(VkDevice){++waits;return VK_SUCCESS;}
VkResult vkAllocateMemory(VkDevice,const VkMemoryAllocateInfo*,const VkAllocationCallbacks*,VkDeviceMemory* out){++driverCalls;*out=(VkDeviceMemory)(uintptr_t)1;return VK_SUCCESS;}
const char* vkResultString(VkResult){return "synthetic";}
bool t_softAlloc=false;
struct VkImageExtra { VkImageCreateFlags flags=0; const VkFormat* viewFormats=nullptr;uint32_t viewFormatCount=0;VkImageUsageFlags defaultViewUsage=0; };
struct Image { VkImage image=VK_NULL_HANDLE; };
struct VKProbeGPU { uint32_t payload[16]; };
constexpr uint32_t PROBE_IRR_RES=64,PROBE_CLUSTER_CAP=64,kProbeLiveCubes=2;
struct Query { uint32_t w,h,layers,mips;VkFormat fmt;VkImageUsageFlags usage;bool cube;bool extra; };
struct Device {
 uint64_t cap=4292870144ull;int mode=0;mutable std::vector<Query> images;mutable std::vector<uint64_t> buffers;
 VkDevice device()const{return (VkDevice)(uintptr_t)1;}uint64_t maxMemoryAllocationSize()const{return cap;}
 uint64_t answer(uint64_t normal)const{return mode==1?80:mode==2?0:mode==3?std::numeric_limits<uint64_t>::max()/2+1:mode==4?cap+1:normal;}
 uint64_t imageMemoryBytes(uint32_t w,uint32_t h,uint32_t layers,uint32_t mips,VkFormat fmt,VkImageUsageFlags usage,bool cube,const VkImageExtra* extra=nullptr)const{
  images.push_back({w,h,layers,mips,fmt,usage,cube,extra!=nullptr});
  // Emulate a padded driver requirement, including the reported failing array size.
  uint64_t bytes=65536;if(w==512&&layers==1536&&fmt==VK_FORMAT_R16G16B16A16_SFLOAT)bytes=4303355904ull;
  else if(layers>6)bytes=uint64_t(w)*h*layers*8;
  return answer(bytes);
 }
 uint64_t bufferMemoryBytes(uint64_t size,VkBufferUsageFlags)const{buffers.push_back(size);return answer((size+65535)&~uint64_t(65535));}
};
struct Budget { struct Caps{}; };
struct LLCleanRendererVK {
 Device* mCtx;Budget::Caps mBudgetCaps;Image mProbeRad;uint32_t mProbeRes=0,mProbeSlotsN=0,mProbeFmt=0;
 struct FitCap {bool valid=false,deferred=false;uint32_t key_a=0,key_b=0,got_a=0,got_b=0;};FitCap mProbeFit;uint32_t mProbeFitFormat=~0u;
 VkFormat mDepthFormat=VK_FORMAT_D32_SFLOAT;bool mDslProbeDefer=true;
 static constexpr uint32_t kGbufCount=4;static constexpr VkFormat kGbufFormats[4]={VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R16G16B16A16_SFLOAT};
 bool mProbesAbsent=false;uint32_t mProbeRetryStreak=0;double mProbeRetryAt=0;
 uint32_t wantR=512,wantN=256,wantF=0;uint64_t held=100,roomBytes=1ull<<40;bool failAlloc=false;
 int destroyCalls=0,allocCalls=0,reliefCalls=0,absentCalls=0;std::vector<std::tuple<uint32_t,uint32_t,uint32_t>> attempted;
 struct VramCand {std::string label;uint64_t need;};struct VramRoom {uint64_t avail=0,budget=0,usage=0,margin=0,drvFree=0,drvMargin=0;bool deferred=false;};
 explicit LLCleanRendererVK(Device& d):mCtx(&d){}
 void probeWanted(const Budget::Caps&,uint32_t& r,uint32_t& n,bool=true)const{r=wantR;n=wantN;}
 uint32_t probeFormatWanted()const{return wantF;}
 uint64_t probeBytes(uint32_t,uint32_t,uint32_t=~0u)const;
 uint64_t probeHeldBytes()const{return held;}
 VramRoom vramRoom(uint64_t)const{VramRoom v;v.avail=v.budget=roomBytes;return v;}
 bool vramDriverFits(uint64_t need,uint64_t* available=nullptr,uint64_t* margin=nullptr)const{if(available)*available=roomBytes;if(margin)*margin=0;return need<=roomBytes;}
 int vramPick(const char*,const std::vector<VramCand>&,uint64_t,int,VramRoom*)const;
 static std::string vramText(uint64_t n){return std::to_string(n);}
 void noteVramFit(const std::string&){}
 void destroyProbes(){++destroyCalls;mProbeRad.image=VK_NULL_HANDLE;}
 void vramRelief(){++reliefCalls;}
 bool probesAbsentFallback(){++absentCalls;mProbesAbsent=true;return true;}
 bool allocProbes(uint32_t r,uint32_t n,bool,uint32_t f=~0u){if(f==~0u)f=wantF;assert(probeBytes(r,n,f)!=std::numeric_limits<uint64_t>::max());++allocCalls;attempted.emplace_back(r,n,f);if(failAlloc)return false;mProbeRes=r;mProbeSlotsN=n;mProbeFmt=f;mProbeRad.image=(VkImage)(uintptr_t)1;return true;}
 void configureProbes(bool);void resizeProbesIfNeeded();
};
struct VkDeviceContext {
 uint64_t mMaxMemoryAllocationSize=100,mAllocationLimitRejects=0,mInjectCount=0;struct {std::mutex m;}mAllocMutex;
 bool mRuntimeAllocs=false,mInjectHard=false;uint64_t mInjectEvery=0;double mLastOomRetryLog=0;
 VkDevice mDevice=(VkDevice)(uintptr_t)1;int relief=0,logs=0;
 void allocLog(const char*){++logs;}bool runRelief(double,uint64_t&){++relief;return false;}
 bool allocationSizeAllowed(uint64_t,const char*);
 VkResult allocateMemory(const VkMemoryAllocateInfo&,VkDeviceMemory*,const char*);
};
'''.replace('@HEADER@',str(header))
tests=r'''
int main(){const uint64_t invalid=std::numeric_limits<uint64_t>::max();
 assert(vkAllocationSizeFits(100,100)&&!vkAllocationSizeFits(101,100)&&vkAllocationSizeFits(invalid,0));
 {VkDeviceContext d;VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=101;VkDeviceMemory out=(VkDeviceMemory)(uintptr_t)1;
 assert(d.allocateMemory(ai,&out,"fixture")==VK_ERROR_OUT_OF_DEVICE_MEMORY&&!out&&driverCalls==0&&d.relief==0&&d.logs==1);
 assert(d.allocateMemory(ai,&out,"fixture")==VK_ERROR_OUT_OF_DEVICE_MEMORY&&d.logs==1&&d.mAllocationLimitRejects==2);
 ai.allocationSize=100;assert(d.allocateMemory(ai,&out,"fixture")==VK_SUCCESS&&driverCalls==1);
 d.mMaxMemoryAllocationSize=0;ai.allocationSize=101;assert(d.allocateMemory(ai,&out,"fixture")==VK_SUCCESS&&driverCalls==2);}
 {Device d;LLCleanRendererVK r(d);assert(r.probeBytes(512,256)==invalid);d.cap=4303355904ull;assert(r.probeBytes(512,256)!=invalid);d.cap=0;assert(r.probeBytes(512,256)!=invalid);}
 {Device d;d.mode=1;d.cap=100;LLCleanRendererVK r(d);assert(r.probeBytes(128,64)==960);assert(d.images.size()==9&&d.buffers.size()==3);
  assert(d.images[4].usage==(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT));
  d.images.clear();d.buffers.clear();r.wantF=3;assert(r.probeBytes(128,64)==1120);assert(d.images.size()==11&&d.images[0].extra&&d.images[0].mips==7&&d.images[0].layers==384);
  d.mode=2;assert(r.probeBytes(128,64)==invalid);d.mode=3;d.cap=0;assert(r.probeBytes(128,64)==invalid);}
 {Device d;LLCleanRendererVK r(d);r.configureProbes(false);assert(r.allocCalls==1&&r.mProbeRes==256&&r.mProbeSlotsN==256&&r.reliefCalls==0);for(auto [res,n,f]:r.attempted){(void)n;(void)f;assert(res!=512);}}
 {Device d;d.mode=4;d.cap=100;LLCleanRendererVK r(d);r.wantR=128;r.wantN=64;r.wantF=1;r.mProbeRad.image=(VkImage)(uintptr_t)1;r.mProbeRes=128;r.mProbeSlotsN=64;r.mProbeFmt=0;waits=0;
  r.configureProbes(true);assert(r.allocCalls==0&&r.destroyCalls==0&&r.reliefCalls==0&&waits==0&&r.mProbeRad.image&&r.mProbeFmt==0);
  assert(r.mProbeFit.valid&&r.mProbeFitFormat==1);r.resizeProbesIfNeeded();assert(r.allocCalls==0&&r.destroyCalls==0&&waits==0);}
 {Device d;d.mode=4;d.cap=100;LLCleanRendererVK r(d);r.configureProbes(false);assert(r.allocCalls==0&&r.destroyCalls==0&&r.reliefCalls==0&&r.absentCalls==1);}
 {Device d;LLCleanRendererVK r(d);r.failAlloc=true;r.configureProbes(true);assert(r.allocCalls>0&&r.allocCalls<=10&&r.reliefCalls==1&&r.absentCalls==1);for(auto [res,n,f]:r.attempted){(void)n;(void)f;assert(res!=512);}}
 std::cout<<"PASS #1081 actual planner/selection/resize/central allocation bodies: exact padding and equality, unknown cap, aggregate versus per-allocation, zero query/overflow, all resource query descriptors, invalid candidate exclusion, retained old-format set, no destructive retry, bounded soft fallback. Synthetic driver/dependencies, no Vulkan device.\n";
}
'''
a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(prefix+fmt+plan+configure+pick+resize+gate+tests)
print('Prepared unchanged actual planner and central-gate bodies; no compilation or GPU.')
