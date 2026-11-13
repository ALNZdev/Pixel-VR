#include "pipe_handler.h"
#include "config_manager.h"
#include <thread>
#include <string>
#include <cstring>
#include "driverlog.h"

// Inicializa en "identidad": cuaterniones a (1,0,0,0), todo lo demás en cero/false.
static AllData MakeDefaultData() {
    AllData d{};
    d.hmd_qw = 1.0f;
    d.left_qw = 1.0f;
    d.right_qw = 1.0f;
    return d;
}

AllData g_RemoteData = MakeDefaultData();

std::mutex g_DataMutex;
std::atomic<bool> g_bPipeRunning = false;
std::chrono::steady_clock::time_point g_RemoteDataReceiptTime{};
static std::thread* g_pInputThread = nullptr;

#pragma pack(push, 1)
struct ReceiverPacket {
    float hmd_qw, hmd_qx, hmd_qy, hmd_qz, hmd_error;
    float hmd_gyroX, hmd_gyroY, hmd_gyroZ;
    float left_qw, left_qx, left_qy, left_qz;
    bool left_a, left_b, left_system, left_grip, left_trigger, left_stick_click;
    float left_stick_x, left_stick_y;
    bool left_status;
    float left_error;
    float left_gyroX, left_gyroY, left_gyroZ;
    float right_qw, right_qx, right_qy, right_qz;
    bool right_a, right_b, right_system, right_grip, right_trigger, right_stick_click;
    float right_stick_x, right_stick_y;
    bool right_status;
    float right_error;
    float right_gyroX, right_gyroY, right_gyroZ;
};
#pragma pack(pop)

static void ApplyReceiverPacket(const ReceiverPacket& packet) {
    std::lock_guard<std::mutex> lock(g_DataMutex);
    g_RemoteData.hmdPosX = 0.0f;
    g_RemoteData.hmdPosY = 1.70f;
    g_RemoteData.hmdPosZ = 0.0f;
    g_RemoteData.hmd_qw = packet.hmd_qw;
    g_RemoteData.hmd_qx = packet.hmd_qx;
    g_RemoteData.hmd_qy = packet.hmd_qy;
    g_RemoteData.hmd_qz = packet.hmd_qz;
    g_RemoteData.hmd_error = packet.hmd_error;
    g_RemoteData.hmdAngularVelocityX = packet.hmd_gyroX;
    g_RemoteData.hmdAngularVelocityY = packet.hmd_gyroY;
    g_RemoteData.hmdAngularVelocityZ = packet.hmd_gyroZ;
    g_RemoteData.lPosX = -0.2f; g_RemoteData.lPosY = 1.2f; g_RemoteData.lPosZ = -0.4f;
    g_RemoteData.left_qw = packet.left_qw; g_RemoteData.left_qx = packet.left_qx;
    g_RemoteData.left_qy = packet.left_qy; g_RemoteData.left_qz = packet.left_qz;
    g_RemoteData.left_a = packet.left_a; g_RemoteData.left_b = packet.left_b;
    g_RemoteData.left_system = packet.left_system; g_RemoteData.left_grip = packet.left_grip;
    g_RemoteData.left_trigger = packet.left_trigger; g_RemoteData.left_stick_click = packet.left_stick_click;
    g_RemoteData.left_stick_x = packet.left_stick_x; g_RemoteData.left_stick_y = packet.left_stick_y;
    g_RemoteData.left_status = packet.left_status; g_RemoteData.left_error = packet.left_error;
    g_RemoteData.leftAngularVelocityX = packet.left_gyroX;
    g_RemoteData.leftAngularVelocityY = packet.left_gyroY;
    g_RemoteData.leftAngularVelocityZ = packet.left_gyroZ;
    g_RemoteData.rPosX = 0.2f; g_RemoteData.rPosY = 1.2f; g_RemoteData.rPosZ = -0.4f;
    g_RemoteData.right_qw = packet.right_qw; g_RemoteData.right_qx = packet.right_qx;
    g_RemoteData.right_qy = packet.right_qy; g_RemoteData.right_qz = packet.right_qz;
    g_RemoteData.right_a = packet.right_a; g_RemoteData.right_b = packet.right_b;
    g_RemoteData.right_system = packet.right_system; g_RemoteData.right_grip = packet.right_grip;
    g_RemoteData.right_trigger = packet.right_trigger; g_RemoteData.right_stick_click = packet.right_stick_click;
    g_RemoteData.right_stick_x = packet.right_stick_x; g_RemoteData.right_stick_y = packet.right_stick_y;
    g_RemoteData.right_status = packet.right_status; g_RemoteData.right_error = packet.right_error;
    g_RemoteData.rightAngularVelocityX = packet.right_gyroX;
    g_RemoteData.rightAngularVelocityY = packet.right_gyroY;
    g_RemoteData.rightAngularVelocityZ = packet.right_gyroZ;
    g_RemoteDataReceiptTime = std::chrono::steady_clock::now();
}

