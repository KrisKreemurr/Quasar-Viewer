#!/usr/bin/env python3
"""Stage a differential fixture with the exact legacy ping-retry assignment.

Only the old unit-value/clock and unrelated packet boundaries are faked;
the selected new circuit-state implementation is compiled separately.
No compiler or fixture is run here.
"""
from pathlib import Path
import hashlib
import sys

message=Path(__file__).resolve().parents[1]
old=(message/'llcircuit.cpp').read_text()
start=old.index('packetp->mExpirationTime = now + llmax(LL_MINIMUM_RELIABLE_TIMEOUT_SECONDS,')
end=old.index(';',start)+1
statement=old[start:end]
prefix=r'''
#include "lltransportcircuitstate.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
// Narrow unit-value boundary: the production expression receives the actual
// selected state's effective ping in milliseconds, represented as float.
struct Milliseconds { float v; };
static Milliseconds operator*(float f,Milliseconds v) { return {f*v.v}; }
struct F32Seconds {
    float v;
    explicit F32Seconds(float x):v(x) {}
    explicit F32Seconds(Milliseconds x):v(x.v/1000.f) {}
};
static bool operator<(F32Seconds a,F32Seconds b) { return a.v<b.v; }
static double operator+(double a,F32Seconds b) { return a+b.v; }
template<class T> static T llmax(T a,T b) { return a<b ? b : a; }
static constexpr float LL_RELIABLE_TIMEOUT_FACTOR=5.f;
static const F32Seconds LL_MINIMUM_RELIABLE_TIMEOUT_SECONDS(1.f);
struct LegacyPacket { double mExpirationTime=0; };
static float selectedPing=0;
static Milliseconds getPingDelayAveraged() { return {selectedPing}; }
static double legacyDeadline(double now,float ping)
{
    selectedPing=ping; LegacyPacket packet; LegacyPacket* packetp=&packet;
'''
suffix=r'''
    return packet.mExpirationTime;
}
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"failed line %d\n",__LINE__); std::abort(); } } while(false)
int main()
{
    using State=LLTransportCircuitState;
    for (uint64_t replyTime : {100100ULL,1900100ULL})
    {
        State state; uint64_t charged=0;
        CHECK(state.init({1,1,1,1,1,1,8388608},charged));
        const LLTransportEndpoint endpoint{0x0100007f,1234};
        const auto circuit=state.enable(endpoint,7,true,100,{1000000,60000000,0.f,0.5f});
        CHECK(circuit.status==State::Status::Ok);
        CHECK(state.notePingReply(endpoint,circuit.incarnation,0,1000000,replyTime)==State::Status::Ok);
        CHECK(state.reserveReliableSlot(endpoint,circuit.incarnation,7,10)==State::Status::Ok);
        CHECK(state.acceptReliableSlot(10)==State::Status::Ok);
        uint32_t id=0; CHECK(state.allocateReliablePacketId(10,id)==State::Status::Ok);
        const uint8_t packet[]={0x40,0,0,0,1,0,42};
        CHECK(state.prepareReliable(10,id,packet,sizeof(packet),{2,1,17,false,true},2000000)==State::Status::Ok);
        auto action=state.advance(2000000,0); CHECK(action.kind==State::ActionKind::InitialSend);
        CHECK(state.commitAttempt(action.token,2000000,17,true,7)==State::Status::Ok);
        action=state.advance(2000002,0); CHECK(action.kind==State::ActionKind::Resend);
        float ping=0; CHECK(state.effectivePingMilliseconds(endpoint,circuit.incarnation,1000000,2000002,ping)==State::Status::Ok);
        const double oldDue=legacyDeadline(2.000002,ping)*1000000.0;
        int64_t interval=0; CHECK(state.selectedRetryInterval(action.token,2000002,interval)==State::Status::Ok);
        const uint64_t quantized=2000002+static_cast<uint64_t>(interval);
        CHECK(static_cast<double>(quantized)+0.00001>=oldDue && static_cast<double>(quantized)-oldDue<1.00001);
        CHECK(state.commitAttempt(action.token,2000002,interval,true,7)==State::Status::Ok);
        CHECK(state.advance(static_cast<uint64_t>(std::floor(oldDue))-1,0).kind==State::ActionKind::None);
        CHECK(state.advance(quantized,0).kind==State::ActionKind::None); // strict > deadline
        CHECK(state.advance(quantized+1,0).kind==State::ActionKind::Resend);
    }
}
'''
path=Path(sys.argv[1]).resolve()
root=message.parents[1].resolve()
if not path.is_relative_to(root): raise SystemExit('output must be inside this worktree')
path.parent.mkdir(parents=True,exist_ok=True)
content=prefix+'    '+statement+'\n'+suffix
path.write_text(content)
print('legacy_statement_sha256',hashlib.sha256(statement.encode()).hexdigest())
print('fixture_sha256',hashlib.sha256(content.encode()).hexdigest())
