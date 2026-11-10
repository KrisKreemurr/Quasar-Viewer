#!/usr/bin/env python3
"""Validate the actual cache-upgrade predicate with fake format/encoder endpoints."""
import argparse,pathlib,subprocess,resource
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
ap=argparse.ArgumentParser();ap.add_argument('--source-root',type=pathlib.Path,required=True);ap.add_argument('--out',type=pathlib.Path,required=True);a=ap.parse_args();root=a.source_root.resolve();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
s=(root/'indra/newview/vkgfx/llcleanrenderervk.cpp').read_text()
def body(marker):
 i=s.index(marker);b=s.index('{',i);d=1;j=b+1
 while d:
  d+=(s[j]=='{')-(s[j]=='}');j+=1
 return s[b:j]
code=r'''#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include "llvktextureprep.h"
using VkFormat=uint32_t;
constexpr VkFormat VK_FORMAT_UNDEFINED=0,VK_FORMAT_R8G8B8A8_UNORM=37,VK_FORMAT_R8G8B8A8_SRGB=43;
constexpr uint8_t TEXENC_NONE=0,TEXENC_BC1=1,TEXENC_BC3=2,TEXENC_BC5=3,TEXENC_BC4=4,TEXENC_BC7HQ=5,TEXENC_COUNT=6;
VkFormat texEncFormat(uint8_t enc,bool srgb){return enc&&enc<TEXENC_COUNT ? 100u+2u*enc+srgb:0u;}
uint32_t texMipLevels(uint32_t w,uint32_t h){uint32_t n=1;while(std::max(w,h)>1){w=std::max(w/2,1u);h=std::max(h/2,1u);++n;}return n;}
struct SharedTex{uint32_t id=7,image=10,w=64,h=64,slot[2]={1,0},hold=1024,src_w=1024,src_h=1024,full_w=1024,full_h=1024,const_w=0,const_h=0;VkFormat fmt=104;uint8_t enc=TEXENC_BC3,enc_level=4,comps=4,mask_cut=0,cov=0;bool pinned=false,hud=false,bake=false,base_srgb=false,lossy=false,reenc_force=false;};
struct TexStageTicket{uint32_t textureId=7,expectedOld=10,width=512,height=512,sourceWidth=1024,sourceHeight=1024,sourceFullWidth=1024,sourceFullHeight=1024,sourceComps=4,sourceEncoding=TEXENC_BC7HQ,sourceEncodingLevel=4,sourceCoverageCut=0;VkFormat destinationFormat=110;LLVKTexturePrep::SourceKind prepKind=LLVKTexturePrep::SourceKind::CompressedLevels;LLVKTexturePrep::Recipe prepRecipe;};
enum class TexStageDemand{Wanted,Target,Extent,Recipe,Covered,Policy};
struct TexEncPick{uint8_t enc=TEXENC_BC3;};
struct H{uint32_t mTexCompLevel=4;bool cov_on=true;
bool covFlat(const SharedTex& st)const COVFLAT
uint8_t covWant(const SharedTex& st)const COVWANT
bool covStale(const SharedTex& st)const COVSTALE
static uint32_t targetSkip(uint32_t,uint32_t,uint32_t,uint32_t){return 0;}
static uint32_t texBias(uint32_t){return 0;}
static bool texConstCollapse(const SharedTex&,uint32_t,uint32_t){return false;}
static TexEncPick texEncFor(const SharedTex&,uint32_t,uint32_t){return {};}
bool texEncAcceptable(const SharedTex& st)const ACCEPTABLE
TexStageDemand check(const SharedTex&st,const TexStageTicket&ticket) DEMAND
};
void reset(TexStageTicket&t){t={};t.prepRecipe.kind=LLVKTexturePrep::RecipeKind::CopyCompressed;t.prepRecipe.targetWidth=t.width;t.prepRecipe.targetHeight=t.height;t.prepRecipe.targetLevels=texMipLevels(t.width,t.height);t.prepRecipe.payloadLevels=t.prepRecipe.targetLevels;t.prepRecipe.targetStorageFormat=t.destinationFormat;t.prepRecipe.preparedStorageFormat=t.destinationFormat;}
int main(){H h;SharedTex st;TexStageTicket t;reset(t);
// Exact old failure: a complete HQ cache replacement must not match old cheap storage.
assert(h.check(st,t)==TexStageDemand::Wanted);
for(unsigned n=0;n<8;++n){assert(h.check(st,t)==TexStageDemand::Wanted);}
// Matching prefix+resident tail remains legal; format-changing tails never are.
t.prepRecipe.payloadLevels=1;t.prepRecipe.residentTailLevels=t.prepRecipe.targetLevels-1;assert(h.check(st,t)==TexStageDemand::Recipe);
st.enc=TEXENC_BC7HQ;st.fmt=texEncFormat(st.enc,false);assert(h.check(st,t)==TexStageDemand::Wanted);
reset(t);st={};t.prepRecipe.preparedStorageFormat=37;assert(h.check(st,t)==TexStageDemand::Recipe);reset(t);
t.destinationFormat=111;t.prepRecipe.targetStorageFormat=111;t.prepRecipe.preparedStorageFormat=111;assert(h.check(st,t)==TexStageDemand::Recipe);reset(t);
t.sourceEncodingLevel=3;assert(h.check(st,t)==TexStageDemand::Recipe);reset(t);
t.sourceEncoding=TEXENC_BC5;t.destinationFormat=texEncFormat(TEXENC_BC5,false);t.prepRecipe.targetStorageFormat=t.destinationFormat;t.prepRecipe.preparedStorageFormat=t.destinationFormat;assert(h.check(st,t)==TexStageDemand::Recipe);reset(t);
st.mask_cut=128;st.cov=0;t.sourceCoverageCut=128;assert(h.check(st,t)==TexStageDemand::Wanted);
t.sourceCoverageCut=192;assert(h.check(st,t)==TexStageDemand::Wanted);
t.sourceCoverageCut=0;assert(h.check(st,t)==TexStageDemand::Policy);t.sourceCoverageCut=128;
h.cov_on=false;assert(h.check(st,t)==TexStageDemand::Policy);h.cov_on=true;
t.expectedOld=11;assert(h.check(st,t)==TexStageDemand::Target);t.expectedOld=10;
t.width=2048;assert(h.check(st,t)==TexStageDemand::Extent);
std::cout<<"PASS full cached codec replacement, compatible tail, incompatible tail/format/level/policy/identity/extent rejection\n";}
'''
for key,marker in [('COVFLAT','auto covFlat ='),('COVWANT','auto covWant ='),('COVSTALE','auto covStale ='),('ACCEPTABLE','bool LLCleanRendererVK::texEncAcceptable('),('DEMAND','auto texStageDemand =')]:code=code.replace(key,body(marker))
cpp=out/'cache_recipe.cpp';cpp.write_text(code);exe=out/'cache_recipe';subprocess.run(['/usr/bin/c++','-std=c++20','-O0','-Wall','-Wextra','-Werror','-I'+str(root/'indra/newview/vkgfx'),'-I'+str(root/'indra/llrender'),str(cpp),'-o',str(exe)],cwd=out,check=True);subprocess.run([str(exe)],cwd=out,check=True)
