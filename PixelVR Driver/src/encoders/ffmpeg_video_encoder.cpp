#include "ffmpeg_video_encoder.h"
#include "driverlog.h"

#include <dxgi1_2.h>
#include <d3d11_4.h>
#include <algorithm>
#include <climits>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#ifdef PIXELVR_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#endif

using Microsoft::WRL::ComPtr;

struct FFmpegVideoEncoder::Impl
{
#ifdef PIXELVR_HAS_FFMPEG
    AVCodecContext* codec = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* sws = nullptr;
    AVBufferRef* hwDevice = nullptr;
#endif
};

namespace
{
#ifdef PIXELVR_HAS_FFMPEG
    const char* AvError(int error, char (&buffer)[AV_ERROR_MAX_STRING_SIZE])
    {
        av_strerror(error, buffer, sizeof(buffer));
        return buffer;
    }

    bool IsHardwareCodecName(const char* name)
    {
        return std::strstr(name, "_amf") || std::strstr(name, "_qsv") ||
            std::strstr(name, "_nvenc");
    }
#endif

    struct AdapterInfo { int index; UINT vendor; std::string name; };

    std::vector<AdapterInfo> EnumerateAdapters()
    {
        std::vector<AdapterInfo> result;
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return result;
        for (UINT i = 0;; ++i)
        {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
            bool duplicate = false;
            for (const auto& old : result)
            {
                ComPtr<IDXGIAdapter1> prev;
                if (factory->EnumAdapters1(static_cast<UINT>(old.index), &prev) == S_OK)
                {
                    DXGI_ADAPTER_DESC1 prevDesc{};
                    if (SUCCEEDED(prev->GetDesc1(&prevDesc)) &&
                        prevDesc.AdapterLuid.LowPart == desc.AdapterLuid.LowPart &&
                        prevDesc.AdapterLuid.HighPart == desc.AdapterLuid.HighPart)
                        duplicate = true;
                }
            }
            if (duplicate) continue;
            char name[512]{};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name,
                static_cast<int>(sizeof(name)), nullptr, nullptr);
            result.push_back({ static_cast<int>(i), desc.VendorId, name });
        }
        return result;
    }
}

FFmpegVideoEncoder::FFmpegVideoEncoder(bool allowSoftwareFallback)
    : m_allowSoftwareFallback(allowSoftwareFallback), m_impl(std::make_unique<Impl>())
{
    m_info.name = "FFmpeg";
}

FFmpegVideoEncoder::~FFmpegVideoEncoder() { Shutdown(); }

bool FFmpegVideoEncoder::Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
    uint32_t width, uint32_t height, uint32_t bitrateKbps,
    uint32_t framerate, VideoCodec codec)
{
    Shutdown();
    m_impl = std::make_unique<Impl>();
    if (!device || !context || !width || !height || !framerate || (width & 1) || (height & 1))
        return false;
#ifndef PIXELVR_HAS_FFMPEG
    (void)bitrateKbps; (void)codec;
    DriverLog("[FFmpeg] No compilado: coloca el SDK shared de FFmpeg en lib/ffmpeg y vuelve a configurar CMake");
    return false;
#else
    av_log_set_level(AV_LOG_ERROR);
    m_device = device;
    m_context = context;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &m_staging))) return false;

    bool ok = TryHardwareCandidates(width, height, bitrateKbps, framerate, codec);
    if (!ok && m_allowSoftwareFallback)
        ok = TrySoftwareEncoder(width, height, bitrateKbps, framerate, codec);
    if (!ok) Shutdown();
    return ok;
#endif
}

