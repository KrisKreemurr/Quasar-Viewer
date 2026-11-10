#!/usr/bin/env python3
"""Compile actual main consumer bodies with fake value publications, no legacy table."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[3]
message = (root / 'indra/llmessage/message.cpp').read_text()
xfer = (root / 'indra/llmessage/llxfermanager.cpp').read_text()
usage = (root / 'indra/newview/sldatausage.cpp').read_text()

def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

bodies = '\n'.join(function(message, signature) for signature in (
    'bool LLMessageSystem::isTrustedSender(const LLHost&',
    'bool    LLMessageSystem::getCircuitTrust(',
    'bool LLMessageSystem::checkCircuitBlocked(',
    'bool LLMessageSystem::checkCircuitAlive(const U32',
    'bool LLMessageSystem::checkCircuitAlive(const LLHost'))
xfer_start = xfer.index('void LLXferManager::retransmitUnackedPackets()')
xfer_end = xfer.index('    // Re-build mOutgoingHosts data', xfer_start)
bodies += '\n' + xfer[xfer_start:xfer_end] + '}\n'
overflow_start = xfer.index('                LLTransportCircuitSnapshot circuit;', xfer.index('void LLXferManager::processFileRequest'))
overflow_if = xfer.index('                if (!known)', overflow_start)
overflow_end = overflow_if + len(function(xfer[overflow_if:], '                if (!known)'))
bodies += '\nvoid LLXferManager::checkOverflow(LLXfer* xferp) {\n' + xfer[overflow_start:overflow_end] + '\n}\n'
bodies += function(usage, '    UdpTotals udp_totals()')
transfer = (root / 'indra/llmessage/lltransfermanager.cpp').read_text()
transfer_body = function(transfer, 'void LLTransferSourceChannel::updateTransfers()')
assert transfer_body.index('transportOwnerActive()') < transfer_body.index('mCircuitInfo')

prefix = r'''
#include "lltransportcircuittypes.h"
#include "lltransportringstats.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <string>
using U32=uint32_t; using U64=uint64_t;
struct Log { template<class T> Log& operator<<(const T&) { return *this; } };
#define LL_WARNS(...) Log{}
#define LL_DEBUGS(...) Log{}
#define LL_INFOS(...) Log{}
#define LL_ENDL 0
struct LLHost { bool valid=true; bool isOk() const { return valid; } std::string getIPandPort() const { return "host"; } };
struct LLMessageSystem {
    bool known=true, owner=true, ringKnown=true; U64 generation=1;
    LLTransportCircuitSnapshot circuit;
    LLTransportRingStats ring;
    mutable unsigned reads=0;
    // No mCircuitInfo exists in this fixture: any raw circuit read fails compilation.
    struct LegacyRing {
        U64 getSessionBytesIn() const { return 101; } U64 getSessionBytesOut() const { return 102; }
        U64 getSessionPacketsIn() const { return 103; } U64 getSessionPacketsOut() const { return 104; }
    } mPacketRing;
    bool getCircuitSnapshot(const LLHost&,LLTransportCircuitSnapshot& out) const {
        ++reads; if (!known) return false; out=circuit; return true;
    }
    bool getTransportRingStats(LLTransportRingStats& out) const { if (!ringKnown) return false; out=ring; return true; }
    bool transportOwnerActive() const { return owner; }
    U64 transportLoginGenerationOnMain() const { return generation; }
    LLHost findHost(U32 code) { return {code!=0}; }
    bool isTrustedSender(const LLHost&) const;
    bool getCircuitTrust(const LLHost&);
    bool checkCircuitBlocked(U32);
    bool checkCircuitAlive(U32);
    bool checkCircuitAlive(const LLHost&);
} messages;
LLMessageSystem* gMessageSystem=&messages;
constexpr int e_LL_XFER_IN_PROGRESS=1, LL_ERR_CIRCUIT_GONE=-23017, LL_ERR_CANNOT_OPEN_FILE=-42;
unsigned completed=0, destroyed=0, aborted=0;
struct LLXfer {
    int mStatus=e_LL_XFER_IN_PROGRESS, mCallbackResult=0; LLHost mRemoteHost;
    void processEOF() { if (mCallbackResult!=LL_ERR_CIRCUIT_GONE) std::abort(); ++completed; }
    void abort(int result) { if (result!=LL_ERR_CANNOT_OPEN_FILE) std::abort(); ++aborted; }
    ~LLXfer() { ++destroyed; }
};
struct LLXferManager {
    using xfer_list_t=std::list<LLXfer*>;
    xfer_list_t mReceiveList, mSendList;
    void retransmitUnackedPackets();
    void checkOverflow(LLXfer*);
    void removeXfer(LLXfer* value,xfer_list_t& list) { list.remove(value); delete value; }
};
struct UdpTotals { U64 mBytesIn=0,mBytesOut=0,mPacketsIn=0,mPacketsOut=0; };
'''
cases = r'''
static void check(bool value,int line) { if (!value) { std::fprintf(stderr,"fail %d\n",line); std::abort(); } }
#define CHECK(x) check(bool(x),__LINE__)
int main() {
    LLHost host;
    messages.circuit.state=LLTransportSnapshotState::Present;
    messages.circuit.alive=true; messages.circuit.trusted=true;
    CHECK(messages.checkCircuitAlive(host) && messages.checkCircuitAlive(1));
    CHECK(messages.isTrustedSender(host) && messages.getCircuitTrust(host));
    CHECK(!messages.checkCircuitBlocked(1));
    messages.circuit.blocked=true; CHECK(messages.checkCircuitBlocked(1));
    messages.known=false;
    CHECK(!messages.checkCircuitAlive(host) && !messages.getCircuitTrust(host) && messages.checkCircuitBlocked(1));
    messages.known=true; messages.circuit.state=LLTransportSnapshotState::Absent;
    CHECK(!messages.checkCircuitAlive(host) && !messages.isTrustedSender(host) && !messages.checkCircuitBlocked(1));
    CHECK(!messages.checkCircuitAlive(0) && messages.checkCircuitBlocked(0));
    LLXferManager manager; manager.mReceiveList.push_back(new LLXfer);
    messages.circuit.state=LLTransportSnapshotState::Present; messages.circuit.alive=true;
    manager.retransmitUnackedPackets(); CHECK(manager.mReceiveList.size()==1 && !completed);
    messages.known=false;
    manager.retransmitUnackedPackets(); CHECK(manager.mReceiveList.size()==1 && !completed);
    messages.known=true; messages.circuit.state=LLTransportSnapshotState::Unavailable;
    manager.retransmitUnackedPackets(); CHECK(manager.mReceiveList.size()==1 && !completed);
    messages.circuit.state=LLTransportSnapshotState::Absent;
    manager.retransmitUnackedPackets(); CHECK(manager.mReceiveList.empty() && completed==1 && destroyed==1);
    manager.mReceiveList.push_back(new LLXfer);
    messages.circuit.state=LLTransportSnapshotState::Present; messages.circuit.alive=false;
    manager.retransmitUnackedPackets(); CHECK(manager.mReceiveList.empty() && completed==2 && destroyed==2);
    // The request has already been inserted before the overflow check. A
    // thousand unknown reads must reject each new item, preserving the old
    // bounded prefix and never treating contention as a circuit close.
    for (unsigned i=0;i<500;++i) manager.mSendList.push_back(new LLXfer);
    messages.known=false;
    for (unsigned i=0;i<1000;++i) {
        auto* request=new LLXfer; manager.mSendList.push_front(request);
        manager.checkOverflow(request); CHECK(manager.mSendList.size()==500);
    }
    CHECK(aborted==1000 && destroyed==1002);
    messages.known=true; messages.circuit.state=LLTransportSnapshotState::Unavailable;
    auto* request=new LLXfer; manager.mSendList.push_front(request);
    manager.checkOverflow(request); CHECK(manager.mSendList.size()==500 && aborted==1001);
    for (auto* value:manager.mSendList) delete value;
    manager.mSendList.clear();
    messages.ring={}; messages.ring.loginGeneration=1;
    messages.ring.sessionBytesIn=50; messages.ring.sessionBytesOut=75;
    messages.ring.sessionPacketsIn=2; messages.ring.sessionPacketsOut=3;
    auto totals=udp_totals(); CHECK(totals.mBytesIn==50 && totals.mPacketsOut==3);
    messages.ringKnown=false; totals=udp_totals(); CHECK(totals.mBytesIn==50 && totals.mPacketsOut==3);
    messages.generation=2; totals=udp_totals(); CHECK(totals.mBytesIn==0 && totals.mPacketsOut==0);
    messages.ringKnown=true; messages.ring.loginGeneration=2; messages.ring.sessionPacketsOut=5;
    totals=udp_totals(); CHECK(totals.mPacketsOut==5);
    messages.owner=false; totals=udp_totals(); CHECK(totals.mBytesIn==101 && totals.mPacketsOut==104);
}
'''
Path(sys.argv[1]).write_text(prefix + bodies + cases)
