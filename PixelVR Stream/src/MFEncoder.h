#pragma once

#include <string>
#include <vector>
#include <wrl.h>
#include <d3d11.h>

// Media Foundation headers
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <mfreadwrite.h>

class MFEncoder
{
public:
    MFEncoder();
    ~MFEncoder();

    bool Init(int width, int height, int fps, int bitrateKbps, int gopSeconds);
    void Shutdown();

    // Submit a D3D11 texture (ID3D11Texture2D*). If the encoder produces encoded data,
    // it will be appended to outNalUnits (vector of byte arrays).
    // rtTimestampUs: presentation timestamp in microseconds (optional).
    bool SubmitFrame(ID3D11Texture2D* texture, std::vector<std::vector<uint8_t>>& outNalUnits, int64_t rtTimestampUs = 0);

    bool RequestKeyframe();

    const std::string& LastError() const { return m_lastError; }

    // ---- NEW: accessors to share D3D11 device/context with capture code ----
    // Return AddRef'ed pointers (caller must Release).
    ID3D11Device* GetD3D11Device();
    ID3D11DeviceContext* GetD3D11DeviceContext();

private:
    bool CreateD3D11Device();
    bool CreateDeviceManager();
    bool CreateEncoderMFT();
    bool ConfigureEncoder(int width, int height, int fps, int bitrateKbps, int gopSeconds);
    bool ProcessOutput(std::vector<std::vector<uint8_t>>& outNalUnits);

    HRESULT CreateSampleFromTexture(ID3D11Texture2D* texture, IMFSample** outSample);
    bool ConvertTextureToNV12AndCreateSample(ID3D11Texture2D* texture, IMFSample** outSample);

private:
    std::string m_lastError;
    Microsoft::WRL::ComPtr<ID3D11Device> m_d3dDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_d3dContext;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> m_dxgiDeviceManager;
    UINT m_token = 0;
    Microsoft::WRL::ComPtr<IMFTransform> m_encoder;
    int m_width = 0;
    int m_height = 0;
    int m_fps = 0;
    int m_bitrateKbps = 0;
    int m_gopSeconds = 0;
    bool m_initialized = false;
    bool m_requestKeyframe = false;
};