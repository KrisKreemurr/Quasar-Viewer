#!/usr/bin/env python3
"""Source-bound MASK/BLEND surface contract: positions and atomic guide admission.
Usage: script <external-output-dir>. CPU only; no viewer or Vulkan driver.
"""
from pathlib import Path
import re
import subprocess
import sys
root=Path(__file__).resolve().parents[3];out=Path(sys.argv[1]).resolve();out.mkdir(parents=True,exist_ok=True)
def body(s,signature):
 a=s.index(signature);b=s.index('{',a)+1;depth=1
 while depth:
  if s[b]=='{':depth+=1
  elif s[b]=='}':depth-=1
  b+=1
 return s[a:b]
shader=(root/'indra/vkclean/shaders/clean_cluster_draw_common.glsl').read_text()
pos=body(shader,'ClVOut clusterSkinnedVertex(').split('#if !defined(CLEAN_DEPTH_ONLY)')[0]
pos+='    (void)sel;(void)cs;(void)inst;(void)position;(void)normal;(void)texcoord0;return o;\n}\n'
select=(root/'indra/newview/vkgfx/llcleanclusterselectvk.cpp').read_text()
ready=body(select,'bool LLCleanClusterSelectVK::alphaDrawReady(').replace('LLCleanClusterSelectVK::','Fixture::')
record=body(select,'bool LLCleanClusterSelectVK::recordAlphaDraw(')
host=(root/'indra/newview/vkgfx/llcleanrenderervk.cpp').read_text()
# These source assertions fail on the reported old branch: its CLOD blend list
# enters neither early motion/depth nor G-buffer, and always enters late depth.
for call in ['CLPASS_ALPHA_PREPASS, 0u','CLPASS_ALPHA_PREPASS, 1u','CLPASS_ALPHA_GBUFFER, 0u','CLPASS_ALPHA_GBUFFER, 1u']:
 assert call in host, call
