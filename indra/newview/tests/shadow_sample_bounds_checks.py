#!/usr/bin/env python3
"""Compile the actual GLSL sample-control loop with a mocked walk endpoint."""
import argparse
from pathlib import Path
import subprocess
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
source = (args.source / 'indra/vkclean/shaders/clean_shadow_core.glsl').read_text()

def block(signature):
    start = source.index(signature)
    cursor = source.index('{', start)
    depth = 1
    end = cursor + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

structure = block('struct ShRayGen') + ';\n'
constants = '\n'.join(line for line in source.splitlines() if line.startswith('#define SHG_'))
controls = source[source.index('#define SHR_K'):source.index('void shStatsGuard()')]
body = block('vec3 shadowSampleRays(').replace('[[dont_unroll]]', '')
body = re.sub(r'\.([rgb])\b', lambda m: '.'+{'r':'x','g':'y','b':'z'}[m[1]], body)
fixture = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
using uint=uint32_t;
struct vec3 {
 float x,y,z;
 vec3(float a=0):x(a),y(a),z(a){}
 vec3(float a,float b,float c):x(a),y(b),z(c){}
 vec3& operator+=(vec3 v){x+=v.x;y+=v.y;z+=v.z;return *this;}
 vec3& operator/=(float v){x/=v;y/=v;z/=v;return *this;}
 vec3& operator*=(vec3 v){x*=v.x;y*=v.y;z*=v.z;return *this;}
};
vec3 operator+(vec3 a,vec3 b){return a+=b;}
vec3 operator-(vec3 a,vec3 b){return vec3(a.x-b.x,a.y-b.y,a.z-b.z);}
vec3 operator*(vec3 a,float v){return vec3(a.x*v,a.y*v,a.z*v);}
vec3 operator/(vec3 a,float v){return a/=v;}
float dot(vec3 a,vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
float length(vec3 a){return std::sqrt(dot(a,a));}
vec3 normalize(vec3 a){return a/length(a);}
bool greaterThan(vec3 a,vec3 b){return a.x>b.x||a.y>b.y||a.z>b.z;}
bool any(bool v){return v;}
using std::min;using std::max;using std::clamp;using std::sqrt;using std::cos;using std::sin;
#define RT_WALK_FIXTURE
#define RT_SH_QUERY_BUDGET 256u
#define RT_WALK_KS_ANY 255u
uint rt_query_left=0,rt_ray_rejected=0,rt_query_refused=0,rt_self_owner=0;
float rt_block_t=1e30f,rt_fix_t=-1;
bool rt_walk_probe=false,rt_blend_parity=false;
uint calls=0, mode=0;
uint rtFolMode(){return mode>=4?2u:0u;}
uint rtSelfProbe(){return mode==1?~0u:0u;}
bool rtHintDone(uint,vec3){return false;}
bool rtAdaptiveDone(uint,uint,vec3,vec3){return false;}
void shStatsGuard(){}
void shRayKindOf(uint,uint,uint& k,uint& m,bool& p){k=0;m=255;p=false;}
vec3 rtVisWalkFP(vec3,vec3,float,float t,uint,uint,uint,bool){
 ++calls; if(mode==1 || (mode==5 && calls==1))rt_walk_probe=true;
 if(mode==2 || mode>=4)rt_fix_t=t-0.001f;
 return vec3(1);
}
'''
helpers = '''
vec3 shPosOrigin(ShRayGen,vec3){return vec3(0);}
float shPosTmax(ShRayGen,vec3,vec3,float){return 1;}
'''
checks = r'''
int main(){
 for(uint count: {0u,1u,2u,16u,31u,~0u}){
  ShRayGen g{};g.n=count;g.bits=SHG_HARD;g.pos=vec3(0,0,1);g.tmax=10;
  mode=0;calls=0;rt_query_refused=0;
  vec3 result=shadowSampleRays(g,count,false,false);
  assert(calls==clamp(count,1u,16u));assert(std::isfinite(result.x));assert(result.x==1);
  mode=1;calls=0;result=shadowSampleRays(g,count,false,false);
  assert(calls==2);assert(result.x==0); // repeated UNKNOWN cannot restart forever
  rt_walk_probe=false;
  mode=2;calls=0;result=shadowSampleRays(g,count,false,false);
  assert(calls<=3u*clamp(count,1u,16u));assert(std::isfinite(result.x));
  g.bits=SHG_HARD|SHG_SPLIT|SHG_FOL;
  for(uint retryMode: {4u,5u}){
   mode=retryMode;calls=0;rt_query_refused=0;rt_walk_probe=false;
   result=shadowSampleRays(g,count,false,false);
   assert(calls<=3u*(clamp(count,1u,16u)+2u)+1u);
   assert(std::isfinite(result.x));assert(result.x==1);assert(rt_query_refused==0);
  }
  mode=0;calls=0;result=shadowSampleRays(g,count,false,true);
  assert(calls==0);assert(result.x==0);
 }
 std::puts("actual sample loop: count clamp, restart bound, fixture bound, self-shadow PASS");
}
'''
path=args.output/'shadow_bounds.cpp'
path.write_text(fixture+structure+constants+'\n'+controls+helpers+body+'\n'+checks)
exe=args.output/'shadow_bounds'
subprocess.run(['c++','-std=c++20','-O2','-Wall','-Wextra','-Werror',str(path),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
