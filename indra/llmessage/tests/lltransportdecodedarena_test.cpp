/** Actual arena/header tests, not a transport or simulator integration test. */
#include "lltransportdecodedarena.h"
#include "lltransportspscqueue.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "CHECK failed at %d: %s\n", __LINE__, #condition); \
    std::abort(); } } while (false)

using Arena = LLTransportDecodedArena;
using Scratch = LLTransportDecodedPayload<16, 1, 1>;

static Scratch message(uint32_t value)
{
    Scratch result;
    CHECK(result.appendBlock(0, 0));
    CHECK(result.appendField(0, &value, sizeof(value)));
    return result;
}

static void checkValue(const LLTransportDecodedView& view, uint32_t expected)
{
    CHECK(view.byteCount == 4 && view.blockCount == 1 && view.fieldCount == 1);
    CHECK(view.blocks[0].fieldCount == 1 && view.blocks[0].firstField == 0);
    CHECK(view.fields[0].offset == 0 && view.fields[0].size == 4);
    uint32_t value = 0;
    std::memcpy(&value, view.data, sizeof(value));
    CHECK(value == expected);
}

static void sequential()
{
    Arena arena;
    CHECK(!arena.initialize(0, 0));
    CHECK(!arena.initialize(127, 64));
    CHECK(!arena.initialize(128, 128));
    CHECK(!arena.initialize(128, 16));
    CHECK(!arena.initialize(Arena::kMaximumCapacityBytes + 16, 128));
    CHECK(!arena.initialize(2u * 1024u * 1024u, Arena::kMaximumRecordBytes + 16));
    CHECK(arena.initialize(256, 128));
    CHECK(!arena.initialize(512, 128));
    CHECK(arena.capacityBytes() == 256);

    auto source = message(123);
    Arena::Record first, second, third, wrapped;
    LLTransportDecodedView view;
    CHECK(arena.tryCopy(source.view(), first) == Arena::Result::Ready);
    CHECK(!arena.view(first, view)); // uncommitted bytes cannot be borrowed
    CHECK(arena.tryCopy(source.view(), second) == Arena::Result::Busy);
    CHECK(arena.commit(first));
    source.data.fill(0); // queued data must be independent of reused scratch
    CHECK(arena.view(first, view));
    checkValue(view, 123);

    source = message(456);
    CHECK(arena.tryCopy(source.view(), second) == Arena::Result::Ready);
    CHECK(arena.commit(second));
    source = message(789);
    CHECK(arena.tryCopy(source.view(), third) == Arena::Result::Ready);
    CHECK(arena.commit(third));
    CHECK(!arena.view(second, view)); // FIFO ownership, not arbitrary release
    CHECK(!arena.release(second));
    CHECK(arena.tryCopy(source.view(), wrapped) == Arena::Result::Full);

    auto corrupt = first;
    ++corrupt.serial;
    CHECK(!arena.view(corrupt, view));
    corrupt = first; corrupt.end += 16;
    CHECK(!arena.view(corrupt, view));
    corrupt = first; corrupt.byteCount = 0; // same padded length, wrong header
    CHECK(!arena.view(corrupt, view));
    corrupt = first; corrupt.offset += 16;
    CHECK(!arena.view(corrupt, view));
    CHECK(arena.release(first));
    CHECK(!arena.release(first));
    CHECK(arena.view(second, view)); checkValue(view, 456);
    CHECK(arena.release(second));

    source = message(999);
    CHECK(arena.tryCopy(source.view(), wrapped) == Arena::Result::Ready);
    CHECK(wrapped.offset == 0 && wrapped.begin != 0);
    CHECK(arena.commit(wrapped));
    CHECK(arena.view(third, view)); checkValue(view, 789); // held across wrap
    CHECK(arena.release(third));
    corrupt = wrapped; corrupt.end -= 16;
    CHECK(!arena.view(corrupt, view)); // initialized wrap-prefix identity
    CHECK(arena.view(wrapped, view)); checkValue(view, 999);
    CHECK(arena.release(wrapped));
    CHECK(arena.producerBytesInUse() == 0);

    Arena::Record canceled, replacement;
    CHECK(arena.tryCopy(source.view(), canceled) == Arena::Result::Ready);
    corrupt = canceled; ++corrupt.serial;
    CHECK(!arena.cancel(corrupt));
    CHECK(arena.cancel(canceled));
    CHECK(arena.tryCopy(source.view(), replacement) == Arena::Result::Ready);
    CHECK(replacement.begin == canceled.begin && replacement.serial != canceled.serial);
    CHECK(!arena.commit(canceled));
    CHECK(arena.commit(replacement));
    CHECK(!arena.view(canceled, view));
    CHECK(arena.release(replacement));

    auto invalid = source.view(); invalid.data = nullptr;
    Arena::Record unchanged = first;
    CHECK(arena.tryCopy(invalid, unchanged) == Arena::Result::Invalid);
    CHECK(unchanged == first);
    invalid = source.view(); invalid.fieldCount = UINT32_MAX;
    CHECK(arena.tryCopy(invalid, unchanged) == Arena::Result::Invalid);
    invalid = source.view(); invalid.blockCount = UINT32_MAX;
    CHECK(arena.tryCopy(invalid, unchanged) == Arena::Result::Invalid);
    invalid = source.view(); invalid.byteCount = UINT32_MAX;
    CHECK(arena.tryCopy(invalid, unchanged) == Arena::Result::Invalid);
    Scratch empty;
    CHECK(arena.tryCopy(empty.view(), replacement) == Arena::Result::Ready);
    CHECK(arena.commit(replacement));
    CHECK(arena.view(replacement, view));
    CHECK(view.byteCount == 0 && view.fieldCount == 0 && view.blockCount == 0);
    CHECK(arena.release(replacement));
    CHECK(arena.producerBytesInUse() == 0);
}

