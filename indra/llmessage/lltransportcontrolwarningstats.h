/** Bounded coalesced control warnings. SPDX-License-Identifier: LGPL-2.1-only */
#ifndef LL_LLTRANSPORTCONTROLWARNINGSTATS_H
#define LL_LLTRANSPORTCONTROLWARNINGSTATS_H
#include "lltransportcontrol.h"
#include <array>
#include <atomic>
#include <limits>

struct LLTransportControlWarningStats
{
    LLTransportControlWarning last{};
    // coalescedEvents counts publications after the first into a kind's
    // lifetime slot, not events missed since the most recent main read.
    // Once a total saturates, unaccountedEvents counts further affected
    // publications up to its own saturation; saturated makes that explicit.
    uint64_t events=0,truncations=0,coalescedEvents=0,unaccountedEvents=0;
    uint64_t firstTimeUsec=0,lastTimeUsec=0;
    bool saturated=false;
};

// One owner writer, main readers. Per-kind counters never depend on a reader
// draining a queue. All shared fields are atomic; a bounded seqlock read either
// returns one coherent snapshot or leaves the output unchanged. Saturation
// is explicit and never stops the transport.
class LLTransportControlWarningPublication
{
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);
    struct Cell
    {
        std::atomic<uint64_t> sequence{0},events{0},truncations{0},coalesced{0},unaccounted{0},first{0},lastTime{0};
        std::atomic<uint64_t> login{0},incarnation{0},observation{0},offset{0},wanted{0};
        std::atomic<uint32_t> address{0},port{0},message{0},packet{0},lastTruncations{0};
        std::atomic<bool> saturated{false};
        uint64_t next=0,totalEvents=0,totalTruncations=0,totalCoalesced=0,totalUnaccounted=0,firstTime=0;
        bool frozen=false,overflowed=false; // sole owner only
    };
    static std::size_t index(uint32_t number) noexcept
    { return number==1 ? 0 : number==2 ? 1 : number==0xfffffffbu ? 2 : 3; }
    static uint64_t addSaturating(uint64_t value,uint64_t delta,bool& overflow) noexcept
    {
        const auto maximum=(std::numeric_limits<uint64_t>::max)();
        if (delta>maximum-value) { overflow=true; return maximum; }
        return value+delta;
    }
public:
    bool publishOnOwner(const LLTransportControlWarning& warning,uint64_t steadyUsec) noexcept
    {
        const auto i=index(warning.messageNumber);
        if (i>=mCells.size() || !warning.endpoint.addressNetworkOrder || !warning.endpoint.portHostOrder ||
            !warning.loginGeneration || !warning.incarnation ||
            !warning.observation || !warning.truncations) return false;
        auto& c=mCells[i];
        if (c.frozen) return true; // final snapshot explicitly reports unknown overflow
        const auto maximum=(std::numeric_limits<uint64_t>::max)();
        bool overflow=false;
        if (!c.totalEvents) c.firstTime=steadyUsec;
        else c.totalCoalesced=addSaturating(c.totalCoalesced,1,overflow);
        c.totalEvents=addSaturating(c.totalEvents,1,overflow);
        c.totalTruncations=addSaturating(c.totalTruncations,warning.truncations,overflow);
        if (overflow) c.totalUnaccounted=addSaturating(c.totalUnaccounted,1,c.overflowed);
        c.overflowed=c.overflowed || overflow;
        // Reserve the last even sequence for a permanent explicit saturation
        // report rather than wrapping or stopping the owner.
        if (c.next>=maximum-3) { c.overflowed=true; c.totalUnaccounted=maximum; c.frozen=true; }
        c.sequence.store(c.next+1);
        c.events.store(c.totalEvents); c.truncations.store(c.totalTruncations);
        c.coalesced.store(c.totalCoalesced); c.unaccounted.store(c.totalUnaccounted);
        c.first.store(c.firstTime); c.lastTime.store(steadyUsec);
        c.address.store(warning.endpoint.addressNetworkOrder); c.port.store(warning.endpoint.portHostOrder);
        c.login.store(warning.loginGeneration); c.incarnation.store(warning.incarnation); c.observation.store(warning.observation);
        c.offset.store(warning.firstOffset); c.wanted.store(warning.firstWanted);
        c.message.store(warning.messageNumber); c.packet.store(warning.packetId); c.lastTruncations.store(warning.truncations);
        c.saturated.store(c.overflowed); c.next+=2; c.sequence.store(c.next);
        return true;
    }
    bool readOnMain(uint32_t number,LLTransportControlWarningStats& out) const noexcept
    {
        const auto i=index(number); if (i>=mCells.size()) return false;
        const auto& c=mCells[i];
        for (unsigned attempt=0;attempt<3;++attempt)
        {
            const auto before=c.sequence.load(); if (!before || (before&1u)) continue;
            LLTransportControlWarningStats value;
            value.events=c.events.load(); value.truncations=c.truncations.load();
            value.coalescedEvents=c.coalesced.load(); value.unaccountedEvents=c.unaccounted.load();
            value.firstTimeUsec=c.first.load(); value.lastTimeUsec=c.lastTime.load();
            value.last.endpoint={c.address.load(),static_cast<uint16_t>(c.port.load())};
            value.last.loginGeneration=c.login.load(); value.last.incarnation=c.incarnation.load();
            value.last.observation=c.observation.load(); value.last.firstOffset=c.offset.load();
            value.last.firstWanted=c.wanted.load(); value.last.messageNumber=c.message.load();
            value.last.packetId=c.packet.load(); value.last.truncations=c.lastTruncations.load();
            value.saturated=c.saturated.load();
            if (before==c.sequence.load()) { out=value; return true; }
        }
        return false;
    }
#if defined(LL_TRANSPORT_WARNING_TESTING)
    bool testSeedTotalsOnOwner(uint32_t number,uint64_t events,uint64_t truncations) noexcept
    {
        const auto i=index(number); if (i>=mCells.size() || mCells[i].next) return false;
        mCells[i].totalEvents=events; mCells[i].totalTruncations=truncations;
        return true;
    }
#endif
    // After join, absence is a successful result, distinct from read failure.
    bool readFinalOnMain(uint32_t number,LLTransportControlWarningStats& out,bool& present) const noexcept
    {
        const auto i=index(number); if (i>=mCells.size()) return false;
        if (!mCells[i].sequence.load(std::memory_order_acquire)) { present=false; return true; }
        LLTransportControlWarningStats value;
        if (!readOnMain(number,value)) return false;
        out=value; present=true; return true;
    }
private:
    std::array<Cell,3> mCells{};
};
#endif
