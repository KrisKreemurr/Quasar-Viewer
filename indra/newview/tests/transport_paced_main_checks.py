#!/usr/bin/env python3
"""Generate a CPU fixture using the actual paced helper, native wake and Boost fibers.
Message facade/HTTP and viewer handlers are fake endpoints, not a live circuit test.
Generation is separate from compilation so the parent's serial validation slot applies.
"""
import argparse
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('source', type=Path)
p.add_argument('output', type=Path)
a=p.parse_args(); root=a.source.resolve(); out=a.output.resolve(); out.mkdir(parents=True,exist_ok=True)
s=(root/'indra/newview/llappviewer.cpp').read_text()
start=s.index('bool serviceTransportUntilRenderDeadline(')
end=s.index('\n} // namespace',start)
actual=s[start:end]
cpp=r'''#include "lltransportmainwake.cpp"
#include <algorithm>
#include <boost/fiber/all.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <thread>
#include <vector>
using S64=long long;
#define REQUIRE(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();}}while(false)
bool gDoDisconnect=false,gDisconnected=false,pending=false;
unsigned gFrameCount=17,readers=0,udp=0,completed=0,pumps=0;
bool messagingTeardownPendingOnMain() noexcept{return pending;}
struct Message {
 bool active=true,stopInHandler=false,detachInHandler=false;
 std::shared_ptr<LLTransportMainWake> wake=llCreateTransportMainWake();
 bool transportOwnerActive()const{return active;}
 auto transportMainWakeOnMain()const{return wake;}
 void pumpTransportCompletions(unsigned limit){REQUIRE(readers==1);REQUIRE(limit==64);++completed;}
};
using LLMessageSystem=Message;
Message* gMessageSystem=nullptr;
struct LockMessageChecker {
 Message* message;
 explicit LockMessageChecker(Message*m):message(m){++readers;}
 ~LockMessageChecker(){--readers;}
 bool checkMessages(S64 frame){REQUIRE(frame==17);if(udp==0)return false;--udp;if(message->stopInHandler)pending=true;if(message->detachInHandler)message->active=false;return true;}
};
namespace llcoro { void suspend(){boost::this_fiber::yield();} }
std::deque<unsigned> capabilities;
std::vector<unsigned> delivered;
boost::fibers::promise<void>* reply=nullptr;
bool responseSeen=false,stopInHttp=false,detachInHttp=false;
struct LLEventPoll {
 static void serviceMainThread(unsigned budget){
  REQUIRE(!readers);REQUIRE(budget==1000);++pumps;
  if(stopInHttp){pending=true;return;}
  if(detachInHttp){gMessageSystem->active=false;return;}
  if(reply&&!responseSeen){responseSeen=true;reply->set_value();}
  for(unsigned n=0;n<256&&!capabilities.empty();++n){delivered.push_back(capabilities.front());capabilities.pop_front();}
 }
};
ACTUAL
int main(){
 using Clock=std::chrono::steady_clock;
 Message message;REQUIRE(message.wake);gMessageSystem=&message;
 boost::fibers::promise<void> promise;reply=&promise;auto future=promise.get_future();
 bool continued=false;
 boost::fibers::fiber eventPoll([&]{future.get();continued=true;for(unsigned n=0;n<700;++n)capabilities.push_back(n);message.wake->signal();});
 // Simulate a one-second render deadline. Coroutine must run and all capability
 // events must drain while no frame/AgentUpdate/render is performed.
 auto started=Clock::now();udp=900;
 REQUIRE(serviceTransportUntilRenderDeadline(started+std::chrono::seconds(1)));
 eventPoll.join();REQUIRE(continued);REQUIRE(responseSeen);REQUIRE(delivered.size()==700);
 for(unsigned n=0;n<700;++n)REQUIRE(delivered[n]==n);
 REQUIRE(gFrameCount==17);REQUIRE(!readers);REQUIRE(udp==0);REQUIRE(completed>0);
 REQUIRE(pumps>1);REQUIRE(Clock::now()-started<std::chrono::seconds(3));
 reply=nullptr;stopInHttp=true;
 REQUIRE(serviceTransportUntilRenderDeadline(Clock::now()+std::chrono::seconds(1)));REQUIRE(!readers);
 stopInHttp=false;pending=false;message.stopInHandler=true;udp=1;
 REQUIRE(serviceTransportUntilRenderDeadline(Clock::now()+std::chrono::seconds(1)));REQUIRE(pending);REQUIRE(!readers);
 pending=false;message.stopInHandler=false;message.detachInHandler=true;udp=2;
 REQUIRE(serviceTransportUntilRenderDeadline(Clock::now()+std::chrono::seconds(1)));REQUIRE(udp==1);REQUIRE(!readers);
 message.detachInHandler=false;message.active=true;detachInHttp=true;udp=2;
 REQUIRE(serviceTransportUntilRenderDeadline(Clock::now()+std::chrono::seconds(1)));REQUIRE(udp==2);REQUIRE(!readers);
 detachInHttp=false;message.active=true;
 boost::fibers::fiber detach([&]{message.active=false;});
 REQUIRE(serviceTransportUntilRenderDeadline(Clock::now()+std::chrono::seconds(1)));detach.join();REQUIRE(udp==2);REQUIRE(!readers);
 message.active=false;auto before=pumps;
 REQUIRE(!serviceTransportUntilRenderDeadline(Clock::now()+std::chrono::seconds(1)));REQUIRE(before==pumps);
 std::puts("actual paced helper + native wake + real Boost coroutine handoff: PASS");
}
'''.replace('ACTUAL',actual)
(out/'checks.cpp').write_text(cpp)
print(out/'checks.cpp')
