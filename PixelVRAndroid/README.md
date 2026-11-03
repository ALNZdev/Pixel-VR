# PixelVR Android

Minimal Android display client for the existing PixelVR TCP video stream. The phone displays the complete SBS frame; it does not use sensors or tracking.

## Open and build

1. Open this `PixelVRAndroid` folder in Android Studio.
2. In **Settings → Build, Execution, Deployment → Build Tools → Gradle**, select Android Studio's embedded JDK (JDK 17 or newer). This project uses Gradle 8.6 and Android Gradle Plugin 8.4.2; JDK 8 cannot build it.
3. In **Tools → SDK Manager**, install Android SDK Platform 34 and Android SDK Build-Tools 34.0.0, and accept the Android SDK license prompts in Android Studio.
4. Choose **Build → Build APK(s)**. Android Studio will show the generated APK location.

The project intentionally does not include `local.properties`; Android Studio creates it for the SDK installed on that computer.

## Connect to the PC stream

1. Connect the phone by USB, enable USB debugging, and authorize the computer.
2. Confirm the device is listed by `adb devices`.
3. Create the USB reverse mapping with `adb reverse tcp:9944 tcp:9944` if the PC app has not already created it.
4. Install and open PixelVR Android. It stays in landscape/fullscreen and retries the loopback TCP connection until the PC server is available.
5. Start the existing PixelVR Android streaming mode on the PC. The Android app connects to `127.0.0.1:9944`.

## Stream protocol

The receiver follows the driver's 28-byte little-endian `PVRF` header and reads exactly the announced payload length. It accepts H.264 or HEVC Annex-B access units, takes frame dimensions from each header, and sends them to Android `MediaCodec` with the activity's `Surface` as the output. TCP reads are accumulated across arbitrary packet boundaries; incomplete connections are retried.
