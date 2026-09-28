#include "hmd.h"

#include "driverlog.h"
#include "main.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace
{
    constexpr float kPi = 3.14159265358979323846f;

    constexpr int32_t kDefaultWindowWidth = 1920;
    constexpr int32_t kDefaultWindowHeight = 1080;

    constexpr int32_t kMinDisplayDimension = 64;
    constexpr int32_t kMaxDisplayDimension = 16384;

    constexpr float kMinFov = 1.0f;
    constexpr float kMaxFov = 179.0f;

    float ClampFloat(float value, float minimum, float maximum)
    {
        if (!std::isfinite(value))
            return minimum;

        if (value < minimum)
            return minimum;

        if (value > maximum)
            return maximum;

        return value;
    }

    int32_t ClampDimension(int32_t value, int32_t fallback)
    {
        if (value < kMinDisplayDimension ||
            value > kMaxDisplayDimension)
        {
            return fallback;
        }

        return value;
    }

    vr::DriverPose_t MakeHmdPose(
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

    void NormalizeQuaternion(
        float& qw,
        float& qx,
        float& qy,
        float& qz)
    {
        const float length = std::sqrt(
            qw * qw +
            qx * qx +
            qy * qy +
            qz * qz);

        if (!std::isfinite(length) || length <= 0.000001f)
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

MyHMDDeviceDriver::MyHMDDeviceDriver()
    : m_unObjectId(vr::k_unTrackedDeviceIndexInvalid)
    , m_ulPropertyContainer(vr::k_ulInvalidPropertyContainer)
{
    m_sSerialNumber = "PixelVR_HMD";
    m_sModelNumber = "PixelVR HMD";
}

void MyHMDDeviceDriver::ResetDisplayState()
{
    std::lock_guard<std::mutex> lock(m_displayStateMutex);

    m_displayConfigurationLocked = false;
    m_displayPropertiesApplied = false;
}

void MyHMDDeviceDriver::LoadSettings()
{
    const int32_t configuredInterval =
        vr::VRSettings()->GetInt32(
            "driver_pixelvr",
            "settingsCheckInterval");

    m_settingsCheckIntervalMs =
        configuredInterval > 0
        ? configuredInterval
        : 500;

    /*
     * IMPORTANTE:

     * windowX/windowY/windowWidth/windowHeight,
     * renderWidth/renderHeight, directMode y EDID
     * solo se leen durante la activación inicial.
     *
     * SteamVR no permite cambiar de forma segura el modo
     * de presentación o el adaptador gráfico después de crear
     * el compositor.
     */
    {
        std::lock_guard<std::mutex> lock(m_displayStateMutex);

        if (!m_displayConfigurationLocked)
        {
            m_windowX = vr::VRSettings()->GetInt32(
                "driver_pixelvr",
                "windowX");

            m_windowY = vr::VRSettings()->GetInt32(
                "driver_pixelvr",
                "windowY");

            const int32_t configuredWindowWidth =
                vr::VRSettings()->GetInt32(
                    "driver_pixelvr",
                    "windowWidth");

            const int32_t configuredWindowHeight =
                vr::VRSettings()->GetInt32(
                    "driver_pixelvr",
                    "windowHeight");

            const int32_t configuredRenderWidth =
                vr::VRSettings()->GetInt32(
                    "driver_pixelvr",
                    "renderWidth");

            const int32_t configuredRenderHeight =
                vr::VRSettings()->GetInt32(
                    "driver_pixelvr",
                    "renderHeight");

            m_windowWidth = static_cast<uint32_t>(
                ClampDimension(
                    configuredWindowWidth,
                    kDefaultWindowWidth));

            m_windowHeight = static_cast<uint32_t>(
                ClampDimension(
                    configuredWindowHeight,
                    kDefaultWindowHeight));

            m_renderWidth = static_cast<uint32_t>(
                ClampDimension(
                    configuredRenderWidth,
                    static_cast<int32_t>(m_windowWidth)));

            m_renderHeight = static_cast<uint32_t>(
                ClampDimension(
                    configuredRenderHeight,
                    static_cast<int32_t>(m_windowHeight)));

            m_bDebugMode = vr::VRSettings()->GetBool(
                "driver_pixelvr",
                "debugMode");

            m_bDirectMode = vr::VRSettings()->GetBool(
                "driver_pixelvr",
                "directMode");

            m_nEdidVid = vr::VRSettings()->GetInt32(
                "driver_pixelvr",
                "edidVid");

            m_nEdidPid = vr::VRSettings()->GetInt32(
                "driver_pixelvr",
                "edidPid");

            /*
             * Un EDID cero no identifica ningún display.
             * No se fuerza modo directo si faltan ambos valores.
             */
            if (m_bDirectMode &&
                (m_nEdidVid <= 0 || m_nEdidPid <= 0))
            {
                DriverLog(
                    "[HMD] EDID inválido para modo directo "
                    "(VID=%d PID=%d). Se usará modo ventana.",
                    m_nEdidVid,
                    m_nEdidPid);

                m_bDirectMode = false;
            }

            m_displayConfigurationLocked = true;

            DriverLog(
                "[HMD] Display config: mode=%s "
                "window=(%d,%d %ux%u) render=%ux%u "
                "edid=(%d,%d)",
                m_bDirectMode ? "direct" : "windowed",
                m_windowX,
                m_windowY,
                m_windowWidth,
                m_windowHeight,
                m_renderWidth,
                m_renderHeight,
                m_nEdidVid,
                m_nEdidPid);
        }
    }

    /*
     * Estos valores sí pueden actualizarse durante la sesión.
     * No modifican la ventana ni el backend gráfico.
     */
    m_fDisplayFrequency = vr::VRSettings()->GetFloat(
        "driver_pixelvr",
        "displayFrequency");

    if (!std::isfinite(m_fDisplayFrequency) ||
        m_fDisplayFrequency <= 0.0f)
    {
        m_fDisplayFrequency = 90.0f;
    }

    m_fIPD = vr::VRSettings()->GetFloat(
        "driver_pixelvr",
        "ipdMeters");

    if (!std::isfinite(m_fIPD) || m_fIPD <= 0.0f)
    {
        m_fIPD = 0.063f;
    }

    m_fFovLeft = ClampFloat(
        vr::VRSettings()->GetFloat(
            "driver_pixelvr",
            "fovLeftDegrees"),
        kMinFov,
        kMaxFov);

    m_fFovRight = ClampFloat(
        vr::VRSettings()->GetFloat(
            "driver_pixelvr",
            "fovRightDegrees"),
        kMinFov,
        kMaxFov);

    m_fFovTop = ClampFloat(
        vr::VRSettings()->GetFloat(
            "driver_pixelvr",
            "fovTopDegrees"),
        kMinFov,
        kMaxFov);

    m_fFovBottom = ClampFloat(
        vr::VRSettings()->GetFloat(
            "driver_pixelvr",
            "fovBottomDegrees"),
        kMinFov,
        kMaxFov);

    m_fTrackingScale = vr::VRSettings()->GetFloat(
        "driver_pixelvr",
        "trackingScale");

    if (!std::isfinite(m_fTrackingScale) ||
        m_fTrackingScale <= 0.0f)
    {
        m_fTrackingScale = 1.0f;
    }

    m_bEnablePositionTracking =
        vr::VRSettings()->GetBool(
            "driver_pixelvr",
            "enablePositionTracking");

    m_bEnableRotationTracking =
        vr::VRSettings()->GetBool(
            "driver_pixelvr",
            "enableRotationTracking");

    m_bEnablePSMTracking =
        vr::VRSettings()->GetBool(
            "driver_pixelvr",
            "enablePSMTracking");

    m_iPSMTrackingTimeout =
        vr::VRSettings()->GetInt32(
            "driver_pixelvr",
            "psmTrackingTimeout");

    if (m_iPSMTrackingTimeout <= 0)
        m_iPSMTrackingTimeout = 5000;

    m_bVerboseLogging =
        vr::VRSettings()->GetBool(
            "driver_pixelvr",
            "verboseLogging");

    m_bEnableAsyncReprojection =
        vr::VRSettings()->GetBool(
            "driver_pixelvr",
            "enableAsyncReprojection");

    m_bVsyncEnabled =
        vr::VRSettings()->GetBool(
            "driver_pixelvr",
            "vsyncEnabled");

    if (m_ulPropertyContainer !=
        vr::k_ulInvalidPropertyContainer)
    {
        ApplyRuntimeProperties();
    }
}

void MyHMDDeviceDriver::ApplyRuntimeProperties()
{
    if (m_ulPropertyContainer ==
        vr::k_ulInvalidPropertyContainer)
    {
        return;
    }

    vr::VRProperties()->SetFloatProperty(
        m_ulPropertyContainer,
        vr::Prop_UserIpdMeters_Float,
        m_fIPD);

    vr::VRProperties()->SetFloatProperty(
        m_ulPropertyContainer,
        vr::Prop_DisplayFrequency_Float,
        m_fDisplayFrequency);

    vr::VRProperties()->SetFloatProperty(
        m_ulPropertyContainer,
        vr::Prop_FieldOfViewLeftDegrees_Float,
        m_fFovLeft);

    vr::VRProperties()->SetFloatProperty(
        m_ulPropertyContainer,
        vr::Prop_FieldOfViewRightDegrees_Float,
        m_fFovRight);

    vr::VRProperties()->SetFloatProperty(
        m_ulPropertyContainer,
        vr::Prop_FieldOfViewTopDegrees_Float,
        m_fFovTop);

    vr::VRProperties()->SetFloatProperty(
        m_ulPropertyContainer,
        vr::Prop_FieldOfViewBottomDegrees_Float,
        m_fFovBottom);
}

void MyHMDDeviceDriver::ApplyDisplayProperties()
{
    if (m_ulPropertyContainer ==
        vr::k_ulInvalidPropertyContainer)
    {
        return;
    }

    vr::VRProperties()->SetBoolProperty(
        m_ulPropertyContainer,
        vr::Prop_DisplayDebugMode_Bool,
        m_bDebugMode);

    if (m_bDirectMode)
    {
        vr::VRProperties()->SetBoolProperty(
            m_ulPropertyContainer,
            vr::Prop_IsOnDesktop_Bool,
            false);

        vr::VRProperties()->SetBoolProperty(
            m_ulPropertyContainer,
            vr::Prop_HasDriverDirectModeComponent_Bool,
            true);

        vr::VRProperties()->SetInt32Property(
            m_ulPropertyContainer,
            vr::Prop_EdidVendorID_Int32,
            m_nEdidVid);

        vr::VRProperties()->SetInt32Property(
            m_ulPropertyContainer,
            vr::Prop_EdidProductID_Int32,
            m_nEdidPid);

        DriverLog(
            "[HMD] Display mode: direct "
            "VID=%d PID=%d",
            m_nEdidVid,
            m_nEdidPid);
    }
    else
    {
        /*
         * En modo ventana SteamVR debe tratar el HMD como una
         * pantalla extendida/virtual. No se publica EDID de un
         * display real en este modo.
         */
        vr::VRProperties()->SetBoolProperty(
            m_ulPropertyContainer,
            vr::Prop_IsOnDesktop_Bool,
            true);

        vr::VRProperties()->SetBoolProperty(
            m_ulPropertyContainer,
            vr::Prop_HasDriverDirectModeComponent_Bool,
            false);

        vr::VRProperties()->SetInt32Property(
            m_ulPropertyContainer,
            vr::Prop_EdidVendorID_Int32,
            0);

        vr::VRProperties()->SetInt32Property(
            m_ulPropertyContainer,
            vr::Prop_EdidProductID_Int32,
            0);

        DriverLog(
            "[HMD] Display mode: windowed "
            "bounds=(%d,%d %ux%u)",
            m_windowX,
            m_windowY,
            m_windowWidth,
            m_windowHeight);
    }

    m_displayPropertiesApplied = true;
}

vr::EVRInitError MyHMDDeviceDriver::Activate(uint32_t unObjectId)
{
    m_unObjectId = unObjectId;

    m_ulPropertyContainer =
        vr::VRProperties()->TrackedDeviceToPropertyContainer(
            m_unObjectId);

    ResetDisplayState();

    /*
     * Esta es la única lectura inicial del modo de pantalla.
     * Después de ApplyDisplayProperties() la configuración queda
     * congelada hasta que SteamVR desactive y vuelva a activar
     * el dispositivo.
     */
    LoadSettings();

    vr::VRProperties()->SetStringProperty(
        m_ulPropertyContainer,
        vr::Prop_SerialNumber_String,
        m_sSerialNumber.c_str());

    vr::VRProperties()->SetStringProperty(
        m_ulPropertyContainer,
        vr::Prop_ModelNumber_String,
        m_sModelNumber.c_str());

    vr::VRProperties()->SetInt32Property(
        m_ulPropertyContainer,
        vr::Prop_DeviceClass_Int32,
        vr::TrackedDeviceClass_HMD);

    vr::VRProperties()->SetStringProperty(
        m_ulPropertyContainer,
        vr::Prop_ManufacturerName_String,
        "PixelVR Project");

    vr::VRProperties()->SetStringProperty(
        m_ulPropertyContainer,
        vr::Prop_RenderModelName_String,
        "{pixelvr}/rendermodels/hmd");

    ApplyRuntimeProperties();
    ApplyDisplayProperties();

    DriverLog("[HMD] Activated successfully");

    return vr::VRInitError_None;
}

void MyHMDDeviceDriver::Deactivate()
{
    DriverLog("[HMD] Deactivated");

    m_unObjectId = vr::k_unTrackedDeviceIndexInvalid;
    m_ulPropertyContainer = vr::k_ulInvalidPropertyContainer;

    ResetDisplayState();
}

void MyHMDDeviceDriver::EnterStandby()
{
}

void MyHMDDeviceDriver::GetWindowBounds(
    int32_t* pnX,
    int32_t* pnY,
    uint32_t* pnWidth,
    uint32_t* pnHeight)
{
    if (pnX == nullptr ||
        pnY == nullptr ||
        pnWidth == nullptr ||
        pnHeight == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_displayStateMutex);

    /*
     * Estas coordenadas son absolutas dentro del escritorio virtual
     * de Windows. No deben ser transformadas ni normalizadas.
     * SteamVR las consulta al crear la ventana.
     */
    *pnX = m_windowX;
    *pnY = m_windowY;
    *pnWidth = m_windowWidth;
    *pnHeight = m_windowHeight;
}

bool MyHMDDeviceDriver::IsDisplayOnDesktop()
{
    std::lock_guard<std::mutex> lock(m_displayStateMutex);
    return !m_bDirectMode;
}

bool MyHMDDeviceDriver::IsDisplayRealDisplay()
{
    std::lock_guard<std::mutex> lock(m_displayStateMutex);
    return m_bDirectMode;
}

void MyHMDDeviceDriver::GetRecommendedRenderTargetSize(
    uint32_t* pnWidth,
    uint32_t* pnHeight)
{
    if (pnWidth == nullptr || pnHeight == nullptr)
        return;

    std::lock_guard<std::mutex> lock(m_displayStateMutex);

    *pnWidth = m_renderWidth;
    *pnHeight = m_renderHeight;
}

void MyHMDDeviceDriver::GetEyeOutputViewport(
    vr::EVREye eEye,
    uint32_t* pnX,
    uint32_t* pnY,
    uint32_t* pnWidth,
    uint32_t* pnHeight)
{
    if (pnX == nullptr ||
        pnY == nullptr ||
        pnWidth == nullptr ||
        pnHeight == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_displayStateMutex);

    /*
     * El viewport es relativo a la ventana, no al escritorio.
     * Las coordenadas absolutas del monitor solo se devuelven
     * mediante GetWindowBounds().
     */
    const uint32_t eyeWidth = m_windowWidth / 2;

    *pnY = 0;
    *pnWidth = eyeWidth;
    *pnHeight = m_windowHeight;
    *pnX = eEye == vr::Eye_Left
        ? 0
        : eyeWidth;
}

void MyHMDDeviceDriver::GetProjectionRaw(
    vr::EVREye /*eEye*/,
    float* pfLeft,
    float* pfRight,
    float* pfTop,
    float* pfBottom)
{
    if (pfLeft == nullptr ||
        pfRight == nullptr ||
        pfTop == nullptr ||
        pfBottom == nullptr)
    {
        return;
    }

    const float left =
        std::tan(m_fFovLeft * kPi / 360.0f);

    const float right =
        std::tan(m_fFovRight * kPi / 360.0f);

    const float top =
        std::tan(m_fFovTop * kPi / 360.0f);

    const float bottom =
        std::tan(m_fFovBottom * kPi / 360.0f);

    *pfLeft = -left;
    *pfRight = right;
    *pfTop = -top;
    *pfBottom = bottom;
}

vr::DistortionCoordinates_t MyHMDDeviceDriver::ComputeDistortion(
    vr::EVREye /*eEye*/,
    float fU,
    float fV)
{
    vr::DistortionCoordinates_t coordinates{};

    coordinates.rfBlue[0] = fU;
    coordinates.rfBlue[1] = fV;

    coordinates.rfGreen[0] = fU;
    coordinates.rfGreen[1] = fV;

    coordinates.rfRed[0] = fU;
    coordinates.rfRed[1] = fV;

    return coordinates;
}

bool MyHMDDeviceDriver::ComputeInverseDistortion(
    vr::HmdVector2_t* pResult,
    vr::EVREye /*eEye*/,
    uint32_t /*unChannel*/,
    float fU,
    float fV)
{
    if (pResult == nullptr)
        return false;

    pResult->v[0] = fU;
    pResult->v[1] = fV;

    return true;
}

vr::DriverPose_t MyHMDDeviceDriver::GetPose()
{
    float rawPosX = 0.0f;
    float rawPosY = 0.0f;
    float rawPosZ = 0.0f;
    bool rawTracked = false;

    if (!m_bEnablePSMTracking)
    {
        std::lock_guard<std::mutex> lock(g_DataMutex);

        rawPosX = g_RemoteData.hmdPosX;
        rawPosY = g_RemoteData.hmdPosY;
        rawPosZ = g_RemoteData.hmdPosZ;

        /*
         * El paquete del pipe no tiene un flag independiente
         * para la posición del HMD.
         */
        rawTracked = true;
    }
    else
    {
        PSMTrackingManager::TrackingData tracking{};

        if (!g_psmTracking.GetTrackingData(tracking))
        {
            return MakeHmdPose(
                0.0f,
                0.0f,
                0.0f,
                1.0f,
                0.0f,
                0.0f,
                0.0f,
                false);
        }

        rawPosX = tracking.hmdPosX;
        rawPosY = tracking.hmdPosY;
        rawPosZ = tracking.hmdPosZ;
        rawTracked = tracking.hmdTracked;
    }

    float qw = 1.0f;
    float qx = 0.0f;
    float qy = 0.0f;
    float qz = 0.0f;

    {
        std::lock_guard<std::mutex> lock(g_DataMutex);

        qw = g_RemoteData.hmd_qw;
        qx = g_RemoteData.hmd_qx;
        qy = g_RemoteData.hmd_qy;
        qz = g_RemoteData.hmd_qz;
    }

    NormalizeQuaternion(qw, qx, qy, qz);

    float posX = rawPosX;
    float posY = rawPosY;
    float posZ = rawPosZ;

    if (!m_bEnablePositionTracking)
    {
        posX = 0.0f;
        posY = 0.0f;
        posZ = 0.0f;
    }

    if (!m_bEnableRotationTracking)
    {
        qw = 1.0f;
        qx = 0.0f;
        qy = 0.0f;
        qz = 0.0f;
    }

    posX *= m_fTrackingScale;
    posY *= m_fTrackingScale;
    posZ *= m_fTrackingScale;

    return MakeHmdPose(
        posX,
        posY,
        posZ,
        qw,
        qx,
        qy,
        qz,
        rawTracked);
}

void MyHMDDeviceDriver::MyRunFrame()
{
    if (m_unObjectId ==
        vr::k_unTrackedDeviceIndexInvalid)
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_lastSettingsCheck)
        .count();

    if (elapsedMs >= m_settingsCheckIntervalMs)
    {
        /*
         * LoadSettings() no modifica nunca directMode,
         * EDID ni los límites de la ventana después de Activate().
         * Por tanto no puede provocar una migración de ventana
         * entre GPUs o monitores.
         */
        LoadSettings();
        m_lastSettingsCheck = now;
    }

    vr::VRServerDriverHost()->TrackedDevicePoseUpdated(
        m_unObjectId,
        GetPose(),
        sizeof(vr::DriverPose_t));
}

void* MyHMDDeviceDriver::GetComponent(
    const char* pchComponentNameAndVersion)
{
    if (pchComponentNameAndVersion == nullptr)
        return nullptr;

    if (_stricmp(
        pchComponentNameAndVersion,
        vr::IVRDisplayComponent_Version) == 0)
    {
        return static_cast<vr::IVRDisplayComponent*>(this);
    }

    return nullptr;
}

void MyHMDDeviceDriver::DebugRequest(
    const char* /*pchRequest*/,
    char* pchResponseBuffer,
    uint32_t unResponseBufferSize)
{
    if (pchResponseBuffer != nullptr &&
        unResponseBufferSize > 0)
    {
        pchResponseBuffer[0] = '\0';
    }
}