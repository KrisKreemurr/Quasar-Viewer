/** Main-thread boundary for an eventual transport owner. SPDX-License-Identifier: LGPL-2.1-only */
#ifndef LL_LLTRANSPORTMAINFACADE_H
#define LL_LLTRANSPORTMAINFACADE_H
#include "lltransportcircuittypes.h"
#include "lltransportingress.h"
#include "lltransportringstats.h"
#include "lltransportcontrolwarningstats.h"
#include <cstdint>

// All calls are main-thread only. Implementations communicate with the owner
// using bounded value storage: they must not call a viewer handler on the owner,
// return a borrowed circuit pointer, or wait for a mutex held by main. Failure
// leaves output arguments unchanged. This interface cannot enable the actor.
enum class LLTransportCommandOffer : uint8_t { Accepted, Full, Invalid, Unavailable };
struct LLTransportBacklog
{
    // Buffered packet-ring values, not published APP ingress or kernel data.
    // loadRate preserves the legacy ring's bufferedPackets / 256 scale.
    // Legacy pre-owner snapshots carry generation zero.
    uint32_t pendingPackets = 0;
    uint64_t pendingBytes = 0;
    float loadRate = 0.f;
    uint64_t loginGeneration = 0;
};
struct LLTransportOwnerControl;
struct LLTransportOwnerFatal;
struct LLTransportOwnerCommand;
class LLTransportMainFacade
{
public:
    virtual ~LLTransportMainFacade() = default;
    // A successful read may say Absent or Present. False means unavailable;
    // it must never be treated as authoritative absence.
    virtual bool getCircuitSnapshot(LLTransportEndpoint endpoint, LLTransportCircuitSnapshot& out) const noexcept = 0;
    // Accepted reserves a terminal completion until main consumes it. Commands
    // and sends share owner order; owner-side conditional close must finish
    // before main removes a region. Unavailable must not mutate legacy state.
    virtual LLTransportCommandOffer submitCircuitCommand(const LLTransportCircuitCommand& command) noexcept = 0;
    // Main nonreentrant scope includes callback reservation and rollback.
    virtual bool beginOfferOnMain() noexcept { return false; }
    virtual LLTransportCommandOffer offerReservedOnMain(const LLTransportOwnerCommand&) noexcept
    { return LLTransportCommandOffer::Unavailable; }
    virtual void endOfferOnMain() noexcept {}
    // Before popping a barrier-delayed close, main reserves bounded pending
    // storage; circuit terminals have a separate lane from reliable sends.
    virtual bool popCircuitCompletion(LLTransportCircuitCompletion& out) noexcept = 0;
    // Peek keeps the terminal and its credit until the caller has classified
    // it and explicitly released it; both operations are main-consumer only.
    virtual bool peekCircuitCompletion(LLTransportCircuitCompletion&) noexcept { return false; }
    virtual bool releaseCircuitCompletion() noexcept { return false; }
    virtual bool peekOwnerControl(LLTransportOwnerControl&) noexcept { return false; }
    virtual bool releaseOwnerControl() noexcept { return false; }
    virtual bool peekOwnerFatal(LLTransportOwnerFatal&) noexcept { return false; }
    virtual bool releaseOwnerFatal() noexcept { return false; }
    // Coalesced warning values under the frozen no-callback policy; no owner
    // callback or raw packet pointer crosses to main.
    virtual bool controlWarningStats(uint32_t, LLTransportControlWarningStats&) const noexcept { return false; }
    // Main peeks a terminal value; its owner credit is released only after
    // releaseReliableCompletion. Both methods are main-thread only.
    virtual bool peekReliableCompletion(LLTransportSendCompletion&) noexcept { return false; }
    virtual bool releaseReliableCompletion() noexcept { return false; }
    virtual bool popUnreliableCompletion(LLTransportUnreliableCompletion&) noexcept { return false; }
    virtual void failCompletion() noexcept {}
    // Value snapshots only; never access the worker-owned packet ring/socket.
    virtual bool backlog(LLTransportBacklog&) const noexcept { return false; }
    // Main consumer only. Empty says nothing about UDP still in the kernel or
    // a publication racing this observation. False return means unavailable.
    virtual bool ingressPending(bool&) const noexcept { return false; }
    virtual bool ringStats(LLTransportRingStats&) const noexcept { return false; }
    // Main mirrors this anchored legacy epoch into the old main-owned cache.
    // Circuit state and snapshot ages must use the separate steady epoch.
    virtual bool legacyMessageTimeUsec(uint64_t&) const noexcept { return false; }
    // Main acquires a stable FIFO event/view until releaseInbound after the
    // reader is cleared. False means none/unavailable, never discard.
    virtual bool peekInbound(LLTransportIngressEvent&, LLTransportDecodedView&) noexcept { return false; }
    // True only after owner join and an authoritative check that no inbound
    // event remains. False means either nonempty or unavailable; the default
    // cannot authorize a shutdown barrier or event release.
    virtual bool inboundEmptyAfterJoinOnMain() const noexcept { return false; }
    virtual bool releaseInbound() noexcept { return false; }
    virtual bool schemaMatches(uint32_t, uint64_t) const noexcept { return false; }
    // Retain already-ACKed data on fatal; do not silently release/replay it.
    virtual void failInbound() noexcept {}
};
#endif
