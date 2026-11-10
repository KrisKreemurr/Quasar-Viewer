#!/usr/bin/env python3
"""Extract real SSS host methods into a narrow command-recording CPU fixture."""
from pathlib import Path
import subprocess
import sys
import xml.etree.ElementTree as ET
root,out=map(Path,sys.argv[1:3]);out.mkdir(parents=True,exist_ok=True)
host=(root/'indra/newview/vkgfx/llcleanrenderervk.cpp').read_text()
def method(name):
 start=host.index(name);start=host.rfind('\n',0,start)+1
 pos=host.index('{',start);depth=1;end=pos+1
 while depth:
  if host[end]=='{':depth+=1
  elif host[end]=='}':depth-=1
  end+=1
 return host[start:end]
code=r'''
#define VK_NO_PROTOTYPES
#define LL_HAS_CLEAN_PIPELINE
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
using U32=uint32_t;using F32=float;
struct Settings{bool on=true;float scale=1;U32 debug=0;} gSavedSettings;
template<class T>struct LLCachedControl{
 std::string name;T fallback;
 LLCachedControl(Settings&,const char* n,T f):name(n),fallback(f){}
 operator T()const{
  if(name=="RenderVKSkinSSS")return T(gSavedSettings.on);
  if(name=="RenderVKSkinSSSScale")return T(gSavedSettings.scale);
  if(name=="RenderVKDebugView")return T(gSavedSettings.debug);
  return fallback;
 }
};
float llclamp(float v,float lo,float hi){return std::clamp(v,lo,hi);}
struct VKDeferPush{uint32_t flags=0,width=0,height=0;};
std::vector<uint32_t> pushes;unsigned dispatches=0;
void vkCmdPipelineBarrier2(VkCommandBuffer,const VkDependencyInfo*){}
void vkCmdBindPipeline(VkCommandBuffer,VkPipelineBindPoint,VkPipeline){}
void vkCmdDispatch(VkCommandBuffer,uint32_t,uint32_t,uint32_t){++dispatches;}
void vkCmdPushConstants(VkCommandBuffer,VkPipelineLayout,VkShaderStageFlags,uint32_t,uint32_t,const void* p){pushes.push_back(static_cast<const VKDeferPush*>(p)->flags);}
struct LLCleanRendererVK{
 VkPipeline mSkinSssPipe=reinterpret_cast<VkPipeline>(1);
 VkPipelineLayout mDeferSplitLayout{};
 bool mDsSssFull=true,stereo=false;uint32_t mDsW=128,mDsH=64,mViewCount=1;
 bool stereoActive()const{return stereo;}
 void gpuSubMark(VkCommandBuffer,const char*){}
 bool skinSssWanted()const;
 void recordSkinSss(VkCommandBuffer,const VKDeferPush&,uint32_t,uint32_t,bool);
};
'''+method('bool LLCleanRendererVK::skinSssWanted() const')+'\n'+method('void LLCleanRendererVK::recordSkinSss(')+r'''
int main(){
 LLCleanRendererVK renderer;VKDeferPush p{0x12345,128,64};
 for(U32 debug=31;debug<=33;++debug){
  dispatches=0;pushes.clear();gSavedSettings={false,0,debug};
  assert(renderer.skinSssWanted());renderer.recordSkinSss({},p,128,64,true);
  assert(dispatches==1&&pushes.size()==2);
  assert((pushes[0]>>16)==debug-30&&(pushes[0]&65535)==0);
  assert(pushes[1]==p.flags);
 }
 dispatches=0;gSavedSettings={true,0,0};renderer.recordSkinSss({},p,128,64,false);assert(dispatches==0);
 gSavedSettings={true,4,32};renderer.recordSkinSss({},p,128,64,false);assert((pushes[pushes.size()-2]&65535)==4000);
 renderer.stereo=true;assert(!renderer.skinSssWanted());
 renderer.stereo=false;renderer.mDsSssFull=false;dispatches=0;renderer.recordSkinSss({},p,128,64,false);assert(dispatches==0);
}
'''
(out/'check.cpp').write_text(code)
subprocess.run(['c++','-std=c++20','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers',str(out/'check.cpp'),'-o',str(out/'check')],check=True)
subprocess.run([str(out/'check')],check=True)
shader=(root/'indra/vkclean/shaders/clean_deferred_light.comp').read_text()
sss=shader.split('// ---- SKIN SUBSURFACE SCATTERING (17;',1)[1].split('#elif CLEAN_DEFER_PASS == 7',1)[0]
assert 'abs(change) * 16.0' in sss and 'debug == 3u' in sss and 'sssDebugStore(px, S0)' in sss
assert 'debug == 1u' in sss and 'sssDebugStore(px, vec3(1.0))' in sss
packing=(root/'indra/vkclean/shaders/clean_gbuffer.glsl').read_text()
feed=(root/'indra/newview/llvovolume.cpp').read_text()
assert '(s.cel ? GB_VAR_CEL : ((s.skin && lit) ? GB_VAR_SKIN' in packing
assert '(inst.flags & (8u | 4096u)) == 0u' in feed
assert '(inst.flags & (2u | 8u | 4096u)) == 0u' not in feed[feed.index('// SKIN (RenderVKSkinSSS'):feed.index('// SKIN (RenderVKSkinSSS')+1000]
# Material admission: preserve blend bit2; continue excluding fullbright/HUD.
for flags in [0,2,8,4096,2|8,2|4096]:
    old=(flags & (2|8|4096))==0
    new=(flags & (8|4096))==0
    assert (new!=old)==(flags==2)
assert '(dbgview >= 31u && dbgview <= 33u)' in host
assert '(dv >= 31u && dv <= 33u)' in host
assert '((U32)s_dbgview >= 31u && (U32)s_dbgview <= 33u)' in host
ET.parse(root/'indra/newview/app_settings/settings.xml')
print('PASS actual host methods:31..33 force allocation/export, zero-scale dispatch, restored push, scale encode, unready/stereo gates; shader channels/split route/XML')
