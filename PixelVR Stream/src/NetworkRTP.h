#pragma once
#include <string>
#include <cstdint>
#include <vector>

class NetworkRTP
{
public:
    NetworkRTP();
    ~NetworkRTP();

    // Inicializa el socket UDP al destIp:destPort
    // Guarda la IP/puerto internamente; la estructura sockaddr se construye en el .cpp
    bool Init(const std::string& destIp, int destPort);

    void Shutdown();

    // Envía un bytestream H.264 (Annex-B o similar). Hace parse de NALs y packetiza RTP/FU-A.
    bool SendH264Bytestream(const uint8_t* data, size_t size, uint32_t rtpTimestampIncrement);

    // Envío simple (un solo UDP) - no fragmenta NALs grandes.
    bool SendData(const uint8_t* data, size_t size);

    std::string LastError() const { return m_lastError; }

private:
    bool SendRtpPacket(const uint8_t* payload, size_t payloadSize, bool marker);
    bool PacketizeAndSendNAL(const uint8_t* nalData, size_t nalSize, bool isLastInFrame);

private:
    std::string m_lastError;

    // Tipo de socket opaco (intptr_t funciona tanto en Windows como POSIX)
    intptr_t m_sock = -1;

    // Guardamos destino como IP y puerto para crear sockaddr_in en tiempo de envío
    std::string m_destIp;
    uint16_t m_destPort = 0;

    uint16_t m_seq = 0;
    uint32_t m_ssrc = 0x12345678;
    uint32_t m_rtpTimestamp = 0;
    unsigned m_mtu = 1200;
};