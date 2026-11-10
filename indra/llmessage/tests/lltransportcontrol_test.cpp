/** Source-only control extraction checks. SPDX-License-Identifier: LGPL-2.1-only */
#include "../lltransportcontrol.h"
#include <cstdio>
#include <cstdlib>
static void check(bool v, int line) { if (!v) { std::fprintf(stderr,"failed %d\n",line); std::abort(); } }
#define CHECK(v) check(bool(v), __LINE__)
int main()
{
    LLTransportDecodedPayload<128, 4, 8> data;
    const uint8_t ping=7; const uint32_t oldest=1234;
    CHECK(data.appendBlock(0,0) && data.appendField(0,&ping,1) && data.appendField(1,&oldest,4));
    LLTransportControlValues out;
    CHECK(llTransportDecodeControl(1,data.view(),out) && out.kind==LLTransportControlValues::Kind::StartPing);
    CHECK(out.pingId==7 && out.oldestUnacked==1234);
    data.reset(); const uint32_t first=4, second=9;
    CHECK(data.appendBlock(0,0) && data.appendField(0,&first,4));
    CHECK(data.appendBlock(0,1) && data.appendField(0,&second,4));
    CHECK(llTransportDecodeControl(0xfffffffbu,data.view(),out) && out.ackCount==2);
    CHECK(out.acknowledgements[0]==4 && out.acknowledgements[1]==9);
    const auto old=out; CHECK(!llTransportDecodeControl(2,data.view(),out) && out.ackCount==old.ackCount);
}
