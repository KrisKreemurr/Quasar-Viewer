#!/usr/bin/env python3
"""Extract the production ring sample with fake main endpoints; no socket/GPU."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[3]
source = (root / 'indra/newview/llworld.cpp').read_text()
start = source.index('        LLTransportCircuitSnapshot circuit;', source.index('void LLWorld::updateRegionRing()'))
end = source.index('\n    }\n    logRegionRingState', start)
body = source[start:end]
# Main must never remove a region on an age-only owner observation. Retain
# the existing ordered timeout/disable paths until a conditional close exists.
assert 'if (gMessageSystem->transportOwnerActive()) return;' in source[source.index('void LLWorld::updateRegionRing()'):]
assert 'mCircuitInfo' not in source
prefix = r'''
#include "lltransportcircuittypes.h"
#include <chrono>
#include <cstdlib>
#include <cstdio>
using F64 = double;
struct Region { int getHost() const { return 7; } };
struct Info { bool mCircuitKnown=false; double mSincePacket=-1; };
struct Messages {
    bool available=true; LLTransportCircuitSnapshot value;
    bool getCircuitSnapshot(int host, LLTransportCircuitSnapshot& out) {
        if (host!=7) std::abort();
        if (!available) return false;
        out=value; return true;
    }
} messages;
Messages* gMessageSystem=&messages;
void sample(Region* regionp, Info& info) {
'''
cases = r'''
}
static void check(bool value, int line) { if (!value) { std::fprintf(stderr,"fail %d\n",line); std::abort(); } }
#define CHECK(x) check(bool(x),__LINE__)
int main() {
    Region region;
    const auto now=std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    messages.value.state=LLTransportSnapshotState::Present;
    messages.value.ownerMonotonicUsec=now;
    messages.value.birthMonotonicUsec=now-20000000;
    messages.value.hasLastPacket=true;
    messages.value.lastPacketMonotonicUsec=now-1000000;
    Info receiving; sample(&region,receiving);
    CHECK(receiving.mCircuitKnown && receiving.mSincePacket>=1. && receiving.mSincePacket<1.2);
    CHECK(receiving.mSincePacket<5.); // receiving root must not report a global stall
    messages.value.hasLastPacket=false;
    Info newborn; sample(&region,newborn);
    CHECK(newborn.mCircuitKnown && newborn.mSincePacket>=20. && newborn.mSincePacket<20.2);
    messages.value.birthMonotonicUsec=now-21000000;
    Info later; sample(&region,later);
    CHECK(later.mSincePacket>newborn.mSincePacket); // birth age advances without packets
    messages.value.state=LLTransportSnapshotState::Absent;
    Info absent; sample(&region,absent); CHECK(absent.mCircuitKnown && absent.mSincePacket<0.);
    messages.available=false;
    Info unavailable; sample(&region,unavailable); CHECK(!unavailable.mCircuitKnown);
    messages.available=true; messages.value.state=LLTransportSnapshotState::Unavailable;
    Info unknown; sample(&region,unknown); CHECK(!unknown.mCircuitKnown);
    messages.value.state=LLTransportSnapshotState::Present;
    messages.value.ownerMonotonicUsec=now+100000000;
    Info future; sample(&region,future); CHECK(!future.mCircuitKnown);
    messages.value.ownerMonotonicUsec=now; messages.value.birthMonotonicUsec=now+1;
    Info corrupt; sample(&region,corrupt); CHECK(!corrupt.mCircuitKnown);
    messages.value.incarnation=2; messages.value.birthMonotonicUsec=now;
    Info replaced; sample(&region,replaced);
    CHECK(replaced.mCircuitKnown && replaced.mSincePacket<.2);
}
'''
Path(sys.argv[1]).write_text(prefix + body + cases)
