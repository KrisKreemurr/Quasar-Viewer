#!/usr/bin/env python3
"""Extract legacy duplicate-list cleanup for a bounded U32 parity fixture."""
import hashlib
from pathlib import Path
import sys

message=Path(__file__).resolve().parents[1]
source=(message/'llcircuit.cpp').read_text()
start=source.index('void LLCircuitData::clearDuplicateList(')
opening=source.index('{',start); depth=0; end=None
for i in range(opening,len(source)):
    if source[i]=='{': depth+=1
    elif source[i]=='}':
        depth-=1
        if not depth: end=i+1; break
if end is None: raise SystemExit('unterminated legacy method')
body=source[start:end]
prefix=r'''
#include "lltransportcircuitstate.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <map>
using U32=uint32_t; using U64Microseconds=uint64_t; using F64Seconds=double; using TPACKETID=uint32_t;
constexpr double LL_DUPLICATE_SUPPRESSION_TIMEOUT=60000000.; // fake unit boundary; no wrapped entries in this case
struct Log { template<class T> Log& operator<<(const T&) { return *this; } };
#define LL_WARNS(...) Log()
#define LL_INFOS(...) Log()
#define LL_ENDL 0
struct LLMessageSystem { static U64Microseconds getMessageTimeUsecs() { return 103; } };
struct LLCircuitData {
    using packet_time_map=std::map<TPACKETID,U64Microseconds>;
    packet_time_map mRecentlyReceivedReliablePackets; U32 mHighestPacketID=5;
    void clearDuplicateList(TPACKETID);
};
'''
suffix=r'''
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"failed %d\n",__LINE__); std::abort(); } } while(false)
int main()
{
    using State=LLTransportCircuitState;
    for (uint32_t oldest : {UINT32_MAX,4u})
    {
        LLCircuitData old; old.mRecentlyReceivedReliablePackets[1]=101; old.mRecentlyReceivedReliablePackets[5]=103;
        old.clearDuplicateList(oldest);
        State state; uint64_t charged=0; CHECK(state.init({1,2,4,4,1,4,8388608},charged));
        LLTransportEndpoint endpoint{0x0100007f,1234};
        const auto enabled=state.enable(endpoint,7,true,100,{1000000,60000000,0.f,0.5f});
        CHECK(enabled.status==State::Status::Ok);
        for (uint32_t packet : {1u,5u})
        {
            const auto received=state.reserveReceive(endpoint,packet,false,true,103);
            CHECK(received.status==State::ReceiveStatus::New && state.commitReceive(received.reservation,1,103)==State::Status::Ok);
        }
        CHECK(state.forgetReceivedBefore(endpoint,enabled.incarnation,oldest,103)==State::Status::Ok);
        for (uint32_t packet : {1u,5u})
            CHECK((state.classifyResentDuplicate(endpoint,enabled.incarnation,7,packet,true)==State::DuplicateClass::Duplicate)
                  == (old.mRecentlyReceivedReliablePackets.count(packet)!=0));
    }
}
'''
output=Path(sys.argv[1]).resolve(); root=message.parents[1].resolve()
if not output.is_relative_to(root): raise SystemExit('output must be in this worktree')
output.parent.mkdir(parents=True,exist_ok=True)
content=prefix+body+'\n'+suffix; output.write_text(content)
print('legacy_method_sha256',hashlib.sha256(body.encode()).hexdigest())
print('fixture_sha256',hashlib.sha256(content.encode()).hexdigest())
