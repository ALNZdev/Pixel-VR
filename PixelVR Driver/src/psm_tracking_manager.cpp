#include "psm_tracking_manager.h"
#include "driverlog.h"

#include <chrono>
#include <cstring>

PSMTrackingManager::PSMTrackingManager()
    : m_hmdId(PSM_INVALID_HMD_ID),
    m_controllerLeftId(PSM_INVALID_CONTROLLER_ID),
    m_controllerRightId(PSM_INVALID_CONTROLLER_ID),
    m_bConnected(false),
    m_bRunning(false),
    m_updateThread(nullptr)
{
    memset(&m_currentData, 0, sizeof(TrackingData));
}

PSMTrackingManager::~PSMTrackingManager()
{
    Shutdown();
}

bool PSMTrackingManager::Initialize()
{
    if (m_bConnected)
        return true;

    DriverLog("[PSMTracking] Intentando conectar a PSMoveService...");
    PSMResult result = PSM_Initialize("localhost", "9512", 3000);

    if (result != PSMResult_Success)
    {
        DriverLog("[PSMTracking] ERROR: No se pudo conectar a PSMoveService (codigo: %d)", result);
        DriverLog("[PSMTracking] Asegúrate de que PSMoveServiceEx está ejecutándose");
        return false;
    }

    DriverLog("[PSMTracking] Conectado exitosamente a PSMoveService");

    PSMHmdList hmdList;
    memset(&hmdList, 0, sizeof(PSMHmdList));
    PSM_GetHmdList(&hmdList, PSM_DEFAULT_TIMEOUT);

    if (hmdList.count > 0)
    {
        m_hmdId = hmdList.hmd_id[0];
        DriverLog("[PSMTracking] HMD encontrado: ID=%d", m_hmdId);

        unsigned int streamFlags =
            PSMControllerDataStreamFlags::PSMStreamFlags_includePositionData |
            PSMControllerDataStreamFlags::PSMStreamFlags_includePhysicsData |
            PSMControllerDataStreamFlags::PSMStreamFlags_includeCalibratedSensorData;

        if (PSM_AllocateHmdListener(m_hmdId) == PSMResult_Success &&
            PSM_StartHmdDataStream(m_hmdId, streamFlags, PSM_DEFAULT_TIMEOUT) == PSMResult_Success)
        {
            DriverLog("[PSMTracking] HMD stream iniciado exitosamente");
        }
        else
        {
            DriverLog("[PSMTracking] ERROR: No se pudo iniciar HMD stream");
            PSM_Shutdown();
            return false;
        }
    }
    else
    {
        DriverLog("[PSMTracking] ADVERTENCIA: No hay HMD disponible en PSMoveService");
    }

    PSMControllerList controllerList;
    memset(&controllerList, 0, sizeof(PSMControllerList));
    PSM_GetControllerList(&controllerList, PSM_DEFAULT_TIMEOUT);

    DriverLog("[PSMTracking] Controladores encontrados: %d", controllerList.count);

    for (int i = 0; i < controllerList.count; ++i)
    {
        DriverLog("[PSMTracking] Controller index=%d id=%d serial=%s type=%d",
            i,
            controllerList.controller_id[i],
            controllerList.controller_serial[i],
            static_cast<int>(controllerList.controller_type[i]));
    }

    // Ajusta este mapeo según cómo aparezcan los mandos.
    if (controllerList.count > 1)
    {
        m_controllerLeftId = controllerList.controller_id[1];
        unsigned int streamFlags =
            PSMControllerDataStreamFlags::PSMStreamFlags_includePositionData |
            PSMControllerDataStreamFlags::PSMStreamFlags_includePhysicsData |
            PSMControllerDataStreamFlags::PSMStreamFlags_includeCalibratedSensorData;

        if (PSM_AllocateControllerListener(m_controllerLeftId) == PSMResult_Success &&
            PSM_StartControllerDataStream(m_controllerLeftId, streamFlags, PSM_DEFAULT_TIMEOUT) == PSMResult_Success)
        {
            DriverLog("[PSMTracking] Controlador izquierdo stream iniciado");
        }
    }

    if (controllerList.count > 0)
    {
        m_controllerRightId = controllerList.controller_id[0];
        unsigned int streamFlags =
            PSMControllerDataStreamFlags::PSMStreamFlags_includePositionData |
            PSMControllerDataStreamFlags::PSMStreamFlags_includePhysicsData |
            PSMControllerDataStreamFlags::PSMStreamFlags_includeCalibratedSensorData;

        if (PSM_AllocateControllerListener(m_controllerRightId) == PSMResult_Success &&
            PSM_StartControllerDataStream(m_controllerRightId, streamFlags, PSM_DEFAULT_TIMEOUT) == PSMResult_Success)
        {
            DriverLog("[PSMTracking] Controlador derecho stream iniciado");
        }
    }

    m_bConnected = true;
    m_bRunning = true;

    m_updateThread = new std::thread(PSMTrackingManager::UpdateThreadEntry, this);
    DriverLog("[PSMTracking] Thread de actualización iniciado");

    return true;
}

