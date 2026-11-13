package com.pixelvr

import android.app.Activity
import android.graphics.SurfaceTexture
import android.opengl.EGL14
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.opengl.GLSurfaceView
import android.util.Log
import android.view.Surface
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

/** Decodes the SBS stream to an external GL texture and submits it through OpenXR. */
class StereoRenderer(
    private val activity: Activity,
    private val onDecoderSurfaceReady: (Surface) -> Unit
) : GLSurfaceView.Renderer {
    private var surfaceTexture: SurfaceTexture? = null
    private var decoderSurface: Surface? = null
    private val openXrSession = OpenXrSession()
    private var textureId = 0
    private var xrReady = false
    private val textureMatrix = FloatArray(16)

    override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
        // The EGL context and OpenXR session are recreated after the activity resumes.
        release()
        val textures = IntArray(1)
        GLES20.glGenTextures(1, textures, 0)
        textureId = textures[0]
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, textureId)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)

        val texture = SurfaceTexture(textureId)
        texture.setDefaultBufferSize(1920, 1080)
        texture.setOnFrameAvailableListener { onFrameAvailable() }
        surfaceTexture = texture
        decoderSurface = Surface(texture)

        xrReady = initializeOpenXr()
        if (!xrReady) Log.e(TAG, "OpenXR initialization failed; immersive display stays black")
        if (xrReady) onDecoderSurfaceReady(decoderSurface!!)
    }

    private fun initializeOpenXr(): Boolean {
        val display = EGL14.eglGetCurrentDisplay()
        val drawSurface = EGL14.eglGetCurrentSurface(EGL14.EGL_DRAW)
        val context = EGL14.eglGetCurrentContext()
        if (display == EGL14.EGL_NO_DISPLAY || drawSurface == EGL14.EGL_NO_SURFACE ||
            context == EGL14.EGL_NO_CONTEXT
        ) return false

        val configId = IntArray(1)
        if (!EGL14.eglQuerySurface(display, drawSurface, EGL14.EGL_CONFIG_ID, configId, 0)) return false

        val configs = arrayOfNulls<android.opengl.EGLConfig>(1)
        val configCount = IntArray(1)
        val found = EGL14.eglChooseConfig(
            display,
            intArrayOf(EGL14.EGL_CONFIG_ID, configId[0], EGL14.EGL_NONE),
            0,
            configs,
            0,
            configs.size,
            configCount,
            0
        )
        val eglConfig = configs.firstOrNull() ?: return false
        return found && openXrSession.initialize(
            activity,
            display.nativeHandle,
            eglConfig.nativeHandle,
            context.nativeHandle
        )
    }

    /** Called on the GL thread while its EGL context is current. */
    fun release() {
        openXrSession.shutdown()
        xrReady = false
        decoderSurface?.release()
        decoderSurface = null
        surfaceTexture?.setOnFrameAvailableListener(null)
        surfaceTexture?.release()
        surfaceTexture = null
        if (textureId != 0) GLES20.glDeleteTextures(1, intArrayOf(textureId), 0)
        textureId = 0
    }

    override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
        GLES20.glViewport(0, 0, width, height)
    }

    override fun onDrawFrame(gl: GL10?) {
        // Keep the Android app surface black. The visible image is submitted only as an XR projection layer.
        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, 0)
        GLES20.glClearColor(0f, 0f, 0f, 1f)
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
        if (!xrReady) return

        val texture = surfaceTexture ?: return
        texture.updateTexImage()
        texture.getTransformMatrix(textureMatrix)
        openXrSession.render(textureId, textureMatrix, StreamFov.asDegrees(), StreamFov.frameRate())
    }

    private fun onFrameAvailable() {
        // GLSurfaceView is configured for continuous rendering; do not call GL from this callback.
    }

    companion object {
        private const val TAG = "StereoRenderer"
    }
}
