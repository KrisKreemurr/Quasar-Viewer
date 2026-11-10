/**
 * @file llpacketring.h
 * @brief definition of LLPacketRing class for implementing a resend,
 * drop, or delay in packet transmissions
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#pragma once

#include <vector>
#include <memory>
#include <cstdint>

#include "llhost.h"
#include "llpacketbuffer.h"
#include "llthrottle.h"
#include "llrand.h"


class LLPacketRing
{
public:
    LLPacketRing();
    ~LLPacketRing();

    // Captured on main while LLProxy is main-owned. Worker calls receive this
    // value through the ordered transport configuration handoff; they must not
    // inspect LLProxy or retain a reference to its mutable state.
    struct UDPProxySnapshot
    {
        bool enabled = false;
        LLHost relay;
    };
    static UDPProxySnapshot captureUDPProxyOnMain();

    // Call before the ring is handed to its sole transport owner. The caller
    // supplies a seed without invoking LLUUID's shared mutable seed buffer on
    // the worker. Failure preserves the previous generator and is explicit.
    bool prepareOwnerDropRandom(U32 seed) noexcept;
    // Main, only while the owner is stopped. A startup transaction can stage a
    // generator, exchange it before handoff, and restore the previous one if
    // binding/startup fails or after join. No generator is changed on failure.
    static std::unique_ptr<LLRandLagFib2281> makeOwnerDropRandom(U32 seed) noexcept;
    std::unique_ptr<LLRandLagFib2281> exchangeOwnerDropRandomOnMain(std::unique_ptr<LLRandLagFib2281> next) noexcept;
    // Main, before handoff or after join. Requested retained allocation bytes;
    // allocator and OS overhead are accounted separately by the owner ledger.
    bool ownerRetainedBytesOnMain(uint64_t& out) const noexcept;
    // Sole owner only: harvest bytes without the legacy S32 bits multiply.
    // Call before the first owner socket operation and after each bounded
    // turn; negative legacy counters fail without modifying outputs or ring.
    bool takeActualBytesOnOwner(U64& inBytes, U64& outBytes) noexcept;
    // Main after a failed startup has joined the owner, only if no owner I/O
    // followed the initial harvest. Never overwrite subsequently accrued bytes.
    bool restoreInitialActualBytesOnMain(U64 inBytes, U64 outBytes) noexcept;

    // receive one packet: either buffered or from the socket
    S32  receivePacket (S32 socket, char *datap);
    S32  receivePacket (S32 socket, char *datap, const UDPProxySnapshot& proxy);

    // Owner-only, exactly one buffered dequeue or socket receive attempt.
    // Only WouldBlock proves that the socket is drained. A discarded buffered,
    // zero-byte, simulated-drop or short SOCKS datagram was still consumed.
    enum class OwnerReceiveStatus { Delivered, ConsumedDiscarded, WouldBlock, Interrupted, Error };
    struct OwnerReceiveResult
    {
        OwnerReceiveStatus status = OwnerReceiveStatus::Error;
        S32 bytes = 0;
        S32 nativeError = 0;
    };
    OwnerReceiveResult receivePacketOnce(S32 socket, char* datap, const UDPProxySnapshot& proxy);
    // send one packet
    bool sendPacket(int h_socket, const char * send_buffer, S32 buf_size, LLHost host);
    bool sendPacket(int h_socket, const char * send_buffer, S32 buf_size, LLHost host,
                    const UDPProxySnapshot& proxy);
    // Owner-only: one nonblocking socket attempt, no retry or global proxy read.
    // firstAttempt accounts the datagram once across all its socket attempts.
    NetSendResult sendPacketOnce(int h_socket, const char* send_buffer, S32 buf_size,
                                 const LLHost& host, const UDPProxySnapshot& proxy, bool firstAttempt);

    // drains packets from socket and returns final mNumBufferedPackets
    S32 drainSocket(S32 socket);
    S32 drainSocket(S32 socket, const UDPProxySnapshot& proxy);

    void dropPackets(U32);
    void setDropPercentage (F32 percent_to_drop);

    inline LLHost getLastSender() const;
    inline LLHost getLastReceivingInterface() const;

    S32 getActualInBytes() const { return mActualBytesIn; }
    S32 getActualOutBytes() const { return mActualBytesOut; }
    S32 getAndResetActualInBits()   { S32 bits = mActualBytesIn * 8; mActualBytesIn = 0; return bits;}
    S32 getAndResetActualOutBits()  { S32 bits = mActualBytesOut * 8; mActualBytesOut = 0; return bits;}

    // #529 data usage: UDP payload bytes / datagrams since startup (never reset).
    // Exclusive ring owner only (main before handoff, transport after handoff).
    U64 getSessionBytesIn() const { return mSessionBytesIn; }
    U64 getSessionBytesOut() const { return mSessionBytesOut; }
    U64 getSessionPacketsIn() const { return mSessionPacketsIn; }
    U64 getSessionPacketsOut() const { return mSessionPacketsOut; }

    S32 getNumBufferedPackets() const { return (S32)(mNumBufferedPackets); }
    S32 getNumBufferedBytes() const { return mNumBufferedBytes; }
    S32 getNumDroppedPackets() const { return mNumDroppedPacketsTotal + mNumDroppedPackets; }

    F32 getBufferLoadRate() const; // from 0 to 4 (0 - empty, 1 - default size is full)
    void dumpPacketRingStats();
protected:
    // returns 'true' if we should intentionally drop a packet
    bool computeDrop();
    F32 ownerDropSample() noexcept;

    // returns packet_size of received packet, zero or less if no packet found
    S32 receivePacketInternal(S32 socket, char* datap, const UDPProxySnapshot& proxy, bool valueMetadata);
    S32 receiveOrDropPacket(S32 socket, char *datap, bool drop, const UDPProxySnapshot& proxy, bool valueMetadata);
    S32 receiveOrDropBufferedPacket(char *datap, bool drop);

    // returns packet_size of packet buffered
    S32 bufferInboundPacket(S32 socket, const UDPProxySnapshot& proxy, bool valueMetadata);
    S32 drainSocketInternal(S32 socket, const UDPProxySnapshot& proxy, bool valueMetadata);

    // returns 'true' if ring was expanded
    bool expandRing();

protected:
    std::vector<LLPacketBuffer*> mPacketRing;
    S16 mHeadIndex { 0 };
    S16 mNumBufferedPackets { 0 };
    S32 mNumDroppedPackets { 0 };
    S32 mNumDroppedPacketsTotal { 0 };
    S32 mNumBufferedBytes { 0 };

    S32 mActualBytesIn { 0 };
    S32 mActualBytesOut { 0 };
    U64 mSessionBytesIn { 0 };
    U64 mSessionBytesOut { 0 };
    U64 mSessionPacketsIn { 0 };
    U64 mSessionPacketsOut { 0 };
    F32 mDropPercentage { 0.0f };   // % of inbound packets to drop
    U32 mPacketsToDrop { 0 };       // drop next inbound n packets
    std::unique_ptr<LLRandLagFib2281> mOwnerDropRandom;

    // These are the sender and receiving_interface for the last packet delivered by receivePacket()
    LLHost mLastSender;
    LLHost mLastReceivingIF;
};


inline LLHost LLPacketRing::getLastSender() const
{
    return mLastSender;
}

inline LLHost LLPacketRing::getLastReceivingInterface() const
{
    return mLastReceivingIF;
}
