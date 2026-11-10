#include "lltransportcircuitstate.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace
{
bool same(LLTransportEndpoint a, LLTransportEndpoint b) noexcept
{ return a.addressNetworkOrder == b.addressNetworkOrder && a.portHostOrder == b.portHostOrder; }
bool valid(LLTransportEndpoint a) noexcept { return a.addressNetworkOrder && a.portHostOrder; }
constexpr uint32_t PacketSpace = 0x01000000u;
constexpr int32_t CircuitGone = -23017, Timeout = -23016;
uint64_t next(uint64_t& value) noexcept
{ if (value == std::numeric_limits<uint64_t>::max()) return 0; return ++value; }
uint64_t plus(uint64_t a, uint64_t b) noexcept
{ return b > UINT64_MAX - a ? UINT64_MAX : a + b; }
uint64_t watchdogDelay(uint64_t heartbeat, float offsetDraw, float jitterDraw) noexcept
{
    // Legacy uses F32 heartbeat arithmetic and F64 absolute message time.
    const float seconds = static_cast<float>(static_cast<double>(heartbeat) / 1000000.0);
    const float offset = seconds * offsetDraw;
    const float period = 0.95f * seconds + jitterDraw * (0.1f * seconds);
    const double microseconds = (static_cast<double>(offset) + static_cast<double>(period)) * 1000000.0;
    if (microseconds >= static_cast<double>(UINT64_MAX)) return UINT64_MAX;
    // An integer clock must not fire earlier than the legacy fractional time.
    return static_cast<uint64_t>(std::ceil(microseconds));
}
}

struct LLTransportCircuitState::Impl
{
    struct Circuit
    {
        LLTransportCircuitSnapshot snapshot{};
        uint64_t login = 0, lastPing = 0, lastPingReceived = 0, timeoutEvent = 0, successfulOutBytes = 0, ackFirstAge = 0;
        uint64_t heartbeatUsec = 0, timeoutUsec = 0, nextWatchdog = 0, timeoutCheckTime = 0, timeoutAppSequence = 0;
        bool watchdogConfigured = false, pingArmed = false;
        bool lossTrackingComplete = true, observationAvailable = false, observationResent = false;
        uint64_t observationSequence = 0;
        uint32_t observationPacket = 0;
        uint32_t highest = 0, lastOut = 0, expected = 0, wrap = 0, pingsInTransit = 0;
        uint8_t lastPingId = 0, nextAction = 0;
        float averagePingMs = 1000.f;
        Action deferredAck{}, deferredPing{};
    };
    struct Receive
    {
        uint64_t id = 0, incarnation = 0;
        uint32_t packet = 0, remember = UINT32_MAX, ack = UINT32_MAX;
        bool reliable = false, resent = false, alreadyObserved = false;
        uint32_t reservedLosses = 0;
    };
    struct Loss { uint64_t incarnation = 0, reservation = 0, time = 0; uint32_t packet = 0; };
    struct Remembered { uint64_t incarnation = 0, reserved = 0, time = 0; uint32_t packet = 0; bool committed = false; };
    struct Ack { uint64_t incarnation = 0, reserved = 0, time = 0, serial = 0; uint32_t packet = 0; bool ready = false; };
    enum class TxState : uint8_t { Free, Reserved, Admitted, Initial, Unacked, Final, Complete };
    struct Tx
    {
        TxState state = TxState::Free;
        LLTransportEndpoint endpoint{};
        uint64_t incarnation = 0, login = 0, token = 0, deadline = 0;
        int64_t firstInterval = 0, interval = 0;
        uint32_t packet = 0, size = 0, retries = 0;
        int32_t result = 0;
        bool successIsTimeout = false, packetAssigned = false, deadlineBeforeEpoch = false, pingBasedRetry = false;
        std::array<uint8_t, MaxPacketBytes> bytes{};
    };
    Limits limits{};
    std::unique_ptr<Circuit[]> circuits;
    std::unique_ptr<Receive[]> receives;
    std::unique_ptr<Remembered[]> remembered;
    std::unique_ptr<Ack[]> acks;
    std::unique_ptr<Tx[]> reliable;
    std::unique_ptr<Loss[]> losses;
    uint64_t sequence = 0, incarnation = 0, reservation = 0, ackSerial = 0, actionSerial = 0, timeoutSerial = 0;
    Action outstanding{};
    Piggyback piggyback{};
    uint64_t piggybackSerial = 0;
    std::array<uint64_t, 250> piggybackAckSerials{};
    uint32_t cursor = 0;
    bool shuttingDown = false;

    Circuit* find(LLTransportEndpoint endpoint) noexcept
    {
        for (uint32_t i = 0; i < limits.circuits; ++i)
            if (circuits[i].snapshot.state == LLTransportSnapshotState::Present && same(circuits[i].snapshot.endpoint, endpoint)) return &circuits[i];
        return nullptr;
    }
    const Circuit* find(LLTransportEndpoint endpoint) const noexcept
    { return const_cast<Impl*>(this)->find(endpoint); }
    Circuit* find(uint64_t id) noexcept
    {
        for (uint32_t i = 0; i < limits.circuits; ++i)
            if (circuits[i].snapshot.state == LLTransportSnapshotState::Present && circuits[i].snapshot.incarnation == id) return &circuits[i];
        return nullptr;
    }
    static void setDeadline(Tx& tx, uint64_t now, int64_t interval) noexcept
    {
        tx.deadlineBeforeEpoch = false;
        if (interval >= 0) { tx.deadline = plus(now, static_cast<uint64_t>(interval)); return; }
        const uint64_t magnitude = static_cast<uint64_t>(-(interval + 1)) + 1;
        if (magnitude > now) { tx.deadline = 0; tx.deadlineBeforeEpoch = true; }
        else tx.deadline = now - magnitude;
    }
    static bool expired(const Tx& tx, uint64_t now) noexcept
    { return tx.deadlineBeforeEpoch || now > tx.deadline; }
    bool publish(Circuit& circuit) noexcept
    {
        const auto id = next(sequence);
        if (!id) return false;
        circuit.snapshot.publicationSequence = id;
        return true;
    }
};

LLTransportCircuitState::LLTransportCircuitState() noexcept = default;
LLTransportCircuitState::~LLTransportCircuitState() = default;

namespace
{
bool checkedAddProduct(uint64_t& total, uint64_t count, uint64_t size) noexcept
{
    if (size && count > UINT64_MAX / size) return false;
    const uint64_t part = count * size;
    if (part > UINT64_MAX - total || part > SIZE_MAX || total + part > SIZE_MAX) return false;
    total += part;
    return true;
}
}

bool LLTransportCircuitState::estimateBytes(const Limits& limits, uint64_t& bytes) noexcept
{
    bytes = 0;
    if (!limits.byteBudget || !limits.circuits || limits.circuits > 1024 || !limits.receives || limits.receives > 4096 ||
        !limits.remembered || limits.remembered > 65536 || !limits.acks || limits.acks > 65536 ||
        !limits.reliable || limits.reliable > 4096 || !limits.losses || limits.losses > 65536) return false;
    uint64_t total = sizeof(Impl);
    if (!checkedAddProduct(total, limits.circuits, sizeof(Impl::Circuit)) ||
        !checkedAddProduct(total, limits.receives, sizeof(Impl::Receive)) ||
        !checkedAddProduct(total, limits.remembered, sizeof(Impl::Remembered)) ||
        !checkedAddProduct(total, limits.acks, sizeof(Impl::Ack)) ||
        !checkedAddProduct(total, limits.reliable, sizeof(Impl::Tx)) ||
        !checkedAddProduct(total, limits.losses, sizeof(Impl::Loss)) || total > limits.byteBudget) return false;
    bytes = total;
    return true;
}

bool LLTransportCircuitState::init(const Limits& limits, uint64_t& charged) noexcept
{ return initWithFault(limits, charged, UINT32_MAX); }

#if defined(LL_TRANSPORT_STATE_TESTING)
bool LLTransportCircuitState::testInit(const Limits& limits, uint64_t& charged, uint32_t failBefore) noexcept
{ return initWithFault(limits, charged, failBefore); }
bool LLTransportCircuitState::testCheckedAddProduct(uint64_t& total, uint64_t count, uint64_t size) noexcept
{ return checkedAddProduct(total, count, size); }
bool LLTransportCircuitState::testSetLastOut(LLTransportEndpoint endpoint, uint64_t incarnation, uint32_t value) noexcept
{
    if (!mImpl || value>=PacketSpace) return false;
    auto* circuit=mImpl->find(endpoint);
    if (!circuit || circuit->snapshot.incarnation!=incarnation) return false;
    circuit->lastOut=value;
    return true;
}
bool LLTransportCircuitState::testSetSequence(uint64_t value) noexcept
{
    if (!mImpl) return false;
    mImpl->sequence=value;
    return true;
}
#endif

bool LLTransportCircuitState::initWithFault(const Limits& limits, uint64_t& charged, uint32_t failBefore) noexcept
{
    charged = 0;
    mAllocationCalls = 0;
    uint64_t needed = 0;
    if (mImpl || !estimateBytes(limits, needed)) return false;
    const auto allocationAllowed = [&]() noexcept { return mAllocationCalls++ != failBefore; };
    if (!allocationAllowed()) return false;
    std::unique_ptr<Impl> value(new (std::nothrow) Impl);
    if (!value) return false;
    value->limits = limits;
    if (!allocationAllowed()) return false;
    value->circuits.reset(new (std::nothrow) Impl::Circuit[limits.circuits]);
    if (!value->circuits || !allocationAllowed()) return false;
    value->receives.reset(new (std::nothrow) Impl::Receive[limits.receives]);
    if (!value->receives || !allocationAllowed()) return false;
    value->remembered.reset(new (std::nothrow) Impl::Remembered[limits.remembered]);
    if (!value->remembered || !allocationAllowed()) return false;
    value->acks.reset(new (std::nothrow) Impl::Ack[limits.acks]);
    if (!value->acks || !allocationAllowed()) return false;
    value->reliable.reset(new (std::nothrow) Impl::Tx[limits.reliable]);
    if (!value->reliable || !allocationAllowed()) return false;
    value->losses.reset(new (std::nothrow) Impl::Loss[limits.losses]);
    if (!value->losses) return false;
    mImpl = std::move(value);
    charged = needed;
    return true;
}