static void PipeLoop() {
    const char* pipeName = "\\\\.\\pipe\\PixelVR_Pipe";
    while (g_bPipeRunning) {
        HANDLE hPipe = CreateFileA(pipeName, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (hPipe == INVALID_HANDLE_VALUE) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        AllData packet;
        DWORD bytesRead;
        while (g_bPipeRunning) {
            if (ReadFile(hPipe, &packet, sizeof(AllData), &bytesRead, NULL) && bytesRead == sizeof(AllData)) {
                std::lock_guard<std::mutex> lock(g_DataMutex);
                g_RemoteData = packet;
                g_RemoteDataReceiptTime = std::chrono::steady_clock::now();
            }
            else {
                break;
            }
        }
        CloseHandle(hPipe);
    }
}

static void SerialLoop() {
    const std::string configuredPort = ConfigManager::LoadComPort();
    if (configuredPort.empty())
        DriverLog("[Serial] No hay comPort configurado en default.vrsettings");

    while (g_bPipeRunning) {
        if (configuredPort.empty()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        std::string devicePath = configuredPort.rfind("\\\\.\\", 0) == 0
            ? configuredPort : "\\\\.\\" + configuredPort;
        HANDLE port = CreateFileA(devicePath.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (port == INVALID_HANDLE_VALUE) {
            DriverLog("[Serial] No se pudo abrir %s (error %lu); reintentando", configuredPort.c_str(), GetLastError());
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(port, &dcb)) {
            CloseHandle(port);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        dcb.BaudRate = 460800; dcb.ByteSize = 8; dcb.Parity = NOPARITY; dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE; dcb.fDtrControl = DTR_CONTROL_DISABLE; dcb.fRtsControl = RTS_CONTROL_DISABLE;
        COMMTIMEOUTS timeouts{};
        timeouts.ReadTotalTimeoutConstant = 100;
        SetCommState(port, &dcb);
        SetCommTimeouts(port, &timeouts);
        DriverLog("[Serial] Receptor conectado en %s a 460800 baudios", configuredPort.c_str());

        ReceiverPacket packet{};
        uint8_t frame[sizeof(ReceiverPacket) + 2]{};
        size_t payloadIndex = 0;
        int syncState = 0;
        while (g_bPipeRunning) {
            uint8_t byte = 0; DWORD received = 0;
            if (!ReadFile(port, &byte, 1, &received, nullptr) || received == 0)
                continue;
            if (syncState != 2) {
                if (syncState == 0) syncState = byte == 0xAA ? 1 : 0;
                else if (byte == 0x55) { syncState = 2; payloadIndex = 0; }
                else syncState = byte == 0xAA ? 1 : 0;
            } else {
                frame[payloadIndex++] = byte;
            }
            if (payloadIndex == sizeof(frame)) {
                if (frame[sizeof(ReceiverPacket)] == 0xFF && frame[sizeof(ReceiverPacket) + 1] == 0xFE) {
                    std::memcpy(&packet, frame, sizeof(packet));
                    ApplyReceiverPacket(packet);
                }
                payloadIndex = 0;
                syncState = 0;
            }
        }
        CloseHandle(port);
    }
}

void StartTrackingInput() {
    if (g_bPipeRunning) return;
    g_bPipeRunning = true;
    g_pInputThread = new std::thread(ConfigManager::LoadDriverMode() == "driverTest" ? PipeLoop : SerialLoop);
}

void StopTrackingInput() {
    g_bPipeRunning = false;
    if (g_pInputThread) {
        g_pInputThread->join();
        delete g_pInputThread;
        g_pInputThread = nullptr;
    }
}
