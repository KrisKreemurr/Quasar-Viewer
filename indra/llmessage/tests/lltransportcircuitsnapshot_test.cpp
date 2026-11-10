/**
 * @file lltransportcircuitsnapshot_test.cpp
 * @brief Standalone coherent circuit snapshot checks.
 *
 * Copyright (c) 2026, Slipstream Viewer Project
 * SPDX-License-Identifier: LGPL-2.1-only
 */
#include "../lltransportcircuitsnapshot.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

static void check(bool condition, const char* expression, int line)
{
    if (!condition) { std::fprintf(stderr, "check failed at line %d: %s\n", line, expression); std::abort(); }
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

int main()
{
    LLTransportCircuitSnapshotCell cell;
    CHECK(cell.read().state == LLTransportSnapshotState::Unavailable);
    LLTransportCircuitSnapshot missing;
    missing.state = LLTransportSnapshotState::Absent;
    missing.publicationSequence = 1;
    cell.publish(missing);
    CHECK(cell.read().state == LLTransportSnapshotState::Absent);

    uint64_t age = 99;
    CHECK(!llTransportCircuitAgeUsec(missing, 10, age) && age == 99);
    LLTransportCircuitSnapshot timing;
    timing.state = LLTransportSnapshotState::Present;
    timing.ownerMonotonicUsec = 20;
    timing.birthMonotonicUsec = 5;
    CHECK(llTransportCircuitAgeUsec(timing, 25, age) && age == 20);
    CHECK(llTransportCircuitAgeUsec(timing, 30, age) && age == 25);
    timing.hasLastPacket = true;
    timing.lastPacketMonotonicUsec = 18;
    CHECK(llTransportCircuitAgeUsec(timing, 25, age) && age == 7);
    CHECK(!llTransportCircuitAgeUsec(timing, 19, age));
    timing.lastPacketMonotonicUsec = 21;
    CHECK(!llTransportCircuitAgeUsec(timing, 25, age));

    LLTransportCircuitSnapshot live;
    live.state = LLTransportSnapshotState::Present;
    live.endpoint = {42, 9000};
    live.incarnation = 3;
    live.publicationSequence = 7;
    live.hasLastPacket = true;
    live.lastPacketMonotonicUsec = 18;
    LLTransportCircuitCommand close;
    close.kind = LLTransportCircuitCommandKind::DisableIfUnchanged;
    close.endpoint = live.endpoint;
    close.targetIncarnation = live.incarnation;
    close.observedPublicationSequence = live.publicationSequence;
    close.observedHasLastPacket = live.hasLastPacket;
    close.observedLastPacketMonotonicUsec = live.lastPacketMonotonicUsec;
    CHECK(llTransportCheckConditionalClose(close, live) == LLTransportCircuitCommandStatus::Applied);
    ++live.publicationSequence; // a second packet in the same clock tick
    CHECK(llTransportCheckConditionalClose(close, live) == LLTransportCircuitCommandStatus::PacketChanged);
    --live.publicationSequence;
    ++live.incarnation;
    CHECK(llTransportCheckConditionalClose(close, live) == LLTransportCircuitCommandStatus::IncarnationChanged);
    --live.incarnation;
    ++live.endpoint.portHostOrder;
    CHECK(llTransportCheckConditionalClose(close, live) == LLTransportCircuitCommandStatus::IncarnationChanged);
    --live.endpoint.portHostOrder;
    live.state = LLTransportSnapshotState::Unavailable;
    CHECK(llTransportCheckConditionalClose(close, live) == LLTransportCircuitCommandStatus::Unavailable);
    close.observedPublicationSequence = 0;
    CHECK(llTransportCheckConditionalClose(close, live) == LLTransportCircuitCommandStatus::Invalid);

    std::atomic<bool> done{false};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    std::thread producer([&]()
    {
        for (uint32_t i = 1; i <= 100000; ++i)
        {
            LLTransportCircuitSnapshot value;
            value.state = LLTransportSnapshotState::Present;
            value.incarnation = i;
            value.publicationSequence = i;
            value.endpoint.addressNetworkOrder = i;
            value.ownerMonotonicUsec = i * 2u;
            value.lastPacketMonotonicUsec = i;
            value.birthMonotonicUsec = i / 2;
            value.hasLastPacket = true;
            value.packetsIn = i;
            value.packetsOut = ~i;
            value.lossQuality = (i & 1u) ? LLTransportLossQuality::Complete : LLTransportLossQuality::Partial;
            value.alive = (i & 1u) != 0;
            cell.publish(value);
        }
        done.store(true, std::memory_order_release);
    });
    while (!done.load(std::memory_order_acquire))
    {
        const LLTransportCircuitSnapshot value = cell.read();
        if (value.state == LLTransportSnapshotState::Present)
        {
            const uint32_t i = value.packetsIn;
            CHECK(value.incarnation == i && value.publicationSequence == i);
            CHECK(value.endpoint.addressNetworkOrder == i && value.packetsOut == ~i);
            CHECK(value.ownerMonotonicUsec == uint64_t(i) * 2 && value.lastPacketMonotonicUsec == i && value.hasLastPacket);
            CHECK(value.birthMonotonicUsec == i / 2);
            CHECK(value.lossQuality == ((i & 1u) ? LLTransportLossQuality::Complete : LLTransportLossQuality::Partial));
            CHECK(value.alive == ((i & 1u) != 0));
        }
        CHECK(std::chrono::steady_clock::now() < deadline);
    }
    producer.join();
    CHECK(cell.read().incarnation == 100000);

    done.store(false, std::memory_order_relaxed);
    const auto pausedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    std::thread pausedProducer([&]()
    {
        for (uint32_t i = 1; i <= 100000; ++i)
        {
            LLTransportCircuitSnapshot value;
            value.state = LLTransportSnapshotState::Present;
            value.incarnation = i;
            cell.publish(value);
        }
        done.store(true, std::memory_order_release);
    });
    // No reads or consumer acknowledgements while the owner publishes.
    while (!done.load(std::memory_order_acquire))
    {
        CHECK(std::chrono::steady_clock::now() < pausedDeadline);
        std::this_thread::yield();
    }
    pausedProducer.join();
    CHECK(cell.read().incarnation == 100000);
}
