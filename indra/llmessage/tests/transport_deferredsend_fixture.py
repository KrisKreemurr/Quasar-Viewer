#!/usr/bin/env python3
"""Generate actual deferred-send template with fake socket/ring boundary."""
from pathlib import Path
import hashlib, sys
root=Path(__file__).resolve().parents[3]
source=(root/'indra/llmessage/lltransportdeferredsend.h').read_text()
a=source.index('template <std::size_t ControlCapacity, std::size_t DataCapacity>')
b=source.rindex('\n#endif')
body=source[a:b]
prefix=r'''
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#define NET_BUFFER_SIZE 8192
#define WSAECONNRESET 10054
using S32=int32_t;
struct LLTransportEndpoint { uint32_t addressNetworkOrder=0; uint16_t portHostOrder=0; };
struct LLHost { LLHost(uint32_t, uint16_t) {} };
enum class NetSendStatus { Sent, WouldBlock, Refused, Failed };
struct NetSendResult { NetSendStatus status; int osError; };
struct LLPacketRing {
 struct UDPProxySnapshot {};
 unsigned calls=0; bool failAlways=false; std::array<uint8_t,16> seen{}; std::array<bool,16> first{};
 NetSendResult sendPacketOnce(int,const char* p,S32 n,const LLHost&,const UDPProxySnapshot&,bool f) {
   if (calls>=seen.size() || n!=7) { std::abort(); }
   seen[calls]=uint8_t(p[6]); first[calls]=f; ++calls;
   if (failAlways || calls==1) {
#if LL_WINDOWS
     return {NetSendStatus::WouldBlock,10035};
#else
     return {NetSendStatus::WouldBlock,EAGAIN};
#endif
   }
   return {NetSendStatus::Sent,0};
 }
};
static void check(bool v,int n) { if (!v) { std::fprintf(stderr,"fail %d\n",n); std::abort(); } }
#define CHECK(x) check(bool(x),__LINE__)
'''
cases=r'''
int main() {
 using Queue=LLTransportDeferredSend<1,1>; Queue q; LLPacketRing ring; LLPacketRing::UDPProxySnapshot proxy;
 const LLTransportEndpoint ep{1,2}; uint8_t data[7]={0,0,0,0,0,0,42}, control[7]={0,0,0,0,0,0,9};
 CHECK(q.enqueue(Queue::Lane::Data,10,ep,data,7)==Queue::Offer::Accepted);
 CHECK(q.enqueue(Queue::Lane::Control,20,ep,control,7)==Queue::Offer::Accepted);
 CHECK(q.enqueue(Queue::Lane::Control,21,ep,control,7)==Queue::Offer::Full);
 control[6]=77; data[6]=88;
 auto a=q.attemptOne(ring,1,proxy); CHECK(a.status==Queue::Status::Deferred && a.token==20 && a.wireBytes==7);
 auto b=q.attemptOne(ring,1,proxy); CHECK(b.status==Queue::Status::Sent && b.token==10);
 auto c=q.attemptOne(ring,1,proxy); CHECK(c.status==Queue::Status::Sent && c.token==20);
 CHECK(ring.seen[0]==9 && ring.seen[1]==42 && ring.seen[2]==9);
 CHECK(ring.first[0] && ring.first[1] && !ring.first[2]);
 CHECK(q.attemptOne(ring,1,proxy).status==Queue::Status::Empty);
 CHECK(q.enqueue(Queue::Lane::Data,30,ep,data,7)==Queue::Offer::Accepted);
 CHECK(q.cancelOne().status==Queue::Status::Cancelled);
 CHECK(q.cancelOne().status==Queue::Status::Empty);
}
'''
out=Path(sys.argv[1]);out.write_text(prefix+body+'\n'+cases)
print('source',hashlib.sha256(source.encode()).hexdigest())
print('fixture',hashlib.sha256(out.read_bytes()).hexdigest())
