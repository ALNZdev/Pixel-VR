#pragma once

#ifdef PIXELVRSTREAM_EXPORTS
#define PIXELVRSTREAM_API __declspec(dllexport)
#else
#define PIXELVRSTREAM_API __declspec(dllimport)
#endif

extern "C"
{
    typedef void(__cdecl* PixelVRStatusCallback)(
        const char* message);

    PIXELVRSTREAM_API bool StartStream(
        const char* phoneIp,
        int videoPort,
        int controlPort,
        int width,
        int height,
        int fps,
        int bitrateKbps,
        int gopSeconds);

    PIXELVRSTREAM_API void StopStream();

    PIXELVRSTREAM_API bool IsStreamRunning();

    PIXELVRSTREAM_API void RequestKeyframe();

    PIXELVRSTREAM_API void RegisterStatusCallback(
        PixelVRStatusCallback callback);

    PIXELVRSTREAM_API bool SetStreamParameters(
        int bitrateKbps,
        int fps,
        int gopSeconds);
}