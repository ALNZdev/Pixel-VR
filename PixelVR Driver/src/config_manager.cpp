#include "config_manager.h"
#include "driverlog.h"

#include "openvr_driver.h"

#include <algorithm>

namespace
{
    constexpr char kSection[] = "driver_pixelvr";

    uint32_t ClampU32(int32_t value, uint32_t minimum, uint32_t maximum, uint32_t fallback)
    {
        if (value <= 0)
            return fallback;

        const uint32_t u = static_cast<uint32_t>(value);
        return (std::min)((std::max)(u, minimum), maximum);
    }
}

StreamConfig ConfigManager::LoadStreamConfig()
{
    StreamConfig config{};

    vr::IVRSettings* settings = vr::VRSettings();
    if (settings == nullptr)
        return config;

    config.enable = settings->GetBool(kSection, "streamEnable");
    config.localhostOnly = !settings->GetBool(kSection, "streamBindAll");

    const int32_t port = settings->GetInt32(kSection, "streamPort");
    config.port = static_cast<uint16_t>(ClampU32(port, 1024, 65535, 9944));

    const int32_t bitrate = settings->GetInt32(kSection, "streamBitrateKbps");
    config.bitrateKbps = ClampU32(bitrate, 1000, 80000, 15000);

    const int32_t fps = settings->GetInt32(kSection, "streamFramerate");
    config.framerate = ClampU32(fps, 0, 240, 0);

    char codec[64] = {};
    settings->GetString(kSection, "streamCodec", codec, sizeof(codec));
    if (_stricmp(codec, "hevc") == 0 || _stricmp(codec, "h265") == 0)
        config.codec = VideoCodec::HEVC;
    else
        config.codec = VideoCodec::H264;

    config.width = ClampU32(settings->GetInt32(kSection, "streamWidth"), 64, 16384, 0);
    config.height = ClampU32(settings->GetInt32(kSection, "streamHeight"), 64, 16384, 0);

    return config;
}

DisplayModeSetting ConfigManager::LoadDisplayMode()
{
    vr::IVRSettings* settings = vr::VRSettings();
    if (settings == nullptr)
        return DisplayModeSetting::Monitor;

    char mode[64] = {};
    settings->GetString(kSection, "displayMode", mode, sizeof(mode));
    return _stricmp(mode, "android") == 0 ? DisplayModeSetting::Android : DisplayModeSetting::Monitor;
}
