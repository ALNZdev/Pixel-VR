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
#include "encoders/ffmpeg_video_encoder.h"

#include <chrono>
#include <iterator>
#include <vector>
#include <string>
#include <dxgi1_2.h>
#include <wrl.h>

namespace
{
    using Microsoft::WRL::ComPtr;

    uint64_t AdapterLuid(ID3D11Device* device)
    {
        if (!device) return 0;
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc{};
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) ||
            FAILED(dxgiDevice->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&desc)))
            return 0;
        return (static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32) |
            static_cast<uint32_t>(desc.AdapterLuid.LowPart);
    }

    uint64_t AdapterLuid(IDXGIAdapter1* adapter)
    {
        DXGI_ADAPTER_DESC1 desc{};
        if (!adapter || FAILED(adapter->GetDesc1(&desc))) return 0;
        return (static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32) |
            static_cast<uint32_t>(desc.AdapterLuid.LowPart);
    }

    bool IsAmd(IDXGIAdapter1* adapter)
    {
        DXGI_ADAPTER_DESC1 desc{};
        return adapter && SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == 0x1002;
    }

    bool CreateAdapterDevice(IDXGIAdapter1* adapter, ComPtr<ID3D11Device>& device,
        ComPtr<ID3D11DeviceContext>& context)
    {
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL obtained{};
        HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, static_cast<UINT>(std::size(levels)),
            D3D11_SDK_VERSION, &device, &obtained, &context);
        if (hr == E_INVALIDARG)
            hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, 1,
                D3D11_SDK_VERSION, &device, &obtained, &context);
        return SUCCEEDED(hr) && device && context;
    }

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
    m_transferStaging.Reset();
    m_encoderInput.Reset();
    m_encoderContext.Reset();
    m_encoderDevice.Reset();
    m_crossAdapter = false;
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
    uint32_t fps,
    EncoderSelection selection)
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

    std::unique_ptr<IStreamVideoEncoder> encoder;
    ComPtr<ID3D11Device> selectedDevice;
    ComPtr<ID3D11DeviceContext> selectedContext;
    std::string selectedAdapterName;
    const uint64_t renderLuid = AdapterLuid(device);

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        DriverLog("[Stream] No se pudo enumerar adaptadores DXGI para buscar el encoder");
        Stop();
        return false;
    }

    struct Candidate { ComPtr<IDXGIAdapter1> adapter; std::string name; bool amd = false; };
    std::vector<Candidate> candidates;
    for (UINT i = 0;; ++i)
    {
        Candidate candidate;
        if (factory->EnumAdapters1(i, &candidate.adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(candidate.adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        bool duplicateLuid = false;
        for (const Candidate& old : candidates)
        {
            DXGI_ADAPTER_DESC1 oldDesc{};
            if (SUCCEEDED(old.adapter->GetDesc1(&oldDesc)) &&
                oldDesc.AdapterLuid.LowPart == desc.AdapterLuid.LowPart &&
                oldDesc.AdapterLuid.HighPart == desc.AdapterLuid.HighPart)
                duplicateLuid = true;
        }
        if (duplicateLuid) continue;
        char name[512]{};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, static_cast<int>(sizeof(name)), nullptr, nullptr);
        candidate.name = name;
        candidate.amd = desc.VendorId == 0x1002;
        candidates.push_back(std::move(candidate));
    }

    auto tryCandidate = [&](const Candidate& candidate, bool tryAmf, bool preferHardwareMf) -> bool
    {
        ComPtr<ID3D11Device> candidateDevice;
        ComPtr<ID3D11DeviceContext> candidateContext;
        const bool sameAdapter = AdapterLuid(candidate.adapter.Get()) == renderLuid;
        if (sameAdapter)
        {
            candidateDevice = device;
            candidateContext = context;
        }
        else if (!CreateAdapterDevice(candidate.adapter.Get(), candidateDevice, candidateContext))
        {
            DriverLog("[Stream] No se pudo crear D3D11 en '%s'", candidate.name.c_str());
            return false;
        }

        if (tryAmf)
        {
            auto amd = std::make_unique<AMDEncoder>();
            if (amd->Initialize(candidateDevice.Get(), candidateContext.Get(), m_width, m_height,
                m_config.bitrateKbps, m_fps, m_config.codec))
            {
                encoder = std::move(amd);
                selectedDevice = candidateDevice;
                selectedContext = candidateContext;
                selectedAdapterName = candidate.name;
                return true;
            }
            DriverLog("[Stream] AMF no inició en '%s'; se continúa con el siguiente adaptador AMD", candidate.name.c_str());
        }
        else
        {
            auto mf = std::make_unique<MediaFoundationEncoder>(preferHardwareMf);
            if (mf->Initialize(candidateDevice.Get(), candidateContext.Get(), m_width, m_height,
                m_config.bitrateKbps, m_fps, m_config.codec))
            {
                encoder = std::move(mf);
                selectedDevice = candidateDevice;
                selectedContext = candidateContext;
                selectedAdapterName = candidate.name;
                return true;
            }
        }
        return false;
    };

    // Try the real AMD AMF encoder on every AMD adapter. Each AMF context is bound
    // to the D3D11 device created for that adapter, not to the compositor device.
    if (selection != EncoderSelection::HardwareFallbackOnly)
    {
        for (const Candidate& candidate : candidates)
            if (candidate.amd && tryCandidate(candidate, true, true)) break;
        if (!encoder && selection == EncoderSelection::AMDOnly)
        {
            DriverLog("[Stream] Ningún adaptador AMD pudo inicializar AMF");
            Stop();
            return false;
        }
    }

    // FFmpeg owns a hardware context for the selected GPU; its encoders are
    // independent of the compositor D3D11 device. It tries AMF, QSV and NVENC.
    if (!encoder && selection != EncoderSelection::AMDOnly)
    {
        auto ffmpegHardware = std::make_unique<FFmpegVideoEncoder>(false);
        if (ffmpegHardware->Initialize(device, context, m_width, m_height,
            m_config.bitrateKbps, m_fps, m_config.codec))
        {
            encoder = std::move(ffmpegHardware);
            selectedDevice = device;
            selectedContext = context;
            selectedAdapterName = encoder->GetInfo().name;
        }
    }

    // Hardware Media Foundation is probed per adapter. FFmpeg software and the
    // software MFT remain final CPU fallbacks.
    if (!encoder)
    {
        for (const Candidate& candidate : candidates)
            if (tryCandidate(candidate, false, true)) break;
    }
    if (!encoder && selection != EncoderSelection::AMDOnly)
    {
        auto ffmpegSoftware = std::make_unique<FFmpegVideoEncoder>(true);
        if (ffmpegSoftware->Initialize(device, context, m_width, m_height,
            m_config.bitrateKbps, m_fps, m_config.codec))
        {
            encoder = std::move(ffmpegSoftware);
            selectedDevice = device;
            selectedContext = context;
            selectedAdapterName = encoder->GetInfo().name;
        }
    }
    if (!encoder && selection != EncoderSelection::AMDOnly)
    {
        auto software = std::make_unique<MediaFoundationEncoder>(false);
        if (software->Initialize(device, context, m_width, m_height,
            m_config.bitrateKbps, m_fps, m_config.codec))
        {
            encoder = std::move(software);
            selectedDevice = device;
            selectedContext = context;
            selectedAdapterName = "adaptador de composición (software)";
        }
    }
    if (!encoder)
    {
        DriverLog("[Stream] No se pudo iniciar un encoder compatible con el codec configurado");
        Stop();
        return false;
    }

    m_encoderDevice = selectedDevice;
    m_encoderContext = selectedContext;
    m_crossAdapter = AdapterLuid(m_encoderDevice.Get()) != renderLuid;
    if (m_crossAdapter)
    {
        D3D11_TEXTURE2D_DESC stageDesc{};
        stageDesc.Width = m_width; stageDesc.Height = m_height;
        stageDesc.MipLevels = 1; stageDesc.ArraySize = 1;
        stageDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        stageDesc.SampleDesc.Count = 1;
        stageDesc.Usage = D3D11_USAGE_STAGING;
        stageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        HRESULT hr = device->CreateTexture2D(&stageDesc, nullptr, &m_transferStaging);
        if (SUCCEEDED(hr))
        {
            D3D11_TEXTURE2D_DESC inputDesc = stageDesc;
            inputDesc.Usage = D3D11_USAGE_DEFAULT;
            inputDesc.CPUAccessFlags = 0;
            inputDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            hr = m_encoderDevice->CreateTexture2D(&inputDesc, nullptr, &m_encoderInput);
        }
        if (FAILED(hr) || !m_transferStaging || !m_encoderInput)
        {
            DriverLog("[Stream] No se pudo preparar el puente de fotogramas entre adaptadores (%08X)", static_cast<unsigned>(hr));
            Stop();
            return false;
        }
        DriverLog("[Stream] Encoder '%s' separado del adaptador de composición; transferencia BGRA por staging CPU activa",
            selectedAdapterName.c_str());
    }
    else
    {
        DriverLog("[Stream] Encoder y composición usan el mismo adaptador: '%s'", selectedAdapterName.c_str());
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
    const EncoderInfo& encoderInfo = m_encoder->GetInfo();
    DriverLog("[Stream] Pipeline listo: encoder=%s (%s), SBS %ux%u @ %u fps, %u kbps, puerto %u",
        encoderInfo.name.c_str(), encoderInfo.isHardware ? "hardware" : "CPU",
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

bool VideoStreamPipeline::PrepareEncoderTexture(ID3D11Texture2D* source, ID3D11Texture2D** encoderTexture)
{
    if (!source || !encoderTexture) return false;
    *encoderTexture = source;
    if (!m_crossAdapter) return true;

    // CopyResource/Map serializes the render adapter's queued writes. The mapped
    // source memory remains valid until UpdateSubresource has copied it to the other GPU.
    ComPtr<ID3D11Multithread> multithread;
    const bool protectedContext = SUCCEEDED(m_context.As(&multithread)) && multithread.Get() != nullptr;
    if (protectedContext) multithread->Enter();
    m_context->CopyResource(m_transferStaging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = m_context->Map(m_transferStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr))
    {
        if (protectedContext) multithread->Leave();
        DriverLog("[Stream] No se pudo leer fotograma para transferirlo al encoder (%08X)", static_cast<unsigned>(hr));
        return false;
    }
    m_encoderContext->UpdateSubresource(m_encoderInput.Get(), 0, nullptr, mapped.pData, mapped.RowPitch, 0);
    m_context->Unmap(m_transferStaging.Get(), 0);
    if (protectedContext) multithread->Leave();
    *encoderTexture = m_encoderInput.Get();
    return true;
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
        ID3D11Texture2D* encoderTexture = nullptr;
        const bool transferred = client && PrepareEncoderTexture(m_slots[slot].texture.Get(), &encoderTexture);
        const bool encoded = transferred && m_encoder->EncodeFrame(encoderTexture, bitstream, keyframe);
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
