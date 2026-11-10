/**
 * @file lleventpoll.cpp
 * @brief Implementation of the LLEventPoll class.
 *
 * $LicenseInfo:firstyear=2006&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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

#include "lleventpoll.h"
#include "llappviewer.h"
#include "llagent.h"

#include "llsdserialize.h"
#include "lleventtimer.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "message.h"
#include "lltransportmainwake.h"
#include <new>
#include <array>
#include <algorithm>
#include <chrono>
#include "workqueue.h"
#include "lltrans.h"
#include "llcoros.h"
#include "lleventcoro.h"
#include "llcorehttputil.h"
#include "lleventfilter.h"

namespace LLEventPolling
{
namespace Details
{
    // Fixed metadata registry; only main accesses it. HTTP workers retain just the
    // native wake. A full lane backpressures the response coroutine before its
    // next HTTP acknowledgement instead of dropping accepted simulator events.
    static std::array<std::weak_ptr<LLEventPollImpl>, 1024> sPolls;
    static size_t sPollCursor = 0;
    static std::unique_ptr<LL::WorkQueue> sCapabilityWork;

    static LL::WorkQueue* capabilityWork() noexcept
    {
        if (!sCapabilityWork)
        {
            try { sCapabilityWork = std::make_unique<LL::WorkQueue>("sim-capabilities", 1024, false); }
            catch (const std::bad_alloc&) { return nullptr; }
        }
        return sCapabilityWork.get();
    }

    static bool messagingAvailable() noexcept
    {
        return gMessageSystem && !gDisconnected && !gDoDisconnect
            && !messagingTeardownPendingOnMain();
    }


    enum class CapabilityDispatch { Handled, Retry, Failed };
    struct PendingCapability;
    static std::shared_ptr<PendingCapability> sBlockedCapability;

    class LLEventPollImpl: public std::enable_shared_from_this<LLEventPollImpl>
    {
    public:
        LLEventPollImpl(const LLHost &sender);

        void start(const std::string &url);
        void stop();
        void serviceReply();
        static CapabilityDispatch handleMessage(const std::string& sender_ip, const LLSD& content);
        bool stopped() const noexcept { return mDone; }

    private:
        // We will wait RETRY_SECONDS + (errorCount * RETRY_SECONDS_INC) before retrying after an error.
        // This means we attempt to recover relatively quickly but back off giving more time to recover
        // until we finally give up after MAX_EVENT_POLL_HTTP_ERRORS attempts.
        static const F32                EVENT_POLL_ERROR_RETRY_SECONDS;
        static const F32                EVENT_POLL_ERROR_RETRY_SECONDS_INC;
        static const S32                MAX_EVENT_POLL_HTTP_ERRORS;

        void                            eventPollCoro(std::string url);

        bool                            mDone;
        LLCore::HttpRequest::ptr_t      mHttpRequest;
        LLCore::HttpRequest::policy_t   mHttpPolicy;
        std::string                     mSenderIp;
        int                             mCounter;
        LLCoreHttpUtil::HttpCoroutineAdapter::wptr_t mAdapter;

        static int                      sNextCounter;
    };


    struct PendingCapability
    {
        std::weak_ptr<LLEventPollImpl> poll;
        std::string sender;
        LLSD message;
        uint64_t loginGeneration = 0;
        bool done = false;
        bool failed = false;
    };

    // Main capability lane only. A refused reservation remains at its head so
    // neither later events nor the HTTP response acknowledgement can pass it.
    static bool dispatchCapability(const std::shared_ptr<PendingCapability>& item)
    {
        if (!messagingAvailable()) return false;
        if (item->loginGeneration != gMessageSystem->transportLoginGenerationOnMain())
        { item->failed = true; return true; }
        const auto poll = item->poll.lock();
        if (!poll || poll->stopped())
        {
            LLWorld* world = LLWorld::getInstance();
            if (!world || !world->getRegion(LLHost(item->sender)))
            { item->done = true; return true; }
        }
        try
        {
            const auto result = LLEventPollImpl::handleMessage(item->sender, item->message);
            if (item->loginGeneration && result == CapabilityDispatch::Retry) return false;
            if (item->loginGeneration && (result == CapabilityDispatch::Failed || !messagingAvailable() ||
                item->loginGeneration != gMessageSystem->transportLoginGenerationOnMain()))
            { item->failed = true; return true; }
            item->done = true;
        }
        catch (...)
        {
            item->failed = true; // never acknowledge or replay partial handler effects
        }
        return true;
    }

    const F32 LLEventPollImpl::EVENT_POLL_ERROR_RETRY_SECONDS = 15.f; // ~ half of a normal timeout.
    const F32 LLEventPollImpl::EVENT_POLL_ERROR_RETRY_SECONDS_INC = 5.f; // ~ half of a normal timeout.
    const S32 LLEventPollImpl::MAX_EVENT_POLL_HTTP_ERRORS = 10; // ~5 minutes, by the above rules.

    int LLEventPollImpl::sNextCounter = 1;


    LLEventPollImpl::LLEventPollImpl(const LLHost &sender) :
        mDone(false),
        mHttpRequest(),
        mHttpPolicy(LLCore::HttpRequest::DEFAULT_POLICY_ID),
        mSenderIp(),
        mCounter(sNextCounter++)

    {
        LLAppCoreHttp & app_core_http(LLAppViewer::instance()->getAppCoreHttp());

        mHttpRequest = LLCore::HttpRequest::ptr_t(new LLCore::HttpRequest);
        mHttpPolicy = app_core_http.getPolicy(LLAppCoreHttp::AP_LONG_POLL);
        mSenderIp = sender.getIPandPort();
    }

    CapabilityDispatch LLEventPollImpl::handleMessage(const std::string& sender_ip, const LLSD& content)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_APP;
        std::string msg_name = content["message"].asString();
        LLSD message;
        try
        {
            message["sender"] = sender_ip;
            message["body"] = content["body"];
        }
        catch (std::bad_alloc&)
        {
            LLError::LLUserWarningMsg::showOutOfMemory();
            LL_ERRS("LLCoros") << "Bad memory allocation on message: " << msg_name << LL_ENDL;
        }

        const LLPointer<LLSimpleResponse> response = LLSimpleResponse::create();
        LLMessageSystem::dispatch(msg_name, message, response);
        if (response->mCode == 503 && response->mMessage == "Transport peer admission full") return CapabilityDispatch::Retry;
        if (response->mCode == 503 && response->mMessage == "Messaging unavailable") return CapabilityDispatch::Failed;
        return CapabilityDispatch::Handled;
    }

    void LLEventPollImpl::start(const std::string &url)
    {
        if (!url.empty())
        {
            // This notifier runs on the HTTP worker, under its reply-queue lock.
            // Retain only the native wake: never the poll, viewer globals or a handler.
            // Main still decodes and dispatches the response at its service point.
            if (gMessageSystem && gMessageSystem->initializeTransportMainWakeOnMain())
            {
                const auto wake = gMessageSystem->transportMainWakeOnMain();
                if (wake)
                {
                    try
                    {
                        mHttpRequest->setReplyNotifier([wake]() noexcept { wake->signal(); });
                    }
                    catch (const std::bad_alloc&)
                    {
                        // Keep the request alive and serviced by the normal main-loop
                        // pumper; allocation failure must not drop an acknowledged event.
                    }
                }
            }
            bool registered = false;
            for (auto& entry : sPolls)
            {
                if (entry.expired())
                {
                    entry = weak_from_this();
                    registered = true;
                    break;
                }
            }
            if (!registered)
            {
                // The normal HTTP main-loop pumper remains available; no extra
                // registry allocation or worker-side handler fallback.
                LL_WARNS("LLEventPollImpl") << "Capability service registry is full" << LL_ENDL;
            }
            std::string coroname =
                LLCoros::instance().launch("LLEventPollImpl::eventPollCoro",
                boost::bind(&LLEventPollImpl::eventPollCoro, this->shared_from_this(), url));
            LL_INFOS("LLEventPollImpl") << coroname << " with  url '" << url << LL_ENDL;
        }
    }

    void LLEventPollImpl::serviceReply()
    {
        if (!mDone && messagingAvailable())
            mHttpRequest->update(1L); // one reply may exceed this soft limit
    }

    void LLEventPollImpl::stop()
    {
        mDone = true;

        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter = mAdapter.lock();
        if (adapter)
        {
            LL_INFOS() << "requesting stop for event poll coroutine <" << mCounter << ">" << LL_ENDL;
            // cancel the yielding operation if any.
            adapter->cancelSuspendedOperation();
        }
        else
        {
            LL_INFOS() << "Coroutine for poll <" << mCounter << "> previously stopped.  No action taken." << LL_ENDL;
        }
    }

    void LLEventPollImpl::eventPollCoro(std::string url)
    {
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t httpAdapter(new LLCoreHttpUtil::HttpCoroutineAdapter("EventPoller", mHttpPolicy));
        LLSD acknowledge;
        int errorCount = 0;
        int counter = mCounter; // saved on the stack for logging.

        LL_DEBUGS("LLEventPollImpl") << " <" << counter << "> entering coroutine." << LL_ENDL;

        mAdapter = httpAdapter;

        // No event handler runs from the HTTP reply callback/coroutine. All are
        // posted FIFO to the dedicated main capability lane below.

        // continually poll for a server update until we've been flagged as
        // finished
        while (!mDone)
        {
            LLSD request;
            request["ack"] = acknowledge;
            request["done"] = mDone;

//          LL_DEBUGS("LLEventPollImpl::eventPollCoro") << "<" << counter << "> request = "
//              << LLSDXMLStreamer(request) << LL_ENDL;

            LL_DEBUGS("LLEventPollImpl") << " <" << counter << "> posting and yielding." << LL_ENDL;
            LLSD result = httpAdapter->postAndSuspend(mHttpRequest, url, request);

//          LL_DEBUGS("LLEventPollImpl::eventPollCoro") << "<" << counter << "> result = "
//              << LLSDXMLStreamer(result) << LL_ENDL;

            if (!messagingAvailable())
            {
                // Lost connection or disconnected during quit, don't process sim/region update
                // messages, they might populate some cleaned up classes (LLWorld, region and object list)
                LL_INFOS("LLEventPollImpl") << "Dropping event messages" << LL_ENDL;
                break;
            }

            LLSD httpResults = result["http_result"];
            LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(httpResults);

            if (!status)
            {
                if (status == LLCore::HttpStatus(LLCore::HttpStatus::EXT_CURL_EASY, CURLE_OPERATION_TIMEDOUT))
                {   // A standard timeout response we get this when there are no events.
                    LL_DEBUGS("LLEventPollImpl") << "All is very quiet on target server. It may have gone idle?" << LL_ENDL;
                    errorCount = 0;
                    continue;
                }
                else if ((status == LLCore::HttpStatus(LLCore::HttpStatus::LLCORE, LLCore::HE_OP_CANCELED)) ||
                        (status == LLCore::HttpStatus(HTTP_NOT_FOUND)))
                {   // Event polling for this server has been canceled.  In
                    // some cases the server gets ahead of the viewer and will
                    // return a 404 error (Not Found) before the cancel event
                    // comes back in the queue
                    LL_WARNS("LLEventPollImpl") << "Canceling coroutine" << LL_ENDL;
                    break;
                }
                else if (!status.isHttpStatus())
                {
                    /// Some LLCore or LIBCurl error was returned.  This is unlikely to be recoverable
                    LL_WARNS("LLEventPollImpl") << "Critical error from poll request returned from libraries.  Canceling coroutine." << LL_ENDL;
                    break;
                }
                LL_WARNS("LLEventPollImpl") << "<" << counter << "> Error result from LLCoreHttpUtil::HttpCoroHandler. Code "
                    << status.toTerseString() << ": '" << httpResults["message"] << "'" << LL_ENDL;

                if (errorCount < MAX_EVENT_POLL_HTTP_ERRORS)
                {   // An unanticipated error has been received from our poll
                    // request. Calculate a timeout and wait for it to expire(sleep)
                    // before trying again.  The sleep time is increased by 5 seconds
                    // for each consecutive error.
                    ++errorCount;

                    F32 waitToRetry = EVENT_POLL_ERROR_RETRY_SECONDS
                        + errorCount * EVENT_POLL_ERROR_RETRY_SECONDS_INC;

                    LL_WARNS("LLEventPollImpl") << "<" << counter << "> Retrying in " << waitToRetry <<
                        " seconds, error count is now " << errorCount << LL_ENDL;

                    llcoro::suspendUntilTimeout(waitToRetry);

                    if (mDone || gDisconnected)
                        break;
                    LL_INFOS("LLEventPollImpl") << "<" << counter << "> About to retry request." << LL_ENDL;
                    continue;
                }
                else
                {
                    // At this point we have given up and the viewer will not receive HTTP messages from the simulator.
                    // IMs, teleports, about land, selecting land, region crossing and more will all fail.
                    // They are essentially disconnected from the region even though some things may still work.
                    // Since things won't get better until they relog we force a disconnect now.
                    mDone = true;

                    // *NOTE:Mani - The following condition check to see if this failing event poll
                    // is attached to the Agent's main region. If so we disconnect the viewer.
                    // Else... its a child region and we just leave the dead event poll stopped and
                    // continue running.
                    if (gAgent.getRegion() && gAgent.getRegion()->getHost().getIPandPort() == mSenderIp)
                    {
                        LL_WARNS("LLEventPollImpl") << "< " << counter << "> Forcing disconnect due to stalled main region event poll." << LL_ENDL;
                        LLAppViewer::instance()->forceDisconnect(LLTrans::getString("AgentLostConnection"));
                    }
                    break;
                }
            }

            errorCount = 0;

            if (!result.isMap() ||
                !result.has("events") ||
                !result["events"].isArray() ||
                !result.has("id"))
            {
                LL_WARNS("LLEventPollImpl") << " <" << counter << "> received event poll with no events or id key: " << result << LL_ENDL;
                continue;
            }

            const LLSD responseId = result["id"];
            LLSD events = result["events"];

            if (responseId.isUndefined())
            {
                LL_WARNS("LLEventPollImpl") << " id undefined" << LL_ENDL;
            }

            // was LL_INFOS() but now that CoarseRegionUpdate is TCP @ 1/second, it'd be too verbose for viewer logs. -MG
            LL_DEBUGS("LLEventPollImpl") << " <" << counter << "> " << events.size() << "events (id " << responseId << ")" << LL_ENDL;


            LLSD::array_const_iterator i = events.beginArray();
            LLSD::array_const_iterator end = events.endArray();
            for (; i != end; ++i)
            {
                if (i->has("message"))
                {
                    std::shared_ptr<PendingCapability> item;
                    while (!item && !mDone && messagingAvailable())
                    {
                        try
                        {
                            item = std::make_shared<PendingCapability>();
                            item->poll = weak_from_this(); item->sender = mSenderIp; item->message = *i;
                            item->loginGeneration = gMessageSystem->transportLoginGenerationOnMain();
                        }
                        catch (const std::bad_alloc&) { item.reset(); llcoro::suspend(); }
                    }
                    bool queued = false;
                    while (item && !queued && !mDone && messagingAvailable())
                    {
                        if (auto* queue = capabilityWork())
                        {
                            try
                            {
                                queued = queue->tryPost([item]()
                                {
                                    if (!dispatchCapability(item)) sBlockedCapability = item;
                                });
                            }
                            catch (const std::bad_alloc&) {}
                        }
                        if (queued)
                        {
                            // Preserve legacy response acknowledgement after
                            // queue admission when the experiment is inactive.
                            if (!item->loginGeneration) item->done = true;
                            if (const auto wake = gMessageSystem->transportMainWakeOnMain()) wake->signal();
                        }
                        else llcoro::suspend();
                    }
                    // Do not acknowledge the HTTP response until the actual
                    // main-thread handler has completed, not merely queued.
                    while (queued && !item->done && !item->failed && !mDone && messagingAvailable())
                        llcoro::suspend();
                    if (!queued || !item->done || item->failed) break;
                }
            }
            if (i != end || mDone || !messagingAvailable()) break;
            acknowledge = responseId;
        }
        LL_DEBUGS("LLEventPollImpl") << " <" << counter << "> Leaving coroutine." << LL_ENDL;
    }

}
}

void LLEventPoll::serviceMainThread(unsigned budgetUsec)
{
    using namespace LLEventPolling::Details;
    static bool servicing = false;
    if (servicing || !messagingAvailable() || !budgetUsec) return;
    struct ServiceScope
    {
        bool& active;
        explicit ServiceScope(bool& value) : active(value) { active = true; }
        ~ServiceScope() { active = false; }
    } scope(servicing);
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::microseconds(std::min(budgetUsec, 2000u));
    if (sBlockedCapability)
    {
        if (!dispatchCapability(sBlockedCapability)) return;
        sBlockedCapability.reset();
    }
    // Drain older events before collecting newer HTTP replies. Each poll retains
    // response order; shared lane admission is FIFO across polls.
    if (sCapabilityWork)
    {
        for (unsigned n = 0; n != 256 && sCapabilityWork->size()
             && messagingAvailable() && !sBlockedCapability && Clock::now() < deadline; ++n)
            sCapabilityWork->runOne();
    }
    if (sBlockedCapability) return;
    unsigned pumped = 0;
    for (size_t scanned = 0; scanned != sPolls.size() && pumped != 8
         && messagingAvailable() && Clock::now() < deadline; ++scanned)
    {
        const auto poll = sPolls[sPollCursor].lock();
        sPollCursor = (sPollCursor + 1) % sPolls.size();
        if (poll)
        {
            ++pumped;
            poll->serviceReply();
        }
    }
    if (sCapabilityWork)
    {
        for (unsigned n = 0; n != 256 && sCapabilityWork->size()
             && messagingAvailable() && !sBlockedCapability && Clock::now() < deadline; ++n)
            sCapabilityWork->runOne();
    }
}

LLEventPoll::LLEventPoll(const std::string& poll_url, const LLHost& sender):
    mImpl()
{
    mImpl = std::make_shared<LLEventPolling::Details::LLEventPollImpl>(sender);
    mImpl->start(poll_url);
}

LLEventPoll::~LLEventPoll()
{
    mImpl->stop();

}
