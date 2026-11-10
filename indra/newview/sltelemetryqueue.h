/**
 * @file sltelemetryqueue.h
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
// Worker-only queue implementation, shared verbatim with offline race/recovery tests.
#pragma once
#include "sltelemetryreport.h"
#include <boost/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
namespace SLTelemetry { namespace QueueDetail {
namespace fs = std::filesystem;
inline std::atomic<std::uint64_t> sEpoch{0};
inline std::atomic<bool> sAllowed{false}, sPurge{false};
inline std::mutex sDiskMutex;
inline std::atomic<void(*)()> sWarning{nullptr};
inline void queueWarning() { if (const auto callback=sWarning.load()) callback(); }
// Pending revocation blocks every read/write/send, including after a restart.
inline bool current(std::uint64_t epoch) { return sAllowed.load() && !sPurge.load() && sEpoch.load() == epoch; }
inline std::atomic<std::uint64_t> sRemovedEpoch{0};
inline void restorePurge(bool pending)
{
    sPurge.store(pending);
    sRemovedEpoch.store(0);
    if (pending) sAllowed.store(false);
}
template<typename Save> bool beginPurge(Save save)
{
    sAllowed.store(false);
    sEpoch.fetch_add(1);
    sPurge.store(true);
    sRemovedEpoch.store(0);
    return save(true); // Failure keeps the in-memory gate closed.
}
template<typename Save> bool confirmPurge(std::uint64_t epoch, Save save)
{
    if (sEpoch.load()!=epoch || !sPurge.load() || sRemovedEpoch.load()!=epoch) return false;
    if (!save(false)) return false; // Durable pending stays set if saving fails.
    if (sEpoch.load()!=epoch) return false;
    sPurge.store(false);
    return true;
}
struct DiskResult
{
    std::string body, filename;
    unsigned count = 0;
};
inline std::string readBody(const fs::path& path)
{
    if (!fs::is_regular_file(fs::symlink_status(path)) || fs::file_size(path) > BODY_LIMIT) return {};
    std::ifstream stream(path, std::ios::binary);
    std::string body(BODY_LIMIT+1, '\0');
    stream.read(body.data(), static_cast<std::streamsize>(body.size()));
    body.resize(static_cast<std::size_t>(stream.gcount()));
    return validateReport(body) ? body : std::string();
}
#ifdef SL_TELEMETRY_STANDALONE
inline void (*beforePurge)() = nullptr;
#endif
inline bool purgeLocked(const fs::path& root, std::uint64_t epoch)
{
    if (!sPurge.load() || sEpoch.load()!=epoch) return false;
    sRemovedEpoch.store(0);
#ifdef SL_TELEMETRY_STANDALONE
    if (beforePurge) beforePurge();
#endif
    // remove_all throws on partial removal/failure. Never exchange the latch first.
    fs::remove_all(root);
    if (sEpoch.load()!=epoch || !sPurge.load()) return false;
    sRemovedEpoch.store(epoch);
    return true; // Main thread must durably acknowledge before lifting the gate.
}
#ifdef SL_TELEMETRY_STANDALONE
inline void (*beforeWrite)(const fs::path&) = nullptr;
#endif
inline void atomicWrite(const fs::path& path, const std::string& body, std::uint64_t epoch)
{
    if (!current(epoch)) return;
#ifdef SL_TELEMETRY_STANDALONE
    if(beforeWrite)beforeWrite(path);
#endif
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) throw std::runtime_error("disk");
        stream.write(body.data(), static_cast<std::streamsize>(body.size()));
        stream.close();
        if (!stream) throw std::runtime_error("disk");
    }
    fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write);
    if (!current(epoch)) { fs::remove(temporary); return; }
    // Windows rename does not replace existing files.
    fs::remove(path);
    fs::rename(temporary, path);
    if (!current(epoch)) fs::remove(path);
}
inline std::tm localCalendar()
{
    const auto now=std::time(nullptr);
    std::tm date{};
#ifdef _WIN32
    localtime_s(&date,&now);
#else
    localtime_r(&now,&date);
#endif
    return date;
}
inline unsigned monthNow()
{
    const auto date=localCalendar();
    return date.tm_year>0?static_cast<unsigned>((date.tm_year+1900)*12+date.tm_mon):0;
}
inline bool prune(const fs::path& directory)
{
    std::array<fs::directory_entry, QUEUE_LIMIT> kept;
    std::size_t count = 0, scanned = 0;
    static const std::regex filenameRule("[0-9a-f]{32}_[0-9]\\.json");
    for (const auto& entry : fs::directory_iterator(directory))
    {
        if (++scanned > 64) { fs::remove_all(directory); return false; }
        const auto filename = entry.path().filename().string();
        if (!fs::is_regular_file(entry.symlink_status()) || !std::regex_match(filename, filenameRule) ||
            fs::file_time_type::clock::now() - entry.last_write_time() >= std::chrono::hours(24*30) || readBody(entry.path()).empty())
        { fs::remove_all(entry.path()); continue; }
        if (count < QUEUE_LIMIT) { kept[count++] = entry; continue; }
        auto oldest = std::min_element(kept.begin(), kept.end(), [](const auto& left, const auto& right) { return left.last_write_time() < right.last_write_time(); });
        if (entry.last_write_time() > oldest->last_write_time()) { fs::remove(oldest->path()); *oldest = entry; }
        else fs::remove(entry.path());
    }
    return true;
}
inline void makeRoom(const fs::path& directory)
{
    fs::path oldest;
    fs::file_time_type oldestTime{};
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(directory))
    {
        ++count;
        if (oldest.empty() || entry.last_write_time() < oldestTime) { oldest = entry.path(); oldestTime = entry.last_write_time(); }
    }
    if (count >= QUEUE_LIMIT) fs::remove(oldest);
}
inline std::string identifier(const std::string& body)
{
    return std::string(boost::json::parse(body).at("report_id").as_string());
}
inline Snapshot sOSCache;
inline std::uint64_t sOSEpoch=0;
inline void copyOS(const Snapshot& source,Snapshot& target)
{
    target.install=source.install;target.kernel=source.kernel;target.glibc=source.glibc;
    target.distro=source.distro;target.distroMajor=source.distroMajor;target.desktop=source.desktop;
    target.session=source.session;target.flatpak=source.flatpak;target.windowsBuild=source.windowsBuild;
    target.swapPresent=source.swapPresent;target.psi=source.psi;
}
inline DiskResult diskWork(std::uint64_t epoch, const std::string& rootName, const Window& frozen,
                    Snapshot hardware, unsigned index, unsigned action, bool loggedIn,
                    bool wantSend, const std::string& finished, bool removeFinished, void (*probe)(Snapshot&)=nullptr)
{
    DiskResult result;
    try
    {
        std::lock_guard<std::mutex> guard(sDiskMutex);
        const fs::path root(rootName), queue = root / "queue";
        if (!current(epoch)) return result;
        if(probe && action)
        {
            if(sOSEpoch!=epoch){sOSCache=hardware;probe(sOSCache);sOSEpoch=epoch;}
            copyOS(sOSCache,hardware);
        }
        if (fs::is_symlink(fs::symlink_status(root)) || fs::is_symlink(fs::symlink_status(queue))) return result;
        if (!current(epoch)) return result;
        fs::create_directories(queue);
        fs::permissions(root, fs::perms::owner_all);
        fs::permissions(queue, fs::perms::owner_all);
        if (!current(epoch)) return result;
        if (!finished.empty())
        {
            if (removeFinished) fs::remove(queue / finished);
        }
        if (!prune(queue) || !current(epoch)) return result;
        const auto checkpoint = root / "checkpoint.json";
        if (action == 3 && fs::exists(checkpoint))
        {
            auto body = fs::file_time_type::clock::now() - fs::last_write_time(checkpoint) < std::chrono::hours(24*30) ? readBody(checkpoint) : std::string();
            if (!body.empty())
            {
                auto data = boost::json::parse(body);
                data.at("segment").as_object()["kind"] = "checkpoint";
                static constexpr const char* endings[]={"unknown","normal","crash","llerror_crash","froze","logout_crash","out_of_memory","device_lost"};
                data.at("events").as_object()["prev_session_end"]=endings[hardware.previousEnd<8?hardware.previousEnd:0];
                body = boost::json::serialize(data);
                if (validateReport(body) && current(epoch))
                {
                    makeRoom(queue);
                    atomicWrite(queue / (identifier(body) + "_0.json"), body, epoch);
                    if (data.at("survey").as_bool() && current(epoch)) atomicWrite(root / "survey_month", std::to_string(monthNow()), epoch);
                }
            }
            fs::remove(checkpoint);
        }
        if (action == 1 || action == 2 || action == 4 || action == DISK_ACTION_INITIAL)
        {
            unsigned surveyedMonth = 0;
            if (fs::exists(root / "survey_month") && fs::file_size(root / "survey_month") <= 32)
            { std::ifstream state(root / "survey_month"); state >> surveyedMonth; }
            const unsigned month = monthNow();
            const bool survey = month && surveyedMonth != month;
            const char* kind = action == 1 ? "checkpoint" : action == 4 ? "exit" :
                action == DISK_ACTION_INITIAL ? "initial" : "periodic";
            const auto body = buildReport(frozen, hardware, index, kind, survey, loggedIn);
            if (!body.empty() && current(epoch))
            {
                if (!prune(queue) || !current(epoch)) return result;
                if (action != 1) makeRoom(queue);
                atomicWrite(action == 1 ? checkpoint : queue / (identifier(body) + "_0.json"), body, epoch);
                if (action != 1) fs::remove(checkpoint);
                // Mark the survey month when a report is queued (checkpoint alone is not a survey).
                if (survey && action != 1 && current(epoch)) atomicWrite(root / "survey_month", std::to_string(month), epoch);
            }
        }
        if (!prune(queue)) return result;
        fs::path oldest;
        fs::file_time_type oldestTime{};
        for (const auto& entry : fs::directory_iterator(queue))
        {
            ++result.count;
            if (oldest.empty() || entry.last_write_time() < oldestTime) { oldest = entry.path(); oldestTime = entry.last_write_time(); }
        }
        if (wantSend && !oldest.empty() && current(epoch))
        {
            auto filename = oldest.filename().string();
            const unsigned attempts = filename[33] - '0';
            result.body = readBody(oldest);
            if (attempts == 9) fs::remove(oldest);
            else
            {
                filename[33] = static_cast<char>('0' + attempts + 1);
                fs::rename(oldest, queue / filename);
            }
            result.filename = filename;
        }
        if (!current(epoch)) return {};
    }
    catch (...) { queueWarning(); }
    return result;
}
}}
