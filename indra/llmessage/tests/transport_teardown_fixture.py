#!/usr/bin/env python3
"""Generate an actual-source teardown/reader lifetime fixture; no viewer or GPU."""
import hashlib
import pathlib
import sys

message = pathlib.Path(__file__).resolve().parents[1]
root = message.parents[1]
header = (message / 'message.h').read_text()
source = (message / 'message.cpp').read_text()
locks = header[header.index('class LLMessageReaderPointer\n'):header.index('/**\n * LockMessageReader is great')]
start = source.index('namespace\n{\n// Main-thread only. A reentrant shutdown request')
shutdown = source[start:source.index('\nvoid LLMessageSystem::resetReceiveCounts()', start)]
prefix = r'''
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <thread>
std::atomic<unsigned> lockAttempts{0};
namespace LLCoros {
using Mutex = std::mutex;
struct LockType {
    std::unique_lock<std::mutex> lock;
    explicit LockType(Mutex& m):lock(m,std::defer_lock) { ++lockAttempts; lock.lock(); }
};
}
struct LLMessageReader {};
'''
middle = r'''
void end_messaging_system(bool);
std::atomic<unsigned> deleted{0}, cleanups{0}, vfileCleanups{0};
struct LLMessageSystem {
    LLMessageReaderPointer mMessageReader;
    bool active = false;
    bool hasMessageReaderUsersOnMain() const noexcept { return mMessageReader.hasUsers(); }
    bool transportOwnerActive() const noexcept { return active; }
    bool requestTransportOwnerStopOnMain() noexcept { return true; }
    void stopLogging() {}
    void summarizeLogs(std::ostringstream&) {}
    ~LLMessageSystem() { ++deleted; }
};
LLMessageSystem* gMessageSystem = nullptr;
struct Transfers { void cleanup() { ++cleanups; end_messaging_system(false); } } gTransferManager;
struct LLTransferTargetVFile { static void updateQueue(bool) { ++vfileCleanups; } };
struct Log { template<class T> Log& operator<<(const T&) { return *this; } };
#define LL_INFOS(...) Log()
#define LL_ENDL 0
'''
suffix = r'''
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"failed line %d\n",__LINE__); std::abort(); } } while(false)
template<class P> void until(P pred) {
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (!pred()) { if (std::chrono::steady_clock::now()>deadline) std::abort(); std::this_thread::yield(); }
}
int main() {
    LLMessageReader reader; gMessageSystem=new LLMessageSystem;
    auto* first=new LockMessageReader(gMessageSystem->mMessageReader,&reader);
    std::atomic<bool> acquired{false},release{false};
    std::thread waiter([&] { LockMessageReader next(gMessageSystem->mMessageReader,&reader); acquired=true; until([&]{return release.load();}); });
    until([]{return lockAttempts.load()>=2;});
    end_messaging_system(false);
    CHECK(messagingTeardownPendingOnMain() && deleted==0 && cleanups==0);
    CHECK(!finishDeferredMessagingTeardownOnMain());
    delete first; until([&]{return acquired.load();});
    CHECK(!finishDeferredMessagingTeardownOnMain() && deleted==0);
    release=true; waiter.join();
    CHECK(finishDeferredMessagingTeardownOnMain() && !messagingTeardownPendingOnMain());
    CHECK(!gMessageSystem && deleted==1 && cleanups==1 && vfileCleanups==1);
    gMessageSystem=new LLMessageSystem; gMessageSystem->active=true;
    end_messaging_system(false); CHECK(messagingTeardownPendingOnMain() && cleanups==1);
    CHECK(!finishDeferredMessagingTeardownOnMain());
    gMessageSystem->active=false; CHECK(finishDeferredMessagingTeardownOnMain());
    CHECK(!gMessageSystem && deleted==2 && cleanups==2 && vfileCleanups==2);
}
'''
output=pathlib.Path(sys.argv[1]).resolve()
if not output.is_relative_to(root): raise SystemExit('output must be in this worktree')
output.parent.mkdir(parents=True,exist_ok=True)
text=prefix+locks+middle+shutdown+suffix
output.write_text(text)
print('reader_source_sha256',hashlib.sha256(locks.encode()).hexdigest())
print('shutdown_source_sha256',hashlib.sha256(shutdown.encode()).hexdigest())
print('fixture_sha256',hashlib.sha256(text.encode()).hexdigest())
