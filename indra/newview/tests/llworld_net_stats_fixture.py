#!/usr/bin/env python3
"""Run actual LLWorld counter consumer with deterministic owner publications."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[3]
source = (root / 'indra/newview/llworld.cpp').read_text()
start = source.index('void LLWorld::updateNetStats()')
end = source.index('\n\nvoid LLWorld::printPacketsLost()', start)
body = source[start:end]
prefix = r'''
#include "lltransportringstats.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
using S32=int32_t; using U64=uint64_t; using F32=float;
constexpr S32 S32_MAX=INT32_MAX;
template<class T> T llmin(T a,T b) { return std::min(a,b); }
struct F64Bits {
    double value=0; F64Bits(double v=0):value(v){}
    F64Bits& operator+=(F64Bits other) { value+=other.value; return *this; }
};
using F32Bits=F64Bits;
namespace LLStatViewer { enum Kind {MESSAGE_SYSTEM_DATA_IN,MESSAGE_SYSTEM_DATA_OUT,ACTIVE_MESSAGE_DATA_RECEIVED,PACKETS_IN,PACKETS_OUT,PACKETS_LOST,PACKETS_LOST_PERCENT}; }
double values[7]{}; unsigned adds[7]{};
void add(LLStatViewer::Kind kind,F64Bits v) { values[kind]+=v.value; ++adds[kind]; }
void add(LLStatViewer::Kind kind,S32 v) { values[kind]+=v; ++adds[kind]; }
void sample(LLStatViewer::Kind kind,double v) { values[kind]=v; ++adds[kind]; }
namespace LLUnits { struct Ratio { static double fromValue(double v) { return v; } }; }
struct LLViewerStats {
    double packets=0,lost=0;
    static LLViewerStats& instance() { static LLViewerStats stats; return stats; }
    void addFrameCounts(int in,int drop) { packets+=in; lost+=drop; }
    double getPacketsInTotal() const { return packets; } double getPacketsLostTotal() const { return lost; }
};
struct LLViewerRegion { F64Bits mBitsReceived; double mPacketsReceived=0; void updateNetStats() {} };
struct Messages {
    bool owner=true, known=true; LLTransportRingStats stats;
    S32 mPacketsIn=0,mPacketsOut=0,mDroppedPackets=0;
    bool transportOwnerActive() const { return owner; }
    bool getTransportRingStats(LLTransportRingStats& out) const { if (!known) return false; out=stats; return true; }
    struct Ring {
        unsigned reads=0; S32 getAndResetActualInBits() { ++reads; return 80; }
        S32 getAndResetActualOutBits() { ++reads; return 160; }
    } mPacketRing;
} messages;
Messages* gMessageSystem=&messages;
struct LLWorld {
    using region_list_t=std::vector<LLViewerRegion*>;
    region_list_t mActiveRegionList;
    S32 mLastPacketsIn=0,mLastPacketsOut=0,mLastPacketsLost=0;
    U64 mTransportStatsGeneration=0,mTransportActualIn=0,mTransportActualOut=0,mTransportPacketsIn=0,mTransportPacketsOut=0;
    void updateNetStats();
};
'''
cases = r'''
static void check(bool value,int line) { if (!value) { std::fprintf(stderr,"fail %d\n",line); std::abort(); } }
#define CHECK(x) check(bool(x),__LINE__)
int main() {
    LLWorld world; messages.stats.loginGeneration=1;
    messages.stats.actualIn=10; messages.stats.actualOut=20;
    messages.stats.sessionPacketsIn=2; messages.stats.sessionPacketsOut=3;
    world.updateNetStats();
    CHECK(values[0]==80 && values[1]==160 && values[3]==2 && values[4]==3);
    CHECK(!messages.mPacketRing.reads && !adds[LLStatViewer::PACKETS_LOST] && !adds[LLStatViewer::PACKETS_LOST_PERCENT]);
    world.updateNetStats(); CHECK(values[0]==80 && values[3]==2); // no double harvest
    messages.stats.actualIn=15; messages.stats.sessionPacketsIn=4;
    messages.known=false; world.updateNetStats(); CHECK(values[0]==80 && world.mTransportActualIn==10);
    messages.known=true; world.updateNetStats(); CHECK(values[0]==120 && values[3]==4);
    messages.stats.actualIn=1; world.updateNetStats(); CHECK(world.mTransportActualIn==15 && values[0]==120);
    messages.stats.loginGeneration=2; messages.stats.actualIn=2; messages.stats.actualOut=3;
    messages.stats.sessionPacketsIn=1; messages.stats.sessionPacketsOut=1;
    world.updateNetStats(); CHECK(values[0]==136 && values[1]==184 && values[3]==5 && values[4]==4);
    messages.owner=false; messages.mPacketsIn=7; messages.mPacketsOut=8; messages.mDroppedPackets=1;
    world.updateNetStats(); CHECK(messages.mPacketRing.reads==2 && adds[LLStatViewer::PACKETS_LOST]==1);
}
'''
Path(sys.argv[1]).write_text(prefix + body + cases)
