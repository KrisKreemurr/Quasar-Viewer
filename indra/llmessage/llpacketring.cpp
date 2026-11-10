/**
 * @file llpacketring.cpp
 * @brief implementation of LLPacketRing class.
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

#include "linden_common.h"

#include "llpacketring.h"

#if LL_WINDOWS
    #include <winsock2.h>
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
#endif

// linden library includes
#include "llerror.h"
#include "lltimer.h"
#include "llproxy.h"
#include "llrand.h"
#include "message.h"
#include "u64.h"
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

constexpr S16 MAX_BUFFER_RING_SIZE = 1024;
constexpr S16 DEFAULT_BUFFER_RING_SIZE = 256;

LLPacketRing::LLPacketRing ()
    : mPacketRing(DEFAULT_BUFFER_RING_SIZE, nullptr)
{
    LLHost invalid_host;
    try
    {
        for (size_t i = 0; i < mPacketRing.size(); ++i)
            mPacketRing[i] = new LLPacketBuffer(invalid_host, nullptr, 0);
    }
    catch (...)
    {
        for (LLPacketBuffer* packet : mPacketRing) delete packet;
        throw;
    }
}

LLPacketRing::~LLPacketRing ()
{
    for (auto packet : mPacketRing)
    {
        delete packet;
    }
    mPacketRing.clear();
    mNumBufferedPackets = 0;
    mNumBufferedBytes = 0;
    mHeadIndex = 0;
}

S32 LLPacketRing::receivePacket (S32 socket, char *datap)
{
    return receivePacketInternal(socket, datap, captureUDPProxyOnMain(), false);
}

LLPacketRing::UDPProxySnapshot LLPacketRing::captureUDPProxyOnMain()
{
    UDPProxySnapshot snapshot;
    snapshot.enabled = LLProxy::isSOCKSProxyEnabled();
    if (snapshot.enabled) snapshot.relay = LLProxy::getInstance()->getUDPProxy();
    return snapshot;
}

S32 LLPacketRing::receivePacket (S32 socket, char *datap, const UDPProxySnapshot& proxy)
{
    return receivePacketInternal(socket, datap, proxy, true);
}

S32 LLPacketRing::receivePacketInternal(S32 socket, char* datap, const UDPProxySnapshot& proxy, bool valueMetadata)
{
    bool drop = computeDrop();
    return (mNumBufferedPackets > 0) ?
        receiveOrDropBufferedPacket(datap, drop) :
        receiveOrDropPacket(socket, datap, drop, proxy, valueMetadata);
}

LLPacketRing::OwnerReceiveResult LLPacketRing::receivePacketOnce(S32 socket, char* datap,
                                                                 const UDPProxySnapshot& proxy)
{
    if (!datap) return {};
    const bool drop = computeDrop();
    if (mNumBufferedPackets > 0)
    {
        const S32 size = receiveOrDropBufferedPacket(datap, drop);
        return {size > 0 ? OwnerReceiveStatus::Delivered : OwnerReceiveStatus::ConsumedDiscarded, size, 0};
    }
    char proxyBuffer[NET_BUFFER_SIZE + SOCKS_HEADER_SIZE];
    const auto result = receive_packet_once(socket, proxy.enabled ? proxyBuffer : datap);
    if (result.status == NetReceiveStatus::WouldBlock) return {OwnerReceiveStatus::WouldBlock, 0, result.nativeError};
    if (result.status == NetReceiveStatus::Interrupted) return {OwnerReceiveStatus::Interrupted, 0, result.nativeError};
    if (result.status != NetReceiveStatus::Datagram || result.bytes < 0 || result.bytes > NET_BUFFER_SIZE)
        return {OwnerReceiveStatus::Error, 0, result.nativeError};
    if (result.bytes > 0)
    {
        mActualBytesIn += result.bytes;
        mSessionBytesIn += result.bytes;
        ++mSessionPacketsIn;
    }
    if (drop || !result.bytes || (proxy.enabled && result.bytes <= SOCKS_HEADER_SIZE))
        return {OwnerReceiveStatus::ConsumedDiscarded, 0, 0};
    S32 size = result.bytes;
    if (proxy.enabled)
    {
        size -= SOCKS_HEADER_SIZE;
        std::memcpy(datap, proxyBuffer + SOCKS_HEADER_SIZE, size);
        // Match the existing SOCKS IPv4 header interpretation.
        proxywrap_t header{};
        std::memcpy(&header, proxyBuffer, SOCKS_HEADER_SIZE);
        mLastSender.setAddress(header.addr);
        mLastSender.setPort(ntohs(header.port));
    }
    else
    {
        mLastSender = LLHost(result.info.senderAddress, result.info.senderPort);
    }
    mLastReceivingIF = LLHost(result.info.receivingAddress, INVALID_PORT);
    return {OwnerReceiveStatus::Delivered, size, 0};
}

static bool send_packet_helper(int socket, const char * datap, S32 data_size, LLHost host,
                               const LLPacketRing::UDPProxySnapshot& proxy)
{
    if (!proxy.enabled)
    {
        return send_packet(socket, datap, data_size, host.getAddress(), host.getPort());
    }

    char headered_send_buffer[NET_BUFFER_SIZE + SOCKS_HEADER_SIZE];

    proxywrap_t *socks_header = static_cast<proxywrap_t*>(static_cast<void*>(&headered_send_buffer));
    socks_header->rsv   = 0;
    socks_header->addr  = host.getAddress();
    socks_header->port  = htons(host.getPort());
    socks_header->atype = ADDRESS_IPV4;
    socks_header->frag  = 0;

    memcpy(headered_send_buffer + SOCKS_HEADER_SIZE, datap, data_size);

    return send_packet( socket,
                        headered_send_buffer,
                        data_size + SOCKS_HEADER_SIZE,
                        proxy.relay.getAddress(),
                        proxy.relay.getPort());
}

bool LLPacketRing::sendPacket(int socket, const char * datap, S32 data_size, LLHost host)
{
    return sendPacket(socket, datap, data_size, host, captureUDPProxyOnMain());
}

bool LLPacketRing::sendPacket(int socket, const char * datap, S32 data_size, LLHost host,
                              const UDPProxySnapshot& proxy)
{
    mActualBytesOut += data_size;
    mSessionBytesOut += data_size;
    ++mSessionPacketsOut;
    return send_packet_helper(socket, datap, data_size, host, proxy);
}

NetSendResult LLPacketRing::sendPacketOnce(int socket, const char* datap, S32 data_size,
                                            const LLHost& host, const UDPProxySnapshot& proxy,
                                            bool firstAttempt)
{
    if (!datap || data_size <= 0 || data_size > NET_BUFFER_SIZE) return {NetSendStatus::Failed, 0};
    if (firstAttempt)
    {
        mActualBytesOut += data_size;
        mSessionBytesOut += data_size;
        ++mSessionPacketsOut;
    }
    if (!proxy.enabled)
        return send_packet_once(socket, datap, data_size, host.getAddress(), host.getPort());

    char buffer[NET_BUFFER_SIZE + SOCKS_HEADER_SIZE];
    proxywrap_t header{};
    header.addr = host.getAddress();
    header.port = htons(host.getPort());
    header.atype = ADDRESS_IPV4;
    std::memcpy(buffer, &header, SOCKS_HEADER_SIZE);
    std::memcpy(buffer + SOCKS_HEADER_SIZE, datap, data_size);
    return send_packet_once(socket, buffer, data_size + SOCKS_HEADER_SIZE,
                            proxy.relay.getAddress(), proxy.relay.getPort());
}

void LLPacketRing::dropPackets (U32 num_to_drop)
{
    mPacketsToDrop += num_to_drop;
}

void LLPacketRing::setDropPercentage (F32 percent_to_drop)
{
    mDropPercentage = percent_to_drop;
}

bool LLPacketRing::computeDrop()
{
    F32 sample = 0.f;
    if (mDropPercentage > 0.f)
    {
        if (mOwnerDropRandom) sample = ownerDropSample();
        else sample = ll_frand(100.f); // legacy main-thread path
    }
    bool drop = mDropPercentage > 0.f && sample < mDropPercentage;
    if (drop)
    {
        ++mPacketsToDrop;
    }
    if (mPacketsToDrop > 0)
    {
        --mPacketsToDrop;
        drop = true;
    }
    return drop;
}

F32 LLPacketRing::ownerDropSample() noexcept
{
    F32 unit = static_cast<F32>((*mOwnerDropRandom)());
    if (!(unit >= 0.f && unit < 1.f)) unit = std::fmod(unit, 1.f);
    const F32 sample = unit * 100.f;
    return sample >= 100.f ? 0.f : sample;
}

bool LLPacketRing::prepareOwnerDropRandom(U32 seed) noexcept
{
    try
    {
        std::unique_ptr<LLRandLagFib2281> generator(new LLRandLagFib2281(seed));
        mOwnerDropRandom = std::move(generator);
        return true;
    }
    catch (...) { return false; }
}

std::unique_ptr<LLRandLagFib2281> LLPacketRing::makeOwnerDropRandom(U32 seed) noexcept
{
    try { return std::unique_ptr<LLRandLagFib2281>(new LLRandLagFib2281(seed)); }
    catch (...) { return {}; }
}

std::unique_ptr<LLRandLagFib2281> LLPacketRing::exchangeOwnerDropRandomOnMain(
    std::unique_ptr<LLRandLagFib2281> next) noexcept
{
    return std::exchange(mOwnerDropRandom,std::move(next));
}

bool LLPacketRing::ownerRetainedBytesOnMain(uint64_t& out) const noexcept
{
    const auto maximum=(std::numeric_limits<uint64_t>::max)();
    uint64_t total=sizeof(*this);
    auto addProduct=[&](uint64_t count,uint64_t size) noexcept {
        if (size && count>(maximum-total)/size) return false;
        total+=count*size; return true;
    };
    if (!addProduct(mPacketRing.capacity(),sizeof(LLPacketBuffer*)) ||
        !addProduct(mPacketRing.size(),sizeof(LLPacketBuffer)) ||
        (mOwnerDropRandom && !addProduct(1,sizeof(LLRandLagFib2281)))) return false;
    out=total;
    return true;
}

bool LLPacketRing::takeActualBytesOnOwner(U64& inBytes, U64& outBytes) noexcept
{
    if (mActualBytesIn<0 || mActualBytesOut<0) return false;
    const U64 in=static_cast<U64>(mActualBytesIn), out=static_cast<U64>(mActualBytesOut);
    mActualBytesIn=0; mActualBytesOut=0;
    inBytes=in; outBytes=out;
    return true;
}

bool LLPacketRing::restoreInitialActualBytesOnMain(U64 inBytes, U64 outBytes) noexcept
{
    const auto maximum=static_cast<U64>((std::numeric_limits<S32>::max)());
    if (mActualBytesIn || mActualBytesOut || inBytes>maximum || outBytes>maximum) return false;
    mActualBytesIn=static_cast<S32>(inBytes); mActualBytesOut=static_cast<S32>(outBytes);
    return true;
}

S32 LLPacketRing::receiveOrDropPacket(S32 socket, char *datap, bool drop,
                                      const UDPProxySnapshot& proxy, bool valueMetadata)
{
    S32 packet_size = 0;
    NetReceiveInfo info;

    // pull straight from socket
    if (proxy.enabled)
    {
        char buffer[NET_BUFFER_SIZE + SOCKS_HEADER_SIZE];   /* Flawfinder ignore */
        packet_size = valueMetadata ? receive_packet(socket, buffer, info) : receive_packet(socket, buffer);
        if (packet_size > 0)
        {
            mActualBytesIn += packet_size;
            mSessionBytesIn += packet_size;
            ++mSessionPacketsIn;
        }

        if (packet_size > SOCKS_HEADER_SIZE)
        {
            if (drop)
            {
                packet_size = 0;
            }
            else
            {
                // *FIX We are assuming ATYP is 0x01 (IPv4), not 0x03 (hostname) or 0x04 (IPv6)
                packet_size -= SOCKS_HEADER_SIZE; // The unwrapped packet size
                memcpy(datap, buffer + SOCKS_HEADER_SIZE, packet_size);
                proxywrap_t * header = static_cast<proxywrap_t*>(static_cast<void*>(buffer));
                mLastSender.setAddress(header->addr);
                mLastSender.setPort(ntohs(header->port));
                mLastReceivingIF = valueMetadata ? LLHost(info.receivingAddress, INVALID_PORT) : ::get_receiving_interface();
            }
        }
        else
        {
            packet_size = 0;
        }
    }
    else
    {
        packet_size = valueMetadata ? receive_packet(socket, datap, info) : receive_packet(socket, datap);
        if (packet_size > 0)
        {
            mActualBytesIn += packet_size;
            mSessionBytesIn += packet_size;
            ++mSessionPacketsIn;
            if (drop)
            {
                packet_size = 0;
            }
            else
            {
                mLastSender = valueMetadata ? LLHost(info.senderAddress, info.senderPort) : ::get_sender();
                mLastReceivingIF = valueMetadata ? LLHost(info.receivingAddress, INVALID_PORT) : ::get_receiving_interface();
            }
        }
    }
    return packet_size;
}

