/** Bounded main-thread callback ownership; no socket or viewer is started. */
#include "lltransportcallbackregistry.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "CHECK failed at %d: %s\n", __LINE__, #condition); \
    std::abort(); } } while (false)

namespace
{
using Registry = LLTransportCallbackRegistry<2>;
struct State
{
    Registry* registry = nullptr;
    Registry::Token replacement = 0;
    int calls = 0;
    int32_t result = 0;
    bool throwOnCall = false;
};

void completion(void** data, int32_t status)
{
    auto& state = *static_cast<State*>(*data);
    ++state.calls;
    state.result = status;
    // The completed slot must already be available even with the other full.
    state.replacement = state.registry->reserve(nullptr, nullptr, {});
    CHECK(state.replacement != 0);
    if (state.throwOnCall) throw std::runtime_error("callback test");
}
}

int main()
{
    Registry registry;
    State state{&registry};
    void* value = &state;
    Registry::Context context{17, 29, 0x0100007fu, 13000};
    const auto first = registry.reserve(completion, &value, context);
    const auto other = registry.reserve(nullptr, nullptr, {});
    CHECK(first && other && first != other);
    CHECK(registry.size() == Registry::capacity());
    CHECK(registry.reserve(completion, &value, context) == 0);

    Registry::Record record;
    record.userData = &value;
    CHECK(!registry.takeAccepted(first, context, record));
    CHECK(record.userData == &value); // refusal preserves output
    CHECK(registry.markAccepted(first));
    CHECK(!registry.markAccepted(first));
    CHECK(!registry.releaseUnaccepted(first));
    auto wrong = context;
    ++wrong.circuitIncarnation;
    CHECK(!registry.takeAccepted(first, wrong, record));
    CHECK(registry.size() == 2 && record.userData == &value);
    wrong = context; ++wrong.loginGeneration;
    CHECK(!registry.takeAccepted(first, wrong, record));
    wrong = context; ++wrong.addressNetworkOrder;
    CHECK(!registry.takeAccepted(first, wrong, record));
    wrong = context; ++wrong.portHostOrder;
    CHECK(!registry.takeAccepted(first, wrong, record));
    CHECK(registry.takeAccepted(first, context, record));
    CHECK(record.callback == completion && record.userData == &value);
    CHECK(record.context.loginGeneration == 17);
    CHECK(record.context.circuitIncarnation == 29);
    CHECK(record.context.addressNetworkOrder == 0x0100007fu);
    CHECK(record.context.portHostOrder == 13000);
    CHECK(registry.size() == 1);
    record.callback(record.userData, -23017); // original CIRCUIT_GONE status
    CHECK(state.calls == 1 && state.result == -23017);
    CHECK(!registry.takeAccepted(first, context, record));
    CHECK(!registry.releaseUnaccepted(first));
    CHECK(!registry.markAccepted(first)); // old token cannot touch reused slot
    CHECK(registry.size() == 2);
    CHECK(registry.releaseUnaccepted(state.replacement));
    CHECK(registry.releaseUnaccepted(other));
    CHECK(registry.size() == 0);
    CHECK(state.calls == 1); // rejected enqueue never calls back

    for (unsigned i = 0; i < 10000; ++i)
    {
        const auto token = registry.reserve(nullptr, nullptr, context);
        CHECK(token != 0 && token != first);
        CHECK(registry.markAccepted(token));
        CHECK(registry.takeAccepted(token, context, record));
        CHECK(record.callback == nullptr);
        CHECK(!registry.takeAccepted(token, context, record));
    }
    CHECK(!registry.markAccepted(0));
    CHECK(!registry.markAccepted((uint64_t{1} << 16) | 0xffffu));

    const auto throwing = registry.reserve(completion, &value, context);
    CHECK(registry.markAccepted(throwing));
    CHECK(registry.takeAccepted(throwing, context, record));
    state.throwOnCall = true;
    bool caught = false;
    try { record.callback(record.userData, 37); }
    catch (const std::runtime_error&) { caught = true; }
    CHECK(caught && state.result == 37);
    CHECK(!registry.takeAccepted(throwing, context, record));
    CHECK(registry.releaseUnaccepted(state.replacement));
    CHECK(registry.size() == 0);
    std::puts("PASS main callback registry");
}
