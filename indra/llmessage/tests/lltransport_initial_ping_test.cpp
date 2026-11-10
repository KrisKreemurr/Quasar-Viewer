// Actual owner circuit state: first ping-based reliable/semireliable deadline.
#include "../lltransportcircuitstate.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

static void check(bool ok,int line) { if (!ok) { std::fprintf(stderr,"failed line %d\n",line); std::abort(); } }
#define CHECK(x) check(bool(x),__LINE__)
int main()
{
    using State=LLTransportCircuitState;
    for (const bool retryBased : {false,true})
    {
        State state; uint64_t charged=0; CHECK(state.init({1,1,1,1,1,1,8388608},charged));
        const LLTransportEndpoint endpoint{0x0100007f,1234};
        const auto enabled=state.enable(endpoint,7,true,100,{1000000,60000000,0.f,0.5f});
        CHECK(enabled.status==State::Status::Ok);
        CHECK(state.notePingReply(endpoint,enabled.incarnation,0,1000000,1900100)==State::Status::Ok);
        float ping=0.f; CHECK(state.effectivePingMilliseconds(endpoint,enabled.incarnation,1000000,2000000,ping)==State::Status::Ok);
        const auto expected=static_cast<uint64_t>(std::ceil(static_cast<double>(std::max(1.f,5.f*ping/1000.f))*1000000.0));
        CHECK(state.reserveReliableSlot(endpoint,enabled.incarnation,7,11)==State::Status::Ok);
        CHECK(state.acceptReliableSlot(11)==State::Status::Ok);
        uint32_t packet=0; CHECK(state.allocateReliablePacketId(11,packet)==State::Status::Ok);
        const uint8_t bytes[]={0x40,0,0,0,1,0,42};
        State::RetryPolicy policy; policy.retries=retryBased ? 1 : 0; policy.firstIntervalUsec=1;
        policy.pingBasedInitial=true; policy.pingBasedRetry=retryBased;
        CHECK(state.prepareReliable(11,packet,bytes,sizeof(bytes),policy,2000000)==State::Status::Ok);
        const auto first=state.advance(2000000,0); CHECK(first.kind==State::ActionKind::InitialSend);
        CHECK(state.commitAttempt(first.token,2000000,0,true,sizeof(bytes))==State::Status::Ok);
        CHECK(state.advance(2000000+expected,0).kind==State::ActionKind::None);
        const auto due=state.advance(2000001+expected,0);
        if (retryBased) CHECK(due.kind==State::ActionKind::Resend);
        else { State::Completion completion; CHECK(due.kind==State::ActionKind::None &&
            state.takeCompletion(completion) && completion.token==11 && completion.result==-23016); }
    }
}
