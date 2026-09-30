#pragma once

#include <d3d11.h>
#include <cstdint>
#include <memory>
#include <vector>
#include <string>

/// Codec de video soportado
enum class VideoCodec
{
    H264,
    HEVC
};

/// Información del encoder actual
struct EncoderInfo
{
    bool isInitialized = false;
    bool isHardware = false;
    std::string name;          // "NVIDIA NVENC", "AMD VCN", "Intel QuickSync", etc.
    std::string adapterName;   // Nombre del adaptador DXGI
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bitrateKbps = 0;
    uint32_t framerate = 0;
    VideoCodec codec = VideoCodec::H264;
};

/// Interfaz abstracta para encoders de video
class IStreamVideoEncoder
{
public:
    virtual ~IStreamVideoEncoder() = default;

    /// Inicializa el encoder
    /// @param device Dispositivo D3D11 (debe ser del adaptador que soporte encoding)
    /// @param context Contexto de dispositivo D3D11
    /// @param width Ancho del video
    /// @param height Alto del video
    /// @param bitrateKbps Bitrate en kbps
    /// @param framerate FPS deseado
    /// @param codec H264 o HEVC
    /// @return true si se inicializó correctamente
    virtual bool Initialize(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        uint32_t width,
        uint32_t height,
        uint32_t bitrateKbps,
        uint32_t framerate,
        VideoCodec codec) = 0;

    /// Codifica un frame
    /// @param texture Textura D3D11 con el frame a codificar
    /// @param outputData Vector donde se almacenan los datos codificados
    /// @param isKeyFrame Output: true si es keyframe
    /// @return true si la codificación fue exitosa
    virtual bool EncodeFrame(
        ID3D11Texture2D* texture,
        std::vector<uint8_t>& outputData,
        bool& isKeyFrame) = 0;

    /// Obtiene información del encoder
    virtual const EncoderInfo& GetInfo() const = 0;

    /// Pide que el siguiente frame codificado sea un IDR (con SPS/PPS)
    virtual void RequestKeyframe() {}

    /// Libera recursos
    virtual void Shutdown() = 0;
};

/// Factory para crear encoders automáticamente
class VideoEncoderFactory
{
public:
    /// Crea el mejor encoder disponible para el adaptador especificado
    /// @param device Dispositivo D3D11
    /// @param context Contexto de dispositivo D3D11
    /// @param adapterName Nombre del adaptador DXGI (ej: "NVIDIA GeForce RTX 3080")
    /// @return Encoder inicializado o nullptr
    static std::unique_ptr<IStreamVideoEncoder> CreateBestForAdapter(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        const std::string& adapterName);

    /// Lista todos los encoders disponibles en el sistema
    static std::vector<std::string> ListAvailableEncoders();
};
