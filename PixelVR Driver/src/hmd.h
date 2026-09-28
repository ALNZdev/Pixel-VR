#pragma once

#include <string>
#include <chrono>
#include <mutex>

#include "openvr_driver.h"
#include "pipe_handler.h"

class MyHMDDeviceDriver final
    : public vr::ITrackedDeviceServerDriver
    , public vr::IVRDisplayComponent
{
public:
    MyHMDDeviceDriver();
    ~MyHMDDeviceDriver() = default;

    // ITrackedDeviceServerDriver
    vr::EVRInitError Activate(uint32_t unObjectId) override;
    void Deactivate() override;
    void EnterStandby() override;
    void* GetComponent(const char* pchComponentNameAndVersion) override;
    void DebugRequest(
        const char* pchRequest,
        char* pchResponseBuffer,
        uint32_t unResponseBufferSize) override;
    vr::DriverPose_t GetPose() override;

    // IVRDisplayComponent
    void GetWindowBounds(
        int32_t* pnX,
        int32_t* pnY,
        uint32_t* pnWidth,
        uint32_t* pnHeight) override;

    bool IsDisplayOnDesktop() override;
    bool IsDisplayRealDisplay() override;

    void GetRecommendedRenderTargetSize(
        uint32_t* pnWidth,
        uint32_t* pnHeight) override;

    void GetEyeOutputViewport(
        vr::EVREye eEye,
        uint32_t* pnX,
        uint32_t* pnY,
        uint32_t* pnWidth,
        uint32_t* pnHeight) override;

    void GetProjectionRaw(
        vr::EVREye eEye,
        float* pfLeft,
        float* pfRight,
        float* pfTop,
        float* pfBottom) override;

    vr::DistortionCoordinates_t ComputeDistortion(
        vr::EVREye eEye,
        float fU,
        float fV) override;

    bool ComputeInverseDistortion(
        vr::HmdVector2_t* pResult,
        vr::EVREye eEye,
        uint32_t unChannel,
        float fU,
        float fV) override;

    // Funciones internas del driver
    void MyRunFrame();
    void LoadSettings();

    const std::string& GetSerialNumber() const
    {
        return m_sSerialNumber;
    }

private:
    void ApplyDisplayProperties();
    void ApplyRuntimeProperties();
    void ResetDisplayState();

private:
    vr::TrackedDeviceIndex_t m_unObjectId;
    vr::PropertyContainerHandle_t m_ulPropertyContainer;

    std::string m_sSerialNumber;
    std::string m_sModelNumber;

    // Configuración de la ventana del visor.
    // Las coordenadas pueden ser negativas cuando el monitor está
    // situado a la izquierda o encima del monitor principal.
    int32_t m_windowX = 0;
    int32_t m_windowY = 0;
    uint32_t m_windowWidth = 1920;
    uint32_t m_windowHeight = 1080;

    // Resolución interna recomendada para SteamVR.
    uint32_t m_renderWidth = 1920;
    uint32_t m_renderHeight = 1080;

    // El modo de pantalla solamente se aplica durante Activate().
    // No debe cambiar durante la ejecución del compositor.
    bool m_bDebugMode = false;
    bool m_bDirectMode = false;

    // EDID para modo direct (solo se usa si m_bDirectMode == true)
    int32_t m_nEdidVid = 0;
    int32_t m_nEdidPid = 0;

    float m_fFovLeft = 60.0f;
    float m_fFovRight = 60.0f;
    float m_fFovTop = 60.0f;
    float m_fFovBottom = 60.0f;

    float m_fIPD = 0.063f;
    float m_fDisplayFrequency = 90.0f;
    float m_fTrackingScale = 1.0f;

    bool m_bEnablePositionTracking = true;
    bool m_bEnableRotationTracking = true;
    bool m_bEnablePSMTracking = true;

    int32_t m_iPSMTrackingTimeout = 5000;

    bool m_bVerboseLogging = false;
    bool m_bEnableAsyncReprojection = true;
    bool m_bVsyncEnabled = true;

    // Hot reload únicamente para valores seguros:
    // tracking, FOV, IPD, frecuencia y logging.
    std::chrono::steady_clock::time_point m_lastSettingsCheck{};
    int32_t m_settingsCheckIntervalMs = 500;

    // Protege la configuración que puede ser consultada simultáneamente
    // por SteamVR y por el hilo de RunFrame().
    mutable std::mutex m_displayStateMutex;

    // Los valores de display quedan congelados después de Activate().
    // Esto evita que SteamVR cambie de modo o reposicione la ventana
    // a mitad de una sesión.
    bool m_displayConfigurationLocked = false;
    bool m_displayPropertiesApplied = false;
};