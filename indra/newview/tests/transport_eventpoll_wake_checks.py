#!/usr/bin/env python3
"""Compile the actual event-poll wake registration against a native wake.
HTTP request queue/MessageSystem boundaries are fakes; the new registration block and
native wake are production source. This is not an HTTP integration/crossing fixture.
"""
import argparse,json,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument("source",type=Path);p.add_argument("output",type=Path);a=p.parse_args()
r=a.source.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
s=(r/"indra/newview/lleventpoll.cpp").read_text()
start=s.index("            if (gMessageSystem && gMessageSystem->initializeTransportMainWakeOnMain())")
end=s.index("            bool registered = false;",start)
actual=s[start:end]
cpp=r'''#include "lltransportmainwake.cpp"
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>
#define REQUIRE(x) do {if(!(x)){std::fprintf(stderr,"failure at %d: %s\n",__LINE__,#x);std::abort();}}while(false)
struct Message {
 std::shared_ptr<LLTransportMainWake> wake;
 bool allowed=true;
 bool initializeTransportMainWakeOnMain(){if(!allowed)return false;if(!wake)wake=llCreateTransportMainWake();return bool(wake);}
 std::shared_ptr<LLTransportMainWake> transportMainWakeOnMain(){return wake;}
} *gMessageSystem=nullptr;
struct Request {
 std::mutex mutex;
 std::function<void()> notifier;
 bool reject=false;
 void setReplyNotifier(std::function<void()> next){std::lock_guard lock(mutex);if(reject)throw std::bad_alloc();notifier=std::move(next);}
 void deliver(){std::lock_guard lock(mutex);if(notifier)notifier();}
} *mHttpRequest=nullptr;
static void registerWake(){ ACTUAL }
int main(){
 Request request;mHttpRequest=&request;
 registerWake();REQUIRE(!request.notifier);
 Message unavailable;unavailable.allowed=false;gMessageSystem=&unavailable;
 registerWake();REQUIRE(!request.notifier);
 auto message=std::make_unique<Message>();gMessageSystem=message.get();
 registerWake();REQUIRE(request.notifier);auto wake=message->wake;REQUIRE(wake);
 uint64_t observed=0;REQUIRE(wake->observe(observed));
 // Destroy messaging before the HTTP callback: only the shared wake may survive.
 gMessageSystem=nullptr;message.reset();
 std::thread producer([&request]{request.deliver();});
 REQUIRE(wake->waitUntil(observed,LLTransportSteadyUsec(llTransportSteadyNow().value+1000000))==LLTransportWakeResult::Signalled);
 producer.join();
 std::weak_ptr<LLTransportMainWake> weak=wake;wake.reset();REQUIRE(!weak.expired());
 request.setReplyNotifier({});REQUIRE(weak.expired());
 Message next;gMessageSystem=&next;request.reject=true;
 registerWake();REQUIRE(!request.notifier); // registration OOM is contained, request lives
 request.reject=false;registerWake();REQUIRE(request.notifier);
 gMessageSystem=nullptr;request.setReplyNotifier({});
 std::puts("actual event-poll wake registration: PASS");
}
'''.replace("ACTUAL",actual)
source=out/"checks.cpp";source.write_text(cpp)
env=os.environ.copy()
for key in ("TMPDIR","TMP","TEMP","XDG_CACHE_HOME"):env[key]=str(out)
env["CCACHE_DISABLE"]="1"
commands=[]
for mode,flags in (("debug",[]),("ndebug",["-DNDEBUG"])):
 commands += [["/usr/bin/c++","-std=c++20","-DLL_LINUX=1","-Wall","-Wextra","-Werror","-pthread","-O2",*flags,"-I"+str(r/"indra/llmessage"),str(source),"-o",str(out/mode)],[str(out/mode)]]
(out/"commands.json").write_text(json.dumps(commands,indent=2))
for i,command in enumerate(commands):
 with (out/(str(i)+".log")).open("w") as log:subprocess.run(command,cwd=out,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=20,check=True)
 print("PASS",i,flush=True)
