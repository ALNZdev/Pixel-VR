#include "video_encoder_mf.h"
#include "driverlog.h"

#include <initguid.h>
#include <codecapi.h>
#include <mferror.h>
#include <wmcodecdsp.h>

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

MediaFoundationEncoder::MediaFoundationEncoder()
{
    m_info.name = "Media Foundation H.264";
    m_info.isHardware = true;
}

MediaFoundationEncoder::~MediaFoundationEncoder()
{
    Shutdown();
}

bool MediaFoundationEncoder::Initialize(
    ID3D11Device* /*device*/,
    ID3D11DeviceContext* /*context*/,
    uint32_t width,
    uint32_t height,
    uint32_t bitrateKbps,
    uint32_t framerate,
    VideoCodec codec)
{
    Shutdown();

    width &= ~1u;
    height &= ~1u;
    if (width < 64 || height < 64 || codec != VideoCodec::H264)
    {
        DriverLog("[MF] Solo H.264 con tamaño par >= 64");
        return false;
    }

    if (framerate == 0)
        framerate = 60;

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr) && hr != MF_E_ALREADY_INITIALIZED)
    {
        DriverLog("[MF] MFStartup falló (0x%08X)", hr);
        return false;
    }
    m_mfStarted = true;

    MFT_REGISTER_TYPE_INFO outputInfo{};
    outputInfo.guidMajorType = MFMediaType_Video;
    outputInfo.guidSubtype = MFVideoFormat_H264;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER | MFT_ENUM_FLAG_LOCALMFT,
        nullptr,
        &outputInfo,
        &activates,
        &count);

    if (FAILED(hr) || count == 0)
    {
        hr = MFTEnumEx(
            MFT_CATEGORY_VIDEO_ENCODER,
            MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
            nullptr,
            &outputInfo,
            &activates,
            &count);
        m_info.isHardware = false;
        m_info.name = "Media Foundation H.264 (software)";
    }

    if (SUCCEEDED(hr) && count > 0 && activates != nullptr)
    {
        hr = activates[0]->ActivateObject(IID_PPV_ARGS(&m_encoder));
        for (UINT32 i = 0; i < count; ++i)
            activates[i]->Release();
        CoTaskMemFree(activates);
    }
    else
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
        DriverLog("[MF] ActivateObject falló (0x%08X)", hr);
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
    outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, framerate, 1);
    MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outputType->SetUINT32(MF_MT_AVG_BITRATE, bitrateKbps * 1000);
    outputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);

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
        "[MF] Encoder listo: %ux%u @ %u kbps %u fps (%s)",
        width,
        height,
        bitrateKbps,
        framerate,
        m_info.name.c_str());
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

    if (m_forceKeyframe)
    {
        sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE);
        m_forceKeyframe = false;
    }

    HRESULT hr = m_encoder->ProcessInput(m_inputStreamId, sample.Get(), 0);
    if (FAILED(hr))
    {
        DriverLog("[MF] ProcessInput falló (0x%08X)", hr);
        return false;
    }

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
    ID3D11Texture2D* /*texture*/,
    std::vector<uint8_t>& outputData,
    bool& isKeyFrame)
{
    outputData.clear();
    isKeyFrame = false;
    return false;
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

    if (m_mfStarted)
    {
        MFShutdown();
        m_mfStarted = false;
    }

    m_info.isInitialized = false;
}