static void concurrent()
{
    Arena arena;
    CHECK(arena.initialize(512, 128));
    LLTransportSpscQueue<Arena::Record, 8> events;
    std::atomic<bool> fullObserved{false}, done{false};
    constexpr uint32_t count = 20000;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    std::thread producer([&]()
    {
        for (uint32_t i = 0; i < count;)
        {
            CHECK(std::chrono::steady_clock::now() < deadline);
            auto* slot = events.reserveProducer();
            if (!slot) { std::this_thread::yield(); continue; }
            const auto source = message(i);
            Arena::Record record;
            const auto result = arena.tryCopy(source.view(), record);
            if (result == Arena::Result::Full)
            {
                events.cancelProducer();
                fullObserved.store(true, std::memory_order_release);
                std::this_thread::yield();
                continue;
            }
            CHECK(result == Arena::Result::Ready);
            CHECK(arena.commit(record));
            *slot = record;
            CHECK(events.publishProducer());
            ++i;
        }
        done.store(true, std::memory_order_release);
    });
    // Force an actual arena-cap refusal before allowing any consumer progress.
    while (!fullObserved.load(std::memory_order_acquire))
    { CHECK(std::chrono::steady_clock::now() < deadline); std::this_thread::yield(); }
    for (uint32_t i = 0; i < count;)
    {
        CHECK(std::chrono::steady_clock::now() < deadline);
        const auto* event = events.peekConsumer();
        if (!event) { std::this_thread::yield(); continue; }
        const auto record = *event;
        LLTransportDecodedView view;
        CHECK(arena.view(record, view));
        checkValue(view, i);
        if (i % 127 == 0)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            checkValue(view, i); // producer cannot reuse a held reader's bytes
        }
        CHECK(arena.release(record));
        CHECK(events.releaseConsumer());
        ++i;
    }
    while (!done.load(std::memory_order_acquire))
    { CHECK(std::chrono::steady_clock::now() < deadline); std::this_thread::yield(); }
    producer.join();
    CHECK(arena.producerBytesInUse() == 0); // producer is joined
}

int main()
{
    sequential();
    concurrent();
    std::puts("PASS packed decoded arena ownership");
}
