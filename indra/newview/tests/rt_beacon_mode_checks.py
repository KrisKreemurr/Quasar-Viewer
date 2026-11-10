#!/usr/bin/env python3
"""Execute production beacon mask/pipeline selection with fake handles, no GPU."""
import argparse
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET


def function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    level, end = 1, brace + 1
    while level:
        level += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source_root', type=Path)
    parser.add_argument('output_dir', type=Path)
    args = parser.parse_args()
    root, out = args.source_root.resolve(), args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = (root / 'indra/newview/vkgfx/llcleanrenderervk.cpp').read_text()
    catalog = source.split('const RtBcEntry kRtBcSpv[10] = {', 1)[1].split('};', 1)[0]
    symbols = re.findall(r'\{(kWorld\w+),', catalog)
    assert len(symbols) == 10
    assert symbols[0] == 'kWorldDeferRtTraceListBcSpv'
    assert symbols[3] == 'kWorldDeferRtSplitLayersVq2BcSpv'
    mask = function(source, 'uint32_t rtBcModeMask(')
    select = function(source, 'VkPipeline LLCleanRendererVK::rtBcPipe(')
    decide = function(source, 'void LLCleanRendererVK::rtHangBeaconDecide(')
    # Worker output handles are read only after poll/join, never while pending.
    assert decide.index('mRtBcNow = false;') < decide.index('mRtBcJob.poll()')
    assert decide.index('if (mRtBcJob.pending()) return;') < decide.index('const uint32_t missing')
    assert '(missing & (1u << k)) == 0u || mRtBcPipe[k]' in decide
    assert 'mRtBcQueuedMask |= missing;' in decide
    assert 'if (mode > 2u) mode = 0u;' in decide
    assert 'mRtBcArmed >= kRtBcArmFrames' in decide
    assert 'beacon %s (mode %u)' in source and 'n.bcMode = mRtBcMode;' in source
    missing = re.search(r'const uint32_t missing = ([^;]+);', decide)[1]
    settings = ET.parse(root / 'indra/newview/app_settings/settings.xml').getroot().find('map')
    entries = list(settings)
    entry = next(entries[i + 1] for i, child in enumerate(entries)
                 if child.tag == 'key' and child.text == 'RenderVKRTHangBeacon')
    fields = list(entry)
    values = {fields[i].text: fields[i + 1].text for i in range(0, len(fields), 2)}
    assert values['Type'] == 'U32' and values['Persist'] == '0' and values['Value'] == '0'
    cpp = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
using VkPipeline=std::uintptr_t;
constexpr VkPipeline VK_NULL_HANDLE=0;
const unsigned kRtBcSpv[10]={};
enum {VQS_LAYERS=4,GIS_TRACE=0};
struct LLCleanRendererVK {
 static constexpr uint32_t kRtBcKernels=10;
 bool mRtBcNow=true;uint32_t mRtBcMask=0;
 VkPipeline mRtBcPipe[10]={};
 VkPipeline mRtTraceListPipe=100,mRtFwdPipe=101,mVisWalkPipe=102;
 VkPipeline mRtVqPipe[5]={0,0,0,0,103};
 VkPipeline mPdnSunVisPipe=104,mPdnSunVisIsoPipe=105,mPdnVisPipe=106,mPdnVisIsoPipe=107;
 VkPipeline mGiTracePipe=108,mGiSplitPipe[1]={109};
 VkPipeline rtBcPipe(VkPipeline p) const;
};
'''+mask+'\n'+select+r'''
int main(){
 LLCleanRendererVK r;
 for(uint32_t k=0;k<10;++k)r.mRtBcPipe[k]=1000+k;
 assert(rtBcModeMask(0)==0&&rtBcModeMask(1)==0x3ff&&rtBcModeMask(2)==9);
 assert(rtBcModeMask(3)==0&&rtBcModeMask(0xffffffffu)==0);
 unsigned checked=0;
 for(uint32_t mode:{0u,1u,2u,3u,0xffffffffu})for(bool armed:{false,true}){
  r.mRtBcMask=rtBcModeMask(mode);r.mRtBcNow=armed;
  for(uint32_t k=0;k<10;++k){
   auto normal=VkPipeline(100+k);
   const bool selected=armed&&((r.mRtBcMask&(1u<<k))!=0);
   assert(r.rtBcPipe(normal)==(selected?1000+k:normal));
   r.mRtBcPipe[k]=0;assert(r.rtBcPipe(normal)==normal);r.mRtBcPipe[k]=1000+k;
   ++checked;
  }
  assert(r.rtBcPipe(0)==0&&r.rtBcPipe(99999)==99999);
 }
 // Exact production missing-mask expression, narrow->full->narrow->off->narrow.
 uint32_t mRtBcQueuedMask=0;
 const uint32_t modes[]={2,1,2,0,2},expected[]={9,0x3f6,0,0,0};
 for(unsigned i=0;i<5;++i){uint32_t mRtBcMask=rtBcModeMask(modes[i]);
  uint32_t next=MISSING_EXPRESSION;assert(next==expected[i]);mRtBcQueuedMask|=next;}
 std::printf("PASS %u actual pipeline mappings + null/missing twins, mode transitions, XML and pending-job guards\n",checked);
}
'''
    cpp = '#include <initializer_list>\n' + cpp.replace('MISSING_EXPRESSION', missing)
    path = out / 'rt_beacon_mode_checks.cpp'
    path.write_text(cpp)
    binary = path.with_suffix('')
    subprocess.run(['c++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
