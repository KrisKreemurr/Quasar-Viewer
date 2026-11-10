#!/usr/bin/env python3
"""Compile extracted production HDR tone/final helpers and test their unit contract.

Requires a real GLM include directory. No viewer link or GPU. The fixture replaces
only external grading (disabled here) and GLSL vector syntax with GLM equivalents.
"""
import argparse
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--out', type=Path, required=True)
p.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[4])
p.add_argument('--glm', type=Path, required=True)
a = p.parse_args()
root = a.source_root
tone = (root / 'indra/vkclean/shaders/clean_tonemap.frag').read_text()
final = (root / 'indra/vkclean/shaders/clean_final.frag').read_text()

def function(source, name):
    m = re.search(r'^(?:vec3|float) ' + name + r'\([^\n]*\)\s*\{', source, re.M)
    assert m, name
    end = source.index('{', m.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[m.start():end]

def cpp(source):
    # GLSL floating literals are floats; unsuffixed C++ literals are doubles.
    return re.sub(r'(?<![\w.])(\d*\.\d+(?:e[-+]?\d+)?|\d+e[-+]?\d+)(?![\w.])', r'\1f', source)

statement = re.findall(r'if\(kDisplayLinear && pc\.p4\.w>0\.0\) c=([^;]+);', tone)[-1]
last = final[final.rindex('    vec3 display='):final.rindex('    outColor=')]
last = last.replace('diff.rgb', 'relative')
code = r'''
#include <glm/glm.hpp>
#include <cassert>
#include <cmath>
#include <initializer_list>
using namespace glm;
struct { vec4 p1{0},p4{0},p5{0}; } pc;
struct { vec4 misc{0}; } grade;
struct { vec4 res_frame{0}, dither{0}; } fx;
bool kDisplayLinear=true;
const vec3 CG_LUMA(.2126f,.7152f,.0722f);
vec3 gradeLinear(vec3 v) { return v; } // grading disabled in these cases
'''
code += cpp(function(tone, 'displayDecodeSrgb')) + '\n'
code += cpp(function(tone, 'displayHeadroom')) + '\n'
code += cpp(function(final, 'displayRange')) + '\n'
code += 'vec3 toneOutput(vec3 c,vec3 scene) { vec3 c_scene=scene; if(kDisplayLinear && pc.p4.w>0.f) c=' + cpp(statement) + '; return c; }\n'
code += 'vec3 finalOutput(vec3 relative) {\n' + cpp(last) + '\nreturn display; }\n'
code += r'''
bool closeColour(vec3 a,vec3 b,float e=.002f) { return length(a-b)<e; }
void setup(float white,float peak) {
 pc.p4.w=peak/white;pc.p5.x=white;pc.p5.y=6;
 fx.res_frame.w=white;fx.dither.w=peak/white;
}
int main() {
 const vec3 encoded(.46f,.32f,.2f);
 const vec3 base=displayDecodeSrgb(encoded);
 for(float white:{203.f,350.f}) for(float peak:{409.f,1000.f,1350.f}) {
  if(peak<white) continue;
  setup(white,peak);
  // Sane coloured midtones must neither whiten nor acquire white squared.
  vec3 nits=finalOutput(toneOutput(encoded,vec3(.2f)));
  assert(closeColour(nits,base*white));assert(nits.r>nits.g && nits.g>nits.b);
  // No discontinuous jump when the scene first enters HDR headroom.
  vec3 below=toneOutput(encoded,vec3(1.f-.000001f));
  vec3 above=toneOutput(encoded,vec3(1.f+.000001f));
  assert(closeColour(below,above));
  float previous=dot(base,CG_LUMA);
  for(float scene:{1.f,1.01f,2.f,10.f,100.f}) {
   vec3 relative=toneOutput(encoded,vec3(scene));
   float lum=dot(relative,CG_LUMA);
   assert(lum+.00001f>=previous);previous=lum;
   assert(max(relative.r,max(relative.g,relative.b))<=peak/white+.00001f);
   assert(std::abs(relative.r/relative.g-base.r/base.g)<.0001f);
   assert(max(finalOutput(relative).r,max(finalOutput(relative).g,finalOutput(relative).b))<=peak+.01f);
  }
 }
 kDisplayLinear=false;setup(350,409);
 assert(closeColour(toneOutput(encoded,vec3(.2f)),encoded));
 assert(closeColour(finalOutput(encoded),encoded));
}
'''
a.out.mkdir(parents=True, exist_ok=True)
src = a.out / 'hdr_display_units.cpp'
src.write_text(code)
exe = a.out / 'hdr_display_units'
subprocess.run(['c++','-std=c++20','-O2','-Wall','-Wextra','-Werror','-I'+str(a.glm),str(src),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
print('production HDR display units PASS')
