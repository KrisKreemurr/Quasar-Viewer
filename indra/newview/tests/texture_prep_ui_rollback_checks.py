#!/usr/bin/env python3
"""Generate actual UI create/rollback bodies with a fake GPU/queue boundary."""
import argparse,runpy
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('source',type=Path);p.add_argument('output',type=Path);a=p.parse_args();root=a.source.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
extract=runpy.run_path(str(root/'indra/newview/tests/texture_prep_ui_format_checks.py'))['extract_braced']
s=(root/'indra/newview/vkgfx/llrendervk/llrendervk.cpp').read_text()
actual='\n'.join(extract(s,marker) for marker in ('uint32_t LLRenderVK::createTexture(', 'uint32_t LLRenderVK::createPreparedTexture('))
cpp=r'''#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#define REQUIRE(x) do{if(!(x)){std::fprintf(stderr,"%d: %s\n",__LINE__,#x);std::abort();}}while(false)
using VkDeviceSize=uint64_t;
using VkBuffer=uint64_t;
using VkCommandBuffer=uint64_t;
constexpr uint64_t VK_NULL_HANDLE=0;
constexpr int VKMEM_UI=0,TF_POINT=1,TF_BILINEAR=2;
struct VkMemScope{explicit VkMemScope(int){}};
namespace llrvk{enum eTexelFormat{TXF_RGBA8};uint32_t texelBytes(eTexelFormat){return 4;}}
using namespace llrvk;
struct Image{uint64_t image=0,view=0;};
struct Context{struct SoftAllocScope{explicit SoftAllocScope(Context*){}};struct NoReliefScope{};unsigned live=0,destroyed=0;void destroyImage(Image& image){REQUIRE(image.image);--live;++destroyed;image={};}};
using VkDeviceContext=Context;
class LLRenderVK {
public:
 using eTexelFormat=llrvk::eTexelFormat;
 struct VkTexture{Image image{};uint64_t view=0,dset=0;uint32_t id=0;int defaultFilter=0;llrvk::eTexelFormat texFmt=TXF_RGBA8;};
 struct PendingUpload{uint64_t image=0;uint32_t w=0,h=0;};
 struct PreparedUploadToken{uint32_t generation=1;uint16_t slot=0,pad=0;};
 Context* mCtx=nullptr;std::vector<VkTexture>mTextures;std::vector<uint32_t>mFreeTexIds;bool accept=false;
 Image createSampledImage(uint32_t,uint32_t,eTexelFormat){++mCtx->live;return{mCtx->live+10,1};}
 uint8_t*allocStaging(VkDeviceSize,PendingUpload&){return nullptr;}
 void submitUpload(const PendingUpload&){}
 void destroyTexture(uint32_t id){if(mTextures[id].image.image)mCtx->destroyImage(mTextures[id].image);}
 bool queuePreparedTextureUpdate(uint32_t,uint32_t,uint32_t,eTexelFormat,VkBuffer,VkDeviceSize,VkDeviceSize,PreparedUploadToken){return accept;}
 uint32_t createTexture(uint32_t,uint32_t,const uint8_t*,bool,eTexelFormat);
 uint32_t createPreparedTexture(uint32_t,uint32_t,llrvk::eTexelFormat,VkBuffer,VkDeviceSize,VkDeviceSize,PreparedUploadToken);
};
ACTUAL
int main(){
 Context context;LLRenderVK renderer;renderer.mCtx=&context;renderer.mTextures.resize(1);
 for(unsigned n=0;n<10000;++n){REQUIRE(!renderer.createPreparedTexture(4,4,TXF_RGBA8,1,0,64,{}));REQUIRE(renderer.mTextures.size()==1);REQUIRE(renderer.mFreeTexIds.empty());REQUIRE(!context.live);}
 renderer.mTextures.resize(4);renderer.mFreeTexIds={1,2};const auto freeCapacity=renderer.mFreeTexIds.capacity();
 for(unsigned n=0;n<10000;++n){REQUIRE(!renderer.createPreparedTexture(4,4,TXF_RGBA8,1,0,64,{}));REQUIRE(renderer.mTextures.size()==4);REQUIRE(renderer.mFreeTexIds.size()==2);REQUIRE(renderer.mFreeTexIds.back()==2);REQUIRE(renderer.mFreeTexIds.capacity()==freeCapacity);REQUIRE(!context.live);}
 renderer.mFreeTexIds={1,3};REQUIRE(!renderer.createPreparedTexture(4,4,TXF_RGBA8,1,0,64,{}));REQUIRE(renderer.mTextures.size()==3);REQUIRE(renderer.mFreeTexIds.size()==1);REQUIRE(!context.live);
 renderer.accept=true;const auto id=renderer.createPreparedTexture(4,4,TXF_RGBA8,1,0,64,{});REQUIRE(id==1);REQUIRE(context.live==1);renderer.destroyTexture(id);REQUIRE(!context.live);
 std::puts("actual prepared UI create/ID rollback: PASS (20001 refusals)");
}
'''.replace('ACTUAL',actual)
(out/'checks.cpp').write_text(cpp)
print(out/'checks.cpp')