LLTransportCircuitState::EnableResult LLTransportCircuitState::enable(LLTransportEndpoint endpoint, uint64_t login, bool trusted,
    uint64_t now, WatchdogConfig watchdog) noexcept
{
    if (!mImpl || mImpl->shuttingDown || !valid(endpoint) || !login || !watchdog.heartbeatUsec ||
        !std::isfinite(watchdog.initialOffsetDraw) || !std::isfinite(watchdog.initialJitterDraw) ||
        watchdog.initialOffsetDraw < 0.f || watchdog.initialOffsetDraw >= 1.f ||
        watchdog.initialJitterDraw < 0.f || watchdog.initialJitterDraw >= 1.f) return {Status::Invalid, 0};
    auto& impl = *mImpl;
    if (auto* old = impl.find(endpoint))
    {
        if (old->login != login) return {Status::Stale, 0};
        if (!old->watchdogConfigured || old->heartbeatUsec != watchdog.heartbeatUsec || old->timeoutUsec != watchdog.timeoutUsec)
            return {Status::Invalid, 0};
        uint64_t pending = 0;
        for (uint32_t i = 0; i < impl.limits.receives; ++i) if (impl.receives[i].id) ++pending;
        if (UINT64_MAX - impl.sequence <= pending) return {Status::Full, 0};
        if (!impl.publish(*old)) return {Status::Full, 0};
        if (!old->snapshot.alive) { old->lastOut = 0; old->expected = 0; }
        old->snapshot.alive = true; old->snapshot.trusted = trusted; old->snapshot.blocked = false;
        old->pingsInTransit = 0; old->lastPingReceived = now; old->snapshot.ownerMonotonicUsec = now;
        return {Status::Ok, old->snapshot.incarnation};
    }
    for (uint32_t i = 0; i < impl.limits.circuits; ++i)
    {
        auto& circuit = impl.circuits[i];
        if (circuit.snapshot.state == LLTransportSnapshotState::Present) continue;
        uint64_t pending = 0;
        for (uint32_t j = 0; j < impl.limits.receives; ++j) if (impl.receives[j].id) ++pending;
        if (impl.incarnation == UINT64_MAX || UINT64_MAX - impl.sequence <= pending) return {Status::Full, 0};
        circuit = {};
        circuit.login = login; circuit.lastPing = circuit.lastPingReceived = now;
        auto& value = circuit.snapshot;
        value.endpoint = endpoint; value.state = LLTransportSnapshotState::Present;
        value.incarnation = next(impl.incarnation); value.publicationSequence = next(impl.sequence);
        value.ownerMonotonicUsec = value.birthMonotonicUsec = now;
        value.alive = true; value.trusted = trusted; value.pingMilliseconds = 1000;
        const auto status = configureWatchdog(endpoint, value.incarnation, watchdog.heartbeatUsec,
                                              watchdog.timeoutUsec, now, watchdog.initialOffsetDraw,
                                              watchdog.initialJitterDraw);
        if (status != Status::Ok) { circuit = {}; return {status, 0}; }
        return {Status::Ok, value.incarnation};
    }
    return {Status::Full, 0};
}

LLTransportCircuitSnapshot LLTransportCircuitState::lookup(LLTransportEndpoint endpoint, uint64_t now) const noexcept
{
    LLTransportCircuitSnapshot result{};
    result.endpoint = endpoint;
    if (!mImpl || !valid(endpoint)) return result;
    const auto* circuit = mImpl->find(endpoint);
    if (!circuit) { result.state = LLTransportSnapshotState::Absent; return result; }
    result = circuit->snapshot;
    result.ownerMonotonicUsec = now;
    return result;
}

LLTransportCircuitState::Status LLTransportCircuitState::lookupWithLossTracking(LLTransportEndpoint endpoint, uint64_t now,
    LLTransportCircuitSnapshot& snapshot, bool& complete) const noexcept
{
    complete = false;
    snapshot = lookup(endpoint, now);
    if (snapshot.state == LLTransportSnapshotState::Unavailable) return Status::Invalid;
    if (snapshot.state == LLTransportSnapshotState::Absent) return Status::Absent;
    const auto* circuit = mImpl->find(endpoint);
    if (!circuit || circuit->snapshot.incarnation != snapshot.incarnation) return Status::Stale;
    complete = circuit->lossTrackingComplete;
    return Status::Ok;
}

LLTransportCircuitState::ReceiveResult LLTransportCircuitState::reserveReceive(LLTransportEndpoint endpoint, uint32_t packet, bool resent, bool reliable, uint64_t now) noexcept
{ return reserveReceiveImpl(endpoint, packet, resent, reliable, now, false); }

LLTransportCircuitState::ReceiveResult LLTransportCircuitState::reserveReceiveImpl(LLTransportEndpoint endpoint, uint32_t packet, bool resent, bool reliable, uint64_t now, bool alreadyObserved) noexcept
{
    if (!mImpl || packet >= PacketSpace) return {};
    auto& impl = *mImpl;
    auto* circuit = impl.find(endpoint);
    if (!circuit || !circuit->snapshot.alive) return {};
    const uint64_t incarnation = circuit->snapshot.incarnation;
    // Packet-order bookkeeping is serial per circuit; a second admission waits
    // for commit/cancel so its loss-capacity preflight cannot become stale.
    for (uint32_t i = 0; i < impl.limits.receives; ++i)
        if (impl.receives[i].incarnation == incarnation) return {ReceiveStatus::Full, 0};
    Impl::Remembered* match = nullptr;
    if (reliable || resent)
        for (uint32_t i = 0; i < impl.limits.remembered; ++i)
        {
            auto& item = impl.remembered[i];
            if (item.incarnation == incarnation && item.packet == packet) { match = &item; break; }
        }
    if (match && match->reserved) return {ReceiveStatus::Full, 0};
    if (resent && match && match->committed)
    {
        if (reliable)
        {
            for (uint32_t i = 0; i < impl.limits.acks; ++i)
                if (!impl.acks[i].incarnation)
                {
                    uint64_t pendingReliable = 0;
                    for (uint32_t j = 0; j < impl.limits.receives; ++j)
                        if (impl.receives[j].id && impl.receives[j].reliable) ++pendingReliable;
                    if (UINT64_MAX - impl.ackSerial <= pendingReliable) return {ReceiveStatus::Full, 0};
                    const auto serial = next(impl.ackSerial);
                    impl.acks[i] = {incarnation, 0, now, serial, packet, true};
                    if (!circuit->ackFirstAge && now >= circuit->snapshot.birthMonotonicUsec)
                        circuit->ackFirstAge = now - circuit->snapshot.birthMonotonicUsec;
                    return {ReceiveStatus::Duplicate, 0};
                }
            return {ReceiveStatus::Full, 0};
        }
        return {ReceiveStatus::Duplicate, 0};
    }
    uint32_t rec = UINT32_MAX, ack = UINT32_MAX, mem = UINT32_MAX;
    for (uint32_t i = 0; i < impl.limits.receives; ++i) if (!impl.receives[i].id) { rec = i; break; }
    if (reliable)
    {
        for (uint32_t i = 0; i < impl.limits.acks; ++i) if (!impl.acks[i].incarnation) { ack = i; break; }
        if (match) mem = static_cast<uint32_t>(match - impl.remembered.get());
        else for (uint32_t i = 0; i < impl.limits.remembered; ++i) if (!impl.remembered[i].incarnation) { mem = i; break; }
    }
    uint32_t lossCount = 0;
    if (!alreadyObserved && circuit->lossTrackingComplete && circuit->snapshot.packetsIn &&
        packet != circuit->expected && packet >= circuit->wrap && !resent)
    {
        bool recovered = false;
        for (uint32_t i = 0; i < impl.limits.losses; ++i)
            if (impl.losses[i].incarnation == incarnation && !impl.losses[i].reservation && impl.losses[i].packet == packet) recovered = true;
        const uint32_t distance = (packet - circuit->expected) & (PacketSpace - 1);
        if (!recovered && distance < 16)
        {
            uint32_t missing = circuit->expected;
            for (uint32_t step = 0; step < distance; ++step)
            {
                bool exists = false;
                for (uint32_t i = 0; i < impl.limits.losses; ++i)
                    if (impl.losses[i].incarnation == incarnation && !impl.losses[i].reservation && impl.losses[i].packet == missing)
                    { exists = true; break; }
                if (!exists) ++lossCount;
                missing = (missing + 1) & (PacketSpace - 1);
            }
        }
    }
    uint32_t lossFree = 0;
    for (uint32_t i = 0; i < impl.limits.losses; ++i) if (!impl.losses[i].incarnation) ++lossFree;
    // Reserve publication and ACK sequence space before accepting a decode.
    uint64_t pending = 0, pendingReliable = 0;
    for (uint32_t i = 0; i < impl.limits.receives; ++i)
        if (impl.receives[i].id) { ++pending; if (impl.receives[i].reliable) ++pendingReliable; }
    if (rec == UINT32_MAX || (reliable && (ack == UINT32_MAX || mem == UINT32_MAX)) ||
        impl.reservation == UINT64_MAX || lossFree < lossCount ||
        (!alreadyObserved && UINT64_MAX - impl.sequence <= pending) || (reliable && UINT64_MAX - impl.ackSerial <= pendingReliable))
        return {ReceiveStatus::Full, 0};
    const uint64_t id = next(impl.reservation);
    impl.receives[rec] = {id, incarnation, packet, mem, ack, reliable, resent, alreadyObserved, lossCount};
    for (uint32_t i = 0, remaining = lossCount; i < impl.limits.losses && remaining; ++i)
        if (!impl.losses[i].incarnation) { impl.losses[i].incarnation = incarnation; impl.losses[i].reservation = id; --remaining; }
    if (reliable)
    {
        auto& entry = impl.remembered[mem];
        if (!entry.incarnation) { entry.incarnation = incarnation; entry.packet = packet; }
        entry.reserved = id;
        impl.acks[ack] = {incarnation, id, 0, 0, packet, false};
    }
    return {ReceiveStatus::New, id};
}

