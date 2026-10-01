package com.pixelvr

import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.util.Log
import android.view.Surface

/** Feeds complete Annex-B access units from the PixelVR protocol into Android's hardware decoder. */
class VideoDecoder(private val surface: Surface) {
    private var codec: MediaCodec? = null
    private var currentCodec = -1
    private var currentWidth = 0
    private var currentHeight = 0
    private var currentFrameRate = 0
    private var nextPresentationTimeUs = 0L

    @Synchronized
    fun decodeFrame(frame: PacketParser.ParsedFrame, payload: ByteArray) {
        if (codec == null || currentCodec != frame.codec ||
            currentWidth != frame.width || currentHeight != frame.height
        ) {
            if (!initCodec(frame)) return
        }
        requestSurfaceFrameRate(frame.frameRate)

        val mediaCodec = codec ?: return
        try {
            val inputIndex = mediaCodec.dequeueInputBuffer(INPUT_TIMEOUT_US)
            if (inputIndex >= 0) {
                val input = mediaCodec.getInputBuffer(inputIndex)
                if (input == null || payload.size > input.capacity()) {
                    mediaCodec.queueInputBuffer(inputIndex, 0, 0, nextPresentationTimeUs, 0)
                    Log.e(TAG, "Encoded access unit exceeds the decoder input buffer")
                    return
                }

                input.clear()
                input.put(payload)
                val pts = maxOf(frame.timestampUs, nextPresentationTimeUs)
                mediaCodec.queueInputBuffer(inputIndex, 0, payload.size, pts, 0)
                nextPresentationTimeUs = pts + 1
            } else {
                // Do not block the stream thread; a full decoder queue means this frame is stale.
                return
            }

            val info = MediaCodec.BufferInfo()
            while (true) {
                val outputIndex = mediaCodec.dequeueOutputBuffer(info, 0)
                if (outputIndex < 0) break
                mediaCodec.releaseOutputBuffer(outputIndex, true)
            }
        } catch (e: Exception) {
            Log.e(TAG, "Video decode failed", e)
            release()
        }
    }

    private fun initCodec(frame: PacketParser.ParsedFrame): Boolean {
        release()
        val mimeType = when (frame.codec) {
            PacketParser.CODEC_H264 -> "video/avc"
            PacketParser.CODEC_HEVC -> "video/hevc"
            else -> return false
        }

        return try {
            val format = MediaFormat.createVideoFormat(mimeType, frame.width, frame.height)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            }

            codec = MediaCodec.createDecoderByType(mimeType).also {
                it.configure(format, surface, null, 0)
                it.start()
            }
            currentCodec = frame.codec
            currentWidth = frame.width
            currentHeight = frame.height
            currentFrameRate = 0
            requestSurfaceFrameRate(frame.frameRate)
            nextPresentationTimeUs = frame.timestampUs
            Log.i(TAG, "Hardware decoder ready: $mimeType ${frame.width}x${frame.height}")
            true
        } catch (e: Exception) {
            Log.e(TAG, "Could not initialize $mimeType decoder", e)
            release()
            false
        }
    }

    @Synchronized
    fun release() {
        val oldCodec = codec
        codec = null
        try {
            oldCodec?.stop()
        } catch (_: Exception) {
        }
        try {
            oldCodec?.release()
        } catch (_: Exception) {
        }
        currentCodec = -1
        currentWidth = 0
        currentHeight = 0
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && currentFrameRate > 0) {
            surface.setFrameRate(0f, Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)
        }
        currentFrameRate = 0
    }

    private fun requestSurfaceFrameRate(frameRate: Int) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R || frameRate !in 15..240 || frameRate == currentFrameRate)
            return
        try {
            surface.setFrameRate(frameRate.toFloat(), Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)
            currentFrameRate = frameRate
            Log.i(TAG, "Requested display rate for stream: ${frameRate}Hz")
        } catch (e: Exception) {
            Log.w(TAG, "Could not request display rate ${frameRate}Hz", e)
        }
    }

    companion object {
        private const val TAG = "VideoDecoder"
        private const val INPUT_TIMEOUT_US = 2_000L
    }
}
