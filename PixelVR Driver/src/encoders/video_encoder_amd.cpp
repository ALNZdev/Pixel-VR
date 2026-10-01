#include "video_encoder_amd.h"
#include "driverlog.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "core/Buffer.h"
#include "core/Surface.h"
#include "core/Version.h"
#include "components/VideoConverter.h"
#include "components/VideoEncoderVCE.h"
#include "components/VideoEncoderHEVC.h"

namespace
{
    constexpr auto kEncodeTimeout = std::chrono::milliseconds(50);

    const char* AMFResultName(AMF_RESULT result)
    {
        switch (result)
        {
        case AMF_OK: return "AMF_OK";
        case AMF_CODEC_NOT_SUPPORTED: return "AMF_CODEC_NOT_SUPPORTED";
        case AMF_ENCODER_NOT_PRESENT: return "AMF_ENCODER_NOT_PRESENT";
        case AMF_NO_DEVICE: return "AMF_NO_DEVICE";
        case AMF_NOT_SUPPORTED: return "AMF_NOT_SUPPORTED";
        default: return "AMF_RESULT_UNKNOWN";
        }
    }

    std::string AMFModulePath(HMODULE module)
    {
        wchar_t path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
            return "(ruta no disponible)";

        char utf8[MAX_PATH * 4]{};
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, static_cast<int>(sizeof(utf8)), nullptr, nullptr);
        return bytes > 0 ? std::string(utf8) : std::string("(ruta no disponible)");
    }
}

AMDEncoder::AMDEncoder()
{
    m_info.name = "AMD VCN (AMF)";
    m_info.isHardware = true;
}

AMDEncoder::~AMDEncoder()
{
    Shutdown();
}

bool AMDEncoder::IsAvailable()
{
    HMODULE module = LoadLibraryW(AMF_DLL_NAME);
    if (module)
    {
        FreeLibrary(module);
        return true;
    }
    DriverLog("[AMD] %s no encontrado (drivers AMD Adrenalin instalados?)", AMF_DLL_NAMEA);
    return false;
}

bool AMDEncoder::IsAMDAdapter(const std::string& adapterName)
{
    std::string name = adapterName;
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);

    return name.find("amd") != std::string::npos ||
        name.find("radeon") != std::string::npos ||
        name.find("rdna") != std::string::npos ||
        name.find("vega") != std::string::npos;
}

