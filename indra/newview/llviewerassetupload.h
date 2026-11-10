/**
* @file llviewerassetupload.h
* @author optional
* @brief brief description of the file
*
* $LicenseInfo:firstyear=2011&license=viewerlgpl$
* Second Life Viewer Source Code
* Copyright (C) 2011, Linden Research, Inc.
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

#ifndef LL_VIEWER_ASSET_UPLOAD_H
#define LL_VIEWER_ASSET_UPLOAD_H

#include "llfoldertype.h"
#include "llassettype.h"
#include "llinventorytype.h"
#include "lleventcoro.h"
#include "llcoros.h"
#include "llcorehttputil.h"
#include "llimage.h"
#include "llj2cuploadoptions.h"

#include <functional>
#include <memory>

struct LLAudioClip;

//=========================================================================
class LLResourceUploadInfo
{
public:
    typedef std::shared_ptr<LLResourceUploadInfo> ptr_t;

    LLResourceUploadInfo(
        LLTransactionID transactId,
        LLAssetType::EType assetType,
        std::string name,
        std::string description,
        S32 compressionInfo,
        LLFolderType::EType destinationType,
        LLInventoryType::EType inventoryType,
        U32 nextOWnerPerms,
        U32 groupPerms,
        U32 everyonePerms,
        S32 expectedCost,
        const LLUUID &destFolderId = LLUUID::null,
        bool showInventory = true);

    virtual ~LLResourceUploadInfo()
    { }

    virtual LLSD        prepareUpload();
    virtual LLSD        generatePostBody();
    virtual void        logPreparedUpload();
    virtual LLUUID      finishUpload(LLSD &result);

    // return true if no further action is need
    virtual bool        failedUpload(LLSD &result, std::string &reason) { return false; }

    LLTransactionID     getTransactionId() const { return mTransactionId; }
    LLAssetType::EType  getAssetType() const { return mAssetType; }
    std::string         getAssetTypeString() const;
    std::string         getName() const { return mName; };
    std::string         getDescription() const { return mDescription; };
    S32                 getCompressionInfo() const { return mCompressionInfo; };
    LLFolderType::EType getDestinationFolderType() const { return mDestinationFolderType; };
    LLInventoryType::EType  getInventoryType() const { return mInventoryType; };
    std::string         getInventoryTypeString() const;
    U32                 getNextOwnerPerms() const { return mNextOwnerPerms; };
    U32                 getGroupPerms() const { return mGroupPerms; };
    U32                 getEveryonePerms() const { return mEveryonePerms; };
    S32                 getExpectedUploadCost() const { return mExpectedUploadCost; };

    // #1443: shown as a top-right progress bar (a batch of one unless setBatchId() put it in one).
    // Saves of existing items (scripts, notecards) are not.
    virtual bool        tracksProgress() const { return true; }
    // The new item is the batch's link in its toast (inventory is never opened on its own).
    virtual bool        showInventoryPanel() const { return mShowInventory; }

    virtual std::string getDisplayName() const;

    LLUUID              getFolderId() const { return mFolderId; }
    LLUUID              getItemId() const { return mItemId; }
    LLAssetID           getAssetId() const { return mAssetId; }

    static bool         findAssetTypeOfExtension(const std::string& exten, LLAssetType::EType& asset_type);
    static bool         findAssetTypeAndCodecOfExtension(const std::string& exten, LLAssetType::EType& asset_type, U32& codec);

    // #1443 background uploads (lluploadqueue.h). An info with a worker step is prepared on the
    // upload pool before its coroutine runs: beginWorkerPrepare() on the main thread (ids, temp
    // names, settings), prepareOnWorker() on a worker (decode / encode / write the cache file;
    // nothing else), and prepareUpload() then only finishes on the main thread.
    virtual bool        needsWorkerPrepare() const { return false; }
    virtual void        beginWorkerPrepare() {}
    virtual void        prepareOnWorker() {}
    virtual void        discardPrepared() {}            // cancelled after the worker step
    virtual void        onCancelled() {}                // cancelled before it was sent
    // A fresh copy to upload again (a new transaction), or null when that is not safe.
    virtual ptr_t       makeRetry() const { return nullptr; }

    U32                 getBatchId() const { return mBatchId; }
    void                setBatchId(U32 batch) { mBatchId = batch; }
    // false: a step of a batch item (a material's texture) that the item's owner reports.
    bool                countsAsBatchItem() const { return mCountsAsBatchItem; }
    void                setCountsAsBatchItem(bool counts) { mCountsAsBatchItem = counts; }
    void                setPipelineRelease(std::function<void()> release) { mPipelineRelease = release; }
    void                releasePipeline();
    void                setExpectedUploadCost(S32 cost) { mExpectedUploadCost = cost; }

protected:
    LLResourceUploadInfo(
        std::string name,
        std::string description,
        S32 compressionInfo,
        LLFolderType::EType destinationType,
        LLInventoryType::EType inventoryType,
        U32 nextOWnerPerms,
        U32 groupPerms,
        U32 everyonePerms,
        S32 expectedCost,
        const LLUUID& destFolderId = LLUUID::null,
        bool showInventory = true);

    LLResourceUploadInfo(
        LLAssetID assetId,
        LLAssetType::EType assetType,
        std::string name );

    void                setTransactionId(LLTransactionID tid) { mTransactionId = tid; }
    void                setAssetType(LLAssetType::EType assetType) { mAssetType = assetType; }
    void                setItemId(LLUUID itemId) { mItemId = itemId; }

    LLAssetID           generateNewAssetId();
    void                incrementUploadStats() const;
    virtual void        assignDefaults();

    void                setAssetId(LLUUID assetId) { mAssetId = assetId; }

private:
    LLTransactionID     mTransactionId;
    LLAssetType::EType  mAssetType;
    std::string         mName;
    std::string         mDescription;
    S32                 mCompressionInfo;
    LLFolderType::EType mDestinationFolderType;
    LLInventoryType::EType mInventoryType;
    U32                 mNextOwnerPerms;
    U32                 mGroupPerms;
    U32                 mEveryonePerms;
    S32                 mExpectedUploadCost;

    LLUUID              mFolderId;
    LLUUID              mItemId;
    LLAssetID           mAssetId;
    bool                mShowInventory;

    U32                 mBatchId = 0;
    bool                mCountsAsBatchItem = true;
    std::function<void()> mPipelineRelease;
};

//-------------------------------------------------------------------------
class LLNewFileResourceUploadInfo : public LLResourceUploadInfo
{
public:
    LLNewFileResourceUploadInfo(
        std::string fileName,
        std::string name,
        std::string description,
        S32 compressionInfo,
        LLFolderType::EType destinationType,
        LLInventoryType::EType inventoryType,
        U32 nextOWnerPerms,
        U32 groupPerms,
        U32 everyonePerms,
        S32 expectedCost,
        const LLUUID &destFolderId = LLUUID::null,
        bool show_inventory = true);

    virtual LLSD        prepareUpload();

    std::string         getFileName() const { return mFileName; };

    void setMaxImageSize(U32 maxUploadSize) { mMaxImageSize = maxUploadSize; }
    // Bulk upload: an image larger than max_w x max_h is scaled down keeping its aspect (to
    // powers of two) instead of each side on its own, and the cost is checked again against
    // the encoded size before it is sent (never more than the quote the user confirmed).
    void setBulkImageLimit(S32 max_w, S32 max_h) { mBulkMaxW = max_w; mBulkMaxH = max_h; }
    // The size such an image is uploaded at (also what the bulk list quotes).
    static void bulkImageSize(S32 w, S32 h, S32 max_w, S32 max_h, S32& out_w, S32& out_h);

    // Sounds only: upload an already-decoded clip (or a slice of it) instead of decoding
    // the file again. Frames are mono samples at the uploader rate (44.1 kHz).
    void setAudioClip(std::shared_ptr<const LLAudioClip> clip, size_t startFrame, size_t frameCount,
                      size_t fadeInFrames = 0, size_t fadeOutFrames = 0);

    // Images and sounds are decoded and encoded on the upload pool.
    bool                needsWorkerPrepare() const override;
    void                beginWorkerPrepare() override;
    void                prepareOnWorker() override;
    void                discardPrepared() override;
    ptr_t               makeRetry() const override;

protected:

    virtual LLSD        exportTempFile();

private:
    LLSD                encodeToCache();    // worker: the image / sound into the asset cache

    std::string         mFileName;
    S32                 mMaxImageSize;
    S32                 mBulkMaxW = 0;
    S32                 mBulkMaxH = 0;

    std::shared_ptr<const LLAudioClip> mAudioClip;
    size_t              mAudioClipStart = 0;
    size_t              mAudioClipCount = 0;
    size_t              mAudioFadeIn = 0;
    size_t              mAudioFadeOut = 0;

    // Set on the main thread by beginWorkerPrepare(), read by the worker.
    std::string         mTempFile;
    LLJ2CUploadOptions  mJ2COptions;
    // Written by the worker, read on the main thread after it handed the info back.
    bool                mWorkerPrepared = false;
    LLSD                mWorkerResult;
    S32                 mEncodedW = 0;
    S32                 mEncodedH = 0;
};

//-------------------------------------------------------------------------
// use when you have a resource in memory and you want to make a new inventory item
class LLNewBufferedResourceUploadInfo : public LLResourceUploadInfo
{
public:
    typedef std::function<void(LLUUID newAssetId, LLSD response)> uploadFinish_f;
    typedef std::function<bool(LLUUID assetId, LLSD response, std::string reason)> uploadFailure_f;

    LLNewBufferedResourceUploadInfo(
        const std::string& buffer,
        const LLAssetID& asset_id,
        std::string name,
        std::string description,
        S32 compressionInfo,
        LLFolderType::EType destinationType,
        LLInventoryType::EType inventoryType,
        LLAssetType::EType assetType,
        U32 nextOWnerPerms,
        U32 groupPerms,
        U32 everyonePerms,
        S32 expectedCost,
        const LLUUID& destFolderId, // use null for default
        bool show_inventory,
        uploadFinish_f finish,
        uploadFailure_f failure);

    virtual LLSD        prepareUpload();
    // #1465: cancelled from its progress bar before it was sent: the owner hears it as a
    // failure (nothing was sent or charged), so it never waits for an answer that won't come.
    void                onCancelled() override;

protected:

    virtual LLSD        exportTempFile();
    virtual LLUUID      finishUpload(LLSD &result);
    virtual bool        failedUpload(LLSD &result, std::string &reason);

private:
    uploadFinish_f  mFinishFn;
    uploadFailure_f  mFailureFn;
    std::string mBuffer;
};

//-------------------------------------------------------------------------
class LLBufferedAssetUploadInfo : public LLResourceUploadInfo
{
public:
    typedef std::function<void(LLUUID itemId, LLUUID newAssetId, LLUUID newItemId, LLSD response)> invnUploadFinish_f;
    typedef std::function<void(LLUUID itemId, LLUUID taskId, LLUUID newAssetId, LLSD response)> taskUploadFinish_f;
    typedef std::function<bool(LLUUID itemId, LLUUID taskId, LLSD response, std::string reason)> uploadFailed_f;

    // destFolderId is the folder to put the new item in, leave null for default
    LLBufferedAssetUploadInfo(LLUUID itemId, LLAssetType::EType assetType, std::string buffer, invnUploadFinish_f finish, uploadFailed_f failed);
    LLBufferedAssetUploadInfo(LLUUID itemId, LLPointer<LLImageFormatted> image, invnUploadFinish_f finish);
    LLBufferedAssetUploadInfo(LLUUID taskId, LLUUID itemId, LLAssetType::EType assetType, std::string buffer, taskUploadFinish_f finish, uploadFailed_f failed);

    virtual LLSD        prepareUpload();
    virtual LLSD        generatePostBody();
    virtual LLUUID      finishUpload(LLSD &result);
    virtual bool        failedUpload(LLSD &result, std::string &reason);

    LLUUID              getTaskId() const { return mTaskId; }
    const std::string & getContents() const { return mContents; }

    virtual bool        tracksProgress() const { return false; }
    virtual bool        showInventoryPanel() const { return false; }

protected:


private:
    bool                mTaskUpload;
    LLUUID              mTaskId;
    std::string         mContents;
    invnUploadFinish_f  mInvnFinishFn;
    taskUploadFinish_f  mTaskFinishFn;
    uploadFailed_f      mFailureFn;
    bool                mStoredToCache;
};

//-------------------------------------------------------------------------
class LLScriptAssetUpload : public LLBufferedAssetUploadInfo
{
public:
    LLScriptAssetUpload(LLUUID itemId, std::string compileTarget, std::string buffer, invnUploadFinish_f finish, uploadFailed_f failed);
    LLScriptAssetUpload(LLUUID taskId, LLUUID itemId, std::string compileTarget,
            bool isRunning, LLUUID exerienceId, std::string buffer, taskUploadFinish_f finish, uploadFailed_f failed);

    virtual LLSD        generatePostBody();

    LLUUID              getExerienceId() const { return mExerienceId; }
    const std::string&  getCompileTarget() const { return mCompileTarget; }
    bool                getIsRunning() const { return mIsRunning; }

private:
    LLUUID              mExerienceId;
    std::string         mCompileTarget;
    bool                mIsRunning;

};

//=========================================================================
class LLViewerAssetUpload
{
public:
    // #1443: every upload runs in the background. A tracked one gets a progress bar (its own
    // batch unless it already has one); one with a worker step waits for a pipeline slot.
    static void EnqueueInventoryUpload(const std::string &url, const LLResourceUploadInfo::ptr_t &uploadInfo);
    // Straight to the upload coroutine (LLUploadQueue, once the info is prepared).
    static void EnqueueUploadCoproc(const std::string &url, const LLResourceUploadInfo::ptr_t &uploadInfo);

    static void AssetInventoryUploadCoproc(LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t &httpAdapter, const LLUUID &id, std::string url, LLResourceUploadInfo::ptr_t uploadInfo);

private:
    // charge_unknown: the failure came after the file went to the server.
    static void HandleUploadError(LLCore::HttpStatus status, LLSD &result, LLResourceUploadInfo::ptr_t &uploadInfo,
                                  bool charge_unknown = false);
};

#endif // !VIEWER_ASSET_UPLOAD_H
