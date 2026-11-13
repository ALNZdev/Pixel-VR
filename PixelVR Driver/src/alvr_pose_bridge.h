#pragma once

#include "openvr_driver.h"

class AlvrPoseBridge
{
public:
    static void Send(const vr::DriverPose_t& pose);
    static void Shutdown();
};
