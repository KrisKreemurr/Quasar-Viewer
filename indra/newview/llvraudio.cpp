/**
 * @file llvraudio.cpp
 * @brief VR (#1122): the viewer's audio follows the headset while a VR session runs (llvraudio.h).
 */

#include "llviewerprecompiledheaders.h"

#include "llvraudio.h"

#include "llaudiodevices.h"
#include "llaudioengine.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llvoicewebrtc.h"

#include "llvraudiopulse.h"
#include "lldir.h"
#include "llfile.h"
#include "vkgfx/llxrsession.h"

#include <boost/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <vector>

#if LL_WINDOWS
#include "llwin32headers.h"
#include <mmdeviceapi.h>
#include <propidl.h>
#endif

namespace
{
// ---- The matcher table ----
// Case-insensitive substrings of a device's name, in priority order (the first rule any device
// matches wins; among devices matching that rule, the first listed). Names as the audio
// backends report them:
//  - Linux (PipeWire / PulseAudio, OpenAL Soft and WebRTC see the same descriptions): an HDMI /
//    DisplayPort sink is "<GPU codec> Digital Stereo (HDMI n) [<EDID monitor name>]", e.g.
//    "GB202 High Definition Audio Controller Digital Stereo (HDMI) [M28U]" -- the Valve Index's
//    DisplayPort audio carries its EDID name ("Index HMD"). Its microphone is the USB device
//    "Valve VR Radio & HMD Mic" (" Mono" appended by PipeWire).
//  - Windows (endpoint friendly names): "<endpoint> (<device>)", e.g. "Headphones (Oculus
//    Virtual Audio Device)", "Microphone (Valve VR Radio & HMD Mic)", "Index HMD (NVIDIA High
//    Definition Audio)".
struct Rule
{
    const char* token;
    const char* label;
};

const Rule kOutputRules[] = {
    {"Index HMD", "Valve Index (DisplayPort audio, EDID name)"},
    {"Valve Index", "Valve Index"},
    {"Oculus Virtual Audio", "Meta Quest Link / Air Link"},
    {"Rift S", "Oculus Rift S"},
    {"Virtual Desktop Audio", "Virtual Desktop"},
    {"Steam Streaming Speakers", "Steam Link (streamed headset)"},
    {"ALVR", "ALVR"},
    {"Reverb G2", "HP Reverb G2"},
    {"Mixed Reality", "Windows Mixed Reality headset"},
    {"VIVE Pro", "HTC VIVE Pro"},
    {"VIVE Cosmos", "HTC VIVE Cosmos"},
    {"HTC Vive", "HTC Vive"},
    {"Bigscreen Beyond", "Bigscreen Beyond"},
    {"Valve", "Valve (generic)"},
};

const Rule kMicRules[] = {
    {"Valve VR Radio & HMD Mic", "Valve Index microphone (USB)"},
    {"Index HMD", "Valve Index"},
    {"Oculus Virtual Audio", "Meta Quest Link / Air Link"},
    {"Rift S", "Oculus Rift S"},
    {"Virtual Desktop Audio", "Virtual Desktop"},
    {"Steam Streaming Microphone", "Steam Link (streamed headset)"},
    {"ALVR", "ALVR"},
    {"Reverb G2", "HP Reverb G2"},
    {"Mixed Reality", "Windows Mixed Reality headset"},
    {"VIVE Pro", "HTC VIVE Pro"},
    {"VIVE Cosmos", "HTC VIVE Cosmos"},
    {"HTC Vive", "HTC Vive"},
    {"Bigscreen Beyond", "Bigscreen Beyond"},
};

bool containsNoCase(const std::string& hay, const char* needle)
{
    const std::string n(needle);
    auto it = std::search(hay.begin(), hay.end(), n.begin(), n.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != hay.end();
}

// PulseAudio / PipeWire list each output's monitor as a capture source: never a microphone.
bool isMonitorSource(const std::string& name)
{
    return name.rfind("Monitor of ", 0) == 0;
}

template <size_t N> const char* matchRules(const Rule (&rules)[N], const std::string& name)
{
    if (name.empty()) return nullptr;
    for (const Rule& r : rules)
    {
        if (containsNoCase(name, r.token)) return r.label;
    }
    return nullptr;
}

// The best match in a list (rule priority first, then list order): index, -1 = none.
template <size_t N, typename NameOf>
int bestMatch(const Rule (&rules)[N], size_t count, NameOf name_of, const char** label_out)
{
    for (const Rule& r : rules)
    {
        for (size_t i = 0; i < count; ++i)
        {
            const std::string name = name_of(i);
            if (!name.empty() && containsNoCase(name, r.token))
            {
                if (label_out) *label_out = r.label;
                return (int)i;
            }
        }
    }
    return -1;
}

bool isDefaultVoiceDevice(const std::string& id)
{
    return id.empty() || id == "Default";
}

// WebRTC names an HDMI / DP output without the EDID monitor name OpenAL Soft appends ("... (HDMI 4)"
// vs "... (HDMI 4) [Index HMD]"): a voice output equal to an OpenAL name minus its " [...]" suffix
// gets that OpenAL name added, so the headset rules see the EDID name.
std::string withEdidName(const std::string& voice_name, const std::vector<std::string>& openal_names)
{
    for (const std::string& n : openal_names)
    {
        const size_t br = n.rfind(" [");
        if (br != std::string::npos && n.back() == ']' && n.compare(0, br, voice_name) == 0 && br == voice_name.size())
            return voice_name + " | " + n;
    }
    return voice_name;
}

constexpr F64 kOutputRetrySec = 5.0;    // headset output not found yet: look again this often
constexpr F64 kOutputRetryForSec = 60.0;    // ... for this long after the session started

// ---- State (main thread only) ----
bool s_running = false;
F64  s_sessionStart = 0.0;
// World audio (OpenAL).
bool s_outputDecided = false;   // switched, already the default, unavailable, or gave up
bool s_outputSwitched = false;  // we moved OpenAL: restore it on the session's end
F64  s_outputNextTry = 0.0;
bool s_engineSeen = false;      // the audio engine existed for a try this session
bool s_outputOnHeadset = false; // world audio plays on the headset (the gain applies)
bool s_outputLimiter = false;   // ... and was reopened with the output limiter (gain > 0 dB)
F32  s_appliedBoost = 1.f;      // the output boost set on the engine
// Linux: the headset's card port found through PulseAudio / PipeWire (llvraudiopulse.h); its
// sink's description is then the headset output's exact name for OpenAL and voice.
LLVRAudioPulse::Headset s_pulse;
// Voice output: WebRTC re-lists devices only on its own events (a card profile switch is not
// one), so while no headset output was found the list is refreshed on the output's schedule.
bool s_voiceOutFound = false;
F64  s_voiceNextRefresh = 0.0;
// Voice.
bool s_userCapture = false;     // the user picked a voice device during this session
bool s_userRender = false;
std::string s_micName, s_micId;
std::string s_voiceLogged;      // the last voice decision logged (no repeats on list refreshes)
bool s_voiceOnHeadset = false;  // voice output plays on the headset (the gain applies)

// ---- The preferred devices (found once per session start, before the name rules) ----
// A device named for the session by, in this order:
//  1. the user (SlipstreamVRAudioOutputDevice / SlipstreamVRAudioInputDevice, "" = Automatic):
//     used when connected, else NOTHING moves (logged) -- never another device instead;
//  2. the OpenXR runtime (Windows, XR_OCULUS_audio_device_guid: the endpoint id);
//  3. SteamVR's own audio settings (steamvr.vrsettings "audio": the playback / recording override
//     when enabled, else the HMD playback device it last saw). SteamVR on Linux writes none of
//     these (it leaves audio alone there): Linux finds the headset by its card port instead.
// 2 and 3 are hints: not listed -> the PulseAudio port / name rules.
struct Preferred
{
    std::string id;      // a device id (Windows: WASAPI endpoint id = WebRTC's guid), "" = none
    std::string name;    // a display name, "" = none
    std::string source;  // for the log
    bool user = false;   // the user's own choice (1)

    bool empty() const { return id.empty() && name.empty(); }
};
Preferred s_prefOut, s_prefIn;

#if LL_WINDOWS
// An endpoint id's friendly name ("Headphones (Oculus Virtual Audio Device)": the name OpenAL Soft
// and WebRTC list). "" = unknown.
std::string endpointFriendlyName(const std::string& id)
{
    if (id.empty()) return std::string();
    const int wn = MultiByteToWideChar(CP_UTF8, 0, id.c_str(), -1, nullptr, 0);
    if (wn <= 1) return std::string();
    std::wstring wid((size_t)wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, id.c_str(), -1, &wid[0], wn);
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::string out;
    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)) && en)
    {
        IMMDevice* dev = nullptr;
        if (SUCCEEDED(en->GetDevice(wid.c_str(), &dev)) && dev)
        {
            IPropertyStore* props = nullptr;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props)
            {
                // PKEY_Device_FriendlyName (functiondiscoverykeys_devpkey.h), spelled out: no
                // INITGUID dance for one key.
                static const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
                {
                    const int n = WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, nullptr, 0, nullptr, nullptr);
                    if (n > 1)
                    {
                        out.assign((size_t)n, '\0');
                        WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, &out[0], n, nullptr, nullptr);
                        out.resize((size_t)n - 1);
                    }
                }
                PropVariantClear(&v);
                props->Release();
            }
            dev->Release();
        }
        en->Release();
    }
    if (init == S_OK || init == S_FALSE) CoUninitialize();
    return out;
}
#endif

