// Test-only POSIX UDP adapter: the actual owner remains the sole reader of
// its socket. No SOCKS, packet simulator, or production ring behavior.
#ifndef TEST_TRANSPORT_OWNER_UDP_RING_H
#define TEST_TRANSPORT_OWNER_UDP_RING_H
#include <arpa/inet.h>
#include <sys/socket.h>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <utility>
using S32=int32_t;
using U64=uint64_t;
constexpr int NET_BUFFER_SIZE=8192, MTUBYTES=1200;
enum class NetSendStatus { Sent, WouldBlock, Refused, Failed };
struct NetSendResult { NetSendStatus status; int osError; };
class LLRandLagFib2281 {};
class LLHost
{
public:
    LLHost()=default;
    LLHost(uint32_t address,uint16_t port):mAddress(address),mPort(port) {}
    uint32_t getAddress() const { return mAddress; }
    uint32_t getPort() const { return mPort; }
private:
    uint32_t mAddress=0; uint16_t mPort=0;
};
class LLPacketRing
{
public:
    struct UDPProxySnapshot {};
    enum class OwnerReceiveStatus { Delivered, ConsumedDiscarded, WouldBlock, Interrupted, Error };
    struct OwnerReceiveResult { OwnerReceiveStatus status; S32 bytes; S32 nativeError; };
    OwnerReceiveResult receivePacketOnce(int socket,char* out,const UDPProxySnapshot&) noexcept
    {
        sockaddr_in peer{}; socklen_t len=sizeof(peer);
        const auto n=recvfrom(socket,out,NET_BUFFER_SIZE,0,reinterpret_cast<sockaddr*>(&peer),&len);
        if (n<0) { const int e=errno; return {e==EAGAIN || e==EWOULDBLOCK ? OwnerReceiveStatus::WouldBlock :
            e==EINTR ? OwnerReceiveStatus::Interrupted : OwnerReceiveStatus::Error,0,e}; }
        mSender=LLHost(peer.sin_addr.s_addr,ntohs(peer.sin_port));
        mInput+=static_cast<uint64_t>(n); ++mInPackets;
        if (n>=7 && static_cast<unsigned char>(out[6])==42) ++mAppPackets;
        return {OwnerReceiveStatus::Delivered,static_cast<S32>(n),0};
    }
    LLHost getLastSender() const noexcept { return mSender; }
    LLHost getLastReceivingInterface() const noexcept { return {}; }
    NetSendResult sendPacketOnce(int socket,const char* bytes,S32 size,const LLHost& host,
                                 const UDPProxySnapshot&,bool) noexcept
    {
        sockaddr_in peer{}; peer.sin_family=AF_INET; peer.sin_addr.s_addr=host.getAddress(); peer.sin_port=htons(host.getPort());
        const auto n=sendto(socket,bytes,static_cast<std::size_t>(size),0,reinterpret_cast<sockaddr*>(&peer),sizeof(peer));
        if (n<0) { const int e=errno; return {e==EAGAIN || e==EWOULDBLOCK ? NetSendStatus::WouldBlock :
            e==ECONNREFUSED ? NetSendStatus::Refused : NetSendStatus::Failed,e}; }
        if (n!=size) return {NetSendStatus::Failed,0};
        mOutput+=static_cast<uint64_t>(n); ++mOutPackets; return {NetSendStatus::Sent,0};
    }
    bool takeActualBytesOnOwner(U64& in,U64& out) noexcept
    { in=mInput-mHarvestedIn; out=mOutput-mHarvestedOut; mHarvestedIn=mInput; mHarvestedOut=mOutput; return true; }
    U64 getSessionBytesIn() const noexcept { return mInput; }
    U64 getSessionBytesOut() const noexcept { return mOutput; }
    U64 getSessionPacketsIn() const noexcept { return mInPackets; }
    U64 getSessionPacketsOut() const noexcept { return mOutPackets; }
    S32 getNumBufferedPackets() const noexcept { return 0; }
    S32 getNumBufferedBytes() const noexcept { return 0; }
    bool ownerRetainedBytesOnMain(uint64_t& out) const noexcept { out=sizeof(*this); return true; }
    static std::unique_ptr<LLRandLagFib2281> makeOwnerDropRandom(uint32_t)
    { return std::make_unique<LLRandLagFib2281>(); }
    std::unique_ptr<LLRandLagFib2281> exchangeOwnerDropRandomOnMain(std::unique_ptr<LLRandLagFib2281> next) noexcept
    { auto old=std::move(mRandom); mRandom=std::move(next); return old; }
    bool restoreInitialActualBytesOnMain(U64 in,U64 out) noexcept
    { mHarvestedIn-=in; mHarvestedOut-=out; return true; }
    // Read on main only after worker join.
    U64 appPacketsAfterJoin() const noexcept { return mAppPackets; }
private:
    LLHost mSender; U64 mInput=0,mOutput=0,mHarvestedIn=0,mHarvestedOut=0,mInPackets=0,mOutPackets=0,mAppPackets=0;
    std::unique_ptr<LLRandLagFib2281> mRandom;
};
#endif