void LLTransportCircuitState::cancelReceive(uint64_t id) noexcept
{
    if (!mImpl || !id) return;
    auto& impl = *mImpl;
    for (uint32_t i = 0; i < impl.limits.receives; ++i)
    {
        auto& receive = impl.receives[i];
        if (receive.id != id) continue;
        if (receive.reliable)
        {
            auto& entry = impl.remembered[receive.remember];
            if (entry.reserved == id) { entry.reserved = 0; if (!entry.committed) entry = {}; }
            impl.acks[receive.ack] = {};
        }
        for (uint32_t j = 0; j < impl.limits.losses; ++j)
            if (impl.losses[j].reservation == id) impl.losses[j] = {};
        receive = {};
        return;
    }
}

LLTransportCircuitState::Status LLTransportCircuitState::commitReceive(uint64_t id, uint32_t bytes, uint64_t now) noexcept
{
    if (!mImpl || !id) return Status::Invalid;
    auto& impl = *mImpl;
    for (uint32_t i = 0; i < impl.limits.receives; ++i)
    {
        auto& receive = impl.receives[i];
        if (receive.id != id) continue;
        auto* circuit = impl.find(receive.incarnation);
        if (!circuit || !circuit->snapshot.alive) { cancelReceive(id); return Status::Stale; }
        if ((!receive.alreadyObserved && impl.sequence == UINT64_MAX) || (receive.reliable && impl.ackSerial == UINT64_MAX)) return Status::Full;
        if (receive.reliable)
        {
            auto& entry = impl.remembered[receive.remember];
            entry.committed = true; entry.reserved = 0; entry.time = now;
            auto& ack = impl.acks[receive.ack];
            ack.reserved = 0; ack.time = now; ack.serial = next(impl.ackSerial); ack.ready = true;
            if (!circuit->ackFirstAge && now >= circuit->snapshot.birthMonotonicUsec)
                circuit->ackFirstAge = now - circuit->snapshot.birthMonotonicUsec;
        }
        if (receive.alreadyObserved) { receive = {}; return Status::Ok; }
        auto& snap = circuit->snapshot;
        const uint32_t packet = receive.packet;
        if (!circuit->lossTrackingComplete) { /* Explicitly unavailable after capacity loss. */ }
        else if (!snap.packetsIn)
        {
            circuit->expected = (packet + 1) & (PacketSpace - 1);
            circuit->wrap = circuit->expected;
        }
        else if (circuit->expected == packet)
            circuit->expected = (circuit->expected + 1) & (PacketSpace - 1);
        else if (packet >= circuit->wrap)
        {
            bool recovered = false;
            for (uint32_t j = 0; j < impl.limits.losses; ++j)
                if (impl.losses[j].incarnation == receive.incarnation && !impl.losses[j].reservation && impl.losses[j].packet == packet)
                { impl.losses[j] = {}; recovered = true; break; }
            if (!recovered && !receive.resent)
            {
                uint32_t missing = circuit->expected;
                const uint32_t distance = (packet - missing) & (PacketSpace - 1);
                if (distance < 16)
                    for (uint32_t step = 0; step < distance; ++step)
                    {
                        Impl::Loss* slot = nullptr;
                        for (uint32_t j = 0; j < impl.limits.losses; ++j)
                            if (impl.losses[j].incarnation == receive.incarnation && !impl.losses[j].reservation && impl.losses[j].packet == missing)
                            { slot = &impl.losses[j]; break; }
                        if (!slot)
                            for (uint32_t j = 0; j < impl.limits.losses; ++j)
                                if (impl.losses[j].reservation == id) { slot = &impl.losses[j]; break; }
                        // Capacity and packet order were reserved at admission.
                        if (slot) { slot->reservation = 0; slot->packet = missing; slot->time = now; }
                        missing = (missing + 1) & (PacketSpace - 1);
                    }
                circuit->expected = (packet + 1) & (PacketSpace - 1);
            }
        }
        ++snap.packetsIn;
        const auto sum = static_cast<int64_t>(snap.bytesIn) + bytes;
        snap.bytesIn = static_cast<int32_t>(std::min<int64_t>(sum, INT32_MAX));
        snap.lastPacketMonotonicUsec = snap.ownerMonotonicUsec = now;
        snap.hasLastPacket = true;
        const int64_t delta = static_cast<int64_t>(circuit->highest) - packet;
        if (delta > PacketSpace / 2 || (delta >= -static_cast<int64_t>(PacketSpace / 2) && packet > circuit->highest)) circuit->highest = packet;
        impl.publish(*circuit);
        receive = {};
        return Status::Ok;
    }
    return Status::Stale;
}

LLTransportCircuitState::DuplicateClass LLTransportCircuitState::classifyResentDuplicate(LLTransportEndpoint endpoint,
    uint64_t incarnation, uint64_t login, uint32_t packet, bool resent) const noexcept
{
    if (!mImpl || packet >= PacketSpace) return DuplicateClass::Invalid;
    const auto& impl = *mImpl;
    const auto* circuit = impl.find(endpoint);
    if (!circuit || !circuit->snapshot.alive || circuit->snapshot.incarnation != incarnation || circuit->login != login)
        return DuplicateClass::Invalid;
    for (uint32_t i = 0; i < impl.limits.receives; ++i)
        if (impl.receives[i].incarnation == incarnation) return DuplicateClass::Busy;
    if (!resent) return DuplicateClass::NotDuplicate;
    for (uint32_t i = 0; i < impl.limits.remembered; ++i)
        if (impl.remembered[i].incarnation == incarnation && impl.remembered[i].packet == packet && impl.remembered[i].committed)
            return DuplicateClass::Duplicate;
    return DuplicateClass::NotDuplicate;
}

LLTransportCircuitState::ArrivalResult LLTransportCircuitState::observeValidatedArrival(LLTransportEndpoint endpoint,
    uint64_t incarnation, uint64_t login, uint32_t packet, bool resent, uint32_t bytes, uint64_t now) noexcept
{
    if (!mImpl || packet >= PacketSpace) return {};
    auto& impl = *mImpl;
    auto* circuit = impl.find(endpoint);
    if (!circuit || !circuit->snapshot.alive || circuit->snapshot.incarnation != incarnation || circuit->login != login) return {};
    for (uint32_t i = 0; i < impl.limits.receives; ++i)
        if (impl.receives[i].incarnation == incarnation) return {ArrivalStatus::Busy, 0};
    // A token belongs only to the immediately observed packet on this circuit.
    circuit->observationAvailable = false;
    // This path must never acknowledge a payload it did not retain.
    const auto reservation = reserveReceiveImpl(endpoint, packet, resent, false, now, false);
    if (reservation.status == ReceiveStatus::Duplicate) return {ArrivalStatus::Duplicate, 0};
    if (reservation.status == ReceiveStatus::Invalid) return {};
    ArrivalStatus result = ArrivalStatus::Observed;
    if (reservation.status == ReceiveStatus::New)
    {
        if (commitReceive(reservation.reservation, bytes, now) != Status::Ok)
        { cancelReceive(reservation.reservation); return {ArrivalStatus::Unavailable, 0}; }
        if (!circuit->lossTrackingComplete) result = ArrivalStatus::PartialLoss;
    }
    else
    {
        uint64_t pending = 0;
        for (uint32_t i = 0; i < impl.limits.receives; ++i) if (impl.receives[i].id) ++pending;
        if (UINT64_MAX - impl.sequence <= pending) return {ArrivalStatus::Unavailable, 0};
        // Once loss-bookkeeping capacity is insufficient, stop publishing it as
        // complete and discard pending gaps rather than invent lost counts.
        circuit->lossTrackingComplete = false;
        for (uint32_t i = 0; i < impl.limits.losses; ++i)
            if (impl.losses[i].incarnation == incarnation) impl.losses[i] = {};
        auto& snap = circuit->snapshot;
        ++snap.packetsIn;
        const auto sum = static_cast<int64_t>(snap.bytesIn) + bytes;
        snap.bytesIn = static_cast<int32_t>(std::min<int64_t>(sum, INT32_MAX));
        snap.lastPacketMonotonicUsec = snap.ownerMonotonicUsec = now;
        snap.hasLastPacket = true;
        const int64_t delta = static_cast<int64_t>(circuit->highest) - packet;
        if (delta > PacketSpace / 2 || (delta >= -static_cast<int64_t>(PacketSpace / 2) && packet > circuit->highest)) circuit->highest = packet;
        impl.publish(*circuit);
        result = ArrivalStatus::PartialLoss;
    }
    circuit->observationSequence = circuit->snapshot.publicationSequence;
    circuit->observationPacket = packet; circuit->observationResent = resent;
    circuit->observationAvailable = true;
    return {result, circuit->observationSequence};
}

