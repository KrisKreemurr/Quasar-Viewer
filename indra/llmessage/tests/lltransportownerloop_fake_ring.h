// Test-only socket boundary. No real UDP, SOCKS, or packet-ring behavior.
#ifndef TEST_TRANSPORT_PUMP_FAKE_RING_H
#define TEST_TRANSPORT_PUMP_FAKE_RING_H
#include <cstdint>
#include <cstring>
#include <vector>
using S32 = int32_t;
constexpr int NET_BUFFER_SIZE = 8192;
constexpr int MTUBYTES = 1200;
enum class NetSendStatus { Sent, WouldBlock, Refused, Failed };
struct NetSendResult { NetSendStatus status; int osError; };
class LLHost
{
public:
    LLHost()=default;
    LLHost(uint32_t address,uint16_t port) : mAddress(address),mPort(port) {}
    uint32_t getAddress() const { return mAddress; }
    uint32_t getPort() const { return mPort; }
private:
    uint32_t mAddress=0x0100007f; uint16_t mPort=1234;
};
class LLPacketRing
{
public:
    S32 getNumBufferedPackets() const noexcept { return 0; }
    S32 getNumBufferedBytes() const noexcept { return 0; }
    struct UDPProxySnapshot {};
    enum class OwnerReceiveStatus { Delivered, ConsumedDiscarded, WouldBlock, Interrupted, Error };
    struct OwnerReceiveResult { OwnerReceiveStatus status=OwnerReceiveStatus::Error; S32 bytes=0; S32 nativeError=0; };
    std::vector<uint8_t> packet;
    LLHost sender{};
    std::vector<std::vector<uint8_t>> sentPackets;
    std::vector<bool> firstAttempts;
    NetSendResult sendResult{NetSendStatus::Sent,0};
    bool throwBeforeAttempt=false;
    bool injectReceive=false;
    OwnerReceiveResult injectedReceive{};
    unsigned receiveAttempts=0;
    S32 receivePacket(int, char* out, const UDPProxySnapshot&)
    {
        if (packet.empty()) return 0;
        const auto n = static_cast<S32>(packet.size());
        std::memcpy(out, packet.data(), packet.size());
        packet.clear();
        return n;
    }
    OwnerReceiveResult receivePacketOnce(int socket,char* out,const UDPProxySnapshot& proxy)
    {
        ++receiveAttempts;
        if (injectReceive) { injectReceive=false; return injectedReceive; }
        const auto n=receivePacket(socket,out,proxy);
        return {n>0 ? OwnerReceiveStatus::Delivered : OwnerReceiveStatus::WouldBlock,n,0};
    }
    LLHost getLastSender() const { return sender; }
    LLHost getLastReceivingInterface() const { return {}; }
    NetSendResult sendPacketOnce(int, const char* bytes, S32 size, const LLHost&, const UDPProxySnapshot&, bool first)
    {
        if (throwBeforeAttempt) throw 1;
        sentPackets.emplace_back(bytes,bytes+size); firstAttempts.push_back(first);
        return sendResult;
    }
};
#endif
