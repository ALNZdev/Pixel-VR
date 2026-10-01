#include "direct_mode.h"
#include "driverlog.h"
#include "video_stream_pipeline.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <string>

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr UINT kVendorAmd = 0x1002;

    std::string WideToUtf8(const wchar_t* w)
    {
        char buf[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
        return buf;
    }
}

PixelVRDirectMode::~PixelVRDirectMode()
{
    Stop();
}

bool PixelVRDirectMode::CreateDeviceForAdapter(IDXGIAdapter1* adapter)
{
    if (!adapter)
    {
        DriverLog("[DirectMode] Adaptador AMD invalido");
        return false;
    }

    m_device.Reset();
    m_context.Reset();
    m_adapter.Reset();
    m_adapterLuid = 0;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(
        adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
        &m_device, &got, &m_context);
    if (FAILED(hr))
    {
        DriverLog("[DirectMode] D3D11CreateDevice fallo (0x%08X)", static_cast<unsigned>(hr));
        return false;
    }

    // The compositor thread (Present) and the AMF threads share this device.
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(m_device.As(&mt)))
        mt->SetMultithreadProtected(TRUE);

    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    m_adapter = adapter;
    m_adapterLuid = (static_cast<uint64_t>(desc.AdapterLuid.HighPart) << 32) | desc.AdapterLuid.LowPart;

    DriverLog("[DirectMode] Dispositivo D3D11 en '%s'", WideToUtf8(desc.Description).c_str());
    return true;
}

bool PixelVRDirectMode::Start(const Options& options)
{
    Stop();
    m_options = options;

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        DriverLog("[DirectMode] CreateDXGIFactory1 fallo");
        return false;
    }

    std::vector<ComPtr<IDXGIAdapter1>> amdAdapters;
    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;

        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || desc.VendorId != kVendorAmd)
            continue;

        DriverLog("[DirectMode] Adaptador AMD candidato %u: %s", i, WideToUtf8(desc.Description).c_str());
        amdAdapters.push_back(adapter);
    }

    if (amdAdapters.empty())
    {
        DriverLog("[DirectMode] No se encontro ningun adaptador AMD para AMF");
        return false;
    }

    const uint32_t sbsW = m_options.sbsWidth ? m_options.sbsWidth : m_options.eyeWidth * 2;
    const uint32_t sbsH = m_options.sbsHeight ? m_options.sbsHeight : m_options.eyeHeight;
    const uint32_t displayFps = static_cast<uint32_t>(m_options.displayHz + 0.5f);
    const uint32_t fps = m_options.stream.framerate > 0 ? m_options.stream.framerate : displayFps;

    for (const auto& adapter : amdAdapters)
    {
        VideoStreamPipeline::Instance().Stop();
        m_composer.Shutdown();
        if (!CreateDeviceForAdapter(adapter.Get()))
            continue;

        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        const std::string name = WideToUtf8(desc.Description);

        if (!m_composer.Initialize(m_device.Get(), m_context.Get()))
        {
            DriverLog("[DirectMode] SbsComposer no pudo inicializarse en '%s'; probando otro adaptador", name.c_str());
            m_composer.Shutdown();
            continue;
        }

        if (!VideoStreamPipeline::Instance().Start(
            m_device.Get(), m_context.Get(), m_options.stream, sbsW, sbsH, fps))
        {
            DriverLog("[DirectMode] Encoder o pipeline no disponible en '%s'; probando otro adaptador AMD", name.c_str());
            VideoStreamPipeline::Instance().Stop();
            m_composer.Shutdown();
            continue;
        }

        DriverLog("[DirectMode] Adaptador AMD seleccionado tras inicializar AMF: '%s'", name.c_str());
        m_started = true;
        m_stopVsync = false;
        m_vsyncThread = std::thread(&PixelVRDirectMode::VsyncThreadMain, this);
        return true;
    }

    DriverLog("[DirectMode] Ningun adaptador AMD pudo inicializar el compositor y el encoder");
    Stop();
    return false;
}

