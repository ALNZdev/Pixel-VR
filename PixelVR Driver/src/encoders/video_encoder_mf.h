#pragma once

#include "video_encoder.h"

#include <d3d11_4.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl.h>
#include <array>
#include <deque>
#include <unordered_map>
#include <vector>

class MediaFoundationEncoder : public IStreamVideoEncoder
{
public:
    explicit MediaFoundationEncoder(bool preferHardware = true);
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
    struct GpuNv12Slot
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outputView;
        bool pending = false;
    };

    bool DrainOutput(std::vector<uint8_t>& outputData, bool& isKeyFrame);
    bool InitializeGpuConverter(uint32_t width, uint32_t height, uint32_t framerate);
    bool EncodeFrameGpu(ID3D11Texture2D* texture, std::vector<uint8_t>& outputData, bool& isKeyFrame);
    bool SubmitSample(IMFSample* sample, std::vector<uint8_t>& outputData, bool& isKeyFrame, bool& inputAccepted);
    static void AvccToAnnexB(std::vector<uint8_t>& data);

    EncoderInfo m_info;
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<ID3D11Multithread> m_multithread;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_stagingTexture;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_videoDevice;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> m_videoContext;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_videoEnumerator;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> m_videoProcessor;
    std::array<GpuNv12Slot, 4> m_gpuNv12Slots;
    std::unordered_map<ID3D11Texture2D*, Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView>> m_inputViews;
    std::deque<size_t> m_pendingGpuSlots;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> m_dxgiManager;
    Microsoft::WRL::ComPtr<IMFTransform> m_encoder;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> m_events;
    bool m_async = false;
    DWORD m_inputStreamId = 0;
    DWORD m_outputStreamId = 0;
    LONGLONG m_timestamp = 0;
    UINT32 m_nalLengthSize = 4;
    bool m_forceKeyframe = true;
    bool m_mfStarted = false;
    bool m_comStarted = false;
    bool m_preferHardware = true;
    bool m_gpuConverterEnabled = false;
};
