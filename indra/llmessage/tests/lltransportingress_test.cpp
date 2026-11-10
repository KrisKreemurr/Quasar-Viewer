/** WIP admission checks; no socket or viewer. SPDX-License-Identifier: LGPL-2.1-only */
#include "../lltransportingress.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
static void check(bool value, int line) { if (!value) { std::fprintf(stderr,"failed %d\n", line); std::abort(); } }
#define CHECK(x) check(bool(x), __LINE__)
struct State
{
    enum class Status { Ok, Full };
    bool reject = false;
    unsigned committed = 0, cancelled = 0;
    Status commitReceive(uint64_t id, uint32_t, uint64_t) noexcept
    { if (!id || reject) return Status::Full; ++committed; return Status::Ok; }
    void cancelReceive(uint64_t) noexcept { ++cancelled; }
};
struct FailingArena
{
    LLTransportDecodedArena& inner;
    LLTransportDecodedArena::Result tryCopy(const LLTransportDecodedView& v, LLTransportDecodedArena::Record& r) noexcept { return inner.tryCopy(v, r); }
    bool commit(const LLTransportDecodedArena::Record&) noexcept { return false; }
    bool cancel(const LLTransportDecodedArena::Record& r) noexcept { return inner.cancel(r); }
};
struct FailingQueue
{
    LLTransportIngressQueue& inner;
    LLTransportIngressEvent* reserveProducer() noexcept { return inner.reserveProducer(); }
    void cancelProducer() noexcept { inner.cancelProducer(); }
    bool publishProducer() noexcept { return false; }
};
int main()
{
    auto scratch = std::make_unique<LLTransportIngressScratch>();
    auto queue = std::make_unique<LLTransportIngressQueue>();
    auto arena = std::make_unique<LLTransportDecodedArena>();
    CHECK(arena->initialize(1024 * 1024, 512 * 1024));
    CHECK(scratch->appendBlock(0, 0));
    const uint8_t byte = 42;
    CHECK(scratch->appendField(0, &byte, 1));
    State state;
    std::atomic<bool> poisoned{false};
    LLTransportIngressEvent metadata;
    metadata.wireBytes = 23; metadata.schemaGeneration = 8;
    CHECK(llTransportPublishDecoded(state, 1, LLTransportSteadyUsec(49), 8, poisoned, *scratch, *arena, *queue, metadata) == LLTransportPublishStatus::InvalidRecord);
    CHECK(state.committed == 0 && state.cancelled == 1);
    metadata.circuitIncarnation = 3; metadata.appSequence = 1;
    CHECK(llTransportPublishDecoded(state, 1, LLTransportSteadyUsec(49), 9, poisoned, *scratch, *arena, *queue, metadata) == LLTransportPublishStatus::InvalidRecord);
    CHECK(state.committed == 0 && state.cancelled == 2);
    state.reject = true;
    CHECK(llTransportPublishDecoded(state, 1, LLTransportSteadyUsec(50), 8, poisoned, *scratch, *arena, *queue, metadata) == LLTransportPublishStatus::StateRejected);
    CHECK(state.committed == 0 && state.cancelled == 3 && queue->peekConsumer() == nullptr);
    CHECK(arena->producerBytesInUse() == 0);
    state.reject = false;
    LLTransportDecodedArena invalidArena;
    CHECK(llTransportPublishDecoded(state, 2, LLTransportSteadyUsec(50), 8, poisoned, *scratch, invalidArena, *queue, metadata) == LLTransportPublishStatus::InvalidRecord);
    LLTransportDecodedArena fullArena;
    CHECK(fullArena.initialize(1024, 512));
    LLTransportDecodedArena::Record fill;
    while (fullArena.tryCopy(scratch->view(), fill) == LLTransportDecodedArena::Result::Ready) CHECK(fullArena.commit(fill));
    CHECK(llTransportPublishDecoded(state, 2, LLTransportSteadyUsec(50), 8, poisoned, *scratch, fullArena, *queue, metadata) == LLTransportPublishStatus::Backpressure);
    auto fullQueue = std::make_unique<LLTransportIngressQueue>();
    for (unsigned i=0; i<4096; ++i) { CHECK(fullQueue->reserveProducer()); CHECK(fullQueue->publishProducer()); }
    CHECK(llTransportPublishDecoded(state, 2, LLTransportSteadyUsec(50), 8, poisoned, *scratch, *arena, *fullQueue, metadata) == LLTransportPublishStatus::Backpressure);
    CHECK(state.committed == 0 && !poisoned.load());
    CHECK(llTransportPublishDecoded(state, 2, LLTransportSteadyUsec(51), 8, poisoned, *scratch, *arena, *queue, metadata) == LLTransportPublishStatus::Published);
    CHECK(state.committed == 1);
    const auto* event = queue->peekConsumer();
    CHECK(event && event->schemaGeneration == 8);
    LLTransportDecodedView view;
    CHECK(arena->view(event->arenaRecord, view) && view.byteCount == 1 && view.data[0] == 42);
    scratch->data[0] = 7;
    CHECK(view.data[0] == 42); // reader borrows until clear; scratch may be reused
    const auto record = event->arenaRecord;
    bool readerHasView = true;
    CHECK(readerHasView && arena->view(record, view)); // handler returned, no release yet
    readerHasView = false; // model clearMessage before FIFO release
    CHECK(!readerHasView && arena->release(record));
    CHECK(queue->releaseConsumer());
    CHECK(!arena->view(record, view) && arena->producerBytesInUse() == 0);
    LLTransportDecodedArena failingBacking;
    CHECK(failingBacking.initialize(1024, 512));
    FailingArena failing{failingBacking};
    CHECK(llTransportPublishDecoded(state, 3, LLTransportSteadyUsec(52), 8, poisoned, *scratch, failing, *queue, metadata) == LLTransportPublishStatus::InvariantFailure);
    CHECK(poisoned.load());
    const auto committed = state.committed;
    CHECK(llTransportPublishDecoded(state, 4, LLTransportSteadyUsec(53), 8, poisoned, *scratch, *arena, *queue, metadata) == LLTransportPublishStatus::Poisoned);
    CHECK(state.committed == committed); // neither new nor duplicate ACK work may proceed
    // A separate poison instance covers metadata publication failure after
    // arena commit; no event may be read and the actor must not resume.
    std::atomic<bool> poisonedQueue{false};
    LLTransportDecodedArena arena2;
    CHECK(arena2.initialize(1024, 512));
    auto queue2 = std::make_unique<LLTransportIngressQueue>();
    FailingQueue failingQueue{*queue2};
    CHECK(llTransportPublishDecoded(state, 5, LLTransportSteadyUsec(54), 8, poisonedQueue, *scratch, arena2, failingQueue, metadata) == LLTransportPublishStatus::InvariantFailure);
    CHECK(poisonedQueue.load() && queue2->peekConsumer() == nullptr);
}
