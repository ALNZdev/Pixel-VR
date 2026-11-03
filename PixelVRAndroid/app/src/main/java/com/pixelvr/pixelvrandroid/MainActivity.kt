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
    private var receiver: StreamReceiver? = null
    private var receiverThread: Thread? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        enterImmersiveMode()

        setContentView(R.layout.activity_main)

        glSurfaceView = findViewById(R.id.glSurfaceView)
        glSurfaceView.setEGLContextClientVersion(2)
        glSurfaceView.setRenderer(object : GLSurfaceView.Renderer {
            override fun onSurfaceCreated(gl: javax.microedition.khronos.opengles.GL10?, config: javax.microedition.khronos.egl.EGLConfig?) {
                val texNames = IntArray(1)
                android.opengl.GLES20.glGenTextures(1, texNames, 0)

                val surfaceTexture = android.graphics.SurfaceTexture(texNames[0])
                surfaceTexture.setDefaultBufferSize(1920, 1080)

                val decoderSurface = android.view.Surface(surfaceTexture)
                val decoder = VideoDecoder(decoderSurface)

                receiver = StreamReceiver(decoder)
                receiverThread = Thread(receiver, "PixelVR-TCP-Receiver").also { it.start() }
            }

            override fun onSurfaceChanged(gl: javax.microedition.khronos.opengles.GL10?, width: Int, height: Int) {
                android.opengl.GLES20.glViewport(0, 0, width, height)
            }

            override fun onDrawFrame(gl: javax.microedition.khronos.opengles.GL10?) {
                // El render stereo se hace en StereoRenderer real
            }
        })
        glSurfaceView.renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) enterImmersiveMode()
    }

    override fun onDestroy() {
        receiver?.stop()
        receiverThread?.interrupt()
        receiverThread = null
        super.onDestroy()
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