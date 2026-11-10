#!/usr/bin/env python3
"""Compile the actual GLSL TLAS transform predicates as scalar C++ (no Vulkan/GPU)."""
import argparse
from pathlib import Path
import subprocess


def function(source, name):
    start = source.index('bool ' + name + '(')
    first = source.index('{', start)
    depth = 1
    end = first + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source_root', type=Path)
    parser.add_argument('output_dir', type=Path)
    args = parser.parse_args()
    root = args.source_root.resolve()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    shader = (root / 'indra/vkclean/shaders/clean_rt_instances.comp').read_text()
    finite = function(shader, 'rtBuildFinite') + '\n' + function(shader, 'rtBuildFinite3')
    linear = function(shader, 'badLinearTransform')
    transform = function(shader, 'badTransform')
    rig = function(shader, 'rigFrame')
    assert 'badLinearTransform(mm[0].xyz, mm[1].xyz, mm[2].xyz)' in transform
    assert 'badLinearTransform(R[0], R[1], R[2])' in rig
    assert '!rtBuildFinite3(T)' in rig
    assert '!rtBuildFinite3(q)' in shader and '!rtBuildFinite3(lpos)' in shader
    assert 'any(isnan(' not in shader and 'any(isinf(' not in shader
    body = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <cstdint>
#include <cstring>
#include <iostream>
uint32_t floatBitsToUint(float v){uint32_t u;std::memcpy(&u,&v,sizeof(u));return u;}
using std::max; using std::abs; using std::isnan; using std::isinf;
struct vec3 { float x,y,z; explicit vec3(float v):x(v),y(v),z(v){}
 vec3(float a,float b,float c):x(a),y(b),z(c){} };
