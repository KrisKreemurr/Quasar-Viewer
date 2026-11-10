/** Actual native wake implementation tests. SPDX-License-Identifier: LGPL-2.1-only */
// Standalone target only: include the actual implementation so the sequence-exhaustion
// fixture can seed its private translation-unit implementation without a public test API.
#include <barrier>
static std::barrier<>* wakeCASBarrier=nullptr;
static void beforeWakeCAS() noexcept { if(wakeCASBarrier)wakeCASBarrier->arrive_and_wait(); }
#define LL_TRANSPORT_WAKE_TEST_BEFORE_CAS beforeWakeCAS
#include "../lltransportmainwake.cpp"
#include <cstdio>
#include <cstdlib>
#include <thread>
#if LL_LINUX
#include <sys/resource.h>
#endif
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr,"wake check failed line %d: %s\n",__LINE__,#x); std::abort(); } } while(false)
static LLTransportSteadyUsec after(uint64_t delta)
{
    return LLTransportSteadyUsec(llTransportSteadyNow().value + delta);
}
static void signalBeforeWait()
{
    auto wake=llCreateTransportMainWake();REQUIRE(wake);
    uint64_t token=0;REQUIRE(wake->observe(token));
    wake->signal();wake->signal();
    REQUIRE(wake->waitUntil(token,after(1000000))==LLTransportWakeResult::Signalled);
    REQUIRE(wake->observe(token));
    const auto deadline=after(10000);
    REQUIRE(wake->waitUntil(token,deadline)==LLTransportWakeResult::Deadline);
    REQUIRE(llTransportSteadyNow().value>=deadline.value);
}
static void signalDuringWait()
{
    auto wake=llCreateTransportMainWake();REQUIRE(wake);
    uint64_t token=0;REQUIRE(wake->observe(token));
    std::atomic<bool> published{false};
    std::thread producer([wake,&published] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        published.store(true,std::memory_order_relaxed);
        wake->signal();
    });
    REQUIRE(wake->waitUntil(token,after(1000000))==LLTransportWakeResult::Signalled);
    REQUIRE(published.load(std::memory_order_relaxed));
    producer.join();
}
static void racingPublication()
{
    auto wake=llCreateTransportMainWake();REQUIRE(wake);
    std::atomic<uint32_t> armed{0},payload{0};
    constexpr uint32_t iterations=5000;
    std::thread producer([wake,&armed,&payload] {
        for(uint32_t i=1;i<=iterations;++i) {
            const auto expiry=after(2000000);
            while(armed.load(std::memory_order_acquire)!=i) {
                REQUIRE(llTransportSteadyNow().value<expiry.value);
                std::this_thread::yield();
            }
            payload.store(i,std::memory_order_relaxed);
            wake->signal();
        }
    });
    for(uint32_t i=1;i<=iterations;++i) {
        uint64_t token=0;REQUIRE(wake->observe(token));
        armed.store(i,std::memory_order_release);
        if((i&3)==0)std::this_thread::yield();
        REQUIRE(wake->waitUntil(token,after(2000000))==LLTransportWakeResult::Signalled);
        REQUIRE(payload.load(std::memory_order_relaxed)>=i);
    }
    producer.join();
}
static void exhaustedTokenNeverSleeps()
{
    NativeMainWake wake((std::numeric_limits<uint64_t>::max)());
    uint64_t token=0;REQUIRE(wake.observe(token));
    REQUIRE(token==(std::numeric_limits<uint64_t>::max)());
    wake.signal();
    uint64_t untouched=71;REQUIRE(!wake.observe(untouched));REQUIRE(untouched==71);
    REQUIRE(wake.waitUntil(token,after(1000000))==LLTransportWakeResult::Unavailable);
    wake.signal();REQUIRE(!wake.observe(untouched));REQUIRE(untouched==71);
}
static void racingExhaustionNeverPublishesWrappedToken()
{
    const uint64_t initial=(std::numeric_limits<uint64_t>::max)()-1;
    NativeMainWake wake(initial);
    std::barrier gate(3);wakeCASBarrier=&gate;
    std::atomic<unsigned> completed{0};
    std::thread first([&]{wake.signal();completed.fetch_add(1,std::memory_order_release);});
    std::thread second([&]{wake.signal();completed.fetch_add(1,std::memory_order_release);});
    // Both producers hold the same pre-max value before attempting their CAS.
    gate.arrive_and_wait();
    while(completed.load(std::memory_order_acquire)!=2) {
        uint64_t token=initial;
        if(wake.observe(token))REQUIRE(token>=initial);
        std::this_thread::yield();
    }
    first.join();second.join();wakeCASBarrier=nullptr;
    uint64_t token=19;REQUIRE(!wake.observe(token));REQUIRE(token==19);
    REQUIRE(wake.waitUntil(initial,after(1000000))==LLTransportWakeResult::Unavailable);
}
static void nativeResourceFailureIsSoft()
{
#if LL_LINUX
    rlimit previous{};REQUIRE(getrlimit(RLIMIT_NOFILE,&previous)==0);
    rlimit empty=previous;empty.rlim_cur=0;
    REQUIRE(setrlimit(RLIMIT_NOFILE,&empty)==0);
    auto wake=llCreateTransportMainWake();
    const int restored=setrlimit(RLIMIT_NOFILE,&previous);
    REQUIRE(restored==0);REQUIRE(!wake);
#endif
}
int main()
{
    signalBeforeWait();signalDuringWait();racingPublication();
    exhaustedTokenNeverSleeps();racingExhaustionNeverPublishesWrappedToken();nativeResourceFailureIsSoft();
    std::puts("actual native transport wake: PASS (5000 races, deadlines, exhaustion, resource failure)");
}
