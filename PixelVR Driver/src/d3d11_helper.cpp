#include "d3d11_helper.h"
#include "driverlog.h"

#include <algorithm>

using Microsoft::WRL::ComPtr;

bool D3D11Helper::CreateDeviceForAdapter(
    int adapterIndex,
    ID3D11Device** device,
    ID3D11DeviceContext** context,
    std::string& adapterName)
{
    if (device == nullptr || context == nullptr)
        return false;

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return false;

    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1(static_cast<UINT>(adapterIndex), &adapter)))
        return false;

    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);

    char name[256] = {};
    WideCharToMultiByte(
        CP_UTF8,
        0,
        desc.Description,
        -1,
        name,
        static_cast<int>(sizeof(name)),
        nullptr,
        nullptr);
    adapterName = name;

    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(
        adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        flags,
        levels,
        2,
        D3D11_SDK_VERSION,
        device,
        &got,
        context);

    if (FAILED(hr))
    {
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(
            adapter.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            flags,
            levels,
            2,
            D3D11_SDK_VERSION,
            device,
            &got,
            context);
    }

    if (FAILED(hr))
    {
        DriverLog("[D3D11] CreateDevice falló (0x%08X) en %s", hr, name);
        return false;
    }

    DriverLog("[D3D11] Device creado en adaptador: %s", name);
    return true;
}

bool D3D11Helper::CopyTextureToBgraCpu(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* source,
    uint32_t srcX,
    uint32_t srcY,
    uint32_t width,
    uint32_t height,
    std::vector<uint8_t>& bgra,
    uint32_t& rowPitch)
{
    if (device == nullptr || context == nullptr || source == nullptr || width == 0 || height == 0)
        return false;

    D3D11_TEXTURE2D_DESC srcDesc{};
    source->GetDesc(&srcDesc);

    if (srcX + width > srcDesc.Width || srcY + height > srcDesc.Height)
        return false;

    D3D11_TEXTURE2D_DESC stagingDesc = srcDesc;
    stagingDesc.Width = width;
    stagingDesc.Height = height;
    stagingDesc.BindFlags = 0;
    stagingDesc.MiscFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.SampleDesc.Quality = 0;

    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging)))
        return false;

    D3D11_BOX box{};
    box.left = srcX;
    box.top = srcY;
    box.front = 0;
    box.right = srcX + width;
    box.bottom = srcY + height;
    box.back = 1;

    context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, source, 0, &box);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return false;

    rowPitch = width * 4;
    bgra.resize(static_cast<size_t>(rowPitch) * height);

    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    for (uint32_t y = 0; y < height; ++y)
    {
        memcpy(
            bgra.data() + static_cast<size_t>(y) * rowPitch,
            src + static_cast<size_t>(y) * mapped.RowPitch,
            rowPitch);
    }

    context->Unmap(staging.Get(), 0);
    return true;
}

void D3D11Helper::BgraToNv12(
    const uint8_t* bgra,
    uint32_t width,
    uint32_t height,
    uint32_t bgraPitch,
    std::vector<uint8_t>& nv12)
{
    width &= ~1u;
    height &= ~1u;

    nv12.resize(static_cast<size_t>(width) * height * 3 / 2);
    uint8_t* yPlane = nv12.data();
    uint8_t* uvPlane = yPlane + static_cast<size_t>(width) * height;

    auto clampByte = [](int v) -> uint8_t
    {
        if (v < 0) return 0;
        if (v > 255) return 255;
        return static_cast<uint8_t>(v);
    };

    for (uint32_t y = 0; y < height; ++y)
    {
        const uint8_t* row = bgra + static_cast<size_t>(y) * bgraPitch;
        uint8_t* yRow = yPlane + static_cast<size_t>(y) * width;

        for (uint32_t x = 0; x < width; ++x)
        {
            const uint8_t b = row[x * 4 + 0];
            const uint8_t g = row[x * 4 + 1];
            const uint8_t r = row[x * 4 + 2];

            yRow[x] = clampByte(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);

            if ((y % 2) == 0 && (x % 2) == 0)
            {
                int bSum = 0, gSum = 0, rSum = 0;
                for (uint32_t dy = 0; dy < 2; ++dy)
                {
                    const uint8_t* p = bgra + static_cast<size_t>(y + dy) * bgraPitch + x * 4;
                    bSum += p[0] + p[4];
                    gSum += p[1] + p[5];
                    rSum += p[2] + p[6];
                }

                const int rAvg = rSum / 4;
                const int gAvg = gSum / 4;
                const int bAvg = bSum / 4;

                const uint32_t uvIndex = (y / 2) * width + x;
                uvPlane[uvIndex] = clampByte(((-38 * rAvg - 74 * gAvg + 112 * bAvg + 128) >> 8) + 128);
                uvPlane[uvIndex + 1] = clampByte(((112 * rAvg - 94 * gAvg - 18 * bAvg + 128) >> 8) + 128);
            }
        }
    }
}
