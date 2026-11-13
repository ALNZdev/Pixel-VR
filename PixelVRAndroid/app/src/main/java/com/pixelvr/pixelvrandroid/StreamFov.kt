package com.pixelvr

/** Latest source projection metadata carried with the video stream. */
internal object StreamFov {
    private val degrees = floatArrayOf(96f, 96f, 93f, 93f)
    @Volatile private var frameRate = 0

    @Synchronized
    fun update(frame: PacketParser.ParsedFrame) {
        degrees[0] = frame.fovLeftDegrees
        degrees[1] = frame.fovRightDegrees
        degrees[2] = frame.fovTopDegrees
        degrees[3] = frame.fovBottomDegrees
        frameRate = frame.frameRate
    }

    @Synchronized
    fun asDegrees() = degrees.copyOf()

    fun frameRate() = frameRate
}
