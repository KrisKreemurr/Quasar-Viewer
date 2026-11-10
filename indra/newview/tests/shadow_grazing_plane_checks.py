#!/usr/bin/env python3
"""Production depth-plane/gather helpers on a grazing glossy floor. CPU, no GPU claim.
Uses system GLM headers (or supply -I via CPLUS_INCLUDE_PATH).
Usage: shadow_grazing_plane_checks.py SOURCE_ROOT OUTPUT_DIR
"""
from pathlib import Path
import re,subprocess,sys
root,out=map(Path,sys.argv[1:3]);out.mkdir(parents=True,exist_ok=True)
sh=root/'indra/vkclean/shaders'
lighting=(sh/'clean_deferred_light.comp').read_text()
def extract(src,signature):
 start=src.index(signature);a=src.index('{',start);end=a+1;depth=1
 while depth:
  depth+=(src[end]=='{')-(src[end]=='}');end+=1
 return src[start:end]
plane=(sh/'clean_pdn_plane.glsl').read_text();common=(sh/'clean_pdn_common.glsl').read_text()
funcs='\n'.join(extract(plane,n) for n in ['float pdnPlaneSlope(', 'float pdnPlaneDepth(', 'vec2 pdnPlaneRaster(', 'vec2 pdnPlaneRasterBetween(', 'float pdnPlaneDepthMismatch('])
funcs+='\n'+extract(common,'vec2 pdnSampleGridHc(')
funcs+='\n'+extract(lighting,'float pdnGrad(')+'\n'+extract(lighting,'float deferPdnPlaneDepth(')
funcs+='\n'+extract((sh/'clean_upsample_common.glsl').read_text(),'vec4 upsampleWeights4(')
funcs+='\n'+extract((sh/'clean_brdf.glsl').read_text(),'void llPunctual(')
funcs+='\n'+extract((sh/'clean_history_time.glsl').read_text(),'float htAlpha(')
funcs=re.sub(r'\b(?:out|inout)\s+(\w+)\s+(\w+)',r'\1& \2',funcs)
funcs=re.sub(r'((?:\b\w+\.)*\b\w+)\.xyz\b',r'vec3(\1)',funcs)
funcs=re.sub(r'(?<![\w.])(\d+\.\d+|\d+e-\d+)(?![\w.])',r'\1f',funcs)
program=r'''
#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/packing.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
using namespace glm;
using std::isinf;
constexpr float PI=3.14159265f;
float htStep(){return 1.f;}
struct {vec4 cam_pos{},cam_fwd{};uvec4 fwd{};} linfo;
struct {uint width=320,height=180;} g_draw_push;
struct {vec4 u_cam_time{};} pc;
mat4 deferInvVP,baseProjection;
float rasterJitter=0, horizon=90, K=230.4f;
float viewZ(float rasterY){float y=rasterY-rasterJitter-horizon;return y>0?K/y:-1.f;}
float pdnViewZ(ivec2 q){float z=viewZ(float(q.y)+.5f);if(z<=0)return -1.f;float d=.1f/z;return .1f/d;}
float half(float v){return unpackHalf2x16(packHalf2x16(vec2(v,0))).x;}
float halton(uint32_t i,uint32_t base){float sum=0,scale=1;while(i){scale/=float(base);sum+=scale*float(i%base);i/=base;}return sum;}
''' +funcs+r'''
struct Summary {uint none=0,partial=0,total=0;double support=0,noiseSquared=0;};
// Identical 4-tap temporal depth predicate; normals are exactly equal on this floor.
Summary temporal(float nominalDistance,float ratio,bool plane){
 Summary result;float previousJ=0;linfo.fwd.z=plane?16u:0u;
 int center=int(std::floor((horizon+K/nominalDistance)/ratio));
 for(uint f=1;f<=256;++f){
  float currentJ=halton(f,2)-.5f, phase=ratio>1?currentJ:0;
  float selected=pdnPlaneRaster(vec2(0,center),vec2(ratio),vec2(0,phase)).y;
  float stableY=selected-currentJ;float pw=K/(stableY-horizon);
  if(!(pw>0))continue;
  float previousPhase=ratio>1?previousJ:0;
  float target=stableY+previousJ;
  vec2 hc=pdnSampleGridHc(vec2(.5f,target),vec2(1,ratio),vec2(0,previousPhase));
  int b=int(std::floor(hc.y));float frac=hc.y-float(b);double support=0,possible=0;
  int step=int(std::max(std::floor(ratio+.5f),1.f));rasterJitter=previousJ;
  for(int dy=0;dy<2;++dy){float weight=dy?frac:1-frac;if(weight<=1e-4f)continue;
   int q=b+dy;if(q<0||q>=int(180/ratio))continue;
   float qp=pdnPlaneRaster(vec2(0,q),vec2(1,ratio),vec2(0,previousPhase)).y;
   float z=pdnViewZ(ivec2(0,int(qp)));if(!(z>0))continue;
   possible+=weight;
   float slope=half(pdnGrad(ivec2(0,int(qp)),ivec2(0,step),z));
   float predicted=plane?pdnPlaneDepth(z,vec2(0,slope),vec2(0,(target-qp)/float(step)),false):z;
   if(predicted>0 && std::abs(predicted-pw)<=std::max(.04f*pw,.03f))support+=weight;
  }
  if(possible>.999){++result.total;result.support+=support;
   if(support<.001)++result.none;
   if(support<.999)++result.partial;
   // Actual production GGX BRDF on a glossy floor, soft point-light visibility
   // represented by the same one-ray Bernoulli samples in both policies.
   // Start each phase from settled history: isolate the admission stage, not RNG.
   vec3 point(0,pw,0),light(0,nominalDistance*2.25f,2.f);
   vec3 n(0,0,1),v=normalize(vec3(pc.u_cam_time)-point),l=normalize(light-point);
   float nl;vec3 df,sp;llPunctual(vec3(.5f),vec3(.04f),.05f,n,v,l,nl,df,sp);
   float glint=glm::min(nl*sp.x,10.f),expected=.5f*glint;
   float raw=(f&1u)?glint:0.f;
   float alpha=support>=.001?htAlpha(.25f):1.f;
   float output=mix(expected,raw,alpha);
   assert(glint>0 && std::isfinite(glint));
   result.noiseSquared+=std::pow(double((output-expected)/expected),2.);
  }
  previousJ=currentJ;
 }
 return result;
}
// Stage-isolation: local glossy lobe's affine neighbourhood, sampled into the
// reduced shadow radiance buffer. All visibility is static: only reconstruction
// weights may change the error. FP16 final guide depths are included.
double upscale(float nominalDistance,bool plane,uint& fallback,double& worstStep){
 float target=std::floor(horizon+K/nominalDistance)+.5f;double square=0,prior=0;
 fallback=0;worstStep=0;
 for(uint f=1;f<=256;++f){
  float j=halton(f,2)-.5f;rasterJitter=j;
  mat4 jittered=baseProjection;jittered[1][1]=2.f*j/float(g_draw_push.height);deferInvVP=inverse(jittered);
  float z=viewZ(target);vec3 P(0,z,0);
  vec2 hc=pdnSampleGridHc(vec2(.5f,target),vec2(2),vec2(j));
  ivec2 b=ivec2(floor(hc));vec2 fr=hc-vec2(b);vec4 bw,e,signal;
  for(int k=0;k<4;++k){ivec2 o(k&1,k>>1),q=b+o;
   float sampled=pdnPlaneRaster(vec2(q),vec2(2),vec2(j)).y;
   float qz=viewZ(sampled);
   bw[k]=(o.x?fr.x:1-fr.x)*(o.y?fr.y:1-fr.y);
   float predicted=plane?deferPdnPlaneDepth(P,vec3(0,0,1),pdnPlaneRaster(vec2(q),vec2(2),vec2(j)),z):z;
   e[k]=plane?pdnPlaneDepthMismatch(half(qz),predicted,std::max(.03f*predicted,.05f))
             : (qz>0?std::abs(half(qz)-predicted)/std::max(.03f*predicted,.05f):1e30f);
   // Affine segment of a glossy highlight; fresh analytic field, no history/no RNG.
   signal[k]=1.f+.04f*(sampled-j-horizon);
  }
  vec4 wz=max(vec4(0),1.f-e);float sum=dot(bw,wz*wz);if(sum<1e-4f)++fallback;
  vec4 w=upsampleWeights4(bw,e,vec4(1));
  double expected=1.+.04*(target-j-horizon),error=double(dot(w,signal))-expected;
  square+=error*error;if(f>1)worstStep=std::max(worstStep,std::abs(error-prior));prior=error;
 }
 return std::sqrt(square/256.);
}
int main(){
 std::setvbuf(stdout,nullptr,_IONBF,0);
 pc.u_cam_time=vec4(0,0,1.6f,0);linfo.cam_pos=pc.u_cam_time;linfo.cam_fwd=vec4(0,1,0,0);
 mat4 proj(0);proj[0][0]=1.f;proj[1][3]=1.f;proj[2][1]=-1.6f;proj[3][2]=.1f;baseProjection=proj;deferInvVP=inverse(proj);
 uint provedLoss=0;
 for(float ratio:{1.f,2.f})for(float d:{5.f,10.f,20.f,40.f,80.f,160.f}){
  Summary before=temporal(d,ratio,false),after=temporal(d,ratio,true);
  assert(before.total>100 && after.total==before.total);
  assert(after.partial==0 && after.none==0);
  if(d>=40)assert(before.partial>0);
  provedLoss+=before.none;
  std::printf("temporal scale1/%g d%gm: partial/empty %u/%u -> %u/%u; support %.5f -> %.5f (%u phases)\n",ratio,d,before.partial,before.none,after.partial,after.none,before.support/before.total,after.support/after.total,before.total);
  std::printf("  GGX + stochastic point-light normalized MSE %.6f -> %.6f (settled-history phase sweep)\n",before.noiseSquared/before.total,after.noiseSquared/after.total);
  assert(after.noiseSquared/after.total < .063);
  if(d>=20)assert(after.noiseSquared < before.noiseSquared*.2);
 }
 assert(provedLoss>100);
 // A lower avatar/camera reaches the same grazing failure at shorter distance.
 pc.u_cam_time.z=.4f;linfo.cam_pos=pc.u_cam_time;K=57.6f;
 Summary lowBefore=temporal(10.f,1.f,false),lowAfter=temporal(10.f,1.f,true);
 assert(lowBefore.none>100 && lowAfter.none==0);
 std::printf("low camera0.4m Full d10m: empty history %u->%u of%u phases\n",lowBefore.none,lowAfter.none,lowBefore.total);
 pc.u_cam_time.z=1.6f;linfo.cam_pos=pc.u_cam_time;K=230.4f;
 for(float d:{5.f,10.f,20.f,40.f,80.f}){
  uint oldFallback,newFallback;double oldStep,newStep;
  double oldRms=upscale(d,false,oldFallback,oldStep),newRms=upscale(d,true,newFallback,newStep);
  std::printf("upsample Half d%gm: fallback %u->%u, glossy reconstruction RMS %.7f->%.7f, max error-step %.7f->%.7f\n",d,oldFallback,newFallback,oldRms,newRms,oldStep,newStep);
  assert(newFallback==0 && newRms<oldRms*.1);
  assert(newStep<oldStep*.1);
 }
 // Spatial edge stops: exact same grazing plane, jumps of up to8 denoiser
 // texels. Proper perspective prediction survives where affine-Z is wrong.
 double oldError=0,newError=0;uint n=0;
 rasterJitter=.125f;
 for(float d:{10.f,20.f,40.f,80.f})for(int off:{-8,-4,-2,-1,1,2,4,8}){
  int rp=int(horizon+K/d);float z=pdnViewZ({0,rp}),zq=pdnViewZ({0,rp+off*2});if(z<=0||zq<=0)continue;
  linfo.fwd.z=0;float oldG=half(pdnGrad({0,rp},{0,2},z));
  linfo.fwd.z=16;float newG=half(pdnGrad({0,rp},{0,2},z));
  float predicted=pdnPlaneDepth(z,vec2(0,newG),vec2(0,off),false);
  assert(predicted>0);
  oldError+=std::abs((z+oldG*off-zq)/zq);newError+=std::abs((predicted-zq)/zq);++n;
 }
 assert(newError<oldError*.01);
 std::printf("spatial same-plane mean relative depth error %.7f -> %.7f (%u taps)\n",oldError/n,newError/n,n);
 // An actual parallel surface offset does not gain acceptance through slope correction.
 for(float z:{5.f,20.f,100.f,1000.f}){
  float wrong=pdnPlaneDepth(z*.75f,vec2(.1f,0),vec2(0),false);
  assert(std::abs(wrong-z)>.04f*z);
 }
 // FP16 similarity plateau never expands the geometric rejection interval.
 for(float z:{1.f,10.f,100.f,1000.f})for(float fraction:{.9f,1.f,1.1f}){
  float tol=.03f*z;float actual=z+fraction*tol;
  float e=pdnPlaneDepthMismatch(actual,z,tol);
  assert((std::abs(actual-z)<tol)==(e<1.f));
 }
 assert(pdnPlaneDepth(10,vec2(2,0),vec2(1,0),false)<0); // across/behind the horizon
 assert(pdnPlaneDepth(10,vec2(2,0),vec2(1,0),true)==12); // orthographic stays affine
 std::puts("PASS source-helper grazing floor: temporal, spatial, upsample stage isolation; FP16 coefficients/depths; parallel-surface rejection; orthographic/horizon controls");
}
'''
(out/'plane.cpp').write_text(program)
subprocess.run(['c++','-std=c++20','-O2','-Wall','-Wextra','-Werror',str(out/'plane.cpp'),'-o',str(out/'plane')],check=True)
subprocess.run([str(out/'plane')],check=True)

