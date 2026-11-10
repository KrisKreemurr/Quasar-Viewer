// Actual pump/state transaction with a fake socket/template boundary.
// SPDX-License-Identifier: LGPL-2.1-only
#include "lltransportreceivepump.h"
#include "lltransportcontrolwarningstats.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>
using U32=uint32_t;
constexpr int MBT_SINGLE=1,MBT_MULTIPLE=2,MBT_VARIABLE=3,MVT_VARIABLE=1,MVT_FIXED=2;
struct LLMessageVariable { int type,size; int getType() const { return type; } int getSize() const { return size; } };
struct LLMessageBlock { int mType=MBT_SINGLE,mNumber=1; std::vector<LLMessageVariable*> mMemberVariables; };
struct LLMessageTemplate { std::vector<LLMessageBlock*> mMemberBlocks; U32 mMessageNumber=0; };
#include "transport_schema_freeze.inc"
static void check(bool ok,int line) { if (!ok) { std::fprintf(stderr,"failed line %d\n",line); std::abort(); } }
#define CHECK(x) check(bool(x),__LINE__)
using State=LLTransportCircuitState;
using Result=LLTransportControlApply;
static constexpr LLTransportEndpoint endpoint{0x0100007f,1234};
static constexpr State::WatchdogConfig watchdog{1000000,60000000,0.f,0.5f};

struct Fixture
{
    std::unique_ptr<State> state=std::make_unique<State>();
    LLMessageVariable id{MVT_FIXED,1}, oldest{MVT_FIXED,4}, ackId{MVT_FIXED,4};
    LLMessageBlock pingBlock,ackBlock;
    LLMessageTemplate ping,ack;
    LLTransportStartupBudget budget;
    LLTransportSchema pingSchema,ackSchema;
    LLTransportSchemaEntry entries[2]{};
    std::unique_ptr<LLTransportDecodedArena> arena=std::make_unique<LLTransportDecodedArena>();
    std::unique_ptr<LLTransportIngressQueue> events=std::make_unique<LLTransportIngressQueue>();
    LLPacketRing ring;
    LLPacketRing::UDPProxySnapshot proxy;
    std::atomic<bool> poison{false};
    std::unique_ptr<LLTransportReceivePump> pump;
    uint64_t incarnation=0;
    explicit Fixture(uint32_t ackCapacity=8, uint32_t rememberedCapacity=16)
    {
        uint64_t charge=0; CHECK(state->init({1,4,rememberedCapacity,ackCapacity,8,16,8388608},charge));
        const auto enabled=state->enable(endpoint,7,true,10,watchdog);
        CHECK(enabled.status==State::Status::Ok); incarnation=enabled.incarnation;
        ping.mMessageNumber=1; pingBlock.mMemberVariables={&id,&oldest}; ping.mMemberBlocks={&pingBlock};
        ack.mMessageNumber=0xfffffffbu; ackBlock.mType=MBT_VARIABLE; ackBlock.mMemberVariables={&ackId}; ack.mMemberBlocks={&ackBlock};
        CHECK((LLTransportStartupBudget::initialize<16384,512,2048,1>(budget,1024,1000000,1000000)));
        CHECK(freezeTransportSchema(ping,8,budget,pingSchema)==LLTransportFreezeStatus::Complete);
        CHECK(freezeTransportSchema(ack,8,budget,ackSchema)==LLTransportFreezeStatus::Complete);
        entries[0]={1,&pingSchema,false,true}; entries[1]={0xfffffffbu,&ackSchema,false,true};
        CHECK(arena->initialize(4096,1024));
        pump=std::make_unique<LLTransportReceivePump>(ring,*state,*arena,*events,poison,entries,2);
    }
    void pingPacket(uint8_t flags,uint8_t packet,bool truncated=false)
    {
        ring.packet={flags,0,0,0,packet,0,1,7};
        if (!truncated) ring.packet.insert(ring.packet.end(),{0,0,0,0});
        CHECK(pump->pollOne(0,proxy,7,LLTransportSteadyUsec(20))==LLTransportRxStatus::ControlPending);
    }
};