S32 LLPacketRing::receiveOrDropBufferedPacket(char *datap, bool drop)
{
    assert(mNumBufferedPackets > 0);
    S32 packet_size = 0;

    S16 ring_size = (S16)(mPacketRing.size());
    S16 packet_index = (mHeadIndex + ring_size - mNumBufferedPackets) % ring_size;
    LLPacketBuffer* packet = mPacketRing[packet_index];
    packet_size = packet->getSize();
    mLastSender = packet->getHost();
    mLastReceivingIF = packet->getReceivingInterface();

    --mNumBufferedPackets;
    mNumBufferedBytes -= packet_size;
    if (mNumBufferedPackets == 0)
    {
        assert(mNumBufferedBytes == 0);
    }

    if (!drop)
    {
        if (packet_size > 0)
        {
            memcpy(datap, packet->getData(), packet_size);
        }
        else
        {
            assert(false);
        }
    }
    else
    {
        packet_size = 0;
    }
    return packet_size;
}

S32 LLPacketRing::bufferInboundPacket(S32 socket, const UDPProxySnapshot& proxy, bool valueMetadata)
{
    if (mNumBufferedPackets == mPacketRing.size() && mNumBufferedPackets < MAX_BUFFER_RING_SIZE)
    {
        expandRing();
    }

    LLPacketBuffer* packet = mPacketRing[mHeadIndex];
    S32 old_packet_size = packet->getSize();
    S32 packet_size = 0;
    NetReceiveInfo info;
    if (proxy.enabled)
    {
        char buffer[NET_BUFFER_SIZE + SOCKS_HEADER_SIZE];   /* Flawfinder ignore */
        packet_size = valueMetadata ? receive_packet(socket, buffer, info) : receive_packet(socket, buffer);
        if (packet_size > 0)
        {
            mActualBytesIn += packet_size;
            mSessionBytesIn += packet_size;
            ++mSessionPacketsIn;
            if (packet_size > SOCKS_HEADER_SIZE)
            {
                // *FIX We are assuming ATYP is 0x01 (IPv4), not 0x03 (hostname) or 0x04 (IPv6)

                proxywrap_t * header = static_cast<proxywrap_t*>(static_cast<void*>(buffer));
                LLHost sender;
                sender.setAddress(header->addr);
                sender.setPort(ntohs(header->port));

                packet_size -= SOCKS_HEADER_SIZE; // The unwrapped packet size
                if (valueMetadata)
                    packet->init(buffer + SOCKS_HEADER_SIZE, packet_size, sender, LLHost(info.receivingAddress, INVALID_PORT));
                else packet->init(buffer + SOCKS_HEADER_SIZE, packet_size, sender);

                mHeadIndex = (mHeadIndex + 1) % (S16)(mPacketRing.size());
                if (mNumBufferedPackets < MAX_BUFFER_RING_SIZE)
                {
                    ++mNumBufferedPackets;
                    mNumBufferedBytes += packet_size;
                }
                else
                {
                    // we overwrote an older packet
                    mNumBufferedBytes += packet_size - old_packet_size;
                }
            }
            else
            {
                // Runt SOCKS wrapper (no payload past the header): discard
                // it, but keep the raw size so drainSocket() keeps draining
                // instead of misreading one bad datagram as an empty socket.
                // The drain accounting counts it as received-but-dropped.
            }
        }
    }
    else
    {
        if (valueMetadata)
        {
            char buffer[NET_BUFFER_SIZE];
            packet_size = receive_packet(socket, buffer, info);
            if (packet_size > 0)
                packet->init(buffer, packet_size, LLHost(info.senderAddress, info.senderPort),
                             LLHost(info.receivingAddress, INVALID_PORT));
        }
        else
        {
            packet->init(socket);
            packet_size = packet->getSize();
        }
        if (packet_size > 0)
        {
            mActualBytesIn += packet_size;
            mSessionBytesIn += packet_size;
            ++mSessionPacketsIn;

            mHeadIndex = (mHeadIndex + 1) % (S16)(mPacketRing.size());
            if (mNumBufferedPackets < MAX_BUFFER_RING_SIZE)
            {
                ++mNumBufferedPackets;
                mNumBufferedBytes += packet_size;
            }
            else
            {
                // we overwrote an older packet
                mNumBufferedBytes += packet_size - old_packet_size;
            }
        }
    }
    return packet_size;
}

