/** Copyright (c) 2026 Slipstream Viewer Project. SPDX-License-Identifier: LGPL-2.1-only */
#include "linden_common.h"
#include "llmessagetemplate.h"
#include "lltransportdecoder.h"
#include <new>

LLTransportFreezeStatus freezeTransportSchema(const LLMessageTemplate& source, uint64_t generation,
                                               LLTransportStartupBudget& budget, LLTransportSchema& result) noexcept
{
    using Status=LLTransportFreezeStatus;
    if (!budget.mInitialized || result.mFrozen || source.mMemberBlocks.size() > UINT32_MAX)
        return Status::InvalidSchema;
    uint64_t uniqueFields=0, maxBlocks=0, maxFields=0, maxBytes=budget.mMaxWire;
    auto add=[](uint64_t& value,uint64_t extra) noexcept
    { if (extra>UINT64_MAX-value) return false; value+=extra; return true; };
    // First pass performs no allocation. Admission includes all schema arrays,
    // 255 variable repeats, configured payload limits and the queue ledger.
    for (const LLMessageBlock* b : source.mMemberBlocks)
    {
        if (!b || b->mMemberVariables.size() > UINT32_MAX) return Status::InvalidSchema;
        uint64_t repeats=0;
        switch (b->mType)
        {
        case MBT_SINGLE: repeats=1; break;
        case MBT_MULTIPLE:
            if (b->mNumber < 0 || b->mNumber > 255) return Status::InvalidSchema;
            repeats=static_cast<uint64_t>(b->mNumber); break;
        case MBT_VARIABLE: repeats=255; break;
        default: return Status::InvalidSchema;
        }
        if (!add(uniqueFields,b->mMemberVariables.size()) || uniqueFields>UINT32_MAX ||
            !add(maxBlocks,repeats) || !add(maxFields,repeats*b->mMemberVariables.size()))
            return Status::CapacityExceeded;
        for (const LLMessageVariable* f : b->mMemberVariables)
        {
            if (!f || f->getSize()<0) return Status::InvalidSchema;
            if (f->getType()==MVT_VARIABLE)
            {
                if (f->getSize()!=1 && f->getSize()!=2 && f->getSize()!=4) return Status::InvalidSchema;
            }
            else if (!add(maxBytes,repeats*static_cast<uint64_t>(f->getSize())))
                return Status::CapacityExceeded;
        }
    }
    if (maxBlocks>budget.mBlocks || maxFields>budget.mFields || maxBytes>budget.mBytes ||
        maxBlocks>UINT32_MAX || maxFields>UINT32_MAX || maxBytes>UINT32_MAX)
        return Status::CapacityExceeded;
    uint64_t schemaBytes=sizeof(LLTransportSchema);
    const uint64_t blockBytes=source.mMemberBlocks.size()*uint64_t(sizeof(LLTransportBlockSchema));
    const uint64_t fieldBytes=uniqueFields*uint64_t(sizeof(LLTransportFieldSchema));
    if (!add(schemaBytes,blockBytes) || !add(schemaBytes,fieldBytes) ||
        budget.mSchemaUsed>budget.mMaxSchema || schemaBytes>budget.mMaxSchema-budget.mSchemaUsed ||
        budget.mOtherBytes>budget.mMaxTotal || budget.mQueueBytes>budget.mMaxTotal-budget.mOtherBytes ||
        budget.mSchemaUsed>budget.mMaxTotal-budget.mOtherBytes-budget.mQueueBytes ||
        schemaBytes>budget.mMaxTotal-budget.mOtherBytes-budget.mQueueBytes-budget.mSchemaUsed ||
        blockBytes>SIZE_MAX || fieldBytes>SIZE_MAX)
        return Status::CapacityExceeded;
    try
    {
        LLTransportSchema candidate;
        candidate.mBlockCount=static_cast<uint32_t>(source.mMemberBlocks.size());
        candidate.mFieldCount=static_cast<uint32_t>(uniqueFields);
        if (candidate.mBlockCount)
        {
            candidate.mBlocks.reset(new(std::nothrow) LLTransportBlockSchema[candidate.mBlockCount]);
            if (!candidate.mBlocks) return Status::AllocationFailed;
        }
        if (candidate.mFieldCount)
        {
            candidate.mFields.reset(new(std::nothrow) LLTransportFieldSchema[candidate.mFieldCount]);
            if (!candidate.mFields) return Status::AllocationFailed;
        }
        uint32_t bi=0, fi=0;
        for (const LLMessageBlock* sourceBlock : source.mMemberBlocks)
        {
            auto& block=candidate.mBlocks[bi++];
            block.repeat=sourceBlock->mType==MBT_SINGLE ? LLTransportBlockSchema::Repeat::Single :
                sourceBlock->mType==MBT_MULTIPLE ? LLTransportBlockSchema::Repeat::Multiple : LLTransportBlockSchema::Repeat::Variable;
            block.count=sourceBlock->mType==MBT_MULTIPLE ? static_cast<uint8_t>(sourceBlock->mNumber) : 1;
            block.firstField=fi;
            block.fieldCount=static_cast<uint32_t>(sourceBlock->mMemberVariables.size());
            for (const LLMessageVariable* sourceField : sourceBlock->mMemberVariables)
            {
                auto& field=candidate.mFields[fi++];
                if (sourceField->getType()==MVT_VARIABLE) field.lengthBytes=static_cast<uint8_t>(sourceField->getSize());
                else field.size=static_cast<uint32_t>(sourceField->getSize());
            }
        }
        candidate.mMessageNumber=source.mMessageNumber; candidate.mGeneration=generation; candidate.mFrozen=true;
        result=std::move(candidate);
        budget.mSchemaUsed+=schemaBytes;
        return Status::Complete;
    }
    catch (...) { return Status::AllocationFailed; }
}
