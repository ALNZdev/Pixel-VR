#include "streaming_server.h"
#include "driverlog.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

StreamingServer::~StreamingServer()
{
    Stop();
}

bool StreamingServer::Start(uint16_t port, bool localhostOnly)
{
    Stop();

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        DriverLog("[StreamServer] WSAStartup falló");
        return false;
    }
    m_wsaStarted = true;

    m_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listen == INVALID_SOCKET)
    {
        DriverLog("[StreamServer] socket() falló");
        Stop();
        return false;
    }

    BOOL reuse = TRUE;
    setsockopt(m_listen, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    u_long nonBlocking = 1;
    ioctlsocket(m_listen, FIONBIO, &nonBlocking);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = localhostOnly ? htonl(INADDR_LOOPBACK) : htonl(INADDR_ANY);

    if (bind(m_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        DriverLog("[StreamServer] bind(%u) falló (%d)", port, WSAGetLastError());
        Stop();
        return false;
    }

    if (listen(m_listen, 1) != 0)
    {
        DriverLog("[StreamServer] listen() falló");
        Stop();
        return false;
    }

    m_port = port;
    DriverLog(
        "[StreamServer] Escuchando TCP %s:%u (ADB reverse -> este puerto)",
        localhostOnly ? "127.0.0.1" : "0.0.0.0",
        port);
    return true;
}

void StreamingServer::CloseClient()
{
    if (m_client != INVALID_SOCKET)
    {
        shutdown(m_client, SD_BOTH);
        closesocket(m_client);
        m_client = INVALID_SOCKET;
    }
}

void StreamingServer::Stop()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    CloseClient();

    if (m_listen != INVALID_SOCKET)
    {
        closesocket(m_listen);
        m_listen = INVALID_SOCKET;
    }

    m_port = 0;

    if (m_wsaStarted)
    {
        WSACleanup();
        m_wsaStarted = false;
    }
}

bool StreamingServer::AcceptIfNeeded()
{
    if (m_listen == INVALID_SOCKET)
        return false;

    if (m_client != INVALID_SOCKET)
        return true;

    sockaddr_in remote{};
    int remoteLen = sizeof(remote);
    SOCKET accepted = accept(m_listen, reinterpret_cast<sockaddr*>(&remote), &remoteLen);
    if (accepted == INVALID_SOCKET)
        return false;

    // Accepted sockets inherit the listener's non-blocking mode on Windows; frames are large,
    // so the data socket must block (bounded by SO_SNDTIMEO) or send() fails with WSAEWOULDBLOCK.
    u_long blocking = 0;
    ioctlsocket(accepted, FIONBIO, &blocking);
    DWORD sendTimeoutMs = 2000;
    setsockopt(accepted, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sendTimeoutMs), sizeof(sendTimeoutMs));

    BOOL nodelay = TRUE;
    setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

    int sendBuf = 512 * 1024; // small on purpose: a deep buffer is added latency
    setsockopt(accepted, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sendBuf), sizeof(sendBuf));

    m_client = accepted;
    DriverLog("[StreamServer] Cliente conectado");
    return true;
}

bool StreamingServer::HasClient()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return AcceptIfNeeded();
}

bool StreamingServer::SendAll(const void* data, size_t bytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!AcceptIfNeeded() || data == nullptr)
        return false;

    const char* ptr = static_cast<const char*>(data);
    size_t remaining = bytes;
    while (remaining > 0)
    {
        const int sent = send(m_client, ptr, static_cast<int>(remaining), 0);
        if (sent <= 0)
        {
            DriverLog("[StreamServer] Cliente desconectado (%d)", WSAGetLastError());
            CloseClient();
            return false;
        }

        ptr += sent;
        remaining -= static_cast<size_t>(sent);
    }

    return true;
}
