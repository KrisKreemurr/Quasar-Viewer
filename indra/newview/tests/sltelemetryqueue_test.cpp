// Author: SkyLine. SPDX-License-Identifier: LGPL-2.1-only
// Same production worker queue; no viewer, server listener, GPU or account dependencies.
#include "sltelemetryqueue.h"
#include <boost/json/src.hpp>
#include <cassert>
#include <cstdlib>
#include <future>
#include <new>
#include <thread>
using namespace SLTelemetry;
using namespace SLTelemetry::QueueDetail;
std::atomic<bool> failAllocation{false};
void* operator new(std::size_t bytes)
{
    if (failAllocation.load()) throw std::bad_alloc();
    if (void* memory = std::malloc(bytes ? bytes : 1)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

int main(int argc, char** argv)
{
    assert(argc == 2);
    const fs::path root = fs::temp_directory_path()/"sltelemetry-queue-offline-test";
    fs::remove_all(root);
    sAllowed=false; sEpoch=1; sPurge=false;
    Snapshot hardware; std::strcpy(hardware.version.data(),"1.4.6.1");
    hardware.previousEnd=1;
    Window window; window.elapsedHour=7; window.seconds=3600; window.frames[0].add(16);
    const auto rootName=root.string();
    auto work = [&](unsigned action,bool wantSend=false,std::uint64_t epoch=1)
    { return diskWork(epoch,rootName,window,hardware,0,action,true,wantSend,"",false); };
    // No files/network/collector start while never asked, declined or stale.
    for (unsigned action : {0u,1u,2u,3u,4u}) { assert(work(action).body.empty()); assert(!fs::exists(root)); }
    beforeWrite=[](const fs::path& path)
    {
        if(path.parent_path().filename()=="queue")
            assert(std::distance(fs::directory_iterator(path.parent_path()),fs::directory_iterator{})<8);
    };
    sAllowed=true;
    for (unsigned i=0;i<12;++i)
    {
        work(2);
        assert(std::distance(fs::directory_iterator(root/"queue"),fs::directory_iterator{})<=8);
    }
    assert(std::distance(fs::directory_iterator(root/"queue"),fs::directory_iterator{})==8);
    // Full eight plus crash checkpoint recovery never grows the queue past eight.
    work(1);assert(fs::exists(root/"checkpoint.json"));
    window.elapsedHour=12; // Recovery must not relabel the old checkpoint with a new clock.
    work(3);
    assert(!fs::exists(root/"checkpoint.json"));
    assert(std::distance(fs::directory_iterator(root/"queue"),fs::directory_iterator{})==8);
    bool recovered=false;
    for (const auto& entry:fs::directory_iterator(root/"queue"))
    {
        const auto body=readBody(entry.path());assert(validateReport(body));
        if(boost::json::parse(body).at("segment").at("kind").as_string()=="checkpoint")
        {recovered=true;assert(boost::json::parse(body).at("events").at("prev_session_end").as_string()=="normal");
         assert(boost::json::parse(body).at("segment").at("elapsed_hour").as_int64()==7);}
    }
    assert(recovered);
    // Before send, data is validated again and the exact serialized bytes are returned.
    const auto outgoing=work(0,true);assert(validateReport(outgoing.body));
    assert(readBody(root/"queue"/outgoing.filename)==outgoing.body);
    // The initial segment is a normal queued report and requests immediate send;
    // survey-month marking and the existing bounded queue path still apply.
    const fs::path initialRoot(root.string()+"-initial");
    Window initialWindow;initialWindow.elapsedHour=0;initialWindow.seconds=120;
    initialWindow.frames[0].add(16);
    const auto initialResult=diskWork(1,initialRoot.string(),initialWindow,hardware,0,
        DISK_ACTION_INITIAL,true,true,"",false);
    assert(validateReport(initialResult.body));
    assert(boost::json::parse(initialResult.body).at("segment").at("kind").as_string()=="initial");
    assert(readBody(initialRoot/"queue"/initialResult.filename)==initialResult.body);
    assert(fs::exists(initialRoot/"survey_month"));
    fs::remove_all(initialRoot);
    // Atomic write checks an obsolete epoch; it cannot recreate a directory or a file.
    sAllowed=false;++sEpoch;sPurge=true;
    {std::lock_guard<std::mutex> lock(sDiskMutex);assert(purgeLocked(root,sEpoch));}
    assert(confirmPurge(sEpoch,[](bool){return true;}));
    atomicWrite(root/"forbidden.json",outgoing.body,1);assert(!fs::exists(root));
    assert(work(2,true,1).body.empty());assert(!fs::exists(root));
    // A writer already queued behind the disk mutex is cancelled before it can create files.
    sAllowed=true;sEpoch=3;
    std::unique_lock<std::mutex> lock(sDiskMutex);
    std::promise<void> ready;auto waiting=ready.get_future();
    std::thread writer([&]{ready.set_value();work(2,true,3);});waiting.wait();
    sAllowed=false;++sEpoch;sPurge=true;lock.unlock();writer.join();
    {std::lock_guard<std::mutex> guard(sDiskMutex);assert(purgeLocked(root,sEpoch));}
    assert(confirmPurge(sEpoch,[](bool){return true;}));assert(!fs::exists(root));
    // Rapid off/on cannot revive old captured work. New epoch can write after old purge.
    sAllowed=true;sEpoch=5;assert(work(2,false,3).body.empty());assert(!fs::exists(root));work(2,false,5);assert(fs::exists(root/"queue"));
    // Actual production revocation helpers with settings stored OUTSIDE the deleted queue.
    // A crash before the worker runs must not lose revocation across restart/re-enable.
    const auto stateFile=fs::path(root.string()+"-privacy-state");
    auto saveState=[&](bool pending)
    {
        std::ofstream stream(stateFile,std::ios::trunc);
        stream<<(pending?1:0);stream.close();return !stream.fail();
    };
    auto restart=[&]
    {
        sAllowed=false;sEpoch=1;sRemovedEpoch=0;sPurge=false;
        bool pending=false;std::ifstream(stateFile)>>pending;restorePurge(pending);
    };
    assert(beginPurge(saveState)); // Persist off/delete, intentionally DO NOT run worker.
    assert(fs::exists(root/"queue"));
    restart();assert(sPurge&&!sAllowed);
    sAllowed=true; // Simulate an attempted re-enable. The production gate still blocks it.
    assert(!current(1));assert(work(0,true).body.empty());
    assert(fs::exists(root/"queue"));
    // Erasure failure retains pending, cannot be acknowledged and cannot send.
    beforePurge=[]{throw std::runtime_error("simulated_remove_failure");};
    bool failed=false;try{purgeLocked(root,1);}catch(...){failed=true;}
    assert(failed&&sPurge);assert(!confirmPurge(1,saveState));assert(work(0,true).body.empty());
    beforePurge=nullptr;
    // Successful erase but crash before main confirmation: durable latch remains set.
    assert(purgeLocked(root,1));assert(sPurge&&!fs::exists(root));
    restart();assert(sPurge);sAllowed=true;assert(work(2,true).body.empty());assert(!fs::exists(root));
    assert(purgeLocked(root,1));
    // Failed settings save cannot clear the latch or permit writes, even after removal.
    assert(!confirmPurge(1,[](bool){return false;}));assert(sPurge&&!current(1));
    assert(!confirmPurge(0,saveState)); // Stale worker acknowledgement.
    assert(confirmPurge(1,saveState));assert(!sPurge);
    restart();assert(!sPurge);sAllowed=true;work(2);assert(fs::exists(root/"queue"));
    // Opt-out persistence failure also fails closed in the current process.
    assert(!beginPurge([](bool){return false;}));assert(sPurge&&!sAllowed);
    assert(work(0,true,sEpoch).body.empty());
    assert(purgeLocked(root,sEpoch));assert(confirmPurge(sEpoch,saveState));
    fs::remove(stateFile);sEpoch=5;sAllowed=true;work(2,false,5);
    // Revalidation rejects edited/truncated content, excess length, unknown nested fields.
    fs::remove_all(root/"queue");fs::create_directory(root/"queue");
    std::ofstream(root/"queue"/"00000000000000000000000000000000_0.json")<<"{}";
    work(0,false,5);assert(fs::is_empty(root/"queue"));
    work(2,false,5);
    for(const auto& entry:fs::directory_iterator(root/"queue"))fs::last_write_time(entry.path(),fs::file_time_type::clock::now()-std::chrono::hours(24*31));
    work(0,false,5);assert(fs::is_empty(root/"queue"));
    // Corruption recovery has a fixed scan cap and fixed storage, no vector sized from disk.
    for(unsigned i=0;i<80;++i)std::ofstream(root/"queue"/(std::to_string(i)+".junk"))<<"x";
    work(0,false,5);assert(!fs::exists(root/"queue")||fs::is_empty(root/"queue"));
    // A dropped allocation is contained by the worker, never propagated to the viewer.
    failAllocation=true;work(2,false,5);failAllocation=false;
    // Shared hardware fixtures run against the C++ validator as well as Python.
    std::ifstream fixture(std::string(argv[1])+"/models.json");
    const auto models=boost::json::parse(std::string((std::istreambuf_iterator<char>(fixture)),{}));
    auto data=boost::json::parse(outgoing.body);
    for(const auto& category:models.as_object())
    {
        const char* field=category.key()=="cpu"?"model":"name";
        for(const auto& value:category.value().at("valid").as_array())
        {data.at(category.key()).as_object()[field]=value;assert(validateReport(boost::json::serialize(data)));}
        for(const auto& value:category.value().at("invalid").as_array())
        {data.at(category.key()).as_object()[field]=value;assert(!validateReport(boost::json::serialize(data)));}
        data.at(category.key()).as_object()[field]="other";
    }
    data.as_object()["unknown_attacker_key"]="secret";assert(!validateReport(boost::json::serialize(data)));
    // New metadata is independent of saturated legacy index, and optional for old/off data.
    Window labeled;labeled.elapsedHour=23;labeled.frames[0].add(16);
    auto labeledData=boost::json::parse(buildReport(labeled,hardware,15,"exit",false,true));
    assert(labeledData.at("schema_minor").as_int64()==1);
    assert(labeledData.at("segment").at("index").as_int64()==15);
    assert(labeledData.at("segment").at("elapsed_hour").as_int64()==23);
    labeled.elapsedHour=24;
    assert(boost::json::parse(buildReport(labeled,hardware,15,"periodic",false,true)).at("segment").at("elapsed_hour").as_int64()==24);
    const auto off=boost::json::parse(buildReport(Window{},hardware,0,"periodic",false,false));
    assert(!off.at("segment").as_object().contains("elapsed_hour"));
    auto legacy=labeledData;legacy.as_object()["schema_minor"]=0;legacy.at("segment").as_object().erase("elapsed_hour");
    assert(validateReport(boost::json::serialize(legacy)));
    for(const auto& bad:boost::json::array{-1,25,0.5,true,nullptr,"0"})
    {labeledData.at("segment").as_object()["elapsed_hour"]=bad;assert(!validateReport(boost::json::serialize(labeledData)));}
    // Common real CPU suffixes normalize only recognized clock/core metadata.
    std::strcpy(hardware.rawCPU.data(),"AMD Ryzen 7 7800X3D 8-Core Processor            ");
    auto normalized=boost::json::parse(buildReport(window,hardware,0,"periodic",false,true));
    assert(normalized.at("cpu").at("model").as_string()=="AMD Ryzen 7 7800X3D");
    std::strcpy(hardware.rawCPU.data(),"Intel(R) Core(TM) i7-12700K CPU @ 3.60GHz");
    normalized=boost::json::parse(buildReport(window,hardware,0,"periodic",false,true));
    assert(normalized.at("cpu").at("model").as_string()=="Intel Core i7-12700K");
    std::strcpy(hardware.rawCPU.data(),"Intel Core i7-12700K John Smith CPU");
    normalized=boost::json::parse(buildReport(window,hardware,0,"periodic",false,true));
    assert(normalized.at("cpu").at("model").as_string()=="other");
    // Maximal sparse bins and every allowed setting still fit the hard body budget.
    Window full;
    for(auto& metric:full.frames){metric.bins.fill(1);metric.frames=66;}
    for(unsigned i=0;i<SETTING_COUNT;++i)
    {
        hardware.hasSetting[i]=true;
        hardware.settings[i]=SETTING_RULES[i].bucketCount?SETTING_RULES[i].buckets[SETTING_RULES[i].bucketCount-1]:SETTING_RULES[i].upper;
    }
    const auto maximal=buildReport(full,hardware,15,"periodic",false,true);
    assert(!maximal.empty()&&maximal.size()<=BODY_LIMIT&&validateReport(maximal));
    static_assert(sizeof(Window)<=8192,"collector must stay fixed and bounded");
    // Different report builds always get fresh cryptographic IDs.
    const auto first=buildReport(window,hardware,0,"periodic",false,true);
    const auto second=buildReport(window,hardware,0,"periodic",false,true);
    assert(!first.empty()&&first.size()<=BODY_LIMIT&&identifier(first)!=identifier(second));
    fs::remove_all(root);
}
