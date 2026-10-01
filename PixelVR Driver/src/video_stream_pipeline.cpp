#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "video_stream_pipeline.h"
#include "driverlog.h"
#include "stream_protocol.h"
#include "streaming_server.h"
#include "video_encoder.h"
#include "encoders/video_encoder_amd.h"
#include "encoders/video_encoder_mf.h"

#include <chrono>
#include <vector>

namespace
{
    uint64_t NowMicroseconds()
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }
}

VideoStreamPipeline& VideoStreamPipeline::Instance()
{
    static VideoStreamPipeline instance;
    return instance;
}

VideoStreamPipeline::~VideoStreamPipeline()
{
    Stop();
}

void VideoStreamPipeline::Stop()
{
    m_stop = true;
    m_cv.notify_all();
    if (m_thread.joinable())
        m_thread.join();

    std::lock_guard<std::mutex> lock(m_mutex);
    m_encoder.reset();
    if (m_server)
        m_server->Stop();
    m_server.reset();
    for (Slot& s : m_slots)
    {
        s.texture.Reset();
        s.state = SlotState::Free;
    }
    m_readySlot = -1;
    m_context.Reset();
    m_device.Reset();
    m_clientConnected = false;
    m_running = false;
}

bool VideoStreamPipeline::Start(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    const StreamConfig& config,
    uint32_t sbsWidth,
    uint32_t sbsHeight,
    uint32_t fps)
{
    Stop();
    m_stop = false;

    if (!device || !context || sbsWidth == 0 || sbsHeight == 0)
        return false;

    m_device = device;
    m_context = context;
    m_config = config;
    // AMF/NV12 need even dimensions.
    m_width = sbsWidth & ~1u;
    m_height = sbsHeight & ~1u;
    m_fps = fps < 15 ? 15 : fps;
    m_frameId = 0;

    // SBS render-target ring. BGRA8: this is what the AMF video converter imports.
    for (Slot& s : m_slots)
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = m_width;
        td.Height = m_height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(device->CreateTexture2D(&td, nullptr, &s.texture)))
        {
            DriverLog("[Stream] No se pudo crear la textura SBS %ux%u", m_width, m_height);
            Stop();
            return false;
        }
        s.state = SlotState::Free;
    }

    std::unique_ptr<IStreamVideoEncoder> encoder = std::make_unique<AMDEncoder>();
    if (!encoder->Initialize(device, context, m_width, m_height, m_config.bitrateKbps, m_fps, m_config.codec))
    {
        encoder.reset();
        DriverLog("[Stream] AMF no disponible; intentando encoder Media Foundation por GPU (%s)",
            m_config.codec == VideoCodec::H264 ? "H.264" : "HEVC");
        auto fallback = std::make_unique<MediaFoundationEncoder>();
        if (fallback->Initialize(device, context, m_width, m_height, m_config.bitrateKbps, m_fps, m_config.codec))
            encoder = std::move(fallback);
        else
        {
            fallback = std::make_unique<MediaFoundationEncoder>(false);
            DriverLog("[Stream] MFT de hardware no disponible; probando encoder Media Foundation por CPU");
            if (fallback->Initialize(device, context, m_width, m_height, m_config.bitrateKbps, m_fps, m_config.codec))
                encoder = std::move(fallback);
        }

        if (!encoder)
        {
            DriverLog("[Stream] No se pudo iniciar un encoder compatible con el codec configurado");
            Stop();
            return false;
        }
    }
    m_encoder = std::move(encoder);

    auto server = std::make_unique<StreamingServer>();
    if (!server->Start(m_config.port, m_config.localhostOnly))
    {
        Stop();
        return false;
    }
    m_server = std::move(server);

    m_running = true;
    m_thread = std::thread(&VideoStreamPipeline::ThreadMain, this);
    DriverLog("[Stream] Pipeline listo: SBS %ux%u @ %u fps, %u kbps, puerto %u",
        m_width, m_height, m_fps, m_config.bitrateKbps, m_config.port);
    return true;
}

