package com.pixelvr.pixelvrandroid

import android.util.Log
import java.io.InputStream
import java.net.Socket

class StreamReceiver(private val videoDecoder: VideoDecoder) {
    private var socket: Socket? = null
    private var running = true

    fun start() {
        try {
            Log.d(TAG, "Conectando a 127.0.0.1:9944...")
            socket = Socket("127.0.0.1", 9944)
            socket!!.noDelay = true  // TCP_NODELAY

            val inputStream = socket!!.getInputStream()
            val buffer = ByteArray(28 + (16 * 1024 * 1024))  // header + max payload

            while (running) {
                // Leer header (28 bytes)
                if (!readFully(inputStream, buffer, 0, 28)) {
                    Log.w(TAG, "Conexión cerrada por servidor")
                    break
                }

                // Parsear header
                val packet = PacketParser.parseHeader(buffer, 0)
                if (packet == null) {
                    Log.e(TAG, "Header inválido, resincronizando...")
                    // TODO: resync (buscar magic en el stream)
                    continue
                }

                Log.d(TAG, "Frame ${packet.frameId}: ${packet.width}x${packet.height}, " +
                        "payload ${packet.payloadBytes} bytes, keyframe=${packet.isKeyframe}")

                // Leer payload
                if (!readFully(inputStream, buffer, 28, packet.payloadBytes)) {
                    Log.w(TAG, "Payload incompleto, reconectando...")
                    break
                }

                // Decodificar
                videoDecoder.decodeFrame(buffer, 28, packet.payloadBytes, packet.codec)
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error en StreamReceiver: ${e.message}", e)
        } finally {
            socket?.close()
            Log.d(TAG, "StreamReceiver detenido")
        }
    }

    fun stop() {
        running = false
        socket?.close()
    }

    private fun readFully(inputStream: InputStream, buffer: ByteArray, offset: Int, length: Int): Boolean {
        var bytesRead = 0
        while (bytesRead < length) {
            val n = inputStream.read(buffer, offset + bytesRead, length - bytesRead)
            if (n == -1) {
                Log.w(TAG, "EOF alcanzado")
                return false
            }
            bytesRead += n
        }
        return true
    }

    companion object {
        private const val TAG = "StreamReceiver"
    }
}