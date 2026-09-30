package com.pixelvr.pixelvrandroid
import android.os.Bundle
import android.util.Log
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.appcompat.app.AppCompatActivity
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch

class MainActivity : AppCompatActivity(), SurfaceHolder.Callback {
    private lateinit var surfaceView: SurfaceView
    private var streamReceiver: StreamReceiver? = null
    private var videoDecoder: VideoDecoder? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        surfaceView = findViewById(R.id.surfaceView)
        surfaceView.holder.addCallback(this)

        Log.d(TAG, "PixelVR Android iniciado")
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        Log.d(TAG, "Surface creada")

        videoDecoder = VideoDecoder(holder.surface)
        streamReceiver = StreamReceiver(videoDecoder!!)

        CoroutineScope(Dispatchers.Default).launch {
            streamReceiver!!.start()
        }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        Log.d(TAG, "Surface changed: ${width}x${height}")
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        Log.d(TAG, "Surface destruida")
        streamReceiver?.stop()
        videoDecoder?.release()
    }

    override fun onDestroy() {
        super.onDestroy()
        streamReceiver?.stop()
        videoDecoder?.release()
    }

    companion object {
        private const val TAG = "PixelVR"
    }
}}