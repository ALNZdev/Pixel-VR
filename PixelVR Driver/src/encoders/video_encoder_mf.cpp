#include "video_encoder_mf.h"
#include "driverlog.h"

#include <initguid.h>
#include <codecapi.h>
#include <mferror.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <array>
#include <cstring>

using Microsoft::WRL::ComPtr;

// wmcodecdsp.h ya fue incluido (via video_encoder_mf.h) antes de initguid.h, por lo que su
// DEFINE_GUID solo declara el simbolo. No existe una .lib que lo defina, asi que lo definimos aqui.
// {6CA50344-051A-4DED-9779-A43305165E35}
extern "C" const GUID CLSID_CMSH264EncoderMFT =
{ 0x6ca50344, 0x051a, 0x4ded, { 0x97, 0x79, 0xa4, 0x33, 0x05, 0x16, 0x5e, 0x35 } };

namespace
{
    void AppendAnnexBNal(std::vector<uint8_t>& out, const uint8_t* nal, size_t size)
    {
        static const uint8_t startCode[] = { 0, 0, 0, 1 };
        out.insert(out.end(), startCode, startCode + 4);
        out.insert(out.end(), nal, nal + size);
    }
}

MediaFoundationEncoder::MediaFoundationEncoder(bool preferHardware)
    : m_preferHardware(preferHardware)
{
    m_info.name = preferHardware ? "Media Foundation hardware" : "Media Foundation software";
    m_info.isHardware = preferHardware;
}

MediaFoundationEncoder::~MediaFoundationEncoder()
{
    Shutdown();
}

