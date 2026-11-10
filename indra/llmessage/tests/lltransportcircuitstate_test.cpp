#include "lltransportcircuitstate.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #expr); std::abort(); } } while (false)
using State = LLTransportCircuitState;

int main()
{
    const State::WatchdogConfig defaultWatch{1000000, 60000000, 0.f, 0.5f};
    const State::WatchdogConfig controlWatch{100, 1000, 0.f, 0.5f};
    const State::WatchdogConfig pingWatch{1000000, 1000000, 0.f, 0.5f};
    uint64_t charged = 0;
    State state;
    CHECK(state.init({2, 2, 2, 3, 2, 32, 8388608}, charged));
    CHECK(state.enable({0x0100007f, 1234}, 7, true, 100, {}).status == State::Status::Invalid);
    CHECK(!state.init({2, 2, 2, 3, 2, 32, 8388608}, charged));
#if defined(LL_TRANSPORT_STATE_TESTING)
    // CompletePing needs an arrival publication and a ping publication while
    // its reliable receive is reserved. Refuse before trailers at MAX-2.
    {
        State boundary; uint64_t bytes=0;
        const LLTransportEndpoint host{0x0100007f,4321};
        CHECK(boundary.init({1,2,2,2,1,2,8388608},bytes));
        const auto enabled=boundary.enable(host,7,true,10,defaultWatch);
        CHECK(enabled.status==State::Status::Ok);
        CHECK(boundary.testSetSequence(UINT64_MAX-2));
        CHECK(boundary.preflightReliableControl(host,enabled.incarnation,7,4,false)==State::ReceiveStatus::Full);
        CHECK(boundary.testSetSequence(UINT64_MAX-3));
        CHECK(boundary.preflightReliableControl(host,enabled.incarnation,7,4,false)==State::ReceiveStatus::New);
        const auto observed=boundary.observeValidatedArrival(host,enabled.incarnation,7,4,false,8,20);
        CHECK(observed.status==State::ArrivalStatus::Observed && observed.observation);
        const auto reserved=boundary.reserveObservedReceive(host,enabled.incarnation,7,4,false,true,20,observed.observation);
        CHECK(reserved.status==State::ReceiveStatus::New && reserved.reservation);
        CHECK(boundary.notePingReply(host,enabled.incarnation,0,1000000,20)==State::Status::Ok);
        CHECK(boundary.commitReceive(reserved.reservation,8,20)==State::Status::Ok);
    }
    // The shared actor ledger reserves estimate once, then releases it on any
    // failed startup. Quota/overflow fail before the first allocation gate.
    State::Limits bounded{1, 1, 1, 1, 1, 1, 8388608};
    uint64_t estimate = 0;
    CHECK(State::estimateBytes(bounded, estimate) && estimate > 0);
    State::Limits tiny = bounded; tiny.byteBudget = estimate - 1;
    State preflight;
    CHECK(!preflight.testInit(tiny, charged, UINT32_MAX) && charged == 0 && preflight.testAllocationCalls() == 0);
    State::Limits invalid = bounded; invalid.circuits = UINT32_MAX;
    CHECK(!preflight.testInit(invalid, charged, UINT32_MAX) && charged == 0 && preflight.testAllocationCalls() == 0);
    State::Limits maximum{1024, 4096, 65536, 65536, 4096, 65536, 1};
    CHECK(!preflight.testInit(maximum, charged, UINT32_MAX) && charged == 0 && preflight.testAllocationCalls() == 0);
    uint64_t overflow = UINT64_MAX - 1;
    CHECK(!State::testCheckedAddProduct(overflow, 2, 2) && overflow == UINT64_MAX - 1);
    uint64_t multiplication = 0;
    CHECK(!State::testCheckedAddProduct(multiplication, UINT64_MAX, 2) && multiplication == 0);
    for (uint32_t failure = 0; failure < 7; ++failure)
    {
        State fault;
        uint64_t ledger = estimate; // actor has reserved this charge
        CHECK(!fault.testInit(bounded, charged, failure));
        CHECK(charged == 0 && fault.testAllocationCalls() == failure + 1);
        ledger -= estimate; // release the reservation after failed init
        CHECK(ledger == 0);
        CHECK(fault.init(bounded, charged) && charged == estimate);
    }
#endif
    LLTransportEndpoint endpoint{0x0100007f, 1234};
    auto enabled = state.enable(endpoint, 7, true, 100, defaultWatch);
    CHECK(enabled.status == State::Status::Ok && enabled.incarnation);
    const auto beforeWrongLogin=state.lookup(endpoint,100);
    CHECK(state.reserveReliableSlot(endpoint,enabled.incarnation,8,500)==State::Status::Stale);
    CHECK(state.reserveReliableSlot(endpoint,enabled.incarnation,0,500)==State::Status::Invalid);
    const auto afterWrongLogin=state.lookup(endpoint,100);
    CHECK(afterWrongLogin.publicationSequence==beforeWrongLogin.publicationSequence &&
          afterWrongLogin.packetsOut==beforeWrongLogin.packetsOut);
    CHECK(state.reserveReliableSlot(endpoint,enabled.incarnation,7,500)==State::Status::Ok);
    CHECK(state.cancelReliableSlot(500)==State::Status::Ok); // stale attempt did not claim token/capacity
    auto first = state.reserveReceive(endpoint, 4, false, true, 101);
    CHECK(first.status == State::ReceiveStatus::New);
    uint32_t blockedId = 0;
    CHECK(state.allocatePacketId(endpoint, enabled.incarnation, blockedId) == State::Status::Busy);
    CHECK(state.expireLosses(endpoint, enabled.incarnation, 102, 10) == State::Status::Busy);
    CHECK(state.reserveReceive(endpoint, 4, true, true, 102).status == State::ReceiveStatus::Full);
    state.cancelReceive(first.reservation);
    CHECK(state.advance(102, 0).kind == State::ActionKind::None);
    first = state.reserveReceive(endpoint, 4, false, true, 103);
    CHECK(state.commitReceive(first.reservation, 24, 103) == State::Status::Ok);
    // Legacy clearDuplicateList accepts a full U32 OldestUnacked. A value
    // beyond the highest ID keeps this committed entry and is not an error.
    CHECK(state.forgetReceivedBefore(endpoint,enabled.incarnation,UINT32_MAX,103)==State::Status::Ok);
    CHECK(state.classifyResentDuplicate(endpoint,enabled.incarnation,7,4,true)==State::DuplicateClass::Duplicate);
    auto snapshot = state.lookup(endpoint, 104);
    CHECK(snapshot.packetsIn == 1 && snapshot.bytesIn == 24 && snapshot.lastPacketMonotonicUsec == 103);
    CHECK(state.reserveReceive(endpoint, 4, true, true, 104).status == State::ReceiveStatus::Duplicate);
    auto acks = state.advance(105, 0);
    CHECK(acks.kind == State::ActionKind::Ack && acks.ackCount == 2 && acks.ackIds[0] == 4 && acks.ackIds[1] == 4);
    uint32_t ackPacket = 0;
    CHECK(state.assignActionPacketId(acks.token, ackPacket) == State::Status::Ok && ackPacket == 1);
    CHECK(state.defer(acks.token) == State::Status::Ok);
    acks = state.advance(106, 0);
    CHECK(acks.ackCount == 2 && acks.packetAssigned && acks.packetId == ackPacket);
    CHECK(state.commitAttempt(acks.token, 106, 0, true, 10) == State::Status::Ok);
    auto readd = state.reserveReceive(endpoint, 4, false, true, 107);
    CHECK(readd.status == State::ReceiveStatus::New);
    // A cancelled re-admission retains the original committed duplicate record.
    state.cancelReceive(readd.reservation);
    auto pending = state.reserveReceive(endpoint, 5, false, true, 108);
    CHECK(pending.status == State::ReceiveStatus::New);
    CHECK(state.reserveReceive(endpoint, 6, false, true, 109).status == State::ReceiveStatus::Full);
    state.cancelReceive(pending.reservation);

    uint8_t bytes[] = {0x40, 1, 2};
    State::RetryPolicy retry{1, 10, 20, false};
    CHECK(state.reserveReliableSlot(endpoint, enabled.incarnation, 12) == State::Status::Ok);
    CHECK(state.acceptReliableSlot(12) == State::Status::Ok);
    uint32_t packetId = 0;
    CHECK(state.allocateReliablePacketId(12, packetId) == State::Status::Ok && packetId == 2);
    CHECK(state.allocateReliablePacketId(12, packetId) == State::Status::Busy);
    CHECK(state.cancelReliableSlot(12) == State::Status::Stale);
    CHECK(state.prepareReliable(12, packetId, bytes, 3, retry, 200) == State::Status::Ok);
    bytes[1] = 99; // ownership does not depend on the caller's buffer
    auto action = state.advance(200, 0);
    CHECK(action.kind == State::ActionKind::InitialSend && action.bytes[0] == 0x40 && action.bytes[1] == 1);
    bytes[1] = 1;
    CHECK(state.defer(action.token) == State::Status::Ok);
    action = state.advance(250, 0);
    // Legacy retains the reliable record even after a permanent send failure.
    CHECK(state.commitAttempt(action.token, 250, 0, false, 0) == State::Status::Ok);
    CHECK(state.oldestUnacked(endpoint, enabled.incarnation) == packetId);
    // The deadline was set at registration (200 + 10), not reset when the
    // deferred initial send finally succeeded at 250.
    action = state.advance(260, 0);
    CHECK(action.kind == State::ActionKind::Resend && action.bytes[0] == 0x60);
    CHECK(state.defer(action.token) == State::Status::Ok);
    action = state.advance(262, 0);
    CHECK(state.commitAttempt(action.token, 262, 20, true, 10) == State::Status::Ok);
    CHECK(state.consumeAck(endpoint, enabled.incarnation, packetId) == State::Status::Ok);
    CHECK(state.consumeAck(endpoint, enabled.incarnation, packetId) == State::Status::Absent);
    State::Completion completion;
    CHECK(state.takeCompletion(completion) && completion.token == 12 && completion.result == 0);
    CHECK(!state.takeCompletion(completion));

    snapshot = state.lookup(endpoint, 300);
    LLTransportCircuitCommand close{};
    close.kind = LLTransportCircuitCommandKind::DisableIfUnchanged;
    close.endpoint = endpoint; close.loginGeneration = 7; close.targetIncarnation = enabled.incarnation;
    close.observedPublicationSequence = snapshot.publicationSequence;
    close.observedHasLastPacket = snapshot.hasLastPacket;
    close.observedLastPacketMonotonicUsec = snapshot.lastPacketMonotonicUsec;
    auto newPacket = state.reserveReceive(endpoint, 8, false, false, 301);
    CHECK(newPacket.status == State::ReceiveStatus::New);
    CHECK(state.commitReceive(newPacket.reservation, 1, 301) == State::Status::Ok);
    CHECK(state.conditionalClose(close, 0).status == LLTransportCircuitCommandStatus::PacketChanged);
    snapshot = state.lookup(endpoint, 302);
    close.observedPublicationSequence = snapshot.publicationSequence;
    close.observedLastPacketMonotonicUsec = snapshot.lastPacketMonotonicUsec;
    auto closeResult = state.conditionalClose(close, 4242);
    CHECK(closeResult.status == LLTransportCircuitCommandStatus::Applied && closeResult.lastPublishedAppSequence == 4242);
    CHECK(state.lookup(endpoint, 303).state == LLTransportSnapshotState::Absent);
    CHECK(state.enable(endpoint, 7, true, 304, defaultWatch).incarnation != enabled.incarnation);

    // A WouldBlock result resolves the data action with defer. ACK and ping
    // must still progress, then the same uncharged bytes can be reacquired.
    State control;
    CHECK(control.init({1, 2, 2, 3, 2, 16, 8388608}, charged));
    auto circuit = control.enable(endpoint, 9, false, 100, controlWatch);
    auto receive = control.reserveReceive(endpoint, 3, false, true, 101);
    CHECK(control.commitReceive(receive.reservation, 1, 101) == State::Status::Ok);
    CHECK(control.reserveReliableSlot(endpoint, circuit.incarnation, 31) == State::Status::Ok);
    CHECK(control.acceptReliableSlot(31) == State::Status::Ok);
    uint32_t controlPacket = 0;
    CHECK(control.allocateReliablePacketId(31, controlPacket) == State::Status::Ok && controlPacket == 1);
    CHECK(control.prepareReliable(31, controlPacket, bytes, 3, retry, 102) == State::Status::Ok);
    CHECK(control.pollWatchdog(endpoint, circuit.incarnation, 200, true, 0, 0.5f).status == State::WatchdogStatus::PingReady);
    auto data = control.advance(200, 0);
    CHECK(data.kind == State::ActionKind::InitialSend && data.packetId == controlPacket);
    CHECK(control.defer(data.token) == State::Status::Ok);
    uint32_t interveningId = 0;
    CHECK(control.allocatePacketId(endpoint, circuit.incarnation, interveningId) == State::Status::Ok && interveningId == 2);
    auto controlAck = control.advance(200, 0);
    CHECK(controlAck.kind == State::ActionKind::Ack && controlAck.ackCount == 1 && controlAck.ackIds[0] == 3);
    uint32_t controlAckId = 0;
    CHECK(control.assignActionPacketId(controlAck.token, controlAckId) == State::Status::Ok);
    CHECK(control.commitAttempt(controlAck.token, 200, 0, true, 10) == State::Status::Ok);
    auto controlPing = control.advance(200, 0);
    CHECK(controlPing.kind == State::ActionKind::Ping && controlPing.oldestUnacked == controlPacket);
    uint32_t controlPingId = 0;
    CHECK(control.assignActionPacketId(controlPing.token, controlPingId) == State::Status::Ok);
    CHECK(control.commitAttempt(controlPing.token, 200, 0, true, 10) == State::Status::Ok);
    data = control.advance(201, 0);
    CHECK(data.kind == State::ActionKind::InitialSend && data.packetId == controlPacket && data.bytes[0] == 0x40);
    CHECK(control.commitAttempt(data.token, 201, 0, true, 10) == State::Status::Ok);
    CHECK(control.reserveReliableSlot(endpoint, circuit.incarnation, 32) == State::Status::Ok);
    CHECK(control.reserveReliableSlot(endpoint, circuit.incarnation, 33) == State::Status::Full);
    CHECK(control.cancelReliableSlot(32) == State::Status::Ok);
    CHECK(!control.takeCompletion(completion));
    CHECK(control.reserveReliableSlot(endpoint, circuit.incarnation, 32) == State::Status::Ok);
    CHECK(control.acceptReliableSlot(32) == State::Status::Ok);
    auto last = control.lookup(endpoint, 203);
    LLTransportCircuitCommand stop{};
    stop.kind = LLTransportCircuitCommandKind::DisableIfUnchanged;
    stop.endpoint = endpoint; stop.loginGeneration = 9; stop.targetIncarnation = circuit.incarnation;
    stop.observedPublicationSequence = last.publicationSequence;
    stop.observedHasLastPacket = last.hasLastPacket;
    stop.observedLastPacketMonotonicUsec = last.lastPacketMonotonicUsec;
    CHECK(control.conditionalClose(stop, 0).status == LLTransportCircuitCommandStatus::Applied);
    CHECK(control.takeCompletion(completion) && completion.token == 31 && completion.result == -23017);
    CHECK(control.takeCompletion(completion) && completion.token == 32 && completion.result == -23017);
    CHECK(!control.takeCompletion(completion));

    // Reliable bookkeeping starts before the first successful socket attempt.
    // Legacy message.cpp registers reliable bytes before the first sendto;
    // Windows net.cpp retries WouldBlock inside that same blocking call, so
    // main-thread processAcks cannot expire or resend it until the call returns.
    // The actor can service control while deferred but must retain Initial.
    State heldInitial;
    CHECK(heldInitial.init({1, 1, 1, 1, 1, 1, 8388608}, charged));
    auto heldCircuit = heldInitial.enable(endpoint, 23, false, 0, defaultWatch);
    CHECK(heldInitial.reserveReliableSlot(endpoint, heldCircuit.incarnation, 121) == State::Status::Ok);
    CHECK(heldInitial.acceptReliableSlot(121) == State::Status::Ok);
    uint32_t heldId = 0;
    CHECK(heldInitial.allocateReliablePacketId(121, heldId) == State::Status::Ok);
    State::RetryPolicy heldPolicy{1, 5, 20, false};
    CHECK(heldInitial.prepareReliable(121, heldId, bytes, 3, heldPolicy, 0) == State::Status::Ok);
    auto held = heldInitial.advance(0, 0);
    CHECK(held.kind == State::ActionKind::InitialSend && held.packetId == heldId && held.bytes[0] == 0x40);
    CHECK(heldInitial.defer(held.token) == State::Status::Ok);
    held = heldInitial.advance(6, 0); // past the registration deadline
    CHECK(held.kind == State::ActionKind::InitialSend && held.packetId == heldId && held.reliableToken == 121 && held.bytes[0] == 0x40);
    CHECK(!heldInitial.takeCompletion(completion));
    CHECK(heldInitial.defer(held.token) == State::Status::Ok);
    held = heldInitial.advance(100, 0);
    CHECK(held.kind == State::ActionKind::InitialSend && held.packetId == heldId && held.bytes[0] == 0x40);
    CHECK(!heldInitial.takeCompletion(completion));
    CHECK(heldInitial.commitAttempt(held.token, 100, 20, true, 3) == State::Status::Ok);
    auto dueRetry = heldInitial.advance(100, 0);
    CHECK(dueRetry.kind == State::ActionKind::Resend && dueRetry.packetId == heldId && dueRetry.bytes[0] == 0x60);
    CHECK(heldInitial.commitAttempt(dueRetry.token, 100, 20, true, 3) == State::Status::Ok);
    CHECK(heldInitial.advance(120, 0).kind == State::ActionKind::None && !heldInitial.takeCompletion(completion));
    CHECK(heldInitial.advance(121, 0).kind == State::ActionKind::None);
    CHECK(heldInitial.takeCompletion(completion) && completion.token == 121 && completion.result == -23016);

    // A zero-retry initial send also remains Initial after its deadline;
    // explicit circuit close is allowed to deliver CircuitGone instead.
    State stoppedInitial;
    CHECK(stoppedInitial.init({1, 1, 1, 1, 1, 1, 8388608}, charged));
    auto stoppedCircuit = stoppedInitial.enable(endpoint, 24, false, 0, defaultWatch);
    CHECK(stoppedInitial.reserveReliableSlot(endpoint, stoppedCircuit.incarnation, 122) == State::Status::Ok);
    CHECK(stoppedInitial.acceptReliableSlot(122) == State::Status::Ok);
    uint32_t stoppedId = 0;
    CHECK(stoppedInitial.allocateReliablePacketId(122, stoppedId) == State::Status::Ok);
    CHECK(stoppedInitial.prepareReliable(122, stoppedId, bytes, 3, {0, 5, 20, false}, 0) == State::Status::Ok);
    auto stopped = stoppedInitial.advance(0, 0);
    CHECK(stopped.kind == State::ActionKind::InitialSend && stoppedInitial.defer(stopped.token) == State::Status::Ok);
    stopped = stoppedInitial.advance(100, 0);
    CHECK(stopped.kind == State::ActionKind::InitialSend && stopped.packetId == stoppedId && stopped.bytes[0] == 0x40);
    CHECK(!stoppedInitial.takeCompletion(completion));
    CHECK(stoppedInitial.defer(stopped.token) == State::Status::Ok);
    CHECK(stoppedInitial.disable(endpoint, stoppedCircuit.incarnation, 0).status == LLTransportCircuitCommandStatus::Applied);
    CHECK(stoppedInitial.takeCompletion(completion) && completion.token == 122 && completion.result == -23017);

    State earlyAck;
    CHECK(earlyAck.init({1, 1, 1, 1, 1, 1, 8388608}, charged));
    auto earlyCircuit = earlyAck.enable(endpoint, 15, false, 0, defaultWatch);
    CHECK(earlyAck.reserveReliableSlot(endpoint, earlyCircuit.incarnation, 51) == State::Status::Ok);
    CHECK(earlyAck.acceptReliableSlot(51) == State::Status::Ok);
    uint32_t earlyId = 0;
    CHECK(earlyAck.allocateReliablePacketId(51, earlyId) == State::Status::Ok);
    CHECK(earlyAck.prepareReliable(51, earlyId, bytes, 3, retry, 0) == State::Status::Ok);
    CHECK(earlyAck.consumeAck(endpoint, earlyCircuit.incarnation, earlyId) == State::Status::Ok);
    CHECK(earlyAck.takeCompletion(completion) && completion.token == 51 && completion.result == 0);
    CHECK(earlyAck.advance(100, 0).kind == State::ActionKind::None);

    // Actual 24-bit ID wrap with an earlier unassigned reservation. Packet 0
    // must resolve by the reliable token, not the other slot's default ID 0.
    State wrapping;
    CHECK(wrapping.init({1, 1, 1, 1, 2, 1, 8388608}, charged));
    auto wrapCircuit = wrapping.enable(endpoint, 19, false, 0, defaultWatch);
    CHECK(wrapping.reserveReliableSlot(endpoint, wrapCircuit.incarnation, 71) == State::Status::Ok);
    CHECK(wrapping.reserveReliableSlot(endpoint, wrapCircuit.incarnation, 72) == State::Status::Ok);
    CHECK(wrapping.acceptReliableSlot(72) == State::Status::Ok);
    uint32_t wrappedId = 0;
    for (uint32_t id = 1; id < 0x01000000u; ++id)
    {
        CHECK(wrapping.allocatePacketId(endpoint, wrapCircuit.incarnation, wrappedId) == State::Status::Ok);
        CHECK(wrappedId == id);
    }
    CHECK(wrapping.allocateReliablePacketId(72, wrappedId) == State::Status::Ok && wrappedId == 0);
    CHECK(wrapping.prepareReliable(72, wrappedId, bytes, 3, retry, 0) == State::Status::Ok);
    auto wrapAction = wrapping.advance(1, 0);
    CHECK(wrapAction.kind == State::ActionKind::InitialSend && wrapAction.reliableToken == 72);
    CHECK(wrapping.commitAttempt(wrapAction.token, 1, 0, true, 3) == State::Status::Ok);
    CHECK(wrapping.consumeAck(endpoint, wrapCircuit.incarnation, 0) == State::Status::Ok);
    CHECK(wrapping.takeCompletion(completion) && completion.token == 72 && completion.result == 0);
    CHECK(wrapping.cancelReliableSlot(71) == State::Status::Ok);

    State shuttingDown;
    CHECK(shuttingDown.init({1, 1, 1, 1, 2, 1, 8388608}, charged));
    auto shuttingCircuit = shuttingDown.enable(endpoint, 16, false, 0, defaultWatch);
    CHECK(shuttingDown.reserveReliableSlot(endpoint, shuttingCircuit.incarnation, 61) == State::Status::Ok);
    CHECK(shuttingDown.acceptReliableSlot(61) == State::Status::Ok);
    CHECK(shuttingDown.reserveReliableSlot(endpoint, shuttingCircuit.incarnation, 62) == State::Status::Ok);
    shuttingDown.shutdown();
    shuttingDown.shutdown();
    CHECK(shuttingDown.takeCompletion(completion) && completion.token == 61 && completion.result == -23017);
    CHECK(!shuttingDown.takeCompletion(completion));
    CHECK(shuttingDown.enable(endpoint, 16, false, 1, defaultWatch).status == State::Status::Invalid);

    // Negative legacy debug timeouts can already be expired at steady time 0.
    // Wire accounting counts the actual successful initial datagram length,
    // including appended ACKs, but not the legacy resend path.
    State negative;
    CHECK(negative.init({1, 1, 1, 1, 1, 1, 8388608}, charged));
    auto negativeCircuit = negative.enable(endpoint, 20, false, 0, defaultWatch);
    CHECK(negative.reserveReliableSlot(endpoint, negativeCircuit.incarnation, 81) == State::Status::Ok);
    CHECK(negative.acceptReliableSlot(81) == State::Status::Ok);
    uint32_t negativeId = 0;
    CHECK(negative.allocateReliablePacketId(81, negativeId) == State::Status::Ok);
    State::RetryPolicy negativeRetry{1, -5, -5, true};
    CHECK(negative.prepareReliable(81, negativeId, bytes, 3, negativeRetry, 0) == State::Status::Ok);
    auto negativeAction = negative.advance(0, 0);
    CHECK(negativeAction.kind == State::ActionKind::InitialSend);
    CHECK(negative.commitAttempt(negativeAction.token, 0, -5, true, 13) == State::Status::Ok);
    negativeAction = negative.advance(0, 0);
    CHECK(negativeAction.kind == State::ActionKind::Resend);
    int64_t fixedNegative=99;
    CHECK(negative.selectedRetryInterval(negativeAction.token,0,fixedNegative)==State::Status::Ok && fixedNegative==-5);
    CHECK(negative.commitAttempt(negativeAction.token, 0, fixedNegative, true, 3) == State::Status::Ok);
    CHECK(negative.advance(0, 0).kind == State::ActionKind::None);
    CHECK(negative.takeCompletion(completion) && completion.token == 81 && completion.result == -23016);
    CHECK(negative.noteOrdinaryAttempt(endpoint, negativeCircuit.incarnation, true, 20) == State::Status::Ok);
    CHECK(negative.noteOrdinaryAttempt(endpoint, negativeCircuit.incarnation, false, 0) == State::Status::Ok);
    uint64_t bytesOut = 0;
    CHECK(negative.successfulBytesOut(endpoint, negativeCircuit.incarnation, bytesOut) == State::Status::Ok && bytesOut == 33);

    State loss;
    CHECK(loss.init({1, 1, 1, 1, 1, 2, 8388608}, charged));
    auto lossCircuit = loss.enable(endpoint, 11, false, 0, defaultWatch);
    auto receive0 = loss.reserveReceive(endpoint, 0, false, false, 1);
    CHECK(loss.commitReceive(receive0.reservation, 1, 1) == State::Status::Ok);
    auto gap = loss.reserveReceive(endpoint, 3, false, false, 2);
    CHECK(gap.status == State::ReceiveStatus::New);
    CHECK(loss.commitReceive(gap.reservation, 1, 2) == State::Status::Ok);
    // The two reserved loss slots cover 1 and 2; a further small gap fails
    // before its packet can be accepted into the decode queue.
    CHECK(loss.reserveReceive(endpoint, 5, false, false, 3).status == State::ReceiveStatus::Full);
    auto recovered = loss.reserveReceive(endpoint, 1, true, false, 4);
    CHECK(recovered.status == State::ReceiveStatus::New);
    CHECK(loss.commitReceive(recovered.reservation, 1, 4) == State::Status::Ok);
    CHECK(loss.expireLosses(endpoint, lossCircuit.incarnation, 12, 10) == State::Status::Ok);
    CHECK(loss.lookup(endpoint, 12).packetsLost == 0); // strict > timeout
    CHECK(loss.expireLosses(endpoint, lossCircuit.incarnation, 13, 10) == State::Status::Ok);
    CHECK(loss.lookup(endpoint, 13).packetsLost == 1);

    State piggy;
    CHECK(piggy.init({1, 1, 3, 3, 1, 3, 8388608}, charged));
    auto piggyCircuit = piggy.enable(endpoint, 13, false, 10, defaultWatch);
    auto ackOne = piggy.reserveReceive(endpoint, 1, false, true, 11);
    CHECK(piggy.commitReceive(ackOne.reservation, 1, 11) == State::Status::Ok);
    auto ackTwo = piggy.reserveReceive(endpoint, 2, false, true, 12);
    CHECK(piggy.commitReceive(ackTwo.reservation, 1, 12) == State::Status::Ok);
    State::Piggyback appended;
    CHECK(piggy.reservePiggyback(endpoint, piggyCircuit.incarnation, 1, appended) == State::Status::Ok);
    CHECK(appended.count == 1 && appended.ackIds[0] == 1);
    CHECK(piggy.finishPiggyback(appended.token, false) == State::Status::Ok);
    CHECK(piggy.reservePiggyback(endpoint, piggyCircuit.incarnation, 1, appended) == State::Status::Ok);
    CHECK(appended.count == 1 && appended.ackIds[0] == 1);
    CHECK(piggy.finishPiggyback(appended.token, true) == State::Status::Ok);
    auto remainingAck = piggy.advance(13, 0);
    CHECK(remainingAck.kind == State::ActionKind::Ack && remainingAck.ackCount == 1 && remainingAck.ackIds[0] == 2);
    uint32_t remainingAckId = 0;
    CHECK(piggy.assignActionPacketId(remainingAck.token, remainingAckId) == State::Status::Ok);
    CHECK(piggy.commitAttempt(remainingAck.token, 13, 0, true, 10) == State::Status::Ok);

    // Piggyback removal does not reset legacy mAckCreationTime. A later ACK
    // inherits the old collection age until a standalone ACK is sent.
    State ackAge;
    CHECK(ackAge.init({1, 1, 2, 2, 1, 1, 8388608}, charged));
    auto ageCircuit = ackAge.enable(endpoint, 18, false, 0, defaultWatch);
    auto ageFirst = ackAge.reserveReceive(endpoint, 1, false, true, 100);
    CHECK(ackAge.commitReceive(ageFirst.reservation, 1, 100) == State::Status::Ok);
    CHECK(ackAge.reservePiggyback(endpoint, ageCircuit.incarnation, 1, appended) == State::Status::Ok);
    CHECK(ackAge.finishPiggyback(appended.token, true) == State::Status::Ok);
    auto ageNext = ackAge.reserveReceive(endpoint, 2, false, true, 200);
    CHECK(ackAge.commitReceive(ageNext.reservation, 1, 200) == State::Status::Ok);
    CHECK(ackAge.advance(250, 150).kind == State::ActionKind::None);
    auto ageDue = ackAge.advance(251, 150);
    CHECK(ageDue.kind == State::ActionKind::Ack && ageDue.ackIds[0] == 2);
    uint32_t agePacket = 0;
    CHECK(ackAge.assignActionPacketId(ageDue.token, agePacket) == State::Status::Ok);
    CHECK(ackAge.commitAttempt(ageDue.token, 251, 0, true, 10) == State::Status::Ok);

    // Different legacy limits: up to 250 piggybacked, 251 in PacketAck.
    State ackLimits;
    CHECK(ackLimits.init({1, 1, 251, 251, 1, 1, 8388608}, charged));
    auto ackCircuit = ackLimits.enable(endpoint, 14, false, 0, defaultWatch);
    for (uint32_t id = 1; id <= 251; ++id)
    {
        auto current = ackLimits.reserveReceive(endpoint, id, false, true, id);
        CHECK(current.status == State::ReceiveStatus::New);
        CHECK(ackLimits.commitReceive(current.reservation, 1, id) == State::Status::Ok);
    }
    auto overCapacity = ackLimits.reserveReceive(endpoint, 252, false, true, 252);
    CHECK(overCapacity.status == State::ReceiveStatus::Full);
    CHECK(ackLimits.reservePiggyback(endpoint, ackCircuit.incarnation, 300, appended) == State::Status::Ok);
    CHECK(appended.count == 250 && appended.ackIds[0] == 1 && appended.ackIds[249] == 250);
    CHECK(ackLimits.finishPiggyback(appended.token, false) == State::Status::Ok);
    auto fullAck = ackLimits.advance(300, 0);
    CHECK(fullAck.kind == State::ActionKind::Ack && fullAck.ackCount == 251);
    CHECK(fullAck.ackIds[0] == 1 && fullAck.ackIds[250] == 251);
    uint32_t fullAckId = 0;
    CHECK(ackLimits.assignActionPacketId(fullAck.token, fullAckId) == State::Status::Ok);
    CHECK(ackLimits.commitAttempt(fullAck.token, 300, 0, true, 10) == State::Status::Ok);

    // A watchdog revival resets outgoing IDs. A deferred unsent ACK must be
    // rebuilt, while the underlying ACK entry remains available.
    State revived;
    CHECK(revived.init({1, 1, 1, 1, 1, 1, 8388608}, charged));
    auto revivedCircuit = revived.enable(endpoint, 17, false, 0, defaultWatch);
    auto reviveReceive = revived.reserveReceive(endpoint, 5, false, true, 1);
    CHECK(revived.commitReceive(reviveReceive.reservation, 1, 1) == State::Status::Ok);
    auto oldAck = revived.advance(2, 0);
    uint32_t oldAckId = 0;
    CHECK(revived.assignActionPacketId(oldAck.token, oldAckId) == State::Status::Ok);
    CHECK(revived.defer(oldAck.token) == State::Status::Ok);
    State::TimeoutEvent revival;
    CHECK(revived.beginWatchdogTimeout(endpoint, revivedCircuit.incarnation, 3, 2, true, 0, revival) == State::Status::Ok);
    CHECK(revived.enable(endpoint, 17, false, 4, defaultWatch).incarnation == revivedCircuit.incarnation);
    CHECK(revived.finishWatchdogTimeout(revival, 0.5f) == State::Status::Ok);
    auto revivedPing = revived.advance(5, 0);
    CHECK(revivedPing.kind == State::ActionKind::Ping);
    uint32_t revivedPingId = 0;
    CHECK(revived.assignActionPacketId(revivedPing.token, revivedPingId) == State::Status::Ok && revivedPingId == 1);
    CHECK(revived.commitAttempt(revivedPing.token, 5, 0, true, 10) == State::Status::Ok);
    auto rebuiltAck = revived.advance(5, 0);
    CHECK(rebuiltAck.kind == State::ActionKind::Ack && !rebuiltAck.packetAssigned && rebuiltAck.ackIds[0] == 5);
    uint32_t rebuiltId = 0;
    CHECK(revived.assignActionPacketId(rebuiltAck.token, rebuiltId) == State::Status::Ok && rebuiltId == 2);
    CHECK(revived.commitAttempt(rebuiltAck.token, 5, 0, true, 10) == State::Status::Ok);

    // Simulate unretained APP payloads while ACK bookkeeping is full. This is
    // not a real packed APP queue test; actor integration must cover that.
    // Activity/bytes advance; unretained payloads do not enter dedup or ACK.
    State arrival;
    CHECK(arrival.init({2, 2, 2, 1, 2, 2, 8388608}, charged));
    LLTransportEndpoint other{0x0200007f, 4321};
    auto arrivalCircuit = arrival.enable(endpoint, 21, false, 100, controlWatch);
    auto otherCircuit = arrival.enable(other, 21, false, 100, controlWatch);
    auto heldAck = arrival.reserveReceive(other, 9, false, true, 100);
    CHECK(heldAck.status == State::ReceiveStatus::New); // exhaust ACK capacity
    CHECK(arrival.reserveReliableSlot(endpoint, arrivalCircuit.incarnation, 91) == State::Status::Ok);
    CHECK(arrival.acceptReliableSlot(91) == State::Status::Ok);
    uint32_t sentId = 0;
    CHECK(arrival.allocateReliablePacketId(91, sentId) == State::Status::Ok);
    CHECK(arrival.prepareReliable(91, sentId, bytes, 3, retry, 100) == State::Status::Ok);
    auto sentAction = arrival.advance(100, 0);
    CHECK(sentAction.kind == State::ActionKind::InitialSend);
    CHECK(arrival.commitAttempt(sentAction.token, 100, 20, true, 3) == State::Status::Ok);
    CHECK(arrival.observeValidatedArrival(endpoint, arrivalCircuit.incarnation, 22, 7, false, 10, 101).status == State::ArrivalStatus::Invalid);
    CHECK(arrival.observeValidatedArrival(endpoint, arrivalCircuit.incarnation + 1, 21, 7, false, 10, 101).status == State::ArrivalStatus::Invalid);
    auto dropped = arrival.observeValidatedArrival(endpoint, arrivalCircuit.incarnation, 21, 7, false, 10, 101);
    CHECK(dropped.status == State::ArrivalStatus::Observed);
    auto afterDropped = arrival.lookup(endpoint, 101);
    CHECK(afterDropped.packetsIn == 1 && afterDropped.bytesIn == 10 && afterDropped.lastPacketMonotonicUsec == 101);
    CHECK(arrival.reserveObservedReceive(endpoint, arrivalCircuit.incarnation, 22, 7, false, true, 101, dropped.observation).status == State::ReceiveStatus::Invalid);
    CHECK(arrival.reserveObservedReceive(endpoint, arrivalCircuit.incarnation, 21, 7, false, true, 101, dropped.observation).status == State::ReceiveStatus::Full);
    CHECK(arrival.classifyResentDuplicate(endpoint, arrivalCircuit.incarnation, 21, 7, true) == State::DuplicateClass::NotDuplicate);
    auto retried = arrival.observeValidatedArrival(endpoint, arrivalCircuit.incarnation, 21, 7, true, 12, 102);
    CHECK(retried.status == State::ArrivalStatus::Observed); // still not committed duplicate
    CHECK(arrival.reserveObservedReceive(endpoint, arrivalCircuit.incarnation, 21, 7, false, true, 102, dropped.observation).status == State::ReceiveStatus::Invalid);
    CHECK(arrival.reserveObservedReceive(endpoint, arrivalCircuit.incarnation, 21, 7, true, true, 102, retried.observation).status == State::ReceiveStatus::Full);
    CHECK(arrival.consumeAck(endpoint, arrivalCircuit.incarnation, sentId) == State::Status::Ok);
    CHECK(arrival.takeCompletion(completion) && completion.token == 91);
    CHECK(arrival.pollWatchdog(endpoint, arrivalCircuit.incarnation, 200, true, 1234, 0.5f).status == State::WatchdogStatus::PingReady);
    auto livePing = arrival.advance(200, 0);
    CHECK(livePing.kind == State::ActionKind::Ping);
    uint32_t livePingId = 0;
    CHECK(arrival.assignActionPacketId(livePing.token, livePingId) == State::Status::Ok);
    CHECK(arrival.commitAttempt(livePing.token, 200, 0, true, 10) == State::Status::Ok);
    arrival.cancelReceive(heldAck.reservation);
    // Unretained t=102 bytes are gone; capacity recovery needs a fresh datagram.
    auto freshRetry = arrival.observeValidatedArrival(endpoint, arrivalCircuit.incarnation, 21, 7, true, 12, 201);
    CHECK(freshRetry.status == State::ArrivalStatus::Observed);
    CHECK(arrival.reserveObservedReceive(endpoint, arrivalCircuit.incarnation, 21, 7, true, true, 201, retried.observation).status == State::ReceiveStatus::Invalid);
    auto retained = arrival.reserveObservedReceive(endpoint, arrivalCircuit.incarnation, 21, 7, true, true, 201, freshRetry.observation);
    CHECK(retained.status == State::ReceiveStatus::New);
    CHECK(arrival.commitReceive(retained.reservation, 12, 201) == State::Status::Ok);
    auto afterRetained = arrival.lookup(endpoint, 201);
    CHECK(afterRetained.packetsIn == 3 && afterRetained.bytesIn == 34 && afterRetained.lastPacketMonotonicUsec == 201);
    CHECK(arrival.classifyResentDuplicate(endpoint, arrivalCircuit.incarnation, 21, 7, true) == State::DuplicateClass::Duplicate);
    CHECK(arrival.classifyResentDuplicate(endpoint, arrivalCircuit.incarnation, 21, 7, false) == State::DuplicateClass::NotDuplicate);
    CHECK(arrival.observeValidatedArrival(endpoint, arrivalCircuit.incarnation, 21, 7, true, 12, 202).status == State::ArrivalStatus::Duplicate);
    auto afterDuplicate = arrival.lookup(endpoint, 202);
    CHECK(afterDuplicate.packetsIn == 3 && afterDuplicate.bytesIn == 34 && afterDuplicate.lastPacketMonotonicUsec == 201 &&
          afterDuplicate.publicationSequence == afterRetained.publicationSequence);
    CHECK(arrival.acknowledgeCommittedDuplicate(endpoint, arrivalCircuit.incarnation, 21, 7, 202) == State::ReceiveStatus::Full);
    auto retainedAck = arrival.advance(202, 0);
    CHECK(retainedAck.kind == State::ActionKind::Ack && retainedAck.ackCount == 1 && retainedAck.ackIds[0] == 7);
    uint32_t retainedAckId = 0;
    CHECK(arrival.assignActionPacketId(retainedAck.token, retainedAckId) == State::Status::Ok);
    CHECK(arrival.commitAttempt(retainedAck.token, 202, 0, true, 10) == State::Status::Ok);
    CHECK(arrival.acknowledgeCommittedDuplicate(endpoint, arrivalCircuit.incarnation, 21, 7, 203) == State::ReceiveStatus::Duplicate);
    LLTransportCircuitCommand disableCommand{};
    disableCommand.kind = LLTransportCircuitCommandKind::Disable;
    disableCommand.endpoint = endpoint; disableCommand.targetIncarnation = arrivalCircuit.incarnation;
    disableCommand.loginGeneration = 21; disableCommand.operationId = 9191;
    auto orderedClose = arrival.disable(disableCommand, 9876);
    CHECK(orderedClose.status == LLTransportCircuitCommandStatus::Applied && orderedClose.lastPublishedAppSequence == 9876);
    CHECK(orderedClose.operationId == 9191 && orderedClose.loginGeneration == 21);
    CHECK(otherCircuit.status == State::Status::Ok);

    // Loss bookkeeping overflow never blocks liveness or fabricates complete
    // loss counts; the explicit partial state survives until a new incarnation.
    State incomplete;
    CHECK(incomplete.init({1, 1, 1, 1, 1, 1, 8388608}, charged));
    auto partialCircuit = incomplete.enable(endpoint, 22, false, 0, defaultWatch);
    CHECK(incomplete.observeValidatedArrival(endpoint, partialCircuit.incarnation, 22, 0, false, 1, 1).status == State::ArrivalStatus::Observed);
    CHECK(incomplete.observeValidatedArrival(endpoint, partialCircuit.incarnation, 22, 2, false, 2, 2).status == State::ArrivalStatus::Observed);
    auto partial = incomplete.observeValidatedArrival(endpoint, partialCircuit.incarnation, 22, 4, false, 3, 3);
    CHECK(partial.status == State::ArrivalStatus::PartialLoss);
    bool complete = true;
    CHECK(incomplete.lossTrackingComplete(endpoint, partialCircuit.incarnation, complete) == State::Status::Ok && !complete);
    CHECK(incomplete.expireLosses(endpoint, partialCircuit.incarnation, 10, 0) == State::Status::Partial);
    LLTransportCircuitSnapshot partialSnap{};
    complete = true;
    CHECK(incomplete.lookupWithLossTracking(endpoint, 3, partialSnap, complete) == State::Status::Ok && !complete);
    CHECK(partialSnap.incarnation == partialCircuit.incarnation);
    CHECK(partialSnap.packetsIn == 3 && partialSnap.bytesIn == 6 && partialSnap.lastPacketMonotonicUsec == 3);
    LLTransportCircuitSnapshot missingSnapshot{};
    complete = true;
    CHECK(incomplete.lookupWithLossTracking(other, 3, missingSnapshot, complete) == State::Status::Absent && !complete);
    auto partialRetained = incomplete.reserveObservedReceive(endpoint, partialCircuit.incarnation, 22, 4, false, true, 4, partial.observation);
    CHECK(partialRetained.status == State::ReceiveStatus::New);
    CHECK(incomplete.commitReceive(partialRetained.reservation, 3, 4) == State::Status::Ok);
    CHECK(incomplete.lookup(endpoint, 4).packetsIn == 3);

    // Same endpoint is closed and reopened: neither the old observation token
    // nor a delayed duplicate-ACK request can affect the new incarnation.
    State reconnect;
    CHECK(reconnect.init({1, 1, 2, 2, 1, 1, 8388608}, charged));
    auto previous = reconnect.enable(endpoint, 31, false, 0, defaultWatch);
    auto oldObservation = reconnect.observeValidatedArrival(endpoint, previous.incarnation, 31, 3, false, 4, 1);
    auto oldRetained = reconnect.reserveObservedReceive(endpoint, previous.incarnation, 31, 3, false, true, 1, oldObservation.observation);
    CHECK(oldRetained.status == State::ReceiveStatus::New && reconnect.commitReceive(oldRetained.reservation, 4, 1) == State::Status::Ok);
    CHECK(reconnect.observeValidatedArrival(endpoint, previous.incarnation, 31, 3, true, 4, 2).status == State::ArrivalStatus::Duplicate);
    CHECK(reconnect.disable(endpoint, previous.incarnation, 71).status == LLTransportCircuitCommandStatus::Applied);
    auto replacement = reconnect.enable(endpoint, 32, false, 3, defaultWatch);
    CHECK(replacement.status == State::Status::Ok && replacement.incarnation != previous.incarnation);
    CHECK(reconnect.classifyResentDuplicate(endpoint, previous.incarnation, 31, 3, true) == State::DuplicateClass::Invalid);
    CHECK(reconnect.classifyResentDuplicate(endpoint, replacement.incarnation, 32, 3, true) == State::DuplicateClass::NotDuplicate);
    auto newObservation = reconnect.observeValidatedArrival(endpoint, replacement.incarnation, 32, 3, true, 4, 4);
    CHECK(newObservation.status == State::ArrivalStatus::Observed);
    CHECK(reconnect.reserveObservedReceive(endpoint, previous.incarnation, 31, 3, false, true, 4, oldObservation.observation).status == State::ReceiveStatus::Invalid);
    CHECK(reconnect.reserveObservedReceive(endpoint, replacement.incarnation, 32, 3, true, true, 4, oldObservation.observation).status == State::ReceiveStatus::Invalid);
    CHECK(reconnect.acknowledgeCommittedDuplicate(endpoint, previous.incarnation, 31, 3, 4) == State::ReceiveStatus::Invalid);
    CHECK(reconnect.acknowledgeCommittedDuplicate(endpoint, replacement.incarnation, 32, 3, 4) == State::ReceiveStatus::Invalid);
    CHECK(reconnect.advance(4, 0).kind == State::ActionKind::None);
    auto newRetained = reconnect.reserveObservedReceive(endpoint, replacement.incarnation, 32, 3, true, true, 4, newObservation.observation);
    CHECK(newRetained.status == State::ReceiveStatus::New && reconnect.commitReceive(newRetained.reservation, 4, 4) == State::Status::Ok);
    CHECK(reconnect.acknowledgeCommittedDuplicate(endpoint, replacement.incarnation, 32, 3, 5) == State::ReceiveStatus::Duplicate);

    State pings;
    CHECK(pings.init({1, 1, 1, 1, 1, 1, 8388608}, charged));
    auto pingCircuit = pings.enable(endpoint, 12, false, 0, pingWatch);
    uint64_t due = 0;
    CHECK(pings.nextWatchdogUsec(endpoint, pingCircuit.incarnation, due) == State::Status::Ok && due == 1000000);
    CHECK(pings.pollWatchdog(endpoint, pingCircuit.incarnation, 999999, true, 0, 0.5f).status == State::WatchdogStatus::NotDue);
    CHECK(pings.pollWatchdog(endpoint, pingCircuit.incarnation, 1000000, true, 0, 0.5f).status == State::WatchdogStatus::PingReady);
    auto pingAction = pings.advance(1000000, 0);
    CHECK(pingAction.kind == State::ActionKind::Ping && pingAction.pingId == 1);
    uint32_t pingPacket = 0;
    CHECK(pings.assignActionPacketId(pingAction.token, pingPacket) == State::Status::Ok);
    CHECK(pings.defer(pingAction.token) == State::Status::Ok);
    pingAction = pings.advance(1000001, 0);
    CHECK(pingAction.kind == State::ActionKind::Ping && pingAction.packetAssigned && pingAction.packetId == pingPacket);
    CHECK(pings.commitAttempt(pingAction.token, 1000001, 0, true, 10) == State::Status::Ok);
    CHECK(pings.notePingReply(endpoint, pingCircuit.incarnation, 1, 1000000, 1100001) == State::Status::Ok);
    CHECK(pings.lookup(endpoint, 1100001).pingMilliseconds == 100);
    float average = 0;
    CHECK(pings.effectivePingMilliseconds(endpoint, pingCircuit.incarnation, 1000000, 1100001, average) == State::Status::Ok);
    CHECK(average > 819.9f && average < 820.1f);
    State::TimeoutEvent timeout;
    CHECK(pings.beginWatchdogTimeout(endpoint, pingCircuit.incarnation, 2100002, 1000000, false, 0, timeout) == State::Status::Full);
    CHECK(pings.lookup(endpoint, 2100002).alive);
    CHECK(pings.beginWatchdogTimeout(endpoint, pingCircuit.incarnation, 2100002, 1000000, true, 777, timeout) == State::Status::Ok);
    CHECK(timeout.lastPublishedAppSequence == 777);
    auto wrongBarrier = timeout; ++wrongBarrier.lastPublishedAppSequence;
    CHECK(pings.finishWatchdogTimeout(wrongBarrier, 0.5f) == State::Status::Stale);
    CHECK(!pings.lookup(endpoint, 2100002).alive);
    CHECK(pings.enable(endpoint, 12, false, 2100003, pingWatch).incarnation == pingCircuit.incarnation);
    CHECK(pings.finishWatchdogTimeout(timeout, 0.5f) == State::Status::Ok);
    CHECK(pings.lookup(endpoint, 2100003).alive);
    CHECK(pings.reserveReliableSlot(endpoint, pingCircuit.incarnation, 44) == State::Status::Ok);
    CHECK(pings.beginWatchdogTimeout(endpoint, pingCircuit.incarnation, 3100004, 1000000, true, 0, timeout) == State::Status::Ok);
    CHECK(pings.finishWatchdogTimeout(timeout, 0.5f) == State::Status::Ok);
    CHECK(pings.lookup(endpoint, 3100004).state == LLTransportSnapshotState::Absent);
    CHECK(!pings.takeCompletion(completion)); // reservation 44 was never accepted

    // One maintenance scan can settle multiple circuits without yielding an
    // action. The directory integration must see every affected incarnation.
    State dirtyState; CHECK(dirtyState.init({2,2,2,2,2,2,8388608},charged));
    const LLTransportEndpoint dirtyOther{0x0200007f,5678};
    const auto firstDirty=dirtyState.enable(endpoint,41,true,0,defaultWatch);
    const auto secondDirty=dirtyState.enable(dirtyOther,41,true,0,defaultWatch);
    CHECK(firstDirty.status==State::Status::Ok && secondDirty.status==State::Status::Ok);
    auto prepareFinal=[&](LLTransportEndpoint host,uint64_t incarnation,uint64_t token) {
        CHECK(dirtyState.reserveReliableSlot(host,incarnation,41,token)==State::Status::Ok);
        CHECK(dirtyState.acceptReliableSlot(token)==State::Status::Ok);
        uint32_t id=0; CHECK(dirtyState.allocateReliablePacketId(token,id)==State::Status::Ok);
        CHECK(dirtyState.prepareReliable(token,id,bytes,3,{0,1,1,false},0)==State::Status::Ok);
    };
    prepareFinal(endpoint,firstDirty.incarnation,301); prepareFinal(dirtyOther,secondDirty.incarnation,302);
    for (unsigned i=0;i<2;++i)
    {
        const auto initial=dirtyState.advance(0,0); CHECK(initial.kind==State::ActionKind::InitialSend);
        CHECK(dirtyState.commitAttempt(initial.token,0,1,true,3)==State::Status::Ok);
    }
    State::DirtyIncarnations dirty;
    CHECK(dirtyState.advance(2,0,dirty).kind==State::ActionKind::None);
    CHECK(!dirty.overflow && dirty.count==2);
    CHECK((dirty.values[0].incarnation==firstDirty.incarnation && dirty.values[1].incarnation==secondDirty.incarnation &&
           dirty.values[0].endpoint.addressNetworkOrder==endpoint.addressNetworkOrder && dirty.values[1].endpoint.addressNetworkOrder==dirtyOther.addressNetworkOrder) ||
          (dirty.values[1].incarnation==firstDirty.incarnation && dirty.values[0].incarnation==secondDirty.incarnation &&
           dirty.values[1].endpoint.addressNetworkOrder==endpoint.addressNetworkOrder && dirty.values[0].endpoint.addressNetworkOrder==dirtyOther.addressNetworkOrder));
    CHECK(dirtyState.takeCompletion(completion)); CHECK(dirtyState.takeCompletion(completion));
    CHECK(!dirtyState.takeCompletion(completion));
    CHECK(dirtyState.advance(3,0,dirty).kind==State::ActionKind::None && dirty.count==0 && !dirty.overflow);
    // Ping-based intervals belong to each selected circuit and are recomputed
    // after every resend, not supplied from whichever circuit ran previously.
    State pingRetry;
    CHECK(pingRetry.init({2,2,2,2,2,2,8388608},charged));
    const LLTransportEndpoint pingOther{0x0200007f,5678};
    const auto low=pingRetry.enable(endpoint,41,true,100,defaultWatch);
    const auto high=pingRetry.enable(pingOther,41,true,100,defaultWatch);
    CHECK(low.status==State::Status::Ok && high.status==State::Status::Ok);
    CHECK(pingRetry.notePingReply(endpoint,low.incarnation,0,1000000,100100)==State::Status::Ok);
    CHECK(pingRetry.notePingReply(pingOther,high.incarnation,0,1000000,1900100)==State::Status::Ok);
    auto preparePing=[&](LLTransportEndpoint host,uint64_t incarnation,uint64_t token) {
        CHECK(pingRetry.reserveReliableSlot(host,incarnation,41,token)==State::Status::Ok);
        CHECK(pingRetry.acceptReliableSlot(token)==State::Status::Ok);
        uint32_t id=0; CHECK(pingRetry.allocateReliablePacketId(token,id)==State::Status::Ok);
        const uint8_t packet[]={0x40,0,0,0,1,0,42};
        CHECK(pingRetry.prepareReliable(token,id,packet,sizeof(packet),{3,1,17,false,true},2000000)==State::Status::Ok);
    };
    preparePing(endpoint,low.incarnation,301); preparePing(pingOther,high.incarnation,302);
    for (unsigned i=0;i<2;++i) {
        const auto initialAction=pingRetry.advance(2000000,0);
        CHECK(initialAction.kind==State::ActionKind::InitialSend);
        int64_t unchanged=123; CHECK(pingRetry.selectedRetryInterval(initialAction.token,2000000,unchanged)==State::Status::Invalid && unchanged==123);
        CHECK(pingRetry.commitAttempt(initialAction.token,2000000,17,true,7)==State::Status::Ok);
    }
    for (unsigned i=0;i<2;++i) {
        const auto resendAction=pingRetry.advance(2000002,0);
        CHECK(resendAction.kind==State::ActionKind::Resend);
        int64_t selected=0; CHECK(pingRetry.selectedRetryInterval(resendAction.token,2000002,selected)==State::Status::Ok);
        CHECK(selected>1000000);
        CHECK(pingRetry.commitAttempt(resendAction.token,2000002,selected,true,7)==State::Status::Ok);
    }
    CHECK(pingRetry.advance(6000000,0).kind==State::ActionKind::None);
    CHECK(pingRetry.notePingReply(endpoint,low.incarnation,0,1000000,6199999)==State::Status::Ok);
    const auto lowAgain=pingRetry.advance(6200000,0);
    CHECK(lowAgain.kind==State::ActionKind::Resend && lowAgain.reliableToken==301);
    int64_t lowInterval=0; CHECK(pingRetry.selectedRetryInterval(lowAgain.token,6200000,lowInterval)==State::Status::Ok);
    CHECK(pingRetry.commitAttempt(lowAgain.token,6200000,lowInterval,true,7)==State::Status::Ok);
    const auto highAgain=pingRetry.advance(11600000,0);
    CHECK(highAgain.kind==State::ActionKind::Resend && highAgain.reliableToken==302);
    int64_t highInterval=0; CHECK(pingRetry.selectedRetryInterval(highAgain.token,11600000,highInterval)==State::Status::Ok);
    CHECK(pingRetry.commitAttempt(highAgain.token,11600000,highInterval,true,7)==State::Status::Ok);
    CHECK(pingRetry.advance(12000000,0).kind==State::ActionKind::None);
    const auto updatedLow=pingRetry.advance(16300000,0);
    CHECK(updatedLow.kind==State::ActionKind::Resend && updatedLow.reliableToken==301);
}
