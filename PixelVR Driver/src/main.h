#pragma once
#include "psm_tracking_manager.h"
#include <memory>
#include "controllers.h"
#include "openvr_driver.h"
#include "hmd.h"

extern PSMTrackingManager g_psmTracking;

// make sure your class is publicly inheriting vr::IServerTrackedDeviceProvider!
class MyDeviceProvider : public vr::IServerTrackedDeviceProvider
{
public:
    vr::EVRInitError Init(vr::IVRDriverContext* pDriverContext) override;
    const char* const* GetInterfaceVersions() override;

    void RunFrame() override;

    bool ShouldBlockStandbyMode() override;
    void EnterStandby() override;
    void LeaveStandby() override;

    void Cleanup() override;

private:
    std::unique_ptr<MyHMDDeviceDriver> m_hmd_device;
    std::unique_ptr<MyControllerDeviceDriver> my_left_controller_device_;
    std::unique_ptr<MyControllerDeviceDriver> my_right_controller_device_;
};