#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "alvr_pose_bridge.h"

#include <cstdint>
#include <mutex>

#ifdef _WIN32

namespace
{
#pragma pack(push, 1)
    struct PixelVrPosePacket
    {
        uint32_t magic; // "PVR1" on little-endian Windows
        uint8_t version;
        uint8_t tracked;
        float position[3];
        float rotationWxyz[4];
    };
#pragma pack(pop)

    constexpr uint32_t kPacketMagic = 0x31525650;
    constexpr uint16_t kAlvrPosePort = 9945;
    std::mutex g_socketMutex;
    SOCKET g_socket = INVALID_SOCKET;
    bool g_winsockStarted = false;

    bool EnsureSocket()
    {
        if (g_socket != INVALID_SOCKET)
            return true;

        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            return false;
        g_winsockStarted = true;

        g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        return g_socket != INVALID_SOCKET;
    }
}
#endif

void AlvrPoseBridge::Send(const vr::DriverPose_t& pose)
{
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(g_socketMutex);
    if (!EnsureSocket())
        return;

    PixelVrPosePacket packet{};
    packet.magic = kPacketMagic;
    packet.version = 1;
    packet.tracked = pose.poseIsValid ? 1 : 0;
    for (int i = 0; i < 3; ++i)
        packet.position[i] = static_cast<float>(pose.vecPosition[i]);
    packet.rotationWxyz[0] = static_cast<float>(pose.qRotation.w);
    packet.rotationWxyz[1] = static_cast<float>(pose.qRotation.x);
    packet.rotationWxyz[2] = static_cast<float>(pose.qRotation.y);
    packet.rotationWxyz[3] = static_cast<float>(pose.qRotation.z);

    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(kAlvrPosePort);
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(
        g_socket,
        reinterpret_cast<const char*>(&packet),
        static_cast<int>(sizeof(packet)),
        0,
        reinterpret_cast<const sockaddr*>(&destination),
        sizeof(destination));
#else
    (void)pose;
#endif
}

void AlvrPoseBridge::Shutdown()
{
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(g_socketMutex);
    if (g_socket != INVALID_SOCKET)
    {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
    }
    if (g_winsockStarted)
    {
        WSACleanup();
        g_winsockStarted = false;
    }
#endif
}
