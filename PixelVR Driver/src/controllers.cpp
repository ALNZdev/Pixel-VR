#include "controllers.h"
#include "driverlog.h"
#include "vrmath.h"
#include <mutex>
#include <cmath>
#include "pipe_handler.h"
#include "main.h"

namespace
{
    vr::DriverPose_t MakePositionAndPipeRotationPose(
        float x,
        float y,
        float z,
        float qw,
        float qx,
        float qy,
        float qz,
        bool tracked)
    {
        vr::DriverPose_t pose{};

        pose.deviceIsConnected = true;
        pose.poseIsValid = tracked;
        pose.result = tracked
            ? vr::TrackingResult_Running_OK
            : vr::TrackingResult_Running_OutOfRange;

        pose.vecPosition[0] = x;
        pose.vecPosition[1] = y;
        pose.vecPosition[2] = z;

        pose.qRotation = { qw, qx, qy, qz };
        pose.qWorldFromDriverRotation = { 1.0f, 0.0f, 0.0f, 0.0f };
        pose.qDriverFromHeadRotation = { 1.0f, 0.0f, 0.0f, 0.0f };

        pose.vecVelocity[0] = 0.0f;
        pose.vecVelocity[1] = 0.0f;
        pose.vecVelocity[2] = 0.0f;

        pose.vecAngularVelocity[0] = 0.0f;
        pose.vecAngularVelocity[1] = 0.0f;
        pose.vecAngularVelocity[2] = 0.0f;

        pose.poseTimeOffset = 0.0;

        return pose;
    }

    void NormalizeQuaternion(float& qw, float& qx, float& qy, float& qz)
    {
        const float length = std::sqrt(qw * qw + qx * qx + qy * qy + qz * qz);

        if (length <= 0.000001f)
        {
            qw = 1.0f;
            qx = 0.0f;
            qy = 0.0f;
            qz = 0.0f;
            return;
        }

        qw /= length;
        qx /= length;
        qy /= length;
        qz /= length;
    }
}

MyControllerDeviceDriver::MyControllerDeviceDriver(vr::ETrackedControllerRole role)
    : m_unObjectId(vr::k_unTrackedDeviceIndexInvalid), m_ulPropertyContainer(vr::k_ulInvalidPropertyContainer),
    m_eRole(role), m_bActive(false)
{
    m_inputHandles.fill(0);
    m_sSerialNumber = (role == vr::TrackedControllerRole_LeftHand) ? "Left" : "Right";
    m_sModelNumber = "PixelVR Controllers";
}

void MyControllerDeviceDriver::LoadSettings()
{
    // El intervalo de chequeo es global (se guarda una sola vez en la
    // sección "driver_pixelvr" del .vrsettings), lo comparten HMD y controles.
    m_settingsCheckIntervalMs = vr::VRSettings()->GetInt32("driver_pixelvr", "settingsCheckInterval");
    if (m_settingsCheckIntervalMs <= 0) m_settingsCheckIntervalMs = 500;

    m_bEnablePSMTracking = vr::VRSettings()->GetBool("driver_pixelvr", "enablePSMTracking");

    const std::string section = (m_eRole == vr::TrackedControllerRole_LeftHand)
        ? "driver_pixelvr_left_controller"
        : "driver_pixelvr_right_controller";

    m_fHapticAmplitude = vr::VRSettings()->GetFloat(section.c_str(), "hapticAmplitude");
    if (m_fHapticAmplitude <= 0.0f) m_fHapticAmplitude = 1.0f;

    m_bEnableHaptics = vr::VRSettings()->GetBool(section.c_str(), "enableHaptics");
    if (!m_bEnableHaptics) {
        m_fHapticAmplitude = 0.0f;
    }
}

