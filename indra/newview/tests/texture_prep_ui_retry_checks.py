#!/usr/bin/env python3
"""Exercise extracted UI retry/admission/publication code with injected outcomes.

The service and facade are fake boundaries. This checks retry state across accepted
worker failures, every source-key field, and accepted-submit publication; it does
not establish GPU execution or fence completion.
"""
from __future__ import annotations

import argparse
import hashlib
import pathlib
import runpy
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-root', type=pathlib.Path, required=True)
    parser.add_argument('--backend-source', type=pathlib.Path)
    parser.add_argument('--out', type=pathlib.Path, required=True)
    parser.add_argument('--run', action='store_true')
    args = parser.parse_args()
    root, out = args.source_root.resolve(), args.out.resolve()
    if root not in out.parents:
        raise SystemExit('output must be under the owned worktree')
    extract = runpy.run_path(str(root / 'indra/newview/tests/texture_prep_ui_format_checks.py'))['extract_braced']
    backend_path = args.backend_source or root / 'indra/newview/vkgfx/llrendervkbackend.cpp'
    source = backend_path.read_text()
    header_path = root / 'indra/newview/vkgfx/llrendervkbackend.h'
    header = header_path.read_text()
    cached = extract(header, 'struct CachedTex') + ';'
    helpers = source[source.index('    auto failureKeyMatches ='):source.index('    auto clearCandidate =')]
    admission = extract(source, 'if (queued == LLVKTexturePrep::QueueResult::Accepted')
    publication = extract(source, 'if (cached.candidateVkId && cached.candidateGeneration == gen')
    submitted = extract(source, 'if (state == LLVKTexturePrepService::State::Submitted)')
    if publication not in submitted:
        raise RuntimeError('publication no longer requires Submitted')
    cpp = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
