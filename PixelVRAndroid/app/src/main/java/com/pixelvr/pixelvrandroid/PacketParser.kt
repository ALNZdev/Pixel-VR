package com.pixelvr.pixelvrandroid

import java.nio.ByteBuffer
import java.nio.ByteOrder

object PacketParser {
    private const val PACKET_MAGIC = 0x46525650U  // 'P','V','R','F'
    const val CODEC_H264 = 0
    const val CODEC_HEVC = 1
    const val FLAG_KEYFRAME = 1

    data class StreamPacket(
        val magic: UInt,
        val frameId: UInt,
        val timestampUs: ULong,
        val width: UShort,
        val height: UShort,
        val codec: UByte,
        val flags: UByte,
        val payloadBytes: UInt,
        val isKeyframe: Boolean
    )

    fun parseHeader(buffer: ByteArray, offset: Int): StreamPacket? {
        if (buffer.size < offset + 28) {
            return null
        }

        val bb = ByteBuffer.wrap(buffer, offset, 28).apply {
            order(ByteOrder.LITTLE_ENDIAN)
        }

        val magic = bb.int.toUInt()
        if (magic != PACKET_MAGIC) {
            return null
        }

        return StreamPacket(
            magic = magic,
            frameId = bb.int.toUInt(),
            timestampUs = bb.long.toULong(),
            width = bb.short.toUShort(),
            height = bb.short.toUShort(),
            codec = bb.get().toUByte(),
            flags = bb.get().toUByte(),
            payloadBytes = bb.int.toUInt(),
            isKeyframe = (bb.get(-2).toInt() and FLAG_KEYFRAME) != 0
        )
    }
}