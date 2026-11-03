#include "sbs_composer.h"
#include "driverlog.h"

#include <d3dcompiler.h>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace
{
    const char* kShaderSource = R"HLSL(
cbuffer Params : register(b0)
{
    float4 uvRect;   // uMin, vMin, uMax, vMax
    float  srgbEncode;
    float3 pad;
};
Texture2D    eyeTex : register(t0);
SamplerState samp   : register(s0);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID)
{
    // Fullscreen triangle.
    float2 p = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = p;
    return o;
}

float3 LinearToSrgb(float3 c)
{
    c = saturate(c);
    return (c <= 0.0031308) ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

float4 PSMain(VSOut i) : SV_Target
{
    float2 uv = lerp(uvRect.xy, uvRect.zw, i.uv);
    float4 c = eyeTex.Sample(samp, uv);
    if (srgbEncode > 0.5)
        c.rgb = LinearToSrgb(c.rgb);
    return c;
}
)HLSL";

    struct Params
    {
        float uvRect[4];
        float srgbEncode;
        float pad[3];
    };

    bool IsSrgb(DXGI_FORMAT f)
    {
        return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
            f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
            f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    }
}

bool SbsComposer::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    Shutdown();
    m_device = device;
    m_context = context;

    ComPtr<ID3DBlob> vsBlob, psBlob, errors;
    HRESULT hr = D3DCompile(kShaderSource, strlen(kShaderSource), "sbs", nullptr, nullptr,
        "VSMain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errors);
    if (FAILED(hr))
    {
        DriverLog("[SBS] VS compile error: %s", errors ? (const char*)errors->GetBufferPointer() : "?");
        return false;
    }
    hr = D3DCompile(kShaderSource, strlen(kShaderSource), "sbs", nullptr, nullptr,
        "PSMain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errors);
    if (FAILED(hr))
    {
        DriverLog("[SBS] PS compile error: %s", errors ? (const char*)errors->GetBufferPointer() : "?");
        return false;
    }

    if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vs)) ||
        FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_ps)))
    {
        DriverLog("[SBS] No se pudieron crear los shaders");
        return false;
    }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&sd, &m_sampler);

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Params);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device->CreateBuffer(&bd, nullptr, &m_constants);

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&blend, &m_blendOpaque);

    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    device->CreateBlendState(&blend, &m_blendAlpha);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    device->CreateRasterizerState(&rd, &m_raster);

    return m_sampler && m_constants && m_blendOpaque && m_blendAlpha && m_raster;
}

void SbsComposer::Shutdown()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_srvCache.clear();
    m_rtvCache.clear();
    m_vs.Reset();
    m_ps.Reset();
    m_sampler.Reset();
    m_constants.Reset();
    m_blendOpaque.Reset();
    m_blendAlpha.Reset();
    m_raster.Reset();
    m_context.Reset();
    m_device.Reset();
}

void SbsComposer::Forget(ID3D11Texture2D* texture)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_srvCache.erase(texture);
    m_rtvCache.erase(texture);
}

SbsComposer::CachedView* SbsComposer::GetSrv(ID3D11Texture2D* texture)
{
    auto it = m_srvCache.find(texture);
    if (it != m_srvCache.end())
        return &it->second;

    D3D11_TEXTURE2D_DESC td{};
    texture->GetDesc(&td);

    CachedView view;
    view.srgb = IsSrgb(td.Format);
    if (FAILED(m_device->CreateShaderResourceView(texture, nullptr, &view.srv)))
        return nullptr;

    return &(m_srvCache[texture] = std::move(view));
}

ID3D11RenderTargetView* SbsComposer::GetRtv(ID3D11Texture2D* target)
{
    auto it = m_rtvCache.find(target);
    if (it != m_rtvCache.end())
        return it->second.Get();

    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(m_device->CreateRenderTargetView(target, nullptr, &rtv)))
        return nullptr;

    m_rtvCache[target] = rtv;
    return rtv.Get();
}

bool SbsComposer::Compose(
    ID3D11Texture2D* target,
    const EyeLayer* leftLayers,
    const EyeLayer* rightLayers,
    uint32_t layerCount)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_context || !target || layerCount == 0)
        return false;

    ID3D11RenderTargetView* rtv = GetRtv(target);
    if (!rtv)
        return false;

    D3D11_TEXTURE2D_DESC td{};
    target->GetDesc(&td);
    const float halfW = static_cast<float>(td.Width) / 2.0f;
    const float h = static_cast<float>(td.Height);

    const float clear[4] = { 0, 0, 0, 1 };
    m_context->OMSetRenderTargets(1, &rtv, nullptr);
    m_context->ClearRenderTargetView(rtv, clear);

    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->IASetInputLayout(nullptr);
    m_context->VSSetShader(m_vs.Get(), nullptr, 0);
    m_context->PSSetShader(m_ps.Get(), nullptr, 0);
    m_context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    m_context->PSSetConstantBuffers(0, 1, m_constants.GetAddressOf());
    m_context->RSSetState(m_raster.Get());

    const float blendFactor[4] = { 1, 1, 1, 1 };

    for (uint32_t layer = 0; layer < layerCount; ++layer)
    {
        m_context->OMSetBlendState(layer == 0 ? m_blendOpaque.Get() : m_blendAlpha.Get(), blendFactor, 0xFFFFFFFF);

        for (int eye = 0; eye < 2; ++eye)
        {
            const EyeLayer& l = (eye == 0 ? leftLayers : rightLayers)[layer];
            if (!l.texture)
                continue;

            CachedView* view = GetSrv(l.texture);
            if (!view)
                continue;

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(m_context->Map(m_constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                Params p{};
                p.uvRect[0] = l.uMin; p.uvRect[1] = l.vMin; p.uvRect[2] = l.uMax; p.uvRect[3] = l.vMax;
                p.srgbEncode = view->srgb ? 1.0f : 0.0f;
                memcpy(mapped.pData, &p, sizeof(p));
                m_context->Unmap(m_constants.Get(), 0);
            }

            D3D11_VIEWPORT vp{};
            vp.TopLeftX = eye == 0 ? 0.0f : halfW;
            vp.TopLeftY = 0.0f;
            vp.Width = halfW;
            vp.Height = h;
            vp.MaxDepth = 1.0f;
            m_context->RSSetViewports(1, &vp);

            ID3D11ShaderResourceView* srv = view->srv.Get();
            m_context->PSSetShaderResources(0, 1, &srv);
            m_context->Draw(3, 0);
        }
    }

    ID3D11ShaderResourceView* nullSrv = nullptr;
    m_context->PSSetShaderResources(0, 1, &nullSrv);
    ID3D11RenderTargetView* nullRtv = nullptr;
    m_context->OMSetRenderTargets(1, &nullRtv, nullptr);
    return true;
}