bool AMDEncoder::Initialize(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    uint32_t width,
    uint32_t height,
    uint32_t bitrateKbps,
    uint32_t framerate,
    VideoCodec codec)
{
    Shutdown();

    if (!device || !context || width == 0 || height == 0 || framerate == 0)
    {
        DriverLog("[AMD] Parametros de inicializacion invalidos");
        return false;
    }

    HMODULE module = LoadLibraryW(AMF_DLL_NAME);
    if (!module)
    {
        DriverLog("[AMD] No se pudo cargar %s", AMF_DLL_NAMEA);
        return false;
    }
    m_amfModule = module;

    amf_uint64 runtimeVersion = 0;
    auto amfQueryVersion = reinterpret_cast<AMFQueryVersion_Fn>(GetProcAddress(module, AMF_QUERY_VERSION_FUNCTION_NAME));
    if (amfQueryVersion && amfQueryVersion(&runtimeVersion) == AMF_OK)
    {
        DriverLog("[AMD] Runtime AMF %u.%u.%u.%u cargado desde %s",
            static_cast<unsigned>(AMF_GET_MAJOR_VERSION(runtimeVersion)),
            static_cast<unsigned>(AMF_GET_MINOR_VERSION(runtimeVersion)),
            static_cast<unsigned>(AMF_GET_SUBMINOR_VERSION(runtimeVersion)),
            static_cast<unsigned>(AMF_GET_BUILD_VERSION(runtimeVersion)),
            AMFModulePath(module).c_str());
    }
    else
    {
        DriverLog("[AMD] AMFQueryVersion no disponible; runtime cargado desde %s", AMFModulePath(module).c_str());
    }

    auto amfInit = reinterpret_cast<AMFInit_Fn>(GetProcAddress(module, AMF_INIT_FUNCTION_NAME));
    if (!amfInit || amfInit(AMF_FULL_VERSION, &m_factory) != AMF_OK || !m_factory)
    {
        DriverLog("[AMD] AMFInit fallo (version del runtime AMF incompatible con los headers)");
        Shutdown();
        return false;
    }

    m_device = device;
    m_context = context;
    m_codec = codec;

    AMF_RESULT res = m_factory->CreateContext(&m_amfContext);
    if (res != AMF_OK)
    {
        DriverLog("[AMD] CreateContext fallo (%d)", static_cast<int>(res));
        Shutdown();
        return false;
    }

    res = m_amfContext->InitDX11(device);
    if (res != AMF_OK)
    {
        DriverLog("[AMD] InitDX11 fallo (%d). El dispositivo D3D11 debe estar en la GPU AMD.", static_cast<int>(res));
        Shutdown();
        return false;
    }

    // --- BGRA -> NV12 (GPU) -------------------------------------------------------------------
    res = m_factory->CreateComponent(m_amfContext, AMFVideoConverter, &m_converter);
    if (res != AMF_OK)
    {
        DriverLog("[AMD] No se pudo crear AMFVideoConverter (%d)", static_cast<int>(res));
        Shutdown();
        return false;
    }
    m_converter->SetProperty(AMF_VIDEO_CONVERTER_MEMORY_TYPE, amf_int64(amf::AMF_MEMORY_DX11));
    m_converter->SetProperty(AMF_VIDEO_CONVERTER_OUTPUT_FORMAT, amf_int64(amf::AMF_SURFACE_NV12));
    m_converter->SetProperty(AMF_VIDEO_CONVERTER_OUTPUT_SIZE, ::AMFConstructSize(width, height));
    res = m_converter->Init(amf::AMF_SURFACE_BGRA, width, height);
    if (res != AMF_OK)
    {
        DriverLog("[AMD] Converter Init fallo (%d)", static_cast<int>(res));
        Shutdown();
        return false;
    }

    // --- Encoder ------------------------------------------------------------------------------
    const bool h264 = (codec == VideoCodec::H264);
    const wchar_t* componentName = h264 ? AMFVideoEncoderVCE_AVC : AMFVideoEncoder_HEVC;
    DriverLog("[AMD] Creando componente AMF para %s en %ux%u",
        h264 ? "H.264" : "HEVC", width, height);
    res = m_factory->CreateComponent(m_amfContext, componentName, &m_encoder);
    if (res != AMF_OK)
    {
        DriverLog("[AMD] CreateComponent encoder %s fallo: %d (%s)",
            h264 ? "H.264" : "HEVC", static_cast<int>(res), AMFResultName(res));
        Shutdown();
        return false;
    }

    const amf_int64 bitrate = static_cast<amf_int64>(bitrateKbps) * 1000;
    const amf_int64 idrPeriod = static_cast<amf_int64>(framerate) * 2; // periodic IDR every ~2 s

    if (h264)
    {
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_USAGE, amf_int64(AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY));
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_PROFILE, amf_int64(AMF_VIDEO_ENCODER_PROFILE_HIGH));
        // Level 5.2: SBS frames (e.g. 3840x1080 @ 90 Hz) exceed level 4.2 limits.
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_PROFILE_LEVEL, amf_int64(AMF_H264_LEVEL__5_2));
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_QUALITY_PRESET, amf_int64(AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED));
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, amf_int64(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR));
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, ::AMFConstructSize(width, height));
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, ::AMFConstructRate(framerate, 1));
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, bitrate);
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, bitrate);
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, bitrate / framerate * 2);
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, amf_int64(0));
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_LOWLATENCY_MODE, true);
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_ENFORCE_HRD, false);
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_FILLER_DATA_ENABLE, false);
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, idrPeriod);
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEADER_INSERTION_SPACING, idrPeriod);
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_FULL_RANGE_COLOR, false);
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_QUERY_TIMEOUT, amf_int64(20)); // ms, makes QueryOutput block briefly
    }
    else
    {
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_USAGE, amf_int64(AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY));
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET, amf_int64(AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED));
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD, amf_int64(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR));
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMESIZE, ::AMFConstructSize(width, height));
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE, ::AMFConstructRate(framerate, 1));
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, bitrate);
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, bitrate);
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, bitrate / framerate * 2);
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, idrPeriod);
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE, amf_int64(AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE_IDR_ALIGNED));
        m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_QUERY_TIMEOUT, amf_int64(20));
    }

    res = m_encoder->Init(amf::AMF_SURFACE_NV12, width, height);
    if (res != AMF_OK)
    {
        DriverLog("[AMD] Encoder Init fallo (%d)", static_cast<int>(res));
        Shutdown();
        return false;
    }

    m_info.width = width;
    m_info.height = height;
    m_info.bitrateKbps = bitrateKbps;
    m_info.framerate = framerate;
    m_info.codec = codec;
    m_info.isInitialized = true;
    m_forceIdr = true;

    DriverLog("[AMD] Encoder VCN listo: %ux%u @ %u kbps, %u fps (%s)",
        width, height, bitrateKbps, framerate, h264 ? "H.264" : "HEVC");
    return true;
}

