// Source fixture for the actual owner action encoder. No socket or actor loop.
#include "lltransportownerloop.cpp"
#include "../lltransportenvelope.h"
#include <cstdio>
#include <cstdlib>
using U8=uint8_t; using U32=uint32_t;
constexpr int MAX_BUFFER_SIZE=8192, LL_PACKET_ID_SIZE=6, LL_ZERO_CODE_FLAG=0x80;
// Actual legacy zero_code body, extracted without modification at preparation.
#include "legacy_zero_code.inc"

static void check(bool value,int line)
{ if (!value) { std::fprintf(stderr,"failed line %d\n",line); std::abort(); } }
#define CHECK(x) check(bool(x),__LINE__)
int main()
{
    using State=LLTransportCircuitState;
    State::Action action{}; EncodedAction wire;
    action.kind=State::ActionKind::Ack; action.ackCount=2;
    action.ackIds[0]=0x01020304; action.ackIds[1]=0xa1b2c3d4;
    CHECK(encodeStateAction(action,0x00112233,wire));
    const uint8_t expectedAck[]={0,0,0x11,0x22,0x33,0,0xff,0xff,0xff,0xfb,2,
                                 4,3,2,1,0xd4,0xc3,0xb2,0xa1};
    CHECK(wire.size==sizeof(expectedAck));
    CHECK(std::memcmp(wire.bytes.data(),expectedAck,sizeof(expectedAck))==0);
    std::array<uint8_t,State::MaxPacketBytes> decoded{};
    auto envelope=llTransportParseEnvelope(wire.bytes.data(),wire.size,decoded.data(),decoded.size());
    CHECK(envelope.status==LLTransportEnvelope::Status::Complete && envelope.messageNumber==0xfffffffb && envelope.packetId==0x00112233);
    action.kind=State::ActionKind::Ping; action.pingId=0xa5; action.oldestUnacked=0x10203040;
    CHECK(encodeStateAction(action,0x00123456,wire));
    const uint8_t expectedPing[]={0,0,0x12,0x34,0x56,0,1,0xa5,0x40,0x30,0x20,0x10};
    CHECK(wire.size==sizeof(expectedPing));
    CHECK(std::memcmp(wire.bytes.data(),expectedPing,sizeof(expectedPing))==0);
    action.kind=State::ActionKind::InitialSend; action.size=7;
    action.bytes[0]=0x40; action.bytes[6]=42;
    CHECK(encodeStateAction(action,0,wire) && wire.size==7);
    CHECK(std::memcmp(wire.bytes.data(),action.bytes.data(),7)==0);
    action.kind=State::ActionKind::Ack; action.ackCount=State::MaxAckBatch;
    CHECK(encodeStateAction(action,1,wire) && wire.size==11+4*State::MaxAckBatch);
    action.ackCount=State::MaxAckBatch+1;
    CHECK(!encodeStateAction(action,1,wire));
    LLTransportOwnerCommand command; command.kind=LLTransportOwnerCommand::Kind::Send;
    command.byteCount=263; command.bytes[5]=0; command.bytes[6]=42; command.bytes[262]=0x5a;
    command.encoding=LLTransportEncoding::ZeroCoded; command.reliable=true;
    CHECK(encodeOwnerCommand(command,0x00123456,wire));
    CHECK((wire.bytes[0]&0xc0)==0xc0 && wire.size==10);
    CHECK(wire.bytes[6]==42 && wire.bytes[7]==0 && wire.bytes[8]==255 && wire.bytes[9]==0x5a);
    envelope=llTransportParseEnvelope(wire.bytes.data(),wire.size,decoded.data(),decoded.size());
    CHECK(envelope.status==LLTransportEnvelope::Status::Complete && envelope.packetId==0x00123456);
    CHECK(envelope.expandedSize==command.byteCount && decoded[262]==0x5a);
    command.encoding=LLTransportEncoding::Unencoded; command.reliable=false;
    CHECK(encodeOwnerCommand(command,0x00123456,wire));
    CHECK(wire.size==command.byteCount && wire.bytes[0]==0 && wire.bytes[5]==command.bytes[5]);
    // Compare the production legacy compressor, not a second model, including
    // runs crossing the 255-byte boundary and cases that do not save bytes.
    for (uint32_t length : {7u,8u,260u,263u,515u,600u})
    {
        for (unsigned pattern=0;pattern<3;++pattern)
        {
            command.byteCount=length; command.encoding=LLTransportEncoding::ZeroCoded;
            command.reliable=true; command.bytes.fill(0); command.bytes[6]=42;
            for (uint32_t i=7;i<length;++i)
                command.bytes[i]=pattern==0 ? 0 : pattern==1 ? uint8_t((i%3) ? i : 0) : uint8_t(i%257==0 ? 1 : 0);
            CHECK(encodeOwnerCommand(command,0x00123456,wire));
            std::array<U8,State::MaxPacketBytes> legacy=command.bytes;
            std::memset(legacy.data(),0,5); putNetworkU32(legacy.data()+1,0x00123456);
            U8* pointer=legacy.data(); U32 size=length; (void)zero_code(&pointer,&size); pointer[0]|=0x40;
            CHECK(wire.size==size && std::memcmp(wire.bytes.data(),pointer,size)==0);
        }
    }
}
