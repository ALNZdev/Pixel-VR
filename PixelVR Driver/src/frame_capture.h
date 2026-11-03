#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl.h>
#include <cstdint>
#include <string>
#include <vector>

class FrameCapture
{
public:
    FrameCapture() = default;
    ~FrameCapture();

    bool Initialize(int32_t windowX, int32_t windowY, uint32_t width, uint32_t height);
    void Shutdown();

    bool AcquireBgraFrame(std::vector<uint8_t>& bgra, uint32_t& pitch);

    ID3D11Device* GetDevice() const { return m_device.Get(); }
    ID3D11DeviceContext* GetContext() const { return m_context.Get(); }
    const std::string& GetAdapterName() const { return m_adapterName; }
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }

private:
    bool RecreateDuplication();

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_duplication;
    Microsoft::WRL::ComPtr<IDXGIOutput1> m_output1;

    std::string m_adapterName;
    int m_adapterIndex = -1;

    int32_t m_windowX = 0;
    int32_t m_windowY = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_cropX = 0;
    uint32_t m_cropY = 0;
    bool m_initialized = false;
};
