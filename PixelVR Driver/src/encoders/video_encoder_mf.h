#pragma once

#include "video_encoder.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl.h>
#include <vector>

class MediaFoundationEncoder : public IStreamVideoEncoder
{
public:
    MediaFoundationEncoder();
    ~MediaFoundationEncoder() override;

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

    bool EncodeNv12(
        const uint8_t* nv12,
        size_t nv12Bytes,
        std::vector<uint8_t>& outputData,
        bool& isKeyFrame);

    const EncoderInfo& GetInfo() const override;
    void Shutdown() override;

    void RequestKeyframe() { m_forceKeyframe = true; }

private:
    bool DrainOutput(std::vector<uint8_t>& outputData, bool& isKeyFrame);
    static void AvccToAnnexB(std::vector<uint8_t>& data);

    EncoderInfo m_info;
    Microsoft::WRL::ComPtr<IMFTransform> m_encoder;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> m_events;
    bool m_async = false;
    DWORD m_inputStreamId = 0;
    DWORD m_outputStreamId = 0;
    LONGLONG m_timestamp = 0;
    UINT32 m_nalLengthSize = 4;
    bool m_forceKeyframe = true;
    bool m_mfStarted = false;
};
