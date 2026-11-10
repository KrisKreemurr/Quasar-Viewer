/** Bounded mesh preparation ownership tests; no viewer, network or GPU. */
#include "llmeshpreparation.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <memory>
#include <new>
#include <thread>
#include <vector>

int main()
{
    auto state = std::make_shared<LLMeshPrepState>();
    LLMeshPrepLease disabled;
    assert(!disabled.acquire(state, 1));
    state->setEnabled(true);
    LLMeshPrepLease tooLarge;
    assert(!tooLarge.acquire(state, LLMeshPrepState::MAX_JOB_BYTES + 1));
    {
        LLMeshPrepLease one, two, refused;
        assert(one.acquire(state, 1024) && two.acquire(state, 1024));
        assert(!refused.acquire(state, 1024));
        one.finishWorker();
        assert(refused.acquire(state, 1024));
        two.finishWorker();
        refused.finishWorker();
        assert(state->active.load() == 0 && state->results.load() == 3);
        state->setEnabled(false);
        assert(!one.current() && !two.current() && !refused.current());
        state->setEnabled(true);
        assert(!one.current());
    }
    assert(state->bytes.load() == 0 && state->results.load() == 0);
    {
        std::vector<LLMeshPrepLease> held;
        for (uint32_t n = 0; n < LLMeshPrepState::MAX_RESULTS; ++n)
        {
            LLMeshPrepLease lease;
            assert(lease.acquire(state, 1024));
            lease.finishWorker();
            held.push_back(std::move(lease));
        }
        LLMeshPrepLease excess;
        assert(!excess.acquire(state, 1));
        assert(state->results.load() == LLMeshPrepState::MAX_RESULTS);
    }
    {
        std::vector<LLMeshPrepLease> held;
        for (unsigned n = 0; n < 4; ++n)
        {
            LLMeshPrepLease lease;
            assert(lease.acquire(state, LLMeshPrepState::MAX_JOB_BYTES));
            lease.finishWorker();
            held.push_back(std::move(lease));
        }
        LLMeshPrepLease excess;
        assert(state->bytes.load() == LLMeshPrepState::MAX_BYTES && !excess.acquire(state, 1));
    }
    try
    {
        LLMeshPrepLease lease;
        assert(lease.acquire(state, 4096));
        throw std::bad_alloc();
    }
    catch (const std::bad_alloc&) {}
    assert(state->bytes.load() == 0 && state->results.load() == 0 && state->active.load() == 0);
    std::vector<std::thread> workers;
    for (unsigned n = 0; n < 8; ++n)
        workers.emplace_back([state] {
            for (unsigned i = 0; i < 1000; ++i)
            {
                LLMeshPrepLease lease;
                if (!lease.acquire(state, 4096)) continue;
                assert(state->results.load() <= LLMeshPrepState::MAX_RESULTS);
                assert(state->active.load() <= LLMeshPrepState::MAX_ACTIVE);
                assert(state->bytes.load() <= LLMeshPrepState::MAX_BYTES);
                lease.finishWorker();
            }
        });
    for (auto& worker : workers) worker.join();
    assert(state->bytes.load() == 0 && state->results.load() == 0 && state->active.load() == 0);
}