std::string readTextFile(const std::string& path)
{
    if (path.empty() || !LLFile::isfile(path)) return std::string();
    llifstream f(path.c_str());
    if (!f) return std::string();
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

const boost::json::object* jsonObject(const boost::json::value& v, const char* key)
{
    const boost::json::object* o = v.if_object();
    if (!o) return nullptr;
    const boost::json::value* c = o->if_contains(key);
    return c ? c->if_object() : nullptr;
}

std::string jsonString(const boost::json::object& o, const char* key)
{
    const boost::json::value* v = o.if_contains(key);
    const boost::json::string* s = v ? v->if_string() : nullptr;
    return s ? std::string(s->c_str()) : std::string();
}

bool jsonBool(const boost::json::object& o, const char* key)
{
    const boost::json::value* v = o.if_contains(key);
    return v && v->is_bool() && v->get_bool();
}

// SteamVR's audio settings (3. above). Only asked when the runtime is SteamVR.
void steamVrAudio(Preferred& out, Preferred& in)
{
    // openvrpaths.vrpath (vrpathreg) names SteamVR's config folder.
    std::string vrpath;
#if LL_WINDOWS
    if (const char* la = std::getenv("LOCALAPPDATA")) vrpath = std::string(la) + "\\openvr\\openvrpaths.vrpath";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        vrpath = std::string(xdg) + "/openvr/openvrpaths.vrpath";
    else if (const char* home = std::getenv("HOME"))
        vrpath = std::string(home) + "/.config/openvr/openvrpaths.vrpath";
#endif
    boost::system::error_code ec;
    const boost::json::value paths = boost::json::parse(readTextFile(vrpath), ec);
    std::string config;
    if (!ec)
    {
        if (const boost::json::object* o = paths.if_object())
            if (const boost::json::value* c = o->if_contains("config"))
                if (const boost::json::array* a = c->if_array(); a && !a->empty() && a->front().is_string())
                    config = std::string(a->front().get_string().c_str());
    }
    if (config.empty())
    {
        LL_INFOS("VRAudio") << "SteamVR audio settings: no config folder in '" << vrpath << "'" << LL_ENDL;
        return;
    }
    const std::string settings_path = config + gDirUtilp->getDirDelimiter() + "steamvr.vrsettings";
    const boost::json::value settings = boost::json::parse(readTextFile(settings_path), ec);
    const boost::json::object* audio = ec ? nullptr : jsonObject(settings, "audio");
    if (!audio)
    {
        LL_INFOS("VRAudio") << "SteamVR audio settings: none in '" << settings_path << "'" << LL_ENDL;
        return;
    }
    // (Keys: openvr.h k_pch_audio_*.)
    if (jsonBool(*audio, "enablePlaybackDeviceOverride"))
    {
        out.id = jsonString(*audio, "playbackDeviceOverride");
        out.name = jsonString(*audio, "playbackDeviceOverrideName");
        out.source = "SteamVR's audio output override";
    }
    else
    {
        out.id = jsonString(*audio, "lastHmdPlaybackDeviceId");
        out.source = "SteamVR's headset playback device";
    }
    if (jsonBool(*audio, "enableRecordingDeviceOverride"))
    {
        in.id = jsonString(*audio, "recordingDeviceOverride");
        in.name = jsonString(*audio, "recordingDeviceOverrideName");
        in.source = "SteamVR's microphone override";
    }
    LL_INFOS("VRAudio") << "SteamVR audio settings ('" << settings_path << "'): output id '" << out.id << "' name '" << out.name
                        << "', microphone id '" << in.id << "' name '" << in.name << "'" << LL_ENDL;
}

void findPreferred()
{
    s_prefOut = Preferred();
    s_prefIn = Preferred();
    // 1. The user's choice.
    const std::string user_out = gSavedSettings.getString("SlipstreamVRAudioOutputDevice");
    const std::string user_in = gSavedSettings.getString("SlipstreamVRAudioInputDevice");
    if (!user_out.empty())
    {
        s_prefOut.name = user_out;
        s_prefOut.source = "your VR headset output choice";
        s_prefOut.user = true;
    }
    if (!user_in.empty())
    {
        s_prefIn.name = user_in;
        s_prefIn.source = "your VR headset microphone choice";
        s_prefIn.user = true;
    }
    LL_INFOS("VRAudio") << "VR headset output choice '" << (user_out.empty() ? "Automatic" : user_out) << "', microphone choice '"
                        << (user_in.empty() ? "Automatic" : user_in) << "'" << LL_ENDL;
    if (s_prefOut.user && s_prefIn.user) return;

    Preferred rt_out, rt_in;
    // 2. The OpenXR runtime (Windows).
    if (LLXrSession::active())
    {
        const LLXrSession& xr = LLXrSession::instance();
        rt_out.id = xr.headsetAudioOutputId();
        rt_in.id = xr.headsetAudioInputId();
        if (!rt_out.id.empty()) rt_out.source = "the OpenXR runtime (XR_OCULUS_audio_device_guid)";
        if (!rt_in.id.empty()) rt_in.source = "the OpenXR runtime (XR_OCULUS_audio_device_guid)";
        // 3. SteamVR's settings.
        if ((rt_out.empty() || rt_in.empty()) && xr.runtimeName().find("SteamVR") != std::string::npos)
        {
            Preferred sv_out, sv_in;
            steamVrAudio(sv_out, sv_in);
            if (rt_out.empty()) rt_out = sv_out;
            if (rt_in.empty()) rt_in = sv_in;
        }
    }
#if LL_WINDOWS
    if (rt_out.name.empty()) rt_out.name = endpointFriendlyName(rt_out.id);
    if (rt_in.name.empty()) rt_in.name = endpointFriendlyName(rt_in.id);
#endif
    if (!s_prefOut.user) s_prefOut = rt_out;
    if (!s_prefIn.user) s_prefIn = rt_in;
    if (!rt_out.empty() || !rt_in.empty())
        LL_INFOS("VRAudio") << "runtime-reported headset devices: output id '" << rt_out.id << "' name '" << rt_out.name << "' ("
                            << rt_out.source << "), microphone id '" << rt_in.id << "' name '" << rt_in.name << "' ("
                            << rt_in.source << ")" << LL_ENDL;
}

// A preferred device among `names` (ids: the matching ids, "" where unknown): index, -1 = none.
int findPreferredIn(const Preferred& p, const std::vector<std::string>& names, const std::vector<std::string>& ids)
{
    if (!p.id.empty())
        for (size_t i = 0; i < ids.size() && i < names.size(); ++i)
            if (ids[i] == p.id) return (int)i;
    if (p.name.empty()) return -1;
    const int strict = LLAudioDevices::findDevice(names, p.name, true);
    return strict >= 0 ? strict : LLAudioDevices::findDevice(names, p.name, false);
}

// The headset gain (SlipstreamVRAudioGainDb), clamped: -20..+12 dB.
F32 headsetGainDb()
{
    static LLCachedControl<F32> s_db(gSavedSettings, "SlipstreamVRAudioGainDb", 6.f);
    return llclamp((F32)s_db, -20.f, 12.f);
}

F32 headsetGainLinear()
{
    return std::pow(10.f, headsetGainDb() / 20.f);
}

bool wantLimiter()
{
    return headsetGainDb() > 0.05f;
}

// World audio's output boost: the headset gain while world audio is on the headset, else 1.
void applyOutputBoost()
{
    const F32 boost = (s_running && s_outputOnHeadset) ? headsetGainLinear() : 1.f;
    if (!gAudiop || std::fabs(boost - s_appliedBoost) < 0.0005f) return;
    s_appliedBoost = boost;
    gAudiop->setOutputBoost(boost);
    LL_INFOS("VRAudio") << "world audio: headset gain " << (boost == 1.f ? 0.f : headsetGainDb()) << " dB (x" << boost << ")"
                        << LL_ENDL;
}

LLVRAudio::changed_signal_t& changedSignal()
{
    static LLVRAudio::changed_signal_t s_signal;
    return s_signal;
}

void logOutputList(const std::vector<std::string>& names)
{
    LL_INFOS("VRAudio") << "world audio (OpenAL) output devices: " << names.size() << LL_ENDL;
    for (const std::string& n : names)
    {
        const char* rule = LLVRAudio::matchHeadsetOutput(n);
        LL_INFOS("VRAudio") << "  '" << n << "'" << (rule ? std::string(" <- headset: ") + rule : std::string()) << LL_ENDL;
    }
}

// World audio: decided once per session (retried while the headset output is not listed yet;
// the device list is logged on the first and the last try).
void resolveOutput(bool first_try, bool last_try)
{
    if (!gAudiop)
    {
        // The session can start on the login screen, before the audio engine exists: wait for it
        // (LLVRAudio::update starts the retry window when it appears).
        if (first_try)
            LL_INFOS("VRAudio") << "world audio: no audio engine yet; moving it when it starts" << LL_ENDL;
        return;
    }
    // Linux: PulseAudio found the headset's port but its sink was not listed yet: look again.
    if (!first_try && !s_pulse.card.empty() && s_pulse.sink_desc.empty()) LLVRAudioPulse::refreshSink(s_pulse);
    // Linux: no port named the headset at the start -- its EDID name shows on the port only once
    // its display link is up, which can trail the session's start: look at the ports again
    // (short deadline, quiet; at most once per retry).
    if (!first_try && s_pulse.card.empty() && s_pulse.server && !s_prefOut.user)
    {
        LLVRAudioPulse::Headset h;
        LLVRAudioPulse::findHeadsetOutput(&LLVRAudio::matchHeadsetOutput, h, true);
        if (!h.card.empty())
        {
            s_pulse = h;
            if (!s_pulse.profile_set.empty() && LLWebRTCVoiceClient::instanceExists())
                LLWebRTCVoiceClient::instance().refreshDeviceLists(false);   // (the new sink, for voice)
        }
        else
        {
            s_pulse.server = h.server;
        }
    }

    const std::vector<std::string> names = gAudiop->listOutputDevices();
    const std::string def = gAudiop->defaultOutputDevice();
    if (first_try || last_try)
    {
        logOutputList(names);
        LL_INFOS("VRAudio") << "world audio: system default output '" << def << "', engine on '"
                            << (gAudiop->currentOutputDevice().empty() ? "<system default>" : gAudiop->currentOutputDevice())
                            << "'" << LL_ENDL;
    }

    std::string target;
    const char* label = nullptr;
    // 0. The preferred device (the user's VR output choice, the runtime's / SteamVR's report).
    if (!s_prefOut.empty())
    {
        // (OpenAL has no ids. Windows: WebRTC's guid is the endpoint id, its name OpenAL's.)
        Preferred p = s_prefOut;
        if (p.name.empty() && !p.id.empty() && LLWebRTCVoiceClient::instanceExists())
            for (const LLVoiceDevice& d : LLWebRTCVoiceClient::instance().getRenderDevices())
                if (d.full_name == p.id) p.name = d.display_name;
        const int idx = findPreferredIn(p, names, std::vector<std::string>());
        if (idx >= 0)
        {
            target = names[idx];
            label = s_prefOut.source.c_str();
        }
        else if (s_prefOut.user)
        {
            // Never another device instead of the one the user named.
            if (last_try)
            {
                LL_INFOS("VRAudio") << "world audio: " << s_prefOut.source << " '" << s_prefOut.name
                                    << "' is not connected; world audio stays on the desktop output" << LL_ENDL;
                s_outputDecided = true;
            }
            else if (first_try)
            {
                LL_INFOS("VRAudio") << "world audio: " << s_prefOut.source << " '" << s_prefOut.name
                                    << "' is not listed yet; looking again every " << kOutputRetrySec << " s for "
                                    << kOutputRetryForSec << " s" << LL_ENDL;
            }
            return;
        }
        else if (first_try)
        {
            LL_INFOS("VRAudio") << "world audio: " << s_prefOut.source << " names id '" << p.id << "' / '" << p.name
                                << "', which OpenAL does not list; trying the headset rules" << LL_ENDL;
        }
    }
    // 1. The sink PulseAudio found by the card port's EDID name (exactly its description).
    if (target.empty() && !s_pulse.sink_desc.empty())
    {
        const int idx = LLAudioDevices::findDevice(names, s_pulse.sink_desc, true);
        if (idx >= 0)
        {
            target = names[idx];
            label = "its card port, via PulseAudio";
        }
        else if (!last_try)
        {
            // (OpenAL lists a new sink a moment after it appears: wait for it rather than settle
            // for a name rule.)
            if (first_try)
                LL_INFOS("VRAudio") << "world audio: waiting for OpenAL to list the headset sink '" << s_pulse.sink_desc << "'"
                                    << LL_ENDL;
            return;
        }
    }
    else if (target.empty() && !s_pulse.card.empty() && !last_try)
    {
        return;   // (the port was found; its sink is not there yet: refreshSink above retries)
    }
    // 2. The name rules (Windows endpoint names; Linux sinks that carry the EDID name).
    if (target.empty())
    {
        const int idx = bestMatch(kOutputRules, names.size(), [&names](size_t i) { return names[i]; }, &label);
        if (idx >= 0) target = names[idx];
    }
    // 3. The system default is the headset (SteamVR's "switch audio device") but not listed as such.
    if (target.empty() && gAudiop->currentOutputDevice().empty())
    {
        label = LLVRAudio::matchHeadsetOutput(def);
        if (label) target = def;
    }
    if (target.empty())
    {
        if (last_try)
        {
            LL_INFOS("VRAudio") << "world audio: no headset output among " << names.size()
                                << " devices; world audio stays on the desktop output. (Linux: the headset's "
                                   "DisplayPort sink must be enabled in the sound card's profile.)"
                                << LL_ENDL;
            s_outputDecided = true;
        }
        else if (first_try)
        {
            LL_INFOS("VRAudio") << "world audio: no headset output among " << names.size() << " devices yet; looking again every "
                                << kOutputRetrySec << " s for " << kOutputRetryForSec << " s" << LL_ENDL;
        }
        return;
    }
    // Moved there by name even when it already plays there (the system default, or the user's
    // chosen output): the headset gain needs the output limiter, and the session's end then
    // puts world audio back on the user's choice.
    const bool limiter = wantLimiter();
    LL_INFOS("VRAudio") << "world audio: headset output '" << target << "' (" << label << "); moving world audio there"
                        << (limiter ? " with the output limiter" : "") << LL_ENDL;
    s_outputSwitched = gAudiop->switchOutputDevice(target, limiter);
    s_outputOnHeadset = s_outputSwitched;
    s_outputLimiter = s_outputSwitched && limiter;
    if (!s_outputSwitched)
    {
        LL_WARNS("VRAudio") << "world audio: could not move to the headset; it stays on the desktop output" << LL_ENDL;
    }
    s_outputDecided = true;
    applyOutputBoost();
}

void restoreOutput()
{
    s_outputOnHeadset = false;
    s_outputLimiter = false;
    applyOutputBoost();   // (back to 1)
    if (!s_outputSwitched) return;
    s_outputSwitched = false;
    if (!gAudiop) return;
    // Back to the user's output choice (Sound preferences; "" = the system default, as opened).
    const std::string user = LLAudioDevices::worldOutputDevice();
    LL_INFOS("VRAudio") << "world audio: back to " << (user.empty() ? std::string("the system default output") : "'" + user + "' (the chosen output device)")
                        << LL_ENDL;
    if (!gAudiop->switchOutputDevice(user))
    {
        LL_WARNS("VRAudio") << "world audio: could not go back; it stays on '" << gAudiop->currentOutputDevice() << "'" << LL_ENDL;
    }
}

// Voice: the override for one direction. "" = none needed / possible. Logs into `why`.
std::string voiceOverrideFor(bool capture, const LLVoiceDeviceList& devices, const std::string& saved,
                             const std::string& system_default, std::string& why, std::string* found_name,
                             std::string* found_id)
{
    const char* what = capture ? "input" : "output";
    if (capture ? s_userCapture : s_userRender)
    {
        why += std::string(" ") + what + ": the user's pick (no override);";
        // Still look the mic up for the dictation hook.
    }
    const char* label = nullptr;
    if (!capture) s_voiceOnHeadset = false;
    // The preferred device first (the user's VR choice; the runtime's / SteamVR's report).
    const Preferred& pref = capture ? s_prefIn : s_prefOut;
    int pref_idx = -1;
    if (!pref.empty())
    {
        std::vector<std::string> shown, ids;
        for (const LLVoiceDevice& d : devices)
        {
            shown.push_back((capture && isMonitorSource(d.display_name)) ? std::string() : d.display_name);
            ids.push_back(d.full_name);
        }
        pref_idx = findPreferredIn(pref, shown, ids);
        if (pref_idx >= 0)
        {
            label = pref.source.c_str();
        }
        else if (pref.user)
        {
            // Never another device instead of the one the user named.
            why += std::string(" ") + what + ": " + pref.source + " '" + pref.name + "' is not connected (no override);";
            return std::string();
        }
    }
    // Output: the sink PulseAudio found by the headset's card port first (Linux).
    int pulse_idx = -1;
    if (pref_idx < 0 && !capture && !s_pulse.sink_desc.empty())
    {
        std::vector<std::string> shown;
        for (const LLVoiceDevice& d : devices) shown.push_back(d.display_name);
        pulse_idx = LLAudioDevices::findDevice(shown, s_pulse.sink_desc, true);
        if (pulse_idx >= 0) label = "its card port, via PulseAudio";
    }
    const std::vector<std::string> openal = (!capture && gAudiop && pulse_idx < 0 && pref_idx < 0) ? gAudiop->listOutputDevices() : std::vector<std::string>();
    const int idx = pref_idx >= 0 ? pref_idx : pulse_idx >= 0 ? pulse_idx : capture ? bestMatch(kMicRules, devices.size(),
                                        [&devices](size_t i) {
                                            const LLVoiceDevice& d = devices[i];
                                            return isMonitorSource(d.display_name) ? std::string() : d.display_name + " | " + d.full_name;
                                        },
                                        &label)
                            : bestMatch(kOutputRules, devices.size(),
                                        [&devices, &openal](size_t i) {
                                            return withEdidName(devices[i].display_name, openal) + " | " + devices[i].full_name;
                                        },
                                        &label);
    if (idx < 0)
    {
        why += std::string(" ") + what + ": no headset device among " + std::to_string(devices.size()) + ";";
        return std::string();
    }
    if (!capture) s_voiceOutFound = true;
    const LLVoiceDevice& dev = devices[idx];
    if (found_name) *found_name = dev.display_name;
    if (found_id) *found_id = dev.full_name;
    if (capture ? s_userCapture : s_userRender) return std::string();
    // (Every path below leaves voice output on the headset: already there, or the override.)
    if (!capture) s_voiceOnHeadset = true;
    if (saved == dev.full_name)
    {
        why += std::string(" ") + what + ": the saved device already is the headset's '" + dev.display_name + "';";
        return std::string();
    }
    if (isDefaultVoiceDevice(saved))
    {
        // (A preferred device: the default must be that very device, not just any headset.)
        const bool def_is_it = pref_idx >= 0
            ? LLAudioDevices::sameDevice(system_default, dev.display_name)
            : (capture ? LLVRAudio::matchHeadsetMic(system_default) : LLVRAudio::matchHeadsetOutput(system_default)) != nullptr;
        if (def_is_it)
        {
            why += std::string(" ") + what + ": the system default already is the headset ('" + system_default + "');";
            return std::string();
        }
    }
    why += std::string(" ") + what + ": headset '" + dev.display_name + "' (" + label + ");";
    return dev.full_name;
}

void resolveVoice()
{
    if (!LLWebRTCVoiceClient::instanceExists()) return;
    LLWebRTCVoiceClient& voice = LLWebRTCVoiceClient::instance();
    const std::string def_out = gAudiop ? gAudiop->defaultOutputDevice() : std::string();
    const std::string def_in = gAudiop ? gAudiop->defaultInputDevice() : std::string();
    std::string why;
    std::string mic_name, mic_id;
    // ("saved" = the user's device without the override: the voice-only pick, else the main
    // device choice of Sound preferences, else "Default".)
    const std::string capture = voiceOverrideFor(true, voice.getCaptureDevices(), voice.userCaptureDevice(), def_in, why,
                                                 &mic_name, &mic_id);
    const std::string render = voiceOverrideFor(false, voice.getRenderDevices(), voice.userRenderDevice(), def_out, why,
                                                nullptr, nullptr);
    if (why != s_voiceLogged)
    {
        s_voiceLogged = why;
        LL_INFOS("VRAudio") << "voice (" << voice.getCaptureDevices().size() << " capture, " << voice.getRenderDevices().size()
                            << " render devices; system default input '" << def_in << "', output '" << def_out << "'):" << why
                            << LL_ENDL;
    }
    voice.setDeviceOverride(capture, render);
    if (mic_name != s_micName || mic_id != s_micId)
    {
        s_micName = mic_name;
        s_micId = mic_id;
        LL_INFOS("VRAudio") << "headset microphone (for dictation): "
                            << (s_micName.empty() ? std::string("none") : "'" + s_micName + "'") << LL_ENDL;
        changedSignal()();
    }
}

void sessionStarted()
{
    LL_INFOS("VRAudio") << "VR session running: moving the viewer's audio to the headset" << LL_ENDL;
    s_sessionStart = LLTimer::getTotalSeconds();
    s_outputDecided = s_outputSwitched = false;
    s_userCapture = s_userRender = false;
    s_voiceLogged.clear();
    s_engineSeen = gAudiop != nullptr;
    s_voiceOutFound = false;
    s_voiceOnHeadset = false;
    s_outputOnHeadset = s_outputLimiter = false;
    s_voiceNextRefresh = s_sessionStart + kOutputRetrySec;
    // The devices the user, the runtime or SteamVR name (see Preferred).
    findPreferred();
    // Linux: the headset's output port by its EDID name among the sound cards' ports (switches
    // the card's profile to it when needed; bounded, logged). Elsewhere / not found: the name rules.
    // Not when the user named the VR output (no card is switched for a device nobody asked for).
    s_pulse = LLVRAudioPulse::Headset();
    if (!s_prefOut.user) LLVRAudioPulse::findHeadsetOutput(&LLVRAudio::matchHeadsetOutput, s_pulse);
    if (!s_pulse.profile_set.empty() && LLWebRTCVoiceClient::instanceExists())
    {
        // (A profile switch is no device event for WebRTC: have it list the new sink now.)
        LLWebRTCVoiceClient::instance().refreshDeviceLists(false);
    }
    resolveOutput(true, false);
    s_outputNextTry = s_sessionStart + kOutputRetrySec;
    resolveVoice();
    changedSignal()();
}

void sessionEnded()
{
    LL_INFOS("VRAudio") << "VR session stopped: the viewer's audio goes back to the desktop devices" << LL_ENDL;
    restoreOutput();
    if (LLWebRTCVoiceClient::instanceExists())
    {
        LLWebRTCVoiceClient::instance().setDeviceOverride(std::string(), std::string());
    }
    // The sound card back to the profile it had (after world audio and voice left its sink).
    LLVRAudioPulse::restore(s_pulse);
    s_pulse = LLVRAudioPulse::Headset();
    s_prefOut = Preferred();
    s_prefIn = Preferred();
    s_outputDecided = false;
    s_engineSeen = false;
    s_voiceOutFound = false;
    s_voiceOnHeadset = false;
    s_userCapture = s_userRender = false;
    s_micName.clear();
    s_micId.clear();
    s_voiceLogged.clear();
    changedSignal()();
}
} // namespace