bool MediaFoundationEncoder::Initialize(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    uint32_t width,
    uint32_t height,
    uint32_t bitrateKbps,
    uint32_t framerate,
    VideoCodec codec)
{
    Shutdown();

    width &= ~1u;
    height &= ~1u;
    if (!device || !context || width < 64 || height < 64)
    {
        DriverLog("[MF] Requiere dispositivo y contexto D3D11, y tamaño par >= 64");
        return false;
    }

    m_device = device;
    m_context = context;
    HRESULT hr = context->QueryInterface(IID_PPV_ARGS(&m_multithread));
    if (FAILED(hr) || !m_multithread)
    {
        DriverLog("[MF] El contexto D3D11 no permite sincronizacion multihilo (%08X)", hr);
        Shutdown();
        return false;
    }
    m_multithread->SetMultithreadProtected(TRUE);
    D3D11_TEXTURE2D_DESC stagingDesc{};
    stagingDesc.Width = width;
    stagingDesc.Height = height;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = m_device->CreateTexture2D(&stagingDesc, nullptr, &m_stagingTexture);
    if (FAILED(hr))
    {
        DriverLog("[MF] No se pudo crear staging texture (%08X)", hr);
        Shutdown();
        return false;
    }

    if (framerate == 0)
        framerate = 60;

    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(comHr))
        m_comStarted = true;
    else if (comHr != RPC_E_CHANGED_MODE)
    {
        DriverLog("[MF] No se pudo inicializar COM (%08X)", comHr);
        Shutdown();
        return false;
    }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr) && hr != MF_E_ALREADY_INITIALIZED)
    {
        DriverLog("[MF] MFStartup falló (0x%08X)", hr);
        return false;
    }
    m_mfStarted = true;

    MFT_REGISTER_TYPE_INFO outputInfo{};
    outputInfo.guidMajorType = MFMediaType_Video;
    outputInfo.guidSubtype = codec == VideoCodec::H264 ? MFVideoFormat_H264 : MFVideoFormat_HEVC;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    const DWORD encoderFlags = m_preferHardware
        ? MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER
        : MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER;
    hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER, encoderFlags, nullptr, &outputInfo, &activates, &count);

    m_info.isHardware = m_preferHardware;
    m_info.name = codec == VideoCodec::H264
        ? (m_preferHardware ? "Media Foundation H.264 (GPU)" : "Media Foundation H.264 (CPU)")
        : (m_preferHardware ? "Media Foundation HEVC (GPU)" : "Media Foundation HEVC (CPU)");

    if (SUCCEEDED(hr) && count > 0 && activates != nullptr)
    {
        hr = activates[0]->ActivateObject(IID_PPV_ARGS(&m_encoder));
        for (UINT32 i = 0; i < count; ++i)
            activates[i]->Release();
        CoTaskMemFree(activates);
    }
    else if (!m_preferHardware && codec == VideoCodec::H264)
    {
        hr = CoCreateInstance(
            CLSID_CMSH264EncoderMFT,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&m_encoder));
        m_info.isHardware = false;
        m_info.name = "Media Foundation H.264 (software)";
    }

    if (FAILED(hr) || m_encoder == nullptr)
    {
        DriverLog("[MF] No se encontró/activó el MFT %s para %s (HRESULT 0x%08X)",
            m_preferHardware ? "de hardware" : "de software",
            codec == VideoCodec::H264 ? "H.264" : "HEVC", static_cast<unsigned>(hr));
        Shutdown();
        return false;
    }

    ComPtr<IMFAttributes> encoderAttrs;
    if (SUCCEEDED(m_encoder->GetAttributes(&encoderAttrs)) && encoderAttrs)
    {
        UINT32 async = 0;
        encoderAttrs->GetUINT32(MF_TRANSFORM_ASYNC, &async);
        m_async = async != 0;
        if (m_async)
        {
            encoderAttrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            m_encoder.As(&m_events);
        }
    }

    if (m_preferHardware)
    {
        ComPtr<IMFAttributes> attrs;
        UINT32 d3d11Aware = FALSE;
        if (SUCCEEDED(m_encoder->GetAttributes(&attrs)) && attrs &&
            SUCCEEDED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &d3d11Aware)) && d3d11Aware)
        {
            UINT resetToken = 0;
            hr = MFCreateDXGIDeviceManager(&resetToken, &m_dxgiManager);
            if (SUCCEEDED(hr))
                hr = m_dxgiManager->ResetDevice(m_device.Get(), resetToken);
            if (SUCCEEDED(hr))
                hr = m_encoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                    reinterpret_cast<ULONG_PTR>(m_dxgiManager.Get()));
            if (FAILED(hr))
            {
                DriverLog("[MF] No se pudo enlazar el encoder con D3D11 (%08X)", static_cast<unsigned>(hr));
                Shutdown();
                return false;
            }
        }
    }

    DWORD inputStreams = 0;
    DWORD outputStreams = 0;
    m_encoder->GetStreamCount(&inputStreams, &outputStreams);

    DWORD inputIds[8]{};
    DWORD outputIds[8]{};
    if (SUCCEEDED(m_encoder->GetStreamIDs(8, inputIds, 8, outputIds)))
    {
        m_inputStreamId = inputIds[0];
        m_outputStreamId = outputIds[0];
    }

    ComPtr<IMFMediaType> outputType;
    MFCreateMediaType(&outputType);
    outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outputType->SetGUID(MF_MT_SUBTYPE, codec == VideoCodec::H264 ? MFVideoFormat_H264 : MFVideoFormat_HEVC);
    MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, framerate, 1);
    MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outputType->SetUINT32(MF_MT_AVG_BITRATE, bitrateKbps * 1000);
    if (codec == VideoCodec::H264)
        outputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
    else
        outputType->SetUINT32(MF_MT_VIDEO_PROFILE, eAVEncH265VProfile_Main_420_8);

    hr = m_encoder->SetOutputType(m_outputStreamId, outputType.Get(), 0);
    if (FAILED(hr))
    {
        DriverLog("[MF] SetOutputType falló (0x%08X)", hr);
        Shutdown();
        return false;
    }

    ComPtr<IMFMediaType> inputType;
    MFCreateMediaType(&inputType);
    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, framerate, 1);
    MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    inputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

    hr = m_encoder->SetInputType(m_inputStreamId, inputType.Get(), 0);
    if (FAILED(hr))
    {
        DriverLog("[MF] SetInputType NV12 falló (0x%08X)", hr);
        Shutdown();
        return false;
    }

    if (m_preferHardware && m_dxgiManager)
        m_gpuConverterEnabled = InitializeGpuConverter(width, height, framerate);

    ComPtr<ICodecAPI> codecApi;
    if (SUCCEEDED(m_encoder.As(&codecApi)))
    {
        VARIANT value;
        VariantInit(&value);

        value.vt = VT_UI4;
        value.ulVal = eAVEncCommonRateControlMode_CBR;
        codecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &value);

        value.ulVal = bitrateKbps * 1000;
        codecApi->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &value);

        value.ulVal = framerate;
        codecApi->SetValue(&CODECAPI_AVEncMPVGOPSize, &value);

        value.ulVal = 0; // Prefer encoding speed on the CPU fallback.
        codecApi->SetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &value);

        VARIANT lowLatency;
        VariantInit(&lowLatency);
        lowLatency.vt = VT_BOOL;
        lowLatency.boolVal = VARIANT_TRUE;
        codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &lowLatency);
    }

    m_encoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    m_encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    m_info.width = width;
    m_info.height = height;
    m_info.bitrateKbps = bitrateKbps;
    m_info.framerate = framerate;
    m_info.codec = codec;
    m_info.isInitialized = true;
    m_timestamp = 0;
    m_forceKeyframe = true;

    DriverLog(
        "[MF] Encoder listo: %ux%u @ %u kbps %u fps (%s, conversión GPU %s)",
        width,
        height,
        bitrateKbps,
        framerate,
        m_info.name.c_str(),
        m_gpuConverterEnabled ? "activa" : "inactiva");
    return true;
}

