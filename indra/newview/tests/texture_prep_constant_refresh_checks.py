#!/usr/bin/env python3
"""Compile source-extracted constant refresh policy/producer checks without a GPU.

Uses the requested checkout's real codec selector, acceptance predicate, residency
re-encode recipe producer, demand validator, and CPU publication assignments.
Only service/encoder availability, adaptive measurements, UUIDs, and ticket plumbing
are doubles. No viewer build, GPU calls, private logs, or production-file writes.
Run against an unpatched checkout with --expect-policy-rejection as a negative control.
"""
from __future__ import annotations

import argparse
import hashlib
import pathlib
import subprocess


def braced(source: str, marker: str) -> str:
    """Extract a production declaration/lambda, ignoring comment/string braces."""
    start = source.index(marker)
    pos = source.index("{", start)
    depth, state = 0, "code"
    i = pos
    while i < len(source):
        c, n = source[i], source[i + 1:i + 2]
        if state == "code":
            if c == "/" and n in ("/", "*"):
                state = "line" if n == "/" else "block"
                i += 2
                continue
            if c in ('"', "'"):
                state = c
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    return source[start:i + 1]
        elif state == "line":
            if c == "\n":
                state = "code"
        elif state == "block":
            if c == "*" and n == "/":
                state = "code"
                i += 2
                continue
        elif c == "\\":
            i += 2
            continue
        elif c == state:
            state = "code"
        i += 1
    raise ValueError(f"unterminated production block: {marker}")


SUPPORT = r'''
#include "llvktextureprep.h"
#include <vulkan/vulkan_core.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <unordered_set>
using U32 = uint32_t;
struct LLUUID {
    uint8_t mData[16] = {};
    bool notNull() const { return mData[0] != 0; }
    bool operator!=(const LLUUID& b) const { return mData[0] != b.mData[0]; }
};
uint64_t texUuidKey(const uint8_t*) { return 1; }
struct SLNightSky { static const LLUUID& starGridId() { static LLUUID id; return id; } };
struct Encoder { bool ready() const { return true; } uint32_t minDim() const { return 32; } };
struct LLImageRaw {
    uint32_t getWidth() const { return 512; }
    uint32_t getHeight() const { return 512; }
    uint32_t getComponents() const { return 4; }
};
struct TexStageTicket {
    enum class GpuOp { None };
    LLUUID textureId;
    VkImage expectedOld = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0, levels = 1, sourceWidth = 512, sourceHeight = 512;
    uint32_t sourceFullWidth = 512, sourceFullHeight = 512, sourceComps = 4;
    uint8_t sourceEncoding = 0, sourceEncodingLevel = 0, sourceCoverageCut = 0;
    VkFormat destinationFormat = VK_FORMAT_UNDEFINED;
    struct { VkFormat format = VK_FORMAT_UNDEFINED; } candidate;
    LLVKTexturePrep::SourceKind prepKind = LLVKTexturePrep::SourceKind::PixelBytes;
    LLVKTexturePrep::Recipe prepRecipe;
};
'''

