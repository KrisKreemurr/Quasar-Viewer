/** Differential tests of decoder versus the extracted production reader loop.
 * Copyright (c) 2026 Slipstream Viewer Project. SPDX-License-Identifier: LGPL-2.1-only */
#include "../lltransportdecoder.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <new>
#include <vector>

// Minimal reader dependencies. Data copies stand in for legacy borrowed
// storage; no real LLMessageSystem or handler is exercised by this fixture.
using U8 = uint8_t; using U16 = uint16_t; using U32 = uint32_t; using S32 = int32_t;
constexpr int MBT_SINGLE=1, MBT_MULTIPLE=2, MBT_VARIABLE=3;
constexpr int MVT_VARIABLE=1, MVT_FIXED=2, MVT_U8=3, MVT_U16=4, MVT_U32=5;
#define LL_ERRS() std::cerr
#define LL_ENDL std::endl
template<class T> T llmax(T a, T b) { return std::max(a,b); }
static void htolememcpy(void* dst,const void* src,int,size_t n) { std::memcpy(dst,src,n); }
static void check(bool b, const char* what) { if (!b) { std::fprintf(stderr,"FAIL: %s\n",what); std::abort(); } }
static std::atomic<unsigned> schemaAllocations{0}, failSchemaAllocation{0};
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept
{
    const unsigned index=++schemaAllocations;
    if (index==failSchemaAllocation.load()) return nullptr;
    try { return ::operator new[](n); } catch (...) { return nullptr; }
}

struct LLMessageVariable
{
    int type, size;
    int getType() const { return type; }
    int getSize() const { return size; }
    char* getName() const { return const_cast<char*>(name); }
    char name[8]{};
};
struct LLMessageBlock
{
    using message_variable_map_t = std::vector<LLMessageVariable*>;
    int mType=MBT_SINGLE, mNumber=1;
    char names[256]{};
    char* mName=names;
    message_variable_map_t mMemberVariables;
};
struct LLMessageTemplate
{
    using message_block_map_t = std::vector<LLMessageBlock*>;
    message_block_map_t mMemberBlocks;
    U32 mMessageNumber=42;
};
struct OldField { std::vector<U8> bytes; };
struct LLMsgBlkData
{
    LLMsgBlkData(char* name,int) : mName(name) {}
    char* mName;
    std::vector<OldField> fields;
    void addVariableBorrowed(char*,int,const void* data,U32 size)
    {
        fields.emplace_back();
        if(size) { const U8* p=static_cast<const U8*>(data); fields.back().bytes.assign(p,p+size); }
    }
    void addVariable(char*,int) { fields.emplace_back(); }
    void addData(char*,const void* data,U32 size,int)
    {
        if(size) { const U8* p=static_cast<const U8*>(data); fields.back().bytes.assign(p,p+size); }
    }
};
struct LLMsgData
{
    std::vector<std::unique_ptr<LLMsgBlkData>> blocks;
    void addBlock(LLMsgBlkData* block) { blocks.emplace_back(block); }
};
#include "transport_schema_freeze.inc"

struct ReferenceReader
{
    LLMessageTemplate* mCurrentRMessageTemplate;
    LLMsgData* mCurrentRMessageData;
    S32 mReceiveSize;
    void logRanOffEndOfPacket(int, S32, S32) {}
    bool read(const U8* buffer, S32 decode_pos)
    {
        int sender=0;
#include "transport_legacy_loop.inc"
        return true;
    }
};

struct Fixture
{
    LLMessageTemplate source;
    std::vector<std::unique_ptr<LLMessageBlock>> blocks;
    std::vector<std::unique_ptr<LLMessageVariable>> fields;
    void block(int kind, int count, std::initializer_list<std::pair<int,int>> vars)
    {
        blocks.emplace_back(new LLMessageBlock);
        auto& b=*blocks.back(); b.mType=kind; b.mNumber=count;
        source.mMemberBlocks.push_back(&b);
        for(auto v:vars)
        {
            fields.emplace_back(new LLMessageVariable{v.first,v.second,{}});
            b.mMemberVariables.push_back(fields.back().get());
        }
    }
};

using TestPayload=LLTransportDecodedPayload<16384,512,2048>;
using TestQueue=LLTransportSpscQueue<TestPayload,2>;
static void initBudget(LLTransportStartupBudget& budget, uint64_t schemaBytes=1000000,
                       uint64_t totalBytes=1000000)
{
    check((LLTransportStartupBudget::initialize<16384,512,2048,2>(
        budget,1024,schemaBytes,totalBytes)),"queue budget initialization");
}

