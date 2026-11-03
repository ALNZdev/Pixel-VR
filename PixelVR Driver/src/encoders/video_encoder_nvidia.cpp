#include "video_encoder_nvidia.h"
#include "driverlog.h"
#include <d3d11.h>
#include <wrl.h>
#include <algorithm>

using Microsoft::WRL::ComPtr;

NvidiaEncoder::NvidiaEncoder()
{
    m_info.name = "NVIDIA NVENC";
    m_info.isHardware = true;
}

NvidiaEncoder::~NvidiaEncoder()
{
    Shutdown();
}

bool NvidiaEncoder::IsAvailable()
{
    // Intentar cargar nvEncodeAPI64.dll
    HMODULE hModule = LoadLibraryA("nvEncodeAPI64.dll");
    if (hModule)
    {
        FreeLibrary(hModule);
        DriverLog("[NVIDIA] NVENC SDK detectado");
        return true;
    }

    hModule = LoadLibraryA("nvEncodeAPI.dll");
    if (hModule)
    {
        FreeLibrary(hModule);
        DriverLog("[NVIDIA] NVENC SDK detectado (32-bit)");
        return true;
    }

    DriverLog("[NVIDIA] NVENC SDK no encontrado");
    return false;
}

bool NvidiaEncoder::IsNvidiaAdapter(const std::string& adapterName)
{
    // Verificar si el nombre del adaptador contiene "NVIDIA", "GeForce", "Tesla", "Quadro", etc.
    std::string name = adapterName;

    // Convertir a minúsculas para comparación
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);

    return name.find("nvidia") != std::string::npos ||
        name.find("geforce") != std::string::npos ||
        name.find("tesla") != std::string::npos ||
        name.find("quadro") != std::string::npos ||
        name.find("rtx") != std::string::npos ||
        name.find("gtx") != std::string::npos;
}

bool NvidiaEncoder::Initialize(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    uint32_t width,
    uint32_t height,
    uint32_t bitrateKbps,
    uint32_t framerate,
    VideoCodec codec)
{
    if (!device || !context)
    {
        DriverLog("[NVIDIA] Error: device o context es nullptr");
        return false;
    }

    if (!IsAvailable())
    {
        DriverLog("[NVIDIA] NVENC no disponible en este sistema");
        return false;
    }

    m_device = device;
    m_context = context;
    m_info.width = width;
    m_info.height = height;
    m_info.bitrateKbps = bitrateKbps;
    m_info.framerate = framerate;
    m_info.codec = codec;
    m_info.isInitialized = true;

    DriverLog("[NVIDIA] Inicializando NVENC: %ux%u @ %u kbps, %u fps (%s)",
        width, height, bitrateKbps, framerate,
        codec == VideoCodec::H264 ? "H.264" : "HEVC");

    // TODO: Cargar nvEncodeAPI.h headers
    // TODO: Inicializar NVENC session
    // TODO: Configurar encoder con los parámetros
    // TODO: Crear bitstream buffer

    // Documentación NVENC:
    // https://developer.nvidia.com/video-encode-sdk
    // Pasos de inicialización:
    // 1. NvEncOpenEncodeSession()
    // 2. NvEncCreateInputBuffer()
    // 3. NvEncCreateBitstreamBuffer()
    // 4. NvEncInitializeEncoder() con NV_ENC_INITIALIZE_PARAMS
    // 5. Configurar bitrate, preset (BALANCED, FAST, LOSSLESS), frame interval

    DriverLog("[NVIDIA] ✓ Encoder inicializado (implementación pending - instala NVIDIA Video Codec SDK)");
    return true;
}

bool NvidiaEncoder::EncodeFrame(
    ID3D11Texture2D* texture,
    std::vector<uint8_t>& outputData,
    bool& isKeyFrame)
{
    if (!texture || !m_device)
    {
        return false;
    }

    // TODO: Implementar codificación
    // 1. Mapear textura D3D11 a recurso NVENC
    // 2. Llamar NvEncEncodePicture()
    // 3. Leer datos del bitstream buffer
    // 4. Copiar a outputData

    isKeyFrame = false;
    return true;
}

const EncoderInfo& NvidiaEncoder::GetInfo() const
{
    return m_info;
}

void NvidiaEncoder::Shutdown()
{
    // TODO: Liberar NVENC resources
    // NvEncDestroyBitstreamBuffer()
    // NvEncDestroyInputBuffer()
    // NvEncDestroyEncoder()
    // NvEncCloseEncodeSession()

    if (m_nvencModule)
    {
        FreeLibrary((HMODULE)m_nvencModule);
        m_nvencModule = nullptr;
    }

    m_device = nullptr;
    m_context = nullptr;
    m_info.isInitialized = false;

    DriverLog("[NVIDIA] Encoder liberado");
}