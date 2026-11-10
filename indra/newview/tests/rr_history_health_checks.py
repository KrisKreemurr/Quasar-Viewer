#!/usr/bin/env python3
"""Source-bound RR finite guards and semantic-key regression; no GPU/vendor claim.
Usage: rr_history_health_checks.py SOURCE_ROOT OUTPUT_DIR
Requires the project's existing GLM headers (system or CPLUS_INCLUDE_PATH).
"""
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

root, out = map(Path, sys.argv[1:3])
out.mkdir(parents=True, exist_ok=True)
shader = (root / 'indra/vkclean/shaders/clean_rr_guides.comp').read_text()
host = (root / 'indra/newview/vkgfx/llcleandlssrrvk.cpp').read_text()
renderer = (root / 'indra/newview/vkgfx/llcleanrenderervk.cpp').read_text()

def extract(signature):
    start = shader.index(signature)
    cursor = shader.index('{', start) + 1
    nesting = 1
    while nesting:
        nesting += (shader[cursor] == '{') - (shader[cursor] == '}')
        cursor += 1
    return shader[start:cursor]

functions = '\n'.join(extract(signature) for signature in (
    'bool rrFinite(', 'bool rrFinite3(', 'vec3 rrSafeColour(', 'vec3 rrSafeNormal(',
    'float rrSafeRoughness(', 'float rrSafeHit(', 'float rrSafeExposure('))
functions = re.sub(r'(?<![\w.])(\d+\.\d+|\d+e-\d+)(?![\w.])', r'\1f', functions)
program = r'''
#include "llrrhistorypolicy.h"
#include <glm/glm.hpp>
#include <cassert>
#include <cmath>
#include <limits>
#include <cstdio>
using namespace glm;
using std::isnan;
using std::isinf;
constexpr float kFar=65504.f;
''' + functions + r'''
int main()
{
    const float invalid=std::numeric_limits<float>::quiet_NaN();
    const float infinite=std::numeric_limits<float>::infinity();
    for (float value : {invalid, infinite, -infinite})
    {
        assert(rrSafeColour(vec3(value),1.f)==vec3(0.f));
        assert(rrSafeNormal(vec3(value))==vec3(0,0,1));
        assert(rrSafeRoughness(value)==1.f);
        assert(rrSafeHit(value)==kFar);
        assert(rrSafeExposure(value)==1.f);
    }
    assert(rrSafeNormal(vec3(0))==vec3(0,0,1));
    assert(rrSafeNormal(vec3(0,0,3))==vec3(0,0,1));
    assert(rrSafeNormal(vec3(0,0,1))==vec3(0,0,1));
    assert(rrSafeColour(vec3(-1,.5f,2),1.f)==vec3(0,.5f,1));
    assert(rrSafeRoughness(-1)==0 && rrSafeRoughness(2)==1);
    assert(rrSafeHit(0)==kFar && rrSafeHit(-1)==kFar);
    assert(rrSafeHit(70000)==kFar && rrSafeHit(4)==4);
    assert(rrSafeExposure(0)==1 && rrSafeExposure(2)==2);
    LLRRHistory::Policy previous{}, current{};
    assert(LLRRHistory::changedMask(previous,current)==0);
    for (uint32_t field=0;field<current.size();++field)
    {
        ++current[field];
        assert(LLRRHistory::changedMask(previous,current)==(1u<<field));
        previous=current; // successful evaluate acknowledges exactly this key
        assert(LLRRHistory::changedMask(previous,current)==0);
    }
    LLRRHistory::ResetBatch batch;
    batch.add(1,2); batch.add(32,4);
    assert(batch.reasons==33 && batch.policyBits==6 && batch.requests==2);
    batch.requests=UINT32_MAX; batch.add(4,8);
    assert(batch.requests==UINT32_MAX && batch.reasons==37 && batch.policyBits==14);
    batch={}; assert(batch.requests==0 && batch.reasons==0 && batch.policyBits==0);
    current.fill(0); previous.fill(1);
    assert(LLRRHistory::changedMask(previous,current)==65535u);
    std::puts("PASS actual RR guards (NaN/Inf/zero/range/valid), all 16 semantic key fields and one-shot acknowledgement");
}
'''
(out / 'check.cpp').write_text(program)
subprocess.run(['c++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                '-I', str(root / 'indra/newview/vkgfx'), str(out / 'check.cpp'),
                '-o', str(out / 'check')], check=True)
subprocess.run([str(out / 'check')], check=True)

assert 'e.reset = f.reset || mFirstEval || mInputsChanged || policyChanges != 0u;' in host
assert host.index('mPolicy = f.policy;') > host.index('if (!LLVKNeuralRecon::instance().evaluate(')
assert 'now - mHealthTime >= std::chrono::seconds(1)' in host
assert 'now - mResetLogTime >= std::chrono::seconds(1)' in host
assert 'VK_PIPELINE_STAGE_2_HOST_BIT' in host and 'VK_ACCESS_2_HOST_READ_BIT' in host
assert 'VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT' in host
assert 'VK_BUFFER_USAGE_STORAGE_BUFFER_BIT' in host
assert 'mInputsChanged = mInputsChanged || (mBuiltValid && !(mBuilt.t == w.t));' in host
assert 'VK_WHOLE_SIZE, B)' in host and 'kHealthOffset' in host
assert 'subgroupAdd(nonfinite ? 1u : 0u)' in shader
assert 'imageStore(oNormalRough, q, vec4(N, rrSafeRoughness(rough)))' in shader
assert 'imageStore(oHitDist, q, vec4(rrSafeHit(hit)))' in shader
assert 'imageStore(oSpecular, q, vec4(rrSafeColour(spec, 1.0)' in shader
assert 512+21*4 <= 768 and 768+2*4 <= 1024 # health words vs CPU metadata/next slot
policy = renderer[renderer.index('// Semantic configuration only:'):renderer.index('ok = mRr[v]->record(cmd, f, err);')]
for forbidden in ('mShadowMergeSunW', 'rsMoved', 'mPdnRouteKey', 'mExposureValue'):
    assert forbidden not in policy
assert 'mRtFallback' in policy and 's_sunPolicy' in policy
assert 'f.resetReasons =' in renderer
settings=list(ET.parse(root/'indra/newview/app_settings/settings.xml').getroot().find('map'))
entry=next(settings[i+1] for i,key in enumerate(settings) if key.tag=='key' and key.text=='RenderVKDlssGuideHealth')
fields=list(entry)
values={key.text:fields[i+1].text for i,key in enumerate(fields) if key.tag=='key'}
assert values['Persist']=='0' and values['Value']=='0'
print('PASS fenced/rate-limited diagnostic wiring; policy exclusions; always-on guards; transient diagnostic default off')