// static
void LLVRAudio::update(bool running)
{
    if (running == s_running)
    {
        if (running && s_outputOnHeadset && gAudiop)
        {
            // The headset gain is live: the limiter follows its sign (one reopen when it
            // crosses 0 dB), the boost its value.
            const bool limiter = wantLimiter();
            if (limiter != s_outputLimiter)
            {
                s_outputLimiter = limiter;
                gAudiop->switchOutputDevice(gAudiop->currentOutputDevice(), limiter);
            }
            applyOutputBoost();
        }
        if (running && !s_voiceOutFound && !s_userRender && LLWebRTCVoiceClient::instanceExists())
        {
            const F64 now = LLTimer::getTotalSeconds();
            if (now >= s_voiceNextRefresh && now - s_sessionStart < kOutputRetryForSec)
            {
                s_voiceNextRefresh = now + kOutputRetrySec;
                LLWebRTCVoiceClient::instance().refreshDeviceLists(false);   // (-> onVoiceDevicesChanged)
            }
        }
        if (running && !s_outputDecided)
        {
            const F64 now = LLTimer::getTotalSeconds();
            if (!gAudiop) return;   // (no engine yet: the window starts when it does)
            if (!s_engineSeen)
            {
                // The engine came up during the session: a fresh first try and retry window.
                s_engineSeen = true;
                s_sessionStart = now;
                s_outputNextTry = now + kOutputRetrySec;
                resolveOutput(true, false);
            }
            else if (now >= s_outputNextTry)
            {
                s_outputNextTry = now + kOutputRetrySec;
                resolveOutput(false, now - s_sessionStart >= kOutputRetryForSec);
            }
        }
        return;
    }
    s_running = running;
    if (running)
        sessionStarted();
    else
        sessionEnded();
}

