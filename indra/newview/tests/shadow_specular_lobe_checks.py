#!/usr/bin/env python3
"""Source-bound CPU regressions for sharp stochastic-shadow specular recovery."""
from pathlib import Path
import argparse, re, subprocess
p=argparse.ArgumentParser();p.add_argument('source',type=Path);p.add_argument('output',type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
sh=a.source/'indra/vkclean/shaders'
def extract(text, signature):
 start=text.index(signature);b=text.index('{',start);i=b+1;depth=1
 while depth:
  depth+=(text[i]=='{')-(text[i]=='}');i+=1
 return text[start:i]
common=(sh/'clean_pdn_common.glsl').read_text();temporal=(sh/'clean_dn_temporal.comp').read_text();spatial=(sh/'clean_dn_atrous.comp').read_text()
radius=extract(common,'float pdnSpecRadius(').replace('clamp(', 'std::clamp(').replace('max(', 'std::max(')
radius=radius.replace('1e-3','1e-3f')
radius=re.sub(r'(?<![\w.])(\d+\.\d+)(?![\w.])',r'\1f',radius)
# Production virtual-failure branch, not an independent arithmetic implementation.
fail=extract(temporal,'else if (dnSpecularLobeGuard())');fail=fail[fail.index('{'):]
# Generate a small wrapper with a returning body (the raw body is void).
program=r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
'''+radius+'\nfloat failedVirtualAge(float hlS,float kv)'+fail[:-1]+'return hlS; }\n'+r'''
int main(){
 // A zero-roughness specular signal cannot acquire a neighbour through young
 // initialization even when that neighbour has the same normal and depth.
 for(float rough: {0.f,.02f,.05f,.1f}){
  float neighbours=0;
  for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){
   if(!x&&!y)continue;
   if(std::hypot(float(x),float(y))<=pdnSpecRadius(rough,10.f,10.f))++neighbours;
  }
  assert(neighbours==0);
 }
 // Rough lobes retain neighbourhood recovery. Actual original iteration
 // selector radius is unchanged by factoring it into the common helper.
 assert(pdnSpecRadius(.6f,10.f,10.f)>2.f);
 for(int i=0;i<=1000;++i){float r=float(i)/1000.f;
  float original=r*r*std::clamp(10.f/20.f*2.f,.25f,1.f)*48.f;
  assert(pdnSpecRadius(r,10.f,10.f)==original);
 }
 // Failed mirror reprojection must not retain surface-reprojected colour;
 // the transition to rough surface history remains bounded and continuous.
 assert(failedVirtualAge(24,1)==0);
 assert(failedVirtualAge(24,0)==24);
 assert(failedVirtualAge(24,.5f)==12);
 std::puts("sharp impulse support, rough recovery, unchanged radius and virtual-failure age PASS");
}
'''
cpp=a.output/'specular_lobe.cpp';cpp.write_text(program);exe=a.output/'specular_lobe'
subprocess.run(['c++','-std=c++20','-O2','-Wall','-Wextra','-Werror',str(cpp),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
assert 'if (!dnSpecularLobeGuard()) pS = mix' in spatial
assert 'length(vec2(o)) <= pdnSpecRadius(g.rough, 0.0, g.z)' in spatial
assert 'length(vec2(o)) <= pdnSpecRadius(g.rough, hitDist, g.z)' in temporal
assert 'abs(g.rough - gq.rough) <= 0.05' in temporal
host=(a.source/'indra/newview/vkgfx/llcleandenoisevk.cpp').read_text()
assert '(p.radiance && p.specularLobeGuard ? 32u : 0u)' in host
assert 'const uint k = dp.ctl.y & 7u;' in spatial
assert 'mSpecularLobeGuard != p.specularLobeGuard' in host
assert 'p.radiance && p.specularLobeGuard' in host
import xml.etree.ElementTree as ET
ET.parse(a.source/'indra/newview/app_settings/settings.xml')
settings=(a.source/'indra/newview/app_settings/settings.xml').read_text().split('<key>RenderVKShadowSpecularLobeGuard</key>',1)[1].split('</map>',1)[0]
assert '<key>Persist</key><integer>0</integer>' in settings
print('source-bound temporal/spatial flag, lobe stops, toggle invalidation, transient setting and XML PASS')