bool MediaFoundationEncoder::InitializeGpuConverter(uint32_t width, uint32_t height, uint32_t framerate)
{
    HRESULT hr = m_device.As(&m_videoDevice);
    if (SUCCEEDED(hr))
        hr = m_context.As(&m_videoContext);
    if (FAILED(hr) || !m_videoDevice || !m_videoContext)
    {
        DriverLog("[MF] Video Processor D3D11 no disponible (%08X)", static_cast<unsigned>(hr));
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = { framerate, 1 };
    content.InputWidth = width;
    content.InputHeight = height;
    content.OutputFrameRate = { framerate, 1 };
    content.OutputWidth = width;
    content.OutputHeight = height;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    hr = m_videoDevice->CreateVideoProcessorEnumerator(&content, &m_videoEnumerator);
    if (FAILED(hr) || !m_videoEnumerator)
    {
        DriverLog("[MF] No se pudo crear el enumerador Video Processor (%08X)", static_cast<unsigned>(hr));
        return false;
    }

    UINT bgraSupport = 0;
    UINT nv12Support = 0;
    hr = m_videoEnumerator->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &bgraSupport);
    if (SUCCEEDED(hr))
        hr = m_videoEnumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &nv12Support);
    if (FAILED(hr) || !(bgraSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ||
        !(nv12Support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
    {
        DriverLog("[MF] Video Processor no admite BGRA→NV12 en este adaptador");
        m_videoEnumerator.Reset();
        return false;
    }

    hr = m_videoDevice->CreateVideoProcessor(m_videoEnumerator.Get(), 0, &m_videoProcessor);
    if (FAILED(hr) || !m_videoProcessor)
    {
        DriverLog("[MF] No se pudo crear Video Processor (%08X)", static_cast<unsigned>(hr));
        m_videoEnumerator.Reset();
        return false;
    }

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColor{};
    inputColor.RGB_Range = 0; // BGRA input uses full-range RGB.
    inputColor.YCbCr_Matrix = 1; // BT.709 matrix.
    m_videoContext->VideoProcessorSetStreamColorSpace(m_videoProcessor.Get(), 0, &inputColor);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColor{};
    outputColor.YCbCr_Matrix = 1; // BT.709 matrix.
    outputColor.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    m_videoContext->VideoProcessorSetOutputColorSpace(m_videoProcessor.Get(), &outputColor);

    for (GpuNv12Slot& slot : m_gpuNv12Slots)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        hr = m_device->CreateTexture2D(&desc, nullptr, &slot.texture);
        if (FAILED(hr))
            break;

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC viewDesc{};
        viewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        viewDesc.Texture2D.MipSlice = 0;
        hr = m_videoDevice->CreateVideoProcessorOutputView(
            slot.texture.Get(), m_videoEnumerator.Get(), &viewDesc, &slot.outputView);
        if (FAILED(hr))
            break;
    }

    if (FAILED(hr))
    {
        DriverLog("[MF] No se pudo crear la superficie NV12 GPU (%08X); se usará conversión CPU", static_cast<unsigned>(hr));
        for (GpuNv12Slot& slot : m_gpuNv12Slots)
        {
            slot.texture.Reset();
            slot.outputView.Reset();
        }
        m_videoProcessor.Reset();
        m_videoEnumerator.Reset();
        m_videoContext.Reset();
        m_videoDevice.Reset();
        return false;
    }

    DriverLog("[MF] Conversión BGRA→NV12 configurada en GPU (%ux%u)", width, height);
    return true;
}

void MediaFoundationEncoder::AvccToAnnexB(std::vector<uint8_t>& data)
{
    if (data.size() < 4)
        return;

    // Already Annex-B
    if (data[0] == 0 && data[1] == 0 && (data[2] == 1 || (data[2] == 0 && data[3] == 1)))
        return;

    std::vector<uint8_t> annexB;
    annexB.reserve(data.size() + 16);

    size_t offset = 0;
    while (offset + 4 <= data.size())
    {
        const uint32_t nalSize =
            (static_cast<uint32_t>(data[offset]) << 24) |
            (static_cast<uint32_t>(data[offset + 1]) << 16) |
            (static_cast<uint32_t>(data[offset + 2]) << 8) |
            static_cast<uint32_t>(data[offset + 3]);
        offset += 4;

        if (nalSize == 0 || offset + nalSize > data.size())
            break;

        AppendAnnexBNal(annexB, data.data() + offset, nalSize);
        offset += nalSize;
    }

    if (!annexB.empty())
        data.swap(annexB);
}

bool MediaFoundationEncoder::DrainOutput(std::vector<uint8_t>& outputData, bool& isKeyFrame)
{
    outputData.clear();
    isKeyFrame = false;

    MFT_OUTPUT_STREAM_INFO streamInfo{};
    m_encoder->GetOutputStreamInfo(m_outputStreamId, &streamInfo);

    while (true)
    {
        MFT_OUTPUT_DATA_BUFFER buffer{};
        buffer.dwStreamID = m_outputStreamId;

        ComPtr<IMFSample> sample;
        const bool providesSample = (streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
        if (!providesSample)
        {
            MFCreateSample(&sample);
            ComPtr<IMFMediaBuffer> mediaBuffer;
            const DWORD bufSize = streamInfo.cbSize > 0 ? streamInfo.cbSize : 1024 * 1024;
            MFCreateMemoryBuffer(bufSize, &mediaBuffer);
            sample->AddBuffer(mediaBuffer.Get());
            buffer.pSample = sample.Get();
        }

        DWORD status = 0;
        HRESULT hr = m_encoder->ProcessOutput(0, 1, &buffer, &status);
        if (buffer.pEvents)
        {
            buffer.pEvents->Release();
            buffer.pEvents = nullptr;
        }

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
            return !outputData.empty();

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
            continue;

        if (FAILED(hr) || buffer.pSample == nullptr)
            return !outputData.empty();

        ComPtr<IMFSample> outSample = buffer.pSample;
        if (providesSample)
            buffer.pSample->Release();

        if (m_gpuConverterEnabled && !m_pendingGpuSlots.empty())
        {
            const size_t completedSlot = m_pendingGpuSlots.front();
            m_pendingGpuSlots.pop_front();
            if (completedSlot < m_gpuNv12Slots.size())
                m_gpuNv12Slots[completedSlot].pending = false;
        }

        UINT32 cleanPoint = 0;
        outSample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint);
        if (cleanPoint)
            isKeyFrame = true;

        ComPtr<IMFMediaBuffer> mediaBuffer;
        if (FAILED(outSample->ConvertToContiguousBuffer(&mediaBuffer)))
            continue;

        BYTE* data = nullptr;
        DWORD maxLen = 0;
        DWORD curLen = 0;
        if (FAILED(mediaBuffer->Lock(&data, &maxLen, &curLen)) || data == nullptr || curLen == 0)
            continue;

        outputData.insert(outputData.end(), data, data + curLen);
        mediaBuffer->Unlock();
    }
}

bool MediaFoundationEncoder::EncodeNv12(
    const uint8_t* nv12,
    size_t nv12Bytes,
    std::vector<uint8_t>& outputData,
    bool& isKeyFrame)
{
    outputData.clear();
    isKeyFrame = false;

    if (!m_info.isInitialized || nv12 == nullptr || m_encoder == nullptr)
        return false;

    const size_t expected = static_cast<size_t>(m_info.width) * m_info.height * 3 / 2;
    if (nv12Bytes < expected)
        return false;

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(expected), &buffer)))
        return false;

    BYTE* dest = nullptr;
    DWORD maxLen = 0;
    if (FAILED(buffer->Lock(&dest, &maxLen, nullptr)))
        return false;

    memcpy(dest, nv12, expected);
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(expected));

    ComPtr<IMFSample> sample;
    MFCreateSample(&sample);
    sample->AddBuffer(buffer.Get());

    const LONGLONG duration = 10'000'000 / static_cast<LONGLONG>(m_info.framerate == 0 ? 60 : m_info.framerate);
    sample->SetSampleTime(m_timestamp);
    sample->SetSampleDuration(duration);
    m_timestamp += duration;

    bool inputAccepted = false;
    return SubmitSample(sample.Get(), outputData, isKeyFrame, inputAccepted);
}

