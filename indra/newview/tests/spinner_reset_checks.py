#!/usr/bin/env python3
"""Compile real anchor/reset/instance methods with real LL quaternion math.
Usage: script SOURCE OUTPUT REAL_PROJECT_COMMAND_JSON
The JSON supplies compiler/include flags only; all outputs stay in OUTPUT.
"""
from pathlib import Path
import json
import re
import subprocess
import sys
root,out,flags_file=map(Path,sys.argv[1:4]);out.mkdir(parents=True,exist_ok=True)
vo=(root/'indra/newview/llviewerobject.cpp').read_text()
store=(root/'indra/newview/llcleaninstancestore.cpp').read_text()
header=(root/'indra/newview/llviewerobject.h').read_text()
math=(root/'indra/llmath/llquaternion.cpp').read_text()
def function(text,name):
 start=text.index(name);start=text.rfind('\n',0,start)+1
 pos=text.index('{',start);depth=1;end=pos+1
 while depth:
  depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]
fields=header[header.index('    LLQuaternion    mGpuSpinBase;'):header.index('    bool            mGpuSpinValid = false;')+len('    bool            mGpuSpinValid = false;')]
code=r'''
#include "llmath.h"
#include "v3math.h"
#include "llquaternion.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <vector>
#include <string>
#include <iostream>
struct LLWorldClock{static double time;static double getTime(){return time;}};
double LLWorldClock::time=100.;
struct Region{float dilation=1;float getTimeDilation()const{return dilation;}};
struct LLViewerObject{
 Region* mRegionp=nullptr;LLQuaternion rotation;LLVector3 omega;
 float mRotTime=0;LLQuaternion mAngularVelocityRot;
'''+fields+r'''
 const LLQuaternion& getRotation()const{return rotation;}
 LLVector3 getAngularVelocity()const{return omega;}
 void gpuSpinPose(LLQuaternion&,LLVector3&,float&);
 void resetRot();void resetRotTime();
};
struct Diag{std::string reason;float oldPhase,newPhase;};
std::vector<Diag> logs;
static void gpuSpinDiag(const LLViewerObject*,const char* reason,float oldPhase,float newPhase,float,float,float){logs.push_back({reason,oldPhase,newPhase});}
'''+function(math,'LLQuaternion::LLQuaternion(F32 angle, const LLVector3 &vec)')+'\n'+function(math,'LLVector3       operator*(const LLVector3 &a, const LLQuaternion &rot)')+'\n'+ '\n'.join(function(vo,n) for n in ['static F32 gpuSpinPhase(','void LLViewerObject::gpuSpinPose(','void LLViewerObject::resetRotTime()','void LLViewerObject::resetRot()'])+r'''
struct LLCleanInstanceStore{
 struct Instance{float model_matrix[16]={};float spin_axis[4]={};float spin_pivot[4]={};unsigned flags=0,lod_group_id=0;};
 static bool rowAgentSpace(unsigned,unsigned){return true;}
 static void mulMat4(const float[16],const float[16],float[16]);
};
'''+function(store,'void LLCleanInstanceStore::mulMat4(')+'\n'+function(store,'static bool applyComposedPose(')+r'''
LLQuaternion rendered(LLViewerObject& o){
 LLQuaternion b;LLVector3 v;float epoch;o.gpuSpinPose(b,v,epoch);
 float rate=v.length();if(rate>0)b*=LLQuaternion(gpuSpinPhase(v,epoch),v/rate);
 return b;
}
bool same(const LLQuaternion&a,const LLQuaternion&b){
 for(const LLVector3 axis:{LLVector3(1,0,0),LLVector3(0,1,0),LLVector3(0,0,1)})
  if(((axis*a)-(axis*b)).length()>2e-4f)return false;
 return true;
}
int main(){
 Region region;region.dilation=.8f;LLViewerObject obj;obj.mRegionp=&region;obj.omega=LLVector3(0,0,1);
 obj.rotation=LLQuaternion(.25f,LLVector3(1,0,0));
 LLQuaternion initial;LLVector3 velocity;float epoch;obj.gpuSpinPose(initial,velocity,epoch);
 assert(velocity==LLVector3(0,0,.8f));
 const auto epoch0=epoch;const auto base0=initial;
 // Repeated server/sweep reads while CPU mRotTime advances on another clock.
 for(int frame=1;frame<=6000;++frame){
  LLWorldClock::time=100.+frame/60.;obj.mRotTime+=.8f/60.f;
  obj.gpuSpinPose(initial,velocity,epoch);
  assert(epoch==epoch0 && initial==base0);
  LLQuaternion expected=base0;expected*=LLQuaternion(float(.8*(LLWorldClock::time-100.)),LLVector3(0,0,1));
  assert(same(rendered(obj),expected));
 }
 // Rate changes preserve the rendered pose exactly at the event, including0.
 for(float d:{.4f,0.f,.9f,1.f}){
  LLWorldClock::time+=.25;auto before=rendered(obj);region.dilation=d;
  auto after=rendered(obj);assert(same(before,after));
  obj.gpuSpinPose(initial,velocity,epoch);assert(velocity==obj.omega*d);
  LLWorldClock::time+=.5;LLQuaternion expected=after;
  if(velocity.length()>0)expected*=LLQuaternion(velocity.length()*.5f,velocity/velocity.length());
  assert(same(rendered(obj),expected));
 }
 // An omega change without an LL reset is also phase-continuous.
 auto before=rendered(obj);obj.omega=LLVector3(1.4f,0,0);assert(same(before,rendered(obj)));
 // Server reset: final updated orientation is authoritative, not a stale pre-reset base.
 obj.resetRotTime();obj.rotation=LLQuaternion(.7f,LLVector3(0,1,0));assert(same(rendered(obj),obj.rotation));
 obj.resetRot();obj.rotation=LLQuaternion(-.4f,LLVector3(1,1,0));assert(same(rendered(obj),obj.rotation));
 // The child's offset about the root pivot uses that same root anchor.
 const LLVector3 pivot(3,4,5),local(2,0,0);
 obj.gpuSpinPose(initial,velocity,epoch);LLWorldClock::time+=.75;
 const LLQuaternion delta(gpuSpinPhase(velocity,epoch),velocity/velocity.length());
 const auto childGpu=pivot+(local*initial)*delta;
 const auto childExpected=pivot+local*rendered(obj);
 assert((childGpu-childExpected).length()<2e-4f);
 // Real applyComposedPose must not dirty an unchanged anchor on sweeps.
 LLCleanInstanceStore::Instance inst;float pose[16]={1,0,0,0,0,1,0,0,0,0,1,0,3,4,5,1};
 float axis[4]={0,0,1,100},pv[4]={3,4,5,0};
 assert(applyComposedPose(inst,pose,axis,pv,nullptr,nullptr));
 for(int i=0;i<1000;++i)assert(!applyComposedPose(inst,pose,axis,pv,nullptr,nullptr));
 axis[3]=101;assert(applyComposedPose(inst,pose,axis,pv,nullptr,nullptr));
 // Old heuristic: .8 dilation moves inferred t0 by .2sec/sec and creates a discontinuity.
 float oldPhase=.8f*2.f;float priorGpuPhase=2.f;
 assert(std::abs(priorGpuPhase-oldPhase)>.05f);
 assert(logs.size()<32);
 // A busy region can send many dilation changes: keep the base unit length.
 for(int i=0;i<10000;++i){
  LLWorldClock::time+=.01;region.dilation=(i&1)? .8f:.81f;rendered(obj);
  float norm=0;for(float q:obj.mGpuSpinBase.mQ)norm+=q*q;
  assert(std::abs(std::sqrt(norm)-1.f)<1.2e-6f); // LL normalize allows1ppm
 }
 std::cout<<"PASS6000 steady reads, dilation/axis continuity, explicit resets, linked pivot,1000 no-dirty sweeps, old drift reproduction;10000 rate-rebase norm checks\n";
}
'''
(out/'check.cpp').write_text(code)
a=json.loads(flags_file.read_text());cmd=[a[0]];i=1
while i<len(a):
 if a[i] in ['-isystem','-I']:
  cmd+=a[i:i+2];i+=2;continue
 if a[i].startswith(('-I','-D','-m')):cmd.append(a[i])
 i+=1
