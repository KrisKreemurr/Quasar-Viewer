/**
 * @file sltelemetry.cpp
 * @author SkyLine
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 * $/LicenseInfo$
 */
#include "llviewerprecompiledheaders.h"
#include "sltelemetry.h"
#include "sltelemetryreport.h"
#include "sltelemetryqueue.h"
#include "llagent.h"
#include "llappviewer.h"
#include "llcharacter.h"
#include "llframetimer.h"
#include "lltexturefetch.h"
#include "llvoavatar.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lleventcoro.h"
#include "llfocusmgr.h"
#include "llgpuinfo.h"
#include "llimageworker.h"
#include "llmemorygovernor.h"
#include "llnotificationsutil.h"
#include "llstartup.h"
#include "llsys.h"
#include "llversioninfo.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewertexturelist.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "llviewernetwork.h"
#include "rlvhandler.h"
#include <sstream>
#include <string_view>
#if LL_LINUX
#include <sys/utsname.h>
#endif
#if __has_include("vkgfx/llxrsession.h")
#include "vkgfx/llxrsession.h"
#define SL_TELEMETRY_HAS_XR 1
#endif
#include "slupdatechecker.h"
#include "sldatausage.h"
#include "lltexturecache.h"
#include "workqueue.h"
#include "httpresponse.h"
#include "httphandler.h"
#include <SDL3/SDL.h>
#include <openssl/hmac.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <boost/json.hpp>
#if __has_include("slbench.h")
#include "slbench.h"
#define SL_TELEMETRY_HAS_BENCH 1
#endif
#include <thread>

