#include "MFEncoder.h"
#include <mfapi.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <vector>
#include <sstream>
#include <chrono>
#include <cstdarg>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

// Forward-declare IMFTransform if headers on some SDK variants didn't expose it early enough.
// This avoids "IMFTransform undefined" compile errors in some environments.
struct IMFTransform;

static void SetLastError(std::string& out, const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
#if defined(_MSC_VER)
    vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
#else
    vsnprintf(buf, sizeof(buf), fmt, ap);
#endif
    va_end(ap);
    out = buf;
}

MFEncoder::MFEncoder()
{
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr))
    {
        m_lastError = "MFStartup failed";
    }
}

MFEncoder::~MFEncoder()
{
    Shutdown();
    MFShutdown();
}

bool MFEncoder::Init(int width, int height, int fps, int bitrateKbps, int gopSeconds)
{
    if (m_initialized) return true;

    m_width = width;
    m_height = height;
    m_fps = fps;
    m_bitrateKbps = bitrateKbps;
    m_gopSeconds = gopSeconds;

    if (!CreateD3D11Device()) return false;
    if (!CreateDeviceManager()) return false;
    if (!CreateEncoderMFT()) return false;
    if (!ConfigureEncoder(width, height, fps, bitrateKbps, gopSeconds)) return false;

    m_initialized = true;
    return true;
}

void MFEncoder::Shutdown()
{
    if (m_encoder)
    {
        // Some MFTs accept these messages, but they are optional; ignore failures.
#ifdef MFT_MESSAGE_COMMAND_FLUSH
        m_encoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
#endif
#ifdef MFT_MESSAGE_NOTIFY_END_OF_STREAM
        m_encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
#endif
    }
    m_encoder.Reset();
    m_dxgiDeviceManager.Reset();
    m_d3dContext.Reset();
    m_d3dDevice.Reset();
    m_initialized = false;
}

bool MFEncoder::RequestKeyframe()
{
    if (!m_initialized) return false;
    m_requestKeyframe = true;
    return true;
}

// ---- NEW: return AddRef'ed device/context for sharing ----
ID3D11Device* MFEncoder::GetD3D11Device()
{
    if (!m_d3dDevice) return nullptr;
    ID3D11Device* d = m_d3dDevice.Get();
    d->AddRef();
    return d;
}

ID3D11DeviceContext* MFEncoder::GetD3D11DeviceContext()
{
    if (!m_d3dContext) return nullptr;
    ID3D11DeviceContext* c = m_d3dContext.Get();
    c->AddRef();
    return c;
}

bool MFEncoder::CreateD3D11Device()
{
    if (m_d3dDevice) return true;

    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        creationFlags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &m_d3dDevice,
        &obtained,
        &m_d3dContext);

    if (FAILED(hr))
    {
        SetLastError(m_lastError, "D3D11CreateDevice failed: 0x%08x", hr);
        return false;
    }
    return true;
}

bool MFEncoder::CreateDeviceManager()
{
    if (m_dxgiDeviceManager) return true;
    UINT resetToken = 0;
    HRESULT hr = MFCreateDXGIDeviceManager(&resetToken, &m_dxgiDeviceManager);
    if (FAILED(hr))
    {
        SetLastError(m_lastError, "MFCreateDXGIDeviceManager failed: 0x%08x", hr);
        return false;
    }
    hr = m_dxgiDeviceManager->ResetDevice(m_d3dDevice.Get(), resetToken);
    if (FAILED(hr))
    {
        SetLastError(m_lastError, "DXGIDeviceManager ResetDevice failed: 0x%08x", hr);
        return false;
    }
    m_token = resetToken;
    return true;
}

