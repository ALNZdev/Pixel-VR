#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <wrl.h>
#include <cstdint>
#include <string>
#include <vector>

class D3D11Helper
{
public:
    static bool CreateDeviceForAdapter(
        int adapterIndex,
        ID3D11Device** device,
        ID3D11DeviceContext** context,
        std::string& adapterName);

    static bool CopyTextureToBgraCpu(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11Texture2D* source,
        uint32_t srcX,
        uint32_t srcY,
        uint32_t width,
        uint32_t height,
        std::vector<uint8_t>& bgra,
        uint32_t& rowPitch);

    static void BgraToNv12(
        const uint8_t* bgra,
        uint32_t width,
        uint32_t height,
        uint32_t bgraPitch,
        std::vector<uint8_t>& nv12);
};
