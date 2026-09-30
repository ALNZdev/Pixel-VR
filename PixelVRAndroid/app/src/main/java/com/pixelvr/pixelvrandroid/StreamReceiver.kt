package com.pixelvr

import android.util.Log
import java.io.InputStream
import java.net.Socket

class StreamReceiver(private val decoder: VideoDecoder) {
    private var socket: Socket? = null
    private var running = true

    fun start() {
        try {
            Log.d(TAG, "Connecting to 127.0.0.1:9944")

            socket = Socket("127.0.0.1", 9944)
            socket?.tcpNoDelay = true

            val input = socket?.getInputStream() ?: return

            while (running) {
                val header = ByteArray(28)
                if (!readFully(input, header, 28)) {
                    Log.w(TAG, "Connection closed by server")
                    break
                }

                val frame = PacketParser.parseHeader(header, 0)
                if (frame == null) {
                    Log.w(TAG, "Invalid header, skipping packet")
                    continue
                }

                val payload = ByteArray(frame.payloadBytes)
                if (!readFully(input, payload, frame.payloadBytes)) {
                    Log.w(TAG, "Incomplete payload")
                    break
                }

                Log.d(TAG, "Frame ${frame.frameId} ${frame.width}x${frame.height} codec=${frame.codec} key=${frame.isKeyframe}")
                decoder.decodeFrame(payload, frame.codec)
            }
        } catch (e: Exception) {
            Log.e(TAG, "StreamReceiver error: ${e.message}", e)
        } finally {
            running = false
            try {
                socket?.close()
            } catch (_: Exception) {
            }
            Log.d(TAG, "StreamReceiver stopped")
        }
    }

    fun stop() {
        running = false
        try {
            socket?.close()
        } catch (_: Exception) {
        }
    }

    private fun readFully(input: InputStream, buffer: ByteArray, length: Int): Boolean {
        var total = 0
        while (total < length) {
            val read = input.read(buffer, total, length - total)
            if (read == -1) {
                return false
            }
            total += read
        }
        return true
    }

    companion object {
        private const val TAG = "StreamReceiver"
    }
}