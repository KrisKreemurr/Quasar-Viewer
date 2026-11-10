#!/usr/bin/env python3
"""Focused source-extracted test for queued morph deltas across renderer reset.

This fixture composes the production pose snapshot/reset methods with the actual
full-seed block and incremental base-write lambda from LLCleanAnimStore::buildFrame.
The generated C++ and executable belong in an external output directory.
"""
import argparse
from pathlib import Path
import subprocess


def braced(source: str, marker: str) -> str:
    start = source.index(marker)
    opening = source.index("{", start)
    depth = 1
    pos = opening + 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[start:pos]


PRELUDE = r'''
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <unordered_map>
#include <vector>
using U8=uint8_t; using U16=uint16_t; using U32=uint32_t;
using S32=int32_t; using F32=float;
struct LLCleanSkinStore { static constexpr U32 MAX_AVATARS=2; };
struct LLVector3 {
    F32 mV[3]{};
    LLVector3()=default;
    LLVector3(F32 x,F32 y,F32 z):mV{x,y,z}{}
    void clearVec(){mV[0]=mV[1]=mV[2]=0.f;}
};
struct LLQuaternion { F32 mQ[4]{}; LLQuaternion(F32 x,F32 y,F32 z,F32 w):mQ{x,y,z,w}{} };
struct LLXformMatrix {
    LLVector3 p{0,0,0}, s{1,1,1}; LLQuaternion r{0,0,0,1};
    const LLVector3& getPosition()const{return p;}
    const LLQuaternion& getRotation()const{return r;}
    const LLVector3& getScale()const{return s;}
};
struct LLJoint {
    U8 mGpuExtWrite=0; U32 mDirtyFlags=0;
    LLVector3 mAnimMorphHeld;
    LLXformMatrix x;
    LLXformMatrix* getXform(){return &x;}
    void setLocalFromAnim(const LLVector3& p,const LLQuaternion& r,const LLVector3& s){x.p=p;x.r=r;x.s=s;}
};
struct LLVOAvatar;
struct LLCleanAnimStore {
    static constexpr U32 MAX_JOINTS=4, HEADER_WORDS=8, ENTRY_WORDS=32, RING=3;
    struct AvState {
        bool seeded=true, cmp_armed=false, bound=false;
        U8 mode=2; U32 slot=1, topo_slot=1, last_root_frame=3;
        U32 follow_key=7, follow_frame=9;
        std::vector<F32> seed;
        std::vector<LLJoint*> joints=std::vector<LLJoint*>(MAX_JOINTS);
    };
    struct ArenaDirty { U32 desc_lo=0,desc_hi=0,key_lo=0,key_hi=0; bool full=false; };
    std::unordered_map<const void*,std::unique_ptr<AvState>> mAvatars;
    std::vector<U32> mRbRecords[RING],mRbReq[RING],mCmpRefs,mCmpSlotsThisFrame;
    bool mCmpPending=false,mTopoDirty=false; U32 mCmpArmedCount=0; ArenaDirty mArenaDirty;
    void captureLocals(AvState& st,std::vector<F32>& out)const {
        out.assign(MAX_JOINTS*12u,0.f);
        for(U32 j=0;j<MAX_JOINTS;++j) if(st.joints[j]) {
            F32* d=out.data()+j*12u; const auto& x=st.joints[j]->x;
            std::memcpy(d,x.p.mV,12); std::memcpy(d+4,x.r.mQ,16); std::memcpy(d+8,x.s.mV,12);
        }
    }
    U32 preserveRendererLocalPoseSnapshot(const F32*,size_t,const std::vector<U32>*);
    void invalidateRendererConsumerState();
    void runFullSeed(AvState*,LLVOAvatar*,std::vector<U32>&);
    void runNextBaseWrite(AvState*,std::vector<U32>&);
};
struct LLVOAvatar { U32 mGpuExtBits[7]{}; };
'''