bool MediaFoundationEncoder::SubmitSample(
    IMFSample* sample,
    std::vector<uint8_t>& outputData,
    bool& isKeyFrame,
    bool& inputAccepted)
{
    inputAccepted = false;
    if (!sample || !m_encoder)
        return false;

    if (m_forceKeyframe)
    {
        sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE);
        m_forceKeyframe = false;
    }

    HRESULT hr = m_encoder->ProcessInput(m_inputStreamId, sample, 0);
    if (FAILED(hr))
    {
        DriverLog("[MF] ProcessInput falló (0x%08X)", hr);
        return false;
    }
    inputAccepted = true;

    if (m_async && m_events)
    {
        const DWORD start = GetTickCount();
        while (GetTickCount() - start < 40)
        {
            ComPtr<IMFMediaEvent> event;
            hr = m_events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
            if (hr == MF_E_NO_EVENTS_AVAILABLE)
            {
                Sleep(1);
                continue;
            }
            if (FAILED(hr) || event == nullptr)
                break;

            MediaEventType type = MEUnknown;
            event->GetType(&type);
            if (type == METransformHaveOutput)
            {
                DrainOutput(outputData, isKeyFrame);
                break;
            }
        }
    }
    else if (!DrainOutput(outputData, isKeyFrame))
    {
        return false;
    }

    AvccToAnnexB(outputData);
    return !outputData.empty();
}

