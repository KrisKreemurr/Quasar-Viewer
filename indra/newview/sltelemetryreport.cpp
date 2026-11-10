/**
 * @file sltelemetryreport.cpp
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
#ifndef SL_TELEMETRY_STANDALONE
#include "llviewerprecompiledheaders.h"
#endif
#include "sltelemetryreport.h"
#include "sltelemetryschema.h"
#include <boost/json.hpp>
#include <openssl/rand.h>
#include <regex>

namespace SLTelemetry
{
namespace
{
namespace json = boost::json;
bool matches(const json::value& value, const json::object& rule)
{
    if (const auto* choices = rule.if_contains("enum"))
    {
        bool found = false;
        for (const auto& choice : choices->as_array()) if (choice == value) found = true;
        if (!found) return false;
    }
    const auto type = rule.at("type").as_string();
    if (type == "object")
    {
        if (!value.is_object()) return false;
        const auto& fields = rule.at("properties").as_object();
        const auto& data = value.as_object();
        for (const auto& required : rule.at("required").as_array())
            if (!data.contains(required.as_string())) return false;
        for (const auto& item : data)
        {
            const auto* child = fields.if_contains(item.key());
            if (!child || !matches(item.value(), child->as_object())) return false;
        }
    }
    else if (type == "integer")
    {
        if (!value.is_int64() && !value.is_uint64()) return false;
        const double number = value.is_int64() ? static_cast<double>(value.as_int64()) : static_cast<double>(value.as_uint64());
        if (number < json::value_to<double>(rule.at("minimum")) || number > json::value_to<double>(rule.at("maximum"))) return false;
    }
    else if (type == "number")
    {
        if (!value.is_number()) return false;
        const double number = json::value_to<double>(value);
        const double step = json::value_to<double>(rule.at("multipleOf"));
        if (!std::isfinite(number) || number < json::value_to<double>(rule.at("minimum")) ||
            number > json::value_to<double>(rule.at("maximum")) || std::abs(number/step-std::round(number/step)) > 1e-6) return false;
    }
    else if (type == "boolean") { if (!value.is_bool()) return false; }
    else if (type == "array")
    {
        if (!value.is_array()) return false;
        const auto& data = value.as_array();
        if (data.size() < json::value_to<std::size_t>(rule.at("minItems")) ||
            data.size() > json::value_to<std::size_t>(rule.at("maxItems"))) return false;
        for (const auto& item : data) if (!matches(item, rule.at("items").as_object())) return false;
    }
    else if (type == "string")
    {
        if (!value.is_string()) return false;
        const auto& data = value.as_string();
        if (const auto* choices = rule.if_contains("enum"))
        {
            bool found = false;
            for (const auto& choice : choices->as_array()) if (choice == value) found = true;
            if (!found) return false;
        }
        else
        {
            if (data.size() < json::value_to<std::size_t>(rule.at("minLength")) ||
                data.size() > json::value_to<std::size_t>(rule.at("maxLength"))) return false;
            const std::string pattern(rule.at("pattern").as_string());
            if (!std::regex_match(data.begin(), data.end(), std::regex(pattern))) return false;
        }
    }
    else return false;
    return true;
}
json::object histogram(const Histogram& sample, unsigned seconds)
{
    json::array bins;
    for (unsigned index = 0; index < sample.bins.size(); ++index)
        if (sample.bins[index]) bins.push_back(json::array{index, sample.bins[index]});
    auto rounded=[](double value){return std::round(value*10)/10;};
    const double p99=percentile(sample,.99), p999=percentile(sample,.999);
    return {{"seconds",seconds}, {"p", {{"p1",rounded(percentile(sample,.01))}, {"p5",rounded(percentile(sample,.05))},
              {"p25",rounded(percentile(sample,.25))}, {"p50",rounded(percentile(sample,.5))}, {"p75",rounded(percentile(sample,.75))},
              {"p95",rounded(percentile(sample,.95))}, {"p99",rounded(p99)}, {"p999",rounded(p999)}}},
            {"fps_low_1pct",p99>0?rounded((std::min)(2000.0,1000/p99)):0.0},
            {"fps_low_01pct",p999>0?rounded((std::min)(2000.0,1000/p999)):0.0}, {"x2_median",sample.x2Median},
            {"hist", std::move(bins)}, {"frames", sample.frames}, {"over_50ms", sample.over50},
            {"over_100ms", sample.over100}, {"over_250ms", sample.over250}, {"over_1s", sample.over1000}};
}
}
bool validateReport(const std::string& body) noexcept
{
    try
    {
        if (body.empty() || body.size() > BODY_LIMIT) return false;
        json::parse_options options; options.max_depth = 12;
        const auto data = json::parse(body, {}, options);
        if (json::serialize(data) != body) return false; // canonical bytes also reject duplicate keys
        static const auto schema = json::parse(SCHEMA_JSON);
        if (!matches(data, schema.as_object())) return false;
        for (const char* category : {"steady","loading","background","vr"})
        {
            const auto& metric=data.at("perf").at(category);
            std::uint64_t total = 0;
            int previous = -1;
            for (const auto& pair : metric.at("hist").as_array())
            {
                const auto& bin = pair.as_array();
                const auto index = json::value_to<int>(bin[0]);
                const auto count = json::value_to<std::uint32_t>(bin[1]);
                if (index <= previous || index > 65 || !count) return false;
                previous = index; total += count;
            }
            if (total != json::value_to<std::uint32_t>(metric.at("frames"))) return false;
            for (const char* field : {"over_50ms", "over_100ms", "over_250ms", "over_1s", "x2_median"})
                if (json::value_to<std::uint32_t>(metric.at(field)) > total) return false;
        }
        const auto& present=data.at("settings").as_object();
        const auto& missing=data.at("availability").at("settings_missing").as_array();
        unsigned absent=0;
        for(const auto& rule:SETTING_RULES)
        {
            unsigned count=0;
            for(const auto& name:missing)if(name.as_string()==rule.name)++count;
            if(present.contains(rule.name)?count!=0:count!=1)return false;
            if(count)++absent;
        }
        if(absent!=missing.size())return false;
        return true;
    }
    catch (...) { return false; }
}
std::string buildReport(const Window& window, const Snapshot& snapshot, unsigned index,
                        const char* kind, bool survey, bool loggedIn)
{
    // RAND failure drops the report; never fall back to viewer UUID generation.
    unsigned char random[16];
    if (RAND_bytes(random, sizeof(random)) != 1) return {};
    static constexpr char hex[] = "0123456789abcdef";
    std::string identifier(32, '0');
    for (unsigned i = 0; i < 16; ++i) { identifier[i*2] = hex[random[i] >> 4]; identifier[i*2+1] = hex[random[i] & 15]; }
    static constexpr const char* channels[] = {"release", "beta", "jetstream", "test", "other"};
    static constexpr const char* builds[] = {"Release", "RelWithDebInfo", "Debug", "other"};
    static constexpr const char* isas[] = {"AVX2", "SSE4_2", "NEON", "unknown"};
    static constexpr const char* systems[] = {"windows", "linux", "macos", "unknown"};
    static constexpr const char* vendors[] = {"nvidia", "amd", "intel", "apple", "other"};
    static constexpr const char* endings[] = {"unknown", "normal", "crash", "llerror_crash", "froze", "logout_crash", "out_of_memory", "device_lost"};
    const char* duration = window.seconds < 300 ? "<5" : window.seconds < 900 ? "5-15" :
                           window.seconds < 1800 ? "15-30" : window.seconds < 2700 ? "30-45" : "45-60";
    static constexpr const char* installs[] = {"windows_setup", "windows_zip", "flatpak", "deb", "aur", "tarball", "other"};
    static constexpr const char* resolutions[] = {"720p", "1080p", "1440p", "2160p", "uw1440", "uw1080", "5k", "8k", "other", "unknown"};
    static constexpr const char* modes[] = {"windowed", "maximized", "fullscreen_desktop", "fullscreen_exclusive", "unknown"};
    static const auto schema = json::parse(SCHEMA_JSON);
    const auto& rules = schema.at("properties");
    std::string cpuModel=snapshot.rawCPU[0]?snapshot.rawCPU.data():snapshot.cpu.data();
    if(cpuModel.size()<=256 && std::regex_match(cpuModel,std::regex("[A-Za-z0-9 ()@.,+_-]*")))
    {
        cpuModel=std::regex_replace(cpuModel,std::regex(R"(\(R\)|\(TM\))"),"");
        cpuModel=std::regex_replace(cpuModel,std::regex(" +")," ");
        if(!cpuModel.empty()&&cpuModel.front()==' ')cpuModel.erase(0,1);
        if(!cpuModel.empty()&&cpuModel.back()==' ')cpuModel.pop_back();
        cpuModel=std::regex_replace(cpuModel,std::regex(R"(( CPU)? @ [0-9]+(\.[0-9]+)?GHz$| CPU$| [0-9]{1,3}-Core Processor$)"),"");
    }
    else cpuModel="other";
    std::array<char,65> gpuModel{};
    normalizeModel(snapshot.rawGPU[0]?snapshot.rawGPU.data():snapshot.gpu.data(),gpuModel);
    const std::string cpu=matches(json::value(cpuModel),rules.at("cpu").at("properties").at("model").as_object())?cpuModel:"other";
    const std::string gpu=matches(gpuModel.data(),rules.at("gpu").at("properties").at("name").as_object())?gpuModel.data():"other";
    static constexpr const char* desktops[] = {"KDE","GNOME","Hyprland","sway","XFCE","Cinnamon","MATE","LXQt","Budgie","COSMIC","gamescope","other"};
    static constexpr const char* distros[] = {"arch","cachyos","endeavouros","manjaro","ubuntu","debian","linuxmint","pop","fedora","opensuse-tumbleweed","opensuse-leap","nixos","gentoo","void","steamos","bazzite","alpine","rocky","almalinux","rhel","centos","zorin","elementary","kali","mx","solus","garuda","artix","other"};
    static constexpr const char* sessions[] = {"wayland","x11","windows","other"};
    static constexpr const char* grids[] = {"main","beta","other"};
    auto counts = [](const auto& values) { json::array result; for (const auto value: values) result.push_back(value); return result; };
    json::object settings;
    json::array missing;
    for (unsigned i = 0; i < SETTING_COUNT; ++i)
    {
        const auto& rule = SETTING_RULES[i];
        if (!snapshot.hasSetting[i]) { missing.push_back(rule.name); continue; }
        if (rule.type == 0) settings[rule.name] = snapshot.settings[i] != 0;
        else if (rule.type == 1) settings[rule.name] = static_cast<std::int64_t>(snapshot.settings[i]);
        else settings[rule.name] = snapshot.settings[i];
    }
    auto steady=histogram(window.frames[0],window.classSeconds[0]);
    steady["jitter_p99"]=std::round(percentile(window.jitter,.99)*10)/10;
    steady["stutter_seconds"]=window.stutterSeconds;
    auto gauge=[](const Gauge& value){return json::object{{"p50",value.median()},{"max",value.peak}};};
    json::object data{
        {"schema", 1}, {"schema_minor", 1}, {"consent", CONSENT_VERSION}, {"report_id", identifier}, {"survey", survey},
        {"segment", {{"index", index}, {"kind", kind}, {"minutes_bucket", duration}, {"logged_in", loggedIn}}},
        {"viewer", {{"version", snapshot.version.data()}, {"channel", channels[snapshot.channel]},
                    {"build_config", builds[snapshot.build]}, {"isa", isas[snapshot.isa]}, {"install_kind", installs[snapshot.install]}}},
        {"os", {{"family", systems[snapshot.os]}, {"windows_build", snapshot.windowsBuild}, {"kernel", counts(snapshot.kernel)},
                {"glibc",counts(snapshot.glibc)}, {"distro",distros[snapshot.distro]}, {"distro_major",snapshot.distroMajor},
                {"session_type",sessions[snapshot.session]}, {"desktop",desktops[snapshot.desktop]}, {"flatpak",snapshot.flatpak}}},
        {"cpu", {{"model", cpu}, {"threads", snapshot.threads}}},
        {"memory_hw", {{"ram_gb", snapshot.ramGB}, {"swap_present", snapshot.swapPresent}, {"psi",snapshot.psi}}},
        {"gpu", {{"name", gpu}, {"vendor", vendors[snapshot.vendor]}, {"vram_gb", snapshot.vramGB}}},
        {"display", {{"resolution", resolutions[snapshot.resolution]}, {"refresh_hz", snapshot.refresh},
                     {"scale_quarters", snapshot.scaleQuarters}, {"hdr", snapshot.hdr}, {"monitors", snapshot.monitors},
                     {"window_mode", modes[snapshot.windowMode]}}},
        {"settings", std::move(settings)},
        {"features", {{"voice",snapshot.features[0]}, {"media_autoplay",snapshot.features[1]}, {"rlva",snapshot.features[2]},
                      {"discord",snapshot.features[3]}, {"ao",snapshot.features[4]}, {"external_editor_set",snapshot.features[5]}, {"autotune",snapshot.features[6]}}},
        {"grid",grids[snapshot.grid]},
        {"perf", {{"steady",std::move(steady)}, {"loading", histogram(window.frames[1],window.classSeconds[1])},
                  {"background", histogram(window.frames[2],window.classSeconds[2])}, {"vr", histogram(window.frames[3],window.classSeconds[3])},
                  {"gpu_ms",{{"p50",std::round(percentile(window.gpuTime,.5)*10)/10},{"p95",std::round(percentile(window.gpuTime,.95)*10)/10}}},
                  {"rig_gpu_ms_p50",std::round(percentile(window.rigTime,.5)*10)/10},{"bound",counts(window.boundSeconds)},
                  {"upscaler_s",counts(window.upscalerSeconds)}}},
        {"memory", {{"ram_state_s", json::array{window.ramSeconds[0], window.ramSeconds[1], window.ramSeconds[2]}},
                    {"rss_peak_half_gb", window.peakRSSHalfGB}, {"ram_avail_ratio_hist", counts(window.availableRatio)},
                    {"psi_some10_max_5pct",window.psiMax5pct}, {"alloc_failures",counts(window.allocFailures)},
                    {"alloc_retried",counts(window.allocRetried)}, {"alloc_dropped",counts(window.allocDropped)},
                    {"sheds",window.sheds}, {"reserve_released",window.reserveReleased},
                    {"vram",{{"usage_ratio_hist",counts(window.vramRatio)},{"usage_half_gb_max",window.vramHalfGB},
                             {"overhead_quarter_gb_max",window.overheadQuarterGB},{"ladder_s",counts(window.vramSeconds)},{"rung_max",window.rungMax}}}}},
        {"assets", {{"objects",gauge(window.assetGauges[0])},{"objects_active",gauge(window.assetGauges[1])},
                    {"avatars",gauge(window.assetGauges[2])},{"avatars_visible",gauge(window.assetGauges[3])},{"textures",gauge(window.assetGauges[4])}}},
        {"loading", {{"tex", {{"workers",gauge(window.loadingGauges[0])},{"http_inflight",gauge(window.loadingGauges[1])},
                        {"queued",gauge(window.loadingGauges[2])},{"decode",gauge(window.loadingGauges[3])},
                        {"decode_governor_wait",gauge(window.loadingGauges[4])},{"decode_oom",gauge(window.loadingGauges[5])},
                        {"decode_pending",gauge(window.loadingGauges[6])},{"cache",{{"hit",window.cache[0]},{"miss",window.cache[1]},{"error",window.cache[2]}}}}},
                     {"gov", {{"waiters",gauge(window.loadingGauges[7])},{"inflight_mb",gauge(window.loadingGauges[8])},
                              {"budget_mb",gauge(window.loadingGauges[9])}}}, {"data_mb_log2",counts(window.dataMBLog2)}}},
        {"availability", {{"display", snapshot.displayAvailable ? "available" : "unavailable"},
                          {"loading", "portable_subset"}, {"gpu_details", "unsupported"}, {"vr_identity", "unsupported"},
                          {"renderer_metrics", "portable_subset"}, {"os_details", "portable_subset"},
                          {"settings", "portable_subset"}, {"events", "portable_subset"}, {"settings_missing", std::move(missing)}}}, {"events", {{"teleports", window.teleports}, {"prev_session_end", endings[index == 0 ? snapshot.previousEnd : 0]},
                    {"region_crossings",window.regionCrossings},{"disconnects",window.disconnects},
                    {"teleport_duration_hist",counts(window.teleportDuration)}, {"login_s_bucket",index==0?window.loginBucket:7},
                    {"world_ready_s_bucket",index==0?window.readyBucket:7},{"long_frames",window.longFrames}}}
    };
    if (window.elapsedHour<=24) data.at("segment").as_object()["elapsed_hour"]=window.elapsedHour;
    auto result = json::serialize(data);
    return validateReport(result) ? result : std::string();
}
}