void PixelVRDirectMode::Stop()
{
    m_stopVsync = true;
    if (m_vsyncThread.joinable())
        m_vsyncThread.join();

    VideoStreamPipeline::Instance().Stop();

    {
        std::lock_guard<std::mutex> lock(m_setsMutex);
        m_sets.clear();
        m_handleToSet.clear();
    }

    m_layerCount = 0;
    m_syncTexture.Reset();
    m_syncHandle = 0;
    m_composer.Shutdown();
    m_context.Reset();
    m_device.Reset();
    m_adapter.Reset();
    m_started = false;
}

// -------------------------------------------------------------------------------------------------
// Swap texture sets
// -------------------------------------------------------------------------------------------------

void PixelVRDirectMode::CreateSwapTextureSet(
    uint32_t unPid,
    const SwapTextureSetDesc_t* pDesc,
    SwapTextureSet_t* pOut)
{
    if (!pDesc || !pOut || !m_device)
        return;

    *pOut = {};

    SwapSet set;
    set.pid = unPid;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = pDesc->nWidth;
    td.Height = pDesc->nHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = static_cast<DXGI_FORMAT>(pDesc->nFormat);
    td.SampleDesc.Count = 1; // resolved by the compositor; MSAA sets are not supported here
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED; // legacy shared handle, opened by the compositor

    for (int i = 0; i < 3; ++i)
    {
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &set.textures[i])))
        {
            DriverLog("[DirectMode] CreateTexture2D fallo (%ux%u fmt %u)", td.Width, td.Height, static_cast<unsigned>(td.Format));
            return;
        }

        ComPtr<IDXGIResource> res;
        HANDLE shared = nullptr;
        if (FAILED(set.textures[i].As(&res)) || FAILED(res->GetSharedHandle(&shared)) || !shared)
        {
            DriverLog("[DirectMode] GetSharedHandle fallo");
            return;
        }

        set.handles[i] = static_cast<vr::SharedTextureHandle_t>(reinterpret_cast<uintptr_t>(shared));
        pOut->rSharedTextureHandles[i] = set.handles[i];
    }
    pOut->unTextureFlags = 0;

    std::lock_guard<std::mutex> lock(m_setsMutex);
    m_sets.push_back(std::move(set));
    m_handleToSet.clear();
    for (size_t i = 0; i < m_sets.size(); ++i)
        for (auto h : m_sets[i].handles)
            m_handleToSet[h] = i;
}

ID3D11Texture2D* PixelVRDirectMode::FindTexture(vr::SharedTextureHandle_t handle)
{
    std::lock_guard<std::mutex> lock(m_setsMutex);
    auto it = m_handleToSet.find(handle);
    if (it == m_handleToSet.end())
        return nullptr;

    const SwapSet& set = m_sets[it->second];
    for (int i = 0; i < 3; ++i)
        if (set.handles[i] == handle)
            return set.textures[i].Get();
    return nullptr;
}

void PixelVRDirectMode::DestroySwapTextureSet(vr::SharedTextureHandle_t sharedTextureHandle)
{
    std::lock_guard<std::mutex> lock(m_setsMutex);
    auto it = m_handleToSet.find(sharedTextureHandle);
    if (it == m_handleToSet.end())
        return;

    const size_t index = it->second;
    for (auto& tex : m_sets[index].textures)
        m_composer.Forget(tex.Get());

    m_sets.erase(m_sets.begin() + index);
    m_handleToSet.clear();
    for (size_t i = 0; i < m_sets.size(); ++i)
        for (auto h : m_sets[i].handles)
            m_handleToSet[h] = i;
}

void PixelVRDirectMode::DestroyAllSwapTextureSets(uint32_t unPid)
{
    std::lock_guard<std::mutex> lock(m_setsMutex);
    for (size_t i = m_sets.size(); i-- > 0;)
    {
        if (m_sets[i].pid != unPid)
            continue;
        for (auto& tex : m_sets[i].textures)
            m_composer.Forget(tex.Get());
        m_sets.erase(m_sets.begin() + i);
    }
    m_handleToSet.clear();
    for (size_t i = 0; i < m_sets.size(); ++i)
        for (auto h : m_sets[i].handles)
            m_handleToSet[h] = i;
}

