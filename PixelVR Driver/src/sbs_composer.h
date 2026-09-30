#pragma once

#include <d3d11.h>
#include <wrl.h>

#include <cstdint>
#include <mutex>
#include <unordered_map>

/// GPU-only side-by-side composer.
///
/// Draws the left/right eye textures (as delivered by the SteamVR compositor through
/// IVRDriverDirectModeComponent::SubmitLayer) into one B8G8R8A8_UNORM render target
/// laid out as [ left | right ], scaling to the target size and honouring texture bounds.
/// If the eye texture is an *_SRGB format the shader re-encodes to sRGB so bytes are
/// passed through unchanged (the AMF converter expects gamma-encoded BGRA).
class SbsComposer
{
public:
    struct EyeLayer
    {
        ID3D11Texture2D* texture = nullptr;
        float uMin = 0.f, vMin = 0.f, uMax = 1.f, vMax = 1.f;
    };

    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void Shutdown();

    /// Removes cached views for a texture that is about to be destroyed.
    void Forget(ID3D11Texture2D* texture);

    /// Clears `target` and draws each layer (index 0 opaque, the rest alpha-blended).
    bool Compose(
        ID3D11Texture2D* target,
        const EyeLayer* leftLayers,
        const EyeLayer* rightLayers,
        uint32_t layerCount);

private:
    struct CachedView
    {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        bool srgb = false;
    };

    CachedView* GetSrv(ID3D11Texture2D* texture);
    ID3D11RenderTargetView* GetRtv(ID3D11Texture2D* target);

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constants;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendOpaque;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendAlpha;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_raster;

    std::mutex m_mutex; // Forget() comes from SteamVR's server thread, Compose() from the compositor thread
    std::unordered_map<ID3D11Texture2D*, CachedView> m_srvCache;
    std::unordered_map<ID3D11Texture2D*, Microsoft::WRL::ComPtr<ID3D11RenderTargetView>> m_rtvCache;
};
