#pragma once

#include "video_encoder.h"

#include <atomic>
#include <d3d11.h>
#include <memory>
#include <string>
#include <vector>
#include <wrl.h>

class FFmpegVideoEncoder final : public IStreamVideoEncoder
{
public:
    explicit FFmpegVideoEncoder(bool allowSoftwareFallback = false);
    ~FFmpegVideoEncoder() override;

    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
        uint32_t width, uint32_t height, uint32_t bitrateKbps,
        uint32_t framerate, VideoCodec codec) override;
    bool EncodeFrame(ID3D11Texture2D* texture,
        std::vector<uint8_t>& outputData, bool& isKeyFrame) override;
    const EncoderInfo& GetInfo() const override { return m_info; }
    void RequestKeyframe() override { m_forceKeyframe = true; }
    void Shutdown() override;

private:
    bool TryHardwareCandidates(uint32_t width, uint32_t height,
        uint32_t bitrateKbps, uint32_t framerate, VideoCodec codec);
    bool TrySoftwareEncoder(uint32_t width, uint32_t height,
        uint32_t bitrateKbps, uint32_t framerate, VideoCodec codec);
    bool OpenEncoder(const char* encoderName, int adapterIndex, bool hardware,
        uint32_t width, uint32_t height, uint32_t bitrateKbps,
        uint32_t framerate, VideoCodec codec);

    EncoderInfo m_info;
    bool m_allowSoftwareFallback = false;
    std::atomic<bool> m_forceKeyframe{ true };
    int64_t m_pts = 0;
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_staging;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
