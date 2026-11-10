/**
 * @file llj2cuploadoptions.h
 * @brief The JPEG 2000 settings an upload encode uses, read once on the main thread (#1443).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef LL_LLJ2CUPLOADOPTIONS_H
#define LL_LLJ2CUPLOADOPTIONS_H

#include "stdtypes.h"

// Upload encodes run on worker threads, which never read settings: the caller takes these on
// the main thread (fromSettings(), in llviewertexturelist.cpp) and hands them over.
struct LLJ2CUploadOptions
{
    bool mLosslessSmall = false;    // LosslessJ2CUpload: small images are encoded losslessly
    bool mAdvanced = false;         // Jpeg2000AdvancedCompression
    S32  mBlockSize = 0;            // Jpeg2000BlocksSize
    S32  mPrecinctSize = 0;         // Jpeg2000PrecinctsSize

    static LLJ2CUploadOptions fromSettings();   // main thread only
};

#endif // LL_LLJ2CUPLOADOPTIONS_H
