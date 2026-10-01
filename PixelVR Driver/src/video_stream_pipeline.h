#pragma once

#include "config_manager.h"

#include <d3d11.h>
#include <wrl.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class IStreamVideoEncoder;
class StreamingServer;

/// Encode + transport half of the Android display path.
///
///   DirectMode::Present  ->  AcquireTarget()  (SBS render target from a small ring)
///                        ->  SubmitTarget()   (mailbox: only the newest frame is kept)
///   encode thread        ->  AMF (VCN) -> StreamPacketHeader + payload -> TCP (adb reverse)
///
/// The compositor thread never blocks on encoding or on the network: if the encoder or the
/// socket is slower than the display rate, older frames are dropped BEFORE encoding, which keeps
/// the H.264 reference chain intact and latency bounded.
class VideoStreamPipeline
{
public:
    enum class EncoderSelection
    {
        Auto,
        AMDOnly,
        MediaFoundationOnly
    };

    static VideoStreamPipeline& Instance();

    /// (Re)starts the pipeline on `device` (must be on the GPU that owns the AMF encoder).
    /// sbsWidth/sbsHeight is the full [left|right] frame size.
    bool Start(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        const StreamConfig& config,
        uint32_t sbsWidth,
        uint32_t sbsHeight,
        uint32_t fps,
        EncoderSelection selection = EncoderSelection::Auto);

    void Stop();

    bool IsRunning() const { return m_running; }

    /// True while an Android client is connected (updated by the encode thread).
    bool HasClient() const { return m_clientConnected; }

    /// Returns a free SBS render target, or nullptr if there is no client / no free slot
    /// (caller should simply skip this frame).
    ID3D11Texture2D* AcquireTarget(int& slot);

    /// Hands the composed frame to the encoder (GPU work must already be flushed).
    void SubmitTarget(int slot);

    /// Gives a slot back without encoding it.
    void CancelTarget(int slot);

private:
    VideoStreamPipeline() = default;
    ~VideoStreamPipeline();

    void ThreadMain();
    bool SendPacket(const std::vector<uint8_t>& payload, bool keyframe, uint64_t timestampUs);

    enum class SlotState { Free, Writing, Ready, Encoding };
    static constexpr int kSlots = 3;

    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        SlotState state = SlotState::Free;
        uint64_t timestampUs = 0;
    };

    StreamConfig m_config{};
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_fps = 90;

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;

    std::unique_ptr<IStreamVideoEncoder> m_encoder;
    std::unique_ptr<StreamingServer> m_server;

    Slot m_slots[kSlots];
    int m_readySlot = -1;
    uint32_t m_frameId = 0;

    std::thread m_thread;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_stop{ false };
    std::atomic<bool> m_clientConnected{ false };
    std::mutex m_mutex;
    std::condition_variable m_cv;
};
