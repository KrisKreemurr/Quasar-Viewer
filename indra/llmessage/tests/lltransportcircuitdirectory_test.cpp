/** Source-only snapshot directory checks. SPDX-License-Identifier: LGPL-2.1-only */
#include "../lltransportcircuitdirectory.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
static void check(bool value, int line) { if (!value) { std::fprintf(stderr,"failed %d\n",line); std::abort(); } }
#define CHECK(x) check(bool(x),__LINE__)
int main()
{
    using Directory=LLTransportCircuitDirectory<8>;
    auto publish=[](Directory& d, const LLTransportCircuitSnapshot& value) {
        if (!d.beginStateMutationOnOwner()) return false;
        if (!d.upsertOnOwner(value)) return false;
        return d.endStateMutationOnOwner();
    };
    auto erase=[](Directory& d, LLTransportEndpoint endpoint, uint64_t incarnation) {
        if (!d.beginStateMutationOnOwner()) return false;
        const bool removed=d.eraseOnOwner(endpoint,incarnation);
        return d.endStateMutationOnOwner() && removed;
    };
    auto directory=std::make_unique<Directory>();
    LLTransportEndpoint ep{42,1234}; LLTransportCircuitSnapshot out;
    out.packetsIn=999; CHECK(!directory->get(ep,out) && out.packetsIn==999);
    CHECK(directory->activateOnOwner());
    CHECK(directory->get(ep,out) && out.state==LLTransportSnapshotState::Absent);
    LLTransportCircuitSnapshot value; value.endpoint=ep; value.state=LLTransportSnapshotState::Present;
    value.incarnation=1; value.publicationSequence=1; value.packetsIn=1;
    CHECK(directory->beginStateMutationOnOwner());
    // State can be changing; this is the last committed observation only.
    CHECK(directory->get(ep,out) && out.state==LLTransportSnapshotState::Absent);
    CHECK(directory->upsertOnOwner(value));
    CHECK(directory->endStateMutationOnOwner());
    CHECK(directory->get(ep,out) && out.incarnation==1 && out.packetsIn==1);
    CHECK(directory->beginStateMutationOnOwner());
    CHECK(directory->get(ep,out) && out.incarnation==1); // no global turn-long read stall
    CHECK(directory->endStateMutationOnOwner());
    CHECK(!erase(*directory,ep,2));
    value.incarnation=2; value.publicationSequence=2; value.packetsIn=2;
    CHECK(publish(*directory,value) && !erase(*directory,ep,1));
    CHECK(directory->get(ep,out) && out.incarnation==2);
    CHECK(erase(*directory,ep,2));
    CHECK(directory->get(ep,out) && out.state==LLTransportSnapshotState::Absent);
    // Reuse tombstones and fill all slots; Full preserves existing records.
    for (uint32_t i=1;i<=8;++i) { value.endpoint={i,1234}; value.incarnation=i; value.publicationSequence=i; CHECK(publish(*directory,value)); }
    value.endpoint={99,1234}; value.incarnation=99; value.publicationSequence=99;
    CHECK(erase(*directory,{4,1234},4));
    CHECK(publish(*directory,value));
    CHECK(directory->get({99,1234},out) && out.incarnation==99);
    // Simulate successful owner state enable for a ninth endpoint followed by
    // directory capacity failure: neither that live endpoint nor an existing
    // one may be misreported as authoritative after publication failure.
    value.endpoint={100,1234}; value.incarnation=100; value.publicationSequence=100;
    CHECK(!publish(*directory,value));
    out.packetsIn=777;
    CHECK(!directory->get({100,1234},out) && out.packetsIn==777);
    CHECK(!directory->get({1,1234},out) && out.packetsIn==777);
    CHECK(!directory->activateOnOwner());
    auto staleDir=std::make_unique<Directory>(); CHECK(staleDir->activateOnOwner());
    auto current=value; current.endpoint=ep; current.incarnation=2; current.publicationSequence=2;
    CHECK(publish(*staleDir,current));
    auto stale=current; stale.incarnation=1; CHECK(!publish(*staleDir,stale));
    CHECK(!staleDir->get(ep,out));
    auto eraseFail=std::make_unique<Directory>(); CHECK(eraseFail->activateOnOwner());
    CHECK(publish(*eraseFail,current));
    CHECK(!erase(*eraseFail,ep,1)); // state unchanged; stale erase is harmless
    CHECK(eraseFail->get(ep,out) && out.incarnation==2);
    eraseFail->failClosedOnOwner(); // required if a state mutation could not publish
    CHECK(!eraseFail->get(ep,out));
    auto concurrent=std::make_unique<Directory>(); CHECK(concurrent->activateOnOwner());
    LLTransportCircuitSnapshot initial; initial.endpoint=ep; initial.state=LLTransportSnapshotState::Present;
    initial.incarnation=1; initial.publicationSequence=1; initial.packetsIn=1;
    CHECK(publish(*concurrent,initial));
    std::atomic<bool> done{false};
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    std::thread owner([&] { for (uint32_t i=2;i<30000;++i) { auto next=initial; next.incarnation=i; next.publicationSequence=i; next.packetsIn=i; CHECK(publish(*concurrent,next)); } done.store(true); });
    while (!done.load())
    {
        LLTransportCircuitSnapshot sample;
        if (concurrent->get(ep,sample)) CHECK(sample.state==LLTransportSnapshotState::Present && sample.incarnation==sample.packetsIn && sample.publicationSequence==sample.packetsIn);
        CHECK(std::chrono::steady_clock::now()<deadline);
    }
    owner.join();
#if defined(LL_TRANSPORT_DIRECTORY_TESTING)
    auto exhausted=std::make_unique<Directory>(); CHECK(exhausted->activateOnOwner());
    exhausted->testSetSequence((std::numeric_limits<uint64_t>::max)()-1);
    CHECK(exhausted->beginStateMutationOnOwner());
    CHECK(!exhausted->upsertOnOwner(initial)); // structural sequence cannot wrap
    out.packetsIn=777; CHECK(!exhausted->get(ep,out) && out.packetsIn==777);
#endif
}