assert '!mClBlendPreNow && clusterDrawAlpha(cmd, CLPASS_ALPHA_DEPTH' in host
assert 'pass == CLPASS_ALPHA && mClBlendGbufNow' in host
assert 'ci.pre_variant = pre_variant' in host
assert 'in.view | ((in.pass == CLPASS_ALPHA || in.pass == CLPASS_ALPHA_DEPTH) ? in.flags : 0u)' in record
assert 'mAlphaSurfacePre[t][in.pre_variant]' in record and 'mAlphaSurfaceGbuf[t]' in record
# (bit 31 the near-opaque skip, bit 30 the avatar alpha-depth pre-pass: neither is the view)
assert 'g_draw_push.pad0 & 0x3fffffffu' in (root/'indra/vkclean/shaders/clean_cluster_draw.task').read_text()
# Both task/mesh flavours use this exact shared position producer; no alpha
# branch or alpha mode is present in its geometry expression.
assert 'clusterVertex(sel, t,' in (root/'indra/vkclean/shaders/clean_cluster_draw.mesh').read_text()
assert 'const uint s = clSkinnedCluster(sel)' in shader
cpp=r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
using uint=uint32_t;using std::min;
struct vec2{float x,y;};struct vec3{float x,y,z;};struct vec4{float x,y,z,w;vec4(vec3 v,float a):x(v.x),y(v.y),z(v.z),w(a){}};
vec3 operator+(vec3 a,vec3 b){return{a.x+b.x,a.y+b.y,a.z+b.z};}
vec3 operator-(vec3 a,vec3 b){return{a.x-b.x,a.y-b.y,a.z-b.z};}
struct M3{vec3 operator*(vec3 v)const{return{-v.y,v.x,v.z};}};
struct M4{vec4 operator*(vec4 v)const{return{{v.x*1.5f,v.y*.75f,v.z*.5f+v.w*.25f},v.w};}};
struct Origin{vec3 xyz;};struct Push{M4 u_view_proj;Origin u_origin{{2048.125f,1024.5f,73.f}};}pc;
struct ClVOut{uint receiver_class;vec4 pos{{0,0,0},0};};
struct ClSelected{uint instance;};struct Instance{uint flags;};
struct CI{uint flags;};std::array<CI,16>cl_instances;
struct ClChar{M3 m;vec3 t;};constexpr uint CL_MAX_CLUSTER_VERTS=64;
uint clSkinRow(uint flags){return(flags>>16)&1023;}
ClChar clSkinChar(uint row,bool previous){assert(row==5&&!previous);return{M3{},vec3{.125f,-.25f,.375f}};}
vec3 clSkinEye(){return{2048.25f,1024.75f,72.875f};}
std::array<vec3,1024>cl_skin_out;
vec3 SV_POS(const std::array<vec3,1024>&a,uint v){return a[v];}
'''+pos+r'''
using VkPipeline=int;constexpr int VK_NULL_HANDLE=0;
enum LLClusterDrawPass{CLPASS_ALPHA,CLPASS_ALPHA_DEPTH,CLPASS_TRANS_DEPTH,CLPASS_TRANS_COLOR,CLPASS_SHADOW,CLPASS_PROJ,CLPASS_PROBE,CLPASS_ALPHA_PREPASS,CLPASS_ALPHA_GBUFFER};
struct Fixture{
 bool mReady=true;uint mAlphaOrderFrame=100,mFrame=100,mMainView=0,mViewCount=1;
 struct View{bool valid=true;};View mViewRecs[1];
 VkPipeline mAlphaSurfaceGbuf[3]{1,1,1};VkPipeline mAlphaSurfacePre[3][4]{{1,1,1,1},{1,1,1,1},{1,1,1,1}};
 VkPipeline mAlphaPipe[3][2]{{1,1},{1,1},{1,1}},mAlphaDepthPipe[3]{1,1,1},mTransPipe[3][2]{{1,1},{1,1},{1,1}},mCapAlphaPipe[3]{1,1,1};
 int32_t drawTier()const{return 2;}bool passReady(LLClusterDrawPass)const{return true;}bool rigListShared(int)const{return true;}
 bool alphaDrawReady(LLClusterDrawPass)const;
};
'''+ready+r'''
int main(){
 // Two faces share a welded boundary, but occupy DIFFERENT selected entries/slots.
 cl_instances[7].flags=(5u<<16)|2048u|1u;cl_instances[9].flags=cl_instances[7].flags|8192u;
 for(uint k=0;k<64;++k){cl_skin_out[2*64+k]={float(k)/16.f,-.25f,float(k)*.0125f};cl_skin_out[3*64+k]=cl_skin_out[2*64+k];}
 for(uint k=0;k<64;++k){
  auto mask=clusterSkinnedVertex(7,{7},{4},2,k,{0,0,0},{0,0,1},{0,0});
  auto blend=clusterSkinnedVertex(9,{9},{2},3,k,{0,0,0},{0,0,1},{0,0});
  assert(std::memcmp(&mask.pos,&blend.pos,sizeof(mask.pos))==0);
 }
 Fixture f;assert(f.alphaDrawReady(CLPASS_ALPHA_PREPASS)&&f.alphaDrawReady(CLPASS_ALPHA_GBUFFER));
 for(uint pv=0;pv<4;++pv){f.mAlphaSurfacePre[2][pv]=0;assert(!f.alphaDrawReady(CLPASS_ALPHA_PREPASS)&&!f.alphaDrawReady(CLPASS_ALPHA_GBUFFER));f.mAlphaSurfacePre[2][pv]=1;}
 f.mAlphaSurfaceGbuf[2]=0;assert(!f.alphaDrawReady(CLPASS_ALPHA_PREPASS));f.mAlphaSurfaceGbuf[2]=1;
 f.mAlphaOrderFrame=99;assert(!f.alphaDrawReady(CLPASS_ALPHA_PREPASS));
 // High skip bit must not select the wrong cluster view or discard soft fringes.
 for(uint view=0;view<8;++view)assert(((view|0x80000000u|0x40000000u)&0x3fffffffu)==view&&((view|0x80000000u)&0x3fffffffu)==view);
 std::cout<<"PASS production skinned positions MASK/BLEND bit-identical; coupled pre/G-buffer readiness; all guide variants and view/skip-bit contract\n";
}
'''
p=out/'checks.cpp';p.write_text(cpp)
subprocess.run(['c++','-std=c++20','-O2','-Wall','-Wextra','-Werror',str(p),'-o',str(out/'checks')],check=True)
subprocess.run([str(out/'checks')],check=True)