bool MFEncoder::CreateEncoderMFT()
{
    IMFActivate** ppActivate = nullptr;
    UINT32 count = 0;

    MFT_REGISTER_TYPE_INFO inputType = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO outputType = { MFMediaType_Video, MFVideoFormat_H264 };

    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        &inputType,
        &outputType,
        &ppActivate,
        &count);

    if (FAILED(hr) || count == 0)
    {
        hr = MFTEnumEx(
            MFT_CATEGORY_VIDEO_ENCODER,
            MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
            &inputType,
            &outputType,
            &ppActivate,
            &count);
        if (FAILED(hr) || count == 0)
        {
            SetLastError(m_lastError, "No H.264 encoder MFT found on this system.");
            return false;
        }
    }

    IMFActivate* pActivate = ppActivate[0];
    hr = pActivate->ActivateObject(IID_PPV_ARGS(&m_encoder));
    for (UINT32 i = 0; i < count; ++i)
    {
        if (ppActivate[i]) ppActivate[i]->Release();
    }
    CoTaskMemFree(ppActivate);

    if (FAILED(hr) || !m_encoder)
    {
        SetLastError(m_lastError, "Activate encoder MFT failed: 0x%08x", hr);
        return false;
    }

    // Try to set DXGI device manager on encoder via IMFAttributes if supported.
    ComPtr<IMFAttributes> attr;
    if (SUCCEEDED(m_encoder.As(&attr)))
    {
#ifdef MF_SA_D3D11_AWARE
        // Some MFTs accept the DXGI device manager via this attribute.
        attr->SetUnknown(MF_SA_D3D11_AWARE, m_dxgiDeviceManager.Get());
#endif
    }

    return true;
}

bool MFEncoder::ConfigureEncoder(int width, int height, int fps, int bitrateKbps, int gopSeconds)
{
    if (!m_encoder)
    {
        SetLastError(m_lastError, "Encoder MFT not created.");
        return false;
    }

    HRESULT hr = S_OK;
    ComPtr<IMFMediaType> inputType;
    hr = MFCreateMediaType(&inputType);
    if (FAILED(hr)) { SetLastError(m_lastError, "MFCreateMediaType input failed"); return false; }
    hr = inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    hr = inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    hr = MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, (UINT32)width, (UINT32)height);
    hr = MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, (UINT32)fps, 1);
    hr = inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(hr)) { SetLastError(m_lastError, "Set input media type attributes failed"); return false; }

    hr = m_encoder->SetInputType(0, inputType.Get(), 0);
    if (FAILED(hr))
    {
        SetLastError(m_lastError, "SetInputType failed: 0x%08x", hr);
        return false;
    }

    ComPtr<IMFMediaType> outputType;
    hr = MFCreateMediaType(&outputType);
    if (FAILED(hr)) { SetLastError(m_lastError, "MFCreateMediaType output failed"); return false; }
    hr = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    hr = outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    hr = MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, (UINT32)width, (UINT32)height);
    hr = MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, (UINT32)fps, 1);
    hr = outputType->SetUINT32(MF_MT_AVG_BITRATE, bitrateKbps * 1000);

    hr = m_encoder->SetOutputType(0, outputType.Get(), 0);
    if (FAILED(hr))
    {
        SetLastError(m_lastError, "SetOutputType failed: 0x%08x", hr);
        return false;
    }

    // Some MFTs can accept begin/start messages; not all SDKs define the macros, so skip if undefined.
#ifdef MFT_MESSAGE_NOTIFY_BEGIN_STREAM
    m_encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAM, 0);
#endif
#ifdef MFT_MESSAGE_NOTIFY_START_OF_STREAM
    m_encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
#endif

    return true;
}

HRESULT MFEncoder::CreateSampleFromTexture(ID3D11Texture2D* texture, IMFSample** outSample)
{
    if (!texture || !outSample) return E_INVALIDARG;
    *outSample = nullptr;

    ComPtr<IDXGISurface> dxgiSurface;
    HRESULT hr = texture->QueryInterface(__uuidof(IDXGISurface), reinterpret_cast<void**>(dxgiSurface.GetAddressOf()));
    if (SUCCEEDED(hr) && dxgiSurface)
    {
        ComPtr<IMFMediaBuffer> mediaBuffer;
        hr = MFCreateDXGISurfaceBuffer(__uuidof(IDXGISurface), dxgiSurface.Get(), 0, FALSE, mediaBuffer.GetAddressOf());
        if (SUCCEEDED(hr))
        {
            ComPtr<IMFSample> sample;
            hr = MFCreateSample(sample.GetAddressOf());
            if (FAILED(hr)) return hr;
            hr = sample->AddBuffer(mediaBuffer.Get());
            if (FAILED(hr)) return hr;

            *outSample = sample.Detach();
            return S_OK;
        }
    }

    return E_FAIL;
}

