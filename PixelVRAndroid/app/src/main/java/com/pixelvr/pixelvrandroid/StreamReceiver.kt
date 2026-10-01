package com.pixelvr

import android.util.Log
import java.io.InputStream
import java.net.InetSocketAddress
import java.net.Socket

/** Android is the TCP client; `adb reverse tcp:<port> tcp:<port>` maps this loopback port to the PC. */
class StreamReceiver(
    private val decoder: VideoDecoder,
    private val port: Int = DEFAULT_PORT
) : Runnable {
    @Volatile private var running = true
    @Volatile private var socket: Socket? = null

    override fun run() {
        try {
            while (running) {
                try {
                    receiveConnection()
                } catch (e: Exception) {
                    if (running) Log.w(TAG, "Stream connection ended; retrying", e)
                } finally {
                    try { socket?.close() } catch (_: Exception) { }
                    socket = null
                }

                if (running) {
                    try {
                        Thread.sleep(RECONNECT_DELAY_MS)
                    } catch (_: InterruptedException) {
                        Thread.currentThread().interrupt()
                        break
                    }
                }
            }
        } finally {
            decoder.release()
            Log.i(TAG, "Receiver stopped")
        }
    }

    private fun receiveConnection() {
        val connection = Socket()
        socket = connection
        connection.tcpNoDelay = true
        connection.connect(InetSocketAddress("127.0.0.1", port), CONNECT_TIMEOUT_MS)
        Log.i(TAG, "Connected to 127.0.0.1:$port")

        val input = connection.getInputStream()
        while (running) {
            val headerBytes = readNextHeader(input) ?: break
            val frame = PacketParser.parseHeader(headerBytes, 0)
            if (frame == null) {
                // readNextHeader only returns a header with the protocol magic and valid bounds.
                throw IllegalStateException("Invalid PixelVR packet header")
            }

            val payload = ByteArray(frame.payloadBytes)
            if (!readFully(input, payload, payload.size)) break
            decoder.decodeFrame(frame, payload)
        }
    }

    /** Reconstructs the fixed 28-byte header despite arbitrary TCP read boundaries. */
    private fun readNextHeader(input: InputStream): ByteArray? {
        val header = ByteArray(PacketParser.HEADER_SIZE)
        if (!readFully(input, header, header.size)) return null

        while (running && PacketParser.parseHeader(header, 0) == null) {
            System.arraycopy(header, 1, header, 0, header.size - 1)
            val next = input.read()
            if (next < 0) return null
            header[header.lastIndex] = next.toByte()
        }
        return if (running) header else null
    }

    fun stop() {
        running = false
        try { socket?.close() } catch (_: Exception) { }
    }

    private fun readFully(input: InputStream, buffer: ByteArray, length: Int): Boolean {
        var total = 0
        while (running && total < length) {
            val count = input.read(buffer, total, length - total)
            if (count < 0) return false
            total += count
        }
        return total == length
    }

    companion object {
        private const val TAG = "StreamReceiver"
        private const val DEFAULT_PORT = 9944
        private const val CONNECT_TIMEOUT_MS = 1_500
        private const val RECONNECT_DELAY_MS = 500L
    }
}