vec3 operator/(vec3 a,float s){return vec3(a.x/s,a.y/s,a.z/s);}
vec3 abs(vec3 a){return vec3(abs(a.x),abs(a.y),abs(a.z));}
float dot(vec3 a,vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
vec3 cross(vec3 a,vec3 b){return vec3(a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x);}
float length(vec3 a){return std::sqrt(dot(a,a));}
bool greaterThan(vec3 a,vec3 b){return a.x>b.x||a.y>b.y||a.z>b.z;}
bool isnan(vec3 a){return isnan(a.x)||isnan(a.y)||isnan(a.z);}
bool isinf(vec3 a){return isinf(a.x)||isinf(a.y)||isinf(a.z);}
struct vec4 {vec3 xyz;float w;};
bool isnan(vec4 a){return isnan(a.xyz)||isnan(a.w);}
bool isinf(vec4 a){return isinf(a.xyz)||isinf(a.w);}
bool any(bool a){return a;}
struct mat4 {vec4 c[4];vec4& operator[](int i){return c[i];}const vec4& operator[](int i)const{return c[i];}};
mat4 matrix(vec3 a,vec3 b,vec3 c){return {{{a,0},{b,0},{c,0},{vec3(0),1}}};}
'''
    baseline = subprocess.check_output(['git', 'show', '3f3d4c73ce:indra/vkclean/shaders/clean_rt_instances.comp'], cwd=root, text=True)
    old_transform = function(baseline, 'badTransform').replace('badTransform(', 'baselineBadTransform(', 1)
    # Keep the shipped singular-transform fix as the finite-math regression baseline.
    shipped = subprocess.check_output(['git', 'show', '160b955f9f:indra/vkclean/shaders/clean_rt_instances.comp'], cwd=root, text=True)
    shipped_linear = function(shipped, 'badLinearTransform').replace('badLinearTransform(', 'shippedBadLinearTransform(', 1)
    shipped_transform = function(shipped, 'badTransform').replace('badTransform(', 'shippedBadTransform(', 1).replace('badLinearTransform(', 'shippedBadLinearTransform(')
    body += finite + '\n' + linear + '\n' + transform + '\n' + old_transform + '\n' + shipped_linear + '\n' + shipped_transform
    body += r'''
int main(){
 const vec3 lo(-1),hi(1);
 auto rejected=[&](vec3 a,vec3 b,vec3 c){return badTransform(matrix(a,b,c),lo,hi);};
 assert(!rejected(vec3(1,0,0),vec3(0,1,0),vec3(0,0,1)));
 // The unmodified candidate predicate accepted this invalid input.
 assert(!baselineBadTransform(matrix(vec3(0),vec3(0,1,0),vec3(0,0,1)),lo,hi));
 assert(!baselineBadTransform(matrix(vec3(1,1,0),vec3(2,1,0),vec3(3,2,0)),lo,hi));
 // A legal zero joint scale becomes an invalid TLAS transform.
 assert(rejected(vec3(0),vec3(0,1,0),vec3(0,0,1)));
 assert(rejected(vec3(1,0,0),vec3(2,0,0),vec3(0,0,1)));
 // A collapsed parent followed by rotation need not have a zero column.
 assert(rejected(vec3(1,1,0),vec3(2,1,0),vec3(3,2,0)));
 assert(!rejected(vec3(-1,0,0),vec3(0,1,0),vec3(0,0,1)));
 assert(!rejected(vec3(1e-15f,0,0),vec3(0,1e-15f,0),vec3(0,0,1e-15f)));
 assert(!rejected(vec3(1e-15f,0,0),vec3(0,1e3f,0),vec3(0,0,0.5f)));
 assert(!rejected(vec3(1,0,0),vec3(0.25f,1,0),vec3(0,0,1)));
 assert(rejected(vec3(std::numeric_limits<float>::quiet_NaN(),0,0),vec3(0,1,0),vec3(0,0,1)));
 assert(rejected(vec3(std::numeric_limits<float>::infinity(),0,0),vec3(0,1,0),vec3(0,0,1)));
 // Every matrix lane and bounds axis must reject non-finite values even when
 // floating-point isnan/isinf predicates may be optimized away.
 for(float invalid : {std::numeric_limits<float>::quiet_NaN(),
                      std::numeric_limits<float>::infinity(),
                      -std::numeric_limits<float>::infinity()}) {
  for(int col=0;col<4;++col) {
   for(int lane=0;lane<4;++lane) {
    auto m=matrix(vec3(1,0,0),vec3(0,1,0),vec3(0,0,1));
    if(lane==0)m[col].xyz.x=invalid;
    if(lane==1)m[col].xyz.y=invalid;
    if(lane==2)m[col].xyz.z=invalid;
    if(lane==3)m[col].w=invalid;
    assert(badTransform(m,lo,hi));
   }
  }
  auto m=matrix(vec3(1,0,0),vec3(0,1,0),vec3(0,0,1));
  assert(badTransform(m,vec3(invalid,0,0),hi));
  assert(badTransform(m,lo,vec3(0,invalid,0)));
 }
 // Record how the shipped floating-point guards behave under this compiler.
 // The candidate must reject independently of whether that compiler folds them.
 auto nonfiniteTranslation=matrix(vec3(1,0,0),vec3(0,1,0),vec3(0,0,1));
 nonfiniteTranslation[3].xyz.x=std::numeric_limits<float>::quiet_NaN();
 assert(badTransform(nonfiniteTranslation,lo,hi));
 std::cout << "shipped guard rejects NaN translation: " << shippedBadTransform(nonfiniteTranslation,lo,hi) << "\n";
 // Existing finite-position and world-size guards stay active.
 auto t=matrix(vec3(1,0,0),vec3(0,1,0),vec3(0,0,1));t[3].xyz=vec3(1e8f,0,0);
 assert(badTransform(t,lo,hi));
 assert(rejected(vec3(1e7f,0,0),vec3(0,1,0),vec3(0,0,1)));
}
'''
    cpp = out / 'rt_transform_checks.cpp'
    binary = out / 'rt_transform_checks'
    cpp.write_text(body)
    for suffix, flags in [('strict', []), ('finite_math', ['-ffast-math'])]:
        variant = binary.with_name(binary.name + '_' + suffix)
        subprocess.run(['c++', '-std=c++17', '-O2', *flags, '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(variant)], check=True)
        subprocess.run([str(variant)], check=True)
    print('PASS actual GLSL predicates, strict + finite-math: all matrix lanes/bounds nonfinite, singular, reflected, tiny, anisotropic, shear')


if __name__ == '__main__':
    main()
