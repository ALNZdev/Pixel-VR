#pragma once
#include <windows.h>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <chrono>

// AllData: estado unificado consumido por los dispositivos SteamVR. Lo puede
// alimentar el receptor USB directamente o el pipe de pruebas de Connect.
// Las rotaciones WXYZ vienen del pipe.
// Las posiciones XYZ vienen de PSMoveServiceEx.
// Los botones y touch también vienen del pipe.
#pragma pack(push, 1)
struct AllData {
    // --- HMD ---
    float hmdPosX, hmdPosY, hmdPosZ;
    float hmd_qw, hmd_qx, hmd_qy, hmd_qz;
    float hmd_error;
    float hmdAngularVelocityX, hmdAngularVelocityY, hmdAngularVelocityZ;

    // --- LEFT CONTROLLER ---
    float lPosX, lPosY, lPosZ;
    float left_qw, left_qx, left_qy, left_qz;
    bool  left_a, left_b, left_system, left_grip, left_trigger, left_stick_click;
    float left_stick_x, left_stick_y;
    bool  left_status;
    float left_error;
    float leftAngularVelocityX, leftAngularVelocityY, leftAngularVelocityZ;

    // --- RIGHT CONTROLLER ---
    float rPosX, rPosY, rPosZ;
    float right_qw, right_qx, right_qy, right_qz;
    bool  right_a, right_b, right_system, right_grip, right_trigger, right_stick_click;
    float right_stick_x, right_stick_y;
    bool  right_status;
    float right_error;
    float rightAngularVelocityX, rightAngularVelocityY, rightAngularVelocityZ;
};
#pragma pack(pop)

// Variables globales para el driver
extern AllData g_RemoteData;
extern std::mutex g_DataMutex;
extern std::atomic<bool> g_bPipeRunning;
extern std::chrono::steady_clock::time_point g_RemoteDataReceiptTime;

// Funciones de control
void StartTrackingInput();
void StopTrackingInput();
