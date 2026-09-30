#pragma once

#include "openvr_driver.h"
#include "config_manager.h"
#include "sbs_composer.h"

#include <d3d11_4.h>
#include <dxgi.h>
#include <wrl.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

/// IVRDriverDirectModeComponent for the "Android Phone (USB)" display mode.
///
/// SteamVR renders each eye into textures allocated HERE (CreateSwapTextureSet), submits them
/// through SubmitLayer()/Present(), and this class:
///   1. composes [left|right] on the GPU (SbsComposer),
///   2. hands the SBS frame to VideoStreamPipeline (AMD AMF -> TCP -> adb reverse -> phone).
/// There is no desktop capture and no physical monitor involved: these are the real compositor frames.
class PixelVRDirectMode final : public vr::IVRDriverDirectModeComponent
{
public:
    struct Options
    {
        uint32_t eyeWidth = 1920;   // per-eye render target size requested from SteamVR (informational)
        uint32_t eyeHeight = 1080;
        uint32_t sbsWidth = 0;      // final stream size, both eyes (0 = eyeWidth*2)
        uint32_t sbsHeight = 0;     // 0 = eyeHeight
        float displayHz = 90.0f;
        StreamConfig stream;
    };

    PixelVRDirectMode() = default;
    ~PixelVRDirectMode();

    /// Creates the D3D11 device (AMD adapter preferred), the composer and the stream pipeline.
    bool Start(const Options& options);
    void Stop();

    /// LUID of the adapter that owns the shared textures; publish it via
    /// vr::Prop_GraphicsAdapterLuid_Uint64 so SteamVR renders on the same GPU.
    uint64_t GetAdapterLuid() const { return m_adapterLuid; }

    // --- IVRDriverDirectModeComponent ---
    void CreateSwapTextureSet(
        uint32_t unPid,
        const SwapTextureSetDesc_t* pSwapTextureSetDesc,
        SwapTextureSet_t* pOutSwapTextureSet) override;

    void DestroySwapTextureSet(vr::SharedTextureHandle_t sharedTextureHandle) override;
    void DestroyAllSwapTextureSets(uint32_t unPid) override;

    void GetNextSwapTextureSetIndex(
        vr::SharedTextureHandle_t sharedTextureHandles[2],
        uint32_t(*pIndices)[2]) override;

    void SubmitLayer(const SubmitLayerPerEye_t(&perEye)[2]) override;
    void Present(vr::SharedTextureHandle_t syncTexture) override;
    void PostPresent(const Throttling_t* pThrottling) override;

private:
    struct SwapSet
    {
        uint32_t pid = 0;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> textures[3];
        vr::SharedTextureHandle_t handles[3] = {};
    };

    static constexpr uint32_t kMaxLayers = 4;

    bool PickAdapterAndCreateDevice();
    ID3D11Texture2D* FindTexture(vr::SharedTextureHandle_t handle);
    void VsyncThreadMain();

    Options m_options;

    Microsoft::WRL::ComPtr<IDXGIAdapter1> m_adapter;
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    uint64_t m_adapterLuid = 0;

    SbsComposer m_composer;

    std::mutex m_setsMutex;
    std::vector<SwapSet> m_sets;                                   // owns textures
    std::map<vr::SharedTextureHandle_t, size_t> m_handleToSet;     // handle -> index in m_sets (rebuilt on erase)

    // Layers received via SubmitLayer since the last Present.
    SbsComposer::EyeLayer m_leftLayers[kMaxLayers];
    SbsComposer::EyeLayer m_rightLayers[kMaxLayers];
    uint32_t m_layerCount = 0;

    // Sync texture opened from the compositor (keyed mutex), cached by handle.
    vr::SharedTextureHandle_t m_syncHandle = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_syncTexture;

    std::thread m_vsyncThread;
    std::atomic<bool> m_stopVsync{ false };
    bool m_started = false;
};