static void reliableReplyAndAck()
{
    Fixture f; f.pingPacket(0x40,1);
    CHECK(f.pump->preparePendingControl(LLTransportSteadyUsec(20),false,true)==Result::NeedReplyCapacity);
    CHECK(f.pump->preparePendingControl(LLTransportSteadyUsec(20),true,true)==Result::Ready);
    CHECK(f.state->advance(21,0).kind==State::ActionKind::None); // reserved ACK is not sendable
    CHECK(f.pump->applyPendingControl(LLTransportSteadyUsec(21),1000000,true,true)==Result::Applied);
    CHECK(!f.pump->releaseControl());
    CHECK(f.pump->commitPendingControl(LLTransportSteadyUsec(21),false)==Result::NeedReplyCapacity);
    CHECK(f.state->advance(22,0).kind==State::ActionKind::None);
    CHECK(f.pump->commitPendingControl(LLTransportSteadyUsec(22),true)==Result::Applied);
    CHECK(f.pump->commitPendingControl(LLTransportSteadyUsec(22),true)==Result::Rejected);
    const auto action=f.state->advance(23,0);
    CHECK(action.kind==State::ActionKind::Ack && action.ackCount==1 && action.ackIds[0]==1);
}

static void fullAndDiagnosticAndStale()
{
    Fixture full(1,1);
    const auto old=full.state->reserveReceive(endpoint,5,false,true,11);
    CHECK(old.status==State::ReceiveStatus::New && full.state->commitReceive(old.reservation,8,11)==State::Status::Ok);
    CHECK(full.state->reserveReliableSlot(endpoint,full.incarnation,90)==State::Status::Ok);
    CHECK(full.state->acceptReliableSlot(90)==State::Status::Ok);
    uint32_t outId=0; CHECK(full.state->allocateReliablePacketId(90,outId)==State::Status::Ok);
    const uint8_t outbound[]={0x40,0,0,0,1,0,42,1};
    CHECK(full.state->prepareReliable(90,outId,outbound,sizeof(outbound),{1,1000000,1000000,false},12)==State::Status::Ok);
    auto sent=full.state->advance(12,1000000); CHECK(sent.kind==State::ActionKind::InitialSend);
    CHECK(full.state->commitAttempt(sent.token,12,0,true,8)==State::Status::Ok);
    // Full new reliable control with a trailer must be refused before that
    // trailer can ACK an outgoing operation or the body can change state.
    full.ring.packet={0x50,0,0,0,2,0,1,7,0,0,0,0,
        uint8_t(outId>>24),uint8_t(outId>>16),uint8_t(outId>>8),uint8_t(outId),1};
    CHECK(full.pump->pollOne(0,full.proxy,7,LLTransportSteadyUsec(20))==LLTransportRxStatus::DataFull);
    CHECK(full.pump->controlBackpressureOnOwner()==1);
    CHECK(full.state->consumeAck(endpoint,full.incarnation,outId)==State::Status::Ok);
    // A committed resent duplicate is distinguished when its ACK lane is full.
    full.ring.packet={0x60,0,0,0,5,0,1,7,0,0,0,0};
    CHECK(full.pump->pollOne(0,full.proxy,7,LLTransportSteadyUsec(20))==LLTransportRxStatus::DataFull);
    CHECK(full.pump->controlBackpressureOnOwner()==1 && full.pump->controlDuplicateAckBackpressureOnOwner()==1);
    // Ordinary unreliable ping still applies while the reliable lane is full.
    full.ring.packet={0,0,0,0,3,0,1,8,0,0,0,0};
    CHECK(full.pump->pollOne(0,full.proxy,7,LLTransportSteadyUsec(20))==LLTransportRxStatus::ControlPending);
    CHECK(full.pump->applyPendingControl(LLTransportSteadyUsec(20),1000000,true,true)==Result::Applied);
    CHECK(full.pump->commitPendingControl(LLTransportSteadyUsec(20),true)==Result::Applied);
    const auto ack=full.state->advance(21,0);
    CHECK(ack.kind==State::ActionKind::Ack && ack.ackCount==1 && ack.ackIds[0]==5);
    uint32_t ackPacket=0;
    CHECK(full.state->assignActionPacketId(ack.token,ackPacket)==State::Status::Ok);
    CHECK(full.state->commitAttempt(ack.token,21,0,true,10)==State::Status::Ok);
    // The remembered table remains full, but a known resent duplicate is
    // acknowledged without admitting or reapplying its body.
    full.ring.packet={0x60,0,0,0,5,0,1,7,0,0,0,0};
    CHECK(full.pump->pollOne(0,full.proxy,7,LLTransportSteadyUsec(22))==LLTransportRxStatus::Duplicate);
    const auto duplicateAck=full.state->advance(23,0);
    CHECK(duplicateAck.kind==State::ActionKind::Ack && duplicateAck.ackCount==1 && duplicateAck.ackIds[0]==5);
    Fixture truncated; truncated.pingPacket(0x40,3,true);
    CHECK(truncated.pump->preparePendingControl(LLTransportSteadyUsec(20),true,false)==Result::NeedDiagnostics);
    CHECK(truncated.state->advance(21,0).kind==State::ActionKind::None);
    // Missing fixed fields are zero-filled for compatibility. A bounded
    // warning must be published before the exact event may apply or ACK.
    LLTransportControlWarning warning; CHECK(truncated.pump->pendingControlWarning(warning));
    CHECK(warning.truncations && warning.packetId==3 && warning.observation);
    auto wrong=warning; ++wrong.observation;
    CHECK(!truncated.pump->markControlWarningRetainedOnOwner(wrong));
    LLTransportControlWarningPublication warnings;
    CHECK(warnings.publishOnOwner(warning,21));
    CHECK(truncated.pump->markControlWarningRetainedOnOwner(warning));
    CHECK(!truncated.pump->markControlWarningRetainedOnOwner(warning));
    CHECK(truncated.pump->applyPendingControl(LLTransportSteadyUsec(21),1000000,true,false)==Result::Applied);
    CHECK(truncated.pump->commitPendingControl(LLTransportSteadyUsec(21),true)==Result::Applied);
    LLTransportControlWarningStats published;
    CHECK(warnings.readOnMain(1,published) && published.last.observation==warning.observation && published.events==1);
    for (uint32_t i=0;i<1000;++i) CHECK(warnings.publishOnOwner(warning,22+i));
    CHECK(warnings.readOnMain(1,published) && published.events==1001 && published.coalescedEvents==1000 && !published.saturated);
#if defined(LL_TRANSPORT_WARNING_TESTING)
    LLTransportControlWarningPublication saturated;
    CHECK(saturated.testSeedTotalsOnOwner(1,UINT64_MAX,UINT64_MAX));
    CHECK(saturated.publishOnOwner(warning,21));
    CHECK(saturated.readOnMain(1,published) && published.saturated && published.unaccountedEvents==1);
#endif
    const auto truncatedAck=truncated.state->advance(22,0);
    CHECK(truncatedAck.kind==State::ActionKind::Ack && truncatedAck.ackCount==1 && truncatedAck.ackIds[0]==3);
    Fixture stale; stale.pingPacket(0x40,4);
    CHECK(stale.pump->preparePendingControl(LLTransportSteadyUsec(20),true,true)==Result::Ready);
    CHECK(stale.state->disable(endpoint,stale.incarnation,0).status==LLTransportCircuitCommandStatus::Applied);
    CHECK(stale.pump->discardPendingIfStale(7,LLTransportSteadyUsec(21)));
    CHECK(stale.state->advance(22,0).kind==State::ActionKind::None);
    // Violate the caller's no-interleaving rule: commit must poison instead
    // of pretending the previously applied reliable control was ACKed.
    Fixture broken; broken.pingPacket(0x40,6);
    CHECK(broken.pump->applyPendingControl(LLTransportSteadyUsec(20),1000000,true,true)==Result::Applied);
    CHECK(broken.state->disable(endpoint,broken.incarnation,0).status==LLTransportCircuitCommandStatus::Applied);
    CHECK(broken.pump->commitPendingControl(LLTransportSteadyUsec(21),true)==Result::Poisoned && broken.poison.load());
    CHECK(broken.state->advance(22,0).kind==State::ActionKind::None);
}

