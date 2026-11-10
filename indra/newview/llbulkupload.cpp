/**
 * @file llbulkupload.cpp
 * @brief Bulk upload without stalls (#1443).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llbulkupload.h"

#include "llagent.h"
#include "llagentbenefits.h"
#include "llfilesystem.h"
#include "llfloaterperms.h"
#include "llgltfmaterial.h"
#include "llimage.h"
#include "llimagej2c.h"
#include "llinventorymodel.h"
#include "llj2cuploadoptions.h"
#include "llmaterialeditor.h"
#include "llpermissions.h"
#include "llsdserialize.h"
#include "lltimer.h"
#include "lltinygltfhelper.h"
#include "lltrans.h"
#include "lluploadqueue.h"
#include "llviewerassetupload.h"
#include "llviewercontrol.h"
#include "llviewermenufile.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"

namespace
{
    const S32 MAX_DIM = LLViewerFetchedTexture::MAX_IMAGE_SIZE_DEFAULT;

    bool is_gltf(const std::string& ext)
    {
        return ext == "gltf" || ext == "glb";
    }

    // The size a material texture uploads at (convertToUploadFile: each side to a power of two).
    void material_size(const tinygltf::Image* image, S32& w, S32& h)
    {
        w = h = 0;
        // as LLTinyGLTFHelper::getTexture: 8 bits, at most 4 channels
        if (image && image->bits == 8 && image->component <= 4 && image->width > 0 && image->height > 0)
        {
            w = LLImageRaw::biasedDimToPowerOfTwo(image->width, MAX_DIM);
            h = LLImageRaw::biasedDimToPowerOfTwo(image->height, MAX_DIM);
        }
    }

    void texture_limit(bool allow_2k, S32& max_w, S32& max_h)
    {
        max_w = allow_2k ? gSavedSettings.getS32("max_texture_dimension_X") : 1024;
        max_h = allow_2k ? gSavedSettings.getS32("max_texture_dimension_Y") : 1024;
    }

    //-------------------------------------------------------------------------
    // A material's texture, already encoded into the asset cache by the worker: only sent.
    class LLMaterialTextureUploadInfo : public LLResourceUploadInfo
    {
    public:
        typedef std::function<void(const LLUUID& new_asset)> done_f;
        typedef std::function<void(const std::string& reason, bool charge_unknown)> failed_f;

        LLMaterialTextureUploadInfo(const LLTransactionID& tid, const std::string& name, S32 cost, const LLUUID& folder,
                                    done_f done, failed_f failed)
        :   LLResourceUploadInfo(tid, LLAssetType::AT_TEXTURE, name, name, 0, LLFolderType::FT_TEXTURE, LLInventoryType::IT_TEXTURE,
                                 LLFloaterPerms::getNextOwnerPerms("Uploads"), LLFloaterPerms::getGroupPerms("Uploads"),
                                 LLFloaterPerms::getEveryonePerms("Uploads"), cost, folder, false),
            mDone(done),
            mFailed(failed)
        {
        }

        LLUUID finishUpload(LLSD& result) override
        {
            LLUUID item = LLResourceUploadInfo::finishUpload(result);
            done_f done;
            done.swap(mDone);
            mFailed = nullptr;
            if (done)
            {
                done(result["new_asset"].asUUID());
            }
            return item;
        }

        bool failedUpload(LLSD& result, std::string& reason) override
        {
            failed_f failed;
            failed.swap(mFailed);
            mDone = nullptr;
            if (failed)
            {
                failed(reason, result["charge_unknown"].asBoolean());
            }
            return true; // the material reports it
        }

    private:
        done_f   mDone;
        failed_f mFailed;
    };

    //-------------------------------------------------------------------------
    // The materials of one glTF file. One pipeline job: the worker loads the file once and
    // encodes every texture into the asset cache; the main thread then sends one material at a
    // time (its textures, then the material item) and frees the slot when all have finished.
    class MaterialFileJob : public std::enable_shared_from_this<MaterialFileJob>
    {
    public:
        struct Tex
        {
            bool            mHas = false;
            std::string     mName;
            LLTransactionID mTid;
            LLAssetID       mAsset;
            S32             mW = 0;
            S32             mH = 0;
            LLUUID          mNewAsset;
        };
        struct Mat
        {
            S32         mIndex = 0;
            S32         mQuoted = 0;
            std::string mName;
            std::string mDesc;
            std::string mError;
            Tex         mTex[LLBulkUpload::MATERIAL_SLOTS];
            LLColor4    mBaseColor;
            LLColor3    mEmissive;
            F32         mMetallic = 1.f;
            F32         mRoughness = 1.f;
            std::string mAlphaMode;
            F32         mAlphaCutoff = 0.5f;
            // main thread, while it uploads
            S32         mPending = 0;
            S32         mUploaded = 0;
            bool        mFailed = false;
            bool        mChargeUnknown = false;
            std::string mFailReason;
        };

        static void submit(const std::string& file, const std::vector<LLBulkUpload::MaterialScan>& materials,
                           const std::vector<S32>& quotes, const LLUUID& dest, U32 batch)
        {
            std::shared_ptr<MaterialFileJob> job(new MaterialFileJob());
            job->mFile = file;
            job->mDest = dest;
            job->mBatch = batch;
            job->mMats.resize(materials.size());
            for (size_t i = 0; i < materials.size(); ++i)
            {
                job->mMats[i].mIndex = materials[i].mIndex;
                job->mMats[i].mName = materials[i].mName;
                job->mMats[i].mQuoted = i < quotes.size() ? quotes[i] : 0;
            }

            LLUploadQueue::Job q;
            q.mName = gDirUtilp->getBaseFileName(file, false);
            q.mBatch = batch;
            q.mBefore = [job]() { job->before(); };
            q.mWork = [job]() { job->work(); };
            q.mAfter = [job](std::function<void()> release) { job->after(release); };
            q.mCancel = [job](bool prepared) { job->cancel(prepared); };
            LLUploadQueue::instance().submit(std::move(q));
        }

    private:
        MaterialFileJob() = default;

        // main thread: what the worker must not look up
        void before()
        {
            mOpts = LLJ2CUploadOptions::fromSettings();
            mDescHeader = LLTrans::getString("Material Texture Name Header");
            const LLUUID session = gAgent.getSecureSessionID();
            for (Mat& m : mMats)
            {
                for (Tex& t : m.mTex)
                {
                    t.mTid.generate();
                    t.mAsset = t.mTid.makeAssetID(session);
                }
            }
        }

        // worker thread: decode, pack and encode; files into the asset cache
        void work()
        {
            LLTimer timer;
            try
            {
                encode();
            }
            catch (const std::bad_alloc&)
            {
                mLoadError = "not enough memory to read it";
            }
            catch (...)
            {
                LOG_UNHANDLED_EXCEPTION("bulk material encode");
                mLoadError = "it could not be read";
            }
            mWorkerMs = timer.getElapsedTimeF64() * 1000.0;
        }

        void encode()
        {
            tinygltf::Model model;
            if (!LLTinyGLTFHelper::loadModel(mFile, model))
            {
                mLoadError = "it could not be read as a glTF material file";
                return;
            }
            const std::string folder = gDirUtilp->getDirName(mFile);
            for (Mat& m : mMats)
            {
                if (m.mIndex < 0 || m.mIndex >= (S32)model.materials.size())
                {
                    m.mError = "the material is no longer in the file";
                    continue;
                }
                const tinygltf::Material& in = model.materials[m.mIndex];
                LLMaterialEditor::ImportNames names;
                LLMaterialEditor::getImportNames(mFile, model, m.mIndex, names);
                m.mName = names.mMaterial;

                // the material editor's import: the same images, packing and encoding
                std::string ignored;
                LLPointer<LLImageRaw> img[LLBulkUpload::MATERIAL_SLOTS];
                LLPointer<LLImageRaw> occlusion;
                img[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR] =
                    LLTinyGLTFHelper::getTexture(folder, model, in.pbrMetallicRoughness.baseColorTexture.index, ignored);
                img[LLGLTFMaterial::GLTF_TEXTURE_INFO_NORMAL] =
                    LLTinyGLTFHelper::getTexture(folder, model, in.normalTexture.index, ignored);
                img[LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS] =
                    LLTinyGLTFHelper::getTexture(folder, model, in.pbrMetallicRoughness.metallicRoughnessTexture.index, ignored);
                img[LLGLTFMaterial::GLTF_TEXTURE_INFO_EMISSIVE] =
                    LLTinyGLTFHelper::getTexture(folder, model, in.emissiveTexture.index, ignored);
                if (in.occlusionTexture.index != in.pbrMetallicRoughness.metallicRoughnessTexture.index)
                {
                    occlusion = LLTinyGLTFHelper::getTexture(folder, model, in.occlusionTexture.index, ignored);
                }
                LLTinyGLTFHelper::packImages(in, img[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR],
                                             img[LLGLTFMaterial::GLTF_TEXTURE_INFO_NORMAL],
                                             img[LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS],
                                             img[LLGLTFMaterial::GLTF_TEXTURE_INFO_EMISSIVE], occlusion);
                occlusion = nullptr;

                const std::string tex_names[LLBulkUpload::MATERIAL_SLOTS] =
                    { names.mBaseColor, names.mNormal, names.mMetallicRoughness, names.mEmissive };
                bool has[LLBulkUpload::MATERIAL_SLOTS] = {};
                for (S32 k = 0; k < LLBulkUpload::MATERIAL_SLOTS && m.mError.empty(); ++k)
                {
                    if (img[k].isNull())
                    {
                        continue;
                    }
                    has[k] = true;
                    // a lossless normal map, as the material editor uploads it
                    std::string encode_error;
                    LLPointer<LLImageJ2C> j2c = LLViewerTextureList::convertToUploadFile(
                        img[k], MAX_DIM, false, k == LLGLTFMaterial::GLTF_TEXTURE_INFO_NORMAL, mOpts, &encode_error);
                    img[k] = nullptr;
                    if (j2c.isNull() || j2c->getDataSize() <= 0)
                    {
                        // #1465: with the encoder's reason (nothing of this material is sent)
                        m.mError = tex_names[k] + " could not be encoded" + (encode_error.empty() ? std::string() : ": " + encode_error);
                        break;
                    }
                    LLFileSystem file(m.mTex[k].mAsset, LLAssetType::AT_TEXTURE, LLFileSystem::WRITE);
                    if (!file.write(j2c->getData(), j2c->getDataSize()))
                    {
                        m.mError = tex_names[k] + " could not be written to the cache";
                        break;
                    }
                    Tex& t = m.mTex[k];
                    t.mHas = true;
                    t.mW = j2c->getWidth();
                    t.mH = j2c->getHeight();
                    // for ease of inventory management, the material's name comes first
                    t.mName = m.mName + ": " + tex_names[k];
                }

                m.mDesc = LLMaterialEditor::importDescription(mDescHeader, names,
                                                              has[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR],
                                                              has[LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS],
                                                              has[LLGLTFMaterial::GLTF_TEXTURE_INFO_EMISSIVE],
                                                              has[LLGLTFMaterial::GLTF_TEXTURE_INFO_NORMAL]);
                // the editor's fields (LLMaterialEditor::getGLTFMaterial)
                m.mBaseColor = LLTinyGLTFHelper::getColor(in.pbrMetallicRoughness.baseColorFactor);
                const LLColor4 emissive = LLTinyGLTFHelper::getColor(in.emissiveFactor);
                m.mEmissive.setVec(emissive.mV[VRED], emissive.mV[VGREEN], emissive.mV[VBLUE]);
                m.mMetallic = llclamp((F32)in.pbrMetallicRoughness.metallicFactor, 0.f, 1.f);
                m.mRoughness = llclamp((F32)in.pbrMetallicRoughness.roughnessFactor, 0.f, 1.f);
                m.mAlphaMode = in.alphaMode;
                m.mAlphaCutoff = llclamp((F32)in.alphaCutoff, 0.f, 1.f);
            }
        }

        // main thread, after the worker step
        void after(std::function<void()> release)
        {
            mRelease = release;
            S32 textures = 0;
            for (const Mat& m : mMats)
            {
                for (const Tex& t : m.mTex)
                {
                    textures += t.mHas ? 1 : 0;
                }
            }
            LL_INFOS("UploadQueue") << "Bulk upload: " << mFile << ": " << mMats.size() << " material(s), " << textures
                                    << " texture(s) encoded on the upload pool in " << llformat("%.1f", mWorkerMs) << " ms" << LL_ENDL;
            if (!mLoadError.empty())
            {
                for (Mat& m : mMats)
                {
                    removeCached(m);
                    failMaterial(m, mLoadError, false);
                }
                mNext = mMats.size();
            }
            startNext();
        }

        void cancel(bool prepared)
        {
            for (Mat& m : mMats)
            {
                if (prepared)
                {
                    removeCached(m);
                }
            }
            LLUploadTracker::instance().cancelled(mBatch, (S32)mMats.size());
        }

        void startNext()
        {
            LLUploadTracker& tracker = LLUploadTracker::instance();
            while (mNext < mMats.size())
            {
                const size_t mi = mNext++;
                Mat& m = mMats[mi];
                if (!m.mError.empty())
                {
                    removeCached(m);
                    failMaterial(m, m.mError, false);
                    continue;
                }
                if (tracker.isCancelled(mBatch))
                {
                    removeCached(m);
                    tracker.cancelled(mBatch);
                    continue;
                }
                const std::string url = gAgent.getRegionCapability("NewFileAgentInventory");
                if (url.empty() || !LLMaterialEditor::capabilitiesAvailable())
                {
                    removeCached(m);
                    failMaterial(m, "this region does not support material uploads", false);
                    continue;
                }
                // The quote came from the images' headers; never send more than was confirmed.
                S32 cost = 0;
                for (const Tex& t : m.mTex)
                {
                    cost += t.mHas ? LLAgentBenefitsMgr::current().getTextureUploadCost(t.mW, t.mH) : 0;
                }
                if (cost > m.mQuoted)
                {
                    removeCached(m);
                    failMaterial(m, llformat("its textures cost L$%d, more than the L$%d confirmed; not uploaded", cost, m.mQuoted), false);
                    continue;
                }
                startMaterial(mi, url);
                return; // the next one starts when this one's textures have finished
            }
            if (mOpen == 0 && mRelease)
            {
                std::function<void()> release;
                release.swap(mRelease);
                release();
            }
        }

        void startMaterial(size_t mi, const std::string& url)
        {
            Mat& m = mMats[mi];
            ++mOpen;
            m.mPending = 0;
            for (const Tex& t : m.mTex)
            {
                m.mPending += t.mHas ? 1 : 0;
            }
            if (m.mPending == 0)
            {
                createMaterial(mi);
                startNext();
                return;
            }
            auto self = shared_from_this();
            for (S32 k = 0; k < LLBulkUpload::MATERIAL_SLOTS; ++k)
            {
                const Tex& t = m.mTex[k];
                if (!t.mHas)
                {
                    continue;
                }
                LLResourceUploadInfo::ptr_t info = std::make_shared<LLMaterialTextureUploadInfo>(
                    t.mTid, t.mName, LLAgentBenefitsMgr::current().getTextureUploadCost(t.mW, t.mH), mDest,
                    [self, mi, k](const LLUUID& new_asset) { self->onTexture(mi, k, true, new_asset, std::string(), false); },
                    [self, mi, k](const std::string& reason, bool unknown) { self->onTexture(mi, k, false, LLUUID::null, reason, unknown); });
                // a step of the material: the material is the batch's item
                info->setBatchId(mBatch);
                info->setCountsAsBatchItem(false);
                LLViewerAssetUpload::EnqueueInventoryUpload(url, info);
            }
        }

        void onTexture(size_t mi, S32 slot, bool ok, const LLUUID& new_asset, const std::string& reason, bool charge_unknown)
        {
            Mat& m = mMats[mi];
            --m.mPending;
            if (ok && new_asset.notNull())
            {
                m.mTex[slot].mNewAsset = new_asset;
                ++m.mUploaded;
            }
            else
            {
                m.mFailed = true;
                m.mChargeUnknown |= charge_unknown;
                if (m.mFailReason.empty())
                {
                    m.mFailReason = m.mTex[slot].mName + ": " + (reason.empty() ? std::string("the server returned no asset") : reason);
                }
            }
            if (m.mPending > 0)
            {
                return;
            }
            if (m.mFailed)
            {
                std::string why = m.mFailReason;
                if (m.mUploaded > 0)
                {
                    why += llformat(" (%d of its textures were uploaded; the material was not created)", m.mUploaded);
                }
                failMaterial(m, why, m.mChargeUnknown);
                materialFinished();
            }
            else
            {
                createMaterial(mi);
            }
            startNext();
        }

        void createMaterial(size_t mi)
        {
            Mat& m = mMats[mi];
            LLGLTFMaterial mat;
            mat.mBaseColor = m.mBaseColor;
            mat.mEmissiveColor = m.mEmissive;
            mat.mMetallicFactor = m.mMetallic;
            mat.mRoughnessFactor = m.mRoughness;
            mat.setAlphaMode(m.mAlphaMode);
            mat.mAlphaCutoff = m.mAlphaCutoff;
            // SL-19392: imports default to single sided (as the material editor's)
            mat.mDoubleSided = false;
            for (S32 k = 0; k < LLBulkUpload::MATERIAL_SLOTS; ++k)
            {
                mat.mTextureId[k] = m.mTex[k].mHas ? m.mTex[k].mNewAsset : LLUUID::null;
            }

            LLSD asset;
            asset["version"] = LLGLTFMaterial::ASSET_VERSION;
            asset["type"] = LLGLTFMaterial::ASSET_TYPE;
            asset["data"] = mat.asJSON();
            std::ostringstream str;
            LLSDSerialize::serialize(asset, str, LLSDSerialize::LLSD_BINARY);

            // Uploads use the Upload permissions, as the material editor's import does.
            LLPermissions perms;
            perms.init(gAgent.getID(), gAgent.getID(), LLUUID::null, LLUUID::null);
            perms.initMasks(PERM_ALL, PERM_ALL, LLFloaterPerms::getEveryonePerms("Uploads"),
                            LLFloaterPerms::getGroupPerms("Uploads"), LLFloaterPerms::getNextOwnerPerms("Uploads"));

            auto self = shared_from_this();
            LLMaterialEditor::createInventoryItem(str.str(), m.mName, m.mDesc, perms, mDest, mBatch,
                                                  [self](bool) { self->materialFinished(); });
        }

        void materialFinished()
        {
            mOpen = llmax(mOpen - 1, 0);
            if (mNext >= mMats.size() && mOpen == 0 && mRelease)
            {
                std::function<void()> release;
                release.swap(mRelease);
                release();
            }
        }

        void failMaterial(const Mat& m, const std::string& reason, bool charge_unknown)
        {
            LLUploadTracker::Failure failure;
            failure.mName = m.mName.empty() ? gDirUtilp->getBaseFileName(mFile, false) : m.mName;
            failure.mReason = reason;
            failure.mChargeUnknown = charge_unknown;
            if (!charge_unknown && m.mUploaded == 0)
            {
                // Nothing of it was charged: it may be offered again (as a new upload).
                LLBulkUpload::MaterialScan scan;
                scan.mIndex = m.mIndex;
                scan.mName = failure.mName;
                const std::string file = mFile;
                const LLUUID dest = mDest;
                const S32 quoted = m.mQuoted;
                failure.mRetry = [file, scan, dest, quoted]()
                {
                    const U32 batch = LLUploadTracker::instance().begin(scan.mName, 1, quoted, true);
                    MaterialFileJob::submit(file, { scan }, { quoted }, dest, batch);
                };
            }
            LLUploadTracker::instance().failed(mBatch, failure);
        }

        void removeCached(const Mat& m)
        {
            for (const Tex& t : m.mTex)
            {
                if (t.mAsset.notNull())
                {
                    LLFileSystem::removeFile(t.mAsset, LLAssetType::AT_TEXTURE, ENOENT);
                }
            }
        }

        std::string         mFile;
        LLUUID              mDest;
        U32                 mBatch = 0;
        std::vector<Mat>    mMats;
        // set on the main thread before the worker step
        LLJ2CUploadOptions  mOpts;
        std::string         mDescHeader;
        // set by the worker
        std::string         mLoadError;
        F64                 mWorkerMs = 0.0;
        // main thread, after the worker step
        std::function<void()> mRelease;
        size_t              mNext = 0;
        S32                 mOpen = 0;
    };
}

//=============================================================================

bool LLBulkUpload::classify(FileScan& scan)
{
    std::string name = gDirUtilp->getBaseFileName(scan.mFile, true);
    LLStringUtil::replaceNonstandardASCII(name, '?');
    LLStringUtil::replaceChar(name, '|', '?');
    LLStringUtil::stripNonprintable(name);
    LLStringUtil::trim(name);
    scan.mName = name;

    const std::string ext = gDirUtilp->getExtension(scan.mFile);
    if (is_gltf(ext))
    {
        scan.mKind = KIND_PENDING;
        return true;
    }
    LLAssetType::EType type = LLAssetType::AT_NONE;
    U32 codec = 0;
    if (!LLResourceUploadInfo::findAssetTypeAndCodecOfExtension(ext, type, codec))
    {
        scan.mKind = KIND_UNSUPPORTED;
        return false;
    }
    switch (type)
    {
    case LLAssetType::AT_TEXTURE:
        scan.mKind = KIND_PENDING;
        return true; // its size sets the price
    case LLAssetType::AT_SOUND:
        scan.mKind = KIND_SOUND;
        return false;
    case LLAssetType::AT_ANIMATION:
        scan.mKind = KIND_ANIMATION;
        return false;
    default:
        scan.mKind = KIND_UNSUPPORTED;
        return false;
    }
}

void LLBulkUpload::scanOnWorker(FileScan& scan)
{
    LLTimer timer;
    try
    {
        const std::string ext = gDirUtilp->getExtension(scan.mFile);
        if (is_gltf(ext))
        {
            tinygltf::Model model;
            if (!LLTinyGLTFHelper::loadModelImageSizes(scan.mFile, model))
            {
                scan.mKind = KIND_ERROR;
                scan.mError = "not a readable glTF file with materials";
            }
            else
            {
                scan.mKind = KIND_MATERIAL;
                for (S32 i = 0; i < (S32)model.materials.size(); ++i)
                {
                    const tinygltf::Material& in = model.materials[i];
                    MaterialScan m;
                    m.mIndex = i;
                    LLMaterialEditor::ImportNames names;
                    LLMaterialEditor::getImportNames(scan.mFile, model, i, names);
                    m.mName = names.mMaterial;
                    using namespace LLTinyGLTFHelper;
                    material_size(getImageFromTextureIndex(model, in.pbrMetallicRoughness.baseColorTexture.index),
                                  m.mW[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR], m.mH[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR]);
                    material_size(getImageFromTextureIndex(model, in.normalTexture.index),
                                  m.mW[LLGLTFMaterial::GLTF_TEXTURE_INFO_NORMAL], m.mH[LLGLTFMaterial::GLTF_TEXTURE_INFO_NORMAL]);
                    S32& mr_w = m.mW[LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS];
                    S32& mr_h = m.mH[LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS];
                    material_size(getImageFromTextureIndex(model, in.pbrMetallicRoughness.metallicRoughnessTexture.index), mr_w, mr_h);
                    if (!mr_w && in.occlusionTexture.index != in.pbrMetallicRoughness.metallicRoughnessTexture.index)
                    {
                        // occlusion alone becomes the ORM texture (LLTinyGLTFHelper::packImages)
                        material_size(getImageFromTextureIndex(model, in.occlusionTexture.index), mr_w, mr_h);
                    }
                    material_size(getImageFromTextureIndex(model, in.emissiveTexture.index),
                                  m.mW[LLGLTFMaterial::GLTF_TEXTURE_INFO_EMISSIVE], m.mH[LLGLTFMaterial::GLTF_TEXTURE_INFO_EMISSIVE]);
                    scan.mMaterials.push_back(m);
                }
            }
        }
        else
        {
            // An image: its header (the file is read, not decoded).
            const U32 codec = LLImageBase::getCodecFromExtension(ext);
            LLPointer<LLImageFormatted> image = LLImageFormatted::createFromType((S8)codec);
            if (image.isNull() || !image->load(scan.mFile) || image->getWidth() <= 0 || image->getHeight() <= 0)
            {
                scan.mKind = KIND_ERROR;
                scan.mError = !LLImage::getLastThreadError().empty() ? LLImage::getLastThreadError()
                                                                     : std::string("not a readable image");
            }
            else
            {
                scan.mKind = KIND_TEXTURE;
                scan.mWidth = image->getWidth();
                scan.mHeight = image->getHeight();
            }
        }
    }
    catch (const std::bad_alloc&)
    {
        scan.mKind = KIND_ERROR;
        scan.mError = "not enough memory to read it";
    }
    catch (...)
    {
        LOG_UNHANDLED_EXCEPTION("bulk upload scan");
        scan.mKind = KIND_ERROR;
        scan.mError = "it could not be read";
    }
    scan.mWorkerMs = timer.getElapsedTimeF64() * 1000.0;
}

S32 LLBulkUpload::materialCost(const MaterialScan& material)
{
    S32 cost = 0;
    for (S32 k = 0; k < MATERIAL_SLOTS; ++k)
    {
        if (material.mW[k] > 0 && material.mH[k] > 0)
        {
            cost += LLAgentBenefitsMgr::current().getTextureUploadCost(material.mW[k], material.mH[k]);
        }
    }
    return cost;
}

LLBulkUpload::Quote LLBulkUpload::quote(const FileScan& scan, bool allow_2k)
{
    Quote q;
    S32 cost = 0;
    switch (scan.mKind)
    {
    case KIND_TEXTURE:
    {
        S32 max_w = 0, max_h = 0, w = 0, h = 0;
        texture_limit(allow_2k, max_w, max_h);
        LLNewFileResourceUploadInfo::bulkImageSize(scan.mWidth, scan.mHeight, max_w, max_h, w, h);
        q.mCost = LLAgentBenefitsMgr::current().getTextureUploadCost(w, h);
        q.mItems = 1;
        q.mHas2K = w * h >= LLAgentBenefits::MIN_2K_TEXTURE_AREA;
        break;
    }
    case KIND_SOUND:
    case KIND_ANIMATION:
    {
        LLAssetType::EType type = scan.mKind == KIND_SOUND ? LLAssetType::AT_SOUND : LLAssetType::AT_ANIMATION;
        if (LLAgentBenefitsMgr::current().findUploadCost(type, cost))
        {
            q.mCost = cost;
            q.mItems = 1;
        }
        break;
    }
    case KIND_MATERIAL:
        for (const MaterialScan& m : scan.mMaterials)
        {
            q.mCost += materialCost(m);
            ++q.mItems;
            for (S32 k = 0; k < MATERIAL_SLOTS; ++k)
            {
                q.mHas2K |= m.mW[k] * m.mH[k] >= LLAgentBenefits::MIN_2K_TEXTURE_AREA;
            }
        }
        break;
    default:
        break;
    }
    return q;
}

U32 LLBulkUpload::start(const std::vector<std::shared_ptr<FileScan>>& scans, bool allow_2k, const LLUUID& dest)
{
    LLTimer timer;
    S32 items = 0;
    S32 cost = 0;
    for (const std::shared_ptr<FileScan>& scan : scans)
    {
        const Quote q = quote(*scan, allow_2k);
        items += q.mItems;
        cost += q.mCost;
    }
    if (items <= 0)
    {
        return 0;
    }

    const U32 batch = LLUploadTracker::instance().begin(LLTrans::getString("BulkUploadBatchName"), items, cost, false);
    S32 max_w = 0, max_h = 0;
    texture_limit(allow_2k, max_w, max_h);
    const U32 next_owner = LLFloaterPerms::getNextOwnerPerms("Uploads");
    const U32 group = LLFloaterPerms::getGroupPerms("Uploads");
    const U32 everyone = LLFloaterPerms::getEveryonePerms("Uploads");

    for (const std::shared_ptr<FileScan>& scan : scans)
    {
        const Quote q = quote(*scan, allow_2k);
        if (q.mItems <= 0)
        {
            continue;
        }
        if (scan->mKind == KIND_MATERIAL)
        {
            std::vector<S32> quotes;
            for (const MaterialScan& m : scan->mMaterials)
            {
                quotes.push_back(materialCost(m));
            }
            MaterialFileJob::submit(scan->mFile, scan->mMaterials, quotes, dest, batch);
            continue;
        }
        // Images, sounds and animations: a file upload each (images and sounds are encoded on
        // the upload pool when their turn comes).
        std::shared_ptr<LLNewFileResourceUploadInfo> info = std::make_shared<LLNewFileResourceUploadInfo>(
            scan->mFile, scan->mName, scan->mName, 0, LLFolderType::FT_NONE, LLInventoryType::IT_NONE,
            next_owner, group, everyone, q.mCost, dest);
        if (scan->mKind == KIND_TEXTURE)
        {
            info->setBulkImageLimit(max_w, max_h);
        }
        info->setBatchId(batch);
        LLResourceUploadInfo::ptr_t upload = info;
        upload_new_resource(upload);
    }

    LL_INFOS("UploadQueue") << "Bulk upload: confirmed " << items << " item(s) from " << scans.size() << " file(s), L$" << cost
                            << "; queued in " << llformat("%.2f", timer.getElapsedTimeF64() * 1000.0) << " ms (main thread)" << LL_ENDL;
    return batch;
}