// static
void LLVRAudio::onVoiceDevicesChanged()
{
    if (!s_running) return;
    resolveVoice();
}

// static
void LLVRAudio::userPickedVoiceDevice(bool capture)
{
    if (!s_running) return;
    bool& flag = capture ? s_userCapture : s_userRender;
    if (flag) return;
    flag = true;
    if (!capture) s_voiceOnHeadset = false;
    LL_INFOS("VRAudio") << "voice: the user picked a voice " << (capture ? "input" : "output")
                        << " device; the headset override for it ends for this session" << LL_ENDL;
    if (LLWebRTCVoiceClient::instanceExists())
    {
        LLWebRTCVoiceClient& voice = LLWebRTCVoiceClient::instance();
        voice.setDeviceOverride(capture ? std::string() : voice.captureOverride(),
                                capture ? voice.renderOverride() : std::string());
    }
}

// static
bool LLVRAudio::active()
{
    return s_running;
}

// static
bool LLVRAudio::holdsWorldOutput()
{
    return s_outputSwitched;
}

// static
float LLVRAudio::voiceOutputBoost()
{
    return (s_running && s_voiceOnHeadset) ? headsetGainLinear() : 1.f;
}

// static
void LLVRAudio::shutdown()
{
    if (!s_running) return;
    LL_INFOS("VRAudio") << "viewer quitting during a VR session: putting the audio devices back" << LL_ENDL;
    s_running = false;
    sessionEnded();
}

// static
void LLVRAudio::recoverAfterCrash()
{
    LLVRAudioPulse::recoverAfterCrash();
}

// static
const std::string& LLVRAudio::headsetMicName()
{
    return s_micName;
}

// static
const std::string& LLVRAudio::headsetMicVoiceId()
{
    return s_micId;
}

// static
boost::signals2::connection LLVRAudio::addChangedCallback(const changed_signal_t::slot_type& cb)
{
    return changedSignal().connect(cb);
}

// static
const char* LLVRAudio::matchHeadsetOutput(const std::string& name)
{
    return matchRules(kOutputRules, name);
}

// static
const char* LLVRAudio::matchHeadsetMic(const std::string& name)
{
    if (isMonitorSource(name)) return nullptr;
    return matchRules(kMicRules, name);
}
