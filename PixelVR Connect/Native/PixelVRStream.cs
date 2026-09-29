using System;
using System.Runtime.InteropServices;

namespace PixelVR
{
    internal static class PixelVRStream
    {
        private const string DllName = "PixelVRStream.dll";

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void StatusCallback(
            [MarshalAs(UnmanagedType.LPStr)] string message);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl,
            CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool StartStream(
            string phoneIp,
            int videoPort,
            int controlPort,
            int width,
            int height,
            int fps,
            int bitrateKbps,
            int gopSeconds);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void StopStream();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool IsStreamRunning();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void RequestKeyframe();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void RegisterStatusCallback(
            StatusCallback? callback);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        internal static extern bool SetStreamParameters(
            int bitrateKbps,
            int fps,
            int gopSeconds);

        internal static bool Available
        {
            get
            {
                try
                {
                    // Intentamos llamar IsStreamRunning solo para detectar DLL faltante.
                    return IsStreamRunning() || true;
                }
                catch (DllNotFoundException)
                {
                    return false;
                }
                catch (BadImageFormatException)
                {
                    return false;
                }
            }
        }
    }
}