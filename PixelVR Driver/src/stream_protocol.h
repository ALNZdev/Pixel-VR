#pragma once

#include <cstdint>

// PixelVR phone video protocol v2 (video only: no IMU, no sensors).
//
// Transport : TCP. The PC driver is the SERVER, the Android app is the CLIENT and
//             connects to 127.0.0.1:<port> after `adb reverse tcp:<port> tcp:<port>`.
// Byte order: little-endian.
// Framing   : TCP is a byte stream, so every frame is a fixed-size header followed by
//             exactly `payloadBytes` bytes. Each header starts with a magic so the
//             receiver can detect desynchronisation. There is no separate handshake:
//             every packet is self-describing (width/height/codec).
// Payload   : H.264 or HEVC Annex-B (00 00 00 01 NAL units). Keyframes carry SPS/PPS(/VPS).
//             The first packet after a connection is always a keyframe.

namespace pixelvr
{
    constexpr uint32_t kPacketMagic = 0x46525650; // bytes 'P','V','R','F'
    constexpr uint32_t kMaxPayloadBytes = 16u * 1024u * 1024u;

    constexpr uint8_t kCodecH264 = 0;
    constexpr uint8_t kCodecHevc = 1;

    constexpr uint8_t kPacketFlagKeyframe = 1u << 0;

#pragma pack(push, 1)
    struct StreamPacketHeader
    {
        uint32_t magic;          // kPacketMagic
        uint32_t frameId;        // monotonically increasing per connection, starts at 0
        uint64_t timestampUs;    // driver steady_clock (microseconds) when the frame was submitted to the encoder
        uint16_t width;          // full SBS frame width  (both eyes)
        uint16_t height;         // full SBS frame height
        uint8_t  codec;          // kCodecH264 / kCodecHevc
        uint8_t  flags;          // kPacketFlagKeyframe
        uint16_t framerate;      // desired presentation rate (0 = unspecified)
        uint32_t payloadBytes;   // bytes that follow this header
    };
#pragma pack(pop)

    static_assert(sizeof(StreamPacketHeader) == 28, "packet header must be 28 bytes");
}