LLTransportCircuitState::Status LLTransportCircuitState::lossTrackingComplete(LLTransportEndpoint endpoint,
    uint64_t incarnation, bool& complete) const noexcept
{
    if (!mImpl) return Status::Invalid;
    const auto* circuit = mImpl->find(endpoint);
    if (!circuit) return Status::Absent;
    if (circuit->snapshot.incarnation != incarnation) return Status::Stale;
    complete = circuit->lossTrackingComplete;
    return Status::Ok;
}

LLTransportCircuitState::ReceiveResult LLTransportCircuitState::reserveObservedReceive(LLTransportEndpoint endpoint,
    uint64_t incarnation, uint64_t login, uint32_t packet, bool resent, bool reliable, uint64_t now, uint64_t observation) noexcept
{
    if (!mImpl || !observation) return {};
    auto* circuit = mImpl->find(endpoint);
    if (!circuit || circuit->snapshot.incarnation != incarnation || circuit->login != login ||
        !circuit->observationAvailable || circuit->observationSequence != observation ||
        circuit->observationPacket != packet || circuit->observationResent != resent) return {};
    const auto result = reserveReceiveImpl(endpoint, packet, resent, reliable, now, true);
    if (result.status == ReceiveStatus::New) circuit->observationAvailable = false;
    return result;
}

LLTransportCircuitState::ReceiveStatus LLTransportCircuitState::acknowledgeCommittedDuplicate(LLTransportEndpoint endpoint,
    uint64_t incarnation, uint64_t login, uint32_t packet, uint64_t now) noexcept
{
    if (!mImpl || packet >= PacketSpace) return ReceiveStatus::Invalid;
    auto& impl = *mImpl;
    const auto* circuit = impl.find(endpoint);
    if (!circuit || !circuit->snapshot.alive || circuit->snapshot.incarnation != incarnation || circuit->login != login)
        return ReceiveStatus::Invalid;
    bool committed = false;
    for (uint32_t i = 0; i < impl.limits.remembered; ++i)
        if (impl.remembered[i].incarnation == incarnation && impl.remembered[i].packet == packet && impl.remembered[i].committed)
        { committed = true; break; }
    if (!committed) return ReceiveStatus::Invalid;
    return reserveReceiveImpl(endpoint, packet, true, true, now, false).status;
}

LLTransportCircuitState::ReceiveStatus LLTransportCircuitState::preflightReliableControl(
    LLTransportEndpoint endpoint, uint64_t incarnation, uint64_t login, uint32_t packet, bool resent) const noexcept
{
    if (!mImpl || packet>=PacketSpace) return ReceiveStatus::Invalid;
    const auto& impl=*mImpl;
    const auto* circuit=impl.find(endpoint);
    if (!circuit || !circuit->snapshot.alive || circuit->snapshot.incarnation!=incarnation || circuit->login!=login)
        return ReceiveStatus::Invalid;
    uint64_t pending=0,pendingReliable=0;
    bool receiveFree=false, ackFree=false, rememberedFree=false;
    for (uint32_t i=0;i<impl.limits.receives;++i)
    {
        const auto& item=impl.receives[i];
        if (item.id && item.incarnation==incarnation) return ReceiveStatus::Full;
        if (!item.id) receiveFree=true;
        else { ++pending; if (item.reliable) ++pendingReliable; }
    }
    for (uint32_t i=0;i<impl.limits.acks;++i) if (!impl.acks[i].incarnation) { ackFree=true; break; }
    const Impl::Remembered* match=nullptr;
    for (uint32_t i=0;i<impl.limits.remembered;++i)
    {
        const auto& item=impl.remembered[i];
        if (!item.incarnation) rememberedFree=true;
        if (item.incarnation==incarnation && item.packet==packet) match=&item;
    }
    if (match && match->reserved) return ReceiveStatus::Full;
    if (resent && match && match->committed)
        return (!ackFree || UINT64_MAX-impl.ackSerial<=pendingReliable) ? ReceiveStatus::DuplicateFull : ReceiveStatus::Duplicate;
    // Observation publishes once; CompletePing can publish again while its
    // receive reservation is held. Preserve the existing pending-reservation
    // headroom for both operations before any trailer can mutate an ACK target.
    const uint64_t sequenceSpace=UINT64_MAX-impl.sequence;
    if (sequenceSpace<=pending || sequenceSpace-pending<=2 ||
        !ackFree || UINT64_MAX-impl.ackSerial<=pendingReliable)
        return ReceiveStatus::Full;
    // Arrival bookkeeping can consume one reservation before the actual
    // reliable-control reservation. Reserve both IDs before trailer effects.
    if (!receiveFree || (!match && !rememberedFree) || impl.reservation>=UINT64_MAX-1)
        return ReceiveStatus::Full;
    return ReceiveStatus::New;
}

LLTransportCircuitState::Status LLTransportCircuitState::forgetReceivedBefore(LLTransportEndpoint endpoint, uint64_t incarnation, uint32_t oldest, uint64_t now) noexcept
{
    // StartPing's OldestUnacked is an unrestricted U32 in the legacy
    // clearDuplicateList comparison, even though locally generated IDs wrap.
    if (!mImpl) return Status::Invalid;
    auto& impl = *mImpl;
    const auto* circuit = impl.find(endpoint);
    if (!circuit) return Status::Absent;
    if (circuit->snapshot.incarnation != incarnation) return Status::Stale;
    for (uint32_t i = 0; i < impl.limits.remembered; ++i)
    {
        auto& entry = impl.remembered[i];
        if (entry.incarnation != incarnation || !entry.committed || entry.reserved) continue;
        if ((oldest < circuit->highest && entry.packet < oldest) ||
            (entry.packet > circuit->highest && now >= entry.time && now - entry.time > 60000000)) entry = {};
    }
    return Status::Ok;
}

LLTransportCircuitCompletion LLTransportCircuitState::conditionalClose(const LLTransportCircuitCommand& command, uint64_t ownerAppSequence) noexcept
{
    LLTransportCircuitCompletion result{};
    result.endpoint = command.endpoint; result.operationId = command.operationId;
    result.loginGeneration = command.loginGeneration; result.circuitIncarnation = command.targetIncarnation;
    if (!mImpl) { result.status = LLTransportCircuitCommandStatus::Unavailable; return result; }
    auto& impl = *mImpl;
    auto* circuit = impl.find(command.endpoint);
    LLTransportCircuitSnapshot absent{};
    absent.endpoint = command.endpoint; absent.state = LLTransportSnapshotState::Absent;
    result.status = llTransportCheckConditionalClose(command, circuit ? circuit->snapshot : absent);
    if (result.status != LLTransportCircuitCommandStatus::Applied) return result;
    if (circuit->login != command.loginGeneration) { result.status = LLTransportCircuitCommandStatus::IncarnationChanged; return result; }
    const auto incarnation = circuit->snapshot.incarnation;
    result.lastPublishedAppSequence = ownerAppSequence;
    if (impl.outstanding.incarnation == incarnation) impl.outstanding = {};
    if (impl.piggyback.incarnation == incarnation) impl.piggyback = {};
    for (uint32_t i = 0; i < impl.limits.receives; ++i)
        if (impl.receives[i].incarnation == incarnation) cancelReceive(impl.receives[i].id);
    for (uint32_t i = 0; i < impl.limits.remembered; ++i)
        if (impl.remembered[i].incarnation == incarnation) impl.remembered[i] = {};
    for (uint32_t i = 0; i < impl.limits.acks; ++i)
        if (impl.acks[i].incarnation == incarnation) impl.acks[i] = {};
    for (uint32_t i = 0; i < impl.limits.losses; ++i)
        if (impl.losses[i].incarnation == incarnation) impl.losses[i] = {};
    for (uint32_t i = 0; i < impl.limits.reliable; ++i)
    {
        auto& tx = impl.reliable[i];
        if (tx.incarnation != incarnation) continue;
        if (tx.state == Impl::TxState::Reserved) tx = {}; // never accepted
        else if (tx.state != Impl::TxState::Free && tx.state != Impl::TxState::Complete)
        { tx.state = Impl::TxState::Complete; tx.result = CircuitGone; }
    }
    *circuit = {};
    return result;
}

LLTransportCircuitCompletion LLTransportCircuitState::disable(const LLTransportCircuitCommand& command, uint64_t ownerAppSequence) noexcept
{
    LLTransportCircuitCompletion result{};
    result.endpoint = command.endpoint; result.operationId = command.operationId;
    result.loginGeneration = command.loginGeneration; result.circuitIncarnation = command.targetIncarnation;
    if (command.kind != LLTransportCircuitCommandKind::Disable || !valid(command.endpoint) || !command.targetIncarnation)
    { result.status = LLTransportCircuitCommandStatus::Invalid; return result; }
    if (!mImpl) { result.status = LLTransportCircuitCommandStatus::Unavailable; return result; }
    auto* circuit = mImpl->find(command.endpoint);
    if (!circuit) { result.status = LLTransportCircuitCommandStatus::Absent; return result; }
    if (circuit->snapshot.incarnation != command.targetIncarnation || circuit->login != command.loginGeneration)
    { result.status = LLTransportCircuitCommandStatus::IncarnationChanged; return result; }
    auto guarded = command;
    guarded.kind = LLTransportCircuitCommandKind::DisableIfUnchanged;
    guarded.observedPublicationSequence = circuit->snapshot.publicationSequence;
    guarded.observedHasLastPacket = circuit->snapshot.hasLastPacket;
    guarded.observedLastPacketMonotonicUsec = circuit->snapshot.lastPacketMonotonicUsec;
    return conditionalClose(guarded, ownerAppSequence);
}