// A public anti-abuse deterrent; never an account credential.
#ifndef SLIPSTREAM_TELEMETRY_HMAC_KEY
#define SLIPSTREAM_TELEMETRY_HMAC_KEY "b70fc0951b274a982a47df583e17f6291653fa408810547c"
#endif
namespace SLTelemetry
{
namespace
{
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr char ENDPOINT[] = "https://slipstreamviewer.org/api/telemetry/v1";
using namespace QueueDetail;
Window sWindow;
CollectionClock sCollectionClock;
unsigned sIndex = 0, sQueued = 0, sFailures = 0;
bool sWorker = false, sSending = false, sStarted = false, sShutdown = false, sWritingConsent = false;
bool sWasLoggedIn = false, sTeleporting = false, sNeedInitialize = false, sPurgeWorker = false;
InitialSegmentGate sInitialSegment;
Snapshot sHardware;
LLMemoryGovernor::AllocFailStats sAllocBaseline;
std::array<U64,3> sCacheBaseline{};
SLDataUsage::NumericTotals sDataBaseline;
using PolledBaselines = CounterBaselines<LLMemoryGovernor::AllocFailStats,
    std::array<U64,3>, SLDataUsage::NumericTotals>;
void resetBaselines(const PolledBaselines* sampled = nullptr)
{
    if (sampled)
    {
        sAllocBaseline = sampled->allocation;
        sCacheBaseline = sampled->cache;
        sDataBaseline = sampled->data;
        return;
    }
    sAllocBaseline=LLMemoryGovernor::getAllocFailStats();
    sDataBaseline=SLDataUsage::numericTotals();
    if(const auto* cache=LLAppViewer::getTextureCache())
        sCacheBaseline={cache->getStoreReadHits(),cache->getStoreReadMisses(),cache->getStoreReadErrors()};
}
Clock::time_point sNextSecond, sNextSend, sSteadyAfter, sTeleportStarted, sLoggedAt;
double sPreviousSteady=0;
bool sStutter=false,sHaveLogin=false,sTrackTeleport=false;
std::string sLast, sLastDay, sPendingPreview;
std::string sRoot; // Set on the main thread, only after explicit opt-in.
unsigned sLastCheckpoint = 0;

double secondsUntil(Clock::time_point when) { return std::chrono::duration<double>(when - Clock::now()).count(); }
void warning() { LL_WARNS("SLTelemetry") << "report_dropped" << LL_ENDL; }

bool excluded()
{
    // --bench belongs to a newer harness; inspect argv without introducing that dependency.
    // This also excludes the older in-viewer A/B benchmark.
    if (LLAppViewer::instance()->isSecondInstance()) return true;
#ifdef SL_TELEMETRY_HAS_BENCH
    if (SLBench::replayRequested()) return true;
#endif
    return gSavedSettings.getBOOL("RenderVKBench");
}
std::string boundedRead(const std::string& path, std::size_t limit)
{
    std::ifstream file(path, std::ios::binary);
    std::string text(limit, '\0');
    file.read(text.data(), static_cast<std::streamsize>(limit));
    text.resize(static_cast<std::size_t>(file.gcount()));
    return text;
}
unsigned enumIndex(const char* text, const char* const* choices, unsigned count)
{
    if (!text) return count-1;
    std::size_t length = 0;
    while (length < 65 && text[length]) ++length;
    if (length > 64) return count-1;
    const std::string_view value(text,length);
    const auto first = value.substr(0,value.find(':'));
    for (unsigned i=0; i<count; ++i) if (first == choices[i]) return i;
    return count-1;
}
void probeOS(Snapshot& result)
{
    const auto install = SLUpdateChecker::installKindName();
    static constexpr const char* installs[] = {"windows_setup", "windows_zip", "flatpak", "deb", "aur", "tarball", "other"};
    for (unsigned i = 0; i < 7; ++i) if (install == std::string(installs[i])) result.install = i;
#if LL_LINUX
    struct utsname kernel{};
    if (uname(&kernel) == 0)
    {
        unsigned major=0, minor=0;
        if (std::sscanf(kernel.release, "%u.%u", &major, &minor) == 2 && major <= 999 && minor <= 999) result.kernel = {major,minor};
    }
    result.flatpak = std::getenv("FLATPAK_ID") || fs::exists("/.flatpak-info");
    const auto release = boundedRead(result.flatpak ? "/run/host/os-release" : "/etc/os-release", 4096);
    std::istringstream lines(release); std::string line;
    static constexpr const char* distros[] = {"arch","cachyos","endeavouros","manjaro","ubuntu","debian","linuxmint","pop","fedora","opensuse-tumbleweed","opensuse-leap","nixos","gentoo","void","steamos","bazzite","alpine","rocky","almalinux","rhel","centos","zorin","elementary","kali","mx","solus","garuda","artix","other"};
    while (std::getline(lines,line))
    {
        const bool identifier = line.rfind("ID=",0)==0;
        const bool versionID = line.rfind("VERSION_ID=",0)==0;
        if (!identifier && !versionID) continue;
        auto value = line.substr(identifier ? 3 : 11);
        if (value.size() >= 2 && (value.front()=='"' || value.front()=='\'' ) && value.back()==value.front()) value=value.substr(1,value.size()-2);
        if (identifier) result.distro=enumIndex(value.c_str(),distros,29);
        else
        {
            unsigned major=0;
            if (!value.empty() && value[0]>='0' && value[0]<='9' && std::sscanf(value.c_str(),"%u",&major)==1 && major<=999) result.distroMajor=major;
        }
    }
    const auto meminfo=boundedRead("/proc/meminfo",16384);
    const auto swap=meminfo.find("SwapTotal:");
    if (swap!=std::string::npos)
    {
        unsigned long long total=0;
        if (std::sscanf(meminfo.c_str()+swap,"SwapTotal: %llu",&total)==1) result.swapPresent=total>0;
    }
#endif
}
Snapshot snapshot()
{
    Snapshot result;
    // Copy only bounded existing strings. Normalization and grammar validation run
    // in the worker report builder (or the explicitly requested one-shot preview).
    const auto cpu=gSysCPU.getCPUString();
    if(cpu.size()<=256)std::copy(cpu.begin(),cpu.end(),result.rawCPU.begin());
    if(gGPUInfo.mRenderer.size()<=256)std::copy(gGPUInfo.mRenderer.begin(),gGPUInfo.mRenderer.end(),result.rawGPU.begin());
    const auto version = LLVersionInfo::instance().getVersion();
    if (version.size() < result.version.size()) std::copy(version.begin(), version.end(), result.version.begin());
    auto channel = LLVersionInfo::instance().getChannel();
    LLStringUtil::toLower(channel);
    result.channel = channel == "slipstream" || channel == "slipstream release" ? 0 :
                     channel == "slipstream beta" ? 1 : channel == "jetstream" || channel == "slipstream jetstream" ? 2 :
                     channel == "slipstream test" ? 3 : 4;
    if (LLVersionInfo::instance().getBuild() == 0) result.channel = 3;
    const auto build = LLVersionInfo::instance().getBuildConfig();
    result.build = build == "Release" ? 0 : build == "RelWithDebInfo" ? 1 : build == "Debug" ? 2 : 3;
#if defined(__AVX2__) || defined(_M_AVX2)
    result.isa = 0;
#elif defined(__aarch64__) || defined(_M_ARM64)
    result.isa = 2;
#elif defined(__SSE4_2__) || defined(_M_X64)
    result.isa = 1;
#endif
#if LL_WINDOWS
    result.os = 0;
#elif LL_LINUX
    result.os = 1;
#elif LL_DARWIN
    result.os = 2;
#endif
    static constexpr const char* desktops[] = {"KDE","GNOME","Hyprland","sway","XFCE","Cinnamon","MATE","LXQt","Budgie","COSMIC","gamescope","other"};
    result.desktop = enumIndex(std::getenv("XDG_CURRENT_DESKTOP"), desktops, 12);
    const char* driver = SDL_GetCurrentVideoDriver();
    static constexpr const char* sessions[] = {"wayland","x11","windows","other"};
    result.session = enumIndex(driver, sessions, 4);
    const auto systemMemory = LLMemoryGovernor::cachedSystemMemory();
    result.psi = systemMemory.have_psi;
    result.swapPresent = systemMemory.have_commit && systemMemory.commit_limit > systemMemory.total;
#if LL_WINDOWS
    result.windowsBuild = static_cast<unsigned>(llclamp(LLOSInfo::instance().mBuild, 0, 999999));
#elif LL_LINUX
    result.glibc = {static_cast<unsigned>(llclamp(LLOSInfo::instance().mMajorVer,0,999)), static_cast<unsigned>(llclamp(LLOSInfo::instance().mMinorVer,0,999))};

#endif
    result.features = {gSavedSettings.getBOOL("EnableVoiceChat"),gSavedSettings.getBOOL("ParcelMediaAutoPlayEnable"),
        RlvHandler::isEnabled(),gSavedSettings.getBOOL("SlipstreamDiscordEnabled"),gSavedPerAccountSettings.getBOOL("SLAOEnabled"),
        !gSavedSettings.getString("ExternalEditor").empty(),gSavedSettings.getBOOL("AutoTuneLock")};
    result.grid = LLGridManager::instance().isInProductionGrid() ? 0 : LLGridManager::instance().isSystemGrid() ? 1 : 2;
    const unsigned threads = std::thread::hardware_concurrency();
    result.threads = threads <= 32 ? threads : threads <= 48 ? 48 : threads <= 64 ? 64 : threads <= 96 ? 96 : threads <= 128 ? 128 : 129;
    const unsigned ramMB=static_cast<unsigned>(gSysMemory.getPhysicalMemoryKB().valueInUnits<LLUnits::Megabytes>());
    const unsigned ram=(ramMB+512)/1024;
    static constexpr unsigned ramBuckets[] = {4,6,8,12,16,24,32,48,64,96,128,192,256};
    result.ramGB = ramMB > 256*1024 ? 257 : ram ? nearestBucket(ram, ramBuckets) : 0;
    static constexpr unsigned vramBuckets[] = {2,3,4,6,8,10,11,12,16,20,24,32,48};
    const unsigned vram=(gGPUInfo.mVRAM+512)/1024;
    result.vramGB = gGPUInfo.mVRAM > 48*1024 ? 49 : vram ? nearestBucket(vram, vramBuckets) : 0;
    result.vendor = gGPUInfo.mIsNVIDIA ? 0 : gGPUInfo.mIsAMD ? 1 : gGPUInfo.mIsIntel ? 2 : 4;
    for (unsigned i = 0; i < SETTING_COUNT; ++i)
    {
        const auto& rule = SETTING_RULES[i];
        const auto control = gSavedSettings.getControl(rule.name);
        if (!control) continue;
        const auto type = control->type();
        if (rule.type == 0 ? type != TYPE_BOOLEAN : type != TYPE_S32 && type != TYPE_U32 && type != TYPE_F32) continue;
        const double raw = rule.type == 0 ? (control->getValue().asBoolean() ? 1.0 : 0.0) : control->getValue().asReal();
        if (!std::isfinite(raw)) continue;
        double value = (std::max)(rule.lower, (std::min)(raw, rule.upper));
        if (rule.bucketCount)
        {
            double chosen = rule.buckets[0], distance = std::abs(value-chosen);
            for (unsigned j = 1; j < rule.bucketCount; ++j)
            {
                const double delta = std::abs(value-rule.buckets[j]);
                if (delta < distance) { chosen = rule.buckets[j]; distance = delta; }
            }
            value = chosen;
        }
        else value = std::round(value/rule.step)*rule.step;
        result.settings[i] = value; result.hasSetting[i] = true;
    }
    result.previousEnd = gLastExecDeviceLost ? 7 : gLastExecEvent == LAST_EXEC_NORMAL ? 1 :
                         gLastExecEvent == LAST_EXEC_BAD_ALLOC ? 6 : gLastExecEvent == LAST_EXEC_LLERROR_CRASH ? 3 :
                         gLastExecEvent == LAST_EXEC_FROZE || gLastExecEvent == LAST_EXEC_LOGOUT_FROZE ? 4 :
                         gLastExecEvent == LAST_EXEC_LOGOUT_CRASH ? 5 : 2;
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    if (displays) SDL_free(displays);
    result.monitors = static_cast<unsigned>(llclamp(count, 0, 3));
    // SDL's keyboard focus window is the viewer window while focused. Do not substitute a
    // different monitor when unfocused: explicit unavailable is preferable to wrong data.
    SDL_Window* window = SDL_GetKeyboardFocus();
    const SDL_DisplayID display = window ? SDL_GetDisplayForWindow(window) : 0;
    const SDL_DisplayMode* mode = display ? SDL_GetCurrentDisplayMode(display) : nullptr;
    if (mode)
    {
        result.displayAvailable = true;
        const int width = mode->w, height = mode->h;
        result.resolution = width == 3440 && height == 1440 ? 4 : width == 2560 && height == 1080 ? 5 :
            width == 1280 && height == 720 ? 0 : width == 1920 && height == 1080 ? 1 :
            width == 2560 && height == 1440 ? 2 : width == 3840 && height == 2160 ? 3 :
            width == 5120 && height == 2880 ? 6 : width == 7680 && height == 4320 ? 7 : 8;
        static constexpr unsigned refresh[] = {30,50,60,75,90,100,120,144,165,170,180,200,240,280,360};
        result.refresh = mode->refresh_rate > 360 ? 361 : nearestBucket(static_cast<unsigned>((std::max)(0.f, mode->refresh_rate)), refresh);
        result.scaleQuarters = static_cast<unsigned>(llclamp(SDL_GetWindowDisplayScale(window) * 4.f + .5f, 0.f, 32.f));
        result.hdr = SDL_GetBooleanProperty(SDL_GetDisplayProperties(display), SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false);
        const auto flags = SDL_GetWindowFlags(window);
        result.windowMode = flags & SDL_WINDOW_FULLSCREEN ? (SDL_GetWindowFullscreenMode(window) ? 3 : 2) : flags & SDL_WINDOW_MAXIMIZED ? 1 : 0;
    }
    return result;
}

bool safeURL(const std::string& url, unsigned channel)
{
    // Production is a single fixed origin; overrides are exclusively loopback fixtures.
    if (url == ENDPOINT) return channel <= 2;
    static const std::regex local(R"(^http://(127\.0\.0\.1|localhost)(:[0-9]{1,5})?/api/telemetry/v1$)");
    return std::regex_match(url, local);
}
class Reply final : public LLCore::HttpHandler
{
public:
    explicit Reply(LLEventPump& pump) : mPump(pump) {}
    void onCompleted(LLCore::HttpHandle, LLCore::HttpResponse* response) override
    {
        LLSD value;
        const auto status = response->getStatus();
        value["status"] = status.isHttpStatus() ? static_cast<int>(status.getType()) : 0;
        if (const auto headers = response->getHeaders())
        {
            if (const auto* retry = headers->find("retry-after"))
            {
                if (retry->size() <= 5 && std::all_of(retry->begin(), retry->end(), [](char c) { return c >= '0' && c <= '9'; }))
                    value["retry"] = llclamp(std::atoi(retry->c_str()), 0, 86400);
            }
        }
        // No error body, URL, headers, attacker-supplied text or identity-bearing logs.
        mPump.post(value);
    }
private:
    LLEventPump& mPump;
};
std::string sFinished;
bool sRemoveFinished = false;
void send(std::uint64_t epoch, const std::string& url, DiskResult report)
{
    try
    {
        if (!current(epoch) || sShutdown || report.body.empty()) { sSending = false; return; }
        const auto envelope=boost::json::parse(report.body);
        const auto& reportChannel=envelope.at("viewer").at("channel").as_string();
        if(url==ENDPOINT && reportChannel!="release" && reportChannel!="beta" && reportChannel!="jetstream")
        {
            sFinished=report.filename;sRemoveFinished=true;sSending=false;return;
        }
        unsigned char digest[EVP_MAX_MD_SIZE]; unsigned length = 0;
        const std::string stamp = std::to_string(std::time(nullptr));
        const std::string message = stamp + "\n" + report.body;
        const std::string key = SLIPSTREAM_TELEMETRY_HMAC_KEY;
        if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
                  reinterpret_cast<const unsigned char*>(message.data()), message.size(), digest, &length))
            throw std::runtime_error("signature");
        std::string signature; static constexpr char hex[] = "0123456789abcdef";
        for (unsigned i = 0; i < length; ++i) { signature += hex[digest[i] >> 4]; signature += hex[digest[i] & 15]; }
        LLCore::HttpOptions::ptr_t options(new LLCore::HttpOptions);
        options->setTimeout(30); options->setTransferTimeout(30); options->setRetries(0);
        options->setFollowRedirects(false); options->setSSLVerifyPeer(true); options->setSSLVerifyHost(true);
        options->setAnonymous(true); options->setWantHeaders(true); options->setMaxBodyBytes(4096);
        LLCore::HttpHeaders::ptr_t headers(new LLCore::HttpHeaders);
        headers->append("Content-Type", "application/json");
        const auto viewer = boost::json::parse(report.body).at("viewer").as_object();
        headers->append("X-Slipstream-Version", std::string(viewer.at("channel").as_string()) + " " + std::string(viewer.at("version").as_string()));
        headers->append("X-Slipstream-Timestamp", stamp);
        headers->append("X-Slipstream-Signature", signature);
        LLCore::BufferArray::ptr_t body(new LLCore::BufferArray);
        body->append(report.body.data(), report.body.size());
        LLCore::HttpRequest::ptr_t request(new LLCore::HttpRequest);
        LLEventStream pump("SLTelemetryReply", true);
        auto handler = std::make_shared<Reply>(pump);
        LLTempBoundListener pumper = LLEventPumps::instance().obtain("mainloop").listen(
            pump.getName(), [request](const LLSD&) { request->update(0L); return false; });
        if (!current(epoch) || sShutdown) { sSending = false; return; }
        const auto handle = request->requestPost(LLCore::HttpRequest::DEFAULT_POLICY_ID, url, body.get(), options, headers, handler);
        if (handle == LLCORE_HTTP_HANDLE_INVALID) throw std::runtime_error("transport");
        const LLSD reply = llcoro::suspendUntilEventOn(pump);
        if (current(epoch) && !sShutdown)
        {
            const int status = reply["status"].asInteger();
            const bool success = status == 200 || status == 202;
            sFinished = report.filename;
            sRemoveFinished = success || (status >= 300 && status < 500 && status != 429);
            sPendingPreview.clear();
            if (success)
            {
                sLast=report.body;sFailures=0;
                const auto date=localCalendar();char text[16]{};
                if(std::strftime(text,sizeof(text),"%Y-%m-%d",&date))sLastDay=text;
            }
            else if (!sRemoveFinished) ++sFailures;
            static constexpr unsigned delays[] = {60,300,1800,7200};
            const unsigned delay = sRemoveFinished ? 60 : delays[(std::min)(sFailures-1, 3u)];
            sNextSend = Clock::now() + std::chrono::seconds((std::max)(delay, static_cast<unsigned>(reply["retry"].asInteger())));
        }
    }
    catch (...) { warning(); sNextSend = Clock::now() + std::chrono::minutes(5); }
    sSending = false;
}
bool schedule(unsigned action, bool wantSend, const PolledBaselines* initialBaselines = nullptr)
{
    if (sWorker || !sAllowed.load()) return false;
    if (action == DISK_ACTION_INITIAL && !initialBaselines) return false;
    try
    {
        const auto main = LL::WorkQueue::getInstance("mainloop");
        const auto general = LL::WorkQueue::getInstance("General");
        if (!main || !general) return false;
        const auto epoch = sEpoch.load();
        const auto frozen = sWindow;
        const auto hardware = action ? snapshot() : sHardware;
        const unsigned index = sIndex;
        const bool loggedIn = LLStartUp::getStartupState() >= STATE_STARTED && !gDisconnected;
        const auto root = sRoot;
        const auto finished = sFinished;
        const bool removeFinished = sRemoveFinished;
        const auto url = gSavedSettings.getString("SLTelemetryURL");
        wantSend = wantSend && safeURL(url, hardware.channel) && !sSending && sFailures < 4 &&
            secondsUntil(sNextSend) <= 0;
        sWorker = true;
        const bool posted = main->postTo(general,
            [=] { return diskWork(epoch, root, frozen, hardware, index, action, loggedIn, wantSend, finished, removeFinished, &probeOS); },
            [epoch, wantSend, url, finished](DiskResult result)
            {
                sWorker = false;
                if (!current(epoch) || sShutdown) return;
                if (sFinished == finished) sFinished.clear();
                sQueued = result.count;
                sPendingPreview = result.body;
                if (result.body.empty()) sNextSend = Clock::now() + std::chrono::seconds(60);
                if (wantSend && !result.body.empty())
                {
                    try
                    {
                        sSending = true;
                        LLCoros::instance().launch("SLTelemetrySend", [epoch, url, result] { send(epoch, url, result); });
                    }
                    catch (...) { sSending = false; warning(); }
                }
            });
        if (!posted) { sWorker = false; return false; }
        if (action == 3) sNeedInitialize = false;
        if (action == 2 || action == 4 || action == DISK_ACTION_INITIAL)
        {
            sWindow = {}; sLastCheckpoint = 0;
            if (action == DISK_ACTION_INITIAL) resetBaselines(initialBaselines);
            else resetBaselines();
            if (action == DISK_ACTION_INITIAL)
            {
                sNextSecond = Clock::now() + std::chrono::seconds(1);
                sPreviousSteady = 0; sStutter = false; sTrackTeleport = false;
            }
            sIndex = (std::min)(sIndex+1, 15u);
        }
        return true;
    }
    catch (...) { sWorker = false; warning(); return false; }
}
void schedulePurge();
void persistenceError()
{
    sAllowed.store(false);
    LLSD args;
    args["MESSAGE"] = "Usage statistics remain paused. Slipstream could not save your privacy choice or finish deleting queued reports. Please check that your settings folder is writable and retry Stop sharing or Delete queued reports before closing the viewer.";
    LLNotificationsUtil::add("GenericAlert", args);
}
bool savePrivacy(bool pending)
{
    gSavedSettings.setBOOL("SLTelemetryPurgePending", pending);
    // Always serialize this internal latch, including false after an off-state delete.
    // This guarantees a nonzero save count even if every other setting is default.
    gSavedSettings.getControl("SLTelemetryPurgePending")->setPersist(LLControlVariable::PERSIST_ALWAYS);
    // LLControlGroup reports zero on write, close or atomic replacement failure.
    if (gSavedSettings.saveToFile(gSavedSettings.getString("ClientSettingsFile"), true)>0) return true;
    gSavedSettings.setBOOL("SLTelemetryPurgePending", true);
    sPurge.store(true);
    persistenceError();
    return false;
}
void activate()
{
    if (sShutdown || excluded() || !consentEnabled(gSavedSettings.getS32("SLTelemetryConsent"))) return;
    if (sPurge.load() || gSavedSettings.getBOOL("SLTelemetryPurgePending"))
    {
        sAllowed.store(false);
        sPurge.store(true);
        schedulePurge();
        return;
    }
    // No clock read or elapsed tracking before the consent and durable-purge gates above.
    const auto activated=Clock::now();
    sCollectionClock.start(activated);
    QueueDetail::sWarning = &warning;
    sEpoch.fetch_add(1);
    sWindow = {};
    if (!sInitialSegment.handedOff) sIndex = 0;
    sLastCheckpoint = 0;
    sWasLoggedIn=LLStartUp::getStartupState()>=STATE_STARTED && !gDisconnected;sHaveLogin=false;
    sTeleporting=gAgent.getTeleportState()!=LLAgent::TELEPORT_NONE;sTrackTeleport=false;
    sPreviousSteady=0;sStutter=false;sSteadyAfter=activated+std::chrono::seconds(20);
    sRoot = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "telemetry");
    sNextSecond = activated + std::chrono::seconds(1);
    sNextSend = activated + std::chrono::minutes(2);
    sFailures = 0; sNeedInitialize = true; sHardware = snapshot(); resetBaselines();
    sAllowed.store(true);
    schedule(3, false);
}
void schedulePurge()
{
    if (sShutdown || sPurgeWorker || !sPurge.load()) return;
    try
    {
        const auto main=LL::WorkQueue::getInstance("mainloop");
        const auto general=LL::WorkQueue::getInstance("General");
        if (!main || !general) return; // Persistent latch keeps restart/re-enable closed.
        const auto epoch=sEpoch.load();
        const auto root=gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS,"telemetry");
        sPurgeWorker=true;
        if (!main->postTo(general, [epoch,root]
        {
            try { std::lock_guard<std::mutex> guard(sDiskMutex); return purgeLocked(root,epoch); }
            catch (...) { warning(); return false; }
        }, [epoch](bool removed)
        {
            sPurgeWorker=false;
            if (sShutdown) return;
            if (sEpoch.load()!=epoch) { schedulePurge(); return; }
            if (!removed) { persistenceError(); return; }
            if (confirmPurge(epoch, &savePrivacy))
            {
                try { activate(); } catch (...) { warning(); }
            }
        })) sPurgeWorker=false;
    }
    catch (...) { sPurgeWorker=false; warning(); }
}
void changeConsent(S32 choice)
{
    sAllowed.store(false); sEpoch.fetch_add(1);
    if (!consentEnabled(choice)) sInitialSegment.consentRevoked();
    sWindow={};sPendingPreview.clear();sLast.clear();sLastDay.clear();sFinished.clear();sQueued=0;
    sWritingConsent=true;
    gSavedSettings.setS32("SLTelemetryConsent",choice);
    sWritingConsent=false;
    if (!consentEnabled(choice))
    {
        beginPurge(&savePrivacy);
        schedulePurge();
        return;
    }
    // Re-enable cannot bypass an old durable revocation, even if an earlier worker died.
    if (!savePrivacy(sPurge.load() || gSavedSettings.getBOOL("SLTelemetryPurgePending"))) return;
    try { activate(); } catch (...) { warning(); }
}
}
void noteRegionCrossing() {if(sAllowed.load() && sCollectionClock.acceptsEvent(sWindow,Clock::now()))increment(sWindow.regionCrossings);}
void noteDisconnect() {if(sAllowed.load() && sCollectionClock.acceptsEvent(sWindow,Clock::now()))increment(sWindow.disconnects);}
bool enabled() { return consentEnabled(gSavedSettings.getS32("SLTelemetryConsent")); }
void setConsent(bool share)
{
    changeConsent(share ? CONSENT_VERSION : -CONSENT_VERSION);
}
void startup(bool anotherPrompt)
{
    if (sStarted) return;
    sStarted = true;
    // A debug-setting change must have the same cancellation semantics as the UI.
    gSavedSettings.getControl("SLTelemetryConsent")->getSignal()->connect([](LLControlVariable*,const LLSD& value,const LLSD&)
    {
        if(sWritingConsent)return;
        changeConsent(value.asInteger());
    });
    if (excluded()) return;
    // Restore revocation before any activation, queue processing or hardware probe.
    restorePurge(gSavedSettings.getBOOL("SLTelemetryPurgePending"));
    if (gSavedSettings.getS32("SLTelemetryConsent")<0 && !sPurge.load())
        beginPurge(&savePrivacy); // Also cleans queues revoked by older versions.
    if (sPurge.load()) schedulePurge();
    if (enabled()) { try { activate(); } catch (...) { warning(); } }
    if (anotherPrompt || !needsPrompt(gSavedSettings.getS32("SLTelemetryConsent"))) return;
    LLNotificationsUtil::add(gSavedSettings.getS32("SLTelemetryConsent") == 0 ? "SLTelemetryPrompt" : "SLTelemetryPromptScope",
        LLSD(), LLSD(), [](const LLSD& notification, const LLSD& response)
        {
            const int answer = LLNotificationsUtil::getSelectedOption(notification, response);
            if (answer == 0 || answer == 1) setConsent(answer == 0);
            return false;
        });
}
void frameTick(double milliseconds, RendererSample (*rendererSample)())
{
    static LLCachedControl<S32> consent(gSavedSettings, "SLTelemetryConsent", 0);
    if (!consentEnabled(consent) || !sAllowed.load() || sShutdown) return;
    if (excluded()) { sAllowed.store(false); sEpoch.fetch_add(1); sWindow = {}; return; }
    const auto now = Clock::now();
    const bool loggedIn = LLStartUp::getStartupState() >= STATE_STARTED && !gDisconnected;
    const bool loginTransition = loggedIn && !sWasLoggedIn;
    if (loggedIn)
    {
        if (!sInitialSegment.armed) sInitialSegment.arm(now);
        else if (loginTransition) sInitialSegment.restartAfterLogin(now);
    }
    const bool teleporting = gAgent.getTeleportState() != LLAgent::TELEPORT_NONE;
    const bool teleportJustEnded = sTeleporting && !teleporting;
    const bool background = !gFocusMgr.getAppHasFocus() || !gViewerWindow ||
        !gViewerWindow->getWindow()->getVisible() || gViewerWindow->getWindow()->getMinimized();
    const bool settleBarrierClear = now >= sSteadyAfter && !loginTransition && !teleportJustEnded;
    const bool settled = settleBarrierClear && loggedIn && !teleporting && !background;
    const auto flushWindow=[&]
    {
        if (sWorker || !schedule(2,secondsUntil(sNextSend)<=0)) return false;
        // Dropped/backpressured seconds are not observations in the next hour.
        sNextSecond=now+std::chrono::seconds(1);
        sPreviousSteady=0;sStutter=false;sTrackTeleport=false;
        return true;
    };
    // Alignment precedes frame, event, class-duration and renderer sampling.
    // Busy/refused handoffs preserve every old-hour byte and drop future metrics.
    if (!sCollectionClock.beforeSample(sWindow,now,flushWindow)) return;
    if(sWindow.seconds>=3600) { flushWindow(); return; }
    if (loggedIn && sWindow.elapsedHour==ELAPSED_HOUR_UNKNOWN &&
        (!std::isfinite(milliseconds) || milliseconds<0)) return;
    if (loggedIn && !sWasLoggedIn)
    {
        sLoggedAt=now;sHaveLogin=true;sSteadyAfter=now+std::chrono::seconds(20);
        if(!sIndex)sWindow.loginBucket=durationBucket(LLFrameTimer::getElapsedSeconds());
    }
    sWasLoggedIn = loggedIn;
    if (teleporting && !sTeleporting) {increment(sWindow.teleports);sTeleportStarted=now;sTrackTeleport=true;}
    if (sTeleporting && !teleporting)
    {
        sSteadyAfter=now+std::chrono::seconds(20);
        if(sTrackTeleport)increment(sWindow.teleportDuration[durationBucket(std::chrono::duration<double>(now-sTeleportStarted).count())]);
    }
    sTeleporting = teleporting;
    bool vr = false;
#ifdef SL_TELEMETRY_HAS_XR
    vr = LLXrSession::active() && LLXrSession::instance().sessionRunning();
#endif
    const unsigned frameClass=vr?3:background?2:teleporting||now<sSteadyAfter?1:0;
    PolledBaselines sampledBaselines{};
    bool sampledBaselinesReady = false;
    if(loggedIn)
    {
        sWindow.frames[frameClass].add(milliseconds);
        if (sWindow.frames[frameClass].frames && sCollectionClock.stamp(sWindow,now,sNextSecond))
        {
            resetBaselines(); // Also rebase deltas after an empty/logged-out/stalled interval.
            sPreviousSteady=0;sStutter=false;
        }
        static LLCachedControl<U32> longFrame(gSavedSettings,"VKLongFrameLogMs",1000);
        if(milliseconds>longFrame)increment(sWindow.longFrames);
        if(frameClass==0)
        {
            if(sPreviousSteady>0)sWindow.jitter.add(std::abs(milliseconds-sPreviousSteady));
            sPreviousSteady=milliseconds;
            if(milliseconds>100)sStutter=true;
            if(!sIndex && sHaveLogin && sWindow.readyBucket==7)sWindow.readyBucket=durationBucket(std::chrono::duration<double>(now-sLoggedAt).count());
        }
        else sPreviousSteady=0;
    }
    if (now < sNextSecond) return;
    const auto elapsed=std::chrono::duration_cast<std::chrono::seconds>(now-(sNextSecond-std::chrono::seconds(1))).count();
    sNextSecond+=std::chrono::seconds(elapsed);
    if (loggedIn)
    {
        const unsigned sampled=static_cast<unsigned>((std::min)(std::int64_t(3600-sWindow.seconds),static_cast<std::int64_t>(elapsed)));
        sWindow.seconds+=sampled;
        addSaturated(sWindow.classSeconds[frameClass],sampled);
        if(sStutter)increment(sWindow.stutterSeconds);
        sStutter=false;
        sWindow.frames[0].runningMedian=percentile(sWindow.frames[0],.5);
        sWindow.frames[1].runningMedian=percentile(sWindow.frames[1],.5);
        sWindow.frames[3].runningMedian=percentile(sWindow.frames[3],.5);
        const auto memoryState = static_cast<unsigned>(LLMemoryGovernor::getState());
        if (memoryState < 3) addSaturated(sWindow.ramSeconds[memoryState],sampled);
        sWindow.peakRSSHalfGB = (std::max)(sWindow.peakRSSHalfGB,
            static_cast<std::uint32_t>((std::min)(LLMemoryGovernor::getPeakRSS() >> 29, U64(2048))));
        if(rendererSample)
        {
            const auto renderer=rendererSample();
            if(renderer.valid)
            {
                if(renderer.budget)
                {
                    const double ratio=static_cast<double>(renderer.usage)/renderer.budget;
                    increment(sWindow.vramRatio[static_cast<unsigned>((std::min)(15.0,ratio*16/1.5))]);
                }
                sWindow.vramHalfGB=(std::max)(sWindow.vramHalfGB,static_cast<std::uint32_t>((std::min)(std::uint64_t(2048),renderer.usage>>29)));
                sWindow.overheadQuarterGB=(std::max)(sWindow.overheadQuarterGB,static_cast<std::uint32_t>((std::min)(std::uint64_t(4096),renderer.overhead>>28)));
                if(renderer.mode<3)addSaturated(sWindow.vramSeconds[renderer.mode],sampled);
                sWindow.rungMax=(std::max)(sWindow.rungMax,(std::min)(64u,renderer.rung));
            }
            if(renderer.upscaler<4)addSaturated(sWindow.upscalerSeconds[renderer.upscaler],sampled);
            if(renderer.gpuAge<3 && renderer.gpuMs>0 && std::isfinite(renderer.gpuMs))
            {
                sWindow.gpuTime.add(renderer.gpuMs);sWindow.rigTime.add(renderer.rigMs);
                if(frameClass==0)
                {
                    const double median=percentile(sWindow.frames[0],.5);
                    const double limit=gSavedSettings.getS32("RenderFPSLimit");
                    const double capMS=limit>0?1000/limit:0;
                    const bool capped=capMS>0 && std::abs(median-capMS)<capMS*.05;
                    addSaturated(sWindow.boundSeconds[capped?2:renderer.gpuMs>=median*.85?0:1],sampled);
                }
            }
        }
        const auto systemMemory = LLMemoryGovernor::cachedSystemMemory();
        const U64 totalRAM = LLMemoryGovernor::getTotalRAM();
        if (totalRAM)
        {
            const auto ratioBin = static_cast<unsigned>((std::min)(U64(15), LLMemoryGovernor::getAvailBytes()*16/totalRAM));
            increment(sWindow.availableRatio[ratioBin]);
        }
        sWindow.psiMax5pct = (std::max)(sWindow.psiMax5pct, static_cast<unsigned>(llclamp(systemMemory.psi_some10/5.f + .5f,0.f,20.f)));
        const auto allocation = LLMemoryGovernor::getAllocFailStats();
        auto delta = [](U64 value, U64 baseline) { return static_cast<std::uint32_t>((std::min)(value >= baseline ? value-baseline : U64(0), U64(4294967295))); };
        for (unsigned i=0; i<9; ++i)
        {
            sWindow.allocFailures[i]=delta(allocation.failures[i],sAllocBaseline.failures[i]);
            sWindow.allocRetried[i]=delta(allocation.retried[i],sAllocBaseline.retried[i]);
            sWindow.allocDropped[i]=delta(allocation.dropped[i],sAllocBaseline.dropped[i]);
        }
        auto cacheTotals = sCacheBaseline;
        if(const auto* cache=LLAppViewer::getTextureCache())
        {
            cacheTotals={cache->getStoreReadHits(),cache->getStoreReadMisses(),cache->getStoreReadErrors()};
            sWindow.cache={delta(cacheTotals[0],sCacheBaseline[0]),delta(cacheTotals[1],sCacheBaseline[1]),delta(cacheTotals[2],sCacheBaseline[2])};
        }
        const auto data=SLDataUsage::numericTotals();
        for(unsigned i=0;i<3;++i)sWindow.dataMBLog2[i]=logCount((data.bytes[i]>=sDataBaseline.bytes[i]?data.bytes[i]-sDataBaseline.bytes[i]:0)>>20);
        sampledBaselines={allocation,cacheTotals,data};
        sampledBaselinesReady=true;
        sWindow.sheds=delta(allocation.sheds,sAllocBaseline.sheds);
        sWindow.reserveReleased=delta(allocation.reserve_released,sAllocBaseline.reserve_released);
        sWindow.assetGauges[0].add((std::max)(0,gObjectList.getNumObjects()));
        sWindow.assetGauges[1].add((std::max)(0,gObjectList.getNumActiveObjects()));
        sWindow.assetGauges[2].add(LLCharacter::sInstances.size());
        sWindow.assetGauges[3].add((std::max)(0,LLVOAvatar::sNumVisibleAvatars));
        sWindow.assetGauges[4].add((std::max)(0,gTextureList.getNumImages()));
        if(const auto* fetch=LLAppViewer::getTextureFetch())
        {
            const auto& workers=fetch->getWorkerStats();
            const int values[]={workers.mTotal,workers.mInFlight,workers.mWaitRes,workers.mDecode,workers.mDecodeGov,workers.mDecodeOOM};
            for(unsigned i=0;i<6;++i)sWindow.loadingGauges[i].add((std::max)(0,values[i]));
        }
        if(auto* decoder=LLAppViewer::getImageDecodeThread())sWindow.loadingGauges[6].add(decoder->getPending());
        sWindow.loadingGauges[7].add((std::max)(0,LLMemoryGovernor::getWaiters()));
        sWindow.loadingGauges[8].add(LLMemoryGovernor::getInFlightBytes()>>20);
        sWindow.loadingGauges[9].add(LLMemoryGovernor::getBudgetBytes()>>20);
        sWindow.objects = (std::max)(sWindow.objects, logCount(gObjectList.getNumObjects()));
        sWindow.avatars = (std::max)(sWindow.avatars, logCount(LLCharacter::sInstances.size()));
        sWindow.textures = (std::max)(sWindow.textures, logCount(gTextureList.getNumImages()));
        if (auto* decoder = LLAppViewer::getImageDecodeThread())
            sWindow.decodePending = (std::max)(sWindow.decodePending, logCount(decoder->getPending()));
        sWindow.waiters = (std::max)(sWindow.waiters, logCount((std::max)(0, LLMemoryGovernor::getWaiters())));
        sWindow.inflightMB = (std::max)(sWindow.inflightMB, logCount(LLMemoryGovernor::getInFlightBytes() >> 20));
    }
    // Seal only after this frame's histogram and due 1 Hz counter deltas have
    // entered the old window. A refused disk handoff leaves it live. On success,
    // start the next window on the same elapsed-hour clock before any between-frame
    // event callback can run.
    if (!sNeedInitialize && sampledBaselinesReady && sWindow.elapsedHour <= 24)
    {
        if (sInitialSegment.tryHandoff(now, loggedIn, !gDisconnected, teleporting,
                settled, [&] { return schedule(DISK_ACTION_INITIAL, true, &sampledBaselines); }, settleBarrierClear))
            sCollectionClock.stamp(sWindow, now, sNextSecond);
    }
    const unsigned action = sNeedInitialize ? 3 : sWindow.seconds >= 3600 ? 2 : sWindow.seconds >= sLastCheckpoint + 600 ? 1 : 0;
    if (!sWorker && (action || !sFinished.empty() || secondsUntil(sNextSend) <= 0))
    {
        schedule(action, secondsUntil(sNextSend) <= 0);
        if (action == 1) sLastCheckpoint = sWindow.seconds;
    }
}
void shutdown()
{
    if (sAllowed.load() && sWindow.seconds >= 300 && !sWorker) schedule(4, false);
    sShutdown = true; // no shutdown network. The General worker retains only fixed captured data.
}
std::string preview()
{
    try
    {
        if (!sPendingPreview.empty() && sAllowed.load()) return sPendingPreview;
        auto hardware=snapshot();probeOS(hardware);
        return buildReport(sAllowed.load() ? sWindow : Window{}, hardware, sAllowed.load()?sIndex:0, "periodic", false,
                           LLStartUp::getStartupState() >= STATE_STARTED);
    }
    catch (...) { warning(); return {}; }
}
std::string lastSent() { return sLast; }
std::string lastSentDay() { return sLastDay; }
unsigned queuedCount() { return sQueued; }
void deleteQueued()
{
    // Explicit action while off is privacy cleanup only, never collection or creation.
    sWindow={};sQueued=0;sPendingPreview.clear();sFinished.clear();
    beginPurge(&savePrivacy); // Persist BEFORE posting asynchronous erasure.
    schedulePurge();
}
}
