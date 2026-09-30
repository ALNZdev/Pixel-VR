#include "display_mode.h"
#include "driverlog.h"

#include <d3d11.h>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

bool DisplayMode::FindOutputContainingPoint(
    int32_t x,
    int32_t y,
    DisplayOutputInfo& outInfo)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        DriverLog("[Display] No se pudo crear DXGI factory");
        return false;
    }

    for (UINT adapterIndex = 0;; ++adapterIndex)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;

        DXGI_ADAPTER_DESC1 adapterDesc{};
        adapter->GetDesc1(&adapterDesc);

        char adapterName[256] = {};
        WideCharToMultiByte(
            CP_UTF8,
            0,
            adapterDesc.Description,
            -1,
            adapterName,
            static_cast<int>(sizeof(adapterName)),
            nullptr,
            nullptr);

        for (UINT outputIndex = 0;; ++outputIndex)
        {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND)
                break;

            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)))
                continue;

            const RECT& r = desc.DesktopCoordinates;
            if (x >= r.left && x < r.right && y >= r.top && y < r.bottom)
            {
                outInfo.adapterIndex = static_cast<int>(adapterIndex);
                outInfo.outputIndex = static_cast<int>(outputIndex);
                outInfo.desc = desc;
                outInfo.adapterName = adapterName;
                return true;
            }
        }
    }

    DriverLog(
        "[Display] Ninguna salida DXGI contiene el punto (%d,%d)",
        x,
        y);
    return false;
}
