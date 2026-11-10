/**
 * @file lltransportspscqueue_test.cpp
 * @brief Standalone bounded transport handoff checks.
 *
 * Copyright (c) 2026, Slipstream Viewer Project
 * SPDX-License-Identifier: LGPL-2.1-only
 */
#include "../lltransportspscqueue.h"
#include "../lltransportdecodedpayload.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

static void check(bool condition, const char* expression, int line)
{
    if (!condition)
    {
        std::fprintf(stderr, "check failed at line %d: %s\n", line, expression);
        std::abort();
    }
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

struct Message
{
    uint64_t sequence = 0;
    uint64_t complement = 0;
};

int main()
{
    LLTransportDecodedPayload<4, 2, 2> payload;
    uint8_t bytes[] = {1, 2, 3, 4};
    CHECK(!payload.appendField(0, bytes, 1));
    CHECK(payload.appendBlock(3, 0));
    CHECK(payload.appendField(7, bytes, 3));
    bytes[0] = 99;
    CHECK(payload.data[0] == 1); // fields own their bytes
    CHECK(!payload.appendField(8, bytes, 2)); // byte cap leaves state intact
    CHECK(payload.byteCount == 3 && payload.fieldCount == 1 && payload.blocks[0].fieldCount == 1);
    CHECK(payload.appendBlock(3, 1));
    CHECK(payload.appendField(7, bytes + 3, 1));
    CHECK(!payload.appendBlock(4, 0));
    CHECK(payload.blockCount == 2 && payload.fieldCount == 2 && payload.data[3] == 4);
    payload.reset();
    CHECK(payload.byteCount == 0 && payload.blockCount == 0 && payload.fieldCount == 0);
    LLTransportDecodedPayload<8, 1, 1> independent;
    CHECK(independent.appendBlock(1, 0));
    CHECK(!independent.appendBlock(2, 0)); // independent block cap
    CHECK(!independent.appendField(1, nullptr, 1)); // invalid source does not advance
    CHECK(independent.byteCount == 0 && independent.fieldCount == 0);
    CHECK(independent.appendField(1, bytes, 1));
    CHECK(!independent.appendField(2, bytes, 1)); // independent field cap
    CHECK(independent.byteCount == 1 && independent.fieldCount == 1 && independent.blocks[0].fieldCount == 1);
    LLTransportDecodedPayload<4, 1, 2> zeroed;
    CHECK(zeroed.appendBlock(1, 0));
    CHECK(zeroed.appendField(0, bytes, 2));
    CHECK(zeroed.appendZeroField(1, 2));
    CHECK(zeroed.data[2] == 0 && zeroed.data[3] == 0);
    CHECK(!zeroed.appendZeroField(2, 1));
    CHECK(zeroed.byteCount == 4 && zeroed.fieldCount == 2);

    LLTransportSpscQueue<Message, 2> queue;
    bool pending=true; CHECK(queue.pendingOnConsumer(pending) && !pending);
    CHECK(!queue.peekConsumer());
    CHECK(!queue.releaseConsumer());
    CHECK(!queue.publishProducer());
    Message* first = queue.reserveProducer();
    CHECK(first && !queue.reserveProducer());
    first->sequence = 1;
    queue.cancelProducer();
    CHECK(!queue.peekConsumer());
    first = queue.reserveProducer();
    CHECK(first);
    *first = {1, ~uint64_t(1)};
    CHECK(queue.publishProducer());
    CHECK(queue.pendingOnConsumer(pending) && pending);
    Message* second = queue.reserveProducer();
    CHECK(second);
    *second = {2, ~uint64_t(2)};
    CHECK(queue.publishProducer());
    CHECK(!queue.reserveProducer());
    LLTransportSpscQueue<Message, 2> control;
    Message* controlSlot = control.reserveProducer();
    CHECK(controlSlot);
    *controlSlot = {42, ~uint64_t(42)};
    CHECK(control.publishProducer());
    CHECK(control.peekConsumer()->sequence == 42);
    CHECK(control.releaseConsumer()); // separate queue storage only; not ACK progress
    const Message* held = queue.peekConsumer();
    CHECK(held && held->sequence == 1 && held == queue.peekConsumer());
    CHECK(!queue.reserveProducer()); // consumer has not released its slot
    CHECK(queue.releaseConsumer());
    CHECK(!queue.releaseConsumer());
    CHECK(queue.peekConsumer()->sequence == 2);
    CHECK(queue.releaseConsumer());
    CHECK(queue.pendingOnConsumer(pending) && !pending);
    for (uint64_t i = 0; i < 32; ++i)
    {
        Message* slot = queue.reserveProducer();
        CHECK(slot);
        *slot = {i, ~i};
        CHECK(queue.publishProducer());
        CHECK(queue.peekConsumer()->sequence == i);
        CHECK(queue.releaseConsumer());
    }

    LLTransportSpscQueue<Message, 64> concurrent;
    constexpr uint64_t count = 100000;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    std::atomic<bool> producerDone{false};
    std::thread producer([&]()
    {
        for (uint64_t i = 0; i < count; ++i)
        {
            Message* slot = nullptr;
            while (!(slot = concurrent.reserveProducer()))
            {
                CHECK(std::chrono::steady_clock::now() < deadline);
                std::this_thread::yield();
            }
            *slot = {i, ~i};
            CHECK(concurrent.publishProducer());
        }
        producerDone.store(true, std::memory_order_release);
    });
    for (uint64_t i = 0; i < count; ++i)
    {
        const Message* slot = nullptr;
        while (!(slot = concurrent.peekConsumer()))
        {
            CHECK(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
        CHECK(slot->sequence == i && slot->complement == ~i);
        CHECK(concurrent.releaseConsumer());
    }
    while (!producerDone.load(std::memory_order_acquire))
    {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    producer.join(); // producer has passed its last operation; no queue wait remains
    CHECK(!concurrent.peekConsumer());
}