S32 LLPacketRing::drainSocket(S32 socket)
{
    return drainSocketInternal(socket, captureUDPProxyOnMain(), false);
}

S32 LLPacketRing::drainSocket(S32 socket, const UDPProxySnapshot& proxy)
{
    return drainSocketInternal(socket, proxy, true);
}

S32 LLPacketRing::drainSocketInternal(S32 socket, const UDPProxySnapshot& proxy, bool valueMetadata)
{
    // drain into buffer
    S32 packet_size = 1;
    S32 num_loops = 0;
    S32 old_num_packets = mNumBufferedPackets;
    while (packet_size > 0)
    {
        packet_size = bufferInboundPacket(socket, proxy, valueMetadata);
        ++num_loops;
    }
    S32 num_dropped_packets = (num_loops - 1 + old_num_packets) - mNumBufferedPackets;
    if (num_dropped_packets > 0)
    {
        // It will eventually be accounted by mDroppedPackets
        // and mPacketsLost, but track it here for logging purposes.
        mNumDroppedPackets += num_dropped_packets;
    }
    return (S32)(mNumBufferedPackets);
}

bool LLPacketRing::expandRing()
{
    // compute larger size
    constexpr S16 BUFFER_RING_EXPANSION = 256;
    S16 old_size = (S16)(mPacketRing.size());
    S16 new_size = llmin(old_size + BUFFER_RING_EXPANSION, MAX_BUFFER_RING_SIZE);
    if (new_size == old_size)
    {
        // mPacketRing is already maxed out
        return false;
    }

    // make a larger ring and copy packet pointers
    std::vector<LLPacketBuffer*> new_ring(new_size, nullptr);
    for (S16 i = 0; i < old_size; ++i)
    {
        S16 j = (mHeadIndex + i) % old_size;
        new_ring[i] = mPacketRing[j];
    }

    // allocate new packets for the remainder of new_ring
    LLHost invalid_host;
    for (S16 i = old_size; i < new_size; ++i)
    {
        new_ring[i] = new LLPacketBuffer(invalid_host, nullptr, 0);
    }

    // swap the rings and reset mHeadIndex
    mPacketRing.swap(new_ring);
    mHeadIndex = mNumBufferedPackets;
    return true;
}

F32 LLPacketRing::getBufferLoadRate() const
{
    // goes up to MAX_BUFFER_RING_SIZE
    return (F32)mNumBufferedPackets / (F32)DEFAULT_BUFFER_RING_SIZE;
}

void LLPacketRing::dumpPacketRingStats()
{
    mNumDroppedPacketsTotal += mNumDroppedPackets;
    LL_INFOS("Messaging") << "Packet ring stats: " << std::endl
                          << "Buffered packets: " << mNumBufferedPackets << std::endl
                          << "Buffered bytes: " << mNumBufferedBytes << std::endl
                          << "Dropped packets current: " << mNumDroppedPackets << std::endl
                          << "Dropped packets total: " << mNumDroppedPacketsTotal << std::endl
                          << "Dropped packets percentage: " << mDropPercentage << "%" << std::endl
                          << "Actual in bytes: " << mActualBytesIn << std::endl
                          << "Actual out bytes: " << mActualBytesOut << LL_ENDL;
    mNumDroppedPackets = 0;
}
