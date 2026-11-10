#!/usr/bin/env python3
"""Generate actual main-completion methods with a fake owner queue."""
from pathlib import Path
import hashlib,sys
root=Path(__file__).resolve().parents[3]; s=(root/'indra/llmessage/message.cpp').read_text()
a=s.index('LLMessageSystem::TransportCallbackToken LLMessageSystem::reserveTransportCallback(')
b=s.index('\nbool LLMessageSystem::getTransportBacklog(',a);body=s[a:b]
da=s.index('bool LLMessageSystem::detachDrainedTransportOwnerOnMain() noexcept')
db=s.index('\nbool LLMessageSystem::clearDeliveredTransportInboundOnMain()',da);body+='\n'+s[da:db]
prefix=r'''
#include "lltransportmainfacade.h"
#include "lltransportcallbackregistry.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
bool messagingTeardownPendingOnMain() noexcept { return true; }
struct Graph {
 bool finalWarningSnapshotOnMain(uint32_t,LLTransportControlWarningStats&,bool&) const noexcept { return false; }
 bool acknowledgeFinalWarningsOnMain() noexcept { return false; }
 bool detachDrainedRuntimeOnMain() noexcept { return false; }
};
struct LLMessageSystem {
 using TransportCallbackRegistry=LLTransportCallbackRegistry<4096>;
 using TransportCallbackToken=TransportCallbackRegistry::Token;
 std::unique_ptr<LLTransportMainFacade> mTransportMainFacade;
 std::shared_ptr<Graph> mTransportOwnerGraph;
 bool mTransportStopRequested=false;
 bool mTransportOwnerJoined=true,mTransportDispatching=false,mTransportInboundHeld=false,mTransportInboundFatal=false;
 uint32_t mTransportCallbacksExecuting=0;
 LLTransportCallbackRegistry<4096> mTransportCallbacks;
 std::shared_ptr<uint8_t> mTransportCallbackLifetime;
 TransportCallbackToken reserveTransportCallback(TransportCallbackRegistry::Callback,void**,TransportCallbackRegistry::Context) noexcept;
 bool acceptTransportCallback(TransportCallbackToken) noexcept;
 bool cancelUnacceptedTransportCallback(TransportCallbackToken) noexcept;
 uint32_t pumpTransportCompletions(uint32_t);
 bool hasMessageReaderUsersOnMain() const noexcept { return false; }
 bool consumeTransportControlWarningOnMain(uint32_t,const LLTransportControlWarningStats&,bool&) { return false; }
 bool detachDrainedTransportOwnerOnMain() noexcept;
};
static void check(bool v,int line) { if(!v) { std::fprintf(stderr,"failed %d\n",line); std::abort(); } }
#define CHECK(x) check(bool(x),__LINE__)
'''
cases=r'''
struct Facade final : LLTransportMainFacade {
 LLTransportSendCompletion event{}; bool ready=false; int releases=0, failures=0;
 bool getCircuitSnapshot(LLTransportEndpoint,LLTransportCircuitSnapshot&) const noexcept override { return false; }
 LLTransportCommandOffer submitCircuitCommand(const LLTransportCircuitCommand&) noexcept override { return LLTransportCommandOffer::Unavailable; }
 bool popCircuitCompletion(LLTransportCircuitCompletion&) noexcept override { return false; }
 bool peekReliableCompletion(LLTransportSendCompletion& out) noexcept override { if(!ready) return false; out=event; return true; }
 bool releaseReliableCompletion() noexcept override { ++releases; ready=false; return true; }
 void failCompletion() noexcept override { ++failures; }
};
static unsigned calls=0; static std::unique_ptr<LLMessageSystem> owner;
static LLMessageSystem* callbackOwner=nullptr;
static Facade* callbackFacade=nullptr;
static LLMessageSystem::TransportCallbackRegistry::Context callbackContext{2,3,4,5};
static void done(void**,int32_t status) { CHECK(status==-23017); CHECK(callbackOwner && callbackOwner->mTransportCallbacksExecuting==1); ++calls; }
static void destroyOwner(void**,int32_t) { ++calls; owner.reset(); }
static void inner(void**,int32_t) { CHECK(callbackOwner->mTransportCallbacksExecuting==2); CHECK(!callbackOwner->detachDrainedTransportOwnerOnMain()); }
static void outer(void**,int32_t) {
 CHECK(callbackOwner->mTransportCallbacksExecuting==1); CHECK(!callbackOwner->detachDrainedTransportOwnerOnMain());
 auto t=callbackOwner->reserveTransportCallback(inner,nullptr,callbackContext); CHECK(t && callbackOwner->acceptTransportCallback(t));
 callbackFacade->event.operationId=t; callbackFacade->event.endpoint={4,5}; callbackFacade->event.loginGeneration=2; callbackFacade->event.circuitIncarnation=3; callbackFacade->ready=true;
 CHECK(callbackOwner->pumpTransportCompletions(1)==1); CHECK(callbackOwner->mTransportCallbacksExecuting==1);
}
static void throwsCallback(void**,int32_t) { CHECK(callbackOwner->mTransportCallbacksExecuting==1); throw 17; }
int main() {
 auto s=std::make_unique<LLMessageSystem>(); callbackOwner=s.get(); auto f=std::make_unique<Facade>(); auto* p=f.get(); callbackFacade=p; s->mTransportMainFacade=std::move(f);
 LLMessageSystem::TransportCallbackRegistry::Context ctx{2,3,4,5};
 auto token=s->reserveTransportCallback(done,nullptr,ctx); CHECK(token && s->acceptTransportCallback(token));
 p->event.endpoint={4,5}; p->event.operationId=token; p->event.loginGeneration=2; p->event.circuitIncarnation=3; p->event.originalStatus=-23017; p->ready=true;
 CHECK(s->pumpTransportCompletions(10)==1 && calls==1 && p->releases==1 && s->mTransportCallbacksExecuting==0);
 CHECK(s->pumpTransportCompletions(10)==0 && calls==1);
 token=s->reserveTransportCallback(outer,nullptr,ctx); CHECK(token && s->acceptTransportCallback(token));
 p->event.operationId=token; p->ready=true; CHECK(s->pumpTransportCompletions(1)==1 && s->mTransportCallbacksExecuting==0);
 token=s->reserveTransportCallback(throwsCallback,nullptr,ctx); CHECK(token && s->acceptTransportCallback(token));
 p->event.operationId=token; p->ready=true; bool threw=false; try { (void)s->pumpTransportCompletions(1); } catch (int value) { threw=(value==17); }
 CHECK(threw && s->mTransportCallbacksExecuting==0);
 token=s->reserveTransportCallback(done,nullptr,ctx); CHECK(token && s->acceptTransportCallback(token));
 p->event.operationId=token; p->event.circuitIncarnation=9; p->ready=true;
 CHECK(s->pumpTransportCompletions(10)==0 && p->failures==1 && p->ready && calls==1);
 owner=std::make_unique<LLMessageSystem>(); auto other=std::make_unique<Facade>(); auto* q=other.get(); owner->mTransportMainFacade=std::move(other);
 auto t=owner->reserveTransportCallback(destroyOwner,nullptr,ctx); CHECK(t && owner->acceptTransportCallback(t));
 q->event.endpoint={4,5}; q->event.operationId=t; q->event.loginGeneration=2; q->event.circuitIncarnation=3; q->ready=true;
 auto* raw=owner.get(); CHECK(raw->pumpTransportCompletions(10)==1 && !owner && calls==2);
}
'''
out=Path(sys.argv[1]);out.write_text(prefix+body+'\n'+cases)
print('body_sha256',hashlib.sha256(body.encode()).hexdigest());print('fixture_sha256',hashlib.sha256(out.read_bytes()).hexdigest())
