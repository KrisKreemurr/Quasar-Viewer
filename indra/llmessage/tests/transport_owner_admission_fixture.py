#!/usr/bin/env python3
"""Actual owner-admission body and command queue, fake command value boundary."""
import hashlib,pathlib,sys
m=pathlib.Path(__file__).resolve().parents[1];root=m.parents[1]
s=(m/'lltransportowneradmission.h').read_text();body=s[s.index('class LLTransportOwnerAdmission'):s.rindex('#endif')]
prefix=r'''
#include "lltransportcommandqueue.h"
#include "lltransportenvelope.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
struct LLTransportEndpoint { uint32_t addressNetworkOrder=0; uint16_t portHostOrder=0; };
enum class LLTransportCircuitCommandKind:uint8_t { Enable,Disable,DisableIfUnchanged };
struct LLTransportCircuitCommand { LLTransportCircuitCommandKind kind=LLTransportCircuitCommandKind::Enable; LLTransportEndpoint endpoint{}; uint64_t operationId=0,loginGeneration=0,targetIncarnation=0; };
struct LLTransportCircuitState { static constexpr uint32_t MaxPacketBytes=8192; struct TimeoutEvent { LLTransportEndpoint endpoint{}; uint64_t loginGeneration=0,incarnation=0,eventId=0,lastPublishedAppSequence=0; }; };
enum class LLTransportEncoding:uint8_t {Unencoded,ZeroCoded};
enum class LLTransportCommandOffer:uint8_t {Accepted,Full,Invalid,Unavailable};
enum class LLTransportCircuitCommandStatus:uint8_t { Applied,Cancelled };
struct LLTransportCircuitCompletion { LLTransportEndpoint endpoint{}; uint64_t operationId=0,loginGeneration=0,circuitIncarnation=0,lastPublishedAppSequence=0; LLTransportCircuitCommandStatus status=LLTransportCircuitCommandStatus::Cancelled; uint64_t timeoutEventId=0; };
struct LLTransportSendCompletion { LLTransportEndpoint endpoint{}; uint64_t operationId=0,loginGeneration=0,circuitIncarnation=0; int32_t originalStatus=0; };
struct LLTransportUnreliableCompletion { LLTransportEndpoint endpoint{}; uint64_t operationId=0,loginGeneration=0,circuitIncarnation=0; };
struct Schema { bool isFrozen() const { return true; } uint32_t messageNumber() const { return 1; } uint64_t generation() const { return 5; } };
struct LLTransportSchemaEntry { uint32_t messageNumber=0; const Schema* schema=nullptr; };
struct LLTransportOwnerCommand {
 enum class Kind:uint8_t {Enable,Disable,DisableIfUnchanged,FinishWatchdogTimeout,Send,SetAckCollectTime,Stop};
 Kind kind=Kind::Enable; LLTransportCircuitCommand circuit{}; LLTransportCircuitState::TimeoutEvent timeout{};
 uint64_t sendToken=0,schemaGeneration=0,configOperationId=0,ackCollectUsec=0; uint32_t byteCount=0; bool reliable=false; LLTransportEncoding encoding=LLTransportEncoding::Unencoded; std::array<uint8_t,8192> bytes{};
};
struct LLTransportOwnerChannels { LLTransportCommandQueue<LLTransportOwnerCommand,256> commands; };
'''
suffix=r'''
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"failed %d\n",__LINE__); std::abort(); } } while(false)
int main() {
 using Offer=LLTransportCommandOffer; using Kind=LLTransportOwnerCommand::Kind;
 auto channelsPtr=std::make_unique<LLTransportOwnerChannels>(); auto& channels=*channelsPtr;
 Schema schema; const LLTransportSchemaEntry entries[]={{1,&schema}}; LLTransportOwnerAdmission admission(channels,entries,1);
 LLTransportOwnerCommand cmd; cmd.kind=Kind::Send; cmd.circuit.endpoint={1,2}; cmd.circuit.loginGeneration=3; cmd.circuit.targetIncarnation=4; cmd.schemaGeneration=5; cmd.byteCount=7; cmd.reliable=true;
 cmd.bytes[6]=1;
 const LLTransportSchemaEntry duplicateEntries[]={{1,&schema},{1,&schema}}; LLTransportOwnerAdmission badSchema(channels,duplicateEntries,2);
 cmd.sendToken=998; CHECK(channels.commands.beginOfferOnMain()); CHECK(badSchema.offerOnMain(cmd)==Offer::Unavailable && badSchema.emptyOnMain()); channels.commands.endOfferOnMain();
 cmd.sendToken=0;
 CHECK(admission.offerOnMain(cmd)==Offer::Unavailable);
 CHECK(channels.commands.beginOfferOnMain()); CHECK(admission.offerOnMain(cmd)==Offer::Invalid); channels.commands.endOfferOnMain();
 cmd.sendToken=999; cmd.schemaGeneration=6;
 CHECK(channels.commands.beginOfferOnMain()); CHECK(admission.offerOnMain(cmd)==Offer::Invalid && admission.emptyOnMain()); channels.commands.endOfferOnMain();
 cmd.schemaGeneration=5; cmd.bytes[6]=0xff;
 CHECK(channels.commands.beginOfferOnMain()); CHECK(admission.offerOnMain(cmd)==Offer::Invalid && admission.emptyOnMain()); channels.commands.endOfferOnMain();
 cmd.bytes[6]=1;
 for (uint64_t i=1;i<=256;++i) {
  CHECK(channels.commands.beginOfferOnMain()); cmd.sendToken=i;
  CHECK(admission.offerOnMain(cmd)==Offer::Accepted); channels.commands.endOfferOnMain();
  auto* held=channels.commands.peekConsumer(); CHECK(held && held->sendToken==i); CHECK(channels.commands.releaseConsumer());
 }
 CHECK(admission.reliableOutstandingOnMain()==256);
 CHECK(channels.commands.beginOfferOnMain()); cmd.sendToken=257;
 CHECK(admission.offerOnMain(cmd)==Offer::Full); channels.commands.endOfferOnMain();
 LLTransportSendCompletion terminal{{1,2},1,3,4,0};
 auto wrong=terminal; wrong.circuitIncarnation=9;
 CHECK(!admission.matchesReliableOnMain(wrong) && !admission.releaseReliableCreditOnMain(wrong));
 CHECK(admission.matchesReliableOnMain(terminal) && admission.releaseReliableCreditOnMain(terminal));
 CHECK(channels.commands.beginOfferOnMain()); CHECK(admission.offerOnMain(cmd)==Offer::Accepted); channels.commands.endOfferOnMain();
 CHECK(channels.commands.peekConsumer()->sendToken==257); CHECK(channels.commands.releaseConsumer());
 // Other terminal lanes remain independent even while reliable credit is full.
 cmd.reliable=false; cmd.sendToken=300; CHECK(channels.commands.beginOfferOnMain());
 CHECK(admission.offerOnMain(cmd)==Offer::Accepted); channels.commands.endOfferOnMain(); CHECK(channels.commands.releaseConsumer()==false);
 CHECK(channels.commands.peekConsumer()->sendToken==300); CHECK(channels.commands.releaseConsumer()); CHECK(admission.unreliableOutstandingOnMain()==1);
 LLTransportUnreliableCompletion unrel{{1,2},300,3,4};
 CHECK(admission.matchesUnreliableOnMain(unrel) && admission.releaseUnreliableCreditOnMain(unrel) && !admission.releaseUnreliableCreditOnMain(unrel));
 LLTransportOwnerCommand finish; finish.kind=Kind::FinishWatchdogTimeout; finish.circuit.operationId=500;
 finish.timeout.endpoint={1,2}; finish.timeout.loginGeneration=3; finish.timeout.incarnation=4; finish.timeout.eventId=6; finish.timeout.lastPublishedAppSequence=7;
 CHECK(channels.commands.beginOfferOnMain()); CHECK(admission.offerOnMain(finish)==Offer::Accepted); channels.commands.endOfferOnMain();
 CHECK(channels.commands.peekConsumer() && channels.commands.releaseConsumer());
 LLTransportCircuitCompletion timeout; timeout.endpoint={1,2}; timeout.operationId=500; timeout.loginGeneration=3; timeout.circuitIncarnation=4; timeout.timeoutEventId=9; timeout.lastPublishedAppSequence=7;
 CHECK(!admission.matchesCircuitOnMain(timeout) && !admission.releaseCircuitCreditOnMain(timeout));
 timeout.timeoutEventId=6; CHECK(admission.matchesCircuitOnMain(timeout) && admission.releaseCircuitCreditOnMain(timeout));
 LLTransportOwnerCommand config; config.kind=Kind::SetAckCollectTime; config.configOperationId=501; config.ackCollectUsec=100000;
 config.circuit.endpoint={8,9}; config.circuit.loginGeneration=10; config.circuit.targetIncarnation=11;
 CHECK(channels.commands.beginOfferOnMain()); CHECK(admission.offerOnMain(config)==Offer::Invalid); channels.commands.endOfferOnMain();
 config.circuit={};
 CHECK(channels.commands.beginOfferOnMain()); CHECK(admission.offerOnMain(config)==Offer::Accepted); channels.commands.endOfferOnMain();
 CHECK(channels.commands.peekConsumer() && channels.commands.releaseConsumer());
 LLTransportCircuitCompletion configTerminal; configTerminal.operationId=501; configTerminal.lastPublishedAppSequence=99;
 CHECK(admission.matchesCircuitOnMain(configTerminal) && admission.releaseCircuitCreditOnMain(configTerminal));
 // Pending enable masks a committed absence from accepted publication until
 // the identity-matched completion is consumed; unrelated endpoints are free.
 LLTransportOwnerCommand enable; enable.kind=Kind::Enable; enable.circuit.kind=LLTransportCircuitCommandKind::Enable;
 enable.circuit.endpoint={7,8}; enable.circuit.loginGeneration=3; enable.circuit.operationId=502;
 CHECK(!admission.hasPendingEnableOnMain({7,8}));
 CHECK(channels.commands.beginOfferOnMain()); CHECK(admission.offerOnMain(enable)==Offer::Accepted); channels.commands.endOfferOnMain();
 CHECK(admission.hasPendingEnableOnMain({7,8}) && !admission.hasPendingEnableOnMain({7,9}));
 CHECK(channels.commands.peekConsumer() && channels.commands.releaseConsumer());
 LLTransportCircuitCompletion enabled; enabled.endpoint={7,8}; enabled.loginGeneration=3; enabled.operationId=502;
 enabled.status=LLTransportCircuitCommandStatus::Applied; enabled.circuitIncarnation=11;
 auto mismatch=enabled; mismatch.endpoint={7,9};
 CHECK(!admission.releaseCircuitCreditOnMain(mismatch) && admission.hasPendingEnableOnMain({7,8}));
 CHECK(admission.releaseCircuitCreditOnMain(enabled) && !admission.hasPendingEnableOnMain({7,8}));
 channels.commands.closeOnConsumer(); CHECK(!channels.commands.beginOfferOnMain());
 for(uint64_t i=2;i<=257;++i) { terminal.operationId=i; CHECK(admission.releaseReliableCreditOnMain(terminal)); }
 CHECK(admission.emptyOnMain() && !admission.releaseReliableCreditOnMain(terminal));
}
'''
out=pathlib.Path(sys.argv[1]).resolve()
if not out.is_relative_to(root):raise SystemExit('output must be inside worktree')
out.parent.mkdir(parents=True,exist_ok=True);text=prefix+body+suffix;out.write_text(text)
print('body_sha256',hashlib.sha256(body.encode()).hexdigest());print('fixture_sha256',hashlib.sha256(text.encode()).hexdigest())