LLTransportCircuitCompletion LLTransportCircuitState::disable(LLTransportEndpoint endpoint, uint64_t incarnation, uint64_t ownerAppSequence) noexcept
{
    LLTransportCircuitCompletion result{};
    result.endpoint = endpoint; result.circuitIncarnation = incarnation;
    if (!mImpl) { result.status = LLTransportCircuitCommandStatus::Unavailable; return result; }
    auto* circuit = mImpl->find(endpoint);
    if (!circuit) { result.status = LLTransportCircuitCommandStatus::Absent; return result; }
    if (circuit->snapshot.incarnation != incarnation)
    { result.status = LLTransportCircuitCommandStatus::IncarnationChanged; return result; }
    LLTransportCircuitCommand command{};
    command.kind = LLTransportCircuitCommandKind::DisableIfUnchanged;
    command.endpoint = endpoint; command.targetIncarnation = incarnation; command.loginGeneration = circuit->login;
    command.observedPublicationSequence = circuit->snapshot.publicationSequence;
    command.observedHasLastPacket = circuit->snapshot.hasLastPacket;
    command.observedLastPacketMonotonicUsec = circuit->snapshot.lastPacketMonotonicUsec;
    return conditionalClose(command, ownerAppSequence);
}

void LLTransportCircuitState::shutdown() noexcept
{
    if (!mImpl) return;
    mImpl->shuttingDown = true;
    for (uint32_t i = 0; i < mImpl->limits.circuits; ++i)
    {
        const auto snapshot = mImpl->circuits[i].snapshot;
        if (snapshot.state == LLTransportSnapshotState::Present) (void)disable(snapshot.endpoint, snapshot.incarnation, 0);
    }
}

LLTransportCircuitState::Status LLTransportCircuitState::reserveReliableSlot(LLTransportEndpoint endpoint, uint64_t incarnation, uint64_t token) noexcept
{
    if (!mImpl || !token) return Status::Invalid;
    auto& impl = *mImpl;
    auto* circuit = impl.find(endpoint);
    if (!circuit) return Status::Absent;
    if (circuit->snapshot.incarnation != incarnation) return Status::Stale;
    if (!circuit->snapshot.alive) return Status::Invalid;
    Impl::Tx* vacant = nullptr;
    for (uint32_t i = 0; i < impl.limits.reliable; ++i)
    {
        auto& tx = impl.reliable[i];
        if (tx.state == Impl::TxState::Free) { if (!vacant) vacant = &tx; continue; }
        if (tx.token == token) return Status::Busy;
    }
    if (!vacant) return Status::Full;
    vacant->state = Impl::TxState::Reserved; vacant->endpoint = endpoint;
    vacant->incarnation = incarnation; vacant->login = circuit->login; vacant->token = token;
    return Status::Ok;
}

LLTransportCircuitState::Status LLTransportCircuitState::reserveReliableSlot(LLTransportEndpoint endpoint,
    uint64_t incarnation, uint64_t loginGeneration, uint64_t token) noexcept
{
    if (!mImpl || !loginGeneration || !token) return Status::Invalid;
    const auto* circuit=mImpl->find(endpoint);
    if (!circuit) return Status::Absent;
    if (circuit->snapshot.incarnation!=incarnation || circuit->login!=loginGeneration) return Status::Stale;
    return reserveReliableSlot(endpoint,incarnation,token);
}

LLTransportCircuitState::Status LLTransportCircuitState::acceptReliableSlot(uint64_t token) noexcept
{
    if (!mImpl || !token) return Status::Invalid;
    for (uint32_t i = 0; i < mImpl->limits.reliable; ++i)
    {
        auto& tx = mImpl->reliable[i];
        if (tx.token != token || tx.state != Impl::TxState::Reserved) continue;
        auto* circuit = mImpl->find(tx.incarnation);
        if (!circuit || !circuit->snapshot.alive) return Status::Stale;
        tx.state = Impl::TxState::Admitted;
        return Status::Ok;
    }
    return Status::Stale;
}

LLTransportCircuitState::Status LLTransportCircuitState::cancelReliableSlot(uint64_t token) noexcept
{
    if (!mImpl || !token) return Status::Invalid;
    for (uint32_t i = 0; i < mImpl->limits.reliable; ++i)
    {
        auto& tx = mImpl->reliable[i];
        if (tx.token != token || tx.state != Impl::TxState::Reserved) continue;
        if (tx.packetAssigned) return Status::Busy;
        tx = {};
        return Status::Ok;
    }
    return Status::Stale;
}

LLTransportCircuitState::Status LLTransportCircuitState::allocatePacketId(LLTransportEndpoint endpoint, uint64_t incarnation, uint32_t& packet) noexcept
{
    if (!mImpl) return Status::Invalid;
    auto* circuit = mImpl->find(endpoint);
    if (!circuit) return Status::Absent;
    if (circuit->snapshot.incarnation != incarnation) return Status::Stale;
    if (!circuit->snapshot.alive) return Status::Invalid;
    // An outbound ID wrap resets receive wrap bookkeeping in the legacy code.
    // Do not invalidate an in-flight receive's gap-capacity reservation.
    for (uint32_t i = 0; i < mImpl->limits.receives; ++i)
        if (mImpl->receives[i].incarnation == incarnation) return Status::Busy;
    packet = (circuit->lastOut + 1) & (PacketSpace - 1);
    if (packet < circuit->lastOut) circuit->wrap = 0;
    circuit->lastOut = packet;
    ++circuit->snapshot.packetsOut;
    return Status::Ok;
}

LLTransportCircuitState::Status LLTransportCircuitState::allocateReliablePacketId(uint64_t token, uint32_t& packet) noexcept
{
    if (!mImpl || !token) return Status::Invalid;
    for (uint32_t i = 0; i < mImpl->limits.reliable; ++i)
    {
        auto& tx = mImpl->reliable[i];
        if (tx.token != token || tx.state != Impl::TxState::Admitted) continue;
        if (tx.packetAssigned) return Status::Busy;
        const auto status = allocatePacketId(tx.endpoint, tx.incarnation, packet);
        if (status == Status::Ok) { tx.packet = packet; tx.packetAssigned = true; }
        return status;
    }
    return Status::Stale;
}

LLTransportCircuitState::Status LLTransportCircuitState::prepareReliable(uint64_t token, uint32_t packet,
    const uint8_t* bytes, uint32_t size, RetryPolicy policy, uint64_t now) noexcept
{
    if (!mImpl || !token || !bytes || !size || size > MaxPacketBytes || packet >= PacketSpace) return Status::Invalid;
    auto& impl = *mImpl;
    Impl::Tx* target = nullptr;
    for (uint32_t i = 0; i < impl.limits.reliable; ++i)
        if (impl.reliable[i].token == token && impl.reliable[i].state == Impl::TxState::Admitted) { target = &impl.reliable[i]; break; }
    if (!target) return Status::Stale;
    if (!target->packetAssigned || target->packet != packet) return Status::Invalid;
    for (uint32_t i = 0; i < impl.limits.reliable; ++i)
    {
        const auto& tx = impl.reliable[i];
        if (&tx != target && tx.incarnation == target->incarnation && tx.packet == packet &&
            tx.state != Impl::TxState::Free && tx.state != Impl::TxState::Reserved && tx.state != Impl::TxState::Admitted && tx.state != Impl::TxState::Complete) return Status::Busy;
    }
    int64_t initialInterval=policy.firstIntervalUsec;
    if (policy.pingBasedInitial)
    {
        const auto* circuit=impl.find(target->incarnation);
        if (!circuit) return Status::Stale;
        float pingMilliseconds=0.f;
        const auto status=effectivePingMilliseconds(target->endpoint,target->incarnation,
                                                      circuit->heartbeatUsec,now,pingMilliseconds);
        if (status!=Status::Ok) return status;
        // Both legacy reliable and semireliable initial expressions use
        // max(1 second, 5 * averaged ping), with F32 arithmetic.
        const float seconds=std::max(1.f,(5.f*pingMilliseconds)/1000.f);
        initialInterval=static_cast<int64_t>(std::ceil(static_cast<double>(seconds)*1000000.0));
    }
    target->state = Impl::TxState::Initial; target->packet = packet;
    target->size = size; target->retries = policy.retries;
    target->firstInterval = initialInterval; target->interval = policy.retryIntervalUsec;
    // LLReliablePacket starts its initial deadline when registered, before send.
    Impl::setDeadline(*target, now, initialInterval);
    target->successIsTimeout = policy.successIsTimeout;
    target->pingBasedRetry = policy.pingBasedRetry;
    std::copy_n(bytes, size, target->bytes.begin());
    return Status::Ok;
}

LLTransportCircuitState::Status LLTransportCircuitState::failReliable(uint64_t token, int32_t status) noexcept
{
    if (!mImpl || !token || status == 0) return Status::Invalid;
    for (uint32_t i = 0; i < mImpl->limits.reliable; ++i)
    {
        auto& tx = mImpl->reliable[i];
        if (tx.token != token || tx.state != Impl::TxState::Admitted) continue;
        tx.state = Impl::TxState::Complete; tx.result = status;
        return Status::Ok;
    }
    return Status::Stale;
}

uint32_t LLTransportCircuitState::oldestUnacked(LLTransportEndpoint endpoint, uint64_t incarnation) const noexcept
{
    if (!mImpl) return 0;
    const auto* circuit = mImpl->find(endpoint);
    if (!circuit || circuit->snapshot.incarnation != incarnation) return 0;
    const uint32_t lastOut = circuit->lastOut;
    uint32_t after = UINT32_MAX, wrapped = UINT32_MAX;
    for (uint32_t i = 0; i < mImpl->limits.reliable; ++i)
    {
        const auto& tx = mImpl->reliable[i];
        // Legacy registers reliable packets before their initial send attempt.
        if (tx.incarnation != incarnation || (tx.state != Impl::TxState::Initial &&
            tx.state != Impl::TxState::Unacked && tx.state != Impl::TxState::Final)) continue;
        if (tx.packet > lastOut) after = std::min(after, tx.packet);
        else wrapped = std::min(wrapped, tx.packet);
    }
    return after != UINT32_MAX ? after : (wrapped != UINT32_MAX ? wrapped : lastOut);
}

