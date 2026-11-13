package com.pixelvr

import android.annotation.SuppressLint
import android.app.Activity
import android.content.pm.ActivityInfo
import android.opengl.GLSurfaceView
import android.os.Bundle
import android.view.View
import android.view.WindowManager

class MainActivity : Activity() {
    private lateinit var glSurfaceView: GLSurfaceView
    private lateinit var stereoRenderer: StereoRenderer
    private var decoderSurface: android.view.Surface? = null
    private var receiver: StreamReceiver? = null
    private var receiverThread: Thread? = null
    private var resumed = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        enterImmersiveMode()

        setContentView(R.layout.activity_main)

        glSurfaceView = findViewById(R.id.glSurfaceView)
        glSurfaceView.setEGLContextClientVersion(3)
        glSurfaceView.setPreserveEGLContextOnPause(false)
        stereoRenderer = StereoRenderer(this) { surface ->
            runOnUiThread {
                decoderSurface = surface
                if (resumed) startReceiver(surface)
            }
        }
        glSurfaceView.setRenderer(stereoRenderer)
        glSurfaceView.renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
    }

    override fun onResume() {
        super.onResume()
        resumed = true
        glSurfaceView.onResume()
        decoderSurface?.let(::startReceiver)
    }

    override fun onPause() {
        resumed = false
        stopReceiver()
        decoderSurface = null
        glSurfaceView.queueEvent { stereoRenderer.release() }
        glSurfaceView.onPause()
        super.onPause()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) enterImmersiveMode()
    }

    override fun onDestroy() {
        stopReceiver()
        super.onDestroy()
    }

    private fun startReceiver(surface: android.view.Surface) {
        if (receiverThread?.isAlive == true) return
        val streamReceiver = StreamReceiver(VideoDecoder(surface))
        receiver = streamReceiver
        receiverThread = Thread(streamReceiver, "PixelVR-TCP-Receiver").also { it.start() }
    }

    private fun stopReceiver() {
        val oldReceiver = receiver
        val oldThread = receiverThread
        receiver = null
        receiverThread = null
        oldReceiver?.stop()
        oldThread?.interrupt()
        if (oldThread != null && oldThread !== Thread.currentThread()) {
            try {
                oldThread.join(2_000)
            } catch (_: InterruptedException) {
                Thread.currentThread().interrupt()
            }
        }
    }

    @SuppressLint("InlinedApi")
    private fun enterImmersiveMode() {
        window.decorView.systemUiVisibility = (
                View.SYSTEM_UI_FLAG_FULLSCREEN
                        or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                )
    }
}