static void partialAckDoesNotReplayReusedId()
{
    Fixture f;
    auto reserve=[&](uint64_t token) {
        CHECK(f.state->reserveReliableSlot(endpoint,f.incarnation,token)==State::Status::Ok);
        CHECK(f.state->acceptReliableSlot(token)==State::Status::Ok);
        uint32_t id=0; CHECK(f.state->allocateReliablePacketId(token,id)==State::Status::Ok);
        const uint8_t wire[]={0x40,0,0,0,1,0,42,1};
        CHECK(f.state->prepareReliable(token,id,wire,sizeof(wire),{1,1000000,1000000,false},11)==State::Status::Ok);
        return id;
    };
    const auto first=reserve(11); auto action=f.state->advance(11,1000000);
    CHECK(action.kind==State::ActionKind::InitialSend && action.reliableToken==11);
    CHECK(f.state->commitAttempt(action.token,11,0,true,8)==State::Status::Ok);
    const auto second=reserve(12); action=f.state->advance(12,1000000);
    CHECK(action.kind==State::ActionKind::InitialSend && action.reliableToken==12);
    f.ring.packet={0,0,0,0,100,0,0xff,0xff,0xff,0xfb,2};
    auto append=[&](uint32_t value) { for (unsigned i=0;i<4;++i) f.ring.packet.push_back(uint8_t(value>>(8*i))); };
    append(first); append(second);
    CHECK(f.pump->pollOne(0,f.proxy,7,LLTransportSteadyUsec(20))==LLTransportRxStatus::ControlPending);
    CHECK(f.pump->applyPendingControl(LLTransportSteadyUsec(20),1000000,false,true)==Result::Deferred);
    State::Completion completion; CHECK(f.state->takeCompletion(completion) && completion.token==11);
    CHECK(f.state->defer(action.token)==State::Status::Ok);
    CHECK(f.state->testSetLastOut(endpoint,f.incarnation,first-1)); // force legal ID wrap/reuse
    const auto reused=reserve(13); CHECK(reused==first);
    CHECK(f.pump->applyPendingControl(LLTransportSteadyUsec(21),1000000,false,true)==Result::Applied);
    CHECK(f.pump->commitPendingControl(LLTransportSteadyUsec(21),false)==Result::Applied);
    CHECK(f.state->consumeAck(endpoint,f.incarnation,reused)==State::Status::Ok); // retry did not ACK token 13
}
static void poisonedStopCancelsHeldReservation()
{
    Fixture f; f.pingPacket(0x40,1);
    CHECK(f.pump->preparePendingControl(LLTransportSteadyUsec(20),true,true)==Result::Ready);
    CHECK(f.state->preflightReliableControl(endpoint,f.incarnation,7,1,true)==State::ReceiveStatus::Full);
    f.poison.store(true);
    f.pump->cancelHeldForStop(); f.pump->cancelHeldForStop();
    CHECK(f.state->preflightReliableControl(endpoint,f.incarnation,7,1,true)==State::ReceiveStatus::New);
    CHECK(f.state->advance(21,0).kind==State::ActionKind::None);
}
int main() { reliableReplyAndAck(); fullAndDiagnosticAndStale(); partialAckDoesNotReplayReusedId(); poisonedStopCancelsHeldReservation(); }
