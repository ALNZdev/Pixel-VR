using System;
using System.Diagnostics;
using System.IO;
using System.Linq;

namespace PixelVR
{
    public sealed class AdbManager
    {
        private readonly string? adbPath;

        public AdbManager()
        {
            adbPath = FindAdbExecutable();
        }

        public bool IsAvailable => !string.IsNullOrEmpty(adbPath) && File.Exists(adbPath);

        public string? AdbPath => adbPath;

        public string[] GetConnectedDevices()
        {
            if (!IsAvailable)
                return Array.Empty<string>();

            try
            {
                var output = ExecuteAdb("devices");
                var lines = output.Split(new[] { "\r\n", "\n" }, StringSplitOptions.RemoveEmptyEntries);

                return lines
                    .Skip(1)
                    .Where(line => line.Contains("\tdevice") || line.Contains("\tdevice "))
                    .Select(line => line.Split('\t')[0].Trim())
                    .Where(s => !string.IsNullOrWhiteSpace(s))
                    .ToArray();
            }
            catch
            {
                return Array.Empty<string>();
            }
        }

        public bool IsDeviceConnected(string serial)
        {
            if (string.IsNullOrWhiteSpace(serial))
                return false;

            return GetConnectedDevices().Contains(serial, StringComparer.OrdinalIgnoreCase);
        }

        public bool ReversePort(int port, string serial = "")
        {
            if (!IsAvailable)
                return false;

            try
            {
                var args = string.IsNullOrWhiteSpace(serial)
                    ? $"reverse tcp:{port} tcp:{port}"
                    : $"-s {serial} reverse tcp:{port} tcp:{port}";

                var output = ExecuteAdb(args);

                return !output.Contains("error", StringComparison.OrdinalIgnoreCase);
            }
            catch
            {
                return false;
            }
        }

        public bool RemoveReversePort(int port, string serial = "")
        {
            if (!IsAvailable)
                return false;

            try
            {
                var args = string.IsNullOrWhiteSpace(serial)
                    ? $"reverse --remove tcp:{port}"
                    : $"-s {serial} reverse --remove tcp:{port}";

                ExecuteAdb(args);
                return true;
            }
            catch
            {
                return false;
            }
        }

        public bool ClearAllReversals()
        {
            if (!IsAvailable)
                return false;

            try
            {
                ExecuteAdb("reverse --remove-all");
                return true;
            }
            catch
            {
                return false;
            }
        }

        public string GetDeviceInfo(string serial)
        {
            if (!IsAvailable || string.IsNullOrWhiteSpace(serial))
                return serial;

            try
            {
                var brand = ExecuteAdb($"-s {serial} shell getprop ro.product.brand").Trim();
                var model = ExecuteAdb($"-s {serial} shell getprop ro.product.model").Trim();
                var api = ExecuteAdb($"-s {serial} shell getprop ro.build.version.sdk").Trim();

                if (string.IsNullOrWhiteSpace(brand) && string.IsNullOrWhiteSpace(model))
                    return serial;

                return $"{brand} {model} (API {api})";
            }
            catch
            {
                return serial;
            }
        }

        private string? FindAdbExecutable()
        {
            var pathVar = Environment.GetEnvironmentVariable("PATH") ?? string.Empty;
            foreach (var dir in pathVar.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
            {
                var candidate = Path.Combine(dir, "adb.exe");
                if (File.Exists(candidate))
                    return candidate;
            }

            var candidates = new[]
            {
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Android", "Sdk", "platform-tools", "adb.exe"),
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "Android", "android-sdk", "platform-tools", "adb.exe"),
                @"C:\Android\android-sdk\platform-tools\adb.exe",
                @"C:\Program Files\Android\android-sdk\platform-tools\adb.exe"
            };

            foreach (var p in candidates)
            {
                if (File.Exists(p))
                    return p;
            }

            return null;
        }

        private string ExecuteAdb(string args)
        {
            if (!IsAvailable)
                throw new InvalidOperationException("ADB no está disponible.");

            using var process = new Process();
            process.StartInfo.FileName = adbPath;
            process.StartInfo.Arguments = args;
            process.StartInfo.UseShellExecute = false;
            process.StartInfo.RedirectStandardOutput = true;
            process.StartInfo.RedirectStandardError = true;
            process.StartInfo.CreateNoWindow = true;

            process.Start();
            var stdout = process.StandardOutput.ReadToEnd();
            var stderr = process.StandardError.ReadToEnd();
            process.WaitForExit(10000);

            if (!string.IsNullOrWhiteSpace(stderr) && !stderr.Contains("List of devices attached"))
                throw new InvalidOperationException(stderr.Trim());

            return stdout;
        }
    }
}