#!/usr/bin/env python3
"""Check actual auxiliary texture-submit and completion boundaries without Vulkan."""
from pathlib import Path
import argparse
import subprocess


def body(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    level = 1
    end = opening + 1
    while level:
        level += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


p = argparse.ArgumentParser(description=__doc__)
p.add_argument('source_root', type=Path)
p.add_argument('output_dir', type=Path)
a = p.parse_args()
s = (a.source_root / 'indra/newview/vkgfx/llvulkancontext.cpp').read_text()
f = body(s, 'bool LLVulkanContext::renderUIWindow(NativeSurfaceId handle, const std::function<void()>&')
assert f.index('beginTexturePrepRecording()') < f.index('gVK.recordUploads(cmd)')
assert f.index('takePreparedRecordedUploads') < f.index('vkEndCommandBuffer(cmd)')
assert f.index('vkQueueSubmit2') < f.index('texturePrepRecordingSubmitted(UINT32_MAX)')
assert f.index('texturePrepRecordingSubmitted(UINT32_MAX)') < f.index('signalSurfaceSerial(targetSerial)')
assert 'texturePrepRecordingDropped(true)' in f and 'texturePrepRecordingDropped(false)' in f
submit = body(s, 'void LLVulkanContext::texturePrepRecordingSubmitted(')
assert 'if (frameSlot != UINT32_MAX) mImpl->mTexturePrepSlotSerial[frameSlot] = serial;' in submit
observe = body(s, 'void observeSurfaceCompletion(uint64_t completed)')
code = r'''
#include <cassert>
#include <cstdint>
struct Service {
 uint64_t retired=0, deadline=8; bool released=false; int reaps=0;
 void retireGpuUse(uint64_t v) { if(v>retired) retired=v; if(retired>=deadline) released=true; }
 void reapCompleted() { ++reaps; }
};
struct Context {
 Service* mTexturePrepService; uint64_t mSurfaceCompleted=0;
 uint64_t mTexturePrepSurfaceProof=5,mTexturePrepSurfaceSubmit=8;
'''+observe+r'''
};
int main() {
 Service service; Context c{&service};
 // An older main-frame fence cannot retire a later auxiliary submission.
 service.retireGpuUse(3); assert(!service.released);
 c.observeSurfaceCompletion(4); assert(!service.released && service.reaps==0);
 c.observeSurfaceCompletion(5); assert(service.released && service.retired==8 && service.reaps==1);
 assert(c.mTexturePrepSurfaceProof==0 && c.mTexturePrepSurfaceSubmit==0);
 // A newer marker covers all preceding graphics-queue submissions; a missing
 // marker remains pinned until a later accepted marker establishes completion.
 service.deadline=12;service.released=false;
 c.mTexturePrepSurfaceProof=9;c.mTexturePrepSurfaceSubmit=12;
 c.observeSurfaceCompletion(8);assert(!service.released);
 c.observeSurfaceCompletion(10);assert(service.released && service.retired==12);
 c.observeSurfaceCompletion(10);assert(service.reaps==2);
}
'''
a.output_dir.mkdir(parents=True, exist_ok=True)
cpp = a.output_dir / 'native_texture_submission.cpp'
exe = a.output_dir / 'native_texture_submission'
cpp.write_text(code)
subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
print('PASS auxiliary submission proof, older main fence, refused marker retention, later marker coverage')
