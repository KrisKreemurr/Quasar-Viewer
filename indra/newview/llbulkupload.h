/**
 * @file llbulkupload.h
 * @brief Bulk upload without stalls (#1443): files are read for the quote on the upload pool,
 *        and the confirmed uploads are encoded there, pipelined with the uploads.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef LL_LLBULKUPLOAD_H
#define LL_LLBULKUPLOAD_H

#include "lluuid.h"

#include <memory>
#include <string>
#include <vector>

namespace LLBulkUpload
{
    enum EKind
    {
        KIND_PENDING,       // not read yet
        KIND_TEXTURE,
        KIND_SOUND,
        KIND_ANIMATION,
        KIND_MATERIAL,      // a glTF file: one item per material
        KIND_UNSUPPORTED,
        KIND_ERROR          // could not be read
    };

    constexpr S32 MATERIAL_SLOTS = 4;   // LLGLTFMaterial::TextureInfo order: base, normal, ORM, emissive

    // One material of a glTF file: the size each of its textures uploads at (0: it has none).
    struct MaterialScan
    {
        S32         mIndex = 0;
        std::string mName;
        S32         mW[MATERIAL_SLOTS] = {};
        S32         mH[MATERIAL_SLOTS] = {};
    };

    // What the list knows about one picked file.
    struct FileScan
    {
        std::string mFile;
        std::string mName;          // the asset name (the sanitized base name)
        EKind       mKind = KIND_PENDING;
        std::string mError;
        S32         mWidth = 0;     // an image's size in the file
        S32         mHeight = 0;
        std::vector<MaterialScan> mMaterials;
        F64         mWorkerMs = 0.0;
    };

    // Main thread: the kind from the extension. true: the file must be read (scanOnWorker) for
    // the quote; false: the scan is complete already.
    bool classify(FileScan& scan);
    // Worker thread: reads an image's header, or a glTF file without decoding its images.
    void scanOnWorker(FileScan& scan);

    // Main thread: the L$ and the items one scanned file adds to the batch.
    struct Quote
    {
        S32  mCost = 0;
        S32  mItems = 0;
        bool mHas2K = false;    // has an image that uploads at more than 1024 x 1024
    };
    Quote quote(const FileScan& scan, bool allow_2k);
    S32   materialCost(const MaterialScan& material);

    // Main thread: the confirmed uploads, as one batch (one progress bar). They wait on the
    // upload pipeline (LLUploadQueue): nothing is read or encoded here. 0: nothing to upload.
    U32 start(const std::vector<std::shared_ptr<FileScan>>& scans, bool allow_2k, const LLUUID& dest);
}

#endif // LL_LLBULKUPLOAD_H