void PSMTrackingManager::UpdateThreadEntry(PSMTrackingManager* pThis)
{
    if (pThis)
        pThis->UpdateThread();
}

void PSMTrackingManager::UpdateThread()
{
    int sleepMs = 1000 / PSM_UPDATE_RATE_HZ;

    while (m_bRunning && m_bConnected)
    {
        PSM_Update();

        {
            std::lock_guard<std::mutex> lock(m_dataMutex);

            if (m_hmdId != PSM_INVALID_HMD_ID)
            {
                PSMVector3f hmdPos{};
                if (PSM_GetHmdPosition(m_hmdId, &hmdPos) == PSMResult_Success)
                {
                    m_currentData.hmdPosX = hmdPos.x * K_SCALE_PSMMOVE_TO_METERS;
                    m_currentData.hmdPosY = hmdPos.y * K_SCALE_PSMMOVE_TO_METERS;
                    m_currentData.hmdPosZ = hmdPos.z * K_SCALE_PSMMOVE_TO_METERS;
                }

                PSM_GetIsHmdTracking(m_hmdId, &m_currentData.hmdTracked);
            }

            if (m_controllerLeftId != PSM_INVALID_CONTROLLER_ID)
            {
                PSMVector3f leftPos{};
                if (PSM_GetControllerPosition(m_controllerLeftId, &leftPos) == PSMResult_Success)
                {
                    m_currentData.leftPosX = leftPos.x * K_SCALE_PSMMOVE_TO_METERS;
                    m_currentData.leftPosY = leftPos.y * K_SCALE_PSMMOVE_TO_METERS;
                    m_currentData.leftPosZ = leftPos.z * K_SCALE_PSMMOVE_TO_METERS;
                }

                PSM_GetIsControllerTracking(m_controllerLeftId, &m_currentData.leftTracked);
            }

            if (m_controllerRightId != PSM_INVALID_CONTROLLER_ID)
            {
                PSMVector3f rightPos{};
                if (PSM_GetControllerPosition(m_controllerRightId, &rightPos) == PSMResult_Success)
                {
                    m_currentData.rightPosX = rightPos.x * K_SCALE_PSMMOVE_TO_METERS;
                    m_currentData.rightPosY = rightPos.y * K_SCALE_PSMMOVE_TO_METERS;
                    m_currentData.rightPosZ = rightPos.z * K_SCALE_PSMMOVE_TO_METERS;
                }

                PSM_GetIsControllerTracking(m_controllerRightId, &m_currentData.rightTracked);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    }
}

bool PSMTrackingManager::GetTrackingData(TrackingData& outData)
{
    if (!m_bConnected)
        return false;

    std::lock_guard<std::mutex> lock(m_dataMutex);
    outData = m_currentData;
    return true;
}

void PSMTrackingManager::Shutdown()
{
    m_bRunning = false;

    if (m_updateThread)
    {
        m_updateThread->join();
        delete m_updateThread;
        m_updateThread = nullptr;
    }

    if (m_bConnected)
    {
        if (m_hmdId != PSM_INVALID_HMD_ID)
        {
            PSM_StopHmdDataStream(m_hmdId, PSM_DEFAULT_TIMEOUT);
            PSM_FreeHmdListener(m_hmdId);
        }

        if (m_controllerLeftId != PSM_INVALID_CONTROLLER_ID)
        {
            PSM_StopControllerDataStream(m_controllerLeftId, PSM_DEFAULT_TIMEOUT);
            PSM_FreeControllerListener(m_controllerLeftId);
        }

        if (m_controllerRightId != PSM_INVALID_CONTROLLER_ID)
        {
            PSM_StopControllerDataStream(m_controllerRightId, PSM_DEFAULT_TIMEOUT);
            PSM_FreeControllerListener(m_controllerRightId);
        }

        PSM_Shutdown();
        m_bConnected = false;

        DriverLog("[PSMTracking] Desconectado de PSMoveService");
    }
}