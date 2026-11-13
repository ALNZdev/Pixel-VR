package com.pixelvr

import android.app.Activity

/** Small JNI bridge; all graphics calls are made from GLSurfaceView's EGL thread. */
internal class OpenXrSession {
    external fun initialize(
        activity: Activity,
        eglDisplay: Long,
        eglConfig: Long,
        eglContext: Long
    ): Boolean

    external fun render(
        sbsTextureId: Int,
        textureMatrix: FloatArray,
        sourceFovDegrees: FloatArray,
        streamFrameRate: Int
    ): Boolean
    external fun shutdown()

    companion object {
        init {
            System.loadLibrary("pixelvr_openxr")
        }
    }
}