# Wiring: producer and every consumer agree on one latched representation.
renderer=(root/'indra/newview/vkgfx/llcleanrenderervk.cpp').read_text()
host=(root/'indra/newview/vkgfx/llcleandenoisevk.cpp').read_text()
header=(root/'indra/newview/vkgfx/llcleandenoisevk.h').read_text()
tail=(root/'indra/newview/vkgfx/llcleanprojstochvk.cpp').read_text()
temporal=(sh/'clean_dn_temporal.comp').read_text();spatial=(sh/'clean_dn_atrous.comp').read_text()
assert 'mPdnPlaneReconstruction = s_plane && s_sampleGrid;' in renderer
assert '(mPdnPlaneReconstruction ? 16u : 0u)' in renderer
assert 'dp.shadowPlaneReconstruction = mPdnPlaneReconstruction && dp.sampleGridHistory;' in tail
assert host.count('(p.shadowPlaneReconstruction ? 512u : 0u)')==2
assert 'mShadowPlaneReconstruction != p.shadowPlaneReconstruction' in host
assert 'bool shadowPlaneReconstruction = false;' in header # GI uses original format
assert 'pdnPlaneSlope(vz, zp, zn' in lighting
assert 'pdnPlaneDepthMismatch(az, expected, tol)' in lighting
assert 'deferPdnGeomNormal' in lighting
assert temporal.count('dnPreviousDepthAt(')>=4
assert temporal.count('dnSurfaceDepthAt(')==3 and spatial.count('dnSurfaceDepthAt(')==4
assert 'length(hv - hc) <= 0.5' in temporal # shifted virtual taps retain old validity semantics
import xml.etree.ElementTree as ET
m=list(ET.parse(root/'indra/newview/app_settings/settings.xml').getroot().find('map'))
entry=next(m[i+1] for i,k in enumerate(m) if k.tag=='key' and k.text=='RenderVKShadowPlaneReconstruction')
e=list(entry);fields={k.text:e[i+1].text for i,k in enumerate(e) if k.tag=='key'}
assert fields['Persist']=='0' and fields['Value']=='1'
assert 'RenderVKShadowSpecularFixes' not in renderer # rejected candidate not imported
print('PASS one-frame format latch; temporal/spatial/upsample flags; history invalidation; GI default off; transient A/B')
