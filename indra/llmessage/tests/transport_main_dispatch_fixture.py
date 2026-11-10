#!/usr/bin/env python3
"""Compile production main-dispatch bodies against a fake owner/reader boundary."""
from pathlib import Path
import hashlib, sys
root=Path(__file__).resolve().parents[3]
s=(root/'indra/llmessage/message.cpp').read_text()
a=s.index('void LLMessageSystem::clearReceiveState()'); b=s.index('\n\nbool LLMessageSystem::poll(',a)
c=s.index('bool LLMessageSystem::checkTransportMessages('); d=s.index('\n// Returns true if a valid, on-circuit message',c)
def method(name):
    start=s.index(name); opening=s.index('{',start); depth=0
    for i in range(opening,len(s)):
        if s[i]=='{': depth+=1
        elif s[i]=='}':
            depth-=1
            if not depth: return s[start:i+1]
    raise ValueError(name)
bodies=s[a:b]+'\n'+s[c:d]+'\n'+method('bool LLMessageSystem::clearDeliveredTransportInboundOnMain(')+'\n'+method('LLMessageSystem::ShutdownInboundStep LLMessageSystem::dispatchTransportInboundForShutdownOnMain(')
prefix=r'''
#include "lltransportmainfacade.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>
using S32=int32_t;
struct F64Seconds { double value=0; explicit F64Seconds(double v=0):value(v){} };
constexpr S32 MAX_BUFFER_SIZE=8192, MX_RAN_OFF_END_OF_PACKET=1;
struct LLHost { uint32_t a=0; uint16_t p=0; LLHost()=default; LLHost(uint32_t x,uint16_t y):a(x),p(y){} void invalidate() { a=0; p=0; } };
struct LLMessageSystem;
struct LockMessageChecker { explicit LockMessageChecker(LLMessageSystem* = nullptr) {} };
enum class LLTransportDispatchResult { Prepared, Dispatched, Invalid, Retry };
static std::vector<int> trace;
static bool teardownPending=false;
static bool messagingTeardownPendingOnMain() noexcept { return teardownPending; }
struct LLMessageTemplate { uint32_t mSessionReceiveCount=0, mSessionReceiveBytes=0; };
struct Reader {
 LLMessageTemplate temp; bool retry=false; bool prepared=false;
 void clearMessage() { prepared=false; trace.push_back(1); }
 bool selectOwnedTemplate(uint32_t,S32) { return true; }
 LLTransportDispatchResult prepareOwnedDecoded(const LLTransportDecodedView&) {
  if (retry) { retry=false; return LLTransportDispatchResult::Retry; }
  prepared=true; return LLTransportDispatchResult::Prepared;
 }
 LLMessageTemplate* getCurrentTemplate() { return &temp; }
 bool dispatchPreparedOwned(const LLHost&) { if (!prepared) std::abort(); trace.push_back(3); return true; }
};
struct LLMessageSystem {
 std::unique_ptr<LLTransportMainFacade> mTransportMainFacade;
 bool mTransportInboundHeld=false,mTransportInboundDispatched=false,mTransportInboundFatal=false,mTransportDispatching=false;
 bool mTransportOwnerJoined=true, readerUsers=false;
 uint32_t mTransportDiagnosticsDelivered=0;
 LLHost mLastSender,mLastReceivingIF; uint32_t mCurrentRecvPacketID=0; S32 mIncomingCompressedSize=0,mTrueReceiveSize=0;
 bool mLastMessageFromTrustedMessageService=false; Reader reader; Reader* mMessageReader=&reader; Reader* mTemplateMessageReader=&reader;
 uint32_t mPacketsIn=0,mReliablePacketsIn=0; uint64_t mBytesIn=0;
 F64Seconds mCurrentMessageTime, mMessageCountTime; uint32_t mNumMessageCounts=0;
 void clearReceiveState(); bool checkTransportMessages(LockMessageChecker&);
 bool hasMessageReaderUsersOnMain() const noexcept { return readerUsers; }
 bool clearDeliveredTransportInboundOnMain();
 enum class ShutdownInboundStep : uint8_t { Progress, Empty, Barrier, Retry, Fatal, Unavailable };
 ShutdownInboundStep dispatchTransportInboundForShutdownOnMain(uint64_t);
 bool reenter=false, stopOnDiagnostic=false;
 bool callExceptionFunc(int x) { if (x!=MX_RAN_OFF_END_OF_PACKET) std::abort(); trace.push_back(2);
  if (stopOnDiagnostic) teardownPending=true;
  if (reenter) { LockMessageChecker nested; if (checkTransportMessages(nested)) std::abort(); clearReceiveState(); }
  return true; }
};
static void check(bool ok,int line) { if (!ok) { std::fprintf(stderr,"failed %d\n",line); std::abort(); } }
#define CHECK(v) check(bool(v),__LINE__)
'''
cases=r'''
struct Facade final : LLTransportMainFacade {
 LLTransportIngressEvent event{}; bool pending=true, match=true, unavailable=false, emptyProof=true; int releases=0, failures=0;
 bool getCircuitSnapshot(LLTransportEndpoint,LLTransportCircuitSnapshot&) const noexcept override { return false; }
 LLTransportCommandOffer submitCircuitCommand(const LLTransportCircuitCommand&) noexcept override { return LLTransportCommandOffer::Unavailable; }
 bool popCircuitCompletion(LLTransportCircuitCompletion&) noexcept override { return false; }
 bool peekInbound(LLTransportIngressEvent& out,LLTransportDecodedView& view) noexcept override { if (unavailable || !pending) return false; out=event; view={}; return true; }
 bool inboundEmptyAfterJoinOnMain() const noexcept override { return emptyProof && !pending; }
 bool releaseInbound() noexcept override { trace.push_back(4); ++releases; pending=false; return true; }
 bool schemaMatches(uint32_t,uint64_t) const noexcept override { return match; }
 bool legacyMessageTimeUsec(uint64_t& out) const noexcept override { out=1700000000000000ULL; return true; }
 void failInbound() noexcept override { ++failures; }
};
int main() {
 LLMessageSystem sys; auto owner=std::make_unique<Facade>(); auto* f=owner.get();
 f->event.schemaGeneration=3; f->event.messageNumber=1; f->event.expandedBytes=7; f->event.wireBytes=7;
 f->event.truncations=2; f->event.reliable=true; sys.mTransportMainFacade=std::move(owner);
 LockMessageChecker lock; sys.reader.retry=true; sys.reenter=true;
 CHECK(!sys.checkTransportMessages(lock)); CHECK(f->releases==0 && sys.mTransportInboundHeld && !sys.mTransportInboundDispatched);
 CHECK(sys.checkTransportMessages(lock)); CHECK(f->releases==0 && sys.mPacketsIn==1 && sys.mReliablePacketsIn==1);
 CHECK(trace.size()==5 && trace[0]==1 && trace[1]==1 && trace[2]==2 && trace[3]==2 && trace[4]==3);
 CHECK(!sys.checkTransportMessages(lock)); CHECK(f->releases==1 && !sys.mTransportInboundHeld);
 CHECK(trace.size()==7 && trace[5]==1 && trace[6]==4 && f->failures==0);
 LLMessageSystem bad; auto badOwner=std::make_unique<Facade>(); auto* badPtr=badOwner.get();
 badPtr->event=f->event; badPtr->match=false; bad.mTransportMainFacade=std::move(badOwner);
 CHECK(!bad.checkTransportMessages(lock) && bad.mTransportInboundFatal);
 CHECK(badPtr->failures==1 && badPtr->releases==0 && bad.mTransportInboundHeld);
 // A diagnostic may request shutdown. The acknowledged event remains held;
 // controlled retries respect its barrier and do not replay diagnostics.
 trace.clear(); LLMessageSystem stop; auto stopOwner=std::make_unique<Facade>(); auto* stopPtr=stopOwner.get();
 stopPtr->event=f->event; stopPtr->event.appSequence=5; stop.mTransportMainFacade=std::move(stopOwner); stop.stopOnDiagnostic=true;
 CHECK(!stop.checkTransportMessages(lock) && teardownPending && stop.mTransportDiagnosticsDelivered==1);
 CHECK(stopPtr->releases==0 && stop.mTransportInboundHeld && !stop.mTransportInboundDispatched);
 using Step=LLMessageSystem::ShutdownInboundStep;
 stopPtr->unavailable=true;
 CHECK(stop.dispatchTransportInboundForShutdownOnMain(5)==Step::Unavailable && !stop.mTransportInboundFatal);
 stopPtr->unavailable=false;
 CHECK(stop.dispatchTransportInboundForShutdownOnMain(4)==Step::Barrier);
 CHECK(stop.dispatchTransportInboundForShutdownOnMain(5)==Step::Progress && stop.mTransportDiagnosticsDelivered==2);
 CHECK(stop.dispatchTransportInboundForShutdownOnMain(5)==Step::Progress && !stop.mTransportInboundHeld);
 CHECK(stopPtr->releases==1 && stop.mPacketsIn==1 && stop.reader.temp.mSessionReceiveCount==1);
 unsigned diagnostics=0,handlers=0; for (int item:trace) { diagnostics+=item==2; handlers+=item==3; }
 CHECK(diagnostics==2 && handlers==1);
 CHECK(stopPtr->releases==1 && stop.mTransportDiagnosticsDelivered==0);
 stopPtr->emptyProof=false;
 CHECK(stop.dispatchTransportInboundForShutdownOnMain(5)==Step::Unavailable);
 stopPtr->emptyProof=true;
 CHECK(stop.dispatchTransportInboundForShutdownOnMain(5)==Step::Empty);
}
'''
out=Path(sys.argv[1]);out.write_text(prefix+bodies+'\n'+cases)
print('body_sha256',hashlib.sha256(bodies.encode()).hexdigest())
print('fixture_sha256',hashlib.sha256(out.read_bytes()).hexdigest())
