/**
* @file llviewerassetupload.cpp
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

#include "llviewerprecompiledheaders.h"

#include "linden_common.h"
#include "llviewertexturelist.h"
#include "llimage.h"
#include "llimagej2c.h"
#include "lltrans.h"
#include "lluuid.h"
#include "llvorbisencode.h"
#include "llaudiofiledecode.h"
#include "llagentbenefits.h"
#include "llnotificationsutil.h"
#include "lluploadqueue.h"
#include "llviewermenufile.h"
#include "llagent.h"
#include "llfloaterreg.h"
#include "llfloatersnapshot.h"
#include "llstatusbar.h"
#include "llinventorypanel.h"
#include "llsdutil.h"
#include "llviewerassetupload.h"
#include "llappviewer.h"
#include "llavatarappearance.h"
#include "llviewerstats.h"
#include "llfilesystem.h"
#include "llgesturemgr.h"
#include "llpreviewnotecard.h"
#include "llpreviewgesture.h"
#include "llcoproceduremanager.h"
#include "llthread.h"
#include "llkeyframemotion.h"
#include "lldatapacker.h"
#include "llvoavatarself.h"
#include "lldatapacker.h"
#include "llbvhloader.h"
#include "llbvhconsts.h"

#include <typeinfo>

void dialog_refresh_all();

static constexpr U32 LL_ASSET_UPLOAD_TIMEOUT_SEC = 60;

LLResourceUploadInfo::LLResourceUploadInfo(LLTransactionID transactId,
        LLAssetType::EType assetType, std::string name, std::string description,
        S32 compressionInfo, LLFolderType::EType destinationType,
        LLInventoryType::EType inventoryType, U32 nextOWnerPerms,
        U32 groupPerms, U32 everyonePerms, S32 expectedCost,
        const LLUUID& destFolderId, bool showInventory) :
    mTransactionId(transactId),
    mAssetType(assetType),
    mName(name),
    mDescription(description),
    mCompressionInfo(compressionInfo),
    mDestinationFolderType(destinationType),
    mInventoryType(inventoryType),
    mNextOwnerPerms(nextOWnerPerms),
    mGroupPerms(groupPerms),
    mEveryonePerms(everyonePerms),
    mExpectedUploadCost(expectedCost),
    mShowInventory(showInventory),
    mFolderId(destFolderId),
    mItemId(LLUUID::null),
    mAssetId(LLAssetID::null)
{ }


LLResourceUploadInfo::LLResourceUploadInfo(std::string name,
        std::string description, S32 compressionInfo,
        LLFolderType::EType destinationType, LLInventoryType::EType inventoryType,
        U32 nextOWnerPerms, U32 groupPerms, U32 everyonePerms, S32 expectedCost,
        const LLUUID& destFolderId, bool showInventory) :
    mName(name),
    mDescription(description),
    mCompressionInfo(compressionInfo),
    mDestinationFolderType(destinationType),
    mInventoryType(inventoryType),
    mNextOwnerPerms(nextOWnerPerms),
    mGroupPerms(groupPerms),
    mEveryonePerms(everyonePerms),
    mExpectedUploadCost(expectedCost),
    mShowInventory(showInventory),
    mTransactionId(),
    mAssetType(LLAssetType::AT_NONE),
    mFolderId(destFolderId),
    mItemId(LLUUID::null),
    mAssetId(LLAssetID::null)
{
    mTransactionId.generate();
}

LLResourceUploadInfo::LLResourceUploadInfo(LLAssetID assetId, LLAssetType::EType assetType, std::string name) :
    mAssetId(assetId),
    mAssetType(assetType),
    mName(name),
    mDescription(),
    mCompressionInfo(0),
    mDestinationFolderType(LLFolderType::FT_NONE),
    mInventoryType(LLInventoryType::IT_NONE),
    mNextOwnerPerms(0),
    mGroupPerms(0),
    mEveryonePerms(0),
    mExpectedUploadCost(0),
    mShowInventory(true),
    mTransactionId(),
    mFolderId(LLUUID::null),
    mItemId(LLUUID::null)
{
}

LLSD LLResourceUploadInfo::prepareUpload()
{
    if (mAssetId.isNull())
        generateNewAssetId();

    incrementUploadStats();
    assignDefaults();

    return LLSD().with("success", LLSD::Boolean(true));
}

std::string LLResourceUploadInfo::getAssetTypeString() const
{
    return LLAssetType::lookup(mAssetType);
}

std::string LLResourceUploadInfo::getInventoryTypeString() const
{
    return LLInventoryType::lookup(mInventoryType);
}

LLSD LLResourceUploadInfo::generatePostBody()
{
    LLSD body;

    body["folder_id"] = mFolderId;
    body["asset_type"] = getAssetTypeString();
    body["inventory_type"] = getInventoryTypeString();
    body["name"] = mName;
    body["description"] = mDescription;
    body["next_owner_mask"] = LLSD::Integer(mNextOwnerPerms);
    body["group_mask"] = LLSD::Integer(mGroupPerms);
    body["everyone_mask"] = LLSD::Integer(mEveryonePerms);
    body["expected_upload_cost"] = mExpectedUploadCost;

    return body;
}

void LLResourceUploadInfo::logPreparedUpload()
{
    LL_INFOS() << "*** Uploading: " << std::endl <<
        "Type: " << LLAssetType::lookup(mAssetType) << std::endl <<
        "UUID: " << mAssetId.asString() << std::endl <<
        "Name: " << mName << std::endl <<
        "Desc: " << mDescription << std::endl <<
        "Expected Upload Cost: " << mExpectedUploadCost << std::endl <<
        "Folder: " << mFolderId << std::endl <<
        "Asset Type: " << LLAssetType::lookup(mAssetType) << LL_ENDL;
}

LLUUID LLResourceUploadInfo::finishUpload(LLSD &result)
{
    if (getFolderId().isNull())
    {
        return LLUUID::null;
    }

    U32 permsEveryone = PERM_NONE;
    U32 permsGroup = PERM_NONE;
    U32 permsNextOwner = PERM_ALL;

    if (result.has("new_next_owner_mask"))
    {
        // The server provided creation perms so use them.
        // Do not assume we got the perms we asked for in
        // since the server may not have granted them all.
        permsEveryone = result["new_everyone_mask"].asInteger();
        permsGroup = result["new_group_mask"].asInteger();
        permsNextOwner = result["new_next_owner_mask"].asInteger();
    }
    else
    {
        // The server doesn't provide creation perms
        // so use old assumption-based perms.
        if (getAssetTypeString() != "snapshot")
        {
            permsNextOwner = PERM_MOVE | PERM_TRANSFER;
        }
    }

    LLPermissions new_perms;
    new_perms.init(
        gAgent.getID(),
        gAgent.getID(),
        LLUUID::null,
        LLUUID::null);

    new_perms.initMasks(
        PERM_ALL,
        PERM_ALL,
        permsEveryone,
        permsGroup,
        permsNextOwner);

    U32 flagsInventoryItem = 0;
    if (result.has("inventory_flags"))
    {
        flagsInventoryItem = static_cast<U32>(result["inventory_flags"].asInteger());
        if (flagsInventoryItem != 0)
        {
            LL_INFOS() << "inventory_item_flags " << flagsInventoryItem << LL_ENDL;
        }
    }
    S32 creationDate = (S32)time_corrected();

    LLUUID serverInventoryItem = result["new_inventory_item"].asUUID();
    LLUUID serverAssetId = result["new_asset"].asUUID();

    LLPointer<LLViewerInventoryItem> item = new LLViewerInventoryItem(
        serverInventoryItem,
        getFolderId(),
        new_perms,
        serverAssetId,
        getAssetType(),
        getInventoryType(),
        getName(),
        getDescription(),
        LLSaleInfo::DEFAULT,
        flagsInventoryItem,
        creationDate);

    gInventory.updateItem(item);
    gInventory.notifyObservers();

    return serverInventoryItem;
}


LLAssetID LLResourceUploadInfo::generateNewAssetId()
{
    if (gDisconnected)
    {
        LLAssetID rv;

        rv.setNull();
        return rv;
    }
    mAssetId = mTransactionId.makeAssetID(gAgent.getSecureSessionID());

    return mAssetId;
}

void LLResourceUploadInfo::incrementUploadStats() const
{
    if (LLAssetType::AT_SOUND == mAssetType)
    {
        add(LLStatViewer::UPLOAD_SOUND, 1);
    }
    else if (LLAssetType::AT_TEXTURE == mAssetType)
    {
        add(LLStatViewer::UPLOAD_TEXTURE, 1);
    }
    else if (LLAssetType::AT_ANIMATION == mAssetType)
    {
        add(LLStatViewer::ANIMATION_UPLOADS, 1);
    }
}

void LLResourceUploadInfo::assignDefaults()
{
    if (LLInventoryType::IT_NONE == mInventoryType)
    {
        mInventoryType = LLInventoryType::defaultForAssetType(mAssetType);
    }
    LLStringUtil::stripNonprintable(mName);
    LLStringUtil::stripNonprintable(mDescription);

    if (mName.empty())
    {
        mName = "(No Name)";
    }
    if (mDescription.empty())
    {
        mDescription = "(No Description)";
    }

    if (mAssetType == LLAssetType::AT_GLTF ||
        mAssetType == LLAssetType::AT_GLTF_BIN)
    {
        mFolderId = LLUUID::null;
    }
    else
    {
        mFolderId = gInventory.findUserDefinedCategoryUUIDForType(
            (mDestinationFolderType == LLFolderType::FT_NONE) ?
            (LLFolderType::EType)mAssetType : mDestinationFolderType);
    }
}

std::string LLResourceUploadInfo::getDisplayName() const
{
    return (mName.empty()) ? mAssetId.asString() : mName;
};

bool LLResourceUploadInfo::findAssetTypeOfExtension(const std::string& exten, LLAssetType::EType& asset_type)
{
    U32 codec;
    return findAssetTypeAndCodecOfExtension(exten, asset_type, codec);
}

// static
bool LLResourceUploadInfo::findAssetTypeAndCodecOfExtension(const std::string& exten, LLAssetType::EType& asset_type, U32& codec)
{
    bool succ = false;
    std::string exten_lc(exten);
    LLStringUtil::toLower(exten_lc);
    codec = LLImageBase::getCodecFromExtension(exten_lc);
    if (codec != IMG_CODEC_INVALID)
    {
        asset_type = LLAssetType::AT_TEXTURE;
        succ = true;
    }
    else if (exten_lc == "wav" || exten_lc == "ogg" || exten_lc == "mp3"
          || exten_lc == "opus" || exten_lc == "flac")
    {
        // All of these are decoded in the viewer and re-encoded to Ogg Vorbis for upload.
        asset_type = LLAssetType::AT_SOUND;
        succ = true;
    }
    else if (exten_lc == "anim")
    {
        asset_type = LLAssetType::AT_ANIMATION;
        succ = true;
    }
    else if (exten_lc == "bvh")
    {
        asset_type = LLAssetType::AT_ANIMATION;
        succ = true;
    }

    return succ;
}

//=========================================================================
LLNewFileResourceUploadInfo::LLNewFileResourceUploadInfo(
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
    const LLUUID& destFolderId,
    bool show_inventory) :
    LLResourceUploadInfo(name, description, compressionInfo,
    destinationType, inventoryType,
    nextOWnerPerms, groupPerms, everyonePerms, expectedCost,
    destFolderId, show_inventory),
    mFileName(fileName),
    mMaxImageSize(LLViewerFetchedTexture::MAX_IMAGE_SIZE_DEFAULT)
{
}

void LLNewFileResourceUploadInfo::setAudioClip(std::shared_ptr<const LLAudioClip> clip,
                                               size_t startFrame, size_t frameCount,
                                               size_t fadeInFrames, size_t fadeOutFrames)
{
    mAudioClip = clip;
    mAudioClipStart = startFrame;
    mAudioClipCount = frameCount;
    mAudioFadeIn = fadeInFrames;
    mAudioFadeOut = fadeOutFrames;
}

LLSD LLNewFileResourceUploadInfo::prepareUpload()
{
    if (getAssetId().isNull())
        generateNewAssetId();

    // Images and sounds were encoded on the upload pool (LLUploadQueue); what is left is cheap.
    LLSD result = mWorkerPrepared ? mWorkerResult : exportTempFile();
    if (result.has("error"))
        return result;

    if (mBulkMaxW > 0 && getAssetType() == LLAssetType::AT_TEXTURE && mEncodedW > 0 && mEncodedH > 0)
    {
        // The quote came from the file's header; the charge follows the encoded size. Never
        // send one that costs more than the user confirmed.
        const S32 cost = LLAgentBenefitsMgr::current().getTextureUploadCost(mEncodedW, mEncodedH);
        if (cost > getExpectedUploadCost())
        {
            discardPrepared();
            LLSD error;
            error["error"] = LLSD::Boolean(true);
            error["label"] = "ErrorMessage";
            error["message"] = llformat("The encoded image (%d x %d) costs L$%d, more than the L$%d confirmed; not uploaded.",
                                        mEncodedW, mEncodedH, cost, getExpectedUploadCost());
            return error;
        }
        setExpectedUploadCost(cost);
    }

    return LLResourceUploadInfo::prepareUpload();
}

// static
void LLNewFileResourceUploadInfo::bulkImageSize(S32 w, S32 h, S32 max_w, S32 max_h, S32& out_w, S32& out_h)
{
    const S32 cap = LLViewerFetchedTexture::MAX_IMAGE_SIZE_DEFAULT;
    if (w > max_w || h > max_h)
    {
        // Scale to fit, keeping the aspect ratio, to powers of two.
        const F32 scale = llmin((F32)max_w / (F32)w, (F32)max_h / (F32)h);
        out_w = LLImageRaw::contractDimToPowerOfTwo(llclamp((S32)llroundf(w * scale), 4, max_w));
        out_h = LLImageRaw::contractDimToPowerOfTwo(llclamp((S32)llroundf(h * scale), 4, max_h));
    }
    else
    {
        out_w = w;
        out_h = h;
    }
    out_w = LLImageRaw::biasedDimToPowerOfTwo(out_w, cap);
    out_h = LLImageRaw::biasedDimToPowerOfTwo(out_h, cap);
}

bool LLNewFileResourceUploadInfo::needsWorkerPrepare() const
{
    if (mAudioClip)
    {
        return true;
    }
    LLAssetType::EType type = LLAssetType::AT_NONE;
    U32 codec = IMG_CODEC_INVALID;
    return findAssetTypeAndCodecOfExtension(gDirUtilp->getExtension(getFileName()), type, codec)
        && (type == LLAssetType::AT_TEXTURE || type == LLAssetType::AT_SOUND);
}

void LLNewFileResourceUploadInfo::beginWorkerPrepare()
{
    // Main thread: everything the worker must not look up itself.
    if (getAssetId().isNull())
    {
        generateNewAssetId();
    }
    LLAssetType::EType type = LLAssetType::AT_NONE;
    U32 codec = IMG_CODEC_INVALID;
    if (mAudioClip)
    {
        type = LLAssetType::AT_SOUND;
    }
    else
    {
        findAssetTypeAndCodecOfExtension(gDirUtilp->getExtension(getFileName()), type, codec);
    }
    setAssetType(type);
    mTempFile = gDirUtilp->getTempFilename();
    mJ2COptions = LLJ2CUploadOptions::fromSettings();
    mWorkerPrepared = false;
    mWorkerResult = LLSD();
    mEncodedW = mEncodedH = 0;
}

void LLNewFileResourceUploadInfo::prepareOnWorker()
{
    LLSD result;
    try
    {
        result = encodeToCache();
    }
    catch (const std::bad_alloc&)
    {
        result["error"] = LLSD::Boolean(true);
        result["label"] = "ErrorMessage";
        result["message"] = llformat("Not enough memory to prepare %s for upload.", getFileName().c_str());
    }
    catch (...)
    {
        LOG_UNHANDLED_EXCEPTION("upload encode");
        result["error"] = LLSD::Boolean(true);
        result["label"] = "ErrorMessage";
        result["message"] = llformat("Could not prepare %s for upload.", getFileName().c_str());
    }
    if (!mTempFile.empty())
    {
        LLFile::remove(mTempFile, ENOENT);
    }
    mWorkerResult = result;
    mWorkerPrepared = true;
}

void LLNewFileResourceUploadInfo::discardPrepared()
{
    if (mWorkerPrepared && getAssetId().notNull())
    {
        LLFileSystem::removeFile(getAssetId(), getAssetType(), ENOENT);
    }
}

LLResourceUploadInfo::ptr_t LLNewFileResourceUploadInfo::makeRetry() const
{
    // Subclasses report to their own batches (sound split, sound notecard) and retry there.
    if (typeid(*this) != typeid(LLNewFileResourceUploadInfo))
    {
        return nullptr;
    }
    std::shared_ptr<LLNewFileResourceUploadInfo> copy = std::make_shared<LLNewFileResourceUploadInfo>(*this);
    LLTransactionID tid;
    tid.generate();
    copy->setTransactionId(tid);
    copy->setAssetId(LLUUID::null);
    copy->setBatchId(0);
    copy->setPipelineRelease(nullptr);
    copy->mWorkerPrepared = false;
    copy->mWorkerResult = LLSD();
    copy->mTempFile.clear();
    return copy;
}

// Worker thread: decode / encode the file into the asset cache. Reads nothing but this info
// (filled on the main thread by beginWorkerPrepare) and the file.
LLSD LLNewFileResourceUploadInfo::encodeToCache()
{
    auto error = [](const std::string& message, const std::string& label)
    {
        LLSD e(LLSD::emptyMap());
        e["error"] = LLSD::Boolean(true);
        e["message"] = message;
        e["label"] = label;
        return e;
    };

    if (getAssetId().isNull())
    {
        return error("Not connected: nothing was uploaded.", "ErrorMessage");
    }

    const LLAssetType::EType assetType = getAssetType();
    const std::string& filename = mTempFile;

    if (assetType == LLAssetType::AT_TEXTURE)
    {
        U32 codec = LLImageBase::getCodecFromExtension(gDirUtilp->getExtension(getFileName()));
        if (mBulkMaxW > 0)
        {
            // Bulk: fit inside the bulk limit keeping the aspect (bulkImageSize), as quoted.
            LLPointer<LLImageFormatted> image = LLImageFormatted::createFromType(codec);
            LLPointer<LLImageRaw> raw = new LLImageRaw;
            if (image.isNull() || !image->load(getFileName()) || !image->decode(raw, 0.0f))
            {
                return error(llformat("Problem with file %s:\n\n%s\n", getFileName().c_str(),
                                      LLImage::getLastThreadError().c_str()), "ProblemWithFile");
            }
            if (image->getComponents() != 3 && image->getComponents() != 4)
            {
                return error(llformat("%s has %d colour channels; only RGB and RGBA images can be uploaded.",
                                      getFileName().c_str(), (S32)image->getComponents()), "ErrorMessage");
            }
            S32 w = 0, h = 0;
            bulkImageSize(raw->getWidth(), raw->getHeight(), mBulkMaxW, mBulkMaxH, w, h);
            if ((w != raw->getWidth() || h != raw->getHeight()) && !raw->scale(w, h))
            {
                return error(llformat("Could not scale %s to %d x %d.", getFileName().c_str(), w, h), "ErrorMessage");
            }
            LLPointer<LLImageJ2C> j2c = LLViewerTextureList::convertToUploadFile(raw, LLViewerFetchedTexture::MAX_IMAGE_SIZE_DEFAULT,
                                                                                false, false, mJ2COptions);
            if (j2c.isNull() || j2c->getDataSize() <= 0)
            {
                return error(llformat("Could not encode %s.", getFileName().c_str()), "ErrorMessage");
            }
            mEncodedW = j2c->getWidth();
            mEncodedH = j2c->getHeight();
            LLFileSystem file(getAssetId(), assetType, LLFileSystem::WRITE);
            if (!file.write(j2c->getData(), j2c->getDataSize()))
            {
                return error(llformat("Could not write %s to the cache.", getFileName().c_str()), "ErrorMessage");
            }
            return LLSD();
        }

        // It's an image file, the upload procedure is the same for all
        if (!LLViewerTextureList::createUploadFile(getFileName(), filename, (U8)codec, mMaxImageSize, 0, false, mJ2COptions))
        {
            return error(llformat("Problem with file %s:\n\n%s\n",
                getFileName().c_str(), LLImage::getLastThreadError().c_str()), "ProblemWithFile");
        }
    }
    else if (assetType == LLAssetType::AT_SOUND)
    {
        S32 encodeResult = 0;

        std::string exten_lc = gDirUtilp->getExtension(getFileName());
        LLStringUtil::toLower(exten_lc);

        if (mAudioClip)
        {
            // Already decoded (sound preview floater / split upload): encode the chosen slice.
            LL_INFOS() << "Encoding decoded audio (" << mAudioClipCount << " frames) as an ogg file" << LL_ENDL;
            encodeResult = encode_vorbis_pcm(mAudioClip->mSamples.data() + mAudioClipStart, mAudioClipCount,
                                             LLAUDIO_UPLOAD_SAMPLE_RATE, filename, mAudioFadeIn, mAudioFadeOut);
        }
        else if (exten_lc == "wav")
        {
            LL_INFOS() << "Attempting to encode wav as an ogg file" << LL_ENDL;

            encodeResult = encode_vorbis_file(getFileName(), filename);
        }
        else
        {
            // Ogg / MP3 / Opus / FLAC: decode to PCM, then through the same encoder as WAV.
            // An .ogg is never passed through: it is decoded and re-encoded like the rest.
            LL_INFOS() << "Decoding " << exten_lc << " for re-encoding as an ogg file" << LL_ENDL;
            LLAudioClip clip;
            std::string decodeError;
            if (llaudio_decode_file(getFileName(), clip, decodeError) != LLAUDIO_DEC_OK)
            {
                return error(llformat("Could not decode %s: %s\n", getFileName().c_str(), decodeError.c_str()), decodeError);
            }
            if (clip.getDuration() > LLAUDIO_UPLOAD_LIMIT_SECONDS)
            {
                return error(llformat("Audio is too long to upload as one sound: %s\n", getFileName().c_str()),
                             "SoundFileTooLongForUpload");
            }
            encodeResult = encode_vorbis_pcm(clip.mSamples.data(), clip.mSamples.size(),
                                             LLAUDIO_UPLOAD_SAMPLE_RATE, filename);
        }

        if (LLVORBISENC_NOERR != encodeResult)
        {
            if (encodeResult == LLVORBISENC_DEST_OPEN_ERR)
            {
                return error(llformat("Couldn't open temporary compressed sound file for writing: %s\n", filename.c_str()),
                             "CannotOpenTemporarySoundFile");
            }
            return error(llformat("Unknown vorbis encode failure on: %s\n", getFileName().c_str()), "UnknownVorbisEncodeFailure");
        }
    }
    else
    {
        return error(llformat(LLTrans::getString("UnknownFileExtension").c_str(),
                              gDirUtilp->getExtension(getFileName()).c_str()), "ErrorMessage");
    }

    // copy the encoded file into the cache for upload
    LLFILE* infile = LLFile::fopen(filename, "rb");
    if (!infile)
    {
        return error(llformat("Unable to access output file: %s", filename.c_str()), "ErrorMessage");
    }
    bool written = true;
    {
        LLFileSystem file(getAssetId(), assetType, LLFileSystem::WRITE);
        std::vector<U8> copy_buf(65536);
        size_t got = 0;
        while (written && (got = fread(copy_buf.data(), 1, copy_buf.size(), infile)) > 0)
        {
            written = file.write(copy_buf.data(), (S32)got);
        }
    }
    LLFile::close(infile);
    if (!written)
    {
        return error(llformat("Could not write %s to the cache.", getFileName().c_str()), "ErrorMessage");
    }
    return LLSD();
}

LLSD LLNewFileResourceUploadInfo::exportTempFile()
{
    if (needsWorkerPrepare())
    {
        // Not through the upload pool (the legacy asset path): the same steps, here.
        beginWorkerPrepare();
        prepareOnWorker();
        return mWorkerResult;
    }

    std::string filename = gDirUtilp->getTempFilename();

    std::string exten = gDirUtilp->getExtension(getFileName());

    LLAssetType::EType assetType = LLAssetType::AT_NONE;
    U32 codec = IMG_CODEC_INVALID;
    bool found_type = findAssetTypeAndCodecOfExtension(exten, assetType, codec);

    std::string errorMessage;
    std::string errorLabel;

    bool error = false;

    if (exten.empty())
    {
        std::string shortName = gDirUtilp->getBaseFileName(filename);

        // No extension
        errorMessage = llformat(
            "No file extension for the file: '%s'\nPlease make sure the file has a correct file extension",
            shortName.c_str());
        errorLabel = "NoFileExtension";
        error = true;
    }
    else if (!found_type)
    {
        // Unknown extension
        errorMessage = llformat(LLTrans::getString("UnknownFileExtension").c_str(), exten.c_str());
        errorLabel = "ErrorMessage";
        error = true;;
    }
    else if (exten == "bvh")
    {
        assetType = LLAssetType::AT_ANIMATION;
        S32 file_size;
        LLAPRFile infile;
        infile.open(filename, LL_APR_RB, nullptr, &file_size);
        if (!infile.getFileHandle())
        {
            LL_WARNS() << "Can't open BVH file:" << filename << LL_ENDL;
        }
        else
        {
            auto joint_aliases = LLAvatarAppearance::buildJointAliases();

            char*        file_buffer = new char[file_size + 1];
            ELoadStatus  load_status = E_ST_OK;
            S32          line_number = 0;
            LLBVHLoader* loaderp     = new LLBVHLoader(file_buffer, load_status, line_number, joint_aliases);

            if (load_status == E_ST_NO_XLT_FILE)
            {
            LL_WARNS() << "NOTE: No translation table found." << LL_ENDL;
            }
            else
            {
            LL_WARNS() << "ERROR: [line: " << line_number << "] " << BVHSTATUS[load_status] << LL_ENDL;
            }
            // create data buffer for keyframe initialization
            S32                      buffer_size = loaderp->getOutputSize();
            U8*                      buffer      = new U8[buffer_size];
            LLDataPackerBinaryBuffer dp(buffer, buffer_size);

            // pass animation data through memory buffer
            loaderp->serialize(dp);
            LLAPRFile apr_file(filename, LL_APR_WB);
            apr_file.write(buffer, buffer_size);
            apr_file.close();
            delete[] file_buffer;
            delete[] buffer;
            delete loaderp;
        }
        infile.close();
    }
    else if (exten == "anim")
    {
        // Default unless everything succeeds
        errorLabel = "ProblemWithFile";
        error = true;

        // read from getFileName()
        LLAPRFile infile;
        infile.open(getFileName(),LL_APR_RB);
        if (!infile.getFileHandle())
        {
            LL_WARNS() << "Couldn't open file for reading: " << getFileName() << LL_ENDL;
            errorMessage = llformat("Failed to open animation file %s\n", getFileName().c_str());
        }
        else
        {
            S32 size = LLAPRFile::size(getFileName());
            U8* buffer = new(std::nothrow) U8[size];
            if (!buffer)
            {
                LLError::LLUserWarningMsg::showOutOfMemory();
                LL_ERRS() << "Bad memory allocation for buffer, size: " << size << LL_ENDL;
            }
            S32 size_read = infile.read(buffer,size);
            if (size_read != size)
            {
                errorMessage = llformat("Failed to read animation file %s: wanted %d bytes, got %d\n", getFileName().c_str(), size, size_read);
            }
            else
            {
                LLDataPackerBinaryBuffer dp(buffer, size);
                LLKeyframeMotion *motionp = new LLKeyframeMotion(getAssetId());
                motionp->setCharacter(gAgentAvatarp);
                if (motionp->deserialize(dp, getAssetId(), false))
                {
                    // write to temp file
                    bool succ = motionp->dumpToFile(filename);
                    if (succ)
                    {
                        assetType = LLAssetType::AT_ANIMATION;
                        errorLabel = "";
                        error = false;
                    }
                    else
                    {
                        errorMessage = "Failed saving temporary animation file";
                    }
                }
                else
                {
                    errorMessage = "Failed reading animation file";
                }
            }
        }
    }
    else
    {
        // Unknown extension
        errorMessage = llformat(LLTrans::getString("UnknownFileExtension").c_str(), exten.c_str());
        errorLabel = "ErrorMessage";
        error = true;;
    }

    if (error)
    {
        LLSD errorResult(LLSD::emptyMap());

        errorResult["error"] = LLSD::Binary(true);
        errorResult["message"] = errorMessage;
        errorResult["label"] = errorLabel;
        return errorResult;
    }

    setAssetType(assetType);

    // copy this file into the cache for upload
    S32 file_size;
    LLAPRFile infile;
    infile.open(filename, LL_APR_RB, NULL, &file_size);
    if (infile.getFileHandle())
    {
        LLFileSystem file(getAssetId(), assetType, LLFileSystem::APPEND);

        const S32 buf_size = 65536;
        U8 copy_buf[buf_size];
        while ((file_size = infile.read(copy_buf, buf_size)))
        {
            file.write(copy_buf, file_size);
        }
    }
    else
    {
        errorMessage = llformat("Unable to access output file: %s", filename.c_str());
        LLSD errorResult(LLSD::emptyMap());

        errorResult["error"] = LLSD::Binary(true);
        errorResult["message"] = errorMessage;
        return errorResult;
    }

    return LLSD();

}

//=========================================================================
LLNewBufferedResourceUploadInfo::LLNewBufferedResourceUploadInfo(
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
    const LLUUID& destFolderId,
    bool show_inventory,
    uploadFinish_f finish,
    uploadFailure_f failure)
    : LLResourceUploadInfo(name, description, compressionInfo,
        destinationType, inventoryType,
        nextOWnerPerms, groupPerms, everyonePerms, expectedCost, destFolderId, show_inventory)
    , mBuffer(buffer)
    , mFinishFn(finish)
    , mFailureFn(failure)
{
    setAssetType(assetType);
    setAssetId(asset_id);
}

LLSD LLNewBufferedResourceUploadInfo::prepareUpload()
{
    if (getAssetId().isNull())
        generateNewAssetId();

    LLSD result = exportTempFile();
    if (result.has("error"))
        return result;

    return LLResourceUploadInfo::prepareUpload();
}

LLSD LLNewBufferedResourceUploadInfo::exportTempFile()
{
    std::string filename = gDirUtilp->getTempFilename();

    // copy buffer to the cache for upload
    LLFileSystem file(getAssetId(), getAssetType(), LLFileSystem::APPEND);
    file.write((U8*) mBuffer.c_str(), static_cast<S32>(mBuffer.size()));

    return LLSD();
}

LLUUID LLNewBufferedResourceUploadInfo::finishUpload(LLSD &result)
{
    LLUUID newItemId = LLResourceUploadInfo::finishUpload(result);

    if (mFinishFn)
    {
        mFinishFn(result["new_asset"].asUUID(), result);
    }

    return newItemId;
}

bool LLNewBufferedResourceUploadInfo::failedUpload(LLSD &result, std::string &reason)
{
    if (mFailureFn)
    {
        return mFailureFn(getAssetId(), result, reason);
    }

    return false; // Not handled
}

void LLNewBufferedResourceUploadInfo::onCancelled()
{
    uploadFailure_f failed;
    failed.swap(mFailureFn); // once
    if (failed)
    {
        failed(getAssetId(), LLSD().with("cancelled", true), "cancelled before it was sent");
    }
}

//=========================================================================
LLBufferedAssetUploadInfo::LLBufferedAssetUploadInfo(LLUUID itemId, LLAssetType::EType assetType, std::string buffer, invnUploadFinish_f finish, uploadFailed_f failed) :
    LLResourceUploadInfo(std::string(), std::string(), 0, LLFolderType::FT_NONE, LLInventoryType::IT_NONE,
        0, 0, 0, 0),
    mTaskUpload(false),
    mTaskId(LLUUID::null),
    mContents(buffer),
    mInvnFinishFn(finish),
    mTaskFinishFn(nullptr),
    mFailureFn(failed),
    mStoredToCache(false)
{
    setItemId(itemId);
    setAssetType(assetType);
}

LLBufferedAssetUploadInfo::LLBufferedAssetUploadInfo(LLUUID itemId, LLPointer<LLImageFormatted> image, invnUploadFinish_f finish) :
    LLResourceUploadInfo(std::string(), std::string(), 0, LLFolderType::FT_NONE, LLInventoryType::IT_NONE,
        0, 0, 0, 0),
    mTaskUpload(false),
    mTaskId(LLUUID::null),
    mContents(),
    mInvnFinishFn(finish),
    mTaskFinishFn(nullptr),
    mFailureFn(nullptr),
    mStoredToCache(false)
{
    setItemId(itemId);

    LLImageDataSharedLock lock(image);

    EImageCodec codec = static_cast<EImageCodec>(image->getCodec());

    switch (codec)
    {
    case IMG_CODEC_JPEG:
        setAssetType(LLAssetType::AT_IMAGE_JPEG);
        LL_INFOS() << "Upload Asset type set to JPEG." << LL_ENDL;
        break;
    case IMG_CODEC_TGA:
        setAssetType(LLAssetType::AT_IMAGE_TGA);
        LL_INFOS() << "Upload Asset type set to TGA." << LL_ENDL;
        break;
    default:
        LL_WARNS() << "Unknown codec to asset type transition. Codec=" << (int)codec << "." << LL_ENDL;
        break;
    }

    size_t imageSize = image->getDataSize();
    mContents.reserve(imageSize);
    mContents.assign((char *)image->getData(), imageSize);
}

LLBufferedAssetUploadInfo::LLBufferedAssetUploadInfo(LLUUID taskId, LLUUID itemId, LLAssetType::EType assetType, std::string buffer, taskUploadFinish_f finish, uploadFailed_f failed) :
    LLResourceUploadInfo(std::string(), std::string(), 0, LLFolderType::FT_NONE, LLInventoryType::IT_NONE,
        0, 0, 0, 0),
    mTaskUpload(true),
    mTaskId(taskId),
    mContents(buffer),
    mInvnFinishFn(nullptr),
    mTaskFinishFn(finish),
    mFailureFn(failed),
    mStoredToCache(false)
{
    setItemId(itemId);
    setAssetType(assetType);
}

LLSD LLBufferedAssetUploadInfo::prepareUpload()
{
    if (getAssetId().isNull())
        generateNewAssetId();

    LLFileSystem file(getAssetId(), getAssetType(), LLFileSystem::APPEND);

    S32 size = static_cast<S32>(mContents.length()) + 1;
    file.write((U8*)mContents.c_str(), size);

    mStoredToCache = true;

    return LLSD().with("success", LLSD::Boolean(true));
}

LLSD LLBufferedAssetUploadInfo::generatePostBody()
{
    LLSD body;

    if (!getTaskId().isNull())
    {
        body["task_id"] = getTaskId();
    }
    body["item_id"] = getItemId();

    return body;
}

LLUUID LLBufferedAssetUploadInfo::finishUpload(LLSD &result)
{
    LLUUID newAssetId = result["new_asset"].asUUID();
    LLUUID itemId = getItemId();

    if (mStoredToCache)
    {
        LLAssetType::EType assetType(getAssetType());
        LLFileSystem::renameFile(getAssetId(), assetType, newAssetId, assetType);
    }

    if (mTaskUpload)
    {
        LLUUID taskId = getTaskId();

        dialog_refresh_all();

        if (mTaskFinishFn)
        {
            mTaskFinishFn(itemId, taskId, newAssetId, result);
        }
    }
    else
    {
        LLUUID newItemId(LLUUID::null);

        if (itemId.notNull())
        {
            LLViewerInventoryItem* item = (LLViewerInventoryItem*)gInventory.getItem(itemId);
            if (!item)
            {
                LL_WARNS() << "Inventory item for " << getDisplayName() << " is no longer in agent inventory." << LL_ENDL;
                return newAssetId;
            }

            // Update viewer inventory item
            LLPointer<LLViewerInventoryItem> newItem = new LLViewerInventoryItem(item);
            newItem->setAssetUUID(newAssetId);

            gInventory.updateItem(newItem);

            newItemId = newItem->getUUID();
            LL_INFOS() << "Inventory item " << item->getName() << " saved into " << newAssetId.asString() << LL_ENDL;
        }

        if (mInvnFinishFn)
        {
            mInvnFinishFn(itemId, newAssetId, newItemId, result);
        }
        gInventory.notifyObservers();
    }

    return newAssetId;
}

bool LLBufferedAssetUploadInfo::failedUpload(LLSD &result, std::string &reason)
{
    if (mFailureFn)
    {
        return mFailureFn(getItemId(), getTaskId(), result, reason);
    }
    return false;
}

//=========================================================================

LLScriptAssetUpload::LLScriptAssetUpload(LLUUID itemId, std::string compileTarget, std::string buffer, invnUploadFinish_f finish, uploadFailed_f failed) :
    LLBufferedAssetUploadInfo(itemId, LLAssetType::AT_LSL_TEXT, buffer, finish, failed),
    mExerienceId(),
    mCompileTarget(compileTarget),
    mIsRunning(false)
{
}

LLScriptAssetUpload::LLScriptAssetUpload(LLUUID taskId, LLUUID itemId, std::string compileTarget,
        bool isRunning, LLUUID exerienceId, std::string buffer, taskUploadFinish_f finish, uploadFailed_f failed):
    LLBufferedAssetUploadInfo(taskId, itemId, LLAssetType::AT_LSL_TEXT, buffer, finish, failed),
    mExerienceId(exerienceId),
    mCompileTarget(compileTarget),
    mIsRunning(isRunning)
{
}

LLSD LLScriptAssetUpload::generatePostBody()
{
    LLSD body;

    if (getTaskId().isNull())
    {
        body["item_id"] = getItemId();
        body["target"] = getCompileTarget();
    }
    else
    {
        body["task_id"] = getTaskId();
        body["item_id"] = getItemId();
        body["is_script_running"] = getIsRunning();
        body["target"] = getCompileTarget();
        body["experience"] = getExerienceId();
    }

    return body;
}

//=========================================================================
void LLResourceUploadInfo::releasePipeline()
{
    if (mPipelineRelease)
    {
        std::function<void()> release;
        release.swap(mPipelineRelease);
        release();
    }
}

//=========================================================================
/*static*/
void LLViewerAssetUpload::EnqueueInventoryUpload(const std::string &url, const LLResourceUploadInfo::ptr_t &uploadInfo)
{
    if (!uploadInfo)
    {
        return;
    }
    // #1443: a bar of its own unless the caller put it in a batch.
    if (!uploadInfo->getBatchId() && uploadInfo->tracksProgress())
    {
        uploadInfo->setBatchId(LLUploadTracker::instance().begin(uploadInfo->getDisplayName(), 1,
                                                                 uploadInfo->getExpectedUploadCost(), true));
    }
    LLUploadQueue::instance().submit(url, uploadInfo);
}