ID3D11Texture2D* VideoStreamPipeline::AcquireTarget(int& slot)
{
    slot = -1;
    if (!m_running || !m_clientConnected)
        return nullptr;

    std::lock_guard<std::mutex> lock(m_mutex);
    for (int i = 0; i < kSlots; ++i)
    {
        if (m_slots[i].state == SlotState::Free)
        {
            m_slots[i].state = SlotState::Writing;
            slot = i;
            return m_slots[i].texture.Get();
        }
    }
    return nullptr; // encoder is behind: drop this frame
}

void VideoStreamPipeline::SubmitTarget(int slot)
{
    if (slot < 0 || slot >= kSlots)
        return;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Mailbox: an unconsumed older frame is discarded (never encoded).
        if (m_readySlot >= 0 && m_slots[m_readySlot].state == SlotState::Ready)
            m_slots[m_readySlot].state = SlotState::Free;

        m_slots[slot].state = SlotState::Ready;
        m_slots[slot].timestampUs = NowMicroseconds();
        m_readySlot = slot;
    }
    m_cv.notify_one();
}

void VideoStreamPipeline::CancelTarget(int slot)
{
    if (slot < 0 || slot >= kSlots)
        return;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_slots[slot].state == SlotState::Writing)
        m_slots[slot].state = SlotState::Free;
}

bool VideoStreamPipeline::SendPacket(const std::vector<uint8_t>& payload, bool keyframe, uint64_t timestampUs)
{
    pixelvr::StreamPacketHeader header{};
    header.magic = pixelvr::kPacketMagic;
    header.frameId = m_frameId++;
    header.timestampUs = timestampUs;
    header.width = static_cast<uint16_t>(m_width);
    header.height = static_cast<uint16_t>(m_height);
    header.codec = m_config.codec == VideoCodec::HEVC ? pixelvr::kCodecHevc : pixelvr::kCodecH264;
    header.flags = keyframe ? pixelvr::kPacketFlagKeyframe : 0;
    header.framerate = static_cast<uint16_t>(m_fps);
    header.payloadBytes = static_cast<uint32_t>(payload.size());

    // One send() for header+payload would need a copy; two sends with TCP_NODELAY off would
    // add Nagle delay, so TCP_NODELAY is on (see StreamingServer) and we send back to back.
    if (!m_server->SendAll(&header, sizeof(header)))
        return false;
    return m_server->SendAll(payload.data(), payload.size());
}

void VideoStreamPipeline::ThreadMain()
{
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = SUCCEEDED(comHr);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE)
        DriverLog("[Stream] No se pudo inicializar COM en el hilo de encode (%08X)", comHr);

    DriverLog("[Stream] Hilo de encode/envio iniciado");

    std::vector<uint8_t> bitstream;
    bool hadClient = false;

    while (!m_stop)
    {
        const bool client = m_server->HasClient(); // also accepts a pending connection
        if (client != hadClient)
        {
            hadClient = client;
            m_clientConnected = client;
            if (client)
            {
                m_frameId = 0;
                m_encoder->RequestKeyframe(); // new client must start on an IDR
                DriverLog("[Stream] Cliente Android conectado");
            }
            else
            {
                DriverLog("[Stream] Cliente Android desconectado");
            }
        }

        int slot = -1;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(client ? 20 : 100), [&] {
                return m_stop.load() || (m_readySlot >= 0 && m_slots[m_readySlot].state == SlotState::Ready);
            });

            if (m_stop)
                break;

            if (m_readySlot >= 0 && m_slots[m_readySlot].state == SlotState::Ready)
            {
                slot = m_readySlot;
                m_readySlot = -1;
                m_slots[slot].state = SlotState::Encoding;
            }
        }

        if (slot < 0)
            continue;

        bool keyframe = false;
        const bool encoded = client && m_encoder->EncodeFrame(m_slots[slot].texture.Get(), bitstream, keyframe);
        const uint64_t ts = m_slots[slot].timestampUs;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_slots[slot].state = SlotState::Free;
        }

        if (!encoded)
            continue;

        if (!SendPacket(bitstream, keyframe, ts))
        {
            // Client went away mid-frame; next loop iteration notices and re-arms the IDR.
            m_clientConnected = false;
            hadClient = false;
        }
    }

    DriverLog("[Stream] Hilo de encode/envio detenido");
    if (uninitializeCom)
        CoUninitialize();
}
