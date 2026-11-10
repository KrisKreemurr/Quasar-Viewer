#!/usr/bin/env python3
"""Production selector/ownership and estimator partition regressions; CPU only."""
from pathlib import Path
import argparse, re, subprocess, xml.etree.ElementTree as ET
ap=argparse.ArgumentParser();ap.add_argument('source',type=Path);ap.add_argument('output',type=Path);a=ap.parse_args();a.output.mkdir(parents=True,exist_ok=True)
root=a.source;sh=root/'indra/vkclean/shaders';lighting=(sh/'clean_deferred_light.comp').read_text()
def extract(text,signature):
 start=text.index(signature);b=text.index('{',start);end=b+1;depth=1
 while depth:
  depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]
functions='\n'.join(extract(lighting,x) for x in ['bool pdnRestHas(', 'bool pdnExactSpecularOwns(', 'bool pdnGlossySpecular(', 'uint pdnSplitSelect('])
functions=functions.replace('[[dont_unroll]]','').replace('[[unroll]]','')
functions=re.sub(r'\b(?:inout|out)\s+(\w+)\s+(\w+)',r'\1& \2',functions)
functions=functions.replace('PDN_SPLIT_HYST','1.5f').replace('vec4(0.0)','vec4(0.f)')
functions=re.sub(r'(?<![\w.])(\d+\.\d+)(?![\w.])',r'\1f',functions)
fixture=r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
using uint=uint32_t;
struct vec3 {float value=0;}; struct LegacySpec {float gloss=0;};
struct uvec2 {uint x=0,y=0;};
struct uvec4 {uint x,y,z,w;explicit uvec4(uint q=0):x(q),y(q),z(q),w(q){}uint& operator[](uint i){return (&x)[i];}uint operator[](uint i)const{return (&x)[i];}};
struct vec4 {float x,y,z,w;explicit vec4(float q=0):x(q),y(q),z(q),w(q){}float& operator[](uint i){return (&x)[i];}float operator[](uint i)const{return (&x)[i];}};
struct Light {uvec4 tex;};Light lights[16];float specTerms[16],targets[16];uint cluster_index[16],cluster_count[1];
struct Info {vec4 cam_pos;};Info linfo;
constexpr uint PDN_SPLIT_MAX=4,CLUSTER_CAP=16;
bool exactOn=true;bool pdnExactSpecular(){return exactOn;}
bool pdnSplitHas(uvec4 ids,uint li){for(uint k=0;k<4;++k)if(ids[k]==li)return true;return false;}
int pdnSplitFind(uvec2 ids,uint key){return (ids.x&65535u)==key?0:-1;}
uint mainClusterFrag(vec3){return 0;}uint clusterLen(uint n){return n;}uint clusterShadowedLen(uint n){return n;}
bool pdnSplitEligible(uint,bool){return true;}float pdnTargetL(uint li,vec3,vec3,bool){return targets[li];}
bool occCulled(uint,uint){return false;}float pdnRand(uint& s){++s;return .5f;}
float pdnLum(vec3 v){return v.value;}
bool projDirectTerms(uint li,vec3,vec3,vec3,bool,vec3,vec3,vec3,float,LegacySpec, bool,vec3& d,vec3& s,float& distance){d.value=1;s.value=specTerms[li];distance=1;return true;}
using std::min;
'''
checks=r'''
uint choose(uint n,uvec2 previous={}){
 linfo.cam_pos.w=float(n);cluster_count[0]=n;uvec4 li,id;uint probe,seed=4;float nProbe;
 uint count=pdnSplitSelect({}, {}, 1, true,previous,~0u,seed,li,id,probe,nProbe,{},true,{}, {},{},.1f,{});
 assert(count==(n?1u:0u));return li.x;
}
int main(){
 assert(pdnGlossySpecular(true,.39f,{}));assert(!pdnGlossySpecular(true,.4f,{}));
 assert(pdnGlossySpecular(false,1,{.5f}));assert(!pdnGlossySpecular(false,1,{.25f}));
 for(uint i=0;i<16;++i){lights[i].tex.w=100+i;cluster_index[i]=i;targets[i]=1;specTerms[i]=float(i+1);}
 assert(choose(16)==15); // exact BRDF ranking, not diffuse target or RIS draw
 specTerms[0]=16;assert(choose(16)==0); // stable key breaks ties
 specTerms[0]=15.9f;assert(choose(16,{100,0})==15); // no 1.5x diffuse incumbent bias
 for(uint trial=0;trial<1000;++trial){
  uint n=2+trial%15;
  for(uint i=0;i<n;++i)specTerms[i]=.1f+float((trial*37+i*17)%101);
  uint dominant=choose(n);uvec4 ids(~0u);ids.x=dominant;
  double expectedD=0,expectedS=0,estimatedD=0,estimatedS=0,denom=double(n*(n+1))/2;
  double rawExact=specTerms[dominant]*(.1+double((dominant+trial)%10)/10);
  for(uint i=0;i<n;++i){
   double probability=double(i+1)/denom,visibility=.1+double((i+trial)%10)/10;
   double d=1+double(i),s=specTerms[i];
   expectedD+=d*visibility;expectedS+=s*visibility;
   estimatedD+=probability*(d*visibility/probability);
   estimatedS+=probability*((pdnExactSpecularOwns(ids,i)?0:s)*visibility/probability+rawExact);
   assert(!pdnRestHas(ids,i)); // unchanged proposal support, including dominant
  }
  assert(std::abs(expectedD-estimatedD)<1e-9);
  assert(std::abs(expectedS-estimatedS)<1e-9); // no dropped/double-counted light
  exactOn=false;assert(pdnRestHas(ids,dominant));assert(!pdnExactSpecularOwns(ids,dominant));exactOn=true;
 }
 // Different ownership residuals cannot be averaged and then paired with only
 // the centre's exact term: it introduces a deterministic energy error.
 double total=13,exactA=10,exactB=3;
 assert(.5*(total-exactA)+.5*(total-exactB)+exactA!=total);
 assert(.5*(total-exactA+exactA)+.5*(total-exactB+exactB)==total);
 std::puts("actual selector + ownership: dominant/tie/unequal-PDF partition 1000 cases PASS");
}
'''
cpp=a.output/'exact_specular.cpp';cpp.write_text(fixture+functions+checks);exe=a.output/'exact_specular'
subprocess.run(['c++','-std=c++20','-O2','-Wall','-Wextra','-Werror',str(cpp),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
tmp=(sh/'clean_dn_temporal.comp').read_text();sp=(sh/'clean_dn_atrous.comp').read_text();host=(root/'indra/newview/vkgfx/llcleanprojstochvk.cpp').read_text()
assert 'if (pdnExactSpecularOwns(spLi, sLi)) st = vec3(0.0);' in lighting
assert lighting.index('if (pdnExactSpecularOwns(spLi, sLi))')>lighting.index('// (RADIANCE MODE) a RIS pick\'s firefly clamp against its own target')
assert '!s.cel && pdnGlossySpecular(pbr, rough, ls) ? 1u : 0u' in lighting
assert lighting.count('S.radius = 0.0; S.nRays = 1u; S.flags &= ~SH_SPLIT;') == (3 if 'void vqShVisSample(' in lighting else 2)
assert 'dnExactSpecular() ? vec4(0.0) : vec4(1.0)' in tmp and 'dnExactSpecular() ? vec4(0.0) : vec4(1.0)' in sp
assert 'exactSpecular = pdnSane(st * vis / radSf)' in lighting
for p in ['dnSpecularOwner(q, pl) == g_dn_specular_owner','dnSpecularOwner(bq, pl) != g_dn_specular_owner']:assert p in tmp
assert 'dnSpecularOwner(q, par) == g_dn_specular_owner' in tmp
assert 'dnSpecularOwner(q, par) == specularOwner' in sp
raw=extract(tmp,'void dnSplitTemporal(').split('if (dnExactSpecular())',1)[1].split('return;',1)[0]
assert 'imageLoad(uDnIn, ivec3(m, L))' in raw and 'pdnPackHist' not in raw
raw=extract(sp,'vec4 dnSplitAtrous(').split('if (dnExactSpecular())',1)[1].split('return sharp;',1)[0]
assert 'dnDst(m, L, sharp)' in raw and 'dnSrc(m, L)' in raw
assert 'deferExactSpecularTap(q, fin)' in lighting and 'w[k] * deferExactSpecularTap(tp[k], fin)' in lighting
assert 'mPdn->hasSplit()' in host and 'mPdnRadiance && mPdnExactSpecular' in host
ET.parse(root/'indra/newview/app_settings/settings.xml')
s=(root/'indra/newview/app_settings/settings.xml').read_text().split('<key>RenderVKShadowExactSpecular</key>',1)[1].split('</map>',1)[0]
assert '<key>Persist</key><integer>0</integer>' in s
print('source contracts: raw bypass, owner-aware residuals, bounded slot/sample, allocation fallback and A/B PASS')