def main() -> None:
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source-root",type=Path,default=Path(__file__).resolve().parents[3],
                    help="source checkout (defaults to this test's repository root)")
    ap.add_argument("--revision",default=None,
                    help="optional git revision to extract instead of the source-root working tree")
    ap.add_argument("--output",required=True,type=Path)
    ap.add_argument("--compiler",default="c++")
    ap.add_argument("--run",action="store_true",
                    help="compile with strict warnings and execute (wait for the serial CPU token)")
    args=ap.parse_args()
    def source_file(path: str) -> str:
        if args.revision:
            return subprocess.run(["git","-C",str(args.source_root),"show",f"{args.revision}:{path}"],
                                  check=True,text=True,capture_output=True).stdout
        return (args.source_root/path).read_text()
    source=source_file("indra/newview/llcleananimstore.cpp")
    preserve=braced(source,"U32 LLCleanAnimStore::preserveRendererLocalPoseSnapshot(")
    invalidate=braced(source,"void LLCleanAnimStore::invalidateRendererConsumerState()")
    seed_block=braced(source,"if (!st->seeded || st->mode == 1)")
    base_lambda=braced(source,"auto base_one = [&](U32 j) {")+";"

    fixture=r'''
int main(){
    LLCleanAnimStore store;
    auto owned=std::make_unique<LLCleanAnimStore::AvState>();
    LLJoint joint; joint.x.p=LLVector3(1,0,0); joint.x.r=LLQuaternion(0,0,0,1);
    owned->joints[0]=&joint; owned->seed.assign(LLCleanAnimStore::MAX_JOINTS*12u,0.f);
    owned->seed[7]=1.f; owned->seed[8]=owned->seed[9]=owned->seed[10]=1.f;
    auto* st=owned.get(); store.mAvatars.emplace(reinterpret_cast<void*>(1),std::move(owned));

    // Frame A was prepared but never dispatched: its +3 delta exists only in held words.
    // A later physics tick queues +4 in the live joint while A remains held.
    std::vector<F32> gpu(LLCleanSkinStore::MAX_AVATARS*LLCleanAnimStore::MAX_JOINTS*12u,0.f);
    for(U32 j=0;j<LLCleanAnimStore::MAX_JOINTS;++j){F32* p=gpu.data()+(1u*4u+j)*12u;p[0]=10.f;p[7]=1.f;p[8]=p[9]=p[10]=1.f;}
    std::vector<U32> held(52u,0u);
    held[0]=1; held[4]=40; held[5]=52; held[8]=1; held[14]=0; held[15]=1;
    held[40]=0u|(8u<<16u);
    const F32 oldDelta[10]={3.f,0.f,0.f,0.f,0.f,0.f,1.f,1.f,1.f,1.f};
    std::memcpy(&held[41],oldDelta,sizeof(oldDelta));
    joint.mAnimMorphHeld=LLVector3(4,0,0); joint.mGpuExtWrite=8;

    assert(store.preserveRendererLocalPoseSnapshot(gpu.data(),gpu.size(),&held)==1);
    assert(st->seed[0]==13.f && joint.x.p.mV[0]==13.f); // GPU 10 + held-frame delta 3
    assert(joint.mGpuExtWrite==8 && joint.mAnimMorphHeld.mV[0]==4.f); // later delta still queued
    store.invalidateRendererConsumerState();
    assert(!st->seeded && joint.mGpuExtWrite==8 && joint.mAnimMorphHeld.mV[0]==4.f);

    // Execute the exact production full-seed block against the preserved seed.
    LLVOAvatar av; std::vector<U32> basew;
    store.runFullSeed(st,&av,basew);
    assert(st->seeded && joint.mGpuExtWrite==0 && joint.mAnimMorphHeld.mV[0]==0.f);
    assert(basew.size()==12 && (basew[0]>>16u)==7u);
    F32 gpuAfterSeed=0.f; std::memcpy(&gpuAfterSeed,&basew[1],sizeof(F32));
    assert(gpuAfterSeed==17.f); // old +3 and newer +4 each applied once

    // Next ordinary frame: execute the production incremental-write lambda. With the
    // pending delta cleared, it must emit no second write and leave the pose at +17.
    basew.clear(); store.runNextBaseWrite(st,basew);
    assert(basew.empty() && joint.mGpuExtWrite==0 && joint.mAnimMorphHeld.mV[0]==0.f);
    assert(gpuAfterSeed==17.f);
}
'''.replace("{ seed_block }",seed_block).replace("{ base_lambda }",base_lambda)
    out=args.output.resolve(); out.mkdir(parents=True,exist_ok=True)
    wrappers=f'''
void LLCleanAnimStore::runFullSeed(AvState* st,LLVOAvatar* av,std::vector<U32>& basew) {{
    const U32 MAX_JOINTS=LLCleanAnimStore::MAX_JOINTS;
    auto push_base=[&](U32 j,U32 mask,const F32* v){{
        U32 row[12]{{}}; row[0]=j|(mask<<16u); std::memcpy(row+1,v,11u*sizeof(U32));
        basew.insert(basew.end(),row,row+12);
    }};
    {seed_block}
}}
void LLCleanAnimStore::runNextBaseWrite(AvState* st,std::vector<U32>& basew) {{
    F32 v[12]{{}};
    auto push_base=[&](U32 j,U32 mask,const F32* value){{
        U32 row[12]{{}}; row[0]=j|(mask<<16u); std::memcpy(row+1,value,11u*sizeof(U32));
        basew.insert(basew.end(),row,row+12);
    }};
    {base_lambda}
    base_one(0);
}}
'''
    cpp=out/"joint_sequence.cpp"; cpp.write_text(PRELUDE+preserve+"\n"+invalidate+"\n"+wrappers+"\n"+fixture)
    source_label=args.revision or str(args.source_root.resolve())
    print(f"SOURCE READY: extracted preserve/reset/full-seed/base-one from {source_label} to {cpp}")
    if args.run:
        exe=out/"joint_sequence"
        subprocess.run([args.compiler,"-std=c++17","-O1","-Wall","-Wextra","-Werror",str(cpp),"-o",str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
        print("PASS: held +3 and queued +4 cross reset; full seed emits 17 once; next incremental frame emits no duplicate")


if __name__=="__main__":
    main()
