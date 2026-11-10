#!/usr/bin/env python3
"""Exercise actual region snapshot consumers with real LLUnits and fake endpoints.

This is not a viewer TU, actor, socket, or lifecycle integration check.
"""
import argparse
import hashlib
import json
import pathlib
import subprocess


def body(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    depth = 0
    # These two selected methods contain no braces in string/comment literals.
    for pos in range(opening, len(source)):
        if source[pos] == '{':
            depth += 1
        elif source[pos] == '}':
            depth -= 1
            if not depth:
                return source[start:pos + 1]
    raise ValueError('unterminated production method')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-root', type=pathlib.Path, required=True)
    parser.add_argument('--out', type=pathlib.Path, required=True)
    parser.add_argument('--packages-include', type=pathlib.Path, required=True)
    parser.add_argument('--run', action='store_true')
    args = parser.parse_args()
    root, out = args.source_root.resolve(), args.out.resolve()
    if out == root:
        raise ValueError('dedicated output directory required')
    out.mkdir(parents=True, exist_ok=True)
    source_path = root / 'indra/newview/llviewerregion.cpp'
    header_path = root / 'indra/newview/llviewerregion.h'
    source = source_path.read_text()
    header = header_path.read_text()
    for declaration in ('mTransportStatsIncarnation = 0', 'mHasTransportStats = false',
                        'mLastKnownTransportLost = 0'):
        if declaration not in header:
            raise ValueError('missing production field: ' + declaration)
    methods = '\n'.join(body(source, marker) for marker in (
        'void LLViewerRegion::updateNetStats()', 'U32 LLViewerRegion::getPacketsLost() const'))
    support = r'''
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include "llunits.h"
#include "lltransportcircuittypes.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"CHECK %d: %s\n",__LINE__,#x); std::abort(); } } while(false)
#undef LL_INFOS
#undef LL_ENDL
#define LL_INFOS() std::ostringstream()
#define LL_ENDL std::endl
struct Timer {
 F32 elapsed=0; unsigned resets=0;
 F32 getElapsedTimeAndResetF32() { const F32 value=elapsed;elapsed=0;++resets;return value; }
};
struct Host {};
std::ostream& operator<<(std::ostream& out,const Host&) { return out<<"test-host"; }
struct Message {
 bool available=true; LLTransportCircuitSnapshot value{};
 bool getCircuitSnapshot(Host,LLTransportCircuitSnapshot& out)const {
  if(!available){return false;}out=value;return true;
 }
};
Message* gMessageSystem=nullptr;
struct LLViewerRegion {
 struct Impl { Timer mLastNetUpdate; Host mHost; } impl;
 Impl* mImpl=&impl;
 bool mAlive=false; F32 mDeltaTime=0;
 U32 mPacketsIn=0,mLastPacketsIn=0,mPacketsOut=0,mLastPacketsOut=0;
 U32Bits mBitsIn{},mLastBitsIn{}; F32Bits mBitsReceived{}; F32 mPacketsReceived=0;
 S32 mPacketsLost=0,mLastPacketsLost=0; U32Milliseconds mPingDelay{};
 U64 mTransportStatsIncarnation=0; bool mHasTransportStats=false;
 mutable U32 mLastKnownTransportLost=0;
 void updateNetStats(); U32 getPacketsLost()const;
};
'''
    cases = r'''
int main() {
 LLViewerRegion r;Message msg;gMessageSystem=&msg;
 auto& s=msg.value;s.state=LLTransportSnapshotState::Present;
 s.incarnation=1;s.packetsIn=5;s.bytesIn=10;s.packetsOut=3;s.packetsLost=2;
 s.pingMilliseconds=123;s.alive=false;s.lossQuality=LLTransportLossQuality::Complete;
 r.impl.mLastNetUpdate.elapsed=2;r.updateNetStats();
 CHECK(r.mAlive);CHECK(r.mPacketsIn==5&&r.mPacketsReceived==5);
 CHECK(r.mBitsReceived.value()==80&&r.mPingDelay.value()==123&&r.mDeltaTime==2);
 CHECK(r.getPacketsLost()==2);
 msg.available=false;r.impl.mLastNetUpdate.elapsed=3;const auto resets=r.impl.mLastNetUpdate.resets;
 r.updateNetStats();CHECK(r.mAlive&&r.mPacketsReceived==5);
 CHECK(r.getPacketsLost()==2);
 CHECK(r.impl.mLastNetUpdate.resets==resets&&r.impl.mLastNetUpdate.elapsed==3);
 msg.available=true;s.state=LLTransportSnapshotState::Unavailable;r.updateNetStats();
 CHECK(r.mAlive&&r.impl.mLastNetUpdate.resets==resets);
 CHECK(r.getPacketsLost()==2);
 s.state=LLTransportSnapshotState::Absent;r.updateNetStats();
 CHECK(!r.mAlive&&r.impl.mLastNetUpdate.resets==resets+1);CHECK(r.getPacketsLost()==0);
 s.state=LLTransportSnapshotState::Present;s.incarnation=2;s.packetsIn=2;
 s.bytesIn=3;s.packetsOut=1;s.packetsLost=0;r.impl.mLastNetUpdate.elapsed=1;
 r.updateNetStats();CHECK(r.mAlive&&r.mPacketsReceived==7&&r.mBitsReceived.value()==104);
 CHECK(r.mLastPacketsIn==0&&r.mLastBitsIn.value()==0&&r.mLastPacketsOut==0);
 r.updateNetStats();CHECK(r.mPacketsReceived==7&&r.mBitsReceived.value()==104);
 s.lossQuality=LLTransportLossQuality::Partial;s.packetsLost=4;
 CHECK(r.getPacketsLost()==4); // legacy numeric getter is documented lower-bound
 gMessageSystem=nullptr;r.updateNetStats();CHECK(r.mAlive);CHECK(r.getPacketsLost()==4);
 std::puts("PASS actual region snapshot consumers / real LLUnits");
}
'''
    generated = out / 'region_stats.cpp'
    generated.write_text(support + methods + cases)
    receipt = {'qualification': 'actual two region methods; real LLUnits; fake message/table/clock/log endpoints',
               'inputs': {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in (source_path, header_path, generated)}, 'commands': []}
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    if not args.run:
        return
    executable = out / 'region_stats'
    commands = [
        ['/usr/bin/c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-O1', '-UNDEBUG',
         '-DLL_LINUX=1', '-DLL_PROFILER_CONFIGURATION=0', '-I' + str(root / 'indra/llcommon'),
         '-isystem', str(args.packages_include.resolve()),
         '-I' + str(root / 'indra/llmessage'), str(generated), '-o', str(executable)],
        [str(executable)]]
    for command in commands:
        proc = subprocess.run(command, cwd=out, capture_output=True, text=True, timeout=45)
        receipt['commands'].append({'argv': command, 'exit_code': proc.returncode,
                                    'stdout': proc.stdout, 'stderr': proc.stderr})
        (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
        if proc.returncode:
            print(proc.stdout, proc.stderr)
            raise SystemExit(proc.returncode)
    print('PASS; ' + str(out / 'receipt.json'))


if __name__ == '__main__':
    main()
