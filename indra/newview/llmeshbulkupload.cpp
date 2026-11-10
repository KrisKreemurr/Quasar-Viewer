/**
 * @file llmeshbulkupload.cpp
 * @brief Bulk mesh upload (#780): companion mesh detection by name.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llmeshbulkupload.h"

#include "lldir.h"
#include "llviewercontrol.h"

#include <algorithm>
#include <map>

namespace LLMeshBulk
{

std::string toLowerCopy(const std::string& s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](char c) { return (char)tolower((unsigned char)c); });
    return out;
}

suffix_table_t buildSuffixTable()
{
    suffix_table_t table;
    auto add = [&table](const std::string& suffix, ERole role)
    {
        if (suffix.empty())
        {
            return;
        }
        std::string s = toLowerCopy(suffix);
        if (s[0] != '_')
        {
            s.insert(0, "_");
        }
        for (const auto& entry : table)
        {
            if (entry.first == s)
            {
                return; // first definition wins (the user's settings come first)
            }
        }
        table.emplace_back(s, role);
    };

    // The uploader's own suffix settings (Upload > Model: LOD suffix presets) take precedence,
    // so a reversed LOD0 = high preset keeps its meaning.
    static const char* setting_names[] =
    {
        "MeshLowestLodSuffix", "MeshLowLodSuffix", "MeshMediumLodSuffix", "MeshHighLodSuffix", "MeshPhysicsSuffix"
    };
    for (S32 i = 0; i < 5; ++i)
    {
        if (gSavedSettings.controlExists(setting_names[i]))
        {
            add(gSavedSettings.getString(setting_names[i]), (ERole)i);
        }
    }

    add("_lod0", ROLE_LOD0);
    add("_lod1", ROLE_LOD1);
    add("_lod2", ROLE_LOD2);
    add("_lod3", ROLE_HIGH);
    add("_lowest", ROLE_LOD0);
    add("_low", ROLE_LOD1);
    add("_medium", ROLE_LOD2);
    add("_med", ROLE_LOD2);
    add("_high", ROLE_HIGH);
    add("_physics", ROLE_PHYSICS);
    add("_phys", ROLE_PHYSICS);
    add("_collision", ROLE_PHYSICS);
    add("_col", ROLE_PHYSICS);

    // Longest first so "_lowest" is tried before "_low".
    std::stable_sort(table.begin(), table.end(),
                     [](const std::pair<std::string, ERole>& a, const std::pair<std::string, ERole>& b)
                     { return a.first.size() > b.first.size(); });
    return table;
}

static bool is_digits(const std::string& s, size_t from)
{
    return from < s.size() && s.find_first_not_of("0123456789", from) == std::string::npos;
}

NameInfo classifyName(const std::string& name, const suffix_table_t& suffixes)
{
    std::string n = name;

    // Blender duplicate numbering: "Chair_phys.001"
    size_t dot = n.find_last_of('.');
    if (dot != std::string::npos && dot > 0 && is_digits(n, dot + 1))
    {
        n.erase(dot);
    }

    const std::string lower = toLowerCopy(n);

    NameInfo info;
    info.mBase = n;
    info.mRole = ROLE_VISUAL;

    // Collision prefixes (UCX_ convex, UBX_ box, UCP_ capsule, USP_ sphere): UCX_Chair or UCX_Chair_01
    static const char* prefixes[] = { "ucx_", "ubx_", "ucp_", "usp_" };
    for (const char* prefix : prefixes)
    {
        if (lower.size() > 4 && lower.compare(0, 4, prefix) == 0)
        {
            std::string base = n.substr(4);
            size_t us = base.find_last_of('_');
            if (us != std::string::npos && us > 0 && is_digits(base, us + 1))
            {
                base.erase(us);
            }
            info.mBase = base;
            info.mRole = ROLE_PHYSICS;
            return info;
        }
    }

    for (const auto& entry : suffixes)
    {
        const std::string& suffix = entry.first;
        if (lower.size() > suffix.size()
            && lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0)
        {
            info.mBase = n.substr(0, n.size() - suffix.size());
            info.mRole = entry.second;
            return info;
        }
    }

    return info;
}

NameInfo classifyName(const std::string& name)
{
    return classifyName(name, buildSuffixTable());
}

bool isModelFile(const std::string& path)
{
    const std::string ext = gDirUtilp->getExtension(path); // lower case
    return ext == "dae" || ext == "gltf" || ext == "glb" || ext == "fbx";
}

std::vector<std::string> listModelFiles(const std::string& dir)
{
    std::vector<std::string> out;
    for (const std::string& name : gDirUtilp->getFilesInDir(dir))
    {
        if (isModelFile(name))
        {
            out.push_back(gDirUtilp->add(dir, name));
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<Group> groupFiles(const std::vector<std::string>& files_in)
{
    const suffix_table_t suffixes = buildSuffixTable();

    std::vector<std::string> files;
    for (const std::string& f : files_in)
    {
        if (isModelFile(f) && std::find(files.begin(), files.end(), f) == files.end())
        {
            files.push_back(f);
        }
    }
    std::sort(files.begin(), files.end());

    std::vector<Group> groups;
    std::map<std::string, size_t> index;    // lower-case base -> groups[] index
    std::vector<std::string> high_files;    // per group: file named *_high / *_LOD3

    for (const std::string& file : files)
    {
        const NameInfo info = classifyName(gDirUtilp->getBaseFileName(file, true), suffixes);
        const std::string key = toLowerCopy(info.mBase);

        auto found = index.find(key);
        size_t gi;
        if (found == index.end())
        {
            gi = groups.size();
            index[key] = gi;
            groups.emplace_back();
            groups.back().mName = info.mBase;
            high_files.emplace_back();
        }
        else
        {
            gi = found->second;
        }
        Group& group = groups[gi];
        const std::string short_name = gDirUtilp->getBaseFileName(file, false);

        switch (info.mRole)
        {
        case ROLE_VISUAL:
            if (group.mVisual.empty())
            {
                group.mVisual = file;
                group.mName = info.mBase;
            }
            else
            {
                group.mNotes.push_back("Ignored " + short_name + ": another file already provides this model");
            }
            break;
        case ROLE_HIGH:
            if (high_files[gi].empty())
            {
                high_files[gi] = file;
            }
            else
            {
                group.mNotes.push_back("Ignored " + short_name + ": duplicate high LOD file");
            }
            break;
        case ROLE_PHYSICS:
            if (group.mPhysicsFile.empty())
            {
                group.mPhysicsFile = file;
            }
            else
            {
                group.mNotes.push_back("Ignored " + short_name + ": duplicate physics file");
            }
            break;
        default: // ROLE_LOD0..ROLE_LOD2
            if (group.mLODFile[info.mRole].empty())
            {
                group.mLODFile[info.mRole] = file;
            }
            else
            {
                group.mNotes.push_back("Ignored " + short_name + ": duplicate LOD file");
            }
            break;
        }
    }

    for (size_t gi = 0; gi < groups.size(); ++gi)
    {
        Group& group = groups[gi];
        if (!high_files[gi].empty())
        {
            if (group.mVisual.empty())
            {
                group.mVisual = high_files[gi];   // "chair_high.dae" is the model when there is no "chair.dae"
            }
            else
            {
                group.mNotes.push_back("Ignored " + gDirUtilp->getBaseFileName(high_files[gi], false)
                                       + ": " + gDirUtilp->getBaseFileName(group.mVisual, false) + " is the high LOD");
            }
        }
    }

    return groups;
}

namespace
{
    const char* const PHYSICS_FALLBACK_SETTING = "MeshBulkUploadPhysicsFallback";
    const char* const PHYSICS_FALLBACK_NAMES[PHYSICS_FALLBACK_COUNT] =
    {
        "bounding_box", "lowest", "low", "medium", "none"
    };
}

const char* physicsFallbackName(EPhysicsFallback fallback)
{
    return fallback >= 0 && fallback < PHYSICS_FALLBACK_COUNT ? PHYSICS_FALLBACK_NAMES[fallback]
                                                              : PHYSICS_FALLBACK_NAMES[PHYSICS_BOUNDING_BOX];
}

EPhysicsFallback physicsFallbackFromName(const std::string& name)
{
    for (S32 i = 0; i < PHYSICS_FALLBACK_COUNT; ++i)
    {
        if (name == PHYSICS_FALLBACK_NAMES[i])
        {
            return (EPhysicsFallback)i;
        }
    }
    return PHYSICS_BOUNDING_BOX;
}

EPhysicsFallback physicsFallbackSetting()
{
    if (!gSavedSettings.controlExists(PHYSICS_FALLBACK_SETTING))
    {
        return PHYSICS_BOUNDING_BOX;
    }
    return physicsFallbackFromName(gSavedSettings.getString(PHYSICS_FALLBACK_SETTING));
}

void setPhysicsFallbackSetting(EPhysicsFallback fallback)
{
    if (gSavedSettings.controlExists(PHYSICS_FALLBACK_SETTING))
    {
        gSavedSettings.setString(PHYSICS_FALLBACK_SETTING, physicsFallbackName(fallback));
    }
}

S32 physicsComboIndex(EPhysicsFallback fallback, S32 combo_item_count)
{
    // As LLFloaterModelPreview::onPhysicsUseLOD reads the combo: item n (1..4) is LOD 4 - n,
    // the bounding box is the item before "From file".
    switch (fallback)
    {
    case PHYSICS_BOUNDING_BOX:
        return combo_item_count >= 3 ? combo_item_count - 2 : 0;
    case PHYSICS_LOWEST:
        return 4;
    case PHYSICS_LOW:
        return 3;
    case PHYSICS_MEDIUM:
        return 2;
    case PHYSICS_NONE:
    default:
        return 0;
    }
}

} // namespace LLMeshBulk
