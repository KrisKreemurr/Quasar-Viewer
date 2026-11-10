// Windows display capability lookup; no swapchain HDR metadata is used as input.
#pragma once

#if LL_WINDOWS
#include "vk_display_calibration.h"
#include "llwin32headers.h"
#include <SDL3/SDL.h>
#include <dxgi1_6.h>
#include <cmath>

namespace vkdisplay
{
inline void queryWindowsPeak(SDL_Window* window, ReportedLuminance& report)
{
    if (!window) return;
    const HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    const HMONITOR wanted = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
    if (!wanted) return;
    HMODULE module = LoadLibraryW(L"dxgi.dll");
    if (!module) return;
    using FactoryFn = HRESULT (WINAPI*)(REFIID, void**);
    const auto makeFactory = reinterpret_cast<FactoryFn>(GetProcAddress(module, "CreateDXGIFactory1"));
    IDXGIFactory1* factory = nullptr;
    if (makeFactory && SUCCEEDED(makeFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory)
    {
        for (UINT adapterIndex = 0; adapterIndex < 32 && !report.peakNits; ++adapterIndex)
        {
            IDXGIAdapter1* adapter = nullptr;
            if (factory->EnumAdapters1(adapterIndex, &adapter) != S_OK) break;
            for (UINT outputIndex = 0; outputIndex < 64; ++outputIndex)
            {
                IDXGIOutput* output = nullptr;
                if (adapter->EnumOutputs(outputIndex, &output) != S_OK) break;
                DXGI_OUTPUT_DESC desc{};
                const bool matches = SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == wanted;
                if (matches)
                {
                    IDXGIOutput6* output6 = nullptr;
                    if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&output6))) && output6)
                    {
                        DXGI_OUTPUT_DESC1 info{};
                        if (SUCCEEDED(output6->GetDesc1(&info)) && std::isfinite(info.MaxLuminance) &&
                            info.MaxLuminance >= 80.f && info.MaxLuminance <= 10000.f)
                        {
                            report.peakNits = info.MaxLuminance;
                            report.peakSource = "Windows DXGI output advertised maximum";
                        }
                        output6->Release();
                    }
                }
                output->Release();
                if (matches) break;
            }
            adapter->Release();
        }
        factory->Release();
    }
    FreeLibrary(module);
}
}
#endif