vr::EVRInitError MyControllerDeviceDriver::Activate(uint32_t unObjectId) {
    m_unObjectId = unObjectId;
    m_ulPropertyContainer = vr::VRProperties()->TrackedDeviceToPropertyContainer(m_unObjectId);

    LoadSettings();

    vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, vr::Prop_ModelNumber_String, m_sModelNumber.c_str());
    vr::VRProperties()->SetInt32Property(m_ulPropertyContainer, vr::Prop_ControllerRoleHint_Int32, m_eRole);

    const char* renderModel = (m_eRole == vr::TrackedControllerRole_LeftHand)
        ? "{pixelvr}/rendermodels/oculus_quest_controller_left"
        : "{pixelvr}/rendermodels/oculus_quest_controller_right";
    vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, vr::Prop_RenderModelName_String, renderModel);

    const char* controllerType = (m_eRole == vr::TrackedControllerRole_LeftHand)
        ? "pixelvr_left" : "pixelvr_right";
    vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, vr::Prop_ControllerType_String, controllerType);

    const char* profilePath = (m_eRole == vr::TrackedControllerRole_LeftHand)
        ? "{pixelvr}/input/left/pixelvr_controller_left.json"
        : "{pixelvr}/input/right/pixelvr_controller_right.json";
    vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, vr::Prop_InputProfilePath_String, profilePath);

    vr::VRProperties()->SetInt32Property(m_ulPropertyContainer,
        vr::Prop_DeviceClass_Int32,
        vr::TrackedDeviceClass_Controller);

    vr::VRProperties()->SetBoolProperty(m_ulPropertyContainer,
        vr::Prop_WillDriftInYaw_Bool, false);

    vr::VRProperties()->SetBoolProperty(m_ulPropertyContainer,
        vr::Prop_DeviceIsWireless_Bool, true);

    vr::VRProperties()->SetBoolProperty(m_ulPropertyContainer,
        vr::Prop_DeviceProvidesBatteryStatus_Bool, false);

    vr::VRProperties()->SetStringProperty(m_ulPropertyContainer,
        vr::Prop_ManufacturerName_String,
        "PixelVR Project");

    vr::VRProperties()->SetInt32Property(m_ulPropertyContainer,
        vr::Prop_ControllerHandSelectionPriority_Int32,
        (m_eRole == vr::TrackedControllerRole_LeftHand) ? 1 : 0);

    if (m_eRole == vr::TrackedControllerRole_LeftHand)
    {
        vr::VRDriverInput()->CreateBooleanComponent(m_ulPropertyContainer, "/input/x/click", &m_inputHandles[MyComponent_x_click]);
        vr::VRDriverInput()->CreateBooleanComponent(m_ulPropertyContainer, "/input/y/click", &m_inputHandles[MyComponent_y_click]);

        vr::VRDriverInput()->CreateBooleanComponent(m_ulPropertyContainer, "/input/trigger/click", &m_inputHandles[MyComponent_trigger_click_left]);
        vr::VRDriverInput()->CreateScalarComponent(m_ulPropertyContainer, "/input/trigger/value", &m_inputHandles[MyComponent_trigger_value_left], vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedOneSided);
        vr::VRDriverInput()->CreateBooleanComponent(m_ulPropertyContainer, "/input/grip/click", &m_inputHandles[MyComponent_grip_click_left]);
        vr::VRDriverInput()->CreateScalarComponent(m_ulPropertyContainer, "/input/grip/value", &m_inputHandles[MyComponent_grip_value_left], vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedOneSided);

        vr::VRDriverInput()->CreateBooleanComponent(m_ulPropertyContainer, "/input/system/click", &m_inputHandles[MyComponent_system_click_left]);

        vr::VRDriverInput()->CreateScalarComponent(m_ulPropertyContainer, "/input/joystick/x", &m_inputHandles[MyComponent_thumbstick_x_left], vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided);
        vr::VRDriverInput()->CreateScalarComponent(m_ulPropertyContainer, "/input/joystick/y", &m_inputHandles[MyComponent_thumbstick_y_left], vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided);
        vr::VRDriverInput()->CreateBooleanComponent(m_ulPropertyContainer, "/input/joystick/click", &m_inputHandles[MyComponent_thumbstick_click_left]);
    }
    else if (m_eRole == vr::TrackedControllerRole_RightHand)
    {
        vr::VRDriverInput()->CreateBooleanComponent(m_ulPropertyContainer, "/input/a/click", &m_inputHandles[MyComponent_a_click]);
        vr::VRDriverInput()->CreateBooleanComponent(m_ulPropertyContainer, "/input/b/click", &m_inputHandles[MyComponent_b_click]);

        vr::VRDriverInput()->CreateBooleanComponent(m_ulPropertyContainer, "/input/trigger/click", &m_inputHandles[MyComponent_trigger_click_right]);
        vr::VRDriverInput()->CreateScalarComponent(m_ulPropertyContainer, "/input/trigger/value", &m_inputHandles[MyComponent_trigger_value_right], vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedOneSided);

        vr::VRDriverInput()->CreateBooleanComponent(m_ulPropertyContainer, "/input/grip/click", &m_inputHandles[MyComponent_grip_click_right]);
        vr::VRDriverInput()->CreateScalarComponent(m_ulPropertyContainer, "/input/grip/value", &m_inputHandles[MyComponent_grip_value_right], vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedOneSided);

        vr::VRDriverInput()->CreateBooleanComponent(m_ulPropertyContainer, "/input/system/click", &m_inputHandles[MyComponent_system_click_right]);

        vr::VRDriverInput()->CreateScalarComponent(m_ulPropertyContainer, "/input/joystick/x", &m_inputHandles[MyComponent_thumbstick_x_right], vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided);
        vr::VRDriverInput()->CreateScalarComponent(m_ulPropertyContainer, "/input/joystick/y", &m_inputHandles[MyComponent_thumbstick_y_right], vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided);
        vr::VRDriverInput()->CreateBooleanComponent(m_ulPropertyContainer, "/input/joystick/click", &m_inputHandles[MyComponent_thumbstick_click_right]);
    }

    m_bActive = true;
    return vr::VRInitError_None;
}

