#!/usr/bin/env python3
"""Compile a small CPU fixture from the production renderer-handoff methods.

The generated object/binary must be placed outside the source tree.
"""
import argparse
from pathlib import Path
import subprocess


def method(text, needle):
    start = text.index(needle)
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


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
using U8 = uint8_t; using U16 = uint16_t; using U32 = uint32_t;
using S32 = int32_t; using F32 = float;
struct LLCleanSkinStore { static constexpr U32 MAX_AVATARS = 4; };
struct LLVector3 { F32 v[3]{}; LLVector3(F32 x,F32 y,F32 z):v{x,y,z}{} };
struct LLQuaternion { F32 v[4]{}; LLQuaternion(F32 x,F32 y,F32 z,F32 w):v{x,y,z,w}{} };
struct LLJoint {
    enum { GPU_EXT_DIRTY = 8 };
    U8 mGpuExtWrite = 0; U32 mDirtyFlags = 0;
    F32 morph_delta[3]{};
    F32 p[3]{}, q[4]{0,0,0,1}, s[3]{1,1,1};
    void setLocalFromAnim(const LLVector3& pos, const LLQuaternion& rot, const LLVector3& scale) {
        std::memcpy(p,pos.v,sizeof(p)); std::memcpy(q,rot.v,sizeof(q)); std::memcpy(s,scale.v,sizeof(s));
    }
};
struct LLCleanAnimStore {
    static constexpr U32 MAX_JOINTS=4, HEADER_WORDS=8, ENTRY_WORDS=32, RING=3;
    struct AvState {
        bool seeded=true, cmp_armed=true; U32 slot=1, topo_slot=1, last_root_frame=8;
        U32 follow_key=7, follow_frame=9; U32 pending_blends=123;
        std::vector<F32> seed; std::vector<LLJoint*> joints=std::vector<LLJoint*>(MAX_JOINTS);
    };
    struct ArenaDirty { U32 desc_lo=0,desc_hi=0,key_lo=0,key_hi=0; bool full=false; };
    std::unordered_map<const void*,std::unique_ptr<AvState>> mAvatars;
    std::vector<U32> mRbRecords[RING], mRbReq[RING], mCmpRefs, mCmpSlotsThisFrame;
    bool mCmpPending=true, mTopoDirty=false; U32 mCmpArmedCount=4; ArenaDirty mArenaDirty;
    void captureLocals(AvState& st, std::vector<F32>& out) const {
        out.assign(MAX_JOINTS*12u,0.f);
        for (U32 j=0;j<MAX_JOINTS;++j) if (st.joints[j]) {
            F32* d=out.data()+j*12u; LLJoint& x=*st.joints[j];
            std::memcpy(d,x.p,12); std::memcpy(d+4,x.q,16); std::memcpy(d+8,x.s,12);
        }
    }
    U32 preserveRendererLocalPoseSnapshot(const F32*,size_t,const std::vector<U32>*);
    void invalidateRendererConsumerState();
};
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--compiler", default="c++")
    args = ap.parse_args()
    source_dir = Path(__file__).resolve().parents[1]
    source = (source_dir / "llcleananimstore.cpp").read_text()
    methods = method(source, "U32 LLCleanAnimStore::preserveRendererLocalPoseSnapshot(")
    methods += "\n" + method(source, "void LLCleanAnimStore::invalidateRendererConsumerState()")
    fixture = r'''
int main() {
    LLCleanAnimStore store;
    auto state=std::make_unique<LLCleanAnimStore::AvState>();
    std::array<LLJoint,3> joints{};
    for(U32 j=0;j<3;++j) state->joints[j]=&joints[j];
    joints[0].p[0]=1.f; joints[0].mGpuExtWrite=8; joints[0].morph_delta[0]=2.f;
    // The current delta stays queued; the held frame's older delta is applied once.
    joints[1].p[0]=99.f; joints[1].mGpuExtWrite=1; joints[1].mDirtyFlags=LLJoint::GPU_EXT_DIRTY;
    joints[2].p[0]=77.f; // one-shot base write already consumed by the held frame
    auto* st=state.get(); store.mAvatars.emplace(reinterpret_cast<void*>(1),std::move(state));

    std::vector<F32> gpu(LLCleanSkinStore::MAX_AVATARS*LLCleanAnimStore::MAX_JOINTS*12u,0.f);
    for(U32 j=0;j<4;++j) { F32* p=gpu.data()+(1u*4u+j)*12u; p[0]=10.f*(j+1); p[7]=1.f; p[8]=p[9]=p[10]=1.f; }
    std::vector<U32> held(76u,0u);
    held[0]=1; held[4]=40; held[5]=76; held[8]=1; held[8+6]=0; held[8+7]=3;
    held[40]=2u | (1u << 16u); std::memcpy(&held[41],std::array<F32,10>{40.f,0.f,0.f,0.f,0.f,0.f,1.f,1.f,1.f,1.f}.data(),40);
    held[52]=0u | (8u << 16u); std::memcpy(&held[53],std::array<F32,10>{3.f,0.f,0.f,0.f,0.f,0.f,1.f,1.f,1.f,1.f}.data(),40);
    held[64]=3u | (2u << 16u); std::memcpy(&held[65],std::array<F32,10>{0.f,0.f,0.f,0.f,0.5f,0.f,0.8660254f,1.f,1.f,1.f}.data(),40);
    assert(store.preserveRendererLocalPoseSnapshot(gpu.data(),gpu.size(),&held)==1);
    assert(st->seed[0]==13.f && joints[0].p[0]==13.f);       // held delta applies once to GPU base
    assert(joints[0].mGpuExtWrite==8 && joints[0].morph_delta[0]==2.f); // newer queued delta remains
    assert(st->seed[12]==99.f && joints[1].p[0]==99.f);     // CPU base write wins
    assert(st->seed[24]==40.f && joints[2].p[0]==40.f);     // held absolute position overlays GPU pose
    assert(st->seed[36]==40.f && st->seed[41]==0.5f);       // held rotation keeps GPU position
    joints[0].p[0]=22.f; joints[0].mGpuExtWrite=1;
    assert(store.preserveRendererLocalPoseSnapshot(gpu.data(),gpu.size(),&held)==1);
    assert(st->seed[0]==22.f && joints[0].p[0]==22.f);      // newer absolute position supersedes held delta
    const auto saved=st->seed;
    for(U32 r=0;r<LLCleanAnimStore::RING;++r) { store.mRbRecords[r].push_back(1); store.mRbReq[r].push_back(2); }
    store.mCmpRefs.push_back(1); store.mCmpSlotsThisFrame.push_back(1);
    store.invalidateRendererConsumerState();
    assert(!st->seeded && st->topo_slot==0xFFFFFFFFu && st->last_root_frame==0);
    assert(st->pending_blends==123 && st->seed==saved);     // live inputs retained
    assert(store.mRbRecords[0].empty() && store.mRbReq[2].empty());
    assert(store.mCmpRefs.empty() && store.mCmpSlotsThisFrame.empty());
    assert(!store.mCmpPending && store.mTopoDirty && store.mArenaDirty.full);
}
'''
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    cpp = out / "renderer_handoff.cpp"
    exe = out / "renderer_handoff"
    cpp.write_text(PRELUDE + methods + fixture)
    subprocess.run([args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    print("PASS: extracted production pose snapshot + consumer reset; full pose, dirty-base, additive-bit, held-write, and retained-input cases")


if __name__ == "__main__":
    main()