bool MFEncoder::ConvertTextureToNV12AndCreateSample(ID3D11Texture2D* texture, IMFSample** outSample)
{
    if (!texture || !outSample) return false;
    *outSample = nullptr;

    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = m_d3dDevice->CreateTexture2D(&stagingDesc, nullptr, staging.GetAddressOf());
    if (FAILED(hr))
    {
        SetLastError(m_lastError, "Create staging texture failed: 0x%08x", hr);
        return false;
    }

    // IMPORTANT: texture is a raw ID3D11Texture2D*, not a ComPtr here.
    // Use the raw pointer directly (not texture.Get()).
    m_d3dContext->CopyResource(staging.Get(), texture);

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = m_d3dContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr))
    {
        SetLastError(m_lastError, "Map staging failed: 0x%08x", hr);
        return false;
    }

    int w = (int)desc.Width;
    int h = (int)desc.Height;
    std::vector<uint8_t> nv12;
    nv12.resize(w * h + (w * h) / 2);

    uint8_t* src = reinterpret_cast<uint8_t*>(mapped.pData);
    int srcPitch = mapped.RowPitch;

    uint8_t* yPlane = nv12.data();
    uint8_t* uvPlane = nv12.data() + w * h;

    for (int row = 0; row < h; ++row)
    {
        uint8_t* srcRow = src + row * srcPitch;
        for (int col = 0; col < w; ++col)
        {
            int idx = col * 4;
            uint8_t b = srcRow[idx + 0];
            uint8_t g = srcRow[idx + 1];
            uint8_t r = srcRow[idx + 2];
            uint8_t Y = static_cast<uint8_t>((0.257f * r) + (0.504f * g) + (0.098f * b) + 16);
            yPlane[row * w + col] = Y;
        }
    }

    for (int row = 0; row < h; row += 2)
    {
        uint8_t* srcRow0 = src + row * srcPitch;
        uint8_t* srcRow1 = src + (row + 1) * srcPitch;
        for (int col = 0; col < w; col += 2)
        {
            int idx0 = col * 4;
            int idx1 = idx0 + 4;
            uint8_t b0 = srcRow0[idx0 + 0], g0 = srcRow0[idx0 + 1], r0 = srcRow0[idx0 + 2];
            uint8_t b1 = srcRow0[idx1 + 0], g1 = srcRow0[idx1 + 1], r1 = srcRow0[idx1 + 2];
            uint8_t b2 = srcRow1[idx0 + 0], g2 = srcRow1[idx0 + 1], r2 = srcRow1[idx0 + 2];
            uint8_t b3 = srcRow1[idx1 + 0], g3 = srcRow1[idx1 + 1], r3 = srcRow1[idx1 + 2];

            int rAvg = (r0 + r1 + r2 + r3) / 4;
            int gAvg = (g0 + g1 + g2 + g3) / 4;
            int bAvg = (b0 + b1 + b2 + b3) / 4;

            uint8_t U = static_cast<uint8_t>((-0.148f * rAvg) - (0.291f * gAvg) + (0.439f * bAvg) + 128);
            uint8_t V = static_cast<uint8_t>((0.439f * rAvg) - (0.368f * gAvg) - (0.071f * bAvg) + 128);

            int uvRow = (row / 2);
            int uvCol = col;
            uvPlane[uvRow * w + uvCol] = U;
            if (uvCol + 1 < w) uvPlane[uvRow * w + uvCol + 1] = V;
        }
    }

    m_d3dContext->Unmap(staging.Get(), 0);

    IMFMediaBuffer* buffer = nullptr;
    HRESULT hr2 = MFCreateMemoryBuffer(static_cast<DWORD>(nv12.size()), &buffer);
    if (FAILED(hr2))
    {
        SetLastError(m_lastError, "MFCreateMemoryBuffer failed: 0x%08x", hr2);
        return false;
    }

    BYTE* dest = nullptr;
    hr2 = buffer->Lock(&dest, nullptr, nullptr);
    if (SUCCEEDED(hr2))
    {
        memcpy(dest, nv12.data(), nv12.size());
        buffer->Unlock();
        buffer->SetCurrentLength(static_cast<DWORD>(nv12.size()));
    }
    else
    {
        buffer->Release();
        SetLastError(m_lastError, "Buffer Lock failed");
        return false;
    }

    IMFSample* sample = nullptr;
    hr2 = MFCreateSample(&sample);
    if (FAILED(hr2))
    {
        buffer->Release();
        SetLastError(m_lastError, "MFCreateSample failed");
        return false;
    }

    sample->AddBuffer(buffer);
    buffer->Release();
    *outSample = sample;
    return true;
}

