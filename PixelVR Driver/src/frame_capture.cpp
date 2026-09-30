#include "frame_capture.h"
#include "d3d11_helper.h"
#include "display_mode.h"
#include "driverlog.h"

using Microsoft::WRL::ComPtr;

FrameCapture::~FrameCapture()
{
    Shutdown();
}

void FrameCapture::Shutdown()
{
    m_duplication.Reset();
    m_output1.Reset();
    m_context.Reset();
    m_device.Reset();
    m_initialized = false;
}

bool FrameCapture::Initialize(int32_t windowX, int32_t windowY, uint32_t width, uint32_t height)
{
    Shutdown();

    width &= ~1u;
    height &= ~1u;
    if (width < 64 || height < 64)
        return false;

    DisplayOutputInfo outputInfo{};
    if (!DisplayMode::FindOutputContainingPoint(windowX, windowY, outputInfo))
        return false;

    if (!D3D11Helper::CreateDeviceForAdapter(
        outputInfo.adapterIndex,
        &m_device,
        &m_context,
        m_adapterName))
    {
        return false;
    }

    m_adapterIndex = outputInfo.adapterIndex;
    m_windowX = windowX;
    m_windowY = windowY;
    m_width = width;
    m_height = height;

    const RECT& desktop = outputInfo.desc.DesktopCoordinates;
    const int32_t cropX = windowX - desktop.left;
    const int32_t cropY = windowY - desktop.top;
    m_cropX = cropX < 0 ? 0 : static_cast<uint32_t>(cropX);
    m_cropY = cropY < 0 ? 0 : static_cast<uint32_t>(cropY);

    const uint32_t outputW = static_cast<uint32_t>(desktop.right - desktop.left);
    const uint32_t outputH = static_cast<uint32_t>(desktop.bottom - desktop.top);

    if (m_cropX + m_width > outputW)
        m_width = (outputW - m_cropX) & ~1u;
    if (m_cropY + m_height > outputH)
        m_height = (outputH - m_cropY) & ~1u;

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return false;

    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1(static_cast<UINT>(outputInfo.adapterIndex), &adapter)))
        return false;

    ComPtr<IDXGIOutput> output;
    if (FAILED(adapter->EnumOutputs(static_cast<UINT>(outputInfo.outputIndex), &output)))
        return false;

    if (FAILED(output.As(&m_output1)))
    {
        DriverLog("[Capture] IDXGIOutput1 no disponible");
        return false;
    }

    if (!RecreateDuplication())
        return false;

    m_initialized = true;
    DriverLog(
        "[Capture] DXGI duplicación lista: %ux%u crop=(%u,%u) adapter=%s",
        m_width,
        m_height,
        m_cropX,
        m_cropY,
        m_adapterName.c_str());
    return true;
}

bool FrameCapture::RecreateDuplication()
{
    m_duplication.Reset();
    if (m_output1 == nullptr || m_device == nullptr)
        return false;

    HRESULT hr = m_output1->DuplicateOutput(m_device.Get(), &m_duplication);
    if (FAILED(hr))
    {
        DriverLog("[Capture] DuplicateOutput falló (0x%08X). ¿Modo direct o captura bloqueada?", hr);
        return false;
    }

    return true;
}

bool FrameCapture::AcquireBgraFrame(std::vector<uint8_t>& bgra, uint32_t& pitch)
{
    if (!m_initialized || m_duplication == nullptr)
        return false;

    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = m_duplication->AcquireNextFrame(8, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
        return false;

    if (hr == DXGI_ERROR_ACCESS_LOST)
    {
        RecreateDuplication();
        return false;
    }

    if (FAILED(hr) || resource == nullptr)
        return false;

    ComPtr<ID3D11Texture2D> texture;
    hr = resource.As(&texture);

    bool ok = false;
    if (SUCCEEDED(hr) && texture)
    {
        ok = D3D11Helper::CopyTextureToBgraCpu(
            m_device.Get(),
            m_context.Get(),
            texture.Get(),
            m_cropX,
            m_cropY,
            m_width,
            m_height,
            bgra,
            pitch);
    }

    m_duplication->ReleaseFrame();
    return ok;
}