bool FFmpegVideoEncoder::TryHardwareCandidates(uint32_t width, uint32_t height,
    uint32_t bitrateKbps, uint32_t framerate, VideoCodec codec)
{
#ifndef PIXELVR_HAS_FFMPEG
    (void)width; (void)height; (void)bitrateKbps; (void)framerate; (void)codec;
    return false;
#else
    const char* codecNames[2][3] = {
        { "h264_amf", "h264_qsv", "h264_nvenc" },
        { "hevc_amf", "hevc_qsv", "hevc_nvenc" }
    };
    const int codecRow = codec == VideoCodec::H264 ? 0 : 1;
    const auto adapters = EnumerateAdapters();
    for (const auto& adapter : adapters)
    {
        for (int backend = 0; backend < 3; ++backend)
        {
            if ((backend == 0 && adapter.vendor != 0x1002) ||
                (backend == 1 && adapter.vendor != 0x8086) ||
                (backend == 2 && adapter.vendor != 0x10de)) continue;
            if (OpenEncoder(codecNames[codecRow][backend], adapter.index, true,
                width, height, bitrateKbps, framerate, codec))
            {
                m_info.name = std::string("FFmpeg ") + codecNames[codecRow][backend] +
                    " @ " + adapter.name;
                m_info.isHardware = true;
                return true;
            }
        }
    }
    DriverLog("[FFmpeg] Ningún encoder de hardware FFmpeg compatible pudo abrirse para %s",
        codec == VideoCodec::H264 ? "H.264" : "HEVC");
    return false;
#endif
}

bool FFmpegVideoEncoder::TrySoftwareEncoder(uint32_t width, uint32_t height,
    uint32_t bitrateKbps, uint32_t framerate, VideoCodec codec)
{
#ifndef PIXELVR_HAS_FFMPEG
    (void)width; (void)height; (void)bitrateKbps; (void)framerate; (void)codec;
    return false;
#else
    const char* name = codec == VideoCodec::H264 ? "libx264" : "libx265";
    if (!OpenEncoder(name, -1, false, width, height, bitrateKbps, framerate, codec)) return false;
    m_info.name = std::string("FFmpeg ") + name + " (CPU)";
    m_info.isHardware = false;
    return true;
#endif
}

bool FFmpegVideoEncoder::OpenEncoder(const char* encoderName, int adapterIndex,
    bool hardware, uint32_t width, uint32_t height, uint32_t bitrateKbps,
    uint32_t framerate, VideoCodec codec)
{
#ifndef PIXELVR_HAS_FFMPEG
    (void)encoderName; (void)adapterIndex; (void)hardware; (void)width; (void)height;
    (void)bitrateKbps; (void)framerate; (void)codec;
    return false;
#else
    const AVCodec* encoder = avcodec_find_encoder_by_name(encoderName);
    if (!encoder) return false;
    AVCodecContext* ctx = avcodec_alloc_context3(encoder);
    if (!ctx) return false;
    ctx->width = static_cast<int>(width); ctx->height = static_cast<int>(height);
    ctx->time_base = AVRational{ 1, static_cast<int>(framerate) };
    ctx->framerate = AVRational{ static_cast<int>(framerate), 1 };
    ctx->bit_rate = static_cast<int64_t>(bitrateKbps) * 1000;
    ctx->rc_max_rate = ctx->bit_rate;
    ctx->rc_buffer_size = static_cast<int>(std::min<int64_t>(ctx->bit_rate, INT_MAX));
    ctx->gop_size = static_cast<int>(framerate * 2);
    ctx->max_b_frames = 0;
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->thread_count = 1;

    AVPixelFormat inputFormat = AV_PIX_FMT_NV12;
#if LIBAVCODEC_VERSION_MAJOR >= 61
    // FFmpeg 7+ exposes supported formats through avcodec_get_supported_config;
    // AVCodec::pix_fmts is deprecated and absent in newer releases.
    const void* supportedFormats = nullptr;
    int supportedFormatCount = 0;
    const int configRet = avcodec_get_supported_config(nullptr, encoder,
        AV_CODEC_CONFIG_PIX_FORMAT, 0, &supportedFormats, &supportedFormatCount);
    if (configRet >= 0 && supportedFormats && supportedFormatCount > 0)
    {
        const auto* formats = static_cast<const AVPixelFormat*>(supportedFormats);
        bool acceptsNv12 = false;
        for (int i = 0; i < supportedFormatCount; ++i)
            if (formats[i] == AV_PIX_FMT_NV12) acceptsNv12 = true;
        if (!acceptsNv12)
        {
            for (int i = 0; i < supportedFormatCount; ++i)
            {
                const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(formats[i]);
                if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) { inputFormat = formats[i]; break; }
            }
        }
    }