bool MFEncoder::SubmitFrame(ID3D11Texture2D* texture, std::vector<std::vector<uint8_t>>& outNalUnits, int64_t rtTimestampUs)
{
    if (!m_initialized)
    {
        SetLastError(m_lastError, "Encoder not initialized");
        return false;
    }

    if (!texture)
    {
        SetLastError(m_lastError, "NULL texture");
        return false;
    }

    IMFSample* sample = nullptr;
    HRESULT hr = CreateSampleFromTexture(texture, &sample);
    if (FAILED(hr) || !sample)
    {
        if (!ConvertTextureToNV12AndCreateSample(texture, &sample))
        {
            SetLastError(m_lastError, "Failed to create sample from texture");
            return false;
        }
    }

    if (rtTimestampUs > 0)
    {
        // Media Foundation expects 100-nanosecond units
        LONGLONG time100ns = rtTimestampUs * 10;
        sample->SetSampleTime(time100ns);
    }

    hr = m_encoder->ProcessInput(0, sample, 0);
    sample->Release();

    if (hr == MF_E_NOTACCEPTING)
    {
        // encoder needs ProcessOutput to drain
    }
    else if (FAILED(hr))
    {
        SetLastError(m_lastError, "ProcessInput failed: 0x%08x", hr);
        return false;
    }

    // Try to get output
    if (!ProcessOutput(outNalUnits))
    {
        // no output available yet is acceptable
    }

    m_requestKeyframe = false;
    return true;
}

bool MFEncoder::ProcessOutput(std::vector<std::vector<uint8_t>>& outNalUnits)
{
    if (!m_encoder) return false;

    MFT_OUTPUT_STREAM_INFO streamInfo;
    memset(&streamInfo, 0, sizeof(streamInfo));
    HRESULT hr = m_encoder->GetOutputStreamInfo(0, &streamInfo);

    MFT_OUTPUT_DATA_BUFFER outputDataBuffer;
    memset(&outputDataBuffer, 0, sizeof(outputDataBuffer));

    IMFSample* outSample = nullptr;
    hr = MFCreateSample(&outSample);
    if (FAILED(hr)) return false;

    IMFMediaBuffer* outBuffer = nullptr;
    DWORD bufSize = 1024 * 1024;
    hr = MFCreateMemoryBuffer(bufSize, &outBuffer);
    if (FAILED(hr))
    {
        outSample->Release();
        return false;
    }
    outSample->AddBuffer(outBuffer);
    outBuffer->Release();

    outputDataBuffer.dwStreamID = 0;
    outputDataBuffer.pSample = outSample;
    outputDataBuffer.dwStatus = 0;
    outputDataBuffer.pEvents = nullptr;

    DWORD status = 0;
    hr = m_encoder->ProcessOutput(0, 1, &outputDataBuffer, &status);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
    {
        outSample->Release();
        return true;
    }
    else if (FAILED(hr))
    {
        outSample->Release();
        return false;
    }

    IMFMediaBuffer* mediaBuffer = nullptr;
    hr = outputDataBuffer.pSample->ConvertToContiguousBuffer(&mediaBuffer);
    if (SUCCEEDED(hr) && mediaBuffer)
    {
        BYTE* data = nullptr;
        DWORD maxLen = 0, curLen = 0;
        if (SUCCEEDED(mediaBuffer->Lock(&data, &maxLen, &curLen)))
        {
            std::vector<uint8_t> nal(data, data + curLen);
            outNalUnits.push_back(std::move(nal));
            mediaBuffer->Unlock();
        }
        mediaBuffer->Release();
    }

    if (outputDataBuffer.pSample) outputDataBuffer.pSample->Release();
    return true;
}