/**
 * @file qlcpuschedule.cpp
 * @brief Quasar Viewer: keep the main thread on a hybrid CPU's performance cores, and background decoding off them.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Quasar Viewer Source Code (based on Alchemy Viewer)
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

#include "qlcpuschedule.h"

#if LL_WINDOWS

#include "llwin32headers.h"
#include "threadpool.h"

#include <string>
#include <vector>

namespace
{
    std::vector<ULONG> sPerformanceSets;
    std::vector<ULONG> sEfficiencySets;

    // The pools whose threads are steered onto the efficiency cores: background decoding, which comes in bursts and
    // whose latency the frame does not wait on.
    bool is_background_pool(const std::string& pool)
    {
        return pool == "ImageDecode" || pool == "MeshLodProcessing";
    }

    void place_pool_thread(const std::string& pool)
    {
        if (is_background_pool(pool) && !sEfficiencySets.empty())
        {
            SetThreadSelectedCpuSets(GetCurrentThread(), sEfficiencySets.data(), (ULONG)sEfficiencySets.size());
        }
    }

    // The CPU sets of this process's processor group, split by efficiency class: the highest class is the
    // performance cores. False, with both empty, where Windows reports one class or the query fails.
    bool read_cpu_sets()
    {
        sPerformanceSets.clear();
        sEfficiencySets.clear();

        ULONG length = 0;
        GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
        if (length == 0)
        {
            return false;
        }
        std::vector<U8> buffer(length);
        auto* first = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data());
        if (!GetSystemCpuSetInformation(first, length, &length, GetCurrentProcess(), 0))
        {
            return false;
        }

        struct Set { ULONG mId; BYTE mClass; WORD mGroup; };
        std::vector<Set> sets;
        BYTE top = 0;
        BYTE bottom = 0xff;
        for (ULONG offset = 0; offset + sizeof(DWORD) * 2 <= length;)
        {
            auto* info = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data() + offset);
            if (info->Size == 0)
            {
                break;
            }
            if (info->Type == CpuSetInformation)
            {
                const BYTE efficiency = info->CpuSet.EfficiencyClass;
                sets.push_back({ info->CpuSet.Id, efficiency, info->CpuSet.Group });
                top = llmax(top, efficiency);
                bottom = llmin(bottom, efficiency);
            }
            offset += info->Size;
        }

        if (sets.empty() || top == bottom)
        {
            return false;  // one kind of core: nothing to steer
        }

        // Only the processor group the main thread runs in: the CPU sets a thread selects must be in one group, and
        // every desktop hybrid CPU has a single group.
        GROUP_AFFINITY affinity = {};
        const WORD group = GetThreadGroupAffinity(GetCurrentThread(), &affinity) ? affinity.Group : 0;
        for (const Set& set : sets)
        {
            if (set.mGroup != group)
            {
                continue;
            }
            (set.mClass == top ? sPerformanceSets : sEfficiencySets).push_back(set.mId);
        }
        if (sPerformanceSets.empty() || sEfficiencySets.empty())
        {
            sPerformanceSets.clear();
            sEfficiencySets.clear();
            return false;
        }
        return true;
    }

    // Asks Windows not to slow the process as a background one (EcoQoS), where it would.
    void disable_power_throttling()
    {
        PROCESS_POWER_THROTTLING_STATE state = {};
        state.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        state.StateMask   = 0;  // controlled, and off
        if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state)))
        {
            LL_INFOS("Quasar") << "Could not turn off power throttling (" << GetLastError() << ")" << LL_ENDL;
        }
    }
}

bool QLCpuSchedule::apply(bool enabled)
{
    if (!enabled)
    {
        return false;
    }

    disable_power_throttling();

    if (!read_cpu_sets())
    {
        LL_INFOS("Quasar") << "CPU has one kind of core; no thread placement" << LL_ENDL;
        return false;
    }

    // Every thread with no preference of its own, the GL driver's among them, takes the performance cores; the main
    // thread asks for them itself too.
    const ULONG performance = (ULONG)sPerformanceSets.size();
    const BOOL process_ok = SetProcessDefaultCpuSets(GetCurrentProcess(), sPerformanceSets.data(), performance);
    const BOOL thread_ok = SetThreadSelectedCpuSets(GetCurrentThread(), sPerformanceSets.data(), performance);
    if (!process_ok || !thread_ok)
    {
        LL_WARNS("Quasar") << "Could not set CPU sets (" << GetLastError() << "); no thread placement" << LL_ENDL;
        SetProcessDefaultCpuSets(GetCurrentProcess(), nullptr, 0);
        SetThreadSelectedCpuSets(GetCurrentThread(), nullptr, 0);
        sPerformanceSets.clear();
        sEfficiencySets.clear();
        return false;
    }

    LL::ThreadPoolBase::setThreadStartHook(&place_pool_thread);

    LL_INFOS("Quasar") << "Hybrid CPU: main thread and driver on " << sPerformanceSets.size()
                       << " performance logical processors, decoding on " << sEfficiencySets.size()
                       << " efficiency logical processors" << LL_ENDL;
    return true;
}

U32 QLCpuSchedule::efficiencyThreads()
{
    return (U32)sEfficiencySets.size();
}

#else // !LL_WINDOWS

bool QLCpuSchedule::apply(bool)
{
    return false;
}

U32 QLCpuSchedule::efficiencyThreads()
{
    return 0;
}

#endif
