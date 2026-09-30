package com.pixelvr

import android.app.Activity
import android.os.Bundle
import android.util.Log
import android.view.SurfaceHolder
import android.view.SurfaceView

class MainActivity : Activity(), SurfaceHolder.Callback {
    private lateinit var surfaceView: SurfaceView
    private var streamReceiver: StreamReceiver? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        surfaceView = findViewById(R.id.surfaceView)
        surfaceView.holder.addCallback(this)

        Log.d(TAG, "PixelVR Android started")
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        Log.d(TAG, "Surface created")

        val decoder = VideoDecoder(holder.surface)
        streamReceiver = StreamReceiver(decoder)

        Thread {
            streamReceiver?.start()
        }.start()
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        Log.d(TAG, "Surface changed: ${width}x${height}")
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        Log.d(TAG, "Surface destroyed")
        streamReceiver?.stop()
    }

    override fun onDestroy() {
        super.onDestroy()
        streamReceiver?.stop()
    }

    companion object {
        private const val TAG = "PixelVR"
    }
}