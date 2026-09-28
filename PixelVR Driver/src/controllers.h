#pragma once

#include <array>
#include <string>
#include <chrono>
#include "openvr_driver.h"
#include <atomic>
#include <thread>
#include "pipe_handler.h"

enum MyComponent {
    // Control izquierdo
    MyComponent_x_click,
    MyComponent_y_click,
    MyComponent_trigger_click_left,
    MyComponent_trigger_value_left,
    MyComponent_grip_click_left,
    MyComponent_grip_value_left,
    MyComponent_system_click_left,
    MyComponent_thumbstick_x_left,
    MyComponent_thumbstick_y_left,
    MyComponent_thumbstick_click_left,

    // Control derecho
    MyComponent_a_click,
    MyComponent_b_click,
    MyComponent_trigger_click_right,
    MyComponent_trigger_value_right,
    MyComponent_grip_click_right,
    MyComponent_grip_value_right,
    MyComponent_system_click_right,
    MyComponent_thumbstick_x_right,
    MyComponent_thumbstick_y_right,
    MyComponent_thumbstick_click_right,

    MyComponent_MAX
};

class MyControllerDeviceDriver : public vr::ITrackedDeviceServerDriver
{
public:
    MyControllerDeviceDriver(vr::ETrackedControllerRole role);
    virtual vr::EVRInitError Activate(uint32_t unObjectId) override;
    virtual void Deactivate() override;
    virtual void EnterStandby() override;
    virtual void* GetComponent(const char* pchComponentNameAndVersion) override;
    virtual void DebugRequest(const char* pchRequest, char* pchResponseBuffer, uint32_t unResponseBufferSize) override;
    virtual vr::DriverPose_t GetPose() override;
    void MyRunFrame();
    const std::string& GetSerialNumber() { return m_sSerialNumber; }

    void LoadSettings();

private:
    vr::TrackedDeviceIndex_t m_unObjectId;
    vr::PropertyContainerHandle_t m_ulPropertyContainer;
    vr::ETrackedControllerRole m_eRole;

    std::string m_sSerialNumber;
    std::string m_sModelNumber;

    std::array<vr::VRInputComponentHandle_t, MyComponent_MAX> m_inputHandles;
    std::atomic<bool> m_bActive;

    float m_fHapticAmplitude = 1.0f;
    bool m_bEnableHaptics = true;
    bool m_bEnablePSMTracking = true;

    // ─── Hot-reload de configuración (ver MyRunFrame()) ────────────────────
    std::chrono::steady_clock::time_point m_lastSettingsCheck{};
    int32_t m_settingsCheckIntervalMs = 500;
};