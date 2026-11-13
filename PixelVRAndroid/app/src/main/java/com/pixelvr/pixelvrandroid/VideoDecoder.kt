package com.pixelvr

import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.os.SystemClock
import android.util.Log
import android.view.Surface

/** Feeds complete Annex-B access units from the PixelVR protocol into Android's hardware decoder. */
class VideoDecoder(private val surface: Surface) {
    private var codec: MediaCodec? = null
    private var currentCodec = -1
    private var currentWidth = 0
    private var currentHeight = 0
    private var nextPresentationTimeUs = 0L

    @Synchronized
    fun decodeFrame(frame: PacketParser.ParsedFrame, payload: ByteArray) {
        StreamFov.update(frame)
        if (codec == null || currentCodec != frame.codec ||
            currentWidth != frame.width || currentHeight != frame.height
        ) {
            if (!initCodec(frame)) return
        }

        val mediaCodec = codec ?: return
        try {
            // Preserve the encoded reference chain. Dropping one P-frame because the
            // decoder was briefly busy can corrupt the picture until the next IDR.
            val deadlineNs = SystemClock.elapsedRealtimeNanos() + INPUT_DEADLINE_NS
            var inputIndex: Int
            do {
                inputIndex = mediaCodec.dequeueInputBuffer(INPUT_TIMEOUT_US)
                if (inputIndex >= 0) break
                drainOutput(mediaCodec)
            } while (SystemClock.elapsedRealtimeNanos() < deadlineNs)
            if (inputIndex < 0) {
                throw IllegalStateException("Decoder input stalled at access unit ${frame.frameId}")
            }

            val input = mediaCodec.getInputBuffer(inputIndex)
            if (input == null || payload.size > input.capacity()) {
                mediaCodec.queueInputBuffer(inputIndex, 0, 0, nextPresentationTimeUs, 0)
                Log.e(TAG, "Encoded access unit exceeds the decoder input buffer")
                return
            }

            input.clear()
            input.put(payload)
            // The sender's steady_clock epoch is from another machine. Give MediaCodec
            // timestamps in this Android process's monotonic clock domain instead.
            val pts = maxOf(System.nanoTime() / 1_000L, nextPresentationTimeUs)
            mediaCodec.queueInputBuffer(inputIndex, 0, payload.size, pts, 0)
            nextPresentationTimeUs = pts + 1L

            drainOutput(mediaCodec)
        } catch (e: Exception) {
            Log.e(TAG, "Video decode failed", e)
            release()
            throw e
        }
    }

    private fun drainOutput(mediaCodec: MediaCodec) {
        val info = MediaCodec.BufferInfo()
        while (true) {
            val outputIndex = mediaCodec.dequeueOutputBuffer(info, 0)
            if (outputIndex < 0) return
            mediaCodec.releaseOutputBuffer(outputIndex, true)
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
            nextPresentationTimeUs = System.nanoTime() / 1_000L
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

    }

    companion object {
        private const val TAG = "VideoDecoder"
        private const val INPUT_TIMEOUT_US = 2_000L
        private const val INPUT_DEADLINE_NS = 100_000_000L
    }
}
