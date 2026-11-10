#!/usr/bin/env python3
"""Replay production scalar history helpers: broad caster tags vs own visibility delta.
No renderer/GPU claim: deterministic scalar moving-occluder regression.
Usage: python3 stoch_boil_checks.py SOURCE_ROOT OUTPUT_DIR
"""
from pathlib import Path
import re
import subprocess
import sys

root, out = map(Path, sys.argv[1:3])
out.mkdir(parents=True, exist_ok=True)
temporal = (root / 'indra/vkclean/shaders/clean_dn_temporal.comp').read_text()
common = (root / 'indra/vkclean/shaders/clean_pdn_common.glsl').read_text()
time = (root / 'indra/vkclean/shaders/clean_history_time.glsl').read_text()

def function(src, name):
    found = re.search(r'(?:bool|float|uint) ' + name + r'\([^}]+}', src)
    assert found, name
    return found.group()

helpers = '\n'.join(re.findall(r'#define DN_CASTER_\w+ [^\n]+', temporal)) + '\n'
helpers += '\n'.join(function(temporal, n) for n in (
    'dnCasterChange', 'dnCasterVisibilityChange', 'dnCasterVisibilityAcc',
    'dnCasterShouldCap', 'dnCasterHistoryCap'))
helpers += '\n' + '\n'.join(function(common, n) for n in ('pdnVisibilityCode', 'pdnVisibilityValue'))
helpers += '\n' + '\n'.join(function(time, n) for n in ('htAlpha', 'htAdvance'))
code = r'''
#include <algorithm>
#include <cmath>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <initializer_list>
using uint = unsigned int;
using std::abs; using std::isinf;
float min(float a,float b){return std::min(a,b);}
float max(float a,float b){return std::max(a,b);}
float clamp(float x,float a,float b){return std::clamp(x,a,b);}
float mix(float a,float b,float t){return a+(b-a)*t;}
float testStep=1.f;
float htStep(){return testStep;}
''' + helpers + r'''
struct Pixel {float acc=1.f, age=32.f, visibility=1.f;};
uint rng=1341u;
float noise(){rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return float(rng&65535u)/65535.f-.5f;}
void sample(Pixel& p,uint policy,float raw,float mean,float sigma,float vis,float visMean,float visSigma){
 bool changed=policy==1u ? dnCasterChange(raw,mean,p.acc,sigma)
    : policy==2u && dnCasterVisibilityChange(vis,visMean,p.visibility,visSigma,0.f); // exact evidence: noise 0
 if(changed){p.age=0.f;p.acc=raw;}
 // Production neighbourhood clamp: stable radiance within its box keeps age.
 float lo=max(mean-2.f*sigma,0.f), hi=mean+2.f*sigma;
 float bounded=clamp(p.acc,lo,hi);
 float cut=abs(bounded-p.acc)/max(p.acc+mean,1e-4f);
 if(cut>.05f)p.age=min(p.age,mix(8.f,1.f,clamp((cut-.05f)*3.f,0.f,1.f)));
 p.acc=bounded;
 float cap=dnCasterShouldCap(policy,true,changed)?dnCasterHistoryCap(32.f,htStep()):32.f;
 float age=htAdvance(p.age,cap), alpha=p.age>0.f?htAlpha(1.f/max(age,1.f)):1.f;
 p.acc=mix(p.acc,raw,alpha);p.age=age;
 p.visibility=pdnVisibilityValue(pdnVisibilityCode(dnCasterVisibilityAcc(p.visibility,vis,changed?1.f:alpha)));
}
int main(){
 assert(!dnCasterShouldCap(0u,true,true));
 assert(dnCasterShouldCap(1u,true,false));
 assert(!dnCasterShouldCap(2u,true,false));
 assert(dnCasterShouldCap(2u,true,true));
 for(int i=0;i<=1000;++i){float v=float(i)/1000.f;
  assert(abs(pdnVisibilityValue(pdnVisibilityCode(v))-v)<=1.f/60.f+1e-6f);}
 assert(pdnVisibilityCode(-1.f)==0u);
 assert(pdnVisibilityCode(std::numeric_limits<float>::quiet_NaN())==0u);
 assert(pdnVisibilityCode(std::numeric_limits<float>::infinity())==0u);
 assert(!dnCasterVisibilityChange(-1.f,0.f,1.f,0.f,0.f));
 assert(!dnCasterVisibilityChange(0.f,0.f,-1.f,0.f,0.f));
 assert(!dnCasterVisibilityChange(0.f,.5f,.5f,.5f,0.f));
 assert(!dnCasterVisibilityChange(1.f,.5f,.5f,.5f,0.f));
 // Broad moving tag over a lit floor; sampled radiance varies, actual visibility does not.
 for(float fps:{30.f,60.f,120.f,240.f}){
  testStep=60.f/fps;Pixel oldFloor[64],newFloor[64];double oldEnergy=0,newEnergy=0;
  for(int f=0;f<4096;++f)for(int x=0;x<64;++x){
   float raw=1.f+.8f*noise();
   sample(oldFloor[x],1u,raw,1.f,.4f,1.f,1.f,0.f);
   sample(newFloor[x],2u,raw,1.f,.4f,1.f,1.f,0.f);
   if(f>=256){oldEnergy+=std::pow(oldFloor[x].acc-1.f,2);newEnergy+=std::pow(newFloor[x].acc-1.f,2);}
  }
  double denom=64.*(4096.-256.);
  assert(newEnergy < oldEnergy*.25);
  assert(newFloor[0].age==32.f);
  // Moving 8-pixel opaque occluder over the static floor; neighbourhood computed from its mask.
  Pixel p[64];int changedPixels=0,maxLag=0;
  for(int f=0;f<128;++f){
   int start=(f/2)%64;
   for(int x=0;x<64;++x){
    float vis=((x-start+64)%64<8)?0.f:1.f, mean=0.f;
    for(int q=-1;q<=1;++q)mean+=(((x+q-start+128)%64)<8)?0.f:1.f;
    mean/=3.f;float sigma=std::sqrt(mean*(1.f-mean));
    float prior=p[x].visibility;
    sample(p[x],2u,vis,mean,sigma,vis,mean,sigma);
    if(prior!=vis){++changedPixels;if(abs(p[x].acc-vis)>.1f)maxLag=1;}
    assert(abs(p[x].acc-vis)<.1f); // same-frame reset at entering AND departing edges
   }
  }
  assert(changedPixels>120 && maxLag==0);
  std::printf("%gHz floor variance broad=%.8f edge=%.8f reduction=%.2fx; shadow lag=%d frames (%d changed pixels)\n",
     fps,oldEnergy/denom,newEnergy/denom,oldEnergy/newEnergy,maxLag,changedPixels);
 }
}
'''
(out / 'checks.cpp').write_text(code)
subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', str(out/'checks.cpp'), '-o', str(out/'checks')], check=True)
subprocess.run([str(out/'checks')], check=True)
# Source wiring, no second implementation of the selection policy.
assert 'pdnSampleVisibility(evN, evD) << 9u' in (root/'indra/vkclean/shaders/clean_deferred_light.comp').read_text()
assert 'dnCasterEvidence(ownVisibility, visMean, visStoch)' in temporal
assert 'dnCasterShouldCap(dnCasterPolicy(), casterAffected, casterChangeD || follow)' in temporal
assert 'dnCasterShouldCap(dnCasterPolicy(), casterAffected, casterChangeS || follow)' in temporal
assert 'dnCasterPolicy() == 2u ? sunChanged : dynTagged' in temporal
assert 'mCasterCauseDebug != (p.debug == 11u)' in (root/'indra/newview/vkgfx/llcleandenoisevk.cpp').read_text()
import xml.etree.ElementTree as ET
settings=ET.parse(root/'indra/newview/app_settings/settings.xml').getroot().find('map')
children=list(settings); entry=next(children[i+1] for i,v in enumerate(children) if v.tag=='key' and v.text=='RenderVKShadowCasterHistory')
fields=list(entry); values={v.text:fields[i+1].text for i,v in enumerate(fields) if v.tag=='key'}
assert values['Persist']=='0' and values['Value']=='2'
print('PASS visibility packing, unknown evidence, per-channel cap, debug reset, sun gating, nonpersistent setting')
