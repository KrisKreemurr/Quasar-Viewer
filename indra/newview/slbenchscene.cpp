/**
 * @file slbenchscene.cpp
 * @brief Render benchmark capture format helpers (#1304). See slbenchscene.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Slipstream contributors.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "slbenchscene.h"

#include "hbxxh.h"
#include "lldir.h"
#include "llviewercontrol.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace SLBenchScene
{

MaterialRec toRec(U32 id, const LLCleanMaterialStore::MaterialDesc& d)
{
    MaterialRec r;
    std::memset(&r, 0, sizeof(r));
    r.id = id;
    std::memcpy(r.tex[0], d.base_color_id.mData, 16);
    std::memcpy(r.tex[1], d.normal_id.mData, 16);
    std::memcpy(r.tex[2], d.mr_id.mData, 16);
    std::memcpy(r.tex[3], d.emissive_id.mData, 16);
    std::memcpy(r.base_color_factor, d.base_color_factor, sizeof(r.base_color_factor));
    std::memcpy(r.emissive_factor, d.emissive_factor, sizeof(r.emissive_factor));
    std::memcpy(r.spec_color_gloss, d.spec_color_gloss, sizeof(r.spec_color_gloss));
    r.metallic = d.metallic_factor;
    r.roughness = d.roughness_factor;
    r.alpha_cutoff = d.alpha_cutoff;
    r.env_intensity = d.env_intensity;
    r.glow = d.glow;
    r.legacy = d.legacy;
    r.color_all_slots = d.color_all_slots;
    r.gltf = d.gltf;
    r.emissive_mask = d.emissive_mask;
    r.hud = d.hud;
    r.alpha_blend = d.alpha_blend;
    r.bump = d.bump;
    for (int k = 0; k < 3; ++k)
    {
        r.uv_own[k] = d.uv_own[k];
        std::memcpy(r.uv_xform[k], d.uv_xform[k], sizeof(r.uv_xform[k]));
    }
    return r;
}

LLCleanMaterialStore::MaterialDesc fromRec(const MaterialRec& r)
{
    LLCleanMaterialStore::MaterialDesc d;
    std::memcpy(d.base_color_id.mData, r.tex[0], 16);
    std::memcpy(d.normal_id.mData, r.tex[1], 16);
    std::memcpy(d.mr_id.mData, r.tex[2], 16);
    std::memcpy(d.emissive_id.mData, r.tex[3], 16);
    std::memcpy(d.base_color_factor, r.base_color_factor, sizeof(d.base_color_factor));
    std::memcpy(d.emissive_factor, r.emissive_factor, sizeof(d.emissive_factor));
    std::memcpy(d.spec_color_gloss, r.spec_color_gloss, sizeof(d.spec_color_gloss));
    d.metallic_factor = r.metallic;
    d.roughness_factor = r.roughness;
    d.alpha_cutoff = r.alpha_cutoff;
    d.env_intensity = r.env_intensity;
    d.glow = r.glow;
    d.legacy = r.legacy != 0;
    d.color_all_slots = r.color_all_slots != 0;
    d.gltf = r.gltf != 0;
    d.emissive_mask = r.emissive_mask != 0;
    d.hud = r.hud != 0;
    d.alpha_blend = r.alpha_blend != 0;
    d.bump = r.bump;
    for (int k = 0; k < 3; ++k)
    {
        d.uv_own[k] = r.uv_own[k] != 0;
        std::memcpy(d.uv_xform[k], r.uv_xform[k], sizeof(d.uv_xform[k]));
    }
    return d;
}

void envFromFrame(const LLVKWorldFrame& wf, EnvRec& e)
{
    std::memset(&e, 0, sizeof(e));
    std::memcpy(e.sun_dir, wf.sun_dir, sizeof(e.sun_dir));
    std::memcpy(e.sun_color, wf.sun_color, sizeof(e.sun_color));
    std::memcpy(e.ambient_color, wf.ambient_color, sizeof(e.ambient_color));
    std::memcpy(e.sky_horizon, wf.sky_horizon, sizeof(e.sky_horizon));
    std::memcpy(e.sky_zenith, wf.sky_zenith, sizeof(e.sky_zenith));
    std::memcpy(e.water_fog, wf.water_fog, sizeof(e.water_fog));
    std::memcpy(e.atmos, wf.atmos, sizeof(e.atmos));
    std::memcpy(e.water, wf.water, sizeof(e.water));
    std::memcpy(e.water_normal, wf.water_normal, sizeof(e.water_normal));
    std::memcpy(e.clouds, wf.clouds, sizeof(e.clouds));
    std::memcpy(e.cloud_noise, wf.cloud_noise, sizeof(e.cloud_noise));
    std::memcpy(e.bodies, wf.bodies, sizeof(e.bodies));
    std::memcpy(e.sky_tex, wf.sky_tex, sizeof(e.sky_tex));
    e.probe_sky_ambiance = wf.probe_sky_ambiance;
    e.probe_agent_cell[0] = wf.probe_agent_cell[0];
    e.probe_agent_cell[1] = wf.probe_agent_cell[1];
    e.probe_count = (U32)wf.probes.size();
    e.region_count = (U32)wf.probe_regions.size();
}

void envToFrame(const EnvRec& e, LLVKWorldFrame& wf)
{
    std::memcpy(wf.sun_dir, e.sun_dir, sizeof(e.sun_dir));
    std::memcpy(wf.sun_color, e.sun_color, sizeof(e.sun_color));
    std::memcpy(wf.ambient_color, e.ambient_color, sizeof(e.ambient_color));
    std::memcpy(wf.sky_horizon, e.sky_horizon, sizeof(e.sky_horizon));
    std::memcpy(wf.sky_zenith, e.sky_zenith, sizeof(e.sky_zenith));
    std::memcpy(wf.water_fog, e.water_fog, sizeof(e.water_fog));
    std::memcpy(wf.atmos, e.atmos, sizeof(e.atmos));
    std::memcpy(wf.water, e.water, sizeof(e.water));
    std::memcpy(wf.water_normal, e.water_normal, sizeof(e.water_normal));
    std::memcpy(wf.clouds, e.clouds, sizeof(e.clouds));
    std::memcpy(wf.cloud_noise, e.cloud_noise, sizeof(e.cloud_noise));
    std::memcpy(wf.bodies, e.bodies, sizeof(e.bodies));
    std::memcpy(wf.sky_tex, e.sky_tex, sizeof(e.sky_tex));
    wf.probe_sky_ambiance = e.probe_sky_ambiance;
    wf.probe_agent_cell[0] = e.probe_agent_cell[0];
    wf.probe_agent_cell[1] = e.probe_agent_cell[1];
}

std::string benchRoot()
{
    const std::string set = gSavedSettings.getString("BenchRoot");
    if (!set.empty()) return set;
    return gDirUtilp->getOSUserAppDir() + gDirUtilp->getDirDelimiter() + "bench";
}

bool insideSourceTree(const std::string& dir)
{
    std::error_code ec;
    fs::path p = fs::absolute(fs::path(dir), ec);
    if (ec) return true;   // can't tell: refuse
    p = p.lexically_normal();
    for (;;)
    {
        if (fs::exists(p / ".git", ec)) return true;
        const fs::path parent = p.parent_path();
        if (parent.empty() || parent == p) return false;
        p = parent;
    }
}

bool makeOutputDir(const std::string& dir, std::string& err)
{
    if (dir.empty())
    {
        err = "no directory";
        return false;
    }
    if (insideSourceTree(dir))
    {
        err = dir + " is inside a source checkout (a .git above it): bench output must stay outside the repository";
        return false;
    }
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    if (ec || !fs::is_directory(fs::path(dir), ec))
    {
        err = "cannot create " + dir;
        return false;
    }
    return true;
}

std::string hex64(U64 v)
{
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)v);
    return buf;
}

std::string fileDigest(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return std::string();
    HBXXH64 h;
    h.update(f);   // (reads to the end and closes the file)
    return hex64(h.digest());
}

std::string stamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm tmv{};
#if LL_WINDOWS
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tmv);
    return buf;
}

} // namespace SLBenchScene
