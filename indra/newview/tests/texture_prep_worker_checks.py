#!/usr/bin/env python3
"""Compile the actual shared texture-preparation worker body in a CPU fixture.

The fixture models a producer update to the bottom-right texel, then supplies the
full immutable snapshot required by the current no-patch-GPU-copy path. It also
proves that a rectangle snapshot is refused and that emitted bytes cannot be
labelled with an incompatible VkFormat. Worker functions are extracted verbatim
from llvktextureprepworker.cpp; only Vulkan format/size endpoints are supplied.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import shlex
import subprocess
import sys


def extract_braced(source: str, marker: str) -> str:
    start = source.find(marker)
    if start < 0:
        raise RuntimeError(f"missing source marker: {marker}")
    brace = source.find("{", start)
    depth = 0
    state = "code"
    i = brace
    while i < len(source):
        c = source[i]
        n = source[i + 1] if i + 1 < len(source) else ""
        if state == "code":
            if c == "/" and n == "/": state = "line"; i += 2; continue
            if c == "/" and n == "*": state = "block"; i += 2; continue
            if c == '"': state = "string"
            elif c == "'": state = "char"
            elif c == "{": depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0: return source[start:i + 1]
        elif state == "line":
            if c == "\n": state = "code"
        elif state == "block":
            if c == "*" and n == "/": state = "code"; i += 2; continue
        elif state in ("string", "char"):
            if c == "\\": i += 2; continue
            if (state == "string" and c == '"') or (state == "char" and c == "'"):
                state = "code"
        i += 1
    raise RuntimeError(f"unterminated body: {marker}")


def sha(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=pathlib.Path, required=True)
    parser.add_argument("--producer-root", type=pathlib.Path,
                        help="optional producer worktree used to verify the full-snapshot call")
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--run", action="store_true")
    args = parser.parse_args()
    root, out = args.source_root.resolve(), args.out.resolve()
    if out == root:
        raise SystemExit("output must be outside the source tree")
    cpp_path = root / "indra/newview/vkgfx/llvktextureprepworker.cpp"
    worker_source = cpp_path.read_text()
    source_path = root / "indra/newview/vkgfx/llcleanrenderervk.cpp"
    source = source_path.read_text()
    worker_functions = [extract_braced(worker_source, marker) for marker in (
        "uint32_t mipLevels(", "bool mul(", "bool pixelBytes(",
        "uint32_t uiBytesPerPixel(", "uint32_t uiVkFormat(", "UiSwizzle uiSwizzle(",
        "bool uiSourceRecipeValid(",
        "bool prepareUiPixels(", "void expandPixel(", "bool reduceRGBA(",
        "void histogram(", "double coverageAt(", "bool buildCoverage(", "bool workspaceSize(",
        "bool prepareCompressedPayload(", "bool prepareTextureInput(",
        "bool preparedTextureOutputMatches(")]
    start_method = extract_braced(source, "LLCleanRendererVK::TexStageStartResult LLCleanRendererVK::startImmutableTexStage(")
    media_method = extract_braced(source, "void LLCleanRendererVK::recordMediaUpdates(")
    resolve_method = extract_braced(source, "auto resolve_tex = [&](const LLUUID& tid, bool srgb, int role = ROLE_COLOR)")
    shared_pixels = extract_braced(source, "auto queueSharedPixels = [&](SharedTex& st, LLTextureData* source, LLImageRaw* pixels,")
    if "captureVKPrepSource(-1," not in start_method:
        raise RuntimeError("current route must request a full producer snapshot until GPU patch-copy exists")
    if "batch.add(" in resolve_method:
        raise RuntimeError("material resolution must not stage source pixels through TexUploadBatch")
    if "mStageRing" in source or "vk_texStageWait" in source or "mStageSync" in source:
        raise RuntimeError("legacy per-renderer staging ring / synchronous stage wait remains")
    if "static bool prepareTextureInput(" in source:
        raise RuntimeError("CPU pixel worker must have one implementation in llvktextureprepworker.cpp")
    if "void boxReduceRGBA(" in source or "void buildCoverageChainBytes(" in source:
        raise RuntimeError("renderer must not retain duplicate CPU mip/coverage implementations")
    if "dyn_blank" in shared_pixels:
        raise RuntimeError("GPU-only composites must not fabricate CPU placeholder input")
    if "ticket.gpuOnlyMedia = true" not in media_method or "TexStageTicket::Life::Recorded" not in media_method:
        raise RuntimeError("GPU-only composites must use a bounded acquired-frame GPU ticket")
    if "if (bigger && is_media)" not in resolve_method or "startImmutableMediaStage(idx, mMediaSlots[idx], glimg, raw)" not in resolve_method:
        raise RuntimeError("first-load and resized media must use immutable media tickets")
    if "if (is_media)\n                {\n                    // Reserve an empty descriptor slot" not in resolve_method:
        raise RuntimeError("dynamic media first load must reserve a white fallback instead of filling on main")
    if "startImmutableMediaStage(slot, m, glimg, raw)" not in media_method:
        raise RuntimeError("changed media frames must use the shared worker ticket path")
    if "RecipeKind::CopyCompressed" not in source or "queueTexturePrep(" not in source:
        raise RuntimeError("compressed cache prefix and upload recipes must enter the shared service")
    if "batch.add(" in source:
        raise RuntimeError("decoded pixel uploads must not fall back to the legacy batch")
    if args.producer_root:
        producer = (args.producer_root.resolve() / "indra/llrender/lltexturedata.cpp").read_text()
        capture = extract_braced(producer,
            "LLTextureData::EVKPrepCapture LLTextureData::captureVKPrepSource(\n"
            "    S32 consumerGeneration, S32 discard, U8 coverageCut,\n"
            "    const LLVKTexturePrep::CaptureBudget& budget, const LLVKCpuLease& lease,\n"
            "    LLVKTexturePrep::SourceSnapshot& output, U64& mainCopyBytes,\n"
            "    U64& mainCopyCpuNs, bool& mainCopyCpuValid) const noexcept")
        if "consumerGeneration == mVKPrepDirtyFromGen" not in capture or "consumerGeneration" not in capture:
            raise RuntimeError("producer no longer has the expected patch/full-snapshot decision")

    support = r'''#include "llvktextureprep.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <cmath>
#include <vector>
using uint8_t = std::uint8_t; using uint32_t = std::uint32_t; using uint64_t = std::uint64_t;
using VkDeviceSize = std::uint64_t; using VkFormat = std::uint32_t;
constexpr VkFormat VK_FORMAT_UNDEFINED=0, VK_FORMAT_R8_UNORM=9, VK_FORMAT_R8G8_UNORM=16;
constexpr VkFormat VK_FORMAT_R8G8B8A8_UNORM=37, VK_FORMAT_R8G8B8A8_SRGB=43;
constexpr VkFormat VK_FORMAT_B8G8R8A8_UNORM=44, VK_FORMAT_B8G8R8A8_SRGB=50;
uint64_t texLevelBytes(uint32_t w,uint32_t h,VkFormat f) {
 if(f==133u) return (uint64_t)((w+3u)/4u)*((h+3u)/4u)*8u; // BC1 RGBA UNORM
 if(f==VK_FORMAT_R8_UNORM) return (uint64_t)w*h;
 if(f==VK_FORMAT_R8G8_UNORM) return (uint64_t)w*h*2u;
 return (uint64_t)w*h*4u;
}
uint64_t texChainBytesFmt(uint32_t w,uint32_t h,VkFormat f,uint32_t levels=32) {
 uint64_t n=0; for(uint32_t i=0;i<levels;++i){n+=texLevelBytes(w,h,f);if(w==1u&&h==1u)break;w=std::max(w/2u,1u);h=std::max(h/2u,1u);} return n;
}
constexpr uint8_t TEXENC_NONE=0;
VkFormat texEncFormat(uint8_t enc,bool srgb) { return enc==0 ? (srgb ? 43u : 37u) : 133u; }
namespace LLVKTexturePrep {
struct WorkspaceSize { uint64_t outputBytes=0, scratchBytes=0; };
'''
    generated = out / "texture_prep_worker_checks.cpp"
    generated.parent.mkdir(parents=True, exist_ok=True)
    generated.write_text(
        support + "\n" + "\n".join(worker_functions) + "\n} // namespace LLVKTexturePrep\nusing namespace LLVKTexturePrep;\n" + r'''
static void expect(bool value,const char* name){if(!value){std::cerr<<"FAIL "<<name<<"\n";std::exit(2);}}
int main(){
 using namespace LLVKTexturePrep;
 // The producer's bottom-right dirty texel is represented in the full immutable
 // source; the worker must copy the complete image, preserving that final texel.
 auto owner=std::make_shared<std::vector<uint8_t>>(std::initializer_list<uint8_t>{
   1,2,3,4, 5,6,7,8, 9,10,11,12, 201,202,203,204});
 SourceSnapshot full; full.owner=owner; full.byteCount=16; full.retainedBackingBytes=16;
 full.serial=9; full.generation=2; full.baseGeneration=-1; full.width=full.fullWidth=2;
 full.height=full.fullHeight=2; full.levels=1; full.components=4;
 full.pixelFormat=(uint32_t)PixelFormat::RGBA; full.pixelType=(uint32_t)PixelType::UnsignedByte;
 full.spanCount=1; full.kind=SourceKind::PixelBytes;
 full.spans[0]={owner->data(),16,8,0,0,2,2,0,1,SpanLayout::Rows};
 Recipe recipe; recipe.kind=RecipeKind::ConvertPixels; recipe.targetWidth=2; recipe.targetHeight=2;
 recipe.targetStorageFormat=VK_FORMAT_R8G8B8A8_SRGB; recipe.srgb=true;
 std::array<uint8_t,16> outputBytes{}; std::array<uint8_t,32> scratch{}; OutputLayout output{};
 expect(full.valid(),"full-source-valid");
 expect(prepareTextureInput(full,recipe,outputBytes.data(),outputBytes.size(),scratch.data(),scratch.size(),output),"full-image-worker");
 expect(output.byteCount==16 && output.mipCount==1 && output.storageFormat==VK_FORMAT_R8G8B8A8_SRGB,
        "rgba8-srgb-layout");
 expect(output.mips[0].offset==0 && output.mips[0].bytes==16 && output.mips[0].width==2 && output.mips[0].height==2,
        "mip-layout-matches-bytes");
 expect(std::memcmp(outputBytes.data(),owner->data(),16)==0 && outputBytes[12]==201 && outputBytes[15]==204,
        "bottom-right-patch-survives-worker");
 // CEF-style BGRA is converted exactly once by the worker.
 auto bgraOwner=std::make_shared<std::vector<uint8_t>>(std::initializer_list<uint8_t>{30,20,10,40});
 SourceSnapshot bgra=full; bgra.owner=bgraOwner; bgra.byteCount=4; bgra.retainedBackingBytes=4;
 bgra.width=bgra.fullWidth=1; bgra.height=bgra.fullHeight=1; bgra.levels=1;
 bgra.pixelFormat=(uint32_t)PixelFormat::BGRA; bgra.pixelType=(uint32_t)PixelType::UnsignedByte;
 bgra.spans[0]={bgraOwner->data(),4,4,0,0,1,1,0,1,SpanLayout::Rows};
 Recipe bgraRecipe; bgraRecipe.kind=RecipeKind::ConvertPrivateMedia;
 bgraRecipe.targetWidth=1; bgraRecipe.targetHeight=1; bgraRecipe.targetStorageFormat=VK_FORMAT_R8G8B8A8_UNORM;
 std::array<uint8_t,4> bgraOut{}; std::array<uint8_t,8> bgraScratch{}; OutputLayout bgraLayout{};
 expect(prepareTextureInput(bgra,bgraRecipe,bgraOut.data(),bgraOut.size(),bgraScratch.data(),bgraScratch.size(),bgraLayout),"bgra-media-worker");
 expect(bgraOut[0]==10 && bgraOut[1]==20 && bgraOut[2]==30 && bgraOut[3]==40,"bgra-swapped-once");
 // GStreamer declares RGBA bytes with UINT_8888_REV; that producer's byte sequence is
 // already RGBA and must not be interpreted as packed host words by the worker.
 auto gstOwner=std::make_shared<std::vector<uint8_t>>(std::initializer_list<uint8_t>{10,20,30,40});
 SourceSnapshot gst=bgra; gst.owner=gstOwner; gst.pixelFormat=(uint32_t)PixelFormat::RGBA;
 gst.pixelType=(uint32_t)PixelType::Packed8888Rev; gst.spans[0].data=gstOwner->data();
 std::array<uint8_t,4> gstOut{}; OutputLayout gstLayout{};
 expect(prepareTextureInput(gst,bgraRecipe,gstOut.data(),gstOut.size(),bgraScratch.data(),bgraScratch.size(),gstLayout),"gstreamer-rev-worker");
 expect(gstOut[0]==10 && gstOut[1]==20 && gstOut[2]==30 && gstOut[3]==40,"gstreamer-rev-rgba-bytes");
 gst.pixelFormat=(uint32_t)PixelFormat::BGRA;
 expect(!prepareTextureInput(gst,bgraRecipe,gstOut.data(),gstOut.size(),bgraScratch.data(),bgraScratch.size(),gstLayout),"packed-rev-restricted-to-rgba");
 // Legacy luminance-alpha bytes retain their luminance and alpha channels when expanded
 // for a world texture, rather than receiving an opaque alpha default.
 auto laOwner=std::make_shared<std::vector<uint8_t>>(std::initializer_list<uint8_t>{17,201,33,202});
 SourceSnapshot la=full; la.owner=laOwner; la.byteCount=4; la.retainedBackingBytes=4;
 la.width=la.fullWidth=2; la.height=la.fullHeight=1; la.levels=2; la.components=2;
 la.pixelFormat=(uint32_t)PixelFormat::LuminanceAlpha; la.pixelType=(uint32_t)PixelType::UnsignedByte;
 la.spans[0]={laOwner->data(),4,4,0,0,2,1,0,1,SpanLayout::Rows};
 Recipe laRecipe; laRecipe.kind=RecipeKind::ConvertPixels; laRecipe.targetWidth=2; laRecipe.targetHeight=1;
 laRecipe.targetStorageFormat=VK_FORMAT_R8G8B8A8_UNORM; laRecipe.preparedStorageFormat=VK_FORMAT_R8G8B8A8_UNORM;
 std::array<uint8_t,8> laOut{}; OutputLayout laLayout{};
 expect(prepareTextureInput(la,laRecipe,laOut.data(),laOut.size(),scratch.data(),scratch.size(),laLayout),"luminance-alpha-world-route");
 expect(laOut[0]==17 && laOut[1]==17 && laOut[2]==17 && laOut[3]==201
        && laOut[4]==33 && laOut[5]==33 && laOut[6]==33 && laOut[7]==202,"luminance-alpha-preserved");
 expect(preparedTextureOutputMatches(recipe,output,2,2,16),"main-accepts-matching-format");
 Recipe wrong=recipe; wrong.targetStorageFormat=VK_FORMAT_R8G8B8A8_UNORM;
 expect(!prepareTextureInput(full,wrong,outputBytes.data(),outputBytes.size(),scratch.data(),scratch.size(),output),"worker-rejects-format-byte-mismatch");
 wrong=recipe; wrong.encode=1;
 expect(!prepareTextureInput(full,wrong,outputBytes.data(),outputBytes.size(),scratch.data(),scratch.size(),output),"worker-rejects-unimplemented-encoder");
 SourceSnapshot patch=full; patch.baseGeneration=1;
 patch.byteCount=4; patch.retainedBackingBytes=16;
 patch.spans[0]={owner->data()+12,4,4,1,1,1,1,0,1,SpanLayout::Rows};
 expect(patch.valid(),"patch-source-valid");
 expect(!prepareTextureInput(patch,recipe,outputBytes.data(),outputBytes.size(),scratch.data(),scratch.size(),output),
        "worker-refuses-rectangle-without-gpu-base-copy");
 output.storageFormat=VK_FORMAT_R8G8B8A8_UNORM;
 expect(!preparedTextureOutputMatches(recipe,output,2,2,16),"main-rejects-mislabeled-layout");
 // A reduced decode runs the production box filter and writes the exact output extent.
 auto reducedOwner=std::make_shared<std::vector<uint8_t>>(4u*4u*4u);
 for(uint32_t y=0;y<4;++y) for(uint32_t x=0;x<4;++x){
   const size_t p=((size_t)y*4u+x)*4u;
   (*reducedOwner)[p+0]=(uint8_t)(x*20u); (*reducedOwner)[p+1]=(uint8_t)(y*20u);
   (*reducedOwner)[p+2]=60; (*reducedOwner)[p+3]=255;
 }
 SourceSnapshot reduced=full; reduced.owner=reducedOwner; reduced.byteCount=64; reduced.retainedBackingBytes=64;
 reduced.width=reduced.fullWidth=4; reduced.height=reduced.fullHeight=4; reduced.levels=3;
 reduced.spans[0]={reducedOwner->data(),64,16,0,0,4,4,0,1,SpanLayout::Rows};
 Recipe reduceRecipe; reduceRecipe.kind=RecipeKind::ReducePixels; reduceRecipe.targetWidth=2;
 reduceRecipe.targetHeight=2; reduceRecipe.targetStorageFormat=VK_FORMAT_R8G8B8A8_UNORM;
 reduceRecipe.preparedStorageFormat=VK_FORMAT_R8G8B8A8_UNORM;
 WorkspaceSize reduceSize{}; expect(workspaceSize(reduced,reduceRecipe,reduceSize),"reduce-workspace");
 std::vector<uint8_t> reduceOut((size_t)reduceSize.outputBytes), reduceScratch((size_t)reduceSize.scratchBytes);
 OutputLayout reduceLayout{};
 expect(prepareTextureInput(reduced,reduceRecipe,reduceOut.data(),reduceOut.size(),
       reduceScratch.data(),reduceScratch.size(),reduceLayout),"reduce-worker-route");
 expect(reduceLayout.byteCount==16 && reduceLayout.mipCount==1 && reduceLayout.mips[0].width==2
        && reduceLayout.mips[0].height==2,"reduce-output-layout");
 expect(reduceOut[0]==10 && reduceOut[1]==10 && reduceOut[2]==60 && reduceOut[3]==255
        && reduceOut[12]==50 && reduceOut[13]==50,"reduce-pixel-values");
 // Coverage builds the reduced mip chain through the same shared worker.
 auto covOwner=std::make_shared<std::vector<uint8_t>>(4u*4u*4u,255);
 for(size_t p=0;p<16;++p) (*covOwner)[p*4u+3]=(p&1u)?0u:255u;
 SourceSnapshot cov=reduced; cov.owner=covOwner; cov.byteCount=64; cov.retainedBackingBytes=64;
 cov.spans[0]={covOwner->data(),64,16,0,0,4,4,0,1,SpanLayout::Rows};
 Recipe covRecipe; covRecipe.kind=RecipeKind::CoveragePixels; covRecipe.targetWidth=2;
 covRecipe.targetHeight=2; covRecipe.targetLevels=2; covRecipe.alphaCutoff=128;
 covRecipe.targetStorageFormat=VK_FORMAT_R8G8B8A8_UNORM;
 covRecipe.preparedStorageFormat=VK_FORMAT_R8G8B8A8_UNORM;
 WorkspaceSize covSize{}; expect(workspaceSize(cov,covRecipe,covSize),"coverage-workspace");
 std::vector<uint8_t> covOut((size_t)covSize.outputBytes), covScratch((size_t)covSize.scratchBytes);
 OutputLayout covLayout{};
 expect(prepareTextureInput(cov,covRecipe,covOut.data(),covOut.size(),
       covScratch.data(),covScratch.size(),covLayout),"coverage-worker-route");
 expect(covLayout.byteCount==20 && covLayout.mipCount==2 && covLayout.mips[0].width==2
        && covLayout.mips[1].width==1 && covLayout.mips[1].offset==16,"coverage-mip-chain-layout");
 // Coverage filters sRGB colors in linear light while retaining UNORM staging for
 // the GPU encoder. The source color space does not change that byte layout.
 for(size_t p=0;p<16;++p) {
   (*covOwner)[p*4u]=(*covOwner)[p*4u+1]=(*covOwner)[p*4u+2]=(p&1u)?255u:0u;
   (*covOwner)[p*4u+3]=255u;
 }
 covRecipe.srgb=true;
 expect(workspaceSize(cov,covRecipe,covSize),"srgb-coverage-workspace");
 expect(prepareTextureInput(cov,covRecipe,covOut.data(),covOut.size(),
       covScratch.data(),covScratch.size(),covLayout),"srgb-coverage-worker-route");
 expect(covOut[0]==188 && covOut[1]==188 && covOut[2]==188 && covOut[3]==255,
        "srgb-coverage-linear-light-filter");
 expect(covLayout.srgb && covLayout.storageFormat==VK_FORMAT_R8G8B8A8_UNORM
        && preparedTextureOutputMatches(covRecipe,covLayout,2,2,covOut.size()),
        "srgb-coverage-unorm-layout");
 covRecipe.encode=1; covRecipe.targetStorageFormat=texEncFormat(covRecipe.encode,true);
 expect(prepareTextureInput(cov,covRecipe,covOut.data(),covOut.size(),
       covScratch.data(),covScratch.size(),covLayout),"srgb-coverage-encoded-worker-route");
 expect(covLayout.destinationStorageFormat==covRecipe.targetStorageFormat
        && preparedTextureOutputMatches(covRecipe,covLayout,2,2,covOut.size()),
        "srgb-coverage-encoded-layout");
 // A partial GPU-cache read keeps the original source mip indices. The worker stages
 // levels 2..4 for a 1024 source into destination levels 0..2 at 256, while the
 // renderer records the existing 32px resident as destination levels 3..8.
 constexpr VkFormat VK_FORMAT_BC1_RGBA_UNORM_BLOCK = 133;
 auto bcOwner=std::make_shared<std::vector<uint8_t>>(43008u);
 SourceSnapshot partial; partial.owner=bcOwner; partial.byteCount=43008; partial.retainedBackingBytes=43008;
 partial.serial=18; partial.generation=3; partial.width=partial.fullWidth=1024;
 partial.height=partial.fullHeight=1024; partial.levels=11; partial.firstLevel=2;
 partial.components=4; partial.storageFormat=VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
 partial.spanCount=3; partial.kind=SourceKind::CompressedLevels;
 uint64_t bcOffset=0;
 for(uint32_t i=0;i<3;++i){
   const uint32_t level=2+i, mw=1024u>>level, mh=1024u>>level;
   const uint64_t bytes=texLevelBytes(mw,mh,VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
   partial.spans[i]={bcOwner->data()+bcOffset,bytes,0,0,0,mw,mh,level,1,SpanLayout::MipChain};
   bcOffset+=bytes;
 }
 Recipe bcRecipe; bcRecipe.kind=RecipeKind::CopyCompressed; bcRecipe.targetWidth=256;
 bcRecipe.targetHeight=256; bcRecipe.targetLevels=9; bcRecipe.payloadLevels=3;
 bcRecipe.residentTailLevels=6; bcRecipe.targetStorageFormat=VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
 bcRecipe.preparedStorageFormat=VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
 std::array<uint8_t,43008> bcOut{}; OutputLayout bcLayout{};
 expect(partial.valid(),"partial-compressed-source-valid");
 expect(prepareTextureInput(partial,bcRecipe,bcOut.data(),bcOut.size(),nullptr,0,bcLayout),"partial-compressed-prefix-worker");
 expect(bcLayout.mipCount==3 && bcLayout.byteCount==43008
        && bcLayout.mips[0].width==256 && bcLayout.mips[1].width==128 && bcLayout.mips[2].width==64,
        "partial-prefix-layout-distinct-from-nine-level-image");
 expect(std::memcmp(bcOut.data(),bcOwner->data(),bcOut.size())==0,"partial-cache-bytes-preserved");
 expect(preparedTextureOutputMatches(bcRecipe,bcLayout,256,256,bcOut.size()),
        "partial-prefix-final-tail-recipe-accepted");
 std::cout<<"PASS actual worker full-snapshot bottom-right update and format/layout validation\n";
}
''')
    receipt = out / "source-receipt.txt"
    receipt.write_text(
        f"worker sha256 {sha(cpp_path)}\n"
        f"renderer sha256 {sha(source_path)}\n"
        f"generated sha256 {sha(generated)}\n"
        "extracted shared production worker helpers plus renderer admission structure\n"
        "fixture: shared worker full/reduced/coverage/media/packed/compressed routes; full immutable snapshot after bottom-right update; rectangle snapshot refusal; VkFormat/layout mismatch refusal\n"
        "route checks: material resolves and tier-2 cache use queueTexturePrep; media frames use startImmutableMediaStage; no legacy batch pixel fallback\n"
        "note: producer capture implementation is source-checked only when --producer-root is supplied; GPU patch copy is not implemented\n"
    )
    print(f"generated {generated}")
    print(f"receipt {receipt}")
    if args.run:
        exe = out / "texture_prep_worker_checks"
        cmd = ["/usr/bin/c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-UNDEBUG",
               "-I", str(root / "indra/llrender"), "-I", str(root / "indra/newview/vkgfx"),
               str(generated), "-o", str(exe)]
        env = os.environ.copy()
        for name in ("TMPDIR", "TMP", "TEMP", "XDG_CACHE_HOME"):
            path = out / name.lower(); path.mkdir(parents=True, exist_ok=True); env[name] = str(path)
        log = out / "compile-run.log"
        with log.open("w") as f:
            f.write("compile argv: " + shlex.join(cmd) + "\n")
            f.write(f"cwd: {out}\n")
            try: proc = subprocess.run(cmd,cwd=out,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=60,check=False)
            except subprocess.TimeoutExpired: f.write("compile timeout\n"); return 124
            if proc.returncode: return proc.returncode
            f.write("run argv: " + shlex.join([str(exe)]) + "\n")
            try: proc = subprocess.run([str(exe)],cwd=out,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=10,check=False)
            except subprocess.TimeoutExpired: f.write("fixture timeout\n"); return 124
            if proc.returncode: return proc.returncode
        print(f"PASS; see {log}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
