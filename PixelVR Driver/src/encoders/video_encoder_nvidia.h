#pragma once

#include "video_encoder.h"
#include <d3d11.h>
#include <memory>

/// Encoder NVIDIA NVENC para H.264 y HEVC
class NvidiaEncoder : public IStreamVideoEncoder
{
public:
    NvidiaEncoder();
    ~NvidiaEncoder() override;

    bool Initialize(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        uint32_t width,
        uint32_t height,
        uint32_t bitrateKbps,
        uint32_t framerate,
        VideoCodec codec) override;

    bool EncodeFrame(
        ID3D11Texture2D* texture,
        std::vector<uint8_t>& outputData,
        bool& isKeyFrame) override;

    const EncoderInfo& GetInfo() const override;

    void Shutdown() override;

    /// Verifica si NVENC está disponible en este sistema
    static bool IsAvailable();

    /// Verifica si el adaptador es NVIDIA
    static bool IsNvidiaAdapter(const std::string& adapterName);

private:
    EncoderInfo m_info;
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    // NVENC internals - se cargan dinámicamente
    void* m_nvencModule = nullptr;
    void* m_encoder = nullptr;
    void* m_bitstream = nullptr;
};
