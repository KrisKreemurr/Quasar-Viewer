/** Main-built immutable transport schema storage. SPDX-License-Identifier: LGPL-2.1-only */
#include "linden_common.h"
#include "lltransportschemalease.h"
#include "llmessagetemplate.h"
#include "lltransportreceivepump.h"
#include <limits>
#include <new>
#include <utility>

namespace
{
class SchemaLease final : public LLTransportSchemaLease
{
public:
    const LLTransportSchemaEntry* entries() const noexcept override { return mEntries.get(); }
    uint32_t count() const noexcept override { return mCount; }
    uint64_t retainedBytes() const noexcept override { return mBytes; }
    std::unique_ptr<LLTransportSchemaEntry[]> mEntries;
    std::unique_ptr<std::unique_ptr<LLTransportSchema>[]> mSchemas;
    uint32_t mCount=0;
    uint64_t mBytes=0;
};
}

LLTransportSchemaLeaseResult llBuildTransportSchemaLeaseOnMain(
    const std::map<uint32_t, LLMessageTemplate*>& templates, uint64_t generation,
    uint64_t maxRetainedBytes, std::shared_ptr<const LLTransportSchemaLease>& out) noexcept
{
    using Result=LLTransportSchemaLeaseResult;
    const auto count=templates.size();
    if (!generation || !count || count>UINT32_MAX || !maxRetainedBytes) return Result::Invalid;
    const auto maximum=(std::numeric_limits<uint64_t>::max)();
    if (count>(maximum-sizeof(SchemaLease))/sizeof(LLTransportSchemaEntry)) return Result::Capacity;
    uint64_t fixed=sizeof(SchemaLease)+count*uint64_t(sizeof(LLTransportSchemaEntry));
    if (count>(maximum-fixed)/sizeof(std::unique_ptr<LLTransportSchema>)) return Result::Capacity;
    fixed+=count*uint64_t(sizeof(std::unique_ptr<LLTransportSchema>));
    if (fixed>maxRetainedBytes || count>(maxRetainedBytes-fixed)/sizeof(LLTransportSchema) ||
        count>SIZE_MAX/sizeof(LLTransportSchemaEntry) ||
        count>SIZE_MAX/sizeof(std::unique_ptr<LLTransportSchema>))
        return Result::Capacity;
    // The schema freezer charges each schema object plus its arrays. Its
    // fixed queue charge represents a single scratch payload for admission;
    // the graph separately accounts its actual scratch and event queue.
    using ScratchQueue=LLTransportSpscQueue<LLTransportIngressScratch,1>;
    if (maxRetainedBytes-fixed>maximum-sizeof(ScratchQueue)) return Result::Capacity;
    LLTransportStartupBudget budget;
    if (!LLTransportStartupBudget::initialize<65536,2048,16384,1>(budget,NET_BUFFER_SIZE,
            maxRetainedBytes-fixed,maxRetainedBytes-fixed+sizeof(ScratchQueue))) return Result::Capacity;
    try
    {
        auto lease=std::shared_ptr<SchemaLease>(new SchemaLease);
        lease->mEntries.reset(new(std::nothrow) LLTransportSchemaEntry[count]);
        lease->mSchemas.reset(new(std::nothrow) std::unique_ptr<LLTransportSchema>[count]);
        if (!lease->mEntries || !lease->mSchemas) return Result::AllocationFailed;
        uint32_t i=0;
        for (const auto& item : templates)
        {
            if (!item.second || item.first!=item.second->mMessageNumber) return Result::Invalid;
            // Reserve the object's charge before allocating it. The freezer
            // checks and charges this object plus its arrays before allocating
            // either array; previously frozen objects stay in its ledger.
            if (budget.schemaBytesUsed()>maxRetainedBytes-fixed ||
                sizeof(LLTransportSchema)>maxRetainedBytes-fixed-budget.schemaBytesUsed()) return Result::Capacity;
            lease->mSchemas[i].reset(new(std::nothrow) LLTransportSchema);
            if (!lease->mSchemas[i]) return Result::AllocationFailed;
            const auto status=freezeTransportSchema(*item.second,generation,budget,*lease->mSchemas[i]);
            if (status==LLTransportFreezeStatus::AllocationFailed) return Result::AllocationFailed;
            if (status==LLTransportFreezeStatus::CapacityExceeded) return Result::Capacity;
            if (status!=LLTransportFreezeStatus::Complete) return Result::Invalid;
            const bool control=item.first==1 || item.first==2 || item.first==0xfffffffbu;
            lease->mEntries[i]={item.first,lease->mSchemas[i].get(),item.second->getTrust()==MT_TRUST,control};
            ++i;
        }
        lease->mCount=i;
        lease->mBytes=fixed+budget.schemaBytesUsed();
        out=std::move(lease);
        return Result::Complete;
    }
    catch (...) { return Result::AllocationFailed; }
}
