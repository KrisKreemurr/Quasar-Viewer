#!/usr/bin/env python3
"""Compile production change/cap helpers; replay stationary receivers with moving shadows."""
from pathlib import Path
import re
import subprocess
import sys
root,out=map(Path,sys.argv[1:3]);out.mkdir(parents=True,exist_ok=True)
src=(root/'indra/vkclean/shaders/clean_dn_temporal.comp').read_text()
helpers='\n'.join(re.search(r'(?:bool|float) '+n+r'\([^}]+}',src).group() for n in ['dnCasterChange','dnCasterHistoryCap'])
code='''#include <algorithm>
#include <cmath>
#include <cassert>
#include <initializer_list>
float min(float a,float b){return std::min(a,b);}
float max(float a,float b){return std::max(a,b);}
using std::abs;
'''+helpers+'''
int main(){
 for(float fps:{30.f,60.f,120.f,240.f}) {
  float step=60.f/fps;
  assert(dnCasterHistoryCap(32.f,step)<=2.f*step+1e-6f);
  // Static receiver: zero motion, unchanged depth/albedo/light. Raw shadow moves.
  float oldShade=1.f,newShade=1.f;
  for(int frame=0;frame<2;++frame){
   oldShade+=(0.f-oldShade)/32.f; // widened historical variance contains old colour
   if(dnCasterChange(0.f,0.f,newShade,0.f))newShade=0.f;
  }
  assert(oldShade>.93f && newShade==0.f);
  assert(dnCasterChange(1.f,1.f,0.f,0.f)); // caster departure: next sample clears
  // Noise at a fixed penumbra: disagreeing centre and mean do not trigger reset.
  assert(!dnCasterChange(0.f,.5f,.5f,.5f));
  assert(!dnCasterChange(1.f,.5f,.5f,.5f));
  assert(!dnCasterChange(.5f,.5f,.5f,0.f));
 }
}
'''
(out/'check.cpp').write_text(code)
subprocess.run(['c++','-std=c++20','-Wall','-Wextra','-Werror',str(out/'check.cpp'),'-o',str(out/'check')],check=True)
subprocess.run([str(out/'check')],check=True)
producer=(root/'indra/vkclean/shaders/clean_deferred_light.comp').read_text()
spatial=(root/'indra/vkclean/shaders/clean_dn_atrous.comp').read_text()
assert 'casterMoved = casterMoved || pdnVisMoverOf(visW)' in producer
assert 'casterMoved = casterMoved || rt_block_dyn' in producer
assert 'casterMoved ? 256u : 0u' in producer
assert '(mo.w & 256u)' in src and 'h.caster > 0.0' in src
assert '(g.meta & 0x400u) == 0u' in spatial
assert '(casterCapD || casterCapS) ? 0x400u' in src and 'casterNow ? 0x800u' in src
print('PASS production helper fixture: stationary-receiver arrival/departure;2-frame caps30..240Hz; penumbra noise; producer/departure/feedback contract')
