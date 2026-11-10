// Author: SkyLine. SPDX-License-Identifier: LGPL-2.1-only
// Standalone, no viewer or renderer dependencies.
#include "sltelemetrycore.h"
#include <cassert>
#include <string>
#include <numeric>
int main()
{
    using namespace SLTelemetry;
    assert(!consentEnabled(0) && !consentEnabled(-1) && consentEnabled(1) && !consentEnabled(2));
    assert(needsPrompt(0) && !needsPrompt(-1));
    assert(frameBin(0) == 0 && frameBin(1) == 1 && frameBin(2000) == 65);
    unsigned previous = 0;
    for (double sample = 0; sample < 2100; sample += .01)
    {
        const unsigned bin = frameBin(sample);
        assert(bin >= previous && bin < 66); previous = bin;
    }
    Histogram hist;
    for (const double value : {0., 1., 16., 33., 51., 101., 251., 1001., 2001.}) hist.add(value);
    hist.add(std::numeric_limits<double>::quiet_NaN());
    assert(hist.frames == 9 && hist.over50 == 5 && hist.over100 == 4 && hist.over250 == 3 && hist.over1000 == 2);
    assert(std::accumulate(hist.bins.begin(), hist.bins.end(), std::uint64_t(0)) == hist.frames);
    hist = {};
    hist.frames = (std::numeric_limits<std::uint32_t>::max)(); hist.bins[1] = hist.frames;
    hist.add(1500);
    assert(std::accumulate(hist.bins.begin(), hist.bins.end(), std::uint64_t(0)) == hist.frames && hist.over50 == 0);
    Gauge gauge;gauge.add(0);gauge.add(1);gauge.add(3);gauge.add(7);
    assert(gauge.median()==1&&gauge.peak==3);
    std::uint32_t saturated=4294967290u;addSaturated(saturated,10);assert(saturated==4294967295u);
    assert(durationBucket(1)==0&&durationBucket(5)==2&&durationBucket(80)==6);
    std::array<char,65> model;
    assert(normalizeModel("  AMD   Ryzen 7 7800X3D  ", model));
    assert(std::string(model.data()) == "AMD Ryzen 7 7800X3D");
    for (const auto& bad : {std::string(100000, ' '), std::string(65, 'a'), std::string("C:\\Users\\name"), std::string("/home/name"), std::string("user@example.com"), std::string("AMD Ryzen 7 7800X3D\n")})
    {
        assert(!normalizeModel(bad.c_str(), model));
        assert(model[0] == 0);
    }
    // Actual production clock/boundary policy: deterministic supplied monotonic points.
    using Hours=std::chrono::hours;using Seconds=std::chrono::seconds;
    CollectionClock elapsed;
    const CollectionClock::Clock::time_point activation(Hours(100));
    Window first;
    CollectionClock::Clock::time_point nextSecond{};
    unsigned attempts=0;
    assert(elapsed.hour(activation)==ELAPSED_HOUR_UNKNOWN);
    assert(!elapsed.beforeSample(first,activation,[&]{++attempts;return true;}));
    assert(attempts==0 && first.elapsedHour==ELAPSED_HOUR_UNKNOWN); // off/never-asked
    elapsed.start(activation); // Consent mid-run begins at collection hour zero, not app age100h.
    assert(elapsed.beforeSample(first,activation+Seconds(1),[]{return false;}));
    assert(first.elapsedHour==ELAPSED_HOUR_UNKNOWN); // Before the first accepted data.
    first.frames[0].add(16);first.seconds=1;elapsed.stamp(first,activation+Seconds(1),nextSecond);
    assert(first.elapsedHour==0 && elapsed.acceptsEvent(first,activation+Seconds(3599)));
    assert(!elapsed.acceptsEvent(first,activation+Hours(1)));
    const auto copied=first; // Async/checkpoint snapshot captures old metadata, never live clock.
    for (unsigned jumpedHour:{1u,3u,17u})
    {
        assert(!elapsed.beforeSample(first,activation+Hours(jumpedHour),[&]{++attempts;return false;}));
        assert(first.elapsedHour==0 && first.frames[0].frames==1 && first.seconds==1);
    }
    assert(attempts==3); // One attempt per tick, never an hour-by-hour catch-up loop.
    Window frozen;
    assert(!elapsed.beforeSample(first,activation+Hours(17),[&]
    {frozen=first;first={};++attempts;return true;}));
    assert(frozen.elapsedHour==0 && first.elapsedHour==ELAPSED_HOUR_UNKNOWN);
    assert(elapsed.beforeSample(first,activation+Hours(17)+Seconds(1),[]{return false;}));
    first.frames[0].add(33);elapsed.stamp(first,activation+Hours(17)+Seconds(1),nextSecond);
    assert(first.elapsedHour==17); // Logout/backpressure gaps skip bins1..16; no fabricated reports.
    assert(!elapsed.acceptsEvent(Window{},activation+Hours(17))); // No event before first sample.
    assert(elapsed.hour(activation+Hours(23)+Seconds(3599))==23);
    assert(elapsed.hour(activation+Hours(24))==24 && elapsed.hour(activation+Hours(240))==24);
    Window overflow;elapsed.stamp(overflow,activation+Hours(24),nextSecond);
    assert(elapsed.beforeSample(overflow,activation+Hours(30),[]{return false;}));
    elapsed.start(activation+Hours(40));first={}; // Re-enable/new active collection epoch.
    elapsed.stamp(first,activation+Hours(40)+Seconds(1),nextSecond);
    assert(first.elapsedHour==0 && copied.elapsedHour==0 && frozen.elapsedHour==0);
    assert(elapsed.hour(activation)==ELAPSED_HOUR_UNKNOWN);
    CollectionClock late;late.start(activation);Window noEarlyFrames;
    nextSecond=activation+Seconds(1);
    const auto lateFirst=activation+Hours(3)+Seconds(10);
    assert(late.beforeSample(noEarlyFrames,lateFirst,[]{return false;}));
    noEarlyFrames.frames[0].add(16);
    assert(late.stamp(noEarlyFrames,lateFirst,nextSecond));
    assert(noEarlyFrames.elapsedHour==3 && nextSecond==lateFirst+Seconds(1));
    assert(noEarlyFrames.seconds==0); // No three-hour catch-up attributed to hour3.
    assert(!late.stamp(noEarlyFrames,lateFirst+Seconds(2),nextSecond));

    // Initial report: delay is consent/login anchored, not app age; wait for a
    // settled frame or the bounded 300-second fallback, and seal only after the
    // async queue accepts the handoff.
    InitialSegmentGate initial;
    const auto consentLogin = activation + Hours(80);
    initial.arm(consentLogin);
    assert(!initial.ready(consentLogin+Seconds(119),true,true,false,true));
    assert(!initial.ready(consentLogin+Seconds(120),true,true,false,false));
    assert(!initial.ready(consentLogin+Seconds(120),false,true,false,true)); // logout
    assert(!initial.ready(consentLogin+Seconds(120),true,false,false,true)); // disconnected
    assert(!initial.ready(consentLogin+Seconds(300),true,true,true,true)); // teleport
    assert(initial.ready(consentLogin+Seconds(120),true,true,false,true));
    unsigned initialSchedules=0;
    assert(!initial.tryHandoff(consentLogin+Seconds(120),true,true,false,true,[&]
    {++initialSchedules;return false;})); // worker/backpressure refusal retains gate
    assert(initialSchedules==1&&!initial.handedOff);
    assert(initial.tryHandoff(consentLogin+Seconds(121),true,true,false,true,[&]
    {++initialSchedules;return true;}));
    assert(initialSchedules==2&&initial.handedOff&&!initial.armed);
    initial.consentRevoked();initial.arm(consentLogin+Seconds(1000));
    assert(initial.handedOff&&!initial.armed); // re-consent cannot duplicate initial

    InitialSegmentGate fallback;
    fallback.arm(consentLogin);
    assert(!fallback.ready(consentLogin+Seconds(299),true,true,false,false));
    assert(!fallback.ready(consentLogin+Seconds(300),true,true,false,false,false)); // still in login/teleport settle barrier
    assert(fallback.ready(consentLogin+Seconds(300),true,true,false,false,true)); // bounded busy-arrival fallback
    fallback.consentRevoked();
    assert(!fallback.armed&&!fallback.handedOff);
    fallback.arm(consentLogin+Hours(1)); // never charge time before re-consent
    assert(!fallback.ready(consentLogin+Hours(1)+Seconds(119),true,true,false,true));
    assert(fallback.ready(consentLogin+Hours(1)+Seconds(120),true,true,false,true));
    fallback.restartAfterLogin(consentLogin+Hours(2));
    assert(!fallback.ready(consentLogin+Hours(2)+Seconds(119),true,true,false,true));
    assert(fallback.ready(consentLogin+Hours(2)+Seconds(120),true,true,false,true));

    // Closing the initial window does not reset the consent-hour clock. Hour
    // alignment always runs before the initial gate, including when that gate
    // is ready at the 3599->3600 boundary. Busy/refused hourly handoffs retain
    // the old hour; the boundary frame is dropped as before. After acceptance,
    // the next frame is stamped into the current elapsed hour, then the initial
    // report may close that new window at a normal frame-tick end.
    CollectionClock aligned;aligned.start(consentLogin);
    Window beforeInitial;beforeInitial.frames[0].add(16);beforeInitial.seconds=3599;
    aligned.stamp(beforeInitial,consentLogin+Seconds(1),nextSecond);
    beforeInitial.regionCrossings=1;
    InitialSegmentGate initialAtHour;
    initialAtHour.arm(consentLogin);
    const auto boundary=consentLogin+Hours(1);
    assert(initialAtHour.ready(boundary,true,true,false,true));
    unsigned boundaryFlushes=0, initialAtBoundary=0;
    assert(!aligned.beforeSample(beforeInitial,boundary,[&]
    {
        ++boundaryFlushes;
        return false; // Existing hourly schedule is busy/refused; no initial attempt.
    }));
    assert(boundaryFlushes==1&&initialAtBoundary==0);
    assert(beforeInitial.elapsedHour==0&&beforeInitial.seconds==3599&&beforeInitial.regionCrossings==1);
    assert(initialAtHour.armed&&!initialAtHour.handedOff); // refusal preserves old window and initial eligibility
    assert(!aligned.beforeSample(beforeInitial,boundary+Seconds(1),[&]
    {
        ++boundaryFlushes;
        const auto sealed = beforeInitial;
        beforeInitial = {}; // schedule(2) accepts and resets the live window.
        assert(sealed.elapsedHour==0&&sealed.seconds==3599&&sealed.regionCrossings==1);
        return true; // Existing periodic-hour handoff succeeds first.
    }));
    assert(boundaryFlushes==2&&initialAtBoundary==0);
    assert(beforeInitial.elapsedHour==ELAPSED_HOUR_UNKNOWN); // successful hourly seal still drops boundary frame
    assert(initialAtHour.armed&&!initialAtHour.handedOff); // initial action did not relabel the old-hour report
    Window afterHour;
    assert(aligned.beforeSample(afterHour,boundary+Seconds(2),[]{return false;}));
    afterHour.frames[0].add(33);
    assert(aligned.stamp(afterHour,boundary+Seconds(2),nextSecond));
    assert(afterHour.elapsedHour==1&&afterHour.frames[0].frames==1);
    // Poll cumulative counters before the normal-end-tick initial close. Deltas
    // arriving immediately before this seal belong to exactly this window.
    ++afterHour.cache[0]; ++afterHour.allocFailures[0]; ++afterHour.dataMBLog2[0];
    assert(initialAtHour.tryHandoff(boundary+Seconds(3),true,true,false,true,[]{return true;}));
    const Window sealedInitial=afterHour;
    assert(sealedInitial.cache[0]==1&&sealedInitial.allocFailures[0]==1&&sealedInitial.dataMBLog2[0]==1);
    Window nextInitial;
    assert(aligned.stamp(nextInitial,boundary+Seconds(3),nextSecond));
    assert(nextInitial.elapsedHour==1);
    Window alignedHour;
    assert(!aligned.beforeSample(nextInitial,consentLogin+Hours(2),[]{return true;}));
    assert(aligned.beforeSample(alignedHour,consentLogin+Hours(2)+Seconds(1),[]{return false;}));
    alignedHour.frames[0].add(17);aligned.stamp(alignedHour,consentLogin+Hours(2)+Seconds(1),nextSecond);
    assert(alignedHour.elapsedHour==2);

}
