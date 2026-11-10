#!/usr/bin/env python3
"""Extract actual snapshot bridge; fake legacy table/clock boundary, no sockets."""
from pathlib import Path
import hashlib, sys
root = Path(__file__).resolve().parents[3]
source = (root / 'indra/llmessage/message.cpp').read_text()
a = source.index('bool LLMessageSystem::getCircuitSnapshot(')
b = source.index('\nLLTransportCommandOffer LLMessageSystem::submitTransportCircuitCommand', a)
body = source[a:b]
prefix = r'''
#include "lltransportmainfacade.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
struct Unit { double v; double value() const { return v; } };
inline Unit operator-(Unit a, Unit b) { return {a.v-b.v}; }
struct Num { int v; int value() const { return v; } };
struct LLHost { uint32_t a,p; uint32_t getAddress() const { return a; } uint32_t getPort() const { return p; } };
struct LLCircuitData {
    uint32_t packets=2; float age=12; Unit last{95};
    float getAgeInSeconds() const { return age; } uint32_t getPacketsIn() const { return packets; }
    Num getBytesIn() const { return {123}; } uint32_t getPacketsOut() const { return 4; }
    uint32_t getPacketsLost() const { return 5; } Num getPingDelay() const { return {37}; }
    bool isAlive() const { return true; } bool isBlocked() const { return false; } bool getTrusted() const { return true; }
    Unit getLastPacketInTime() const { return last; }
};
struct CircuitTable { const LLCircuitData* value=nullptr; const LLCircuitData* findCircuit(const LLHost&) const { return value; } };
struct LLMessageSystem {
    std::unique_ptr<LLTransportMainFacade> mTransportMainFacade; CircuitTable mCircuitInfo;
    static Unit getMessageTimeSeconds() { return {100}; }
    bool getCircuitSnapshot(const LLHost&, LLTransportCircuitSnapshot&) const noexcept;
};
static void check(bool ok, int line) { if (!ok) { std::fprintf(stderr,"fail %d\n",line); std::abort(); } }
#define CHECK(x) check(bool(x),__LINE__)
'''
cases = r'''
struct Unavailable final : LLTransportMainFacade {
 bool getCircuitSnapshot(LLTransportEndpoint, LLTransportCircuitSnapshot&) const noexcept override { return false; }
 LLTransportCommandOffer submitCircuitCommand(const LLTransportCircuitCommand&) noexcept override { return LLTransportCommandOffer::Unavailable; }
 bool popCircuitCompletion(LLTransportCircuitCompletion&) noexcept override { return false; }
};
int main() {
 LLMessageSystem sys; LLHost host{42,9000}; LLTransportCircuitSnapshot out;
 CHECK(sys.getCircuitSnapshot(host,out) && out.state == LLTransportSnapshotState::Absent);
 LLCircuitData c; sys.mCircuitInfo.value=&c;
 CHECK(sys.getCircuitSnapshot(host,out) && out.state == LLTransportSnapshotState::Present);
 CHECK(out.incarnation==0 && out.publicationSequence==0 && out.lossQuality==LLTransportLossQuality::Complete);
 CHECK(out.packetsIn==2 && out.bytesIn==123 && out.packetsOut==4 && out.packetsLost==5 && out.pingMilliseconds==37);
 CHECK(out.alive && !out.blocked && out.trusted && out.hasLastPacket);
 CHECK(out.ownerMonotonicUsec-out.lastPacketMonotonicUsec==5000000);
 c.packets=0; CHECK(sys.getCircuitSnapshot(host,out) && !out.hasLastPacket);
 CHECK(out.ownerMonotonicUsec-out.birthMonotonicUsec==12000000);
 out.packetsIn=777; sys.mTransportMainFacade=std::make_unique<Unavailable>();
 CHECK(!sys.getCircuitSnapshot(host,out) && out.packetsIn==777);
 CHECK(!sys.getCircuitSnapshot(LLHost{42,65536},out) && out.packetsIn==777);
}
'''
out = Path(sys.argv[1]); out.write_text(prefix + body + '\n' + cases)
print('bridge_sha256', hashlib.sha256(body.encode()).hexdigest())
print('fixture_sha256', hashlib.sha256(out.read_bytes()).hexdigest())
