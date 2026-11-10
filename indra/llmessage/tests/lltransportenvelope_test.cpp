/** Owner wire-envelope checks, no network. SPDX-License-Identifier: LGPL-2.1-only */
#include "../lltransportenvelope.h"
#include <array>
#include <cstdio>
#include <cstdlib>
static void check(bool value, int line) { if (!value) { std::fprintf(stderr, "failed %d\n",line); std::abort(); } }
#define CHECK(x) check(bool(x), __LINE__)
int main()
{
    std::array<uint8_t, 1024> out{};
    const uint8_t packet[] = {0x50,0,0,0,9,0,1,42,0,0,0,3,0,0,0,4,2};
    auto value = llTransportParseEnvelope(packet,sizeof(packet),out.data(),out.size());
    CHECK(value.status == LLTransportEnvelope::Status::Complete && value.packetId == 9);
    CHECK(value.reliable && !value.resent && value.ackCount == 2 && value.messageNumber == 1 && value.bodyOffset == 7);
    uint32_t ack = 0;
    CHECK(llTransportAppendedAck(packet,sizeof(packet),value,0,ack) && ack == 4);
    CHECK(llTransportAppendedAck(packet,sizeof(packet),value,1,ack) && ack == 3);
    CHECK(!llTransportAppendedAck(packet,sizeof(packet),value,2,ack));
    const uint8_t badAck[] = {0x10,0,0,0,1,0,1,2};
    CHECK(llTransportParseEnvelope(badAck,sizeof(badAck),out.data(),out.size()).status == LLTransportEnvelope::Status::MalformedAck);
    const uint8_t compressed[] = {0x80,0,0,0,7,0,1,0,0,2,9};
    value = llTransportParseEnvelope(compressed,sizeof(compressed),out.data(),out.size());
    CHECK(value.status == LLTransportEnvelope::Status::Complete && value.expandedSize == 266);
    CHECK(out[6] == 1 && out[7] == 0 && out[264] == 0 && out[265] == 9);
    CHECK(llTransportParseEnvelope(compressed,sizeof(compressed),out.data(),10).status == LLTransportEnvelope::Status::CapacityExceeded);
    const uint8_t badZero[] = {0x80,0,0,0,1,0,1,0};
    CHECK(llTransportParseEnvelope(badZero,sizeof(badZero),out.data(),out.size()).status == LLTransportEnvelope::Status::MalformedZeroCode);
    const uint8_t low[] = {0,0,0,0,1,0,255,255,255,251};
    value = llTransportParseEnvelope(low,sizeof(low),out.data(),out.size());
    CHECK(value.status == LLTransportEnvelope::Status::Complete && value.messageNumber == 0xfffffffb && value.bodyOffset == 10);
}
