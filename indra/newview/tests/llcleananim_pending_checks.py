#!/usr/bin/env python3
"""CPU regression: production pending store, actual export methods and seed block.

Requires an explicitly granted compiler slot. Artifacts must be outside the repo.
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
    root = Path(__file__).resolve().parents[1]
    out = args.output.resolve()
    assert root not in out.parents and out != root
    out.mkdir(parents=True, exist_ok=True)
    source = (root / "llcleananimstore.cpp").read_text()
    methods = "\n".join(block(source, "void LLCleanAnimStore::AvState::" + name)
                        for name in ("beginExport()", "endExport()"))
    methods += "\n" + block(source, "void LLCleanAnimStore::AvState::emit(")
    methods += "\n" + block(source, "void LLCleanAnimStore::appendPending(")
    seed = block(source, "if (!st->seeded || st->mode == 1)")
    prelude = r'''
#include <cassert>
#include <chrono>
#include <cstdint>
#include <vector>
#include "llanim_pending.h"
using U8 = uint8_t; using U32 = uint32_t; using U64 = uint64_t; using F32 = float; using S32 = int32_t;
template<class T> T llmin(T a, T b) { return a < b ? a : b; }
constexpr S32 JSB_NUM_JOINT_STATES = 6;
struct LLVector3 { F32 mV[3] = {1,2,3}; void clearVec() { mV[0]=mV[1]=mV[2]=0; } };
struct LLQuaternion { F32 mQ[4] = {4,5,6,7}; };
struct LLJointState {
    const void* mGpuList = nullptr; S32 mGpuIndex = 0; F32 mGpuTime = 0;
    U32 getUsage() const { return 7; } F32 getWeight() const { return 1; }
    const LLVector3& getPosition() const { static LLVector3 v; return v; }
    const LLVector3& getScale() const { static LLVector3 v; return v; }
    const LLQuaternion& getRotation() const { static LLQuaternion q; return q; }
};
constexpr U32 MAX_JOINTS = 216;
struct LLJoint { U32 mGpuExtWrite = 7; LLVector3 mAnimMorphHeld{{0,0,0}}; };
struct LLVKHitch { template<class... T> static void count(T...) {} };
struct LLCleanAnimStore {
    using Pending = LLAnimPending<MAX_JOINTS, 6>;
    void appendPending(Pending&, const Pending&);
    U32 mFrame = 1; U64 mSumExportUs = 0, mSumExportIncr = 0, mSumExportFull = 0;
    struct AvState {
        using Pending = LLAnimPending<MAX_JOINTS, 6>;
        LLCleanAnimStore* store = nullptr;
        std::chrono::steady_clock::time_point t_export;
        bool export_incr = false, verifying = false, seeded = false, bound = true;
        int cap_mode = 0, mode = 2; static constexpr int CAP_NONE = 0;
        Pending pend; Pending* out = &pend; U32 pend_frame = ~0u;
        std::vector<F32> seed; std::vector<LLJoint*> joints;
        void beginExport(); void endExport();
        static void emit(Pending&, U32, LLJointState* const*, const bool*, S32);
    };
    void captureLocals(AvState&, std::vector<F32>&) { assert(false); }
};
void put(LLCleanAnimStore::AvState::Pending& p, U32 joint, float value) {
    p.hdr.insert(p.hdr.end(), {joint, U32(p.blends.size()), 2});
    const U32 index = U32(p.values.size() / 12);
    p.blends.push_back({0, 0.75f, nullptr, index, value});
    p.values.insert(p.values.end(), 12, value);
    // Keyframe references and ordered state metadata must survive compaction.
    p.blends.push_back({1, 0.25f, nullptr, 123, value + 1});
}
float latest(const LLCleanAnimStore::AvState::Pending& p, U32 joint) {
    for (size_t h = 0; h < p.hdr.size(); h += 3) if (p.hdr[h] == joint) {
        assert(p.hdr[h + 2] == 2);
        const auto& a = p.blends[p.hdr[h + 1]];
        const auto& b = p.blends[p.hdr[h + 1] + 1];
        assert(a.flags == 0 && a.weight == 0.75f && b.flags == 1 && b.weight == 0.25f);
        assert(b.index == 123 && b.time == a.time + 1);
        for (size_t c = 0; c < 12; ++c) assert(p.values[size_t(a.index) * 12 + c] == a.time);
        return a.time;
    }
    assert(false); return 0;
}
'''
    consume = r'''
void consumeSeed(LLCleanAnimStore::AvState* st, U32& seedWrites) {
    struct Avatar { U32 mGpuExtBits[7] = {}; } avatar;
    auto* av = &avatar;
    auto captureLocals = [](auto&, auto&) { assert(false); };
    auto push_base = [&](U32 j, U32 mask, const F32* values) {
        assert(mask == 7 && values[0] == float(j + 5)); ++seedWrites;
    };
'''+seed+r'''
}
int main() {
    LLCleanAnimStore store; LLCleanAnimStore::AvState state; state.store = &store;
    std::vector<LLJoint> joints(MAX_JOINTS);
    for (U32 j = 0; j < MAX_JOINTS; ++j) {
        state.joints.push_back(&joints[j]); state.seed.insert(state.seed.end(), 12, float(j + 5));
    }
    const auto originalSeed = state.seed;
    // Consumer epoch is deliberately frozen: hidden/minimized/held GPU program.
    for (U32 update = 0; update < 12001; ++update) {
        state.beginExport();
        if (!update) put(state.pend, 200, 42); // joint absent from all later exports stays live
        put(state.pend, 1, float(update));
        put(state.pend, 7, float(update + 10));
        state.endExport();
        assert(state.pend.hdr.size() == 9 && state.pend.blends.size() == 6 && state.pend.values.size() == 36);
        assert(latest(state.pend, 1) == float(update));
        assert(latest(state.pend, 7) == float(update + 10) && latest(state.pend, 200) == 42);
        assert(!state.seeded && state.seed == originalSeed); // export never consumes the one-shot seed
    }
    U32 seedWrites = 0;
    consumeSeed(&state, seedWrites); // actual buildFrame seed branch, first consumption only
    assert(state.seeded && seedWrites == MAX_JOINTS);
    consumeSeed(&state, seedWrites); assert(seedWrites == MAX_JOINTS);
    ++store.mFrame; state.beginExport(); assert(state.pend.hdr.empty());
    put(state.pend, 1, 99); state.endExport(); assert(latest(state.pend, 1) == 99);
    // Malformed data is rejected without a partial compaction/publication.
    auto bad = state.pend; bad.hdr[2] = 7; assert(!bad.compact() && bad.hdr.empty());
    // Direct forced exports: the production pre-append guard bounds transient work.
    auto& p = state.pend;
    for (U32 i = 0; i < 12001; ++i) {
        if (p.hdr.size() >= p.appendHeaderLimit) assert(p.compact());
        put(p, 1, float(i));
        assert(p.hdr.size() <= p.appendHeaderLimit);
        assert(p.blends.size() <= 2 * MAX_JOINTS * 6);
        assert(p.values.size() <= 2 * MAX_JOINTS * 6 * 12);
    }
    assert(p.compact() && p.hdr.size() == 3 && latest(p, 1) == 12000);
    // Actual emit and verification append, including the conservative864-header
    // transient. This bypass of emit-local432 is fixed-size, not an hours-long queue.
    state.beginExport(); // same consumer epoch and existing data => compact at end
    LLCleanAnimStore::Pending verified;
    LLJointState item; LLJointState* states[] = {&item}; const bool additive[] = {false};
    p.clear();
    for (U32 i = 0; i < 2 * MAX_JOINTS; ++i) {
        LLCleanAnimStore::AvState::emit(p, 3, states, additive, 1);
        LLCleanAnimStore::AvState::emit(verified, 7, states, additive, 1);
    }
    assert(p.hdr.size() == 2 * MAX_JOINTS * 3 && verified.hdr.size() == p.hdr.size());
    store.appendPending(p, verified);
    assert(p.hdr.size() == 4 * MAX_JOINTS * 3);
    state.endExport();
    assert(p.hdr.size() == 6 && p.blends.size() == 2 && p.values.size() == 24);
    assert(p.values[0] == 1 && p.values[12] == 1);
    assert(p.blends[0].index == 0 && p.blends[1].index == 1);
}
'''
    fixture = out / "pending.cpp"
    fixture.write_text(prelude + methods + consume)
    command = ["c++", "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + str(root),
               str(fixture), "-o", str(out / "pending")]
    subprocess.run(command, check=True)
    subprocess.run([str(out / "pending")], check=True)
    print("PASS: actual export/seed source, 12001 hidden updates, latest mixed payload, sparse joint retention, seed consumption, bounds")


if __name__ == "__main__":
    main()
