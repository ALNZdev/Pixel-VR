package com.pixelvr

import java.nio.ByteBuffer
import java.nio.ByteOrder

object PacketParser {
    const val HEADER_SIZE = 28
    private const val PACKET_MAGIC = 0x46525650 // 'PVRF'
    const val MAX_PAYLOAD_BYTES = 16 * 1024 * 1024

    const val CODEC_H264 = 0
    const val CODEC_HEVC = 1
    const val FLAG_KEYFRAME = 1

    data class ParsedFrame(
        val frameId: Int,
        val timestampUs: Long,
        val width: Int,
        val height: Int,
        val codec: Int,
        val flags: Int,
        val frameRate: Int,
        val payloadBytes: Int,
        val isKeyframe: Boolean
    )

    fun parseHeader(buffer: ByteArray, offset: Int): ParsedFrame? {
        if (offset < 0 || buffer.size - offset < HEADER_SIZE) return null

        val bb = ByteBuffer.wrap(buffer, offset, HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN)

        val magic = bb.int.toLong() and 0xffffffffL
        if (magic != PACKET_MAGIC.toLong()) {
            return null
        }

        val frameId = bb.int
        val timestampUs = bb.long
        val width = bb.short.toInt() and 0xffff
        val height = bb.short.toInt() and 0xffff
        val codec = bb.get().toInt() and 0xff
        val flags = bb.get().toInt() and 0xff
        val frameRate = bb.short.toInt() and 0xffff
        val payloadBytes = bb.int

        if (width == 0 || height == 0 || codec !in CODEC_H264..CODEC_HEVC) return null
        if (payloadBytes <= 0 || payloadBytes > MAX_PAYLOAD_BYTES) return null

        return ParsedFrame(
            frameId = frameId,
            timestampUs = timestampUs,
            width = width,
            height = height,
            codec = codec,
            flags = flags,
            frameRate = frameRate,
            payloadBytes = payloadBytes,
            isKeyframe = (flags and FLAG_KEYFRAME) != 0
        )
    }
}
