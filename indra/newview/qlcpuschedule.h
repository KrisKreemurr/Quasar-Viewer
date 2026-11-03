/**
 * @file qlcpuschedule.h
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

#pragma once

#include "stdtypes.h"

/// CPU placement for hybrid CPUs (QuasarPreferPerformanceCores), Windows only.
///
/// Intel's hybrid CPUs (12th generation on) have fast performance cores and slower efficiency cores. Windows 11 places
/// threads with the CPU's Thread Director hints; Windows 10 has none, and can run the viewer's main thread, which every
/// frame waits on, or the GL driver's own thread on an efficiency core. Windows says which cores are which through CPU
/// sets (GetSystemCpuSetInformation, EfficiencyClass: the highest class is the performance cores), and takes a soft
/// preference for them, not a hard affinity, so a busy machine can still run any thread anywhere:
///   - the process's default CPU sets are the performance cores, which every thread with no preference of its own
///     takes, the GL driver's included, and the main thread asks for them as well;
///   - the texture decode and mesh decode pools' threads ask for the efficiency cores (through the thread pools' start
///     hook), so a burst of decoding does not take cores from the main thread;
///   - power throttling (EcoQoS) is switched off for the process, so Windows does not slow it when it judges it to be
///     in the background.
/// Nothing is done on a CPU with one kind of core, or anywhere but Windows.
namespace QLCpuSchedule
{
    /// Applies the placement where it is wanted and the CPU is hybrid; true if it was applied. Call on the main thread,
    /// before the thread pools start.
    bool apply(bool enabled);

    /// The logical processors among the efficiency cores, where apply() placed threads; 0 otherwise.
    U32 efficiencyThreads();
}
