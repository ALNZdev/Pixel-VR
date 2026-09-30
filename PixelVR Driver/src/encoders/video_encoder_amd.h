#pragma once

#include "video_encoder.h"

#include <atomic>
#include <d3d11.h>
#include <memory>
#include <string>

#include "core/Factory.h"
#include "core/Context.h"
#include "components/Component.h"

/// AMD AMF hardware encoder (H.264 / HEVC).
///
/// Input : D3D11 texture in DXGI_FORMAT_B8G8R8A8_UNORM created on the SAME device that was
///         passed to Initialize(). The texture is imported into AMF as a native DX11 surface
///         (no CPU copy), converted BGRA->NV12 on the GPU by AMFVideoConverter, and encoded by VCN.
/// Output: Annex-B bitstream.
///
/// EncodeFrame() is synchronous (submit + poll) and must be called from a single thread.
class AMDEncoder : public IStreamVideoEncoder
{
public:
    AMDEncoder();
    ~AMDEncoder() override;

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
    void RequestKeyframe() override { m_forceIdr = true; }
    void Shutdown() override;

    /// Verifica si AMF está disponible en este sistema
    static bool IsAvailable();

    /// Verifica si el adaptador es AMD
    static bool IsAMDAdapter(const std::string& adapterName);

private:
    EncoderInfo m_info;
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    void* m_amfModule = nullptr; // HMODULE of amfrt64.dll
    amf::AMFFactory* m_factory = nullptr; // owned by the DLL, not released
    amf::AMFContextPtr m_amfContext;
    amf::AMFComponentPtr m_converter;
    amf::AMFComponentPtr m_encoder;

    std::atomic<bool> m_forceIdr{ true }; // first frame is always an IDR
    VideoCodec m_codec = VideoCodec::H264;
};
