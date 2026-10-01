# Android USB connection notes

The Android receiver connects to `127.0.0.1:<port>`. The PC must create an ADB reverse mapping for the same TCP port. `MainWindow.xaml.cs` now creates that mapping for the detected/selected authorized phone when Android settings are saved and again before WPF starts SteamVR. It removes only PixelVR's port from the Stop actions and leaves the tunnel in place if the WPF window closes while SteamVR is still running.

After changing `displayMode`, fully restart SteamVR so the driver activates the Android display path. The phone app can be open before or after SteamVR; the Android receiver retries the connection.

The supplied SteamVR log from 2026-09-30 20:07:58 shows PixelVR activated as `desktop`, with a 1366x768 window. The settings file inspected later still had `displayMode: monitor` and `streamEnable: false`, despite its stream dimensions being 2436x1080. That session therefore did not activate the Android streaming path. The ADB reverse omission is a separate connection blocker: the original WPF code defined `ReversePort()` but never called it. The codec selector also appeared empty in the supplied capture; its code had cleared and recreated the XAML items. It now keeps the declared H.264/HEVC items and selects the saved codec.
