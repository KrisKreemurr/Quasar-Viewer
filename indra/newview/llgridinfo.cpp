/**
 * @file llgridinfo.cpp
 * @brief Fetches an OpenSim grid's get_grid_info answer and adds the grid to LLGridManager.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
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

#include "llgridinfo.h"

#include "llcorehttputil.h"
#include "llcoros.h"
#include "llhttpconstants.h"
#include "lltrans.h"
#include "llviewernetwork.h"
#include "llxmlnode.h"

namespace
{
    // The answer is a few hundred bytes; anything big is not grid info
    const size_t MAX_GRID_INFO_BYTES = 256 * 1024;
    const unsigned int GRID_INFO_TIMEOUT_SECS = 15;

    std::string trimmed(std::string text)
    {
        LLStringUtil::trim(text);
        return text;
    }

    // What the user typed -> candidate base URIs without trailing slash or /get_grid_info
    std::vector<std::string> candidate_bases(const std::string& typed)
    {
        std::string text = trimmed(typed);
        const std::string suffix = "get_grid_info";
        if (text.length() >= suffix.length()
            && text.compare(text.length() - suffix.length(), suffix.length(), suffix) == 0)
        {
            text.resize(text.length() - suffix.length());
        }
        while (!text.empty() && text.back() == '/')
        {
            text.pop_back();
        }

        std::vector<std::string> bases;
        if (text.empty())
        {
            return bases;
        }
        if (text.find("://") != std::string::npos)
        {
            bases.push_back(text);
        }
        else
        {
            bases.push_back("https://" + text);
            bases.push_back("http://" + text);
        }
        return bases;
    }

    void fetchCoro(std::string typed_uri, LLGridInfo::result_fn_t callback)
    {
        std::vector<std::string> bases = candidate_bases(typed_uri);
        if (bases.empty())
        {
            callback(false, std::string(), LLTrans::getString("GridFetchNoURI"));
            return;
        }

        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t
            httpAdapter(new LLCoreHttpUtil::HttpCoroutineAdapter("gridInfoCoro", LLCore::HttpRequest::DEFAULT_POLICY_ID));
        LLCore::HttpRequest::ptr_t httpRequest(new LLCore::HttpRequest);

        std::string last_error;
        for (const std::string& base : bases)
        {
            LLCore::HttpOptions::ptr_t httpOpts(new LLCore::HttpOptions);
            httpOpts->setTimeout(GRID_INFO_TIMEOUT_SECS);
            httpOpts->setRetries(0);
            httpOpts->setFollowRedirects(true);
            LLCore::HttpHeaders::ptr_t httpHeaders(new LLCore::HttpHeaders);
            httpHeaders->append(HTTP_OUT_HEADER_ACCEPT, HTTP_CONTENT_TEXT_XML);

            const std::string url = base + "/get_grid_info";
            LL_INFOS("GridManager") << "Fetching " << url << LL_ENDL;
            LLSD result = httpAdapter->getRawAndSuspend(httpRequest, url, httpOpts, httpHeaders);
            LLSD httpResults = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
            LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(httpResults);

            LLStringUtil::format_map_t args;
            args["[URI]"] = base;
            if (!status)
            {
                LL_WARNS("GridManager") << "get_grid_info failed for " << url << ": "
                                        << status.toString() << " [" << status.toTerseString() << "]" << LL_ENDL;
                args["[REASON]"] = status.toString();
                last_error = LLTrans::getString("GridFetchUnreachable", args);
                continue;
            }

            const LLSD::Binary& raw = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
            LLSD info;
            if (raw.size() > MAX_GRID_INFO_BYTES || !LLGridInfo::parseGridInfo(std::string(raw.begin(), raw.end()), info))
            {
                LL_WARNS("GridManager") << url << " did not answer with grid information" << LL_ENDL;
                last_error = LLTrans::getString("GridFetchInvalid", args);
                continue;
            }

            std::string grid_name, error;
            if (LLGridManager::getInstance()->addOpenSimGrid(info, base, grid_name, error))
            {
                callback(true, grid_name, std::string());
            }
            else
            {
                // a failed save still leaves the grid usable for this session
                callback(!grid_name.empty(), grid_name, error);
            }
            return;
        }
        callback(false, std::string(), last_error);
    }
}

bool LLGridInfo::parseGridInfo(const std::string& body, LLSD& info)
{
    info = LLSD::emptyMap();

    LLXMLNodePtr root;
    if (!LLXMLNode::parseBuffer(body.data(), body.size(), root) || root.isNull() || !root->hasName("gridinfo"))
    {
        return false;
    }
    for (LLXMLNodePtr child = root->getFirstChild(); child.notNull(); child = child->getNextSibling())
    {
        if (child->getName() && child->getName()->mString)
        {
            info[std::string(child->getName()->mString)] = trimmed(child->getValue());
        }
    }
    // a document with neither is not grid information
    return info.has("login") || info.has("gridname");
}

void LLGridInfo::addGridFromURI(const std::string& uri, result_fn_t callback)
{
    LLCoros::instance().launch("LLGridInfo::fetchCoro", [uri, callback]() { fetchCoro(uri, callback); });
}
