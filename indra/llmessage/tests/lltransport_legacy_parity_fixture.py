#!/usr/bin/env python3
"""Generate a source-extracted legacy/state differential CPU fixture.

No compilation is performed. Invoke with an output path in the owned worktree.
The actual scheduler, reliable constructor, timeout predicate, and send-accounting
block are extracted from the checkout; clock, randomness, logging, socket and
unrelated callbacks are narrow fakes. The new state is linked separately.
"""
from pathlib import Path
import hashlib
import sys

here = Path(__file__).resolve()
root = here.parents[1]
circuit = (root / 'llcircuit.cpp').read_text()
packet = (root / 'llpacketack.cpp').read_text()
message = (root / 'message.cpp').read_text()
modular = (root.parent / 'llmath/llmodularmath.h').read_text()

def method(text, marker):
    start = text.index(marker)
    opening = text.index('{', start)
    depth = 0
    for index in range(opening, len(text)):
        if text[index] == '{':
            depth += 1
        elif text[index] == '}':
            depth -= 1
            if not depth:
                return text[start:index + 1]
    raise ValueError(marker)

scheduler = method(circuit, 'void LLCircuit::updateWatchDogTimers(')
watchdog = method(circuit, 'bool LLCircuitData::checkCircuitTimeout(')
duplicate = method(circuit, 'bool LLCircuitData::isDuplicateResend(')
packet_in = method(circuit, 'void LLCircuitData::checkPacketInID(')
set_packet_in = method(circuit, 'void LLCircuitData::setPacketInID(')
bytes_in = method(circuit, 'void LLCircuitData::addBytesIn(')
modular_subtract = method(modular, 'inline U32 subtract(')
observer_start = message.index('    if (cdp)\n    {\n        // update circuit packet ID tracking')
observer_end = message.index('\n\n    if(mVerboseLog)', observer_start)
observer = message[observer_start:observer_end]
constructor = method(packet, 'LLReliablePacket::LLReliablePacket(')
initial_start = circuit.index('    mLastPingSendTime = mt_sec + mHeartbeatInterval * distribution_offset;')
initial_end = circuit.index('\n', circuit.index('    mNextPingSendTime =', initial_start))
initial = circuit[initial_start:initial_end]
condition_start = circuit.index('if (now > packetp->mExpirationTime)')
condition = circuit[condition_start:condition_start + len('if (now > packetp->mExpirationTime)')]
ack_condition_start = circuit.index('if (packetp->mTimeout < F32Seconds(0.f))')
ack_condition = circuit[ack_condition_start:ack_condition_start + len('if (packetp->mTimeout < F32Seconds(0.f))')]
account_start = message.index('    if (!success)\n    {\n        mSendPacketFailureCount++;')
account_end = message.index('\n\n    if(mVerboseLog)', account_start)
account = message[account_start:account_end]

