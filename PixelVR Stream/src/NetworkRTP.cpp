#include "NetworkRTP.h"
#include <cstring>
#include <vector>
#include <chrono>
#include <random>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

NetworkRTP::NetworkRTP()
{
    std::random_device rd;
    m_seq = static_cast<uint16_t>(rd() & 0xffff);
    m_ssrc = static_cast<uint32_t>(rd());
}

NetworkRTP::~NetworkRTP()
{
    Shutdown();
}

bool NetworkRTP::Init(const std::string& destIp, int destPort)
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        m_lastError = "WSAStartup failed";
        return false;
    }
#endif

#ifdef _WIN32
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET)
    {
        m_lastError = "socket() failed";
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }
    m_sock = static_cast<intptr_t>(sock);
#else
    int sock = static_cast<int>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (sock < 0)
    {
        m_lastError = "socket() failed";
        return false;
    }
    m_sock = static_cast<intptr_t>(sock);
#endif

    m_destIp = destIp;
    m_destPort = static_cast<uint16_t>(destPort);

    // set MTU conservative
    m_mtu = 1200;
    m_rtpTimestamp = 0;

    return true;
}

void NetworkRTP::Shutdown()
{
#ifdef _WIN32
    if (m_sock != -1)
    {
        SOCKET s = static_cast<SOCKET>(m_sock);
        closesocket(s);
        m_sock = -1;
    }
    WSACleanup();
#else
    if (m_sock != -1)
    {
        close(static_cast<int>(m_sock));
        m_sock = -1;
    }
#endif
}

bool NetworkRTP::SendData(const uint8_t* data, size_t size)
{
    if (m_sock == -1) return false;

    // Construir sockaddr_in local
    sockaddr_in destAddr{};
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(m_destPort);
#ifdef _WIN32
    if (InetPtonA(AF_INET, m_destIp.c_str(), &destAddr.sin_addr) != 1)
    {
        m_lastError = "InetPtonA failed for dest IP";
        return false;
    }
#else
    if (inet_pton(AF_INET, m_destIp.c_str(), &destAddr.sin_addr) != 1)
    {
        m_lastError = "inet_pton failed for dest IP";
        return false;
    }
#endif

#ifdef _WIN32
    SOCKET s = static_cast<SOCKET>(m_sock);
    int sent = sendto(s, reinterpret_cast<const char*>(data), static_cast<int>(size), 0,
        reinterpret_cast<sockaddr*>(&destAddr), static_cast<int>(sizeof(destAddr)));
#else
    int sent = sendto(static_cast<int>(m_sock), reinterpret_cast<const char*>(data), static_cast<int>(size), 0,
        reinterpret_cast<sockaddr*>(&destAddr), static_cast<int>(sizeof(destAddr)));
#endif

    return sent == static_cast<int>(size);
}

bool NetworkRTP::SendRtpPacket(const uint8_t* payload, size_t payloadSize, bool marker)
{
    if (m_sock == -1) return false;

    uint8_t buf[1500];
    if (payloadSize + 12 > sizeof(buf))
    {
        m_lastError = "Payload too large for single RTP packet";
        return false;
    }

    buf[0] = 0x80;
    buf[1] = static_cast<uint8_t>((marker ? 0x80 : 0x00) | (96 & 0x7f));
    uint16_t seq_net = htons(m_seq++);
    memcpy(buf + 2, &seq_net, 2);
    uint32_t ts_net = htonl(m_rtpTimestamp);
    memcpy(buf + 4, &ts_net, 4);
    uint32_t ssrc_net = htonl(m_ssrc);
    memcpy(buf + 8, &ssrc_net, 4);

    memcpy(buf + 12, payload, payloadSize);

    // prepare dest
    sockaddr_in destAddr{};
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(m_destPort);
#ifdef _WIN32
    if (InetPtonA(AF_INET, m_destIp.c_str(), &destAddr.sin_addr) != 1)
    {
        m_lastError = "InetPtonA failed for dest IP";
        return false;
    }
#else
    if (inet_pton(AF_INET, m_destIp.c_str(), &destAddr.sin_addr) != 1)
    {
        m_lastError = "inet_pton failed for dest IP";
        return false;
    }
#endif

#ifdef _WIN32
    SOCKET s = static_cast<SOCKET>(m_sock);
    int sent = sendto(s, reinterpret_cast<const char*>(buf), 12 + static_cast<int>(payloadSize), 0,
        reinterpret_cast<sockaddr*>(&destAddr), static_cast<int>(sizeof(destAddr)));
#else
    int sent = sendto(static_cast<int>(m_sock), reinterpret_cast<const char*>(buf), 12 + static_cast<int>(payloadSize), 0,
        reinterpret_cast<sockaddr*>(&destAddr), static_cast<int>(sizeof(destAddr)));
#endif

    return sent == (12 + static_cast<int>(payloadSize));
}