TESTS = r'''
void expect(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL " << message << '\n'; std::exit(2); }
}
int main(int argc, char**) {
    const bool negative = argc > 1;
    LLCleanRendererVK h;
    using H = LLCleanRendererVK;
    using D = H::TexStageDemand;
    using namespace LLVKTexturePrep;
    unsigned cases = 0;
    for (unsigned kind = 0; kind != 3; ++kind) {
        H::SharedTex st;
        st.image = reinterpret_cast<VkImage>(uintptr_t(1));
        st.slot[0] = 1; st.id.mData[0] = 1;
        st.w = st.h = st.levels = 1;
        st.full_w = st.full_h = st.src_w = st.src_h = st.const_w = st.const_h = 512;
        st.hold = 0x00FFFFFF;
        st.enc = TEXENC_BC7HQ; st.enc_level = 4;
        st.hud = kind != 2; st.base_srgb = kind == 2;
        st.reenc_force = kind == 0;
        h.mTexCompLevel = kind == 2 ? 0 : 4;
        expect(h.texEncFor(st, 1, 1).enc == TEXENC_NONE, "selected conversion really wants RGBA");
        expect(!h.texEncAcceptable(st), "collapsed BC requires conversion");
        h.mResReencQueued.insert(1);
        TexStageTicket ticket;
        expect(h.produce(st, ticket), "production producer schedules conversion");
        expect(ticket.width == 1 && ticket.height == 1, "producer keeps collapsed extent");
        expect(ticket.prepRecipe.kind == RecipeKind::ReducePixels, "full source reduced to one texel");
        auto decision = h.demand(st, ticket);
        std::cout << "case " << kind << " production decision " << unsigned(decision) << '\n';
        expect(decision == (negative ? D::Policy : D::Wanted), "constant same-extent refresh decision");

        // A valid full-dimension producer request must still fail the collapse policy.
        TexStageTicket growth = ticket;
        growth.width = growth.height = growth.prepRecipe.targetWidth = growth.prepRecipe.targetHeight = 512;
        growth.prepRecipe.kind = RecipeKind::ConvertPixels;
        expect(h.demand(st, growth) == D::Policy, "nonrefresh growth remains policy-rejected");
        TexStageTicket malformed = ticket;
        malformed.prepRecipe.targetStorageFormat = VK_FORMAT_UNDEFINED;
        expect(h.demand(st, malformed) == D::Recipe, "malformed refresh still rejected");
        auto changed = st; changed.const_w = changed.const_h = 256;
        expect(h.demand(changed, growth) == D::Wanted, "sharper nonconstant source may grow");
        auto placeholder = st; placeholder.image = VK_NULL_HANDLE;
        auto first = ticket; first.expectedOld = VK_NULL_HANDLE;
        expect(h.demand(placeholder, first) == D::Wanted, "constant first pixels remain allowed");

        if (!negative) {
            // A view/role change after recording must keep the forced rebuild pending.
            auto stale = st;
            stale.reenc_force = true; stale.hud = false; stale.base_srgb = !st.base_srgb;
            h.mTexCompLevel = 0;
            h.publish(stale, ticket);
            expect(h.texEncAcceptable(stale) && stale.reenc_force,
                   "acceptable codec but stale storage format retains force");
            stale = st; stale.reenc_force = true; stale.hud = false;
            h.mTexCompLevel = 4;
            h.publish(stale, ticket);
            expect(!h.texEncAcceptable(stale) && stale.reenc_force,
                   "incompatible current codec retains force");
            stale = st; stale.reenc_force = true; stale.hud = false;
            stale.normal_only = true; stale.base_srgb = false;
            auto role_ticket = ticket; role_ticket.sourceComps = 3;
            h.publish(stale, role_ticket);
            expect(!h.texEncAcceptable(stale) && stale.reenc_force,
                   "new normal-map role requiring BC5 retains force");
            h.mTexCompLevel = kind == 2 ? 0 : 4;
            h.publish(st, ticket);
            expect(st.enc == TEXENC_NONE && h.texEncAcceptable(st), "published format satisfies actual selector");
            expect(st.w == 1 && st.h == 1, "publish preserves one texel");
            expect(!st.reenc_force, "CPU publication clears completed forced refresh");
            TexStageTicket retry;
            expect(!h.produce(st, retry), "actual producer drops completed reencode key");
            expect(!h.mResReencQueued.count(1), "completed key is erased from actual queue set");
        } else {
            TexStageTicket retry;
            expect(h.produce(st, retry) && h.demand(st, retry) == D::Policy,
                   "unpublished baseline repeats producer/policy cancellation");
            expect(h.mResReencQueued.count(1), "cancelled conversion remains queued");
        }
        ++cases;
    }
    // Ordinary ready-encoder tiny color images do not automatically select NONE.
    H::SharedTex tiny; tiny.w = tiny.h = 1;
    h.mTexCompLevel = 4;
    expect(h.texEncFor(tiny, 1, 1).enc == TEXENC_BC7HQ, "ordinary tiny color remains BC7 in actual selector");
    std::cout << "PASS " << cases << " extracted producer/policy cases; "
              << (negative ? "baseline cancellation loop reproduced" : "publication converges")
              << "; growth and recipe guards preserved\n";
}
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=pathlib.Path, required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--compiler", default="/usr/bin/c++")
    parser.add_argument("--expect-policy-rejection", action="store_true")
    args = parser.parse_args()
    root, out = args.source_root.resolve(), args.out.resolve()
    if root == out or root in out.parents:
        parser.error("output must be outside the source checkout")
    cpp_path = root / "indra/newview/vkgfx/llcleanrenderervk.cpp"
    header_path = cpp_path.with_suffix(".h")
    encoder_path = cpp_path.with_name("llcleantexencvk.cpp")
    source, header, encoder = (p.read_text() for p in (cpp_path, header_path, encoder_path))
    encoder_header = cpp_path.with_name("llcleantexencvk.h").read_text()
    methods = "\n".join(braced(source, marker) for marker in (
        "LLCleanRendererVK::TexEncPick LLCleanRendererVK::pickTexEnc(",
        "LLCleanRendererVK::TexEncPick LLCleanRendererVK::texEncFor(",
        "bool LLCleanRendererVK::texEncAcceptable(",
        "uint32_t LLCleanRendererVK::texTargetSkip(",
    ))
    # Capture the actual re-encode queue's completion check and raw recipe producer.
    loop = source[source.index("for (; ri < mResReenc.size(); ++ri)"):]
    gate = loop[loop.index("const TexEncPick pk ="):loop.index("if (st.made_pass == mTexPass)")]
    block = loop[loop.index("if ((covers || (chan_lossy && have_raw)) && !keep_cov_mips)"):]
    producer = block[block.index("const uint32_t w ="):block.index("(void)startImmutableTexStage(")]
    producer += "return capture(st, tw, th, recipe, ticket);"
    # Capture publication of the CPU-prepared image's policy state, including cleanup.
    pub_start = source.index("st.enc = ticket.prepKind == LLVKTexturePrep::SourceKind::CompressedLevels")
    pub_assign = source[pub_start:source.index("\n        }", pub_start)]
    pub_extent_start = source.rindex("st.w = ticket.width;", 0, pub_start)
    pub_extent = source[pub_extent_start:source.index("        if (gpuOp !=", pub_extent_start)]
    pub_tail_start = source.index("// CPU-prepared pixels are the authoritative decode", pub_start)
    pub_end = source.index("        for (uint32_t f = 0; f < 2; ++f)", pub_tail_start)
    pub_tail = source[pub_tail_start:pub_end]
    policies = "\n".join(braced(source, marker) + ";" for marker in (
        "auto texConstCollapse =", "auto covFlat =", "auto covWant =", "auto covStale =",
        "auto texStageDemand =",
    ))
    coverage = "\n".join(braced(source, marker) + ";" for marker in (
        "auto covFlat =", "auto covWant =",
    ))
    cls = r'''