vr::DriverPose_t MyControllerDeviceDriver::GetPose()
{
    // Posición: por defecto viene de PSMoveServiceEx. Si el usuario apaga
    // PSM tracking (enablePSMTracking=false), usamos la posición que ya
    // viaja en el paquete del pipe (lPos/rPos) en su lugar.
    float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
    bool posTracked = false;

    if (!m_bEnablePSMTracking)
    {
        std::lock_guard<std::mutex> lock(g_DataMutex);
        if (m_eRole == vr::TrackedControllerRole_LeftHand)
        {
            posX = g_RemoteData.lPosX;
            posY = g_RemoteData.lPosY;
            posZ = g_RemoteData.lPosZ;
            posTracked = g_RemoteData.left_status;
        }
        else
        {
            posX = g_RemoteData.rPosX;
            posY = g_RemoteData.rPosY;
            posZ = g_RemoteData.rPosZ;
            posTracked = g_RemoteData.right_status;
        }
    }
    else
    {
        PSMTrackingManager::TrackingData tracking{};

        if (!g_psmTracking.GetTrackingData(tracking))
        {
            return MakePositionAndPipeRotationPose(
                0.0f, 0.0f, 0.0f,
                1.0f, 0.0f, 0.0f, 0.0f,
                false
            );
        }

        posTracked = (m_eRole == vr::TrackedControllerRole_LeftHand) ? tracking.leftTracked : tracking.rightTracked;
        posX = (m_eRole == vr::TrackedControllerRole_LeftHand) ? tracking.leftPosX : tracking.rightPosX;
        posY = (m_eRole == vr::TrackedControllerRole_LeftHand) ? tracking.leftPosY : tracking.rightPosY;
        posZ = (m_eRole == vr::TrackedControllerRole_LeftHand) ? tracking.leftPosZ : tracking.rightPosZ;
    }

    float qw = 1.0f;
    float qx = 0.0f;
    float qy = 0.0f;
    float qz = 0.0f;

    {
        std::lock_guard<std::mutex> lock(g_DataMutex);

        if (m_eRole == vr::TrackedControllerRole_LeftHand)
        {
            qw = g_RemoteData.left_qw;
            qx = g_RemoteData.left_qx;
            qy = g_RemoteData.left_qy;
            qz = g_RemoteData.left_qz;
        }
        else
        {
            qw = g_RemoteData.right_qw;
            qx = g_RemoteData.right_qx;
            qy = g_RemoteData.right_qy;
            qz = g_RemoteData.right_qz;
        }
    }

    NormalizeQuaternion(qw, qx, qy, qz);

    return MakePositionAndPipeRotationPose(
        posX, posY, posZ,
        qw, qx, qy, qz,
        posTracked
    );
}