bool MediaFoundationEncoder::EncodeFrame(
    ID3D11Texture2D* texture,
    std::vector<uint8_t>& outputData,
    bool& isKeyFrame)
{
    outputData.clear();
    isKeyFrame = false;
    if (!m_info.isInitialized || !texture || !m_context)
        return false;

    D3D11_TEXTURE2D_DESC sourceDesc{};
    texture->GetDesc(&sourceDesc);
    if (sourceDesc.Width != m_info.width || sourceDesc.Height != m_info.height ||
        sourceDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || sourceDesc.SampleDesc.Count != 1)
    {
        DriverLog("[MF] Entrada incompatible; se esperaba BGRA8 %ux%u", m_info.width, m_info.height);
        return false;
    }

    if (m_gpuConverterEnabled)
        return EncodeFrameGpu(texture, outputData, isKeyFrame);

    if (!m_stagingTexture)
        return false;

    m_multithread->Enter();
    m_context->CopyResource(m_stagingTexture.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = m_context->Map(m_stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    m_multithread->Leave();
    if (FAILED(hr))
    {
        DriverLog("[MF] No se pudo leer la textura D3D11 (%08X)", hr);
        return false;
    }

    const size_t ySize = static_cast<size_t>(m_info.width) * m_info.height;
    std::vector<uint8_t> nv12(ySize + ySize / 2);
    uint8_t* yPlane = nv12.data();
    uint8_t* uvPlane = yPlane + ySize;
    const auto clampByte = [](int value) { return static_cast<uint8_t>(std::min(255, std::max(0, value))); };

    // Conversión BGRA -> NV12 BT.709 de rango limitado; chroma promediada por bloque 2x2.
    for (uint32_t y = 0; y < m_info.height; y += 2)
    {
        const uint8_t* row0 = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        const uint8_t* row1 = row0 + mapped.RowPitch;
        for (uint32_t x = 0; x < m_info.width; x += 2)
        {
            int sumU = 0;
            int sumV = 0;
            for (uint32_t dy = 0; dy < 2; ++dy)
            {
                const uint8_t* row = dy == 0 ? row0 : row1;
                for (uint32_t dx = 0; dx < 2; ++dx)
                {
                    const uint8_t* px = row + static_cast<size_t>(x + dx) * 4;
                    const int b = px[0], g = px[1], r = px[2];
                    yPlane[static_cast<size_t>(y + dy) * m_info.width + x + dx] =
                        clampByte(((47 * r + 157 * g + 16 * b + 128) >> 8) + 16);
                    sumU += ((-26 * r - 87 * g + 112 * b + 128) >> 8) + 128;
                    sumV += ((112 * r - 102 * g - 10 * b + 128) >> 8) + 128;
                }
            }
            const size_t uv = static_cast<size_t>(y / 2) * m_info.width + x;
            uvPlane[uv] = clampByte((sumU + 2) / 4);
            uvPlane[uv + 1] = clampByte((sumV + 2) / 4);
        }
    }
    m_multithread->Enter();
    m_context->Unmap(m_stagingTexture.Get(), 0);
    m_multithread->Leave();

    return EncodeNv12(nv12.data(), nv12.size(), outputData, isKeyFrame);
}

bool MediaFoundationEncoder::EncodeFrameGpu(
    ID3D11Texture2D* texture,
    std::vector<uint8_t>& outputData,
    bool& isKeyFrame)
{
    outputData.clear();
    isKeyFrame = false;
    if (!m_videoContext || !m_videoDevice || !m_videoProcessor || m_pendingGpuSlots.size() >= m_gpuNv12Slots.size())
        return false;

    auto inputIt = m_inputViews.find(texture);
    if (inputIt == m_inputViews.end())
    {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC viewDesc{};
        viewDesc.FourCC = 0;
        viewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        viewDesc.Texture2D.MipSlice = 0;
        viewDesc.Texture2D.ArraySlice = 0;

        Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inputView;
        HRESULT hr = m_videoDevice->CreateVideoProcessorInputView(
            texture, m_videoEnumerator.Get(), &viewDesc, &inputView);
        if (FAILED(hr))
        {
            DriverLog("[MF] No se pudo crear InputView para textura BGRA (%08X)", static_cast<unsigned>(hr));
            return false;
        }
        inputIt = m_inputViews.emplace(texture, std::move(inputView)).first;
    }

    size_t slotIndex = m_gpuNv12Slots.size();
    for (size_t i = 0; i < m_gpuNv12Slots.size(); ++i)
    {
        if (!m_gpuNv12Slots[i].pending)
        {
            slotIndex = i;
            break;
        }
    }
    if (slotIndex == m_gpuNv12Slots.size())
        return false;

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inputIt->second.Get();

    m_multithread->Enter();
    HRESULT hr = m_videoContext->VideoProcessorBlt(
        m_videoProcessor.Get(), m_gpuNv12Slots[slotIndex].outputView.Get(), 0, 1, &stream);
    m_multithread->Leave();
    if (FAILED(hr))
    {
        DriverLog("[MF] Conversión BGRA→NV12 en GPU falló (%08X)", static_cast<unsigned>(hr));
        return false;
    }

    ComPtr<IMFMediaBuffer> mediaBuffer;
    hr = MFCreateDXGISurfaceBuffer(
        __uuidof(ID3D11Texture2D), m_gpuNv12Slots[slotIndex].texture.Get(), 0, FALSE, &mediaBuffer);
    if (FAILED(hr) || !mediaBuffer)
    {
        DriverLog("[MF] No se pudo envolver NV12 GPU para Media Foundation (%08X)", static_cast<unsigned>(hr));
        return false;
    }

    ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr))
        hr = sample->AddBuffer(mediaBuffer.Get());
    if (FAILED(hr) || !sample)
        return false;

    const LONGLONG duration = 10'000'000 / static_cast<LONGLONG>(m_info.framerate == 0 ? 60 : m_info.framerate);
    sample->SetSampleTime(m_timestamp);
    sample->SetSampleDuration(duration);
    m_timestamp += duration;

    // Keep this NV12 surface out of the reuse pool until its encoded output is collected.
    m_gpuNv12Slots[slotIndex].pending = true;
    m_pendingGpuSlots.push_back(slotIndex);
    bool inputAccepted = false;
    if (!SubmitSample(sample.Get(), outputData, isKeyFrame, inputAccepted) && !inputAccepted)
    {
        // A failed input submission will not produce an output to release this slot.
        if (m_gpuNv12Slots[slotIndex].pending)
        {
            m_gpuNv12Slots[slotIndex].pending = false;
            if (!m_pendingGpuSlots.empty() && m_pendingGpuSlots.back() == slotIndex)
                m_pendingGpuSlots.pop_back();
        }
        return false;
    }
    return true;
}

const EncoderInfo& MediaFoundationEncoder::GetInfo() const
{
    return m_info;
}

void MediaFoundationEncoder::Shutdown()
{
    if (m_encoder)
    {
        m_encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        m_encoder.Reset();
    }
    m_events.Reset();
    m_async = false;
    m_stagingTexture.Reset();
    m_inputViews.clear();
    m_pendingGpuSlots.clear();
    for (GpuNv12Slot& slot : m_gpuNv12Slots)
    {
        slot.pending = false;
        slot.outputView.Reset();
        slot.texture.Reset();
    }
    m_gpuConverterEnabled = false;
    m_videoProcessor.Reset();
    m_videoEnumerator.Reset();
    m_videoContext.Reset();
    m_videoDevice.Reset();
    m_dxgiManager.Reset();
    m_multithread.Reset();
    m_context.Reset();
    m_device.Reset();

    if (m_mfStarted)
    {
        MFShutdown();
        m_mfStarted = false;
    }

    if (m_comStarted)
    {
        CoUninitialize();
        m_comStarted = false;
    }

    m_info.isInitialized = false;
}
