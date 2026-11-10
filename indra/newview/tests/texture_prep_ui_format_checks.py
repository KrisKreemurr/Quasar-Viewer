#!/usr/bin/env python3
"""CPU regression for compact UI texture recipes in the production prep worker.

The generator extracts the actual UI selector/packing, workspace sizing, and output
validation functions from llvktextureprepworker.cpp. It also checks the facade/cache
handoff keeps the selected format and byte count instead of forcing RGBA8.
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
        raise RuntimeError(f"missing production function: {marker}")
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
    raise RuntimeError(f"unterminated production function: {marker}")


def sha(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=pathlib.Path, required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--compiler", default="/usr/bin/c++")
    parser.add_argument("--run", action="store_true")
    args = parser.parse_args()
    root, out = args.source_root.resolve(), args.out.resolve()
    if root == out or root in out.parents:
        raise SystemExit("fixture output must be outside the source tree")

    worker_path = root / "indra/newview/vkgfx/llvktextureprepworker.cpp"
    prep_path = root / "indra/newview/vkgfx/llvktextureprep.h"
    backend_path = root / "indra/newview/vkgfx/llrendervkbackend.cpp"
    facade_path = root / "indra/newview/vkgfx/llrendervk/llrendervk.cpp"
    service_path = root / "indra/newview/vkgfx/llvktextureprepservice.cpp"
    worker = worker_path.read_text()
    backend = backend_path.read_text()
    facade = facade_path.read_text()
    service = service_path.read_text()
    functions = [
        extract_braced(worker, "uint32_t mipLevels("),
        extract_braced(worker, "bool mul("),
        extract_braced(worker, "bool pixelBytes("),
        extract_braced(worker, "uint32_t uiBytesPerPixel("),
        extract_braced(worker, "uint32_t uiVkFormat("),
        extract_braced(worker, "UiSwizzle uiSwizzle("),
        extract_braced(worker, "bool uiSourceRecipeValid("),
        extract_braced(worker, "void expandPixel("),
        extract_braced(worker, "bool prepareUiPixels("),
        extract_braced(worker, "bool workspaceSize("),
        extract_braced(worker, "bool prepareTextureInput("),
        extract_braced(worker, "bool preparedTextureOutputMatches("),
    ]
    if "recipe.uiFormat == UiFormat::FontCoverageAuto" not in functions[9]:
        raise RuntimeError("workspace sizing must reserve the font format's two-byte fallback")
    if "uiSourceRecipeValid(source, recipe)" not in functions[9]:
        raise RuntimeError("invalid UI input metadata must be rejected before worker admission")
    if "uiFormatFromLayout(view.layout" not in backend or "layout.uiSwizzle" not in backend:
        raise RuntimeError("UI facade admission must consume worker format and swizzle metadata")
    if "view.layout.byteCount" not in backend or "candidateBytes = view.layout.byteCount" not in backend:
        raise RuntimeError("UI cache accounting must charge the actual compact bytes")
    if "createPreparedTexture(w, h, preparedFmt" not in backend:
        raise RuntimeError("candidate image creation must use the worker-selected format")
    if "sourceBytes != (VkDeviceSize)w * h * bpp" not in facade:
        raise RuntimeError("prepared facade copy must validate bytes for the selected texel format")
    if "hostWrite.size = u.preparedBytes" not in facade:
        raise RuntimeError("prepared host-write barrier must cover the compact payload size")
    if "TXF_LA8" not in facade or "VK_FORMAT_R8G8_UNORM" not in facade or "VK_FORMAT_R8_UNORM" not in facade:
        raise RuntimeError("facade must allocate matching RG8 and R8 sampled images")
    if "&& a.uiFormat == b.uiFormat" not in service:
        raise RuntimeError("service ticket coalescing must distinguish compact UI recipes")

    support = r'''#include "llvktextureprepworker.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>
using uint8_t=std::uint8_t; using uint32_t=std::uint32_t; using uint64_t=std::uint64_t;
using VkFormat=std::uint32_t;
constexpr VkFormat VK_FORMAT_UNDEFINED=0, VK_FORMAT_R8_UNORM=9, VK_FORMAT_R8G8_UNORM=16;
constexpr VkFormat VK_FORMAT_R8G8B8A8_UNORM=37, VK_FORMAT_R8G8B8A8_SRGB=43;
constexpr VkFormat VK_FORMAT_B8G8R8A8_UNORM=44, VK_FORMAT_B8G8R8A8_SRGB=50;
constexpr uint8_t TEXENC_NONE=0;
uint32_t mipLevels(uint32_t,uint32_t);
uint64_t texLevelBytes(uint32_t w,uint32_t h,VkFormat){return (uint64_t)w*h*4u;}
uint64_t texChainBytesFmt(uint32_t w,uint32_t h,VkFormat f,uint32_t levels){
 uint64_t n=0; for(uint32_t i=0;i<levels;++i){n+=texLevelBytes(w,h,f);w=std::max(w/2u,1u);h=std::max(h/2u,1u);} return n;
}
VkFormat texEncFormat(uint8_t,bool){return VK_FORMAT_UNDEFINED;}
bool reduceRGBA(const uint8_t*,uint8_t*,uint32_t,uint32_t,uint32_t,uint32_t,bool){return false;}
bool buildCoverage(uint8_t*,uint8_t*,uint8_t*,uint64_t,const uint8_t*,uint32_t,
                   LLVKTexturePrep::PixelFormat,uint32_t,uint32_t,uint32_t,bool,uint8_t){return false;}
bool LLVKTexturePrep::prepareCompressedPayload(const LLVKTexturePrep::SourceSnapshot&,const LLVKTexturePrep::Recipe&,
                              uint8_t*,uint64_t,LLVKTexturePrep::OutputLayout&) noexcept {return false;}
'''
    tests = r'''
static void expect(bool ok,const char* what){if(!ok){std::cerr<<"FAIL "<<what<<"\n";std::exit(2);}}
static LLVKTexturePrep::SourceSnapshot source(std::shared_ptr<std::vector<uint8_t>> data,
    uint32_t components, LLVKTexturePrep::PixelFormat fmt){
 using namespace LLVKTexturePrep; SourceSnapshot s; s.owner=data; s.byteCount=data->size();
 s.retainedBackingBytes=data->size(); s.serial=1; s.generation=1; s.width=s.fullWidth=2;
 s.height=s.fullHeight=1; s.levels=1; s.components=components; s.pixelFormat=(uint32_t)fmt;
 s.pixelType=(uint32_t)PixelType::UnsignedByte; s.spanCount=1; s.kind=SourceKind::PixelBytes;
 s.spans[0]={data->data(),data->size(),2u*components,0,0,2,1,0,1,SpanLayout::Rows}; return s;
}
static void run(const char* name, LLVKTexturePrep::SourceSnapshot& s,
                LLVKTexturePrep::UiFormat requested, LLVKTexturePrep::UiFormat expected,
                LLVKTexturePrep::UiSwizzle swizzle, uint32_t vkfmt,
                const std::vector<uint8_t>& expectedBytes){
 std::cout << "case " << name << std::endl;
 using namespace LLVKTexturePrep; Recipe r; r.kind=RecipeKind::ConvertPixels;
 r.targetWidth=2; r.targetHeight=1; r.targetLevels=1; r.uiFormat=requested;
 if(requested!=UiFormat::FontCoverageAuto) r.targetStorageFormat=r.preparedStorageFormat=vkfmt;
 WorkspaceSize ws{}; expect(s.valid(),"source.valid"); expect(workspaceSize(s,r,ws),"ui.workspace");
 const uint32_t reservedBpp=requested==UiFormat::FontCoverageAuto?2u:
     requested==UiFormat::RGBA8?4u:requested==UiFormat::LA8?2u:1u;
 expect(ws.outputBytes==(uint64_t)s.width*s.height*reservedBpp,"ui.stage-capacity-is-bounded-worst-case");
 std::vector<uint8_t> dst((size_t)ws.outputBytes), scratch((size_t)ws.scratchBytes);
 OutputLayout out{}; expect(prepareTextureInput(s,r,dst.data(),dst.size(),scratch.data(),scratch.size(),out),name);
 expect(out.uiFormat==expected && out.uiSwizzle==swizzle,"ui.format-swizzle");
 expect(out.storageFormat==vkfmt && out.destinationStorageFormat==vkfmt,"ui.vk-format");
 expect(out.byteCount==expectedBytes.size() && out.components==expectedBytes.size()/2u,"ui.compact-byte-count");
 expect(std::memcmp(dst.data(),expectedBytes.data(),expectedBytes.size())==0,"ui.exact-pixels");
 expect(preparedTextureOutputMatches(r,out,2,1,ws.outputBytes),"ui.admission-match");
 OutputLayout wrong=out; wrong.uiSwizzle=UiSwizzle::None;
 expect(!preparedTextureOutputMatches(r,wrong,2,1,ws.outputBytes),"ui.reject-swizzle-mismatch");
}
int main(){using namespace LLVKTexturePrep;
 auto l=std::make_shared<std::vector<uint8_t>>(std::initializer_list<uint8_t>{10,200});
 auto ls=source(l,1,PixelFormat::Luminance);
 run("legacy-l8",ls,UiFormat::L8,UiFormat::L8,UiSwizzle::Luminance,VK_FORMAT_R8_UNORM,{10,200});
 auto a=std::make_shared<std::vector<uint8_t>>(std::initializer_list<uint8_t>{10,200});
 auto as=source(a,1,PixelFormat::Alpha);
 run("legacy-a8",as,UiFormat::A8,UiFormat::A8,UiSwizzle::Alpha,VK_FORMAT_R8_UNORM,{10,200});
 auto la=std::make_shared<std::vector<uint8_t>>(std::initializer_list<uint8_t>{17,201,33,202});
 auto las=source(la,2,PixelFormat::LuminanceAlpha);
 run("legacy-la8",las,UiFormat::LA8,UiFormat::LA8,UiSwizzle::LuminanceAlpha,VK_FORMAT_R8G8_UNORM,{17,201,33,202});
 auto white=std::make_shared<std::vector<uint8_t>>(std::initializer_list<uint8_t>{255,91,255,173});
 auto ws=source(white,2,PixelFormat::LuminanceAlpha);
 run("font-coverage8",ws,UiFormat::FontCoverageAuto,UiFormat::Coverage8,UiSwizzle::Coverage,VK_FORMAT_R8_UNORM,{91,173});
 auto nonwhite=std::make_shared<std::vector<uint8_t>>(std::initializer_list<uint8_t>{254,91,7,173});
 auto ns=source(nonwhite,2,PixelFormat::LuminanceAlpha);
 run("font-fallback-la8",ns,UiFormat::FontCoverageAuto,UiFormat::LA8,UiSwizzle::LuminanceAlpha,VK_FORMAT_R8G8_UNORM,{254,91,7,173});
 auto packed=source(std::make_shared<std::vector<uint8_t>>(std::initializer_list<uint8_t>{1,2,3,4,5,6,7,8}),4,PixelFormat::RGBA);
 packed.pixelType=(uint32_t)PixelType::Packed8888Rev;
 run("packed-rgba-ui",packed,UiFormat::RGBA8,UiFormat::RGBA8,UiSwizzle::RGBA,VK_FORMAT_R8G8B8A8_UNORM,{1,2,3,4,5,6,7,8});
 auto rgb=std::make_shared<std::vector<uint8_t>>(std::initializer_list<uint8_t>{1,2,3,4,5,6});
 auto rs=source(rgb,3,PixelFormat::RGB);
 run("legacy-rgb-expands",rs,UiFormat::RGBA8,UiFormat::RGBA8,UiSwizzle::RGBA,VK_FORMAT_R8G8B8A8_UNORM,{1,2,3,255,4,5,6,255});
 auto bgr=std::make_shared<std::vector<uint8_t>>(std::initializer_list<uint8_t>{30,20,10,60,50,40});
 auto bs=source(bgr,3,PixelFormat::BGR);
 run("legacy-bgr-byte-order",bs,UiFormat::RGBA8,UiFormat::RGBA8,UiSwizzle::RGBA,VK_FORMAT_R8G8B8A8_UNORM,{30,20,10,255,60,50,40,255});
 auto rgba=std::make_shared<std::vector<uint8_t>>(std::initializer_list<uint8_t>{1,2,3,4,5,6,7,8});
 auto rgs=source(rgba,4,PixelFormat::RGBA);
 run("legacy-rgba",rgs,UiFormat::RGBA8,UiFormat::RGBA8,UiSwizzle::RGBA,VK_FORMAT_R8G8B8A8_UNORM,{1,2,3,4,5,6,7,8});
 auto bgra=std::make_shared<std::vector<uint8_t>>(std::initializer_list<uint8_t>{30,20,10,40,70,60,50,80});
 auto bgs=source(bgra,4,PixelFormat::BGRA);
 run("legacy-bgra-byte-order",bgs,UiFormat::RGBA8,UiFormat::RGBA8,UiSwizzle::RGBA,VK_FORMAT_R8G8B8A8_UNORM,{30,20,10,40,70,60,50,80});
 // World recipes keep the pre-existing four-channel output and never acquire UI swizzles.
 Recipe world; world.kind=RecipeKind::ConvertPixels; world.targetWidth=2; world.targetHeight=1;
 world.targetStorageFormat=VK_FORMAT_R8G8B8A8_UNORM;
 // Invalid source descriptors are rejected by admission, not accepted and failed on a worker.
 Recipe bad=world; bad.uiFormat=UiFormat::RGBA8;
 auto badType=source(rgba,3,PixelFormat::RGB); badType.pixelType=(uint32_t)PixelType::Packed8888Rev;
 WorkspaceSize badSize{}; expect(!workspaceSize(badType,bad,badSize),"ui.reject-packed-type-before-queue");
 auto badBase=source(rgba,4,PixelFormat::RGBA); badBase.baseGeneration=1;
 expect(!workspaceSize(badBase,bad,badSize),"ui.reject-patch-base-before-queue");
 auto badFormat=source(l,1,PixelFormat::Luminance); badFormat.pixelFormat=(uint32_t)PixelFormat::BGRA;
 expect(!workspaceSize(badFormat,bad,badSize),"ui.reject-format-components-before-queue");
 WorkspaceSize worldSize{}; expect(workspaceSize(rgs,world,worldSize),"world.workspace");
 std::array<uint8_t,8> scratch{}; std::array<uint8_t,8> worldOut{}; OutputLayout worldLayout{};
 expect(prepareTextureInput(rgs,world,worldOut.data(),worldOut.size(),scratch.data(),scratch.size(),worldLayout),"world.rgba-path");
 expect(worldLayout.uiFormat==UiFormat::None && worldLayout.uiSwizzle==UiSwizzle::None
        && worldLayout.byteCount==8 && worldLayout.storageFormat==VK_FORMAT_R8G8B8A8_UNORM,"world-unchanged");
 std::cout<<"PASS compact UI format and swizzle worker regressions; world RGBA remains unchanged\n";
}
'''
    generated = out / "texture_prep_ui_format_checks.cpp"
    out.mkdir(parents=True, exist_ok=True)
    generated.write_text(support + "\nnamespace LLVKTexturePrep {\n" + "\n".join(functions) + "\n}\n" + tests)
    receipt = out / "source-receipt.txt"
    receipt.write_text(
        f"worker sha256 {sha(worker_path)}\nprep contract sha256 {sha(prep_path)}\n"
        f"backend sha256 {sha(backend_path)}\nfacade sha256 {sha(facade_path)}\n"
        f"generated fixture sha256 {sha(generated)}\n"
        "extracted production functions: workspaceSize, prepareTextureInput, preparedTextureOutputMatches, compact UI selectors/packer\n"
        "checks: L8/A8/LA8, font coverage auto/fallback, RGB/RGBA bytes, swizzle mismatch, facade selected-format/cache-byte wiring, world RGBA default\n"
    )
    print(f"generated {generated}")
    print(f"receipt {receipt}")
    if args.run:
        exe = out / "texture_prep_ui_format_checks"
        cmd = [args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-UNDEBUG",
               "-I", str(root / "indra/newview/vkgfx"), "-I", str(root / "indra/llrender"), str(generated), "-o", str(exe)]
        env = os.environ.copy()
        for name in ("TMPDIR", "TMP", "TEMP", "XDG_CACHE_HOME"):
            path = out / name.lower(); path.mkdir(parents=True, exist_ok=True); env[name] = str(path)
        log = out / "compile-run.log"
        with log.open("w") as f:
            f.write("compile argv: " + shlex.join(cmd) + "\n")
            f.write(f"cwd: {out}\n")
            try: proc = subprocess.run(cmd, cwd=out, env=env, stdout=f, stderr=subprocess.STDOUT, timeout=30, check=False)
            except subprocess.TimeoutExpired: f.write("compile timed out\n"); return 124
            if proc.returncode: return proc.returncode
            f.write("run argv: " + shlex.join([str(exe)]) + "\n")
            try: proc = subprocess.run([str(exe)], cwd=out, env=env, stdout=f, stderr=subprocess.STDOUT, timeout=10, check=False)
            except subprocess.TimeoutExpired: f.write("fixture timed out\n"); return 124
            if proc.returncode: return proc.returncode
        print(f"PASS; see {log}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