static void compare(Fixture& f, const std::vector<U8>& wire, size_t logicalSize, uint64_t offset)
{
    LLTransportSchema schema;
    LLTransportStartupBudget budget; initBudget(budget);
    check(freezeTransportSchema(f.source,77,budget,schema)==LLTransportFreezeStatus::Complete,"freeze actual source-order schema");
    check(schema.messageNumber()==42 && schema.generation()==77,"schema identity");
    auto output=std::make_unique<LLTransportDecodedPayload<16384,512,2048>>();
    auto result=decodeTransportFields(schema,wire.data(),logicalSize,offset,*output);
    LLMsgData old;
    ReferenceReader reader{&f.source,&old,static_cast<S32>(logicalSize)};
    check(reader.read(wire.data(),static_cast<S32>(offset)),"reference loop");
    bool empty=old.blocks.empty() && !f.source.mMemberBlocks.empty();
    check(result.status==(empty ? LLTransportDecodeStatus::EmptyMessage : LLTransportDecodeStatus::Complete),"status");
    check(output->blockCount==old.blocks.size(),"block count");
    uint32_t fieldIndex=0, oldIndex=0;
    for(uint32_t ordinal=0;ordinal<f.blocks.size();++ordinal)
    {
        const auto* b=f.blocks[ordinal].get();
        uint32_t repeat=0;
        while(oldIndex<old.blocks.size() && old.blocks[oldIndex]->mName==b->mName+repeat)
        {
            const auto& actual=output->blocks[oldIndex];
            const auto& expected=*old.blocks[oldIndex];
            check(actual.schemaOrdinal==ordinal && actual.repeatOrdinal==repeat && actual.firstField==fieldIndex,"block ordinals");
            check(actual.fieldCount==expected.fields.size(),"block fields");
            for(uint32_t j=0;j<actual.fieldCount;++j,++fieldIndex)
            {
                const auto& af=output->fields[fieldIndex]; const auto& ef=expected.fields[j].bytes;
                check(af.schemaOrdinal==j && af.size==ef.size(),"field ordinal and size");
                check(af.offset<=output->byteCount && af.size<=output->byteCount-af.offset,"field bounds");
                check(std::equal(ef.begin(),ef.end(),output->data.begin()+af.offset),"wire bytes");
            }
            ++oldIndex; ++repeat;
        }
    }
    check(oldIndex==old.blocks.size() && fieldIndex==output->fieldCount,"complete comparison");
}

