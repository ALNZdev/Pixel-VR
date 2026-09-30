#pragma once

#include "video_encoder.h"

#include <cstdint>
#include <string>

struct StreamConfig
{
    bool enable = false;
    bool localhostOnly = true;
    uint16_t port = 9944;
    uint32_t bitrateKbps = 15000;
    uint32_t framerate = 0; // 0 = use HMD display frequency
    VideoCodec codec = VideoCodec::H264;
    uint32_t width = 0;  // SBS output width  (0 = use driver window width)
    uint32_t height = 0; // SBS output height (0 = use driver window height)
};

enum class DisplayModeSetting
{
    Monitor, // PC monitor (existing behaviour)
    Android  // Android phone as display over USB (IVRDriverDirectModeComponent)
};

class ConfigManager
{
public:
    static StreamConfig LoadStreamConfig();
    static DisplayModeSetting LoadDisplayMode();
};