#else
    if (encoder->pix_fmts)
    {
        bool acceptsNv12 = false;
        for (const AVPixelFormat* f = encoder->pix_fmts; *f != AV_PIX_FMT_NONE; ++f)
            if (*f == AV_PIX_FMT_NV12) acceptsNv12 = true;
        if (!acceptsNv12)
        {
            for (const AVPixelFormat* f = encoder->pix_fmts; *f != AV_PIX_FMT_NONE; ++f)
            {
                const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(*f);
                if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) { inputFormat = *f; break; }
            }
        }
    }
#endif
    ctx->pix_fmt = inputFormat;

    if (hardware)
    {
        AVBufferRef* d3dDevice = nullptr;
        const std::string ordinal = std::to_string(adapterIndex);
        int ret = av_hwdevice_ctx_create(&d3dDevice, AV_HWDEVICE_TYPE_D3D11VA,
            ordinal.c_str(), nullptr, 0);
        if (ret < 0)
        {
            avcodec_free_context(&ctx);
            char err[AV_ERROR_MAX_STRING_SIZE]{};
            DriverLog("[FFmpeg] D3D11VA adapter %d no abre: %s", adapterIndex, AvError(ret, err));
            return false;
        }
        const bool isNvenc = std::strstr(encoderName, "_nvenc") != nullptr;
        if (isNvenc)
        {
            // NVENC chooses the CUDA ordinal using its private `gpu` option below.
            // FFmpeg does not derive a CUDA device from a D3D11VA device.
            av_buffer_unref(&d3dDevice);
        }
        else
        {
            const AVHWDeviceType targetType = std::strstr(encoderName, "_qsv")
                ? AV_HWDEVICE_TYPE_QSV : AV_HWDEVICE_TYPE_AMF;
            ret = av_hwdevice_ctx_create_derived(&m_impl->hwDevice, targetType, d3dDevice, 0);
            av_buffer_unref(&d3dDevice);
            if (ret < 0)
            {
                avcodec_free_context(&ctx);
                char err[AV_ERROR_MAX_STRING_SIZE]{};
                DriverLog("[FFmpeg] No se pudo derivar dispositivo %s para adaptador %d: %s",
                    encoderName, adapterIndex, AvError(ret, err));
                return false;
            }
            ctx->hw_device_ctx = av_buffer_ref(m_impl->hwDevice);
            if (!ctx->hw_device_ctx)
            {
                av_buffer_unref(&m_impl->hwDevice); avcodec_free_context(&ctx); return false;
            }
        }
        if (std::strstr(encoderName, "_amf"))
        {
            av_opt_set(ctx->priv_data, "usage", "ultralowlatency", 0);
            av_opt_set(ctx->priv_data, "quality", "speed", 0);
            av_opt_set(ctx->priv_data, "gpu", std::to_string(adapterIndex).c_str(), 0);
        }
        else if (std::strstr(encoderName, "_nvenc"))
        {
            av_opt_set(ctx->priv_data, "gpu", std::to_string(adapterIndex).c_str(), 0);
        }
        else if (std::strstr(encoderName, "_qsv"))
        {
            av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
            av_opt_set(ctx->priv_data, "async_depth", "1", 0);
        }
    }
    else if (std::strstr(encoderName, "libx264"))
    {
        av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
        av_opt_set(ctx->priv_data, "annexb", "1", 0);
    }
    else if (std::strstr(encoderName, "libx265"))
    {
        av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
    }

    int ret = avcodec_open2(ctx, encoder, nullptr);
    if (ret < 0)
    {
        char err[AV_ERROR_MAX_STRING_SIZE]{};
        DriverLog("[FFmpeg] No se pudo abrir %s en adapter %d: %s", encoderName, adapterIndex, AvError(ret, err));
        avcodec_free_context(&ctx);
        av_buffer_unref(&m_impl->hwDevice);
        return false;
    }
    m_impl->codec = ctx;
    m_impl->frame = av_frame_alloc();
    m_impl->packet = av_packet_alloc();
    if (!m_impl->frame || !m_impl->packet)
    {
        av_frame_free(&m_impl->frame); av_packet_free(&m_impl->packet);
        avcodec_free_context(&m_impl->codec); av_buffer_unref(&m_impl->hwDevice);
        return false;
    }
    m_impl->frame->format = ctx->pix_fmt;
    m_impl->frame->width = ctx->width;
    m_impl->frame->height = ctx->height;
    ret = av_frame_get_buffer(m_impl->frame, 32);
    if (ret < 0)
    {
        char err[AV_ERROR_MAX_STRING_SIZE]{};
        DriverLog("[FFmpeg] No se pudo reservar el frame de entrada: %s", AvError(ret, err));
        av_frame_free(&m_impl->frame); av_packet_free(&m_impl->packet);
        avcodec_free_context(&m_impl->codec); av_buffer_unref(&m_impl->hwDevice);
        return false;
    }
    m_impl->sws = sws_getContext(static_cast<int>(width), static_cast<int>(height), AV_PIX_FMT_BGRA,
        static_cast<int>(width), static_cast<int>(height), ctx->pix_fmt,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_impl->sws)
    {
        av_frame_free(&m_impl->frame); av_packet_free(&m_impl->packet);
        avcodec_free_context(&m_impl->codec); av_buffer_unref(&m_impl->hwDevice);
        return false;
    }
    m_info.width = width; m_info.height = height; m_info.bitrateKbps = bitrateKbps;
    m_info.framerate = framerate; m_info.codec = codec; m_info.isInitialized = true;
    m_forceKeyframe = true; m_pts = 0;
    DriverLog("[FFmpeg] Encoder abierto: %s, %ux%u @ %u fps, adapter=%d",
        encoderName, width, height, framerate, adapterIndex);
    return true;