bool NetworkRTP::PacketizeAndSendNAL(const uint8_t* nalData, size_t nalSize, bool isLastInFrame)
{
    if (nalSize == 0) return true;

    uint8_t nalHeader = nalData[0];
    uint8_t nalType = nalHeader & 0x1F;
    const size_t maxPayload = m_mtu - 12 - 2;
    if (nalSize <= maxPayload)
    {
        return SendRtpPacket(nalData, nalSize, isLastInFrame);
    }
    else
    {
        size_t offset = 1;
        size_t remaining = nalSize - 1;
        bool first = true;
        while (remaining > 0)
        {
            size_t chunk = (remaining > maxPayload) ? maxPayload : remaining;
            uint8_t fuIndicator = (nalHeader & 0xE0) | 28;
            uint8_t fuHeader = 0;
            if (first) fuHeader |= 0x80;
            if (chunk == remaining) fuHeader |= 0x40;
            fuHeader |= nalType & 0x1F;

            std::vector<uint8_t> pkt;
            pkt.reserve(2 + chunk);
            pkt.push_back(fuIndicator);
            pkt.push_back(fuHeader);
            pkt.insert(pkt.end(), nalData + offset, nalData + offset + chunk);

            bool marker = (chunk == remaining) && isLastInFrame;
            if (!SendRtpPacket(pkt.data(), pkt.size(), marker))
                return false;

            offset += chunk;
            remaining -= chunk;
            first = false;
        }
    }
    return true;
}

bool NetworkRTP::SendH264Bytestream(const uint8_t* data, size_t size, uint32_t rtpTimestampIncrement)
{
    if (!data || size == 0) return false;

    m_rtpTimestamp += rtpTimestampIncrement;

    size_t i = 0;
    auto isStartAt = [&](size_t pos)->int {
        if (pos + 3 <= size && data[pos] == 0x00 && data[pos + 1] == 0x00 && data[pos + 2] == 0x01) return 3;
        if (pos + 4 <= size && data[pos] == 0x00 && data[pos + 1] == 0x00 && data[pos + 2] == 0x00 && data[pos + 3] == 0x01) return 4;
        return 0;
        };

    size_t pos = 0;
    while (pos < size && data[pos] == 0x00) pos++;
    size_t nalStart = SIZE_MAX;
    for (i = 0; i + 3 < size; ++i)
    {
        int prefixLen = isStartAt(i);
        if (prefixLen)
        {
            nalStart = i + prefixLen;
            break;
        }
    }
    if (nalStart == SIZE_MAX) return false;

    size_t cur = nalStart;
    std::vector<std::pair<const uint8_t*, size_t>> nals;

    while (cur < size)
    {
        size_t next = SIZE_MAX;
        for (i = cur; i + 3 < size; ++i)
        {
            int prefixLen = isStartAt(i);
            if (prefixLen)
            {
                next = i;
                break;
            }
        }
        if (next == SIZE_MAX)
        {
            nals.emplace_back(data + cur, size - cur);
            break;
        }
        else
        {
            nals.emplace_back(data + cur, next - cur);
            int prefix = isStartAt(next);
            cur = next + prefix;
        }
    }

    for (size_t idx = 0; idx < nals.size(); ++idx)
    {
        const uint8_t* ptr = nals[idx].first;
        size_t len = nals[idx].second;
        bool isLast = (idx + 1 == nals.size());
        if (!PacketizeAndSendNAL(ptr, len, isLast)) return false;
    }

    return true;
}