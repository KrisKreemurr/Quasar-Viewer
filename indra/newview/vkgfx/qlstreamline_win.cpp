// Quasar: the delay-load hook that hands the viewer's Vulkan imports to NVIDIA's
// Streamline interposer (qlstreamlinevk.h). Windows, with the Streamline SDK, only.
//
// The viewer links vulkan-1.dll delay-loaded (/DELAYLOAD:vulkan-1.dll), so the DLL is
// loaded at the first Vulkan call, through this hook:
//  - before the load: when QuasarStreamline is on and sl.interposer.dll checks out,
//    the interposer is returned in its place (it forwards to the real loader and hooks
//    what Reflex and frame generation need);
//  - an import the interposer does not export comes from the real vulkan-1.dll.
#if defined(_WIN32) && defined(QL_HAS_STREAMLINE)
#include <windows.h>
#include <delayimp.h>
#include <cstring>

#include "qlstreamlinevk.h"

namespace
{
HMODULE sInterposer = nullptr;

FARPROC WINAPI qlDelayHook(unsigned notify, PDelayLoadInfo info)
{
    if (notify == dliNotePreLoadLibrary && info && info->szDll && _stricmp(info->szDll, "vulkan-1.dll") == 0)
    {
        sInterposer = static_cast<HMODULE>(qlsl::bootInterposer());
        return reinterpret_cast<FARPROC>(sInterposer);   // nullptr: the plain loader
    }
    if (notify == dliFailGetProc && info && sInterposer && info->hmodCur == sInterposer && info->dlp.fImportByName)
    {
        static HMODULE real = LoadLibraryA("vulkan-1.dll");
        return real ? GetProcAddress(real, info->dlp.szProcName) : nullptr;
    }
    return nullptr;
}
} // namespace

extern "C" const PfnDliHook __pfnDliNotifyHook2 = qlDelayHook;
extern "C" const PfnDliHook __pfnDliFailureHook2 = qlDelayHook;
#endif
