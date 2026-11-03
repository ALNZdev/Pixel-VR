#include "video_encoder_intel.h"
#include "driverlog.h"
#include <d3d11.h>
#include <algorithm>

IntelEncoder::IntelEncoder()
{
    m_info.name = "Intel QuickSync";
    m_info.isHardware = true;
}

IntelEncoder::~IntelEncoder()
{
    Shutdown();
}

bool IntelEncoder::IsAvailable()
{
    // Intentar cargar oneVPL o Media SDK
    HMODULE hModule = LoadLibraryA("libmfx.dll");
    if (hModule)
    {
        FreeLibrary(hModule);
        DriverLog("[Intel] oneVPL/Media SDK detectado");
        return true;
    }

    hModule = LoadLibraryA("libmfxhw64.dll");
    if (hModule)
    {
        FreeLibrary(hModule);
        DriverLog("[Intel] oneVPL/Media SDK detectado (hardware)");
        return true;
    }

    DriverLog("[Intel] oneVPL/Media SDK no encontrado");
    return false;
}

bool IntelEncoder::IsIntelAdapter(const std::string& adapterName)
{
    // Verificar si el nombre del adaptador contiene "Intel", "UHD", "Iris", "Arc", etc.
    std::string name = adapterName;

    // Convertir a minúsculas para comparación
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);

    return name.find("intel") != std::string::npos ||
        name.find("uhd") != std::string::npos ||
        name.find("iris") != std::string::npos ||
        name.find("arc") != std::string::npos ||
        name.find("hd graphics") != std::string::npos;
}

bool IntelEncoder::Initialize(
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
        DriverLog("[Intel] Error: device o context es nullptr");
        return false;
    }

    if (!IsAvailable())
    {
        DriverLog("[Intel] QuickSync no disponible en este sistema");
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

    DriverLog("[Intel] Inicializando QuickSync: %ux%u @ %u kbps, %u fps (%s)",
        width, height, bitrateKbps, framerate,
        codec == VideoCodec::H264 ? "H.264" : "HEVC");

    // TODO: Cargar oneVPL/Media SDK
    // TODO: Crear sesión QSV
    // TODO: Inicializar encoder con D3D11 interop
    // TODO: Configurar bitrate, profile, preset

    // Documentación Intel:
    // https://github.com/intel/libvpl
    // https://github.com/intel/media-sdk
    // Pasos de inicialización:
    // 1. MFXLoad() o MFXInit()
    // 2. Crear mfxSession
    // 3. MFXVideoCORE_SetHandle() para D3D11
    // 4. MFXVideoENCODE_Init() con mfxEncodeCtrl
    // 5. Configurar CBR, target bitrate, framerate

    DriverLog("[Intel] ✓ Encoder inicializado (implementación pending - instala Intel oneVPL o Media SDK)");
    return true;
}

bool IntelEncoder::EncodeFrame(
    ID3D11Texture2D* texture,
    std::vector<uint8_t>& outputData,
    bool& isKeyFrame)
{
    if (!texture || !m_device)
    {
        return false;
    }

    // TODO: Implementar codificación
    // 1. Mapear texture D3D11 a superficie QSV
    // 2. MFXVideoENCODE_EncodeFrameAsync()
    // 3. Leer datos codificados

    isKeyFrame = false;
    return true;
}

const EncoderInfo& IntelEncoder::GetInfo() const
{
    return m_info;
}

void IntelEncoder::Shutdown()
{
    // TODO: Liberar QSV resources

    if (m_mfxModule)
    {
        FreeLibrary((HMODULE)m_mfxModule);
        m_mfxModule = nullptr;
    }

    m_device = nullptr;
    m_context = nullptr;
    m_info.isInitialized = false;

    DriverLog("[Intel] Encoder liberado");
}