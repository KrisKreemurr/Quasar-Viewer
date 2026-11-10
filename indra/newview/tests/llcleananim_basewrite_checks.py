#!/usr/bin/env python3
"""Compile/run extracted production GPU morph handback and base-write code.

Pass an output directory outside the repository. Run only with the shared compile slot.
"""
import argparse
from pathlib import Path
import subprocess


def block(text, needle):
    start = text.index(needle)
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    source_root = Path(__file__).resolve().parents[1]
    source_tree = source_root.parent
    out = args.output.resolve()
    assert source_tree not in out.parents
    out.mkdir(parents=True, exist_ok=True)

    store = (source_root / "llcleananimstore.cpp").read_text()
    joint = (source_root.parent / "llcharacter/lljoint.cpp").read_text()
    shader = (source_root.parent / "vkclean/shaders/clean_anim.comp").read_text()
    base_writer = block(store, "auto base_one = [&](U32 j)")
    methods = "\n".join(
        block(joint, signature).replace("LLJoint::", "")
        for signature in (
            "void LLJoint::queueGpuMorphDelta(const LLVector3& delta)",
            "void LLJoint::morphPosDelta(const LLVector3& delta)",
            "void LLJoint::notePosAnimated()",
            "void LLJoint::endPosEvaluation()",
        )
    )
    assert "if ((mask & 8u) != 0u) s_pos[jm]  += vec3" in shader
    assert "const LLVector3 held(mAnimMorphHeld);" in joint
    assert "mGpuExtWrite &= (U8)~8u;" in joint
    assert "seeded[0] += delta.mV[0]" in store
    assert "const bool write_local = !((jp->mGpuExtWrite & 7u) || jp->mGpuExtFrame > rec.frame);" in store

    fixture = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