/*static*/
void LLViewerAssetUpload::EnqueueUploadCoproc(const std::string &url, const LLResourceUploadInfo::ptr_t &uploadInfo)
{
    std::string procName("LLViewerAssetUpload::AssetInventoryUploadCoproc(");

    LLCoprocedureManager::instance().enqueueCoprocedure("Upload",
        procName + LLAssetType::lookup(uploadInfo->getAssetType()) + ")",
        boost::bind(&LLViewerAssetUpload::AssetInventoryUploadCoproc, _1, _2, url, uploadInfo));
}

namespace
{
    // However the upload coroutine ends: the pipeline slot is freed and an answered request
    // leaves the batch's in-flight count.
    struct UploadCoprocScope
    {
        LLResourceUploadInfo::ptr_t mInfo;
        bool mSent = false;

        ~UploadCoprocScope()
        {
            if (mSent && LLUploadTracker::instanceExists())
            {
                LLUploadTracker::instance().answered(mInfo->getBatchId());
            }
            mInfo->releasePipeline();
        }
    };
}

//=========================================================================
/*static*/
void LLViewerAssetUpload::AssetInventoryUploadCoproc(LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t &httpAdapter,
    const LLUUID &id, std::string url, LLResourceUploadInfo::ptr_t uploadInfo)
{
    UploadCoprocScope scope{ uploadInfo };
    LLUploadTracker& tracker = LLUploadTracker::instance();
    const U32 batch = uploadInfo->getBatchId();

    if (batch && uploadInfo->countsAsBatchItem() && tracker.isCancelled(batch))
    {
        // Cancelled before it was sent: nothing reached the server.
        uploadInfo->discardPrepared();
        uploadInfo->onCancelled();
        tracker.cancelled(batch);
        return;
    }

    LLCore::HttpRequest::ptr_t httpRequest(new LLCore::HttpRequest);
    LLCore::HttpOptions::ptr_t httpOptions(new LLCore::HttpOptions);
    httpOptions->setTimeout(LL_ASSET_UPLOAD_TIMEOUT_SEC);

    // The decode / encode already ran on the upload pool (LLUploadQueue); this is the rest.
    LLTimer main_timer;
    LLSD result = uploadInfo->prepareUpload();
    const F64 prepare_ms = main_timer.getElapsedTimeF64() * 1000.0;
    uploadInfo->logPreparedUpload();
    LL_INFOS("UploadQueue") << "Upload '" << uploadInfo->getDisplayName() << "': main-thread prepare "
                            << llformat("%.2f", prepare_ms) << " ms" << LL_ENDL;

    if (result.has("error"))
    {
        HandleUploadError(LLCore::HttpStatus(499), result, uploadInfo);
        return;
    }

    // An L$ check right before each charged upload: nothing is sent that cannot be paid for.
    const S32 cost = uploadInfo->getExpectedUploadCost();
    if (cost > 0 && !can_afford_transaction(cost))
    {
        LLStringUtil::format_map_t args;
        args["[COST]"] = llformat("%d", cost);
        LLSD error;
        error["error"] = LLSD::Boolean(true);
        error["label"] = "ErrorMessage";
        error["message"] = LLTrans::getString("UploadNotEnoughMoney", args);
        uploadInfo->discardPrepared();
        HandleUploadError(LLCore::HttpStatus(499), error, uploadInfo);
        return;
    }

    llcoro::suspend();

    tracker.sent(batch, uploadInfo->getDisplayName());
    scope.mSent = true;

    LLSD body = uploadInfo->generatePostBody();

    result = httpAdapter->postAndSuspend(httpRequest, url, body, httpOptions);

    LLSD httpResults = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(httpResults);

    if ((!status) || (result.has("error")))
    {
        // Only the request for an upload slot: nothing was created or charged.
        HandleUploadError(status, result, uploadInfo);
        return;
    }

    std::string uploader = result["uploader"].asString();

    bool success = false;
    if (!uploader.empty() && uploadInfo->getAssetId().notNull())
    {
        result = httpAdapter->postFileAndSuspend(httpRequest, uploader, uploadInfo->getAssetId(), uploadInfo->getAssetType(), httpOptions);
        httpResults = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
        status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(httpResults);

        std::string ulstate = result["state"].asString();

        if ((!status) || (ulstate != "complete"))
        {
            // No answer to the file itself: it may or may not have been created and charged.
            // An answer that is not "complete" is the server refusing it: nothing was charged.
            HandleUploadError(status, result, uploadInfo, !status);
            return;
        }
        if (!result.has("success"))
        {
            result["success"] = LLSD::Boolean((ulstate == "complete") && status);
        }

        S32 uploadPrice = result["upload_price"].asInteger();

        if (uploadPrice > 0)
        {
            // this upload costed us L$, update our balance
            LLStatusBar::sendMoneyBalanceRequest();

            if (batch)
            {
                // the bar and the batch's toast say what it cost
                tracker.charged(batch, uploadPrice);
            }
            else
            {
                LLSD args;
                args["AMOUNT"] = llformat("%d", uploadPrice);
                LLNotificationsUtil::add("UploadPayment", args);
            }
        }
    }
    else
    {
        LL_WARNS() << "No upload url provided.  Nothing uploaded, responding with previous result." << LL_ENDL;
    }
    LLUUID serverInventoryItem = uploadInfo->finishUpload(result);
    success = serverInventoryItem.notNull();

    if (batch && uploadInfo->countsAsBatchItem())
    {
        // The toast links to the new item; inventory is not opened on its own (no focus change).
        const LLUUID link = uploadInfo->getItemId().notNull() ? uploadInfo->getItemId() : serverInventoryItem;
        tracker.succeeded(batch, link);
    }
    if (!success && uploadInfo->showInventoryPanel())
    {
        LL_WARNS() << "Can't find a folder to put it in" << LL_ENDL;
    }

    // Let the Snapshot floater know we have finished uploading a snapshot to inventory
    LLFloater* floater_snapshot = LLFloaterReg::findInstance("snapshot");
    if (uploadInfo->getAssetType() == LLAssetType::AT_TEXTURE && floater_snapshot && floater_snapshot->isShown())
    {
        floater_snapshot->notify(LLSD().with("set-finished", LLSD().with("ok", success).with("msg", "inventory")));
    }
}