void PixelVRDirectMode::GetNextSwapTextureSetIndex(
    vr::SharedTextureHandle_t /*sharedTextureHandles*/[2],
    uint32_t(*pIndices)[2])
{
    // The compositor passes the indices it just used; advance both round-robin (3 textures per set).
    (*pIndices)[0] = ((*pIndices)[0] + 1) % 3;
    (*pIndices)[1] = ((*pIndices)[1] + 1) % 3;
}

// -------------------------------------------------------------------------------------------------
// Frame submission
// -------------------------------------------------------------------------------------------------

void PixelVRDirectMode::SubmitLayer(const SubmitLayerPerEye_t(&perEye)[2])
{
    if (m_layerCount >= kMaxLayers)
        return;

    ID3D11Texture2D* left = FindTexture(perEye[0].hTexture);
    ID3D11Texture2D* right = FindTexture(perEye[1].hTexture);
    if (!left || !right)
        return;

    auto fill = [](SbsComposer::EyeLayer& dst, ID3D11Texture2D* tex, const vr::VRTextureBounds_t& b) {
        dst.texture = tex;
        dst.uMin = b.uMin; dst.vMin = b.vMin;
        dst.uMax = b.uMax; dst.vMax = b.vMax;
    };
    fill(m_leftLayers[m_layerCount], left, perEye[0].bounds);
    fill(m_rightLayers[m_layerCount], right, perEye[1].bounds);
    ++m_layerCount;
}

void PixelVRDirectMode::Present(vr::SharedTextureHandle_t syncTexture)
{
    const uint32_t layerCount = m_layerCount;
    m_layerCount = 0;

    if (!m_device || layerCount == 0)
        return;

    // The sync texture carries a keyed mutex: acquiring it tells the compositor the driver has
    // taken ownership of the submitted frame and it may render the next one.
    ComPtr<IDXGIKeyedMutex> mutex;
    if (syncTexture != 0)
    {
        if (syncTexture != m_syncHandle || !m_syncTexture)
        {
            m_syncTexture.Reset();
            HANDLE h = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(syncTexture));
            if (SUCCEEDED(m_device->OpenSharedResource(h, IID_PPV_ARGS(&m_syncTexture))))
                m_syncHandle = syncTexture;
        }
        if (m_syncTexture)
            m_syncTexture.As(&mutex);
    }

    bool acquired = false;
    if (mutex)
        acquired = SUCCEEDED(mutex->AcquireSync(0, 10));

    // Compose only if a phone is connected; otherwise Present costs (almost) nothing.
    VideoStreamPipeline& pipeline = VideoStreamPipeline::Instance();
    int slot = -1;
    if (ID3D11Texture2D* target = pipeline.AcquireTarget(slot))
    {
        if (m_composer.Compose(target, m_leftLayers, m_rightLayers, layerCount))
        {
            m_context->Flush(); // make sure the GPU work is queued before the encoder thread reads it
            pipeline.SubmitTarget(slot);
        }
        else
        {
            pipeline.CancelTarget(slot);
        }
    }

    if (acquired)
        mutex->ReleaseSync(0);

    for (uint32_t i = 0; i < kMaxLayers; ++i)
    {
        m_leftLayers[i] = {};
        m_rightLayers[i] = {};
    }
}

void PixelVRDirectMode::PostPresent(const Throttling_t* /*pThrottling*/)
{
}

// -------------------------------------------------------------------------------------------------
// Vsync: the driver owns the display clock in direct mode.
// -------------------------------------------------------------------------------------------------

void PixelVRDirectMode::VsyncThreadMain()
{
    const double hz = m_options.displayHz > 1.0f ? m_options.displayHz : 90.0;
    const auto period = std::chrono::duration<double>(1.0 / hz);
    auto next = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);

    while (!m_stopVsync)
    {
        std::this_thread::sleep_until(next);
        vr::VRServerDriverHost()->VsyncEvent(0.0);
        next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);

        // Do not accumulate a backlog after a long stall.
        const auto now = std::chrono::steady_clock::now();
        if (next < now)
            next = now;
    }
}