#endif
}

bool FFmpegVideoEncoder::EncodeFrame(ID3D11Texture2D* texture,
    std::vector<uint8_t>& outputData, bool& isKeyFrame)
{
    outputData.clear(); isKeyFrame = false;
#ifndef PIXELVR_HAS_FFMPEG
    (void)texture;
    return false;
#else
    if (!texture || !m_impl || !m_impl->codec || !m_impl->frame || !m_staging) return false;
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    if (desc.Width != m_info.width || desc.Height != m_info.height ||
        desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) return false;
    ComPtr<ID3D11Multithread> multithread;
    const bool protectedContext = SUCCEEDED(m_context.As(&multithread)) && multithread.Get() != nullptr;
    if (protectedContext) multithread->Enter();
    m_context->CopyResource(m_staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_context->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
    {
        if (protectedContext) multithread->Leave();
        return false;
    }

    int ret = av_frame_make_writable(m_impl->frame);
    if (ret >= 0)
    {
        const uint8_t* source[] = { static_cast<const uint8_t*>(mapped.pData) };
        const int sourceStride[] = { static_cast<int>(mapped.RowPitch) };
        sws_scale(m_impl->sws, source, sourceStride, 0, static_cast<int>(m_info.height),
            m_impl->frame->data, m_impl->frame->linesize);
        m_impl->frame->pts = m_pts++;
        if (m_forceKeyframe.exchange(false))
        {
            m_impl->frame->pict_type = AV_PICTURE_TYPE_I;
            m_impl->frame->flags |= AV_FRAME_FLAG_KEY;
        }
        else
        {
            m_impl->frame->pict_type = AV_PICTURE_TYPE_NONE;
            m_impl->frame->flags &= ~AV_FRAME_FLAG_KEY;
        }
        ret = avcodec_send_frame(m_impl->codec, m_impl->frame);
    }
    m_context->Unmap(m_staging.Get(), 0);
    if (protectedContext) multithread->Leave();
    if (ret < 0) return false;

    while ((ret = avcodec_receive_packet(m_impl->codec, m_impl->packet)) >= 0)
    {
        outputData.insert(outputData.end(), m_impl->packet->data,
            m_impl->packet->data + m_impl->packet->size);
        isKeyFrame = isKeyFrame || (m_impl->packet->flags & AV_PKT_FLAG_KEY) != 0;
        av_packet_unref(m_impl->packet);
    }
    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) return false;
    return !outputData.empty();
#endif
}

void FFmpegVideoEncoder::Shutdown()
{
#ifdef PIXELVR_HAS_FFMPEG
    if (m_impl)
    {
        sws_freeContext(m_impl->sws); m_impl->sws = nullptr;
        av_frame_free(&m_impl->frame); av_packet_free(&m_impl->packet);
        avcodec_free_context(&m_impl->codec); av_buffer_unref(&m_impl->hwDevice);
    }
#endif
    m_staging.Reset(); m_context.Reset(); m_device.Reset();
    m_info.isInitialized = false;
}