using U8=uint8_t; using U32=uint32_t; using U64=uint64_t; using S32=int32_t; using F64=double;
namespace LLVKTexturePrep { enum class QueueResult { Accepted, AlreadyQueued }; }
struct LLVKTexturePrepService {
 enum class FailureReason { None, Exception, UiInputRejected, OutputLayoutRejected };
 struct Ticket { U32 generation=0; uint16_t slot=0xffff; bool valid()const{return generation&&slot<128;} };
 unsigned releases=0; bool release(Ticket){++releases;return true;}
};
struct Facade { unsigned destroyed=0; U64 frameCounter(){return 9;} void destroyTexture(U32 id){assert(id==73);++destroyed;} } gVK;
struct LLFrameTimer { static F64 getElapsedSeconds(){return 0;} };
struct Image { U8 getVKUiKind(){return 1;} unsigned uploaded=0; void noteVKUiUploaded(S32){++uploaded;} };
using LLTextureData=Image;
struct Harness {
 CACHED
 enum class Event { Accept, AlreadyQueued, Ready, Fail, Publish };
 using Reason=LLVKTexturePrepService::FailureReason;
 std::unordered_map<const LLTextureData*,CachedTex> mTexCache;
 Image owner; Image* image=&owner; LLVKTexturePrepService service;
 U64 serial=7,rawBytes=64; S32 gen=3,discard=0; U32 w=4,h=4,components=4,imageComponents=4,primaryFormat=1;
 bool writable=false; U8 uiKind=1; const void* raw=&owner; const void* rawPixels=this; F64 nowSec=1;
 U64 mCacheBytes=128,mStatUploadBytes=0; unsigned mStatUploads=0,mStatReuploads=0,admissions=0;
 Harness(){auto& c=mTexCache[image];c.vkId=73;c.bytes=64;c.serial=serial;}
 CachedTex& entry(){return mTexCache.at(image);}
 U32 step(Event event, Reason reason=Reason::Exception, bool permanent=false){
  auto it=mTexCache.find(image); LLVKTexturePrepService* prep=&service;
  HELPERS
  CachedTex& cached=it->second;
  if(event==Event::Accept || event==Event::AlreadyQueued){
   const auto queued=event==Event::Accept ? LLVKTexturePrep::QueueResult::Accepted : LLVKTexturePrep::QueueResult::AlreadyQueued;
   LLVKTexturePrepService::Ticket ticket{1,2};
   ADMISSION
   ++admissions;
  } else if(event==Event::Fail) {
   cached.prepTicket={};rememberFailure(cached,reason,permanent);
  } else if(event==Event::Publish) {
   cached.candidateVkId=99;cached.candidateWidth=w;cached.candidateHeight=h;
   cached.candidateGeneration=gen;cached.candidateBytes=64;
   PUBLICATION
  }
  return cached.vkId;
 }
};
int main(){
 using E=Harness::Event;using R=Harness::Reason;
 Harness h;
 for(unsigned n=0;n<9;++n){
  h.step(n%2 ? E::AlreadyQueued:E::Accept);
  h.nowSec+=0.125;h.step(E::Fail);
  const double expected=std::min(8.0,0.25*(1u<<std::min(n,5u)));
  const double actual=h.entry().prepRetryAfterSec-h.nowSec;
  if(actual!=expected){std::fprintf(stderr,"accepted worker failure %u: delay %.2f, expected %.2f\n",n+1,actual,expected);return 1;}
  const auto admitted=h.admissions;
  h.nowSec=h.entry().prepRetryAfterSec-0.125;
  assert(h.step(E::Accept)==73 && h.admissions==admitted);
  h.nowSec=h.entry().prepRetryAfterSec;
 }
 const auto retries=h.entry().prepFailureRetries;
 h.step(E::Accept);h.step(E::Ready);
 assert(h.entry().prepFailureValid && h.entry().prepFailureRetries==retries);
 assert(h.step(E::Publish)==99);
 assert(!h.entry().prepFailureValid && !h.entry().prepFailurePermanent && !h.entry().prepFailureRetries);
 assert(h.entry().prepRetryAfterSec==0 && h.owner.uploaded==1 && h.service.releases==1 && gVK.destroyed==1);
 h.nowSec+=1;h.step(E::Fail);assert(h.entry().prepRetryAfterSec-h.nowSec==0.25);
 for(unsigned field=0;field<15;++field){
  Harness t;t.step(E::Fail,R::UiInputRejected,true);t.nowSec+=1000;
  assert(t.step(E::Accept)==73 && t.admissions==0 && t.entry().prepFailurePermanent);
  switch(field){
   case 0:++t.serial;break;case 1:++t.gen;break;case 2:++t.discard;break;
   case 3:++t.w;break;case 4:++t.h;break;case 5:++t.components;break;
   case 6:++t.imageComponents;break;case 7:++t.primaryFormat;break;case 8:++t.rawBytes;break;
   case 9:t.writable=!t.writable;break;case 10:++t.uiKind;break;
   case 11:t.raw=nullptr;break;case 12:t.rawPixels=nullptr;break;
   case 13:++t.gen; t.raw=nullptr;break;case 14:++t.serial;t.rawPixels=nullptr;break;
  }
  t.step(E::Accept);assert(t.admissions==1 && !t.entry().prepFailureValid && !t.entry().prepFailureRetries);
 }
 std::puts("extracted UI retry: PASS (backoff cap, admission/Ready retention, Submitted publication reset, deterministic source changes)");
}
'''
    for key, value in [('CACHED', cached), ('HELPERS', helpers), ('ADMISSION', admission), ('PUBLICATION', publication)]:
        cpp = cpp.replace(key, value)
    out.mkdir(parents=True, exist_ok=True)
    generated = out / 'checks.cpp'
    generated.write_text(cpp)
    (out / 'source-receipt.txt').write_text('\n'.join(
        f'{p} sha256={hashlib.sha256(p.read_bytes()).hexdigest()}' for p in (backend_path, header_path, generated)) + '\n')
    print(generated)
    if args.run:
        exe = out / 'checks'
        result = subprocess.run(['c++', '-std=c++20', '-O0', '-Wall', '-Wextra', '-Werror', '-UNDEBUG', str(generated), '-o', str(exe)], cwd=out, timeout=30)
        if result.returncode:
            return result.returncode
        return subprocess.run([str(exe)], cwd=out, timeout=10).returncode
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