int main()
{
    const uint16_t endianProbe=1;
    check(*reinterpret_cast<const uint8_t*>(&endianProbe)==1,"fixture reference stub requires little endian");
    Fixture mixed;
    mixed.block(MBT_SINGLE,1,{{MVT_FIXED,2},{MVT_VARIABLE,1},{MVT_VARIABLE,2},{MVT_VARIABLE,4}});
    mixed.block(MBT_MULTIPLE,2,{{MVT_FIXED,1}});
    mixed.block(MBT_VARIABLE,1,{{MVT_FIXED,3},{MVT_VARIABLE,1}});
    // Input buffer stays allocated past logical packet end: the old reader
    // forms a pointer there for a zero-size truncated variable field.
    std::vector<U8> wire(256,0);
    const U8 values[]={0xaa,0xbb,2,0x11,0x12,1,0,0x21,2,0,0,0,0x31,0x32,0x41,0x42,2,0x51,0x52,0x53,1,0x61,0x71,0x72,0x73,0};
    std::copy(std::begin(values),std::end(values),wire.begin());
    compare(mixed,wire,sizeof(values),0);
    LLTransportSchema ownedSchema;
    LLTransportStartupBudget ownedBudget; initBudget(ownedBudget);
    check(freezeTransportSchema(mixed.source,9,ownedBudget,ownedSchema)==LLTransportFreezeStatus::Complete,"owned schema");
    auto owned=std::make_unique<LLTransportDecodedPayload<128,16,32>>();
    check(decodeTransportFields(ownedSchema,wire.data(),sizeof(values),0,*owned).status==LLTransportDecodeStatus::Complete,"owned decode");
    wire[0]=0;
    check(owned->view().data[0]==0xaa,"payload does not borrow packet");
    wire[0]=0xaa;
    std::vector<U8> shifted(256,0);
    std::copy(std::begin(values),std::end(values),shifted.begin()+9);
    compare(mixed,shifted,9+sizeof(values),9);
    for(size_t n=0;n<sizeof(values);++n) compare(mixed,wire,n,0);
    wire[2]=255; compare(mixed,wire,sizeof(values),0); // claimed variable bytes exceed packet
    Fixture repeated; repeated.block(MBT_VARIABLE,1,{{MVT_FIXED,2},{MVT_VARIABLE,1}});
    std::vector<U8> many(1024,0); many[0]=255;
    LLTransportSchema schema; LLTransportStartupBudget repeatBudget; initBudget(repeatBudget);
    check(freezeTransportSchema(repeated.source,1,repeatBudget,schema)==LLTransportFreezeStatus::Complete,"repeat schema");
    LLTransportStartupBudget cumulative; initBudget(cumulative,repeatBudget.schemaBytesUsed());
    LLTransportSchema firstFrozen, secondFrozen;
    check(freezeTransportSchema(repeated.source,10,cumulative,firstFrozen)==LLTransportFreezeStatus::Complete,"first schema within ledger");
    const unsigned beforeCumulative=schemaAllocations.load();
    check(freezeTransportSchema(repeated.source,11,cumulative,secondFrozen)==LLTransportFreezeStatus::CapacityExceeded,"cumulative schema cap");
    check(schemaAllocations.load()==beforeCumulative && !secondFrozen.isFrozen(),"cumulative rejection before allocation");
    LLTransportCapacity cap; check(schema.capacityForWireBytes(many.size(),cap),"capacity");
    check(cap.blocks==255 && cap.fields==510 && cap.bytes>=1534,"255 worst-case capacity");
    check(!schema.capacityForWireBytes(UINT64_MAX,cap),"capacity overflow");
    LLTransportStartupBudget noSchema; initBudget(noSchema,0);
    LLTransportSchema refused;
    const unsigned before=schemaAllocations.load();
    check(freezeTransportSchema(repeated.source,2,noSchema,refused)==LLTransportFreezeStatus::CapacityExceeded,"schema budget refused");
    check(schemaAllocations.load()==before && noSchema.schemaBytesUsed()==0,"refuse before allocation");
    LLTransportStartupBudget tinyTotal; initBudget(tinyTotal,1000000,sizeof(TestQueue));
    check(freezeTransportSchema(repeated.source,3,tinyTotal,refused)==LLTransportFreezeStatus::CapacityExceeded,"total budget refused");
    check(schemaAllocations.load()==before && tinyTotal.schemaBytesUsed()==0,"total refusal before allocation");
    LLTransportStartupBudget tooManyCells;
    check(!(LLTransportStartupBudget::initialize<16384,512,2048,4>(tooManyCells,1024,1000000,sizeof(TestQueue))),"queue cell bound");
    LLTransportStartupBudget tooFewBlocks;
    check((LLTransportStartupBudget::initialize<16384,1,2048,2>(tooFewBlocks,1024,1000000,1000000)),"small queue budget");
    check(freezeTransportSchema(repeated.source,3,tooFewBlocks,refused)==LLTransportFreezeStatus::CapacityExceeded,"255 repeats exceed payload slots");
    check(schemaAllocations.load()==before && tooFewBlocks.schemaBytesUsed()==0,"payload refusal before allocation");
    LLTransportStartupBudget allocFailure; initBudget(allocFailure);
    failSchemaAllocation=before+2;
    check(freezeTransportSchema(repeated.source,4,allocFailure,refused)==LLTransportFreezeStatus::AllocationFailed,"second array OOM");
    failSchemaAllocation=0;
    check(allocFailure.schemaBytesUsed()==0 && !refused.isFrozen(),"allocation failure leaves budget and schema unchanged");
    compare(repeated,many,1,0); // all 255 truncated fixed fields zero-filled
    auto small=std::make_unique<LLTransportDecodedPayload<4,1,1>>();
    auto failed=decodeTransportFields(schema,many.data(),1,0,*small);
    check(failed.status==LLTransportDecodeStatus::CapacityExceeded && !small->blockCount && !small->fieldCount && !small->byteCount,"capacity rollback");
    std::puts("PASS: actual reader loop wire-field comparison and bounded rollback");
}