//=========================================================================
/*static*/
void LLViewerAssetUpload::HandleUploadError(LLCore::HttpStatus status, LLSD &result, LLResourceUploadInfo::ptr_t &uploadInfo,
                                            bool charge_unknown)
{
    std::string reason;
    std::string label("CannotUploadReason");

    LL_WARNS() << ll_pretty_print_sd(result) << LL_ENDL;

    if (result.has("label"))
    {
        label = result["label"].asString();
    }

    LLFloaterSnapshot* floater_snapshot = LLFloaterSnapshot::findInstance();
    if (result.has("message"))
    {
        reason = result["message"].asString();
    }
    else
    {
        switch (status.getType())
        {
        case 404:
            reason = LLTrans::getString("AssetUploadServerUnreacheble");
            if (floater_snapshot
                && floater_snapshot->isWaitingState()
                && uploadInfo->getAssetType() == LLAssetType::AT_IMAGE_JPEG)
            {
                label = "CannotUploadSnapshotEmailTooBig";
            }
            break;
        case 499:
            reason = LLTrans::getString("AssetUploadServerDifficulties");
            break;
        case 503:
            reason = LLTrans::getString("AssetUploadServerUnavaliable");
            break;
        default:
            reason = LLTrans::getString("AssetUploadRequestInvalid");
        }
    }
    LLStringUtil::trim(reason);
    result["charge_unknown"] = LLSD::Boolean(charge_unknown);

    const U32 batch = uploadInfo->getBatchId();
    if (!batch)
    {
        // Saves of existing items (scripts, notecards, postcards) keep their alerts.
        LLSD args;
        if(label == "ErrorMessage")
        {
            args["ERROR_MESSAGE"] = reason;
        }
        else
        {
            args["FILE"] = uploadInfo->getDisplayName();
            args["REASON"] = reason;
            args["ERROR"] = reason;
        }

        LLNotificationsUtil::add(label, args);
    }
    else if (uploadInfo->countsAsBatchItem())
    {
        // #1443: the bar turns red and the batch's toast says why (never a blocking alert).
        LLUploadTracker::Failure failure;
        failure.mName = uploadInfo->getDisplayName();
        failure.mReason = reason;
        failure.mChargeUnknown = charge_unknown;
        if (!charge_unknown)
        {
            // Refused before anything was charged: the toast may offer it again (the user's call).
            if (LLResourceUploadInfo::ptr_t proto = uploadInfo->makeRetry())
            {
                failure.mRetry = [proto]()
                {
                    if (LLResourceUploadInfo::ptr_t fresh = proto->makeRetry())
                    {
                        upload_new_resource(fresh);
                    }
                };
            }
        }
        LLUploadTracker::instance().failed(batch, failure);
    }

    if (uploadInfo->failedUpload(result, reason))
    {
        // no further action required, already handled by a callback
        // ex: do not trigger snapshot floater when failing material texture
        return;
    }

    // Todo: move these floater specific actions into proper callbacks

    // Let the Snapshot floater know we have failed uploading.
    if (floater_snapshot && floater_snapshot->isWaitingState())
    {
        if (uploadInfo->getAssetType() == LLAssetType::AT_IMAGE_JPEG)
        {
            floater_snapshot->notify(LLSD().with("set-finished", LLSD().with("ok", false).with("msg", "postcard")));
        }
        if (uploadInfo->getAssetType() == LLAssetType::AT_TEXTURE)
        {
            floater_snapshot->notify(LLSD().with("set-finished", LLSD().with("ok", false).with("msg", "inventory")));
        }
    }
}

