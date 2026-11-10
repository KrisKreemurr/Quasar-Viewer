#!/usr/bin/env python3
"""Run the actual GLSL lattice inverse against producer raster selection."""
import argparse
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=Path)
parser.add_argument('output', type=Path)
a = parser.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
s = (a.source/'indra/vkclean/shaders/clean_pdn_common.glsl').read_text()
start = s.index('vec2 pdnSampleGridHc(')
brace = s.index('{', start)
end = brace+1
depth=1
while depth:
    depth += (s[end]=='{')-(s[end]=='}')
    end += 1
function=s[start:end].replace('max(', 'scalarMax(').replace('clamp(', 'scalarClamp(')
# C++ float arithmetic matches GLSL's scalar type, including decimal literals.
function=re.sub(r'(?<![\w.])(\d+\.\d+|\d+e-\d+)(?![\w.])',r'\1f',function)
fixture=r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
struct vec2 {float v[2];vec2(float x=0,float y=0):v{x,y}{}float& operator[](int i){return v[i];}float operator[](int i)const{return v[i];}};
float scalarMax(float a,float b){return std::max(a,b);}
float scalarClamp(float x,float a,float b){return std::clamp(x,a,b);}
using std::floor;
float halton(uint32_t index,uint32_t base){float sum=0,scale=1;while(index){scale/=float(base);sum+=scale*float(index%base);index/=base;}return sum;}
'''
checks=r'''
int main(){
 uint64_t cases=0;float largest=0;double legacyError=0;
 for(int den: {17,31,393,786,1280,1572,2560})for(int extent: {den,2*den,2*den-1}){
  const float r=float(extent)/float(den);
  for(uint32_t phase=1;phase<=128;++phase){
   const float j=halton(phase,2)-0.5f;
   const float jy=halton(phase,3)-0.5f;
   const float grid=extent>den?j:0;
   const float gridY=extent>den?jy:0;
   for(int k=0;k<den;++k){
    const float px=floor((float(k)+0.5f+grid)*r)+0.5f;
    const float py=floor((float(k)+0.5f+gridY)*r)+0.5f;
    // This is exactly the previous receiver's stable projection. Temporal adds
    // the previous raster phase back before inverting the selected-pixel grid.
    const float stable=px-j,stableY=py-jy;
    const vec2 result=pdnSampleGridHc(vec2(stable+j,stableY+jy),vec2(r,r),vec2(grid,gridY));
    const float error=std::max(std::abs(result[0]-float(k)),std::abs(result[1]-float(k)));
    largest=std::max(largest,error);assert(error<0.001f);
    legacyError+=std::abs(stable/r-0.5f-float(k));++cases;
   }
  }
 }
 // A static, non-jittered half-res lattice picks centres 1.5,3.5,..., not 1,3,...
 const vec2 corrected=pdnSampleGridHc(vec2(21.5f,21.5f),vec2(2,2),vec2(0,0));
 assert(corrected[0]==10);assert(21.5f/2-0.5f==10.25f);
 // No coordinate clamping may manufacture an in-frame history on a camera cut.
 const vec2 outside=pdnSampleGridHc(vec2(-10,-10),vec2(2,2),vec2(0,0));assert(outside[0]<0);
 std::printf("actual GLSL inverse: %llu receiver/phase cases PASS; max %.8g texels; legacy mean drift %.6g\n",(unsigned long long)cases,largest,legacyError/double(cases));
}
'''
path=a.output/'sample_grid.cpp';path.write_text(fixture+function+'\n'+checks)
exe=a.output/'sample_grid'
subprocess.run(['c++','-std=c++20','-O2','-Wall','-Wextra','-Werror',str(path),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
# Temporal-only reuse of ctl.zw must never reach the spatial iteration loop or GI.
common=(a.source/'indra/vkclean/shaders/clean_dn_common.glsl').read_text()
temp=(a.source/'indra/vkclean/shaders/clean_dn_temporal.comp').read_text()
assert 'uintBitsToFloat(dp.ctl.zw)' in common
assert temp.count('dnSampleHistoryHc(')==3
assert '!dnSampleGridHistory() && dnRadiance()' in temp
assert '!dnSampleGridHistory() && (dp.ctl.x & DN_F_SUN_EXACT)' in temp
assert 'dp.ctl.w' not in temp and 'dp.ctl.z' not in temp
host=(a.source/'indra/newview/vkgfx/llcleandenoisevk.cpp').read_text()
assert 'std::memcpy(&q.ctl[2u + axis], &phase' in host
assert 'mPreviousSampleJitter[0] = p.rasterJitter[0]' in host
settings=(a.source/'indra/newview/app_settings/settings.xml').read_text()
setting=settings.split('<key>RenderVKShadowSampleGridHistory</key>',1)[1].split('</map>',1)[0]
assert '<key>Persist</key><integer>0</integer>' in setting
print('temporal-only ctl reuse, old-path gating, transient A/B: PASS')

signature='void LLCleanDenoiserVK::recordTemporal('
start=host.index(signature);brace=host.index('{',start);end=brace+1;depth=1
while depth:
    depth+=(host[end]=='{')-(host[end]=='}');end+=1
record=host[start:end].replace('LLCleanDenoiserVK::','TestDenoiser::')
harness=r'''
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cstdio>
using VkCommandBuffer=uint64_t;using VkDescriptorSet=uint64_t;
constexpr uint32_t VK_PIPELINE_BIND_POINT_COMPUTE=1,VK_SHADER_STAGE_COMPUTE_BIT=1;
struct Push {float dims[4]{},prm0[4]{},prm1[4]{};uint32_t ctl[4]{};float sun0[4]{},sun1[4]{};};
static_assert(sizeof(Push)==96);
Push captured;uint32_t dispatches=0;
void dnComputeBarrier(uint64_t){}
void dnSplitRawSnapshot(uint64_t,uint64_t,uint32_t,uint32_t,uint32_t){}
void vkCmdBindDescriptorSets(uint64_t,uint32_t,uint64_t,uint32_t,uint32_t,const uint64_t*,uint32_t,const uint32_t*){}
void vkCmdPushConstants(uint64_t,uint64_t,uint32_t,uint32_t,uint32_t size,const void* data){assert(size==sizeof(Push));std::memcpy(&captured,data,size);}
void vkCmdBindPipeline(uint64_t,uint32_t,uint64_t){}
void vkCmdDispatch(uint64_t,uint32_t,uint32_t,uint32_t){++dispatches;}
#define VK_PUSH_BUDGET_CHECK(Q) static_assert(sizeof(Q)==96)
struct TestDenoiser {
 struct Params {bool exactSpecular=false,specularLobeGuard=false,sampleGridHistory=false,sun=false,sunNoResolve=false;float rasterJitter[2]{};uint32_t frame=0,debug=0,casterHistoryPolicy=2;bool shadowPlaneReconstruction=false;};
 bool ready=true,mSun=true,mSunNoResolveKnown=false,mSunNoResolve=false;
 bool mSunHistOk=true,mHistOk=true,mSampleGridKnown=false,mSampleGridHistory=false,mSpecularLobeGuard=false,mExactSpecular=false,mSplit=false,mRan=false,mSunRan=false;
 uint32_t mCasterHistoryPolicy=2;bool mCasterCauseDebug=false,mShadowPlaneReconstruction=false;
 uint32_t mW=32,mH=32,mParity=0;uint64_t mDset=1,mLayout=1,mTempPipe=1;
 struct {uint64_t image=1;}mGeom;
 float mPreviousSampleJitter[2]{};
 bool pipelinesReady()const{return ready;}
 bool hasSun()const{return mSun;}
 Push makePush(uint32_t,uint32_t,const Params& p)const{Push q{};q.ctl[0]=mHistOk?1u:0u;q.ctl[2]=3;q.ctl[3]=p.frame;return q;}
 void recordTemporal(VkCommandBuffer,VkDescriptorSet,uint32_t,uint32_t,const Params&);
};
uint32_t phaseBits(float phase){uint32_t value;std::memcpy(&value,&phase,4);return value;}
'''
checks=r'''
int main(){
 TestDenoiser d;TestDenoiser::Params p;
 p.frame=73;p.sampleGridHistory=true;p.rasterJitter[0]=.25f;p.rasterJitter[1]=-.375f;
 d.mSampleGridKnown=true;d.mSampleGridHistory=false;
 d.recordTemporal(1,1,64,64,p);
 assert(!d.mHistOk&&!d.mSunHistOk);assert(captured.ctl[1]&16u);
 assert(captured.ctl[2]==phaseBits(0)&&captured.ctl[3]==phaseBits(0));
 d.mHistOk=d.mSunHistOk=true;p.rasterJitter[0]=-.25f;p.rasterJitter[1]=.125f;
 d.recordTemporal(1,1,64,64,p);
 assert(d.mHistOk&&d.mSunHistOk);assert(captured.ctl[2]==phaseBits(.25f)&&captured.ctl[3]==phaseBits(-.375f));
 const uint32_t count=dispatches;d.ready=false;p.rasterJitter[0]=.49f;
 d.recordTemporal(1,1,64,64,p);assert(dispatches==count);assert(d.mPreviousSampleJitter[0]==-.25f);
 d.ready=true;p.sampleGridHistory=false;d.recordTemporal(1,1,64,64,p);
 assert(!d.mHistOk&&!d.mSunHistOk);assert(!(captured.ctl[1]&16u));assert(captured.ctl[2]==3&&captured.ctl[3]==73);
 TestDenoiser gi;TestDenoiser::Params normal;normal.frame=91;gi.recordTemporal(1,1,64,64,normal);
 assert(gi.mHistOk);assert(!(captured.ctl[1]&16u));assert(captured.ctl[2]==3&&captured.ctl[3]==91);
 // A live representation switch cannot blend old affine-Z metadata with new
 // reciprocal-depth metadata; an unchanged policy must keep history.
 TestDenoiser plane;TestDenoiser::Params pp;
 plane.mSampleGridKnown=plane.mSampleGridHistory=true;pp.sampleGridHistory=true;
 pp.shadowPlaneReconstruction=true;plane.recordTemporal(1,1,64,64,pp);
 assert(!plane.mHistOk&&!plane.mSunHistOk);
 plane.mHistOk=plane.mSunHistOk=true;plane.recordTemporal(1,1,64,64,pp);
 assert(plane.mHistOk&&plane.mSunHistOk);
 pp.shadowPlaneReconstruction=false;plane.recordTemporal(1,1,64,64,pp);
 assert(!plane.mHistOk&&!plane.mSunHistOk);
 std::puts("actual recordTemporal: phase publication, toggle invalidation, unready retention, GI isolation PASS");
}
'''
path=a.output/'grid_host.cpp';path.write_text(harness+record+'\n'+checks)
exe=a.output/'grid_host'
subprocess.run(['c++','-std=c++20','-O2','-Wall','-Wextra','-Werror',str(path),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)

renderer=(a.source/'indra/newview/vkgfx/llcleanrenderervk.cpp').read_text()
producer=(a.source/'indra/newview/vkgfx/llcleanprojstochvk.cpp').read_text()
lighting=(a.source/'indra/vkclean/shaders/clean_deferred_light.comp').read_text()
assert 'mPdnSampleGridHistory = sampleGridHistory;' in renderer
assert '(sampleGridHistory ? 4u : 0u)' in renderer
assert 'dp.sampleGridHistory = mPdnSampleGridHistory;' in producer
assert lighting.count('pdnSampleGridHc(CLEAN_FRAG_XY, 1.0 / r, j)') == 2
print('same-frame latched producer/consumer flag and both upsample paths: PASS')
# The cause view may alter obsolete compatibility bits, never authoritative ages,
# temporal colours, or the spatial feedback path it is trying to diagnose.
atrous=(a.source/'indra/vkclean/shaders/clean_dn_atrous.comp').read_text()
assert atrous.count('PDN_DBG_HISTORY_CAUSE')==1
assert atrous.index('PDN_DBG_HISTORY_CAUSE') > atrous.index('// (DN_F_RADIANCE) YOUNG-HISTORY FEEDBACK')
assert 'PDN_DBG_HISTORY_CAUSE' not in temp[temp.index('    vec3 outD = accD;'):]
assert 'finalMeta = (finalMeta & ~7u) | causes;' in temp
uses=sum(len(re.findall(r'\bpdnMetaLen[DS]\s*\(',p.read_text())) for p in (a.source/'indra/vkclean/shaders').glob('*') if p.suffix in ('.glsl','.comp','.frag','.vert'))
assert uses==2, 'compatibility duration bits gained a reader: move diagnostic payload first'
assert 'dbg <= 12u' in producer
print('history-cause view preserves colour/feedback; compatibility payload has no readers: PASS')
