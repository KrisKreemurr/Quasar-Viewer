#!/usr/bin/env python3
"""Compile the actual shared selection code with CPU-only texture/backend stubs."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
a.out.mkdir(parents=True, exist_ok=True)
header = (root / 'indra/llrender/lluidrawimage.h').read_text()
header = '\n'.join(line for line in header.splitlines() if not line.startswith('#include "'))
source = (root / 'indra/llrender/llrender2dutils.cpp').read_text()
code = source[source.index('namespace\n{\n    LLUIImageDrawState*'):source.index('//\n// Functions')]
def function(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

wrappers = '\n'.join(function(signature) for signature in [
    'void gl_draw_pixel_snapped_image(S32',
    'void gl_draw_scaled_image_with_border(S32 x, S32 y, S32 width',
    'void gl_draw_scaled_rotated_image(S32',
])

stubs = r'''
#include <cassert>
#include <algorithm>
#include <cstdint>
#include <new>
#include <vector>
#include <cstdlib>
bool failAllocation=false;
void* operator new(std::size_t n) { if(failAllocation) throw std::bad_alloc(); if(void* p=std::malloc(n)) return p; throw std::bad_alloc(); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
using U32 = uint32_t; using U64 = uint64_t; using S32 = int; using F32 = float;
template<class T> struct LLPointer {
 T* p = nullptr; LLPointer() = default; LLPointer(T* v):p(v) {}
 T* operator->() const { return p; } operator T*() const { return p; }
 LLPointer& operator=(T* v) { p=v; return *this; }
};
struct LLRectf { float mLeft=0, mTop=1, mRight=1, mBottom=0; };
struct LLColor4 {};
struct LLTextureData { bool ready=false, needs=false; int requests=0;
 void setVKUiKind(int) {} bool takeVKUiNeedsCpu() { bool r=needs; needs=false; return r; } };
struct LLTexture { LLTextureData data; int copies=0;
 LLTextureData* getTextureData() { return &data; } int getVKUiKind() { return 0; }
 void vkRequestCpuCopy() { ++copies; }
};
struct Backend { bool broken=false;
 bool prepareImageGL(LLTextureData* d) { if (!d) return false; ++d->requests; if(!d->ready) broken=true; return d->ready; }
};
struct LLRender { static Backend* sBackend; };
Backend backend; Backend* LLRender::sBackend=&backend;
struct Vec { float mV[3]={0,0,0}; };
struct GL { Vec offset; Vec getUITranslation() { return offset; } } gGL;
'''
prepared = r'''
int drawCalls=0; LLTexture* drawnTexture=nullptr; float drawnUv=0;
static void gl_draw_pixel_snapped_image_prepared(S32,S32,S32,S32,LLTexture* t,const LLColor4&,bool) { ++drawCalls; drawnTexture=t; }
static void gl_draw_scaled_image_with_border_prepared(S32,S32,S32,S32,LLTexture* t,const LLColor4&,bool,const LLRectf& uv,const LLRectf&,bool) { ++drawCalls; drawnTexture=t; drawnUv=uv.mRight; }
static void gl_draw_scaled_rotated_image_prepared(S32,S32,S32,S32,F32,LLTexture* t,const LLColor4&,const LLRectf& uv) { ++drawCalls; drawnTexture=t; drawnUv=uv.mRight; }
'''
tests = r'''
LLUIDrawImage image(LLTexture* texture, float uv=1) { LLUIDrawImage v; v.texture=texture; v.uv.mRight=uv; v.center.mLeft=uv/4; return v; }
int main() {
 LLTexture normal, selected, other; normal.data.ready=true; other.data.ready=true;
 LLUIImageDrawState first, second;
 auto choose=[&](LLUIDrawImage v, int x=0, unsigned kind=1) {return choose_ui_draw_image(x,0,100,20,kind,v);};
 assert(!choose(image(&selected)).texture); // unscoped pending is never white
 { LLUIImageDrawScope scope(first); assert(choose(image(&normal,.5f)).texture==&normal); }
 { LLUIImageDrawScope scope(first);
   auto result=choose(image(&selected)); assert(result.texture==&normal);
   assert(result.uv.mRight==.5f && result.center.mLeft==.125f); // atlas + nine-slice retained
   assert(backend.broken); // pending desired request invalidates retained replay
   { LLUIImageDrawScope child(second); assert(!choose(image(&selected)).texture); }
   assert(sImageDrawState==&first);
 }
 selected.data.ready=true;
 { LLUIImageDrawScope scope(first); assert(choose(image(&selected)).texture==&selected); }
 selected.data.ready=false;
 { LLUIImageDrawScope scope(first); assert(!choose(image(&selected)).texture); } // retired old also unavailable
 // Same rectangle has separate occurrences, not the last texture of the prior widget.
 LLUIImageDrawState layers;
 { LLUIImageDrawScope scope(layers); choose(image(&normal)); choose(image(&other)); }
 { LLUIImageDrawScope scope(layers); assert(choose(image(&selected)).texture==&normal); assert(choose(image(&selected)).texture==&other); }
 // Pending first occurrences must keep their position before later ready layers.
 LLUIImageDrawState pending;
 { LLUIImageDrawScope scope(pending); assert(!choose(image(&selected)).texture); choose(image(&other)); }
 { LLUIImageDrawScope scope(pending); assert(!choose(image(&selected)).texture); assert(choose(image(&selected)).texture==&other); }
 // Translated cells owned by one list cannot borrow one another's history.
 LLUIImageDrawState cells;
 { LLUIImageDrawScope scope(cells); choose(image(&normal)); gGL.offset.mV[1]=25; choose(image(&other)); }
 { LLUIImageDrawScope scope(cells); gGL.offset.mV[1]=0; assert(choose(image(&selected)).texture==&normal); gGL.offset.mV[1]=25; assert(choose(image(&selected)).texture==&other); }
 gGL.offset.mV[1]=0;
 // Source-copy requests are forwarded even when no previous image exists.
 selected.data.needs=true;
 { LLUIImageDrawScope scope(second); choose(image(&selected),44); }
 assert(selected.copies==1);
 // Nested nine-slice helpers share one decision, never change texture midway.
 { PreparedUIImageScope prepared; assert(choose(image(&selected)).texture==&selected); }
 assert(sPreparedImageDepth==0);
 // Cap, overflow, replacement and empty-site paths stay bounded.
 LLUIImageDrawState many;
 { LLUIImageDrawScope scope(many); for(int i=0;i<200;++i) choose(image(&normal),i); }
 assert(many.sites.size()==64);
 { LLUIImageDrawScope scope(many); for(int i=0;i<200;++i) assert(!choose(image(&selected),i+200).texture); }
 assert(many.sites.size()==64);
 assert(sImageDrawState==nullptr);
 LLUIImageDrawState allocationFailure;
 { LLUIImageDrawScope scope(allocationFailure);
   failAllocation=true;
   assert(choose(image(&normal)).texture==&normal);
   assert(!choose(image(&selected)).texture);
   failAllocation=false;
 }
 assert(allocationFailure.sites.empty());
 // Execute all three production public draw wrappers, not just their selector.
 LLUIImageDrawState realDraw; LLColor4 color; LLRectf uv, center; uv.mRight=.25f;
 { LLUIImageDrawScope scope(realDraw);
   gl_draw_pixel_snapped_image(1,0,16,16,&normal,color,false);
   gl_draw_scaled_image_with_border(2,0,16,16,&normal,color,false,uv,center,true);
   gl_draw_scaled_rotated_image(3,0,16,16,45,&normal,color,uv);
 }
 assert(drawCalls==3);
 { LLUIImageDrawScope scope(realDraw);
   gl_draw_pixel_snapped_image(1,0,16,16,&selected,color,false);
   gl_draw_scaled_image_with_border(2,0,16,16,&selected,color,false,center,center,false);
   assert(drawnTexture==&normal && drawnUv==.25f);
   gl_draw_scaled_rotated_image(3,0,16,16,0,&selected,color,center);
   assert(drawnTexture==&normal && drawnUv==.25f);
   gl_draw_pixel_snapped_image(99,0,16,16,&selected,color,false);
 }
 assert(drawCalls==6); // First-use pending site emitted no geometry.
}
'''
cpp = a.out / 'ui_image_draw_checks.cpp'
cpp.write_text(stubs + header + "\n" + code + prepared + wrappers + tests)
exe = a.out / 'ui_image_draw_checks'
subprocess.run(['c++','-std=c++20','-O2','-Wall','-Wextra','-Werror',str(cpp),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
print('actual shared UI draw selection: PASS')
