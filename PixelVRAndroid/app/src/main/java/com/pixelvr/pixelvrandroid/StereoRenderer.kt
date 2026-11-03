package com.pixelvr

import android.graphics.SurfaceTexture
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.opengl.GLSurfaceView
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

class StereoRenderer : GLSurfaceView.Renderer {
    private var surfaceTexture: SurfaceTexture? = null
    private var textureId = 0
    private var programId = 0
    private var positionHandle = 0
    private var texCoordHandle = 0
    private var textureHandle = 0
    private var frameAvailable = false

    private val quadVertices = floatArrayOf(
        -1f, -1f,
        1f, -1f,
        -1f,  1f,
        1f,  1f
    )

    private val quadTexCoords = floatArrayOf(
        0f, 0f,
        1f, 0f,
        0f, 1f,
        1f, 1f
    )

    fun setSurfaceTexture(surfaceTexture: SurfaceTexture) {
        this.surfaceTexture = surfaceTexture
        this.surfaceTexture?.setOnFrameAvailableListener {
            synchronized(this) {
                frameAvailable = true
            }
        }
    }

    override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
        val textures = IntArray(1)
        GLES20.glGenTextures(1, textures, 0)
        textureId = textures[0]

        val tex = SurfaceTexture(textureId)
        setSurfaceTexture(tex)

        val vertexShaderCode = """
            attribute vec2 aPosition;
            attribute vec2 aTexCoord;
            varying vec2 vTexCoord;

            void main() {
                gl_Position = vec4(aPosition, 0.0, 1.0);
                vTexCoord = aTexCoord;
            }
        """.trimIndent()

        val fragmentShaderCode = """
            #extension GL_OES_EGL_image_external : require
            precision mediump float;

            uniform samplerExternalOES uTexture;
            varying vec2 vTexCoord;

            void main() {
                vec2 uv = vTexCoord;
                float eye = step(0.5, uv.x);
                float xEye = (uv.x - eye * 0.5) / 0.5;
                vec2 p = vec2(xEye, uv.y);

                vec2 centered = vec2(p.x * 2.0 - 1.0, p.y * 2.0 - 1.0);
                float r2 = dot(centered, centered);
                centered *= 1.0 + r2 * 0.35;

                vec2 src = (centered + 1.0) * 0.5;
                src.x = mix(src.x * 0.5, 0.5 + src.x * 0.5, eye);

                vec4 color = texture2D(uTexture, clamp(src, 0.0, 1.0));
                gl_FragColor = color;
            }
        """.trimIndent()

        programId = createProgram(vertexShaderCode, fragmentShaderCode)
        positionHandle = GLES20.glGetAttribLocation(programId, "aPosition")
        texCoordHandle = GLES20.glGetAttribLocation(programId, "aTexCoord")
        textureHandle = GLES20.glGetUniformLocation(programId, "uTexture")

        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, textureId)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)

        surfaceTexture?.setDefaultBufferSize(1920, 1080)
        surfaceTexture?.attachToGLContext(textureId)
    }

    override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
        GLES20.glViewport(0, 0, width, height)
    }

    override fun onDrawFrame(gl: GL10?) {
        synchronized(this) {
            if (frameAvailable) {
                surfaceTexture?.updateTexImage()
                frameAvailable = false
            }
        }

        GLES20.glClearColor(0f, 0f, 0f, 1f)
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
        GLES20.glUseProgram(programId)

        val vertexBuffer = ByteBuffer.allocateDirect(quadVertices.size * 4).order(ByteOrder.nativeOrder())
        val vertexFloatBuffer = vertexBuffer.asFloatBuffer()
        vertexFloatBuffer.put(quadVertices)
        vertexFloatBuffer.position(0)

        val texBuffer = ByteBuffer.allocateDirect(quadTexCoords.size * 4).order(ByteOrder.nativeOrder())
        val texFloatBuffer = texBuffer.asFloatBuffer()
        texFloatBuffer.put(quadTexCoords)
        texFloatBuffer.position(0)

        GLES20.glEnableVertexAttribArray(positionHandle)
        GLES20.glVertexAttribPointer(positionHandle, 2, GLES20.GL_FLOAT, false, 0, vertexFloatBuffer)

        GLES20.glEnableVertexAttribArray(texCoordHandle)
        GLES20.glVertexAttribPointer(texCoordHandle, 2, GLES20.GL_FLOAT, false, 0, texFloatBuffer)

        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, textureId)
        GLES20.glUniform1i(textureHandle, 0)

        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
    }

    private fun createProgram(vertexSource: String, fragmentSource: String): Int {
        val vertexShader = compileShader(GLES20.GL_VERTEX_SHADER, vertexSource)
        val fragmentShader = compileShader(GLES20.GL_FRAGMENT_SHADER, fragmentSource)

        val program = GLES20.glCreateProgram()
        GLES20.glAttachShader(program, vertexShader)
        GLES20.glAttachShader(program, fragmentShader)
        GLES20.glLinkProgram(program)

        val status = IntArray(1)
        GLES20.glGetProgramiv(program, GLES20.GL_LINK_STATUS, status, 0)
        if (status[0] == 0) {
            throw RuntimeException("OpenGL link error: ${GLES20.glGetProgramInfoLog(program)}")
        }

        GLES20.glDeleteShader(vertexShader)
        GLES20.glDeleteShader(fragmentShader)
        return program
    }

    private fun compileShader(type: Int, source: String): Int {
        val shader = GLES20.glCreateShader(type)
        GLES20.glShaderSource(shader, source)
        GLES20.glCompileShader(shader)

        val status = IntArray(1)
        GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, status, 0)
        if (status[0] == 0) {
            throw RuntimeException("OpenGL shader error: ${GLES20.glGetProgramInfoLog(shader)}")
        }

        return shader
    }
}