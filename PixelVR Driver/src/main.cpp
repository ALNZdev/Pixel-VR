#include "main.h"
#include "controllers.h"
#include "hmd.h"
#include "pipe_handler.h"
#include "driverlog.h"
#include "video_stream_pipeline.h"

PSMTrackingManager g_psmTracking;
MyControllerDeviceDriver* g_pLeftController = nullptr;
MyControllerDeviceDriver* g_pRightController = nullptr;

vr::EVRInitError MyDeviceProvider::Init(vr::IVRDriverContext* pDriverContext) {
    VR_INIT_SERVER_DRIVER_CONTEXT(pDriverContext);
    DriverLog("[Main] ===== PixelVR Driver Iniciado =====");
    DriverLog("[Main] Versión: 1.0");

    StartPipeThread();
    DriverLog("[Main] Pipe thread iniciado");

    DriverLog("[Main] Iniciando PSMoveServiceEx...");
    if (g_psmTracking.Initialize())
    {
        DriverLog("[Main] ✓ PSMoveServiceEx conectado");
    }
    else
    {
        DriverLog("[Main] ✗ PSMoveServiceEx no disponible (continuando sin él)");
    }

    my_left_controller_device_ = std::make_unique<MyControllerDeviceDriver>(vr::TrackedControllerRole_LeftHand);
    vr::VRServerDriverHost()->TrackedDeviceAdded(my_left_controller_device_->GetSerialNumber().c_str(), vr::TrackedDeviceClass_Controller, my_left_controller_device_.get());
    DriverLog("[Main] Controlador izquierdo registrado");

    my_right_controller_device_ = std::make_unique<MyControllerDeviceDriver>(vr::TrackedControllerRole_RightHand);
    vr::VRServerDriverHost()->TrackedDeviceAdded(my_right_controller_device_->GetSerialNumber().c_str(), vr::TrackedDeviceClass_Controller, my_right_controller_device_.get());
    DriverLog("[Main] Controlador derecho registrado");

    m_hmd_device = std::make_unique<MyHMDDeviceDriver>();
    if (m_hmd_device)
    {
        vr::VRServerDriverHost()->TrackedDeviceAdded(m_hmd_device->GetSerialNumber().c_str(), vr::TrackedDeviceClass_HMD, m_hmd_device.get());
        DriverLog("[Main] HMD registrado");
    }

    DriverLog("[Main] ===== Driver listo =====");
    return vr::VRInitError_None;
}

void MyDeviceProvider::RunFrame() {
    if (my_left_controller_device_) my_left_controller_device_->MyRunFrame();
    if (my_right_controller_device_) my_right_controller_device_->MyRunFrame();
    if (m_hmd_device) m_hmd_device->MyRunFrame();
}

void MyDeviceProvider::Cleanup() {
    VideoStreamPipeline::Instance().Stop();
    StopPipeThread();
    m_hmd_device.reset();
    my_left_controller_device_.reset();
    my_right_controller_device_.reset();

    VR_CLEANUP_SERVER_DRIVER_CONTEXT();
}

const char* const* MyDeviceProvider::GetInterfaceVersions() { return vr::k_InterfaceVersions; }

bool MyDeviceProvider::ShouldBlockStandbyMode() { return false; }

void MyDeviceProvider::EnterStandby() {}

void MyDeviceProvider::LeaveStandby() {}