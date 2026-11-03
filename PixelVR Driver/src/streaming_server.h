#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include <cstdint>
#include <mutex>

class StreamingServer
{
public:
    StreamingServer() = default;
    ~StreamingServer();

    bool Start(uint16_t port, bool localhostOnly);
    void Stop();

    bool HasClient();
    bool SendAll(const void* data, size_t bytes);

    uint16_t Port() const { return m_port; }

private:
    bool AcceptIfNeeded();
    void CloseClient();

    SOCKET m_listen = INVALID_SOCKET;
    SOCKET m_client = INVALID_SOCKET;
    uint16_t m_port = 0;
    bool m_wsaStarted = false;
    std::mutex m_mutex;
};
