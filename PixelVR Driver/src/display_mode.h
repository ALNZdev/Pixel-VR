#pragma once

#include <dxgi.h>
#include <cstdint>
#include <string>

struct DisplayOutputInfo
{
    int adapterIndex = -1;
    int outputIndex = -1;
    DXGI_OUTPUT_DESC desc{};
    std::string adapterName;
};

class DisplayMode
{
public:
    static bool FindOutputContainingPoint(
        int32_t x,
        int32_t y,
        DisplayOutputInfo& outInfo);
};