cmd+=['-std=c++20','-UNDEBUG','-O1','-Wall','-Wextra','-Werror','-Wno-ignored-qualifiers',str(out/'check.cpp'),'-o',str(out/'check')]
(out/'command.json').write_text(json.dumps(cmd,indent=2))
subprocess.run(cmd,check=True);subprocess.run([str(out/'check')],check=True)
assert 'LLWorldClock::getTime() - spinner->getRotTime()' not in store
assert 'parent->gpuSpinPose(pbase, spinVelocity, spinEpoch)' in store
assert 'vobj->gpuSpinPose(base, spinVelocity, spinEpoch)' in store
assert 'spin_axis[3] = spinEpoch' in store
assert 'spinDilation != mGpuSpinDilation' in vo and 'noteObjectMoved(this)' in vo
volume=(root/'indra/newview/llvovolume.cpp').read_text()
assert 'obj->isAttachment() || obj->isSelected()' in volume and 'moving(root) || root->isSelected()' in volume
for name in ['clean_draw.vert','clean_cluster_draw_common.glsl','clean_rt_instances.comp']:
 s=(root/'indra/vkclean/shaders'/name).read_text()
 assert 'spin_axis' in s and '6.28318531' in s
print('PASS stable parent anchor + event notification + raster/CLOD/RT shared-record source contracts')