void MyControllerDeviceDriver::MyRunFrame() {
    if (m_unObjectId == vr::k_unTrackedDeviceIndexInvalid) return;

    // ─── Hot-reload de default.vrsettings (ver hmd.cpp para más detalle) ──
    const auto now = std::chrono::steady_clock::now();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastSettingsCheck).count();

    if (elapsedMs >= m_settingsCheckIntervalMs)
    {
        LoadSettings();
        m_lastSettingsCheck = now;
    }

    vr::DriverPose_t pose = GetPose();
    vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_unObjectId, pose, sizeof(vr::DriverPose_t));

    std::lock_guard<std::mutex> lock(g_DataMutex);
    if (m_eRole == vr::TrackedControllerRole_LeftHand) {
        vr::VRDriverInput()->UpdateBooleanComponent(m_inputHandles[MyComponent_x_click], g_RemoteData.left_a, 0);
        vr::VRDriverInput()->UpdateBooleanComponent(m_inputHandles[MyComponent_y_click], g_RemoteData.left_b, 0);
        vr::VRDriverInput()->UpdateBooleanComponent(m_inputHandles[MyComponent_trigger_click_left], g_RemoteData.left_trigger, 0);
        vr::VRDriverInput()->UpdateScalarComponent(m_inputHandles[MyComponent_trigger_value_left], g_RemoteData.left_trigger ? 1.0f : 0.0f, 0);
        vr::VRDriverInput()->UpdateBooleanComponent(m_inputHandles[MyComponent_grip_click_left], g_RemoteData.left_grip, 0);
        vr::VRDriverInput()->UpdateScalarComponent(m_inputHandles[MyComponent_grip_value_left], g_RemoteData.left_grip ? 1.0f : 0.0f, 0);
        vr::VRDriverInput()->UpdateBooleanComponent(m_inputHandles[MyComponent_system_click_left], g_RemoteData.left_system, 0);
        vr::VRDriverInput()->UpdateScalarComponent(m_inputHandles[MyComponent_thumbstick_x_left], g_RemoteData.left_stick_x, 0);
        vr::VRDriverInput()->UpdateScalarComponent(m_inputHandles[MyComponent_thumbstick_y_left], g_RemoteData.left_stick_y, 0);
        vr::VRDriverInput()->UpdateBooleanComponent(m_inputHandles[MyComponent_thumbstick_click_left], g_RemoteData.left_stick_click, 0);
    }
    else if (m_eRole == vr::TrackedControllerRole_RightHand)
    {
        vr::VRDriverInput()->UpdateBooleanComponent(m_inputHandles[MyComponent_a_click], g_RemoteData.right_a, 0);
        vr::VRDriverInput()->UpdateBooleanComponent(m_inputHandles[MyComponent_b_click], g_RemoteData.right_b, 0);
        vr::VRDriverInput()->UpdateBooleanComponent(m_inputHandles[MyComponent_trigger_click_right], g_RemoteData.right_trigger, 0);
        vr::VRDriverInput()->UpdateScalarComponent(m_inputHandles[MyComponent_trigger_value_right], g_RemoteData.right_trigger ? 1.0f : 0.0f, 0);
        vr::VRDriverInput()->UpdateBooleanComponent(m_inputHandles[MyComponent_grip_click_right], g_RemoteData.right_grip, 0);
        vr::VRDriverInput()->UpdateScalarComponent(m_inputHandles[MyComponent_grip_value_right], g_RemoteData.right_grip ? 1.0f : 0.0f, 0);
        vr::VRDriverInput()->UpdateBooleanComponent(m_inputHandles[MyComponent_system_click_right], g_RemoteData.right_system, 0);
        vr::VRDriverInput()->UpdateScalarComponent(m_inputHandles[MyComponent_thumbstick_x_right], g_RemoteData.right_stick_x, 0);
        vr::VRDriverInput()->UpdateScalarComponent(m_inputHandles[MyComponent_thumbstick_y_right], g_RemoteData.right_stick_y, 0);
        vr::VRDriverInput()->UpdateBooleanComponent(m_inputHandles[MyComponent_thumbstick_click_right], g_RemoteData.right_stick_click, 0);
    }
}

void MyControllerDeviceDriver::Deactivate() { m_bActive = false; }
void MyControllerDeviceDriver::EnterStandby() {}
void* MyControllerDeviceDriver::GetComponent(const char* pchComponentNameAndVersion) { return nullptr; }
void MyControllerDeviceDriver::DebugRequest(const char* pchRequest, char* pchResponseBuffer, uint32_t unResponseBufferSize) {}