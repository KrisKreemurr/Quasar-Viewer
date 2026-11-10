#!/usr/bin/env python3
"""CPU fixture for the production texture prep source capture/adapters.

The generated C++ includes the neutral production SourceSnapshot header and embeds the
actual LLTextureData capture/dirty-chain methods plus the actual LLGpuTexCache Blob
adapter. Only LLImageRaw, refcounting, the cache Blob storage type, main-thread assertion,
and the existing image-owner endpoint are small fixtures.

Run only in an assigned serial validation slot:
  python3 indra/newview/tests/texture_prep_source_capture_checks.py \
      --source-root <worktree> --out <owned-validation-dir> --run
"""
from __future__ import annotations

import argparse
import hashlib
import pathlib
import shlex
import subprocess


def extract_body(source: str, marker: str, occurrence: int = 0) -> str:
    start = -1
    search_from = 0
    for _ in range(occurrence + 1):
        start = source.find(marker, search_from)
        if start < 0:
            break
        search_from = start + len(marker)
    if start < 0:
        raise RuntimeError(f"missing production marker: {marker}")
    brace = source.find("{", start)
    if brace < 0:
        raise RuntimeError(f"missing function body: {marker}")
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
    raise RuntimeError(f"unterminated production body: {marker}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source-root", type=pathlib.Path, required=True)
    ap.add_argument("--out", type=pathlib.Path, required=True)
    ap.add_argument("--compiler", default="c++")
    ap.add_argument("--run", action="store_true")
    args = ap.parse_args()
    root = args.source_root.resolve()
    out = args.out.resolve()
    if out == root or root in out.parents:
        raise SystemExit("validation output must be outside the source worktree")

    texture_cpp = root / "indra/llrender/lltexturedata.cpp"
    texture_header = root / "indra/llrender/lltexturedata.h"
    cache_cpp = root / "indra/newview/llgputexcache.cpp"
    cache_header = root / "indra/newview/llgputexcache.h"
    source_header = root / "indra/llrender/llvktextureprepsource.h"
    renderer_cpp = root / "indra/newview/vkgfx/llcleanrenderervk.cpp"
    worker_cpp = root / "indra/newview/vkgfx/llvktextureprepworker.cpp"
    tcpp = texture_cpp.read_text()
    hcpp = cache_cpp.read_text()
    rcpp = renderer_cpp.read_text()
    wcpp = worker_cpp.read_text()
    texture_snippets = [
        extract_body(tcpp, "static bool check_power_of_two("),
        extract_body(tcpp, "bool LLTextureData::checkSize("),
        extract_body(tcpp, "bool LLTextureData::setSize("),
        extract_body(tcpp, "bool LLTextureData::setFromRawInternal("),
        extract_body(tcpp, "bool LLTextureData::captureVKImmutablePixels("),
        extract_body(tcpp, "LLTextureData::EVKPrepCapture LLTextureData::captureVKPrepSource("),
        extract_body(tcpp, "LLTextureData::EVKPrepCapture LLTextureData::captureVKPrepSource(", 1),
        extract_body(tcpp, "bool LLTextureData::acknowledgeVKPrepSource("),
        extract_body(tcpp, "void LLTextureData::setVKCpuData(LLImageRaw* raw, S32 generation, S32 x, S32 y, S32 width, S32 height)"),
        extract_body(tcpp, "bool LLTextureData::setSubImageVK("),
    ]
    cache_snippet = extract_body(hcpp, "LLVKTexturePrep::CaptureResult LLGpuTexCache::capturePrepSource(")
    worker_functions = [
        extract_body(wcpp, marker) for marker in (
            "uint32_t mipLevels(", "bool mul(", "bool pixelBytes(",
            "uint32_t uiBytesPerPixel(", "uint32_t uiVkFormat(", "UiSwizzle uiSwizzle(",
            "bool uiSourceRecipeValid(",
            "bool prepareUiPixels(", "void expandPixel(", "bool reduceRGBA(",
            "void histogram(", "double coverageAt(", "bool buildCoverage(", "bool workspaceSize(",
            "bool prepareCompressedPayload(", "bool prepareTextureInput(",
            "bool preparedTextureOutputMatches(")
    ]
    admission_snippet = extract_body(rcpp, "LLCleanRendererVK::TexStageStartResult LLCleanRendererVK::startImmutableTexStage(")
    if "captureVKPrepSource(-1," not in admission_snippet:
        raise RuntimeError("renderer must request a full source until GPU base-copy patch support exists")
    capture = texture_snippets[6]
    if "retainedBytes > budget.maxRetainedBytes" not in capture:
        raise RuntimeError("capture no longer checks retained backing against the caller budget")
    copy_account = capture.find("mainCopyBytes = copyBytes")
    capacity_check = capture.find("bytes->capacity()")
    if copy_account < 0 or capacity_check < 0 or copy_account > capacity_check:
        raise RuntimeError("copy-byte accounting must precede allocator-capacity refusal")

    support = r'''#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <cmath>
#include <new>
#include <utility>
#include <vector>
#include "llvktextureprep.h"
#include "llpixelformat.h"
#define LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE
#define LL_WARNS(...) std::cerr
#define LL_ENDL "\n"
std::string llformat(const char*, S32, S32) { return "non-power-of-two input"; }
constexpr S32 MAX_DISCARD_LEVEL=12;
constexpr S32 VK_DIRTY_UNKNOWN = -0x7FFFFFFF;
using VkFormat=U32; using VkDeviceSize=U64;
constexpr VkFormat VK_FORMAT_UNDEFINED=0, VK_FORMAT_R8_UNORM=9, VK_FORMAT_R8G8_UNORM=16,
    VK_FORMAT_R8G8B8A8_UNORM=37, VK_FORMAT_R8G8B8A8_SRGB=43,
    VK_FORMAT_B8G8R8A8_UNORM=44, VK_FORMAT_B8G8R8A8_SRGB=50;
constexpr U8 TEXENC_NONE=0;
VkFormat texEncFormat(U8,bool){return VK_FORMAT_UNDEFINED;}
struct WorkspaceSize { uint64_t outputBytes=0, scratchBytes=0; };
U32 texMipLevels(U32 w,U32 h){return 32u-(U32)__builtin_clz(std::max(std::max(w,h),1u));}
U64 texLevelBytes(U32 w,U32 h,VkFormat f){if(f==133u)return (U64)((w+3u)/4u)*((h+3u)/4u)*8u;if(f==VK_FORMAT_R8_UNORM)return (U64)w*h;if(f==VK_FORMAT_R8G8_UNORM)return (U64)w*h*2u;return (U64)w*h*4u;}
U64 texChainBytesFmt(U32 w,U32 h,VkFormat f,U32 levels=32){U64 n=0;for(U32 i=0;i<levels;++i){n+=texLevelBytes(w,h,f);if(w==1&&h==1)break;w=std::max(w/2u,1u);h=std::max(h/2u,1u);}return n;}
template<class T> class LLPointer {
  std::shared_ptr<T> p;
public:
  LLPointer()=default; LLPointer(T* q):p(q){}
  LLPointer& operator=(T* q){p.reset(q);return *this;}
  LLPointer& operator=(std::nullptr_t){p.reset();return *this;}
  T* get() const{return p.get();} T* operator->() const{return p.get();}
  bool isNull()const{return !p;} bool notNull()const{return !!p;}
};
struct LLImageRaw {
  U16 w=0,h=0; S8 comps=0; std::vector<U8> data;
  LLImageRaw()=default;
  LLImageRaw(U16 W,U16 H,S8 C):w(W),h(H),comps(C),data((size_t)W*H*C){}
  LLImageRaw(const U8* src,U16 W,U16 H,S8 C):LLImageRaw(W,H,C){if(src)std::memcpy(data.data(),src,data.size());}
  bool isBufferInvalid()const{return data.empty();}
  U8* getData(){return data.data();} const U8* getData()const{return data.data();}
  U16 getWidth()const{return w;} U16 getHeight()const{return h;} S8 getComponents()const{return comps;}
  S32 getDataSize()const{return (S32)data.size();}
};
struct LLVKImmutablePixelSnapshot {
  LLPointer<LLImageRaw> mRaw; U16 mWidth=0,mHeight=0; S8 mComponents=0;
  LLPixelFormat mFormatInternal=LLPixelFormat::NONE,mFormatPrimary=LLPixelFormat::NONE;
  LLPixelType mFormatType=LLPixelType::NONE; S32 mGeneration=-1; U64 mSerial=0;
  bool mIsMask=false; U8 mAlphaGradientPct=255; U16 mAlphaCommittedSide=0;
  bool valid()const{return mRaw.notNull();} const LLImageRaw* pixels()const{return mRaw.get();}
  U16 width()const{return mWidth;} U16 height()const{return mHeight;} S8 components()const{return mComponents;}
  LLPixelFormat internalFormat()const{return mFormatInternal;} LLPixelFormat primaryFormat()const{return mFormatPrimary;}
  LLPixelType typeFormat()const{return mFormatType;} S32 generation()const{return mGeneration;} U64 serial()const{return mSerial;}
  U64 byteCount()const{return mRaw.notNull()?(U64)mRaw->getDataSize():0;} bool isMask()const{return mIsMask;}
  U8 alphaGradientPct()const{return mAlphaGradientPct;} U16 alphaCommittedSide()const{return mAlphaCommittedSide;}
};
bool assert_main_thread(){return true;}
bool vkPrepThreadCpuNs(U64& value) noexcept { static U64 tick=0; tick+=100; value=tick; return true; }
template<class T>T llmin(T a,T b){return a<b?a:b;} template<class T>T llmax(T a,T b){return a>b?a:b;}
struct LLTextureData;
struct LLVKCpuLease { LLTextureData* image=nullptr; LLTextureData* get()const{return image;} };
struct LLTextureData {
  static constexpr U8 VKUI_FONT=1;
  using EVKPrepCapture=LLVKTexturePrep::CaptureResult;
  LLImageRaw* mVKCpuData=nullptr; LLPointer<LLImageRaw> mVKCpuDataOwned;
  bool mVKCpuWritable=false,mVKCpuImmutable=false,mVKCpuDropped=false,mIsMask=false,mContentReady=false;
  S32 mVKGeneration=1; U64 mVKSerial=1; S8 mComponents=4;
  U16 mWidth=0,mHeight=0;
  bool mUseMipMaps=true,mHasExplicitFormat=false,mVKAlphaLatchedOff=false,mNeedsAlphaAndPickMask=true;
  S32 mCurrentDiscardLevel=-1,mAlphaOffset=0;
  S8 mMaxDiscardLevel=MAX_DISCARD_LEVEL;
  LLPixelFormat mFormatInternal=LLPixelFormat::RGBA,mFormatPrimary=LLPixelFormat::RGBA;
  LLPixelType mFormatType=LLPixelType::UBYTE; U8 mVKAlphaGradientPct=255; U16 mVKAlphaCommittedSide=0; U8 mVKUiKind=0;
  S32 mVKDirtyGen=VK_DIRTY_UNKNOWN,mVKDirtyFromGen=VK_DIRTY_UNKNOWN;
  S32 mVKDirtyX0=0,mVKDirtyY0=0,mVKDirtyX1=0,mVKDirtyY1=0;
  S32 mVKPrepDirtyGen=VK_DIRTY_UNKNOWN,mVKPrepDirtyFromGen=VK_DIRTY_UNKNOWN;
  S32 mVKPrepDirtyX0=0,mVKPrepDirtyY0=0,mVKPrepDirtyX1=0,mVKPrepDirtyY1=0;
  S32 getWidth()const{return mVKCpuData?mVKCpuData->getWidth():0;}
  S32 getHeight()const{return mVKCpuData?mVKCpuData->getHeight():0;}
  S32 getComponents()const{return mComponents;}
  U8 getVKUiKind()const{return mVKUiKind;}
  static bool checkSize(S32,S32);
  bool setSize(S32,S32,S32,S32=-1);
  bool setFromRawInternal(S32,const LLImageRaw*,bool,LLImageRaw*);
  void calcAlphaChannelOffsetAndStride(){}
  bool vkPostAlphaAnalysis(S32,S32){return true;}
  void analyzeAlpha(const U8*,S32,S32){}
  void setVKOwned(LLImageRaw* p){mVKCpuDataOwned=p;mVKCpuImmutable=false;}
  bool captureVKImmutablePixels(LLVKImmutablePixelSnapshot&)const;
  EVKPrepCapture captureVKPrepSource(S32,S32,U8,const LLVKTexturePrep::CaptureBudget&,const LLVKCpuLease&,LLVKTexturePrep::SourceSnapshot&,U64&)const noexcept;
  EVKPrepCapture captureVKPrepSource(S32,S32,U8,const LLVKTexturePrep::CaptureBudget&,const LLVKCpuLease&,LLVKTexturePrep::SourceSnapshot&,U64&,U64&,bool&)const noexcept;
  bool acknowledgeVKPrepSource(S32);
  void setVKCpuData(LLImageRaw*,S32,S32,S32,S32,S32);
  bool setSubImageVK(const U8*,S32,S32,S32,S32,S32,S32);
};
'''
    cpp = support + "\n".join(texture_snippets) + "\nnamespace LLVKTexturePrep {\n" + "\n".join(worker_functions) + "\n}\nusing namespace LLVKTexturePrep;\n"
    # Run the backend's actual component-to-recipe selector on the captured source.
    backend = (root / "indra/newview/vkgfx/llrendervkbackend.cpp").read_text()
    recipe_start = backend.index("    LLVKTexturePrep::Recipe recipe;", backend.index("captureVKPrepSource(-1,"))
    recipe_end = backend.index("    // The service key stays", recipe_start)
    cpp += r'''
struct LLVKTexturePrepService { enum class FailureReason { OutputLayoutRejected }; };
static LLVKTexturePrep::Recipe actualUiRecipe(const LLVKTexturePrep::SourceSnapshot& source,
                                            const LLTextureData* image) {
 const U32 w=source.width,h=source.height;
 struct { LLVKTexturePrep::Recipe vkId{}; } cached;
 auto rememberFailure=[](auto&,LLVKTexturePrepService::FailureReason,bool){std::abort();};
''' + backend[recipe_start:recipe_end] + "\n return recipe;\n}\n"
    # The actual cache adapter is compiled in a separate fixture below, sharing the
    # production neutral SourceSnapshot header and tests, so cache implementation types
    # do not pollute the LLTextureData harness.
    cpp += r'''static void expect(bool v,const char* n){if(!v){std::cerr<<"FAIL "<<n<<"\n";std::exit(2);}}
static LLImageRaw* makeRaw(U16 w=4,U16 h=4,S8 c=4){auto* p=new LLImageRaw(w,h,c);for(size_t i=0;i<p->data.size();++i)p->data[i]=(U8)i;return p;}
int main(){
  // SVG icons are rasterized at the exact physical draw size, without mipmaps.
  for (const auto dims : {std::pair<U16,U16>{19,19},{27,27},{22,17}}) {
    LLTextureData svg; svg.mUseMipMaps=false; svg.mComponents=0;
    svg.mVKGeneration=-1;
    svg.mFormatPrimary=svg.mFormatInternal=LLPixelFormat::NONE;
    LLPointer<LLImageRaw> pixels=makeRaw(dims.first,dims.second,4);
    expect(svg.setFromRawInternal(0,pixels.get(),false,nullptr),"SVG producer upload");
    LLVKCpuLease svgLease{&svg}; LLVKTexturePrep::SourceSnapshot source;
    LLVKTexturePrep::CaptureBudget budget{65536,65536}; U64 copied=0;
    expect(svg.captureVKPrepSource(-1,0,0,budget,svgLease,source,copied)==LLVKTexturePrep::CaptureResult::Ready,
           "SVG actual producer capture");
    const auto recipe=actualUiRecipe(source,&svg); WorkspaceSize size{};
    std::cout<<"SVG "<<dims.first<<"x"<<dims.second<<" image-components "<<(S32)svg.mComponents
             <<" source-components "<<source.components<<" format "<<source.pixelFormat<<"\n";
    expect(workspaceSize(source,recipe,size),"SVG actual producer passes UI preflight");
    expect(svg.mWidth==dims.first&&svg.mHeight==dims.second&&svg.mComponents==4,
           "SVG exact dimensions and component metadata");
    expect(svg.mMaxDiscardLevel>=0&&svg.mMaxDiscardLevel<=MAX_DISCARD_LEVEL,"SVG bounded discard metadata");
    expect(source.pixelFormat==(U32)LLPixelFormat::RGBA&&source.pixelType==(U32)LLPixelType::UBYTE,
           "SVG snapshot RGBA byte metadata");
    std::vector<U8> output((size_t)size.outputBytes); OutputLayout layout;
    expect(prepareTextureInput(source,recipe,output.data(),output.size(),nullptr,0,layout),"SVG UI worker");
    expect(output==pixels->data,"SVG pixels and alpha preserved exactly");
    expect(preparedTextureOutputMatches(recipe,layout,dims.first,dims.second,output.size()),"SVG output layout");
    expect(!svg.setSize(-1,dims.second,4),"non-mipped negative dimensions remain invalid");
    expect(!svg.setSize(0,19,4),"zero plus non-power-of-two dimensions remain invalid");
  }
  LLTextureData mipmapped;
  expect(!mipmapped.setSize(19,19,4),"mipmapped world size policy unchanged");
  expect(mipmapped.setSize(32,16,4),"mipmapped power-of-two size remains valid");
  LLTextureData empty; empty.mUseMipMaps=false;
  expect(empty.setSize(0,16,4),"existing zero plus power-of-two metadata remains valid");
  LLTextureData d; LLImageRaw* raw=makeRaw(); d.mVKCpuDataOwned=raw; d.mVKCpuData=raw;
  d.mVKCpuWritable=true; d.mVKGeneration=10; d.mVKSerial=44;
  d.mVKUiKind=7;
  LLVKCpuLease lease{&d}; LLVKTexturePrep::CaptureBudget budget{4096,4096};
  LLVKTexturePrep::SourceSnapshot src; U64 copied=99;
  expect(d.captureVKPrepSource(-1,0,127,budget,lease,src,copied)==LLVKTexturePrep::CaptureResult::Ready,"first mutable source ready");
  expect(src.valid()&&src.byteCount==64&&src.mainCopyBytes==64&&copied==64,"full source copied/accounted");
  expect(src.mainCopyCpuValid&&src.mainCopyCpuNs==100,"private-copy thread CPU metric captured");
  expect(src.sourceFlags==0,"UI cache kind is not written into snapshot cache flags");
  expect(src.spans[0].x==0&&src.spans[0].y==0&&src.spans[0].width==4&&src.spans[0].height==4,"full source span");
  expect(d.acknowledgeVKPrepSource(10),"ack first source");
  U8 updated[64]{}; updated[60]=201; updated[61]=202; updated[62]=203; updated[63]=204;
  expect(d.setSubImageVK(updated,4,4,3,3,1,1),"bottom-right producer patch");
  LLVKTexturePrep::SourceSnapshot patch; copied=0;
  expect(d.captureVKPrepSource(10,0,127,budget,lease,patch,copied)==LLVKTexturePrep::CaptureResult::Ready,"bottom-right patch ready");
  expect(patch.baseGeneration==10&&patch.spans[0].x==3&&patch.spans[0].y==3&&patch.spans[0].width==1&&patch.spans[0].height==1,"ack empty rect starts at edit");
  expect(patch.byteCount==4&&copied==4,"patch byte count");
  LLVKTexturePrep::SourceSnapshot fullAfterPatch; copied=0;
  expect(d.captureVKPrepSource(-1,0,127,budget,lease,fullAfterPatch,copied)==LLVKTexturePrep::CaptureResult::Ready,"force full snapshot after patch");
  expect(fullAfterPatch.baseGeneration==-1&&fullAfterPatch.spans[0].x==0&&fullAfterPatch.spans[0].y==0
      &&fullAfterPatch.spans[0].width==4&&fullAfterPatch.spans[0].height==4,"full snapshot replaces patch for worker");
  LLVKTexturePrep::Recipe workerRecipe; workerRecipe.kind=LLVKTexturePrep::RecipeKind::ConvertPixels;
  workerRecipe.targetWidth=4;workerRecipe.targetHeight=4;workerRecipe.targetStorageFormat=VK_FORMAT_R8G8B8A8_UNORM;
  std::array<U8,64> workerOutput{};std::array<U8,80> workerScratch{};LLVKTexturePrep::OutputLayout workerLayout{};
  expect(prepareTextureInput(fullAfterPatch,workerRecipe,workerOutput.data(),workerOutput.size(),workerScratch.data(),workerScratch.size(),workerLayout),"actual worker accepts full post-patch snapshot");
  expect(workerOutput[60]==201&&workerOutput[61]==202&&workerOutput[62]==203&&workerOutput[63]==204,
      "actual worker preserves acknowledged bottom-right patch");
  expect(preparedTextureOutputMatches(workerRecipe,workerLayout,4,4,workerOutput.size()),"worker output format/layout matches VkFormat");
  auto before=patch; LLVKTexturePrep::CaptureBudget refused{4096,0}; copied=77;
  expect(d.captureVKPrepSource(10,0,127,refused,lease,patch,copied)==LLVKTexturePrep::CaptureResult::Deferred,"copy cap deferred");
  expect(patch.generation==before.generation&&patch.spans[0].x==before.spans[0].x&&copied==0,"refusal keeps snapshot");
  LLVKTexturePrep::CaptureBudget enough{4096,4096};
  expect(d.captureVKPrepSource(9,0,127,enough,lease,patch,copied)==LLVKTexturePrep::CaptureResult::Ready,"stale base falls back full");
  expect(patch.baseGeneration==-1&&patch.spans[0].x==0&&patch.spans[0].width==4,"stale base full source");

  // CEF reports BGRA/UBYTE. setSubImageVK already writes the owned backing as RGBA,
  // so the worker snapshot must describe those actual bytes rather than the wire input.
  LLTextureData cef; cef.mVKCpuDataOwned=new LLImageRaw(1,1,4); cef.mVKCpuData=cef.mVKCpuDataOwned.get();
  cef.mComponents=4;
  cef.mFormatInternal=LLPixelFormat::RGB; cef.mFormatPrimary=LLPixelFormat::BGRA;
  cef.mFormatType=LLPixelType::UBYTE;
  const U8 cefBgra[4]={10,20,30,40};
  expect(cef.setSubImageVK(cefBgra,1,1,0,0,1,1),"CEF BGRA media update");
  LLVKCpuLease cefLease{&cef}; LLVKTexturePrep::SourceSnapshot cefSource; copied=0;
  expect(cef.captureVKPrepSource(-1,0,0,enough,cefLease,cefSource,copied)==LLVKTexturePrep::CaptureResult::Ready,"CEF producer snapshot");
  expect(cef.mVKCpuDataOwned->data[0]==30&&cef.mVKCpuDataOwned->data[1]==20
      &&cef.mVKCpuDataOwned->data[2]==10&&cef.mVKCpuDataOwned->data[3]==40,"CEF producer stores canonical RGBA bytes");
  expect(cefSource.pixelFormat==(U32)LLPixelFormat::RGBA&&cefSource.pixelType==(U32)LLPixelType::UBYTE
      &&cefSource.components==4,"CEF worker snapshot labels actual RGBA byte layout");
  expect(cef.mFormatPrimary==LLPixelFormat::BGRA&&cef.mFormatType==LLPixelType::UBYTE,
      "CEF legacy raw-reader wire format stays unchanged");
  LLVKTexturePrep::Recipe mediaRecipe; mediaRecipe.kind=LLVKTexturePrep::RecipeKind::ConvertPrivateMedia;
  mediaRecipe.targetWidth=1; mediaRecipe.targetHeight=1;
  mediaRecipe.targetStorageFormat=VK_FORMAT_R8G8B8A8_UNORM;
  std::array<U8,4> cefPrepared{};std::array<U8,4> mediaScratch{}; LLVKTexturePrep::OutputLayout cefLayout{};
  expect(prepareTextureInput(cefSource,mediaRecipe,cefPrepared.data(),cefPrepared.size(),mediaScratch.data(),mediaScratch.size(),cefLayout),
      "captured CEF source reaches actual worker");
  expect(cefPrepared==std::array<U8,4>{30,20,10,40},"captured CEF BGRA becomes prepared RGBA exactly once");

  // GStreamer declares RGBA with UINT_8888_REV, but its mapped payload is already
  // byte-addressable R,G,B,A. Normalize only the worker snapshot's type; do not swap.
  LLTextureData gst; gst.mVKCpuDataOwned=new LLImageRaw(1,1,4); gst.mVKCpuData=gst.mVKCpuDataOwned.get();
  gst.mComponents=4;
  gst.mFormatInternal=LLPixelFormat::RGBA8; gst.mFormatPrimary=LLPixelFormat::RGBA;
  gst.mFormatType=LLPixelType::UINT_8888_REV;
  const U8 gstRgba[4]={51,102,153,204};
  expect(gst.setSubImageVK(gstRgba,1,1,0,0,1,1),"GStreamer packed-RGBA media update");
  LLVKCpuLease gstLease{&gst}; LLVKTexturePrep::SourceSnapshot gstSource; copied=0;
  expect(gst.captureVKPrepSource(-1,0,0,enough,gstLease,gstSource,copied)==LLVKTexturePrep::CaptureResult::Ready,"GStreamer producer snapshot");
  expect(gst.mVKCpuDataOwned->data==std::vector<U8>(gstRgba,gstRgba+4),"GStreamer producer preserves RGBA byte order");
  expect(gstSource.pixelFormat==(U32)LLPixelFormat::RGBA&&gstSource.pixelType==(U32)LLPixelType::UBYTE
      &&gstSource.components==4,"GStreamer worker snapshot uses supported RGBA UBYTE layout");
  expect(gst.mFormatType==LLPixelType::UINT_8888_REV,"GStreamer legacy raw-reader type stays unchanged");
  std::array<U8,4> gstPrepared{}; LLVKTexturePrep::OutputLayout gstLayout{};
  expect(prepareTextureInput(gstSource,mediaRecipe,gstPrepared.data(),gstPrepared.size(),mediaScratch.data(),mediaScratch.size(),gstLayout),
      "captured GStreamer source reaches actual worker");
  expect(gstPrepared==std::array<U8,4>{51,102,153,204},"captured GStreamer RGBA REV bytes stay identical through worker");

  LLTextureData immutable; immutable.mVKCpuDataOwned=makeRaw(4,4,2); immutable.mVKCpuData=immutable.mVKCpuDataOwned.get(); immutable.mVKCpuImmutable=true;
  immutable.mVKGeneration=22; immutable.mVKSerial=45; immutable.mFormatPrimary=LLPixelFormat::LUMINANCE_ALPHA;
  immutable.mComponents=2; LLVKCpuLease immutableLease{&immutable};
  LLVKTexturePrep::SourceSnapshot retained; copied=123;
  expect(immutable.captureVKPrepSource(-1,0,200,enough,immutableLease,retained,copied)==LLVKTexturePrep::CaptureResult::Ready,"immutable decoded source ready");
  expect(copied==0&&retained.byteCount==32&&retained.components==2&&retained.pixelFormat==(U32)LLPixelFormat::LUMINANCE_ALPHA,"immutable bytes retained with two-channel metadata");

  LLTextureData alias; alias.mVKCpuDataOwned=makeRaw(); alias.mVKCpuData=alias.mVKCpuDataOwned.get();
  alias.mVKGeneration=23; alias.mVKSerial=46; LLVKCpuLease aliasLease{&alias};
  LLVKTexturePrep::SourceSnapshot aliasCopy; copied=0;
  expect(alias.captureVKPrepSource(-1,0,0,enough,aliasLease,aliasCopy,copied)==LLVKTexturePrep::CaptureResult::Ready,"unqualified owned pointer captured by copy");
  expect(copied==64&&aliasCopy.spans[0].data!=alias.mVKCpuDataOwned->getData(),"owned pointer alone is not immutable qualification");

  LLTextureData media; media.mComponents=4; media.mFormatPrimary=LLPixelFormat::RGBA;
  media.mVKCpuDataOwned=makeRaw(4,4,4); media.mVKCpuData=media.mVKCpuDataOwned.get();
  media.mVKCpuWritable=true;
  U8 full[64]; for(size_t i=0;i<sizeof(full);++i)full[i]=(U8)(255-i);
  expect(media.setSubImageVK(full,4,4,0,0,4,4),"first media update");
  LLVKCpuLease mediaLease{&media}; LLVKTexturePrep::SourceSnapshot mediaSrc; copied=0;
  expect(media.captureVKPrepSource(-1,0,0,enough,mediaLease,mediaSrc,copied)==LLVKTexturePrep::CaptureResult::Ready,"first media full source");
  expect(media.acknowledgeVKPrepSource(media.mVKGeneration),"ack media full");
  expect(media.setSubImageVK(full,4,4,3,3,1,1),"next media patch");
  expect(media.captureVKPrepSource((S32)mediaSrc.generation,0,0,enough,mediaLease,mediaSrc,copied)==LLVKTexturePrep::CaptureResult::Ready,"media patch source");
  expect(mediaSrc.baseGeneration>=0&&mediaSrc.spans[0].x==3&&mediaSrc.spans[0].y==3,"media patch base and bounds");
  std::cout<<"texture source capture checks passed\n";
}
'''

    cache_support = r'''#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>
#include "llvktextureprepsource.h"
using U8=uint8_t; using U16=uint16_t; using U32=uint32_t; using U64=uint64_t; using S32=int32_t;
struct LLGpuTexHeader { U16 w=0,h=0,full_w=0,full_h=0,levels=0,alpha_side=0; U8 comps=0,alpha_grad_pct=255,cov_cut=0,enc=0,enc_level=0,flags=0; U32 vk_format=0; U64 payload_bytes=0,payload_hash=0; enum:U8{F_ALPHA_MASK=1}; };
class LLGpuTexCache { public:
 struct Blob {struct Piece{U32 level=0,levels=0;std::vector<U8> bytes;}; LLGpuTexHeader hdr;U32 first_level=0;std::vector<Piece> pieces;};
 static LLVKTexturePrep::CaptureResult capturePrepSource(const std::shared_ptr<const Blob>&,U64,S32,S32,U64,LLVKTexturePrep::SourceSnapshot&);
};
'''
    cache_fixture = cache_support + "\n" + cache_snippet + r'''
static void expect(bool v,const char* n){if(!v){std::cerr<<"FAIL "<<n<<"\n";std::exit(2);}}
int main(){
 auto blob=std::make_shared<LLGpuTexCache::Blob>(); blob->hdr.w=64;blob->hdr.h=32;blob->hdr.full_w=256;blob->hdr.full_h=128;blob->hdr.levels=6;blob->hdr.comps=4;blob->hdr.vk_format=77;blob->hdr.payload_bytes=999;blob->first_level=2;
 LLGpuTexCache::Blob::Piece p; p.level=2;p.levels=4;p.bytes.reserve(128);p.bytes.resize(12);blob->pieces.push_back(std::move(p));
 std::weak_ptr<const LLGpuTexCache::Blob> weak=blob;std::shared_ptr<const LLGpuTexCache::Blob> input=blob;
 LLVKTexturePrep::SourceSnapshot s;expect(LLGpuTexCache::capturePrepSource(input,81,4,2,4096,s)==LLVKTexturePrep::CaptureResult::Ready,"partial cache capture");
 expect(s.valid()&&s.byteCount==12&&s.firstLevel==2&&s.spans[0].levelCount==4,"partial mip descriptor");
 expect(s.retainedBackingBytes>=128&&s.retainedBackingBytes>s.byteCount,"whole Blob capacity charged");
 input.reset();blob.reset();expect(!weak.expired(),"snapshot retains Blob");
 auto old=s;expect(LLGpuTexCache::capturePrepSource(weak.lock(),82,5,2,1,s)==LLVKTexturePrep::CaptureResult::Deferred,"cache cap deferred");
 expect(s.serial==old.serial&&s.byteCount==old.byteCount,"cache refusal preserves output");
 old.owner.reset();s.owner.reset();expect(weak.expired(),"Blob released with snapshot owner");
 std::cout<<"cache source checks passed\n";
}
'''
    out.mkdir(parents=True, exist_ok=True)
    inputs = [
        texture_cpp,
        texture_header,
        renderer_cpp,
        cache_cpp,
        cache_header,
        source_header,
        worker_cpp,
        root / "indra/newview/vkgfx/llrendervkbackend.cpp",
        root / "indra/llcommon/llpixelformat.h",
        root / "indra/llrender/llfontfreetype.cpp",
        root / "indra/newview/llviewertexture.cpp",
        root / "indra/newview/llviewertexture.h",
        root / "indra/newview/gltfscenemanager.cpp",
        root / "indra/newview/llgputexcache.h",
        pathlib.Path(__file__).resolve(),
    ]
    manifest = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    gen1 = out / "texture_prep_source_capture_checks.cpp"
    gen2 = out / "texture_prep_cache_capture_checks.cpp"
    gen1.write_text(cpp)
    gen2.write_text(cache_fixture)
    (out / "source_manifest.json").write_text(__import__("json").dumps(manifest, indent=2) + "\n")
    commands = [
        [args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(root / "indra/llrender"), "-I", str(root / "indra/llcommon"), "-I", str(root / "indra/newview/vkgfx"), str(gen1), "-o", str(out / "texture_prep_source_capture_checks")],
        [args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(root / "indra/llrender"), str(gen2), "-o", str(out / "texture_prep_cache_capture_checks")],
    ]
    (out / "commands.json").write_text(__import__("json").dumps(commands, indent=2) + "\n")
    if not args.run:
        print(f"prepared two exact-source fixtures in {out}")
        return 0
    for command in commands:
        result = subprocess.run(command, cwd=out, check=False)
        if result.returncode:
            return result.returncode
    for exe in (out / "texture_prep_source_capture_checks", out / "texture_prep_cache_capture_checks"):
        result = subprocess.run([str(exe)], cwd=out, check=False)
        if result.returncode:
            return result.returncode
    after = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    if after != manifest:
        raise SystemExit("source changed during fixture run")
    print("two exact-source texture capture fixtures passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
