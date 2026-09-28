#pragma once

#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include "PSMoveClient_CAPI.h"

#ifndef PSM_INVALID_HMD_ID
#define PSM_INVALID_HMD_ID (-1)
#endif

#ifndef PSM_INVALID_CONTROLLER_ID
#define PSM_INVALID_CONTROLLER_ID (-1)
#endif

class PSMTrackingManager {
public:
    PSMTrackingManager();
    ~PSMTrackingManager();

    struct TrackingData {
        // HMD
        float hmdPosX, hmdPosY, hmdPosZ;
        bool hmdTracked;

        // Left Controller
        float leftPosX, leftPosY, leftPosZ;
        bool leftTracked;

        // Right Controller
        float rightPosX, rightPosY, rightPosZ;
        bool rightTracked;

        // Legacy, no se usan
        float hmdQw, hmdQx, hmdQy, hmdQz;
        float leftQw, leftQx, leftQy, leftQz;
        float rightQw, rightQx, rightQy, rightQz;
    };

    bool Initialize();
    bool GetTrackingData(TrackingData& outData);
    void Shutdown();
    bool IsConnected() const { return m_bConnected; }

private:
    void UpdateThread();
    static void UpdateThreadEntry(PSMTrackingManager* pThis);

    PSMHmdID m_hmdId;
    PSMControllerID m_controllerLeftId;
    PSMControllerID m_controllerRightId;

    bool m_bConnected;
    bool m_bRunning;
    std::thread* m_updateThread;
    std::mutex m_dataMutex;

    TrackingData m_currentData;

    static constexpr int PSM_UPDATE_RATE_HZ = 120;
    static constexpr float K_SCALE_PSMMOVE_TO_METERS = 0.01f;  // PSMoveServiceEx da cm
};