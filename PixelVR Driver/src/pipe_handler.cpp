#include "pipe_handler.h"
#include <thread>
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
std::thread* g_pPipeThread = nullptr;

void PipeLoop() {
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
            }
            else {
                break;
            }
        }
        CloseHandle(hPipe);
    }
}

void StartPipeThread() {
    if (!g_bPipeRunning) {
        g_bPipeRunning = true;
        g_pPipeThread = new std::thread(PipeLoop);
    }
}

void StopPipeThread() {
    g_bPipeRunning = false;
    if (g_pPipeThread) {
        g_pPipeThread->join();
        delete g_pPipeThread;
        g_pPipeThread = nullptr;
    }
}