using U32=uint32_t; using U8=uint8_t; using U16=uint16_t; using F32=float;
enum : U32 { MATRIX_DIRTY=1u, POSITION_DIRTY=2u, GPU_EXT_DIRTY=4u };
struct LLVector3 {
    F32 mV[3]{};
    bool isExactlyZero() const { return mV[0]==0.f && mV[1]==0.f && mV[2]==0.f; }
    void clearVec() { mV[0]=mV[1]=mV[2]=0.f; }
    LLVector3& operator+=(const LLVector3& b) { for (U32 i=0;i<3;++i) mV[i]+=b.mV[i]; return *this; }
};
LLVector3 operator+(const LLVector3& a,const LLVector3& b) {
    return {{a.mV[0]+b.mV[0],a.mV[1]+b.mV[1],a.mV[2]+b.mV[2]}};
}
bool operator==(const LLVector3& a,const LLVector3& b) {
    return a.mV[0]==b.mV[0] && a.mV[1]==b.mV[1] && a.mV[2]==b.mV[2];
}
struct LLQuaternion { F32 mQ[4]{}; };
bool operator==(const LLQuaternion& a,const LLQuaternion& b) {
    for (U32 i=0;i<4;++i) if (a.mQ[i]!=b.mQ[i]) return false;
    return true;
}
struct Xform {
    LLVector3 p,s; LLQuaternion r;
    const LLVector3& getPosition() const { return p; }
    const LLQuaternion& getRotation() const { return r; }
    const LLVector3& getScale() const { return s; }
    void setPosition(const LLVector3& v) { p=v; }
};
using LLXformMatrix=Xform;
struct LLJoint {
    inline static bool sAnimApply=false;
    inline static U32 sFrameCounter=1;
    LLJoint* mGpuRoot=nullptr;
    U32* mGpuExtMask=nullptr; U16 mGpuBit=0;
    U8 mGpuExtWrite=0; U32 mGpuExtFrame=0,mDirtyFlags=0,wants=0;
    LLVector3 mAnimMorphHeld; bool mPosAnimated=false,mPosAnimatedPrev=false;
    Xform mXform;
    void touch(U32 flags) { mDirtyFlags|=flags; }
    void gpuWant() { ++wants; }
    const LLVector3& getPosition() { return mXform.getPosition(); }
    Xform* getXform() { return &mXform; }
    void setPosition(const LLVector3& requested) {
        const bool ext=!sAnimApply;
        const bool replace_delta=ext && mGpuRoot && ((mGpuExtWrite&8u) || !mAnimMorphHeld.isExactlyZero());
        if (requested==getPosition() || replace_delta) {
            if (!replace_delta) return;
        }
        mXform.setPosition(requested);
        if (ext && mGpuRoot) {
            if (replace_delta) { mGpuExtWrite &= (U8)~8u; mAnimMorphHeld.clearVec(); }
            mGpuExtWrite |= 1u;
            mGpuExtFrame=sFrameCounter;
            if (mGpuExtMask) mGpuExtMask[mGpuBit>>5] |= 1u << (mGpuBit&31u);
        }
    }
'''+methods+r'''
};
struct State { std::vector<LLJoint*> joints; };
struct Record { U32 j=0,mask=0; F32 v[12]{}; };
int main() {
    const LLVector3 stale{{2.f,3.f,4.f}}, delta{{0.25f,-0.5f,0.75f}};
    const LLQuaternion rotation{{0.f,0.f,0.f,1.f}};
    const LLVector3 scale{{1.f,1.f,1.f}};
    LLJoint root,joint; U32 ext=0; joint.mGpuRoot=&root; joint.mGpuExtMask=&ext;
    joint.mXform.p=stale; joint.mXform.r=rotation; joint.mXform.s=scale;
    LLJoint::sFrameCounter=100;
    // Undriven joint, no readback update between two nonzero morph/physics deltas.
    joint.morphPosDelta(delta); joint.morphPosDelta(delta);
    assert(joint.mGpuExtWrite==8u && joint.mAnimMorphHeld==delta+delta);
    assert(joint.mGpuExtFrame==0u && !(joint.mDirtyFlags&GPU_EXT_DIRTY) && joint.wants==2u);
    // Older GPU readback may advance the CPU local while the unconsumed additive payload
    // remains pending. This is the production consumeReadback eligibility predicate.
    const U32 readback_frame=101u;
    const bool write_local=!((joint.mGpuExtWrite&7u) || joint.mGpuExtFrame>readback_frame);
    assert(write_local);
    if (write_local) joint.mXform.p={{8.f,9.f,10.f}};
    const LLVector3 readback_pose{{8.f,9.f,10.f}};
    assert(joint.mXform.p==readback_pose && joint.mGpuExtWrite==8u
           && joint.mAnimMorphHeld==delta+delta);
    State state; state.joints.push_back(&joint); State* st=&state; Record record; U32 pushes=0;
    auto push_base=[&](U32 j,U32 mask,const F32* values) {
        ++pushes; record.j=j; record.mask=mask;
        for (U32 i=0;i<12;++i) record.v[i]=values[i];
    };
    F32 v[12];
'''+base_writer+r''';
    base_one(0);
    assert(record.mask==8u && record.v[0]==0.5f && record.v[1]==-1.f && record.v[2]==1.5f);
    assert(joint.mGpuExtWrite==0u && joint.mAnimMorphHeld.isExactlyZero());
    // Apply the exact mask operation used by clean_anim.comp to a newer GPU local pose.
    F32 gpu_pos[3]={10.f,20.f,30.f};
    if ((record.mask&1u)!=0u) for (U32 i=0;i<3;++i) gpu_pos[i]=record.v[i];
    if ((record.mask&8u)!=0u) for (U32 i=0;i<3;++i) gpu_pos[i]+=record.v[i];
    assert(gpu_pos[0]==10.5f && gpu_pos[1]==19.f && gpu_pos[2]==31.5f);
    base_one(0); assert(pushes==1u); // consumed exactly once

    // Absolute writes win in either ordering; later deltas fold into the absolute.
    joint.mGpuExtWrite=8u; joint.mAnimMorphHeld=delta; joint.mXform.p=stale;
    joint.setPosition({{7.f,8.f,9.f}});
    assert(joint.mGpuExtWrite==1u && joint.mAnimMorphHeld.isExactlyZero());
    joint.mGpuExtWrite=1u; joint.mXform.p={{7.f,8.f,9.f}};
    joint.queueGpuMorphDelta(delta);
    const LLVector3 absolute_plus_delta{{7.25f,7.5f,9.75f}};
    assert(joint.mGpuExtWrite==1u && joint.mXform.p==absolute_plus_delta);

    // One-shot release must transfer the held value once, without turning the stale
    // CPU local into an absolute position or changing rotation.
    joint.mGpuExtWrite=0; ext=0; joint.mXform.p=stale; joint.mXform.r=rotation;
    joint.mAnimMorphHeld.clearVec(); joint.mPosAnimated=false; joint.mPosAnimatedPrev=false;
    joint.notePosAnimated(); joint.morphPosDelta(delta); joint.morphPosDelta(delta);
    joint.endPosEvaluation(); joint.endPosEvaluation();
    assert(joint.mGpuExtWrite==8u && joint.mAnimMorphHeld==delta+delta);
    assert(joint.mXform.p==stale && joint.mXform.r==rotation && (ext&1u));
    // CPU-only viewers retain the previous LL absolute update behavior.
    LLJoint cpu; cpu.mXform.p=stale; cpu.mXform.r=rotation; cpu.morphPosDelta(delta);
    assert(cpu.mXform.p==stale+delta && cpu.mXform.r==rotation && cpu.mGpuExtWrite==0u);
}
'''
    cpp=out/"basewrite.cpp"
    cpp.write_text(fixture)
    exe=out/"basewrite"
    subprocess.run(["c++","-std=c++17","-O1","-Wall","-Wextra","-Werror",str(cpp),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
    print("PASS: actual morph/end handback and base_one; delayed-readback accumulation, one-shot delta, precedence, CPU behavior, stable rotation")


if __name__ == "__main__":
    main()
