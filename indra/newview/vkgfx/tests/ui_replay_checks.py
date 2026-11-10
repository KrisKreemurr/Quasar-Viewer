#!/usr/bin/env python3
"""Exercise production UI replay with a CPU command recorder, without Vulkan.

--revision can select a historical implementation to demonstrate the failure.
Generated source and executable must be placed outside the source worktree.
"""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[4])
    parser.add_argument('--revision')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compiler', default='c++')
    args = parser.parse_args()
    relative = 'indra/newview/vkgfx/llrendervk/llrendervk.cpp'
    if args.revision:
        source = subprocess.check_output(['git', '-C', str(args.source_root), 'show',
                                          f'{args.revision}:{relative}'], text=True)
    else:
        source = (args.source_root / relative).read_text()
    start = source.index('void LLRenderVK::recordDraws(')
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    production = source[start:end]
    prelude = r'''
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>
using VkCommandBuffer=uint64_t; using VkPipeline=uint64_t;
using VkBuffer=uint64_t; using VkDeviceSize=uint64_t;
using VkDescriptorSet=uint64_t;
constexpr uint64_t VK_NULL_HANDLE=0;
constexpr int VK_PIPELINE_BIND_POINT_GRAPHICS=0, VK_SHADER_STAGE_VERTEX_BIT=1,
              VK_SHADER_STAGE_FRAGMENT_BIT=2, VK_FORMAT_R16G16B16A16_SFLOAT=97;
struct VkViewport {float x,y,width,height,minDepth,maxDepth;};
struct VkOffset2D {int32_t x,y;};
struct VkExtent2D {uint32_t width,height;};
struct VkRect2D {VkOffset2D offset; VkExtent2D extent;};
namespace glm { struct mat4 {float v[16]{};}; }
#define VK_PUSH_BUDGET_CHECK(T) static_assert(sizeof(T)==64)
struct Recorder {
    VkPipeline active=999; // state inherited from a preceding world pass
    unsigned badBinds=0, descriptorBinds=0;
    std::vector<VkPipeline> binds;
    std::vector<std::pair<uint32_t,VkPipeline>> draws;
} rec;
void vkCmdSetViewport(VkCommandBuffer,uint32_t,uint32_t,const VkViewport*){}
void vkCmdBindVertexBuffers(VkCommandBuffer,uint32_t,uint32_t,const VkBuffer*,const VkDeviceSize*){}
void vkCmdBindDescriptorSets(VkCommandBuffer,int,uint64_t,uint32_t,uint32_t,const VkDescriptorSet* set,uint32_t,const void*) {
    assert(*set); ++rec.descriptorBinds;
}
void vkCmdBindPipeline(VkCommandBuffer,int,VkPipeline pipeline) {
    if(!pipeline) ++rec.badBinds;
    rec.binds.push_back(pipeline); rec.active=pipeline;
}
void vkCmdSetScissor(VkCommandBuffer,uint32_t,uint32_t,const VkRect2D*){}
void vkCmdSetLineWidth(VkCommandBuffer,float){}
void vkCmdPushConstants(VkCommandBuffer,uint64_t,int,uint32_t,size_t,const void*){}
void vkCmdDraw(VkCommandBuffer,uint32_t,uint32_t,uint32_t first,uint32_t) {
    rec.draws.emplace_back(first,rec.active);
}
struct Context {float lineWidthMax()const{return 1.f;}};
struct LLRenderVK {
    struct PendingDraw {
        bool offscreen=false,labelOccl=false,scissorEnable=false,isLine=false,alphaTest=false;
        VkPipeline pipe=11,pipePlain=11;
        VkDescriptorSet dset=3;
        float lineWidth=1.f,minAlpha=0.f,anchor[4]{};
        VkRect2D scissor{}; glm::mat4 mvp;
        uint32_t count=3,first=0;
    };
    std::vector<PendingDraw> mPending;
    bool mRecordingOffscreen=false;
    uint64_t mLayout=7;
    Context context; Context* mCtx=&context;
    int mFrameColorFormat=VK_FORMAT_R16G16B16A16_SFLOAT;
    float mDisplayPaperWhiteNits=350.f;
    static constexpr uint32_t kPushAnchorOffset=64,kPushMinAlphaOffset=80;
    void recordDraws(VkCommandBuffer,uint32_t,uint32_t,VkBuffer,size_t,size_t,VkDescriptorSet);
};
'''
    checks = r'''
int main() {
    using Draw=LLRenderVK::PendingDraw;
    LLRenderVK renderer;
    auto replay=[&](std::vector<Draw> pending,uint64_t depth=0) {
        rec=Recorder{}; renderer.mPending=std::move(pending);
        renderer.recordDraws(1,1920,1080,4,0,renderer.mPending.size(),depth);
        assert(rec.badBinds==0);
    };
    Draw missing; missing.pipe=missing.pipePlain=0;
    replay({missing}); assert(rec.draws.empty() && rec.binds.empty());
    Draw valid; valid.first=1;
    Draw later=valid; later.first=3;
    replay({missing,valid,missing,later});
    assert((rec.draws==std::vector<std::pair<uint32_t,VkPipeline>>{{1,11},{3,11}}));
    assert(rec.binds.size()==1); // missing batches do not corrupt the bind cache
    Draw noDescriptor=valid; noDescriptor.dset=0;
    replay({noDescriptor,valid}); assert(rec.draws.size()==1 && rec.draws[0].first==1);
    Draw label=valid; label.labelOccl=true; label.pipe=21; label.pipePlain=0;
    replay({label}); assert(rec.draws.empty()); // selected plain fallback is unavailable
    replay({label},5); assert(rec.draws.size()==1 && rec.draws[0].second==21);
    label.pipe=0; label.pipePlain=31;
    replay({label}); assert(rec.draws.size()==1 && rec.draws[0].second==31);
    replay({label},5); assert(rec.draws.empty()); // unavailable depth-tested variant
    Draw off=valid; off.offscreen=true;
    replay({off,valid}); assert(rec.draws.size()==1);
    renderer.mRecordingOffscreen=true;
    replay({missing,off}); assert(rec.draws.size()==1 && rec.draws[0].second==11);
    renderer.mRecordingOffscreen=false;
    replay({valid,later}); assert(rec.draws.size()==2 && rec.binds.size()==1);
}
'''
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    cpp = output / 'ui_replay.cpp'
    exe = output / 'ui_replay'
    cpp.write_text(prelude + production + checks)
    subprocess.run([args.compiler, '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                    str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    print('PASS: production UI replay rejects absent pipelines/descriptors and preserves valid state')


if __name__ == '__main__':
    main()