class LLCleanRendererVK {
public:
    @DECLS@
    uint32_t mTexCompLevel = 4, mTexSmallHQDim = 128;
    uint32_t mTexPass = 1;
    bool mAdaptReducedCheap = false;
    Encoder encoder; Encoder* mTexEnc = &encoder;
    struct AdaptMeas {};
    bool adaptMeas(uint64_t, bool, AdaptMeas&) const { return false; }
    unsigned adaptReject(const AdaptMeas&) const { return 0; }
    bool texTrialWanted(const SharedTex&) const { return false; }
    std::unordered_set<uint64_t> mResReencQueued;
    static constexpr uint32_t kTexFloorDim = 64;
    TexEncPick pickTexEnc(const TexEncIn&) const;
    TexEncPick texEncFor(const SharedTex&, uint32_t, uint32_t) const;
    bool texEncAcceptable(const SharedTex&) const;
    static uint32_t texTargetSkip(uint32_t, uint32_t, uint32_t, uint32_t);
    bool capture(const SharedTex& st, uint32_t tw, uint32_t th,
                 const LLVKTexturePrep::Recipe& recipe, TexStageTicket& ticket) {
        ticket.textureId = st.id; ticket.expectedOld = st.image;
        ticket.width = tw; ticket.height = th; ticket.prepRecipe = recipe;
        ticket.destinationFormat = (VkFormat)recipe.targetStorageFormat;
        ticket.candidate.format = ticket.destinationFormat;
        return true;
    }
    bool produce(SharedTex& st, TexStageTicket& ticket) {
        const uint64_t rk = 1;
        const bool cov_on = true;
        @COVERAGE@
        LLImageRaw pixels; const LLImageRaw* raw = &pixels;
        do { @GATE@ (void)want; @PRODUCER@ } while (false);
        return false;
    }
    TexStageDemand demand(const SharedTex& st, const TexStageTicket& ticket) {
        const U32 s_const_collapse = 1;
        const bool cov_on = true;
        auto texBias = [](uint32_t) { return 0u; };
        auto targetSkip = [](uint32_t w, uint32_t h, uint32_t d, uint32_t b) {
            return texTargetSkip(w, h, d, b);
        };
        @POLICIES@
        return texStageDemand(st, ticket);
    }
    void publish(SharedTex& st, const TexStageTicket& ticket) {
        const auto gpuOp = TexStageTicket::GpuOp::None;
        @PUBLISH@
    }
};
'''
    declarations = "\n".join(braced(header, m) + ";" for m in (
        "enum TexEncWhy :", "struct TexEncPick ", "struct TexEncIn\n",
        "struct SharedTex ", "enum CovWhy :", "enum class TexStageDemand :",
    ))
    for key, value in (("DECLS", declarations), ("GATE", gate), ("PRODUCER", producer),
                       ("POLICIES", policies), ("COVERAGE", coverage),
                       ("PUBLISH", pub_extent + pub_assign + "\n" + pub_tail)):
        cls = cls.replace("@" + key + "@", value)
    generated = (SUPPORT + braced(encoder_header, "enum LLTexEnc :") + ";\n"
                 + braced(encoder, "VkFormat texEncFormat(") + "\n"
                 + braced(source, "uint32_t texMipLevels(uint32_t w, uint32_t h)\n{") + "\n"
                 + cls + methods + TESTS)
    out.mkdir(parents=True, exist_ok=True)
    artifact = out / "constant_refresh.cpp"
    artifact.write_text(generated)
    for p in (cpp_path, header_path, encoder_path, cpp_path.with_name("llcleantexencvk.h"),
              cpp_path.with_name("llvktextureprep.h"), root / "indra/llrender/llvktextureprepsource.h"):
        print(f"source {p.relative_to(root)} sha256 {hashlib.sha256(p.read_bytes()).hexdigest()}", flush=True)
    exe = out / "constant_refresh"
    subprocess.run([args.compiler, "-std=c++20", "-O0", "-Wall", "-Wextra", "-Werror",
                    "-I", str(cpp_path.parent), "-I", str(root / "indra/llrender"),
                    str(artifact), "-o", str(exe)], check=True)
    subprocess.run([str(exe)] + (["negative"] if args.expect_policy_rejection else []), check=True)


if __name__ == "__main__":
    main()
