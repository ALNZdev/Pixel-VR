package com.pixelvr

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.util.Log
import android.view.Surface

class VideoDecoder(private val surface: Surface) {
    private var codec: MediaCodec? = null
    private var currentCodec = -1

    fun decodeFrame(payload: ByteArray, codecType: Int) {
        if (codec == null || currentCodec != codecType) {
            initCodec(codecType)
        }

        val mediaCodec = codec ?: return

        val inIndex = mediaCodec.dequeueInputBuffer(10000)
        if (inIndex >= 0) {
            val inputBuffer = mediaCodec.getInputBuffer(inIndex)
            if (inputBuffer != null) {
                inputBuffer.clear()
                inputBuffer.put(payload)
                mediaCodec.queueInputBuffer(inIndex, 0, payload.size, 0, 0)
            }
        }

        val info = MediaCodec.BufferInfo()
        val outIndex = mediaCodec.dequeueOutputBuffer(info, 0)
        if (outIndex >= 0) {
            mediaCodec.releaseOutputBuffer(outIndex, true)
        }
    }

    private fun initCodec(codecType: Int) {
        release()

        currentCodec = codecType

        val mimeType = if (codecType == PacketParser.CODEC_HEVC) {
            "video/hevc"
        } else {
            "video/avc"
        }

        Log.d(TAG, "Initializing decoder $mimeType")

        try {
            val format = MediaFormat.createVideoFormat(mimeType, 1920, 1080)
            format.setInteger(
                MediaFormat.KEY_COLOR_FORMAT,
                MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface
            )

            codec = MediaCodec.createDecoderByType(mimeType)
            codec?.configure(format, surface, null, 0)
            codec?.start()
        } catch (e: Exception) {
            Log.e(TAG, "Failed to init codec: ${e.message}", e)
        }
    }

    fun release() {
        try {
            codec?.stop()
            codec?.release()
        } catch (_: Exception) {
        }
        codec = null
        currentCodec = -1
    }

    companion object {
        private const val TAG = "VideoDecoder"
    }
}