package com.pixelvr.pixelvrandroid

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer

class VideoDecoder(private val surface: Surface) {
    private var codec: MediaCodec? = null
    private var width = 0
    private var height = 0

    fun decodeFrame(buffer: ByteArray, offset: Int, length: Int, codecType: Int) {
        try {
            // Inicializar codec si es necesario
            if (codec == null) {
                initCodec(1920, 1080, codecType)
            }

            val inputBufferIndex = codec!!.dequeueInputBuffer(10000)
            if (inputBufferIndex >= 0) {
                val inputBuffer = codec!!.getInputBuffer(inputBufferIndex)
                inputBuffer!!.clear()
                inputBuffer.put(buffer, offset, length)
                codec!!.queueInputBuffer(inputBufferIndex, 0, length, 0, 0)
            }

            // Decodificar
            val outputBufferIndex = codec!!.dequeueOutputBuffer(MediaCodec.BufferInfo(), 0)
            if (outputBufferIndex >= 0) {
                codec!!.releaseOutputBuffer(outputBufferIndex, true)
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error decodificando frame: ${e.message}", e)
        }
    }

    private fun initCodec(width: Int, height: Int, codecType: Int) {
        release()

        this.width = width
        this.height = height

        val mimeType = if (codecType == PacketParser.CODEC_HEVC) {
            "video/hevc"
        } else {
            "video/avc"
        }

        Log.d(TAG, "Inicializando MediaCodec: $mimeType ${width}x${height}")

        val format = MediaFormat.createVideoFormat(mimeType, width, height)
        format.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)

        codec = MediaCodec.createDecoderByType(mimeType)
        codec!!.configure(format, surface, null, 0)
        codec!!.start()
    }

    fun release() {
        try {
            codec?.stop()
            codec?.release()
        } catch (e: Exception) {
            Log.e(TAG, "Error liberando codec: ${e.message}")
        }
        codec = null
    }

    companion object {
        private const val TAG = "VideoDecoder"
    }
}