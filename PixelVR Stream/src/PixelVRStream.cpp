#include "../include/PixelVRStream.h"

#include "OpenVRCapture.h"
#include "MFEncoder.h"
#include "NetworkRTP.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace
{
    std::atomic<bool> g_running{ false };
    std::atomic<bool> g_requestKeyframe{ false };

    std::thread g_worker;
    std::mutex g_mutex;

    PixelVRStatusCallback g_statusCallback = nullptr;

    std::string g_phoneIp;
    int g_videoPort = 5000;
    int g_controlPort = 6000;
    int g_width = 1920;
    int g_height = 1080;
    int g_fps = 60;
    int g_bitrateKbps = 10000;
    int g_gopSeconds = 2;

    OpenVRCapture g_openvrCapture;
    MFEncoder g_mfEncoder;
    NetworkRTP g_network;

    void Notify(const std::string& message)
    {
        PixelVRStatusCallback callback = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            callback = g_statusCallback;
        }
        if (callback != nullptr) callback(message.c_str());
    }

    bool InitializeOpenVRIfNeeded()
    {
        #ifdef USE_OPENVR
        if (!g_openvrCapture.Init())
        {
            Notify(("OpenVR init failed: " + g_openvrCapture.LastError()));
            return false;
        }
        Notify("OpenVR initialized.");
        return true;
        #else
        Notify("OpenVR compiled out.");
        return false;
        #endif
    }

    bool InitializeEncoderAndNetwork()
    {
        // Initialize MF encoder first so we can share its device with OpenVRCapture
        if (!g_mfEncoder.Init(g_width, g_height, g_fps, g_bitrateKbps, g_gopSeconds))
        {
            Notify(("MF encoder init failed: " + g_mfEncoder.LastError()));
            return false;
        }

        // Get device from encoder (AddRef returned pointer)
        ID3D11Device* sharedDevice = g_mfEncoder.GetD3D11Device();
        if (sharedDevice)
        {
            // Initialize OpenVRCapture with shared device
            std::string openvrErr;
            if (!g_openvrCapture.Init(sharedDevice))
            {
                Notify(("OpenVR init failed: " + g_openvrCapture.LastError()));
                // Even if OpenVR failed, release device and decide whether to continue
                sharedDevice->Release();
                return false;
            }
            // we no longer own the reference from GetD3D11Device here
            sharedDevice->Release();
        }
        else
        {
            Notify("Warning: MFEncoder did not provide a D3D11 device; OpenVR capture may create its own device.");
            // try init OpenVRCapture without shared device (it will create own device)
            std::string openvrErr;
            if (!g_openvrCapture.Init(nullptr))
            {
                Notify(("OpenVR init failed: " + g_openvrCapture.LastError()));
                return false;
            }
        }

        // Initialize network (can be after encoder/capture)
        if (!g_network.Init(g_phoneIp, g_videoPort))
        {
            Notify(("Network init failed: " + g_network.LastError()));
            // not fatal for test but notify
        }

        return true;
    }

    void ShutdownAll()
    {
        try { g_network.Shutdown(); }
        catch (...) {}
        try { g_mfEncoder.Shutdown(); }
        catch (...) {}
        try { g_openvrCapture.Shutdown(); }
        catch (...) {}
    }

    bool CaptureEncodeSendOneFrame()
    {
        ID3D11Texture2D* tex = nullptr;
        std::string err;
        if (!g_openvrCapture.CaptureFrame(&tex, err))
        {
            // CaptureFrame failed; we still continue (maybe testing with pre-recorded feed)
            // Notify but do not spam
            static int counter = 0;
            if ((counter++ % 60) == 0) Notify(std::string("CaptureFrame warning: ") + err);
            return false;
        }

        if (!tex)
        {
            Notify("CaptureFrame returned no texture.");
            return false;
        }

        std::vector<std::vector<uint8_t>> nalUnits;
        int64_t tsUs = 0; // present timestamp can be computed
        if (g_mfEncoder.SubmitFrame(tex, nalUnits, tsUs))
        {
            // compute RTP timestamp increment: use 90000 / fps
            uint32_t rtpInc = static_cast<uint32_t>(90000 / (g_fps > 0 ? g_fps : 60));
            for (auto& nal : nalUnits)
            {
                // The encoder output might not be Annex-B; here we pass raw bytes to SendH264Bytestream.
                g_network.SendH264Bytestream(nal.data(), nal.size(), rtpInc);
            }
        }
        tex->Release();
        return true;
    }

    void WorkerThread()
    {
        Notify("Worker thread starting...");

        InitializeOpenVRIfNeeded();

        if (!InitializeEncoderAndNetwork())
        {
            Notify("Encoder or network init failed; stopping worker.");
            ShutdownAll();
            g_running = false;
            return;
        }

        Notify("Stream ready.");

        const auto frameDuration = std::chrono::microseconds(1'000'000 / (g_fps > 0 ? g_fps : 60));
        auto nextFrame = std::chrono::steady_clock::now();

        while (g_running)
        {
            nextFrame += frameDuration;

            if (g_requestKeyframe.exchange(false))
            {
                g_mfEncoder.RequestKeyframe();
                Notify("Keyframe requested.");
            }

            CaptureEncodeSendOneFrame();

            std::this_thread::sleep_until(nextFrame);
        }

        ShutdownAll();
        Notify("Worker stopped.");
    }
}

extern "C"
{
    bool StartStream(
        const char* phoneIp,
        int videoPort,
        int controlPort,
        int width,
        int height,
        int fps,
        int bitrateKbps,
        int gopSeconds)
    {
        if (phoneIp == nullptr || phoneIp[0] == '\0') return false;
        if (g_running) return true;

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_phoneIp = phoneIp;
            g_videoPort = videoPort;
            g_controlPort = controlPort;
            g_width = width;
            g_height = height;
            g_fps = fps;
            g_bitrateKbps = bitrateKbps;
            g_gopSeconds = gopSeconds;
        }

        g_requestKeyframe = false;
        g_running = true;

        try { g_worker = std::thread(WorkerThread); }
        catch (...)
        {
            g_running = false;
            return false;
        }

        return true;
    }

    void StopStream()
    {
        if (!g_running) return;
        g_running = false;
        if (g_worker.joinable()) g_worker.join();
    }

    bool IsStreamRunning()
    {
        return g_running;
    }

    void RequestKeyframe()
    {
        g_requestKeyframe = true;
    }

    void RegisterStatusCallback(PixelVRStatusCallback callback)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_statusCallback = callback;
    }

    bool SetStreamParameters(int bitrateKbps, int fps, int gopSeconds)
    {
        if (bitrateKbps <= 0 || fps <= 0 || gopSeconds <= 0) return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_bitrateKbps = bitrateKbps;
        g_fps = fps;
        g_gopSeconds = gopSeconds;
        return true;
    }
}