bool AMDEncoder::EncodeFrame(
    ID3D11Texture2D* texture,
    std::vector<uint8_t>& outputData,
    bool& isKeyFrame)
{
    isKeyFrame = false;
    outputData.clear();

    if (!texture || !m_info.isInitialized)
        return false;

    // Zero-copy: wrap the D3D11 texture as an AMF surface. The caller must not touch
    // the texture until this function returns.
    amf::AMFSurfacePtr surface;
    if (m_amfContext->CreateSurfaceFromDX11Native(texture, &surface, nullptr) != AMF_OK || !surface)
    {
        DriverLog("[AMD] CreateSurfaceFromDX11Native fallo (la textura debe ser B8G8R8A8_UNORM en el mismo dispositivo)");
        return false;
    }

    // BGRA -> NV12 on the GPU.
    if (m_converter->SubmitInput(surface) != AMF_OK)
        return false;

    amf::AMFDataPtr nv12;
    const auto convDeadline = std::chrono::steady_clock::now() + kEncodeTimeout;
    AMF_RESULT res = AMF_REPEAT;
    while (res == AMF_REPEAT && std::chrono::steady_clock::now() < convDeadline)
    {
        res = m_converter->QueryOutput(&nv12);
        if (res == AMF_REPEAT)
            std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    if (res != AMF_OK || !nv12)
        return false;

    const bool forceIdr = m_forceIdr.exchange(false);
    if (forceIdr)
    {
        if (m_codec == VideoCodec::H264)
        {
            nv12->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, amf_int64(AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR));
            nv12->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, true);
            nv12->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, true);
        }
        else
        {
            nv12->SetProperty(AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE, amf_int64(AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR));
            nv12->SetProperty(AMF_VIDEO_ENCODER_HEVC_INSERT_HEADER, true);
        }
    }

    // Submit (VCN queue can be momentarily full).
    const auto submitDeadline = std::chrono::steady_clock::now() + kEncodeTimeout;
    do
    {
        res = m_encoder->SubmitInput(nv12);
        if (res == AMF_INPUT_FULL)
            std::this_thread::sleep_for(std::chrono::microseconds(300));
    } while (res == AMF_INPUT_FULL && std::chrono::steady_clock::now() < submitDeadline);

    if (res != AMF_OK)
    {
        if (forceIdr)
            m_forceIdr = true; // retry the IDR on the next frame
        DriverLog("[AMD] SubmitInput fallo (%d)", static_cast<int>(res));
        return false;
    }

    // Poll for the encoded frame.
    amf::AMFDataPtr encoded;
    const auto outDeadline = std::chrono::steady_clock::now() + kEncodeTimeout;
    res = AMF_REPEAT;
    while (res == AMF_REPEAT && std::chrono::steady_clock::now() < outDeadline)
    {
        res = m_encoder->QueryOutput(&encoded);
        if (res == AMF_REPEAT)
            std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    if (res != AMF_OK || !encoded)
        return false;

    amf::AMFBufferPtr buffer(encoded);
    if (!buffer || buffer->GetSize() == 0)
        return false;

    const uint8_t* data = static_cast<const uint8_t*>(buffer->GetNative());
    outputData.assign(data, data + buffer->GetSize());

    amf::AMFVariant type;
    if (m_codec == VideoCodec::H264)
    {
        if (encoded->GetProperty(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &type) == AMF_OK)
            isKeyFrame = (type.ToInt64() == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR);
    }
    else
    {
        if (encoded->GetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE, &type) == AMF_OK)
            isKeyFrame = (type.ToInt64() == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR);
    }
    return true;
}

const EncoderInfo& AMDEncoder::GetInfo() const
{
    return m_info;
}

void AMDEncoder::Shutdown()
{
    if (m_encoder)
    {
        m_encoder->Drain();
        m_encoder->Terminate();
        m_encoder = nullptr;
    }
    if (m_converter)
    {
        m_converter->Drain();
        m_converter->Terminate();
        m_converter = nullptr;
    }
    if (m_amfContext)
    {
        m_amfContext->Terminate();
        m_amfContext = nullptr;
    }

    m_factory = nullptr;
    if (m_amfModule)
    {
        // Intentionally NOT calling FreeLibrary: AMF keeps worker threads that may still be
        // unwinding; unloading amfrt64.dll from inside the SteamVR process is a known crash source.
        m_amfModule = nullptr;
    }

    m_device = nullptr;
    m_context = nullptr;
    m_info.isInitialized = false;
}