prologue = r'''
#include "lltransportcircuitstate.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <map>
#include <sstream>
using S32 = int32_t;
using S32Bytes = int32_t;
using U8 = uint8_t;
using U32 = uint32_t;
using F32 = float;
using TPACKETID = uint32_t;
using U64Microseconds = uint64_t;
constexpr TPACKETID LL_MAX_OUT_PACKET_ID = 0x01000000u;
template<class T> T llmax(T a, T b) { return std::max(a, b); }
using F64Seconds = double;
struct F32Seconds {
    double x;
    F32Seconds(double value = 0) : x(value) {}
    double value() const { return x; }
    operator double() const { return x; }
};
static double fakeNow = 0;
static float fakeDraw = 0;
static double totalTime() { return fakeNow; }
static float ll_frand(float maxValue) { return fakeDraw * maxValue; }
struct Logger { template<class T> Logger& operator<<(const T&) { return *this; } };
#define LL_INFOS(...) Logger()
#define LL_WARNS(...) Logger()
#define LL_DEBUGS(...) Logger()
#define LL_ENDL 0
struct LLMessageSystem {
    bool mVerboseLog = false;
    static F64Seconds getMessageTimeSeconds() { return fakeNow; }
    static U64Microseconds getMessageTimeUsecs() { return static_cast<uint64_t>(fakeNow * 1000000.); }
};
static LLMessageSystem fakeGlobalMessageSystem;
static LLMessageSystem* gMessageSystem = &fakeGlobalMessageSystem;
struct FakeThrottle { int adjustments = 0; void dynamicAdjust() { ++adjustments; } };
struct LLCircuitData {
    bool mbAlive = true, watchdogResult = true;
    double mNextPingSendTime = 0;
    int mHost = 0, checks = 0, periods = 0;
    U32 mHighestPacketID = 0, mPacketsIn = 0, mPacketsInID = 0, mWrapID = 0, mLastPacketGap = 0;
    S32Bytes mBytesIn = 0, mBytesInThisPeriod = 0;
    F64Seconds mLastPacketInTime = 0;
    std::map<TPACKETID, U64Microseconds> mPotentialLostPackets, mRecentlyReceivedReliablePackets;
    bool isDuplicateResend(TPACKETID);
    void checkPacketInID(TPACKETID, bool);
    void setPacketInID(TPACKETID);
    void addBytesIn(S32Bytes);
    double mLastPingReceivedTime = 0, mHeartbeatTimeout = 0;
    void (*mTimeoutCallback)(const int&, void*) = nullptr;
    void* mTimeoutUserData = nullptr;
    void setAlive(bool value) { mbAlive = value; }
    bool isAlive() const { return mbAlive; }
    bool checkCircuitTimeout();
    FakeThrottle mThrottles;
    bool updateWatchDogTimers(LLMessageSystem*) { ++checks; return watchdogResult; }
    void checkPeriodTime() { ++periods; }
};
struct CircuitOrder {
    bool operator()(const LLCircuitData* a, const LLCircuitData* b) const {
        return a->mNextPingSendTime == b->mNextPingSendTime ? a < b : a->mNextPingSendTime < b->mNextPingSendTime;
    }
};
struct LLCircuit {
    using ping_set_t = std::set<LLCircuitData*, CircuitOrder>;
    ping_set_t mPingSet;
    F32Seconds mHeartbeatInterval;
    int removals = 0;
    void removeCircuitData(int host) {
        for (auto it = mPingSet.begin(); it != mPingSet.end(); ++it)
            if ((*it)->mHost == host) { mPingSet.erase(it); ++removals; break; }
    }
    void updateWatchDogTimers(LLMessageSystem*);
};
struct LLReliablePacketParams {
    int mHost = 0, mRetries = 0;
    bool mPingBasedRetry = false;
    F32Seconds mTimeout;
    void (*mCallback)(void**,S32) = nullptr;
    void** mCallbackData = nullptr;
    char* mMessageName = nullptr;
};
constexpr int PHL_PACKET_ID = 0;
struct LLReliablePacket {
    int mSocket = 0, mHost = 0, mRetries = 0;
    bool mPingBasedRetry = false;
    F32Seconds mTimeout;
    void (*mCallback)(void**,S32) = nullptr;
    void** mCallbackData = nullptr;
    char* mMessageName = nullptr;
    U8* mBuffer;
    S32 mBufferLength;
    U32 mPacketID = 0;
    F64Seconds mExpirationTime = 0;
    LLReliablePacket(S32, U8*, S32, LLReliablePacketParams*);
    ~LLReliablePacket() { delete[] mBuffer; }
};
struct ByteCircuit { S32Bytes bytes = 0; void addBytesOut(S32Bytes value) { bytes += value; } };
struct ByteMessage {
    int mSendPacketFailureCount = 0;
    void apply(bool success, ByteCircuit* cdp, uint32_t buffer_length);
};
'''
observer_fn = '\nstruct LegacyObserver { U32 mCurrentRecvPacketID = 0; S32 mTrueReceiveSize = 0;\nvoid record(LLCircuitData* cdp, bool recv_resent) {\n' + observer + '\n}\n};\n'
modular_fn = '\nnamespace LLModularMath { template <U8 width>\n' + modular_subtract + '\n}\n'
initial_fn = '''\nstatic double legacyInitial(double now, double heartbeat, float offset, float jitter)\n{\n    F64Seconds mt_sec = now; F32Seconds mHeartbeatInterval(heartbeat);\n    float distribution_offset = offset; fakeDraw = jitter;\n    F64Seconds mLastPingSendTime = 0, mNextPingSendTime = 0;\n    [[maybe_unused]] F64Seconds mLastPingReceivedTime = 0;\n''' + initial + '\n    return mNextPingSendTime;\n}\n'
legacy_expired = '\nstatic bool legacyExpired(double now, const LLReliablePacket* packetp)\n{\n    ' + condition + ' return true;\n    return false;\n}\n'
ack_fn = '\nstatic bool legacyAckTimeout(const LLReliablePacket* packetp)\n{\n    ' + ack_condition + ' return true;\n    return false;\n}\n'
account_fn = '\nvoid ByteMessage::apply(bool success, ByteCircuit* cdp, uint32_t buffer_length)\n{\n' + account + '\n}\n'
tests = r'''
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n", __LINE__, #x); std::abort(); } } while(false)
static void reviveLegacy(const int&, void* context) { static_cast<LLCircuitData*>(context)->setAlive(true); }
using State = LLTransportCircuitState;
int main() {
    constexpr LLTransportEndpoint endpoint{0x0100007f, 1234};
    constexpr State::Limits limits{1, 1, 1, 1, 1, 1, 8388608};
    uint64_t charged = 0;
    constexpr float draws[] = {0.f, 0.25f, 0.5f, 0.75f, 0.999f};
    for (float offset : draws) for (float jitter : draws) {
        State state;
        CHECK(state.init(limits, charged));
        auto e = state.enable(endpoint, 1, false, 0, {1000000, 60000000, offset, jitter});
        CHECK(e.status == State::Status::Ok);
        uint64_t due = 0;
        CHECK(state.nextWatchdogUsec(endpoint, e.incarnation, due) == State::Status::Ok);
        const auto oldDue = static_cast<int64_t>(std::llround(legacyInitial(0., 1., offset, jitter) * 1000000.));
        CHECK(std::llabs(static_cast<int64_t>(due) - oldDue) <= 2);
        LLCircuit old;
        LLCircuitData legacyCircuit;
        old.mHeartbeatInterval = F32Seconds(1.);
        legacyCircuit.mNextPingSendTime = static_cast<double>(due) / 1000000.;
        old.mPingSet.insert(&legacyCircuit);
        fakeNow = static_cast<double>(due) / 1000000.; fakeDraw = jitter;
        LLMessageSystem messageSystem;
        old.updateWatchDogTimers(&messageSystem);
        CHECK(legacyCircuit.checks == 1 && legacyCircuit.mThrottles.adjustments == 1);
        auto outcome = state.pollWatchdog(endpoint, e.incarnation, due, true, 0, jitter);
        CHECK(outcome.status == State::WatchdogStatus::PingReady);
        uint64_t next = 0;
        CHECK(state.nextWatchdogUsec(endpoint, e.incarnation, next) == State::Status::Ok);
        const auto oldNext = static_cast<int64_t>(std::llround(legacyCircuit.mNextPingSendTime * 1000000.));
        CHECK(std::llabs(static_cast<int64_t>(next) - oldNext) <= 2);
    }
    alignas(U32) U8 bytes[8]{};
    LLReliablePacketParams parameters;
    parameters.mRetries = 0;
    parameters.mTimeout = F32Seconds(-0.5);
    fakeNow = 0;
    LLReliablePacket packet(3, bytes, 8, &parameters);
    CHECK(legacyExpired(0., &packet) && legacyAckTimeout(&packet));
    parameters.mTimeout = F32Seconds(0.);
    LLReliablePacket zeroPacket(3, bytes, 8, &parameters);
    CHECK(!legacyExpired(0., &zeroPacket) && !legacyAckTimeout(&zeroPacket));
    CHECK(legacyExpired(0.000001, &zeroPacket));
    State state;
    CHECK(state.init(limits, charged));
    auto e = state.enable(endpoint, 2, false, 0, {1000000, 60000000, 0.f, 0.5f});
    CHECK(state.reserveReliableSlot(endpoint, e.incarnation, 99) == State::Status::Ok);
    CHECK(state.acceptReliableSlot(99) == State::Status::Ok);
    uint32_t packetId = 0;
    CHECK(state.allocateReliablePacketId(99, packetId) == State::Status::Ok);
    CHECK(state.prepareReliable(99, packetId, bytes, 8, {0, -500000, -500000, true}, 0) == State::Status::Ok);
    auto action = state.advance(0, 0);
    CHECK(action.kind == State::ActionKind::InitialSend);
    CHECK(state.commitAttempt(action.token, 0, -500000, true, 1500) == State::Status::Ok);
    CHECK(state.advance(0, 0).kind == State::ActionKind::None);
    State::Completion completion;
    CHECK(state.takeCompletion(completion) && completion.result == -23016);
    ByteCircuit byteCircuit;
    ByteMessage byteMessage;
    byteMessage.apply(true, &byteCircuit, 1500);
    byteMessage.apply(false, &byteCircuit, 1500);
    byteMessage.apply(true, &byteCircuit, 20);
    CHECK(state.noteOrdinaryAttempt(endpoint, e.incarnation, false, 0) == State::Status::Ok);
    CHECK(state.noteOrdinaryAttempt(endpoint, e.incarnation, true, 20) == State::Status::Ok);
    auto rx = state.reserveReceive(endpoint, 1, false, true, 1);
    CHECK(state.commitReceive(rx.reservation, 1, 1) == State::Status::Ok);
    auto ack = state.advance(2, 0);
    CHECK(ack.kind == State::ActionKind::Ack);
    uint32_t controlId = 0;
    CHECK(state.assignActionPacketId(ack.token, controlId) == State::Status::Ok);
    CHECK(state.commitAttempt(ack.token, 2, 0, true, 30) == State::Status::Ok);
    byteMessage.apply(true, &byteCircuit, 30);
    CHECK(state.pollWatchdog(endpoint, e.incarnation, 1000000, true, 0, 0.5f).status == State::WatchdogStatus::PingReady);
    auto ping = state.advance(1000000, 0);
    CHECK(ping.kind == State::ActionKind::Ping);
    CHECK(state.assignActionPacketId(ping.token, controlId) == State::Status::Ok);
    CHECK(state.commitAttempt(ping.token, 1000000, 0, true, 40) == State::Status::Ok);
    byteMessage.apply(true, &byteCircuit, 40);
    uint64_t stateBytes = 0;
    CHECK(state.successfulBytesOut(endpoint, e.incarnation, stateBytes) == State::Status::Ok);
    CHECK(stateBytes == static_cast<uint64_t>(byteCircuit.bytes) && byteMessage.mSendPacketFailureCount == 1);

    // Compare the extracted constructor's strict negative deadline predicate
    // with both successful-ACK callback and expiration paths.
    State negativeAck;
    CHECK(negativeAck.init(limits, charged));
    auto n = negativeAck.enable(endpoint, 3, false, 0, {1000000, 60000000, 0.f, 0.5f});
    CHECK(negativeAck.reserveReliableSlot(endpoint, n.incarnation, 100) == State::Status::Ok);
    CHECK(negativeAck.acceptReliableSlot(100) == State::Status::Ok);
    uint32_t negativeId = 0;
    CHECK(negativeAck.allocateReliablePacketId(100, negativeId) == State::Status::Ok);
    CHECK(negativeAck.prepareReliable(100, negativeId, bytes, 8, {1, -500000, -500000, true}, 0) == State::Status::Ok);
    CHECK(negativeAck.consumeAck(endpoint, n.incarnation, negativeId) == State::Status::Ok);
    CHECK(negativeAck.takeCompletion(completion) && completion.result == -23016);
    State zeroDeadline;
    CHECK(zeroDeadline.init(limits, charged));
    auto z = zeroDeadline.enable(endpoint, 6, false, 0, {1000000, 60000000, 0.f, 0.5f});
    CHECK(zeroDeadline.reserveReliableSlot(endpoint, z.incarnation, 102) == State::Status::Ok);
    CHECK(zeroDeadline.acceptReliableSlot(102) == State::Status::Ok);
    CHECK(zeroDeadline.allocateReliablePacketId(102, negativeId) == State::Status::Ok);
    CHECK(zeroDeadline.prepareReliable(102, negativeId, bytes, 8, {0, 0, 0, false}, 0) == State::Status::Ok);
    auto zeroAction = zeroDeadline.advance(0, 0);
    CHECK(zeroDeadline.commitAttempt(zeroAction.token, 0, 0, false, 0) == State::Status::Ok);
    CHECK(zeroDeadline.advance(0, 0).kind == State::ActionKind::None && !zeroDeadline.takeCompletion(completion));
    CHECK(zeroDeadline.advance(1, 0).kind == State::ActionKind::None);
    CHECK(zeroDeadline.takeCompletion(completion) && completion.result == -23016);
    State minDeadline;
    CHECK(minDeadline.init(limits, charged));
    auto m = minDeadline.enable(endpoint, 4, false, 0, {1000000, 60000000, 0.f, 0.5f});
    CHECK(minDeadline.reserveReliableSlot(endpoint, m.incarnation, 101) == State::Status::Ok);
    CHECK(minDeadline.acceptReliableSlot(101) == State::Status::Ok);
    CHECK(minDeadline.allocateReliablePacketId(101, negativeId) == State::Status::Ok);
    CHECK(minDeadline.prepareReliable(101, negativeId, bytes, 8, {0, INT64_MIN, INT64_MIN, true}, 0) == State::Status::Ok);
    auto minAction = minDeadline.advance(0, 0);
    CHECK(minDeadline.commitAttempt(minAction.token, 0, 0, false, 0) == State::Status::Ok);
    CHECK(minDeadline.advance(0, 0).kind == State::ActionKind::None);
    CHECK(minDeadline.takeCompletion(completion) && completion.result == -23016);

    // Actual legacy logValidMsg circuit block and actual packet/byte helpers:
    // validation has succeeded, but no APP payload is retained in this case.
    State pressure;
    CHECK(pressure.init({1, 1, 1, 1, 1, 4, 8388608}, charged));
    auto pc = pressure.enable(endpoint, 7, false, 0, {1000000, 60000000, 0.f, 0.5f});
    LLCircuitData oldArrival;
    LegacyObserver observer;
    fakeNow = 0.000001; observer.mCurrentRecvPacketID = 1; observer.mTrueReceiveSize = 10;
    observer.record(&oldArrival, false);
    CHECK(pressure.observeValidatedArrival(endpoint, pc.incarnation, 7, 1, false, 10, 1).status == State::ArrivalStatus::Observed);
    fakeNow = 0.000002; observer.mCurrentRecvPacketID = 2; observer.mTrueReceiveSize = 12;
    observer.record(&oldArrival, true);
    const auto pressureObservation = pressure.observeValidatedArrival(endpoint, pc.incarnation, 7, 2, true, 12, 2);
    CHECK(pressureObservation.status == State::ArrivalStatus::Observed);
    const auto pressureSnapshot = pressure.lookup(endpoint, 2);
    CHECK(pressureSnapshot.incarnation == pc.incarnation && pressureSnapshot.packetsIn == oldArrival.mPacketsIn);
    CHECK(pressureSnapshot.bytesIn == oldArrival.mBytesIn && pressureSnapshot.lastPacketMonotonicUsec == 2);
    CHECK(std::abs(static_cast<double>(pressureSnapshot.lastPacketMonotonicUsec) / 1000000. - oldArrival.mLastPacketInTime) < 1e-12);
    CHECK(pressure.advance(2, 0).kind == State::ActionKind::None); // no ACK for dropped payload
    CHECK(!oldArrival.isDuplicateResend(2));
    CHECK(pressure.classifyResentDuplicate(endpoint, pc.incarnation, 7, 2, true) == State::DuplicateClass::NotDuplicate);
    const auto pressureAdmission = pressure.reserveObservedReceive(endpoint, pc.incarnation, 7, 2, true, true, 2, pressureObservation.observation);
    CHECK(pressureAdmission.status == State::ReceiveStatus::New && pressure.commitReceive(pressureAdmission.reservation, 12, 2) == State::Status::Ok);
    oldArrival.mRecentlyReceivedReliablePackets[2] = 2; // old path inserts after successful readMessage
    CHECK(oldArrival.isDuplicateResend(2));
    CHECK(pressure.classifyResentDuplicate(endpoint, pc.incarnation, 7, 2, true) == State::DuplicateClass::Duplicate);
    CHECK(pressure.classifyResentDuplicate(endpoint, pc.incarnation, 7, 2, false) == State::DuplicateClass::NotDuplicate);

    // Actual extracted legacy scheduler removes a timed-out circuit before a
    // ping; new state requires event capacity and preserves callback revival.
    LLCircuit oldTimeout;
    LLCircuitData expiredCircuit;
    oldTimeout.mHeartbeatInterval = F32Seconds(1.);
    expiredCircuit.mNextPingSendTime = 1.; expiredCircuit.watchdogResult = false;
    oldTimeout.mPingSet.insert(&expiredCircuit); fakeNow = 1.; fakeDraw = 0.5f;
    LLMessageSystem fakeMessage;
    oldTimeout.updateWatchDogTimers(&fakeMessage);
    CHECK(expiredCircuit.checks == 1 && oldTimeout.removals == 1 && expiredCircuit.mThrottles.adjustments == 0);
    LLCircuitData legacyTimeout;
    fakeNow = 0.; legacyTimeout.mHeartbeatTimeout = 0.;
    CHECK(legacyTimeout.checkCircuitTimeout() && legacyTimeout.isAlive());
    fakeNow = 1.;
    CHECK(!legacyTimeout.checkCircuitTimeout() && !legacyTimeout.isAlive());
    LLCircuitData legacyRevival;
    legacyRevival.mTimeoutCallback = reviveLegacy;
    legacyRevival.mTimeoutUserData = &legacyRevival;
    CHECK(legacyRevival.checkCircuitTimeout() && legacyRevival.isAlive());
    State watchdog;
    CHECK(watchdog.init(limits, charged));
    auto w = watchdog.enable(endpoint, 5, false, 0, {1000000, 0, 0.f, 0.5f});
    CHECK(watchdog.pollWatchdog(endpoint, w.incarnation, 1000000, false, 0, 0.5f).status == State::WatchdogStatus::Full);
    CHECK(watchdog.lookup(endpoint, 1000000).alive);
    auto expired = watchdog.pollWatchdog(endpoint, w.incarnation, 1000000, true, 0, 0.5f);
    CHECK(expired.status == State::WatchdogStatus::Timeout);
    CHECK(watchdog.advance(1000000, 0).kind == State::ActionKind::None);
    CHECK(watchdog.enable(endpoint, 5, false, 1000001, {1000000, 0, 0.f, 0.5f}).incarnation == w.incarnation);
    CHECK(watchdog.finishWatchdogTimeout(expired.event, 0.5f) == State::Status::Ok);
    CHECK(watchdog.advance(1000001, 0).kind == State::ActionKind::Ping);
}
'''
if len(sys.argv) != 2:
    raise SystemExit(__doc__)
out = Path(sys.argv[1])
out.write_text(prologue + modular_fn + '\n' + scheduler + '\n' + watchdog + '\n' + duplicate + '\n' + set_packet_in + '\n' + packet_in + '\n' + bytes_in + observer_fn + '\n' + constructor + initial_fn + legacy_expired + ack_fn + account_fn + tests)
for name, data in [('llcircuit.cpp', circuit), ('llpacketack.cpp', packet), ('message.cpp', message), ('llmodularmath.h', modular)]:
    print(name, hashlib.sha256(data.encode()).hexdigest())
print('generated', hashlib.sha256(out.read_bytes()).hexdigest())
