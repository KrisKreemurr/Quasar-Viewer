/** Actual ring stats publication checks. SPDX-License-Identifier: LGPL-2.1-only */
#include "../lltransportringstats.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

static void check(bool value,int line) { if (!value) { std::fprintf(stderr,"failed %d\n",line); std::abort(); } }
#define CHECK(x) check(bool(x),__LINE__)
int main()
{
    LLTransportRingPublication first;
    LLTransportRingStats result; result.loginGeneration=99;
    CHECK(!first.readOnMain(result) && result.loginGeneration==99);
    CHECK(first.publishOnOwner(1,10,20,10,20,1,2,17,136));
    CHECK(first.readOnMain(result) && result.loginGeneration==1 && result.actualIn==10 && result.actualOut==20 &&
          result.bufferedPackets==17 && result.bufferedBytes==136);
    LLTransportRingPublication replacement;
    CHECK(replacement.publishOnOwner(2,100,200,100,200,3,4,0,0));
    CHECK(replacement.readOnMain(result) && result.loginGeneration==2 && result.actualIn==100);
    // A larger counter on a replacement is still a distinct generation.
    CHECK(!replacement.publishOnOwner(3,1,1,101,201,4,5,0,0));
    result.loginGeneration=77; CHECK(!replacement.readOnMain(result) && result.loginGeneration==77);
    LLTransportRingPublication invalid;
    CHECK(!invalid.publishOnOwner(0,1,1,1,1,1,1,0,0));
    CHECK(!invalid.readOnMain(result) && result.loginGeneration==77);

    LLTransportRingPublication concurrent;
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (uint64_t i=1;i<=20000;++i)
        { const auto packets=static_cast<uint32_t>(i%1025); CHECK(concurrent.publishOnOwner(9,1,2,i,2*i,i,2*i,packets,packets*8)); }
        done.store(true);
    });
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
    while (!done.load())
    {
        LLTransportRingStats sample;
        if (concurrent.readOnMain(sample))
            CHECK(sample.loginGeneration==9 && sample.actualIn==sample.sessionBytesIn &&
                  sample.actualOut==sample.sessionBytesOut && sample.sessionPacketsIn==sample.actualIn &&
                  sample.sessionPacketsOut==sample.actualOut && sample.bufferedPackets==sample.actualIn%1025 &&
                  sample.bufferedBytes==sample.bufferedPackets*8);
        CHECK(std::chrono::steady_clock::now()<deadline);
    }
    writer.join();
    CHECK(concurrent.readOnMain(result) && result.loginGeneration==9 && result.actualIn==20000);
}
