#!/usr/bin/env python3
# Actual-source contract adapted from the original #1094 round-6 checker.
# Original checker: Sky Line. Contract updated for final ae5a243897 edge semantics.
# Run only in the parent's serial CPU slot; no viewer, simulator or network calls.
from pathlib import Path
import subprocess, sys, tempfile
root=Path(sys.argv[1]).resolve()
temporary=tempfile.TemporaryDirectory(prefix='l3-tps-contract-')
out=Path(temporary.name)/'contract.cpp'
a=(root/'indra/newview/llagent.cpp').read_text(); t=(root/'indra/newview/sltpscamera.cpp').read_text(); m=(root/'indra/newview/llviewermessage.cpp').read_text(); constants=(root/'indra/llcommon/indra_constants.h').read_text()
def fn(s, marker):
 i=s.index(marker); j=s.index('{',i); d=1; k=j+1
 while d:
  if s[k]=='{': d+=1
  elif s[k]=='}': d-=1
  k+=1
 return s[i:k]
def between(s,x,y): return s[s.index(x):s.index(y,s.index(x))]
c=between(constants,'constexpr U32 CONTROL_AT_POS_INDEX','// move these up')
source=r'''#include <cstdint>
#include <vector>
#include <cstdio>
#include <cassert>
using U64=uint64_t; using U32=uint32_t; using S32=int; using F64=double; using MASK=unsigned;
''' + c + r'''
constexpr MASK MASK_NONE=0, MASK_ALT=1, MASK_CONTROL=2; double clock_now;
struct LLFrameTimer { static double getTotalSeconds(){return clock_now;} };
struct LLCrossingTrace { static void noteSentControlFlags(U32){} };
struct Keyboard { MASK mask=0; MASK currentMask(bool){return mask;} } keyboard; Keyboard* gKeyboard=&keyboard;
struct Camera { bool ml=false; bool cameraMouselook(){return ml;} } gAgentCamera;
struct LLAgent { U32 mControlFlags=0; S32 mControlsTakenCount[32]{},mControlsTakenPassedOnCount[32]{}; U32 getControlFlags(){return mControlFlags;} void setControlFlags(U32); void clearControlFlags(U32); void resetControlFlags(); bool leftButtonGrabbed() const; bool leftButtonGrabbed(bool) const; } gAgent;
''' + '\n'.join(fn(a,'void LLAgent::'+n) for n in ('setControlFlags(', 'clearControlFlags(', 'resetControlFlags(')) + fn(a,'bool LLAgent::leftButtonGrabbed()') + fn(a,'bool LLAgent::leftButtonGrabbed(bool') + r'''
struct RlvActions { static bool blocked; static bool isRlvEnabled(){return blocked;} static bool canChangeToMouselook(){return !blocked;} }; bool RlvActions::blocked=false;
bool seated=false; unsigned aim_changes=0;
void face_aim_now(){++aim_changes;} bool isSeated(){return seated;}
struct Focus { bool focused=true; bool getAppHasFocus(){return focused;} } gFocusMgr;
struct SLToolTPS { bool captured=true; static SLToolTPS* getInstance(){static SLToolTPS t; return &t;} bool hasMouseCapture(){return captured;} };
struct SLTPSCamera { bool mLeftDown=false,mLeftFacesAim=false,mEngaged=true,allowed=true; U32 mLeftControlDown=0,mPackedNativeEdge=0;bool mDeferredNativeDown=false,mDeferredNativeUp=false; struct FireEdge{U32 flags=0;uint64_t serial=0;};static constexpr U32 kFireQueueCapacity=64; FireEdge mFireEdges[64]; U32 mFireHead=0,mFireCount=0,mWireFireDown=0,mFireOwnedDown=0;uint64_t mNextFireEdge=0,mPackedFireEdge=0;static bool exists; static bool instanceExists(){return exists;} static SLTPSCamera* getInstance(){static SLTPSCamera t; return &t;} bool canStayEngaged()const{return allowed;} void setLeftButton(bool,bool=true);void queueFireEdge(U32);void cancelFire();static bool applyFireToAgentUpdate(U32&); static void fireAgentUpdateSent(U32);}; bool SLTPSCamera::exists=true;
''' + fn(t,'void SLTPSCamera::setLeftButton(') + fn(t,'void SLTPSCamera::queueFireEdge(') + fn(t,'void SLTPSCamera::cancelFire(') + fn(t,'bool SLTPSCamera::applyFireToAgentUpdate(') + fn(t,'void SLTPSCamera::fireAgentUpdateSent(') + r'''
struct Packet { double time; U32 flags; }; std::vector<Packet> packets;
void send_agent_update(bool force_send){
 static F64 last_send_time=0; static U32 last_control_flags=0; static bool control_flags_follow_up=false;
''' + between(m,'    // compute sec_since_last_send','    LLVector3 camera_pos_agent') + r'''
 (void)raw_control_flags; (void)tps_fire_held;
 if(!send_update){ do {
''' + fn(m,'            if (control_flags_follow_up)') + r'''
 }while(false); }
 if(!send_update) return;
 packets.push_back({now,control_flags});SLTPSCamera::fireAgentUpdateSent(control_flags);
''' + between(m,'    last_send_time = now;','    last_render_state = render_state;') + r'''
}
void frame(double now,bool forced=false){clock_now=now;send_agent_update(forced);gAgent.resetControlFlags();}
void subscribe(U32 families,bool pass=false){
 for(int i=0;i<32;i++){gAgent.mControlsTakenCount[i]=0;gAgent.mControlsTakenPassedOnCount[i]=0;}
 if(families&AGENT_CONTROL_ML_LBUTTON_DOWN)(pass?gAgent.mControlsTakenPassedOnCount:gAgent.mControlsTakenCount)[CONTROL_ML_LBUTTON_DOWN_INDEX]=1;
 if(families&AGENT_CONTROL_LBUTTON_DOWN)(pass?gAgent.mControlsTakenPassedOnCount:gAgent.mControlsTakenCount)[CONTROL_LBUTTON_DOWN_INDEX]=1;
}
int main(){
 auto& tps=*SLTPSCamera::getInstance();const U32 both=AGENT_CONTROL_ML_LBUTTON_DOWN|AGENT_CONTROL_LBUTTON_DOWN,ml=AGENT_CONTROL_ML_LBUTTON_DOWN,up=both<<1,mlook=AGENT_CONTROL_MOUSELOOK;
 double now=1;
 auto reset=[&](){tps=SLTPSCamera();gAgent.mControlFlags=0;keyboard.mask=0;gFocusMgr.focused=true;SLToolTPS::getInstance()->captured=true;RlvActions::blocked=false;SLTPSCamera::exists=true;gAgentCamera.ml=false;seated=false;aim_changes=0;packets.clear();now+=10;};
 auto emit=[&](){now+=.02;frame(now,true);return packets.back().flags;};
 auto check=[&](U32 flags,U32 expected,bool context){assert((flags&(both|up))==expected);assert(bool(flags&mlook)==context);assert(!gAgentCamera.ml);assert(!(gAgent.getControlFlags()&mlook));assert(!((flags&ml)&&(flags&(ml<<1))));assert(!((flags&AGENT_CONTROL_LBUTTON_DOWN)&&(flags&AGENT_CONTROL_LBUTTON_UP)));};
 for(U32 subscribed:{ml,AGENT_CONTROL_LBUTTON_DOWN,both,U32(0)})for(bool pass:{false,true}){
  reset();subscribe(subscribed,pass);U32 expected=ml|(subscribed&AGENT_CONTROL_LBUTTON_DOWN);
  tps.setLeftButton(true);check(emit(),expected,true);
  for(int i=0;i<150;i++){
   gAgent.setControlFlags(expected<<1); // Foreign native-UP/reset activity cannot end the owned hold.
   check(emit(),0,true); // Final ae5: the simulator retains the hold, no repeated DOWN.
  }
  tps.setLeftButton(false);check(emit(),expected<<1,true);check(emit(),0,false);check(emit(),0,false);
 }
 std::puts("PASS: actual final-main press once, held neutral/context, subscription/pass_on and foreign UP suppression, release/baseline; local MLOOK unchanged");
 reset();subscribe(both);tps.setLeftButton(true);tps.setLeftButton(false);
 check(emit(),both,true);check(emit(),up,true);check(emit(),0,false);
 reset();subscribe(both);tps.setLeftButton(true);check(emit(),both,true);tps.setLeftButton(false);tps.setLeftButton(true);
 check(emit(),up,true);check(emit(),both,true);tps.setLeftButton(false);check(emit(),up,true);check(emit(),0,false);
 std::puts("PASS: quick click and release/repress use ordered DOWN and UP packets, never simultaneous pair bits");
 reset();subscribe(both);tps.setLeftButton(true);U32 snap=gAgent.getControlFlags();SLTPSCamera::applyFireToAgentUpdate(snap);gAgent.resetControlFlags(); // no transport call, so edge stays queued
 assert(tps.mFireCount==1);check(emit(),both,true);tps.setLeftButton(false);snap=0;SLTPSCamera::applyFireToAgentUpdate(snap);gAgent.resetControlFlags();assert(tps.mFireCount==1);check(emit(),up,true);check(emit(),0,false);
 std::puts("PASS: skipped/early-return sends and ephemeral reset cannot consume pending edges");
 for(int cause=0;cause<6;cause++){
  reset();subscribe(both);tps.setLeftButton(true);check(emit(),both,true);
  if(cause==0)gFocusMgr.focused=false;
  if(cause==1)SLToolTPS::getInstance()->captured=false;
  if(cause==2)tps.allowed=false;
  if(cause==3)RlvActions::blocked=true;
  if(cause==4)keyboard.mask=MASK_ALT;
  if(cause==5)keyboard.mask=MASK_CONTROL;
  check(emit(),up,true);assert(!tps.mLeftDown);
  U32 tail=emit();assert(!(tail&mlook));assert(!(tail&both));
  gFocusMgr.focused=true;SLToolTPS::getInstance()->captured=true;tps.allowed=true;RlvActions::blocked=false;keyboard.mask=0;check(emit(),0,false);
 }
 reset();subscribe(both);RlvActions::blocked=true;tps.setLeftButton(true);assert(!tps.mLeftDown);check(emit(),0,false);
 reset();subscribe(both);tps.setLeftButton(true);gFocusMgr.focused=false;check(emit(),0,false);
 std::puts("PASS: focus/capture/mode/restriction/modifier cancellation releases existing wire state, cancels unsent presses, requires a new physical press");
 reset();subscribe(both);tps.setLeftButton(true);check(emit(),both,true);
 for(int i=0;i<1000;i++){tps.setLeftButton(false);tps.setLeftButton(true);assert(tps.mFireCount<=64);}tps.setLeftButton(false);
 for(int i=0;i<70;i++){U32 f=emit();assert(!((f&ml)&&(f&(ml<<1))));}
 assert(!tps.mFireCount&&!tps.mWireFireDown);check(emit(),0,false);
 std::puts("PASS: bounded queue overflow drains to release, no allocation or stuck wire hold");
 reset();tps.mEngaged=false;gAgent.setControlFlags(mlook|ml);U32 native=gAgent.getControlFlags();assert(!SLTPSCamera::applyFireToAgentUpdate(native));assert(native==(mlook|ml));

 // Pending TPS cleanup must not consume a fresh native mouselook press.
 reset();subscribe(both);tps.setLeftButton(true);check(emit(),both,true);
 tps.setLeftButton(false);tps.mEngaged=false;gAgentCamera.ml=true;gAgent.setControlFlags(mlook|ml);
 U32 cleanup=emit();assert((cleanup&(both|up))==up && (cleanup&mlook));
 U32 next=emit();assert((next&(both|up))==ml && (next&mlook));
 U32 neutral=emit();assert(!(neutral&(both|up)) && (neutral&mlook));
 gAgent.setControlFlags(AGENT_CONTROL_ML_LBUTTON_UP);U32 nativeup=emit();assert((nativeup&AGENT_CONTROL_ML_LBUTTON_UP)&&!(nativeup&ml));
 // Native release arriving before its deferred press also remains ordered.
 reset();subscribe(both);tps.setLeftButton(true);check(emit(),both,true);
 tps.setLeftButton(false);tps.mEngaged=false;gAgentCamera.ml=true;gAgent.setControlFlags(mlook|ml);emit();
 gAgent.setControlFlags(AGENT_CONTROL_ML_LBUTTON_UP);next=emit();assert((next&(ml|(ml<<1)))==ml);next=emit();assert((next&(ml|(ml<<1)))==(ml<<1));
 std::puts("PASS: pending TPS release followed by native down survives cleanup/reset; deferred native quick release remains ordered");
 reset();subscribe(both);seated=true;tps.setLeftButton(true);assert(aim_changes==0);
 check(emit(),both,true);check(emit(),0,true);tps.setLeftButton(false);check(emit(),up,true);check(emit(),0,false);
 reset();subscribe(both);seated=true;tps.setLeftButton(true);check(emit(),both,true);
 tps.allowed=false;check(emit(),up,true);assert(!tps.mLeftDown);check(emit(),0,false);
 std::puts("PASS: existing manual-seat policy, physical release and seated camera-takeover eligibility cancellation");
 SLTPSCamera::exists=false;native=17;assert(!SLTPSCamera::applyFireToAgentUpdate(native)&&native==17);
 std::puts("PASS: native mouselook and absent TPS owner untouched; no simulator behavior modeled");
}

'''
header=(root/'indra/newview/sltpscamera.h').read_text()
assert 'kFireQueueCapacity = 64' in header, 'Production queue bound changed'
assert 'consumeTPSRightMouseUp' in fn(t,'void SLTPSCamera::disengage('), 'Beta right-release consumption lost'
assert 'SLTPSCamera::fireAgentUpdateSent(control_flags)' in m
assert m.index('SLTPSCamera::fireAgentUpdateSent(control_flags)') > m.index('gAgent.sendReliableMessage();', m.index('void send_agent_update('))
out.write_text(source)
command=['g++','-std=c++20','-O0','-Wall','-Wextra','-Werror',str(out),'-o',str(out.with_suffix(''))]
print('Source contract:', root, flush=True)
subprocess.run(command,check=True)
subprocess.run([str(out.with_suffix(''))],check=True)
temporary.cleanup()
