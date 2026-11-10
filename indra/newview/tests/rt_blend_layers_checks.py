#!/usr/bin/env python3
"""Run the actual fused blend-layer GLSL loop as scalar C++ on the CPU.

Requires c++ with AddressSanitizer and UndefinedBehaviorSanitizer. No Vulkan,
shader compilation, viewer, or GPU execution. Query, material and lighting
operations are mocks: this checks shader control flow/indexing assuming every
Proceed call returns, not hardware traversal or descriptor correctness.

Example:
  python3 indra/newview/tests/rt_blend_layers_checks.py . .validation/blend
  python3 indra/newview/tests/rt_blend_layers_checks.py . .validation/blend --legacy-counter --ref 160b955f9f

Output directory is required. Sources are extracted at runtime; --ref pins git
revisions (resolved full commit IDs are printed), otherwise the working source
is tested. Each revision runs 980 deterministic cases, including 100,000-shell
coplanar inputs and adjacent-float distances in both traversal orders.
The default also requires blend-cap telemetry. --legacy-counter checks the
historical lack of that flag when comparing revisions before the coverage fix.
"""
import argparse
import pathlib
import os
import re
import subprocess

def source(root, ref, file):
    path = 'indra/vkclean/shaders/' + file
    if ref is None:
        return (root / path).read_text()
    return subprocess.check_output(['git', 'show', ref + ':' + path], cwd=root, text=True, timeout=30)


def function(text, name):
    start = text.index('void ' + name + '(')
    masked = re.sub(r'//[^\n]*|/\*.*?\*/', lambda m: ' ' * len(m[0]), text, flags=re.S)
    brace = masked.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (masked[end] == '{') - (masked[end] == '}')
        end += 1
    return text[start:end]

support = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>
using uint = uint32_t;
using std::min; using std::max; using std::clamp; using std::abs;
struct vec3 { float x,y,z;
 vec3(float a=0):x(a),y(a),z(a){} vec3(float a,float b,float c):x(a),y(b),z(c){}
 vec3 operator-()const{return {-x,-y,-z};}
 vec3& operator+=(vec3 b){x+=b.x;y+=b.y;z+=b.z;return *this;}
};
vec3 operator+(vec3 a,vec3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
vec3 operator-(vec3 a,vec3 b){return a+-b;}
vec3 operator*(vec3 a,vec3 b){return {a.x*b.x,a.y*b.y,a.z*b.z};}
vec3 operator*(vec3 a,float b){return a*vec3(b);}
vec3 operator*(float a,vec3 b){return b*a;}
vec3 max(vec3 a,vec3 b){return {max(a.x,b.x),max(a.y,b.y),max(a.z,b.z)};}
float dot(vec3 a,vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
vec3 cross(vec3 a,vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
float inversesqrt(float a){return 1/std::sqrt(a);}
struct vec4 {float x,y,z,w;
 vec4(float a=0):x(a),y(a),z(a),w(a){} vec4(float a,float c,float d,float e):x(a),y(c),z(d),w(e){}
 vec4(vec3 a,float c):x(a.x),y(a.y),z(a.z),w(c){}
 vec3 xyz()const{return {x,y,z};} vec3 rgb()const{return xyz();}
};
struct vec2 {};
struct mat4x3 {};
struct mat3 { explicit mat3(mat4x3){} vec3 operator*(vec3 v)const{return v;} };
struct Candidate {float t=1,alpha=.999f;bool triangle=true,front=true,small=false;uint flags=256u;};
struct Instance {uint flags;};
std::vector<Candidate> stream;
std::vector<Instance> rt_instances;
bool giOn=false, prune=false;
uint initCount=0,proceedCount=0,returnedCount=0,confirmCount=0,terminateCount=0,alphaCount=0,shadowCount=0;
uint budgetLeft=48,refusedCount=0;
uint g_rc_flags=0;
std::vector<float> confirms,shadowDistances;
struct rayQueryEXT {size_t next=0,current=0;bool terminated=false;float bound=0;};
constexpr uint uTLAS=0,gl_RayFlagsNoneEXT=0,RT_MASK_BLEND=2,RT_MASK_WORLD_BLOCK=255;
constexpr uint gl_RayQueryCandidateIntersectionTriangleEXT=1;
bool rtRaySane(vec3&,float&,vec3&,float&){if(!budgetLeft){++refusedCount;return false;}--budgetLeft;return true;}
void rayQueryInitializeEXT(rayQueryEXT& q,uint,uint,uint,vec3,float tmin,vec3,float tmax){
 assert(tmin==0 && tmax>=0); q={};q.bound=tmax;++initCount;
}
bool rayQueryProceedEXT(rayQueryEXT& q){
 ++proceedCount;assert(proceedCount<1000000);
 if(q.terminated)return false;
 while(q.next<stream.size()){
  q.current=q.next++;
  if(prune && stream[q.current].t>q.bound)continue;
  ++returnedCount;return true;
 }
 return false;
}
uint rayQueryGetIntersectionTypeEXT(rayQueryEXT& q,bool){return stream[q.current].triangle?1:2;}
float rayQueryGetIntersectionTEXT(rayQueryEXT& q,bool){return stream[q.current].t;}
uint rayQueryGetIntersectionInstanceCustomIndexEXT(rayQueryEXT& q,bool){return uint(q.current);}
uint rayQueryGetIntersectionGeometryIndexEXT(rayQueryEXT&,bool){return 0;}
vec2 rayQueryGetIntersectionBarycentricsEXT(rayQueryEXT&,bool){return {};}
mat4x3 rayQueryGetIntersectionObjectToWorldEXT(rayQueryEXT&,bool){return {};}
void rayQueryTerminateEXT(rayQueryEXT& q){q.terminated=true;++terminateCount;}
void rayQueryConfirmIntersectionEXT(rayQueryEXT& q){
 ++confirmCount;float t=stream[q.current].t;confirms.push_back(t);q.bound=min(q.bound,t);
}
uint rtHitInstance(uint id,uint){return id;}
bool rtIdIn(uint id){return id<rt_instances.size();}
bool rtCandidateFront(uint id,uint,vec3,mat4x3){return stream[id].front;}
bool rtrInstSmall(uint id,mat4x3){return stream[id].small;}
float rtCandidateAlphaCol(uint id,uint,vec2,float,vec3,rayQueryEXT&,float& cutoff,bool& blend,vec3& col){
 ++alphaCount;cutoff=.5f;blend=true;col=vec3(1);return stream[id].alpha;
}
void rtTriPositions(uint,uint,vec3& a,vec3& b,vec3& c){a=vec3(0);b={1,0,0};c={0,1,0};}
vec3 atmosLightDir(){return {0,0,1};}
float atmosHdrScale(){return 1;}
bool giFrameOn(){return giOn;}
uint rtrOpts(){return 1024;}
struct {struct {uint x=1;} cfg;struct {float x=1,w=0;} prm;} gi;
struct {vec4 u_cam_time;} pc;
constexpr uint GI_F_ON=1,GI_HIT_AMB_PROBES=1,GI_HIT_AMB_CACHE=2;
uint giCacheSlots(){return 0;}
uint giCellKey(vec3,vec3,vec3,float&){return 0;}
uint giCacheFind(uint){return 0;}
bool giCacheRead(uint,vec3&){return false;}
bool atmosClassic(){return false;}
vec3 atmosLinearToSrgb(vec3 a){return a;}
vec3 offsetRayOrigin(vec3 a,vec3){return a;}
float traceShadowRay(vec3 o,vec3,float,float,uint){
 ++shadowCount;shadowDistances.push_back(o.z-.01f);
 if(!budgetLeft){++refusedCount;return 0;}--budgetLeft;return 1;
}
#define RT_BC_SITE(x)
#define RT_SHADE_STAT(a,b)
#define RT_HIT_PRIM(q,c,id) 0u
#define CLEAN_DEFER_PASS 11
'''

tests = r'''
uint cases=0;
void run(std::vector<Candidate> input,bool pruneOn,bool giEnabled,uint budget=48){
 stream=std::move(input);rt_instances.clear();for(auto c:stream)rt_instances.push_back({c.flags});
 prune=pruneOn;giOn=giEnabled;budgetLeft=budget;refusedCount=0;
 g_rc_flags=0;
 initCount=proceedCount=returnedCount=confirmCount=terminateCount=alphaCount=shadowCount=0;
 confirms.clear();shadowDistances.clear();
 uint layers=99;vec3 sum;float T;
 rtrBlendLayersT(vec3(0),vec3(0,0,1),8,vec3(1),vec3(1),.5f,layers,sum,T);
 ++cases;
 assert(initCount==(budget?1u:0u));assert(layers<=4);assert(shadowCount<=layers);
 assert(((g_rc_flags&RC_BUDGET)!=0u)==(!FIXTURE_LEGACY_COUNTER&&terminateCount!=0u));
 assert(budgetLeft<=budget);assert(std::isfinite(T)&&T>=0&&T<=1);
 for(size_t i=1;i<confirms.size();++i)assert(confirms[i]<=confirms[i-1]);
 if(std::all_of(stream.begin(),stream.end(),[](auto c){return c.triangle;})){
  assert(returnedCount<=65);assert(proceedCount<=65);assert(alphaCount<=64);
 }
}
int main(){
 for(uint n:{0u,1u,3u,4u,5u,63u,64u,65u,4096u,100000u}){
  for(bool pruning:{false,true})for(bool giEnabled:{false,true}){
   std::vector<Candidate> v(n);
   run(v,pruning,giEnabled);
   assert(alphaCount==min(n,4u));assert(confirmCount==(n>=4?1u:0u));
   if(n>=65)assert(returnedCount==65&&terminateCount==1);
   for(auto& c:v)c.alpha=0;
   run(v,pruning,giEnabled);assert(alphaCount==min(n,64u));assert(shadowCount==0);
   for(auto& c:v){c.alpha=.999f;c.flags=0;c.front=false;}
   run(v,pruning,giEnabled);assert(alphaCount==0);
   for(auto& c:v){c.flags=256;c.front=true;c.small=true;}
   run(v,pruning,giEnabled);assert(alphaCount==0);
  }
 }
 std::mt19937 rng(0x160b955f);
 for(uint n:{4u,64u,65u,4096u})for(bool pruning:{false,true}){
  std::vector<Candidate> v(n);float t=1;
  for(auto& c:v){c.t=t;t=std::nextafter(t,2.f);}
  run(v,pruning,true);assert(alphaCount==4&&confirmCount==1&&shadowCount==4);
  std::reverse(v.begin(),v.end());run(v,pruning,true);
  assert(confirmCount==0&&alphaCount==min(n,64u));
  for(uint iteration=0;iteration<100;++iteration){std::shuffle(v.begin(),v.end(),rng);run(v,pruning,true);}
 }
 std::vector<Candidate> solid(4096);for(auto& c:solid)c.alpha=1;
 run(solid,false,true);assert(shadowCount==4); // no transmission early-out, but four layers only
 run(solid,false,true,0);assert(initCount==0&&refusedCount==1);
 run(solid,false,true,1);assert(budgetLeft==0&&shadowCount==4&&refusedCount==4);
 std::vector<Candidate> nontri(100000);for(auto& c:nontri)c.triangle=false;
 run(nontri,false,false);assert(returnedCount==100000&&terminateCount==0);
 std::cout<<"PASS "<<cases<<" extracted blend cases; triangle Proceed <=65; no restart; <=4 layer-shadow calls; cap flag "
          <<(FIXTURE_LEGACY_COUNTER?"absent (legacy)":"tracks overflow")<<"\n";
 std::cout<<"CAVEAT synthetic non-triangle stream bypasses triangle counter (not a demonstrated current BLAS path)\n";
 std::cout<<"CAVEAT mocks require Proceed to return; no hardware/driver/texture execution validated\n";
}
'''

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('source_root', type=pathlib.Path)
    parser.add_argument('output_dir', type=pathlib.Path)
    parser.add_argument('--ref', action='append', help='git revision to test; repeat for comparison')
    parser.add_argument('--legacy-counter', action='store_true', help='expect the pre-fix missing blend-cap flag')
    args = parser.parse_args()
    root = args.source_root.resolve()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    for requested in args.ref or [None]:
        ref = subprocess.check_output(['git', 'rev-parse', '--verify', requested + '^{commit}'], cwd=root, text=True, timeout=30).strip() if requested else None
        label = ref[:10] if ref else 'working'
        text = source(root, ref, 'clean_deferred_light.comp')
        body = function(text, 'rtrBlendLayersT')
        (out / (label + '-original.glsl')).write_text(body + '\n')
        body = re.sub(r'\bout (uint|vec3|float) ', r'\1& ', body)
        body = body.replace('[[unroll]]', '').replace('.xyz', '.xyz()').replace('.rgb', '.rgb()')
        body = re.sub(r'\.([rgba])\b', lambda m: '.' + dict(r='x', g='y', b='z', a='w')[m[1]], body)
        body = re.sub(r'(?<![\w.])(\d+\.\d*(?:[eE][+-]?\d+)?|\d+[eE][+-]?\d+)(?![\w.])', r'\1f', body)
        common = source(root, ref, 'clean_rt_common.glsl')
        alpha = source(root, ref, 'clean_alpha_rule.glsl')
        defs = []
        for name, blob in [('RTR_BLEND_LAYERS', text), ('RTR_OCC_FAR', text), ('RC_BUDGET', text), ('RT_CANDIDATE_BUDGET', common), ('ALPHA_VISIBLE_EPS', alpha)]:
            defs.append(re.search(r'^#define ' + name + r'\s+[^\n]+', blob, re.M)[0])
        defs.append('#define FIXTURE_LEGACY_COUNTER ' + str(int(args.legacy_counter)))
        cpp = out / (label + '-blend.cpp')
        cpp.write_text(support + '\n'.join(defs) + '\n' + body + '\n' + tests)
        binary = cpp.with_suffix('')
        subprocess.run(['c++', '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', str(cpp), '-o', str(binary)], check=True, timeout=60)
        print('Revision', ref or 'working source', flush=True)
        subprocess.run([str(binary)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'}, timeout=60)


if __name__ == '__main__':
    main()