LLTransportCircuitState::Action LLTransportCircuitState::advance(uint64_t now, uint64_t collect) noexcept
{ return advanceImpl(now,collect,nullptr); }

LLTransportCircuitState::Action LLTransportCircuitState::advance(uint64_t now, uint64_t collect,
    DirtyIncarnations& dirty) noexcept
{
    dirty.count=0; dirty.overflow=false;
    return advanceImpl(now,collect,&dirty);
}

LLTransportCircuitState::Action LLTransportCircuitState::advanceImpl(uint64_t now, uint64_t collect,
    DirtyIncarnations* dirty) noexcept
{
    if (!mImpl) return {};
    auto& impl = *mImpl;
    if (impl.outstanding.token || impl.actionSerial == UINT64_MAX) return {};
    collect = std::min<uint64_t>(collect, 2000000);
    for (uint32_t step = 0; step < impl.limits.circuits; ++step)
    {
        const auto index = (impl.cursor + step) % impl.limits.circuits;
        auto& circuit = impl.circuits[index];
        if (circuit.snapshot.state != LLTransportSnapshotState::Present || !circuit.snapshot.alive) continue;
        const auto incarnation = circuit.snapshot.incarnation;
        Impl::Tx* chosen = nullptr;
        bool completed=false;
        for (uint32_t i = 0; i < impl.limits.reliable; ++i)
        {
            auto& tx = impl.reliable[i];
            if (tx.incarnation != incarnation) continue;
            if (tx.state == Impl::TxState::Final && Impl::expired(tx, now))
            { tx.state = Impl::TxState::Complete; tx.result = Timeout; completed=true; continue; }
            if (tx.state != Impl::TxState::Initial && !(tx.state == Impl::TxState::Unacked && Impl::expired(tx, now))) continue;
            if (!chosen || tx.packet < chosen->packet) chosen = &tx;
        }
        if (completed && dirty)
        {
            if (dirty->count<dirty->values.size()) dirty->values[dirty->count++]={circuit.snapshot.endpoint,incarnation};
            else dirty->overflow=true;
        }
        Action candidates[3]{};
        for (auto& candidate : candidates)
        { candidate.endpoint = circuit.snapshot.endpoint; candidate.incarnation = incarnation; }
        if (chosen)
        {
            auto& data = candidates[0];
            data.kind = chosen->state == Impl::TxState::Initial ? ActionKind::InitialSend : ActionKind::Resend;
            data.packetId = chosen->packet; data.reliableToken = chosen->token; data.size = chosen->size;
            std::copy_n(chosen->bytes.begin(), chosen->size, data.bytes.begin());
            if (data.kind == ActionKind::Resend) data.bytes[0] |= 0x20;
        }
        auto& acknowledgements = candidates[1];
        if (circuit.deferredAck.kind == ActionKind::Ack) acknowledgements = circuit.deferredAck;
        uint64_t serial = 0;
        for (uint32_t j = 0; j < MaxAckBatch && acknowledgements.kind == ActionKind::None; ++j)
        {
            const Impl::Ack* best = nullptr;
            for (uint32_t i = 0; i < impl.limits.acks; ++i)
            {
                const auto& ack = impl.acks[i];
                if (!ack.ready || ack.incarnation != incarnation || ack.serial <= serial) continue;
                if (!best || ack.serial < best->serial) best = &ack;
            }
            if (!best) break;
            if (!j && (now < circuit.snapshot.birthMonotonicUsec ||
                now - circuit.snapshot.birthMonotonicUsec < circuit.ackFirstAge ||
                now - circuit.snapshot.birthMonotonicUsec - circuit.ackFirstAge <= collect)) break;
            acknowledgements.ackIds[acknowledgements.ackCount++] = best->packet;
            serial = best->serial;
        }
        if (acknowledgements.ackCount && impl.piggyback.incarnation != incarnation)
            acknowledgements.kind = ActionKind::Ack;
        auto& pingAction = candidates[2];
        if (circuit.deferredPing.kind == ActionKind::Ping) pingAction = circuit.deferredPing;
        if (pingAction.kind == ActionKind::None && circuit.pingArmed)
        {
            pingAction.kind = ActionKind::Ping;
            pingAction.pingId = static_cast<uint8_t>(circuit.lastPingId + 1);
            pingAction.oldestUnacked = oldestUnacked(pingAction.endpoint, incarnation);
        }
        Action action{};
        for (uint32_t phase = 0; phase < 3; ++phase)
        {
            const uint32_t chosenPhase = (circuit.nextAction + phase) % 3;
            if (candidates[chosenPhase].kind == ActionKind::None) continue;
            action = candidates[chosenPhase];
            circuit.nextAction = static_cast<uint8_t>((chosenPhase + 1) % 3);
            break;
        }
        if (action.kind == ActionKind::None) continue;
        action.token = next(impl.actionSerial);
        impl.outstanding = action;
        impl.cursor = (index + 1) % impl.limits.circuits;
        return action;
    }
    return {};
}

LLTransportCircuitState::Status LLTransportCircuitState::selectedRetryInterval(uint64_t token,
    uint64_t now, int64_t& out) const noexcept
{
    if (!mImpl || !token) return Status::Invalid;
    const auto& action=mImpl->outstanding;
    if (action.token!=token) return Status::Stale;
    if (action.kind!=ActionKind::Resend) return Status::Invalid;
    const auto* circuit=mImpl->find(action.incarnation);
    if (!circuit) return Status::Stale;
    for (uint32_t i=0;i<mImpl->limits.reliable;++i)
    {
        const auto& tx=mImpl->reliable[i];
        if (tx.incarnation!=action.incarnation || tx.token!=action.reliableToken ||
            tx.packet!=action.packetId || tx.state!=Impl::TxState::Unacked) continue;
        int64_t interval=tx.interval;
        if (tx.pingBasedRetry)
        {
            float pingMilliseconds=0.f;
            const auto status=effectivePingMilliseconds(action.endpoint,action.incarnation,
                circuit->heartbeatUsec,now,pingMilliseconds);
            if (status!=Status::Ok) return status;
            // Actual legacy expression uses F32 seconds: max(1 s, 5 *
            // getPingDelayAveraged()). Do not fire early after rounding.
            const float seconds=std::max(1.f,(5.f*pingMilliseconds)/1000.f);
            interval=static_cast<int64_t>(std::ceil(static_cast<double>(seconds)*1000000.0));
        }
        out=interval;
        return Status::Ok;
    }
    return Status::Stale;
}

LLTransportCircuitState::Status LLTransportCircuitState::defer(uint64_t token) noexcept
{
    if (!mImpl || !token || mImpl->outstanding.token != token) return Status::Stale;
    auto* circuit = mImpl->find(mImpl->outstanding.incarnation);
    if (circuit && mImpl->outstanding.kind == ActionKind::Ack)
    { circuit->deferredAck = mImpl->outstanding; circuit->deferredAck.token = 0; }
    if (circuit && mImpl->outstanding.kind == ActionKind::Ping)
    { circuit->deferredPing = mImpl->outstanding; circuit->deferredPing.token = 0; }
    if (mImpl->piggyback.incarnation == mImpl->outstanding.incarnation) mImpl->piggyback = {};
    mImpl->outstanding = {};
    return Status::Ok;
}

LLTransportCircuitState::Status LLTransportCircuitState::assignActionPacketId(uint64_t token, uint32_t& packet) noexcept
{
    if (!mImpl || !token || mImpl->outstanding.token != token) return Status::Stale;
    auto& action = mImpl->outstanding;
    if (action.kind != ActionKind::Ack && action.kind != ActionKind::Ping) return Status::Invalid;
    if (!action.packetAssigned)
    {
        const auto status = allocatePacketId(action.endpoint, action.incarnation, packet);
        if (status != Status::Ok) return status;
        action.packetId = packet; action.packetAssigned = true;
    }
    packet = action.packetId;
    return Status::Ok;
}

LLTransportCircuitState::Status LLTransportCircuitState::commitAttempt(uint64_t token, uint64_t now, int64_t interval, bool sent, uint32_t wireBytes) noexcept
{
    if (!mImpl || !token || mImpl->outstanding.token != token) return Status::Stale;
    if (!sent && wireBytes) return Status::Invalid;
    auto& impl = *mImpl;
    const auto action = impl.outstanding;
    if (impl.piggyback.incarnation == action.incarnation) return Status::Busy;
    auto* circuit = impl.find(action.incarnation);
    if (!circuit) { impl.outstanding = {}; return Status::Stale; }
    if (action.kind == ActionKind::InitialSend || action.kind == ActionKind::Resend)
    {
        for (uint32_t i = 0; i < impl.limits.reliable; ++i)
        {
            auto& tx = impl.reliable[i];
            if (tx.incarnation != action.incarnation || tx.token != action.reliableToken || !tx.packetAssigned ||
                tx.packet != action.packetId) continue;
            if (action.kind == ActionKind::InitialSend && tx.state == Impl::TxState::Initial)
            {
                tx.state = tx.retries ? Impl::TxState::Unacked : Impl::TxState::Final;
            }
            else if (action.kind == ActionKind::Resend && tx.state == Impl::TxState::Unacked && tx.retries)
            {
                --tx.retries;
                tx.interval = interval;
                Impl::setDeadline(tx, now, interval);
                if (!tx.retries) tx.state = Impl::TxState::Final;
            }
            else { impl.outstanding = {}; return Status::Stale; }
            if (sent && action.kind != ActionKind::Resend)
                circuit->successfulOutBytes = plus(circuit->successfulOutBytes, wireBytes);
            impl.outstanding = {};
            return Status::Ok;
        }
        impl.outstanding = {};
        return Status::Stale;
    }
    if ((action.kind == ActionKind::Ack || action.kind == ActionKind::Ping) && !action.packetAssigned) return Status::Invalid;
    if (action.kind == ActionKind::Ack)
    {
        circuit->deferredAck = {};
        for (uint32_t j = 0; j < action.ackCount; ++j)
        {
            Impl::Ack* best = nullptr;
            for (uint32_t i = 0; i < impl.limits.acks; ++i)
            {
                auto& ack = impl.acks[i];
                if (ack.ready && ack.incarnation == action.incarnation && (!best || ack.serial < best->serial)) best = &ack;
            }
            if (!best || best->packet != action.ackIds[j]) { impl.outstanding = {}; return Status::Stale; }
            *best = {};
        }
        bool remaining = false;
        for (uint32_t i = 0; i < impl.limits.acks; ++i)
            if (impl.acks[i].ready && impl.acks[i].incarnation == action.incarnation) { remaining = true; break; }
        if (!remaining) circuit->ackFirstAge = 0;
    }
    else if (action.kind == ActionKind::Ping)
    {
        circuit->deferredPing = {};
        circuit->pingArmed = false;
        circuit->lastPing = now;
        circuit->lastPingId = action.pingId;
        if (circuit->pingsInTransit != UINT32_MAX) ++circuit->pingsInTransit;
        if (circuit->pingsInTransit > 3) circuit->snapshot.blocked = true;
    }
    if (sent) circuit->successfulOutBytes = plus(circuit->successfulOutBytes, wireBytes);
    impl.outstanding = {};
    return Status::Ok;
}

LLTransportCircuitState::Status LLTransportCircuitState::reservePiggyback(LLTransportEndpoint endpoint, uint64_t incarnation,
    uint32_t capacity, Piggyback& value) noexcept
{
    value = {};
    if (!mImpl || !capacity) return Status::Invalid;
    auto& impl = *mImpl;
    const auto* circuit = impl.find(endpoint);
    if (!circuit) return Status::Absent;
    if (circuit->snapshot.incarnation != incarnation) return Status::Stale;
    if (!circuit->snapshot.alive) return Status::Invalid;
    if (impl.piggyback.token || circuit->deferredAck.kind == ActionKind::Ack ||
        (impl.outstanding.incarnation == incarnation && impl.outstanding.kind == ActionKind::Ack)) return Status::Busy;
    if (impl.piggybackSerial == UINT64_MAX) return Status::Full;
    Piggyback result{};
    result.endpoint = endpoint; result.incarnation = incarnation;
    uint64_t serial = 0;
    for (uint32_t j = 0; j < std::min<uint32_t>(capacity, 250); ++j)
    {
        const Impl::Ack* best = nullptr;
        for (uint32_t i = 0; i < impl.limits.acks; ++i)
        {
            const auto& ack = impl.acks[i];
            if (!ack.ready || ack.incarnation != incarnation || ack.serial <= serial) continue;
            if (!best || ack.serial < best->serial) best = &ack;
        }
        if (!best) break;
        serial = best->serial;
        result.ackIds[result.count] = best->packet;
        impl.piggybackAckSerials[result.count++] = serial;
    }
    if (!result.count) return Status::NotDue;
    result.token = next(impl.piggybackSerial);
    impl.piggyback = result; value = result;
    return Status::Ok;
}

LLTransportCircuitState::Status LLTransportCircuitState::finishPiggyback(uint64_t token, bool attempted) noexcept
{
    if (!mImpl || !token || mImpl->piggyback.token != token) return Status::Stale;
    auto& impl = *mImpl;
    if (attempted)
    {
        // Validate the complete reservation before changing any ACK entry.
        for (uint32_t j = 0; j < impl.piggyback.count; ++j)
        {
            bool found = false;
            for (uint32_t i = 0; i < impl.limits.acks; ++i)
                if (impl.acks[i].ready && impl.acks[i].incarnation == impl.piggyback.incarnation &&
                    impl.acks[i].serial == impl.piggybackAckSerials[j]) { found = true; break; }
            if (!found) return Status::Stale;
        }
        for (uint32_t j = 0; j < impl.piggyback.count; ++j)
            for (uint32_t i = 0; i < impl.limits.acks; ++i)
                if (impl.acks[i].ready && impl.acks[i].incarnation == impl.piggyback.incarnation &&
                    impl.acks[i].serial == impl.piggybackAckSerials[j]) { impl.acks[i] = {}; break; }
    }
    impl.piggyback = {};
    return Status::Ok;
}

LLTransportCircuitState::Status LLTransportCircuitState::consumeAck(LLTransportEndpoint endpoint, uint64_t incarnation, uint32_t packet) noexcept
{
    if (!mImpl || packet >= PacketSpace) return Status::Invalid;
    auto& impl = *mImpl;
    const auto* circuit = impl.find(endpoint);
    if (!circuit) return Status::Absent;
    if (circuit->snapshot.incarnation != incarnation) return Status::Stale;
    for (uint32_t i = 0; i < impl.limits.reliable; ++i)
    {
        auto& tx = impl.reliable[i];
        if (tx.incarnation != incarnation || tx.packet != packet || (tx.state != Impl::TxState::Initial &&
            tx.state != Impl::TxState::Unacked && tx.state != Impl::TxState::Final)) continue;
        if (impl.outstanding.incarnation == incarnation && impl.outstanding.packetId == packet &&
            (impl.outstanding.kind == ActionKind::InitialSend || impl.outstanding.kind == ActionKind::Resend)) return Status::Busy;
        tx.state = Impl::TxState::Complete; tx.result = tx.successIsTimeout ? Timeout : 0;
        return Status::Ok;
    }
    return Status::Absent;
}

bool LLTransportCircuitState::hasCompletion() const noexcept
{
    if (!mImpl) return false;
    for (uint32_t i=0;i<mImpl->limits.reliable;++i)
        if (mImpl->reliable[i].state==Impl::TxState::Complete) return true;
    return false;
}

bool LLTransportCircuitState::takeCompletion(Completion& completion) noexcept
{
    if (!mImpl) return false;
    for (uint32_t i = 0; i < mImpl->limits.reliable; ++i)
    {
        auto& tx = mImpl->reliable[i];
        if (tx.state != Impl::TxState::Complete) continue;
        completion = {tx.endpoint, tx.login, tx.incarnation, tx.token, tx.result};
        tx = {};
        return true;
    }
    return false;
}

LLTransportCircuitState::Status LLTransportCircuitState::notePingReply(LLTransportEndpoint endpoint, uint64_t incarnation,
    uint8_t pingId, uint64_t heartbeat, uint64_t now) noexcept
{
    if (!mImpl) return Status::Invalid;
    auto* circuit = mImpl->find(endpoint);
    if (!circuit) return Status::Absent;
    if (circuit->snapshot.incarnation != incarnation) return Status::Stale;
    if (now < circuit->lastPing) return Status::Invalid;
    uint64_t pending = 0;
    for (uint32_t i = 0; i < mImpl->limits.receives; ++i) if (mImpl->receives[i].id) ++pending;
    if (UINT64_MAX - mImpl->sequence <= pending) return Status::Full;
    float previousEffective = 0.f;
    const auto averageStatus = effectivePingMilliseconds(endpoint, incarnation, heartbeat, now, previousEffective);
    if (averageStatus != Status::Ok) return averageStatus;
    const uint32_t delta = static_cast<uint8_t>(circuit->lastPingId - pingId);
    const uint64_t pingUsec = plus(now - circuit->lastPing,
        delta && heartbeat > UINT64_MAX / delta ? UINT64_MAX : heartbeat * delta);
    circuit->snapshot.pingMilliseconds = static_cast<uint32_t>(std::min<uint64_t>(pingUsec / 1000, UINT32_MAX));
    const float raw = static_cast<float>(circuit->snapshot.pingMilliseconds);
    circuit->averagePingMs = std::clamp(0.8f * std::max(raw, previousEffective) + 0.2f * raw, 100.f, 2000.f);
    circuit->pingsInTransit = delta;
    circuit->lastPingReceived = now;
    if (circuit->snapshot.blocked && delta <= 2) circuit->snapshot.blocked = false;
    circuit->snapshot.ownerMonotonicUsec = now;
    mImpl->publish(*circuit);
    return Status::Ok;
}


LLTransportCircuitState::Status LLTransportCircuitState::expireLosses(LLTransportEndpoint endpoint, uint64_t incarnation,
    uint64_t now, uint64_t timeout) noexcept
{
    if (!mImpl || timeout > 5000000) return Status::Invalid;
    auto& impl = *mImpl;
    auto* circuit = impl.find(endpoint);
    if (!circuit) return Status::Absent;
    if (circuit->snapshot.incarnation != incarnation) return Status::Stale;
    if (!circuit->lossTrackingComplete) return Status::Partial;
    for (uint32_t i = 0; i < impl.limits.receives; ++i)
        if (impl.receives[i].incarnation == incarnation) return Status::Busy;
    uint32_t expired = 0;
    for (uint32_t i = 0; i < impl.limits.losses; ++i)
    {
        const auto& loss = impl.losses[i];
        if (loss.incarnation == incarnation && !loss.reservation && now >= loss.time && now - loss.time > timeout) ++expired;
    }
    if (!expired) return Status::Ok;
    uint64_t pending = 0;
    for (uint32_t i = 0; i < impl.limits.receives; ++i) if (impl.receives[i].id) ++pending;
    if (UINT64_MAX - impl.sequence <= pending) return Status::Full;
    for (uint32_t i = 0; i < impl.limits.losses; ++i)
    {
        auto& loss = impl.losses[i];
        if (loss.incarnation == incarnation && !loss.reservation && now >= loss.time && now - loss.time > timeout) loss = {};
    }
    circuit->snapshot.packetsLost += expired;
    circuit->snapshot.ownerMonotonicUsec = now;
    impl.publish(*circuit);
    return Status::Ok;
}

LLTransportCircuitState::Status LLTransportCircuitState::effectivePingMilliseconds(LLTransportEndpoint endpoint, uint64_t incarnation,
    uint64_t heartbeat, uint64_t now, float& value) const noexcept
{
    if (!mImpl) return Status::Invalid;
    const auto* circuit = mImpl->find(endpoint);
    if (!circuit) return Status::Absent;
    if (circuit->snapshot.incarnation != incarnation) return Status::Stale;
    if (now < circuit->lastPing) return Status::Invalid;
    float inTransitMs = 0.f;
    if (circuit->pingsInTransit)
    {
        // Source formula: ((inTransit * heartbeat - 1s) + elapsed), in ms.
        const double usec = static_cast<double>(circuit->pingsInTransit) * static_cast<double>(heartbeat)
                            - 1000000.0 + static_cast<double>(now - circuit->lastPing);
        inTransitMs = static_cast<float>(std::min(usec / 1000.0, 2000.0));
    }
    value = std::min(std::max(inTransitMs, circuit->averagePingMs), 2000.f);
    return Status::Ok;
}


LLTransportCircuitState::Status LLTransportCircuitState::configureWatchdog(LLTransportEndpoint endpoint, uint64_t incarnation,
    uint64_t heartbeat, uint64_t timeout, uint64_t now, float offsetDraw, float jitterDraw) noexcept
{
    if (!mImpl || !heartbeat || !std::isfinite(offsetDraw) || !std::isfinite(jitterDraw) ||
        offsetDraw < 0.f || offsetDraw >= 1.f || jitterDraw < 0.f || jitterDraw >= 1.f) return Status::Invalid;
    auto* circuit = mImpl->find(endpoint);
    if (!circuit) return Status::Absent;
    if (circuit->snapshot.incarnation != incarnation) return Status::Stale;
    if (circuit->watchdogConfigured || circuit->timeoutEvent) return Status::Busy;
    circuit->heartbeatUsec = heartbeat; circuit->timeoutUsec = timeout;
    circuit->nextWatchdog = plus(now, watchdogDelay(heartbeat, offsetDraw, jitterDraw));
    circuit->watchdogConfigured = true;
    return Status::Ok;
}

LLTransportCircuitState::Status LLTransportCircuitState::nextWatchdogUsec(LLTransportEndpoint endpoint, uint64_t incarnation,
    uint64_t& due) const noexcept
{
    if (!mImpl) return Status::Invalid;
    const auto* circuit = mImpl->find(endpoint);
    if (!circuit) return Status::Absent;
    if (circuit->snapshot.incarnation != incarnation) return Status::Stale;
    if (!circuit->watchdogConfigured) return Status::Invalid;
    due = circuit->nextWatchdog;
    return Status::Ok;
}

LLTransportCircuitState::WatchdogPoll LLTransportCircuitState::pollWatchdog(LLTransportEndpoint endpoint, uint64_t incarnation,
    uint64_t now, bool eventSlotReserved, uint64_t ownerAppSequence, float jitterDraw) noexcept
{
    if (!mImpl) return {};
    auto* circuit = mImpl->find(endpoint);
    if (!circuit) return {WatchdogStatus::Stale, {}};
    if (circuit->snapshot.incarnation != incarnation) return {WatchdogStatus::Stale, {}};
    if (!circuit->watchdogConfigured) return {};
    if (circuit->timeoutEvent || !circuit->snapshot.alive) return {WatchdogStatus::Busy, {}};
    if (now < circuit->nextWatchdog) return {WatchdogStatus::NotDue, {}};
    if (now < circuit->lastPingReceived) return {};
    if (now - circuit->lastPingReceived > circuit->timeoutUsec)
    {
        TimeoutEvent event{};
        const auto status = beginWatchdogTimeout(endpoint, incarnation, now, circuit->timeoutUsec, eventSlotReserved, ownerAppSequence, event);
        if (status == Status::Ok) return {WatchdogStatus::Timeout, event};
        if (status == Status::Full) return {WatchdogStatus::Full, {}};
        if (status == Status::Busy) return {WatchdogStatus::Busy, {}};
        return {};
    }
    if (!std::isfinite(jitterDraw) || jitterDraw < 0.f || jitterDraw >= 1.f) return {};
    circuit->nextWatchdog = plus(now, watchdogDelay(circuit->heartbeatUsec, 0.f, jitterDraw));
    circuit->pingArmed = true;
    return {WatchdogStatus::PingReady, {}};
}

LLTransportCircuitState::Status LLTransportCircuitState::beginWatchdogTimeout(LLTransportEndpoint endpoint, uint64_t incarnation,
    uint64_t now, uint64_t timeout, bool eventSlotReserved, uint64_t ownerAppSequence, TimeoutEvent& event) noexcept
{
    event = {};
    if (!mImpl) return Status::Invalid;
    auto& impl = *mImpl;
    auto* circuit = impl.find(endpoint);
    if (!circuit) return Status::Absent;
    if (circuit->snapshot.incarnation != incarnation) return Status::Stale;
    if (circuit->timeoutEvent || impl.outstanding.incarnation == incarnation || impl.piggyback.incarnation == incarnation) return Status::Busy;
    for (uint32_t i = 0; i < impl.limits.receives; ++i)
        if (impl.receives[i].incarnation == incarnation) return Status::Busy;
    if (!circuit->snapshot.alive || now < circuit->lastPingReceived || now - circuit->lastPingReceived <= timeout) return Status::NotDue;
    if (!eventSlotReserved || impl.timeoutSerial == UINT64_MAX) return Status::Full;
    uint64_t pending = 0;
    for (uint32_t i = 0; i < impl.limits.receives; ++i) if (impl.receives[i].id) ++pending;
    if (UINT64_MAX - impl.sequence <= pending) return Status::Full;
    circuit->timeoutEvent = next(impl.timeoutSerial);
    circuit->timeoutAppSequence = ownerAppSequence;
    circuit->snapshot.alive = false; circuit->snapshot.ownerMonotonicUsec = now;
    // setAlive(false) resets outgoing IDs; discard unsent encoded-control
    // reservations but retain ACK entries for a possible callback revival.
    circuit->deferredAck = {}; circuit->deferredPing = {}; circuit->pingArmed = false;
    circuit->timeoutCheckTime = now;
    circuit->lastOut = 0; circuit->expected = 0;
    impl.publish(*circuit);
    event = {endpoint, circuit->login, incarnation, circuit->timeoutEvent, ownerAppSequence};
    return Status::Ok;
}

LLTransportCircuitState::Status LLTransportCircuitState::finishWatchdogTimeout(const TimeoutEvent& event, float jitterDraw) noexcept
{
    if (!mImpl || !event.eventId) return Status::Invalid;
    auto* circuit = mImpl->find(event.endpoint);
    if (!circuit) return Status::Absent;
    if (circuit->snapshot.incarnation != event.incarnation || circuit->login != event.loginGeneration ||
        circuit->timeoutEvent != event.eventId || circuit->timeoutAppSequence != event.lastPublishedAppSequence) return Status::Stale;
    if (circuit->snapshot.alive)
    {
        if (circuit->watchdogConfigured)
        {
            if (!std::isfinite(jitterDraw) || jitterDraw < 0.f || jitterDraw >= 1.f) return Status::Invalid;
            circuit->nextWatchdog = plus(circuit->timeoutCheckTime, watchdogDelay(circuit->heartbeatUsec, 0.f, jitterDraw));
            circuit->pingArmed = true;
        }
        circuit->timeoutEvent = 0;
        return Status::Ok;
    }
    LLTransportCircuitCommand close{};
    close.kind = LLTransportCircuitCommandKind::DisableIfUnchanged;
    close.endpoint = event.endpoint; close.loginGeneration = event.loginGeneration;
    close.targetIncarnation = event.incarnation;
    close.observedPublicationSequence = circuit->snapshot.publicationSequence;
    close.observedLastPacketMonotonicUsec = circuit->snapshot.lastPacketMonotonicUsec;
    close.observedHasLastPacket = circuit->snapshot.hasLastPacket;
    return conditionalClose(close, event.lastPublishedAppSequence).status == LLTransportCircuitCommandStatus::Applied ? Status::Ok : Status::Stale;
}

LLTransportCircuitState::Status LLTransportCircuitState::noteOrdinaryAttempt(LLTransportEndpoint endpoint, uint64_t incarnation,
    bool sent, uint32_t wireBytes) noexcept
{
    if (!mImpl || (!sent && wireBytes)) return Status::Invalid;
    auto* circuit = mImpl->find(endpoint);
    if (!circuit) return Status::Absent;
    if (circuit->snapshot.incarnation != incarnation) return Status::Stale;
    if (sent) circuit->successfulOutBytes = plus(circuit->successfulOutBytes, wireBytes);
    return Status::Ok;
}

LLTransportCircuitState::Status LLTransportCircuitState::successfulBytesOut(LLTransportEndpoint endpoint, uint64_t incarnation,
    uint64_t& bytes) const noexcept
{
    if (!mImpl) return Status::Invalid;
    const auto* circuit = mImpl->find(endpoint);
    if (!circuit) return Status::Absent;
    if (circuit->snapshot.incarnation != incarnation) return Status::Stale;
    bytes = circuit->successfulOutBytes;
    return Status::Ok;
}
