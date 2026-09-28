using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;

namespace PixelVR
{
    public class DriverSettings
    {
        // Display Settings
        public int WindowX { get; set; } = 0;
        public int WindowY { get; set; } = 0;
        public int WindowWidth { get; set; } = 1920;
        public int WindowHeight { get; set; } = 1080;
        public int RenderWidth { get; set; } = 1920;
        public int RenderHeight { get; set; } = 1080;
        public bool DebugMode { get; set; } = false;
        public bool DirectMode { get; set; } = false;
        public int EdidVid { get; set; } = 0;
        public int EdidPid { get; set; } = 0;

        // HMD Properties
        public string SerialNumber { get; set; } = "PixelVR_HMD_001";
        public string ModelNumber { get; set; } = "PixelVR v1.0";
        public string ManufacturerName { get; set; } = "PixelVR Project";

        // Display & Optics
        public float DisplayFrequency { get; set; } = 90.0f;
        public float IpdMeters { get; set; } = 0.064f;
        public float FovLeftDegrees { get; set; } = 50.0f;
        public float FovRightDegrees { get; set; } = 50.0f;
        public float FovTopDegrees { get; set; } = 50.0f;
        public float FovBottomDegrees { get; set; } = 50.0f;

        // Tracking
        public float TrackingScale { get; set; } = 1.0f;
        public float PoseTimeOffset { get; set; } = 0.0f;
        public bool EnablePositionTracking { get; set; } = true;
        public bool EnableRotationTracking { get; set; } = true;

        // PSM Tracking
        public bool EnablePSMTracking { get; set; } = true;
        public int PsmTrackingTimeout { get; set; } = 5000;
        public int PsmReconnectRetries { get; set; } = 5;

        // Performance
        public bool EnableAsyncReprojection { get; set; } = true;
        public bool EnableInteractionProfile { get; set; } = true;
        public int LoadPriority { get; set; } = 10;
        public bool BlockedBySafeMode { get; set; } = false;

        // Logging
        public bool VerboseLogging { get; set; } = false;
        public string LogFilePath { get; set; } = "pixelvr_driver.log";
        public bool EnablePerformanceMetrics { get; set; } = false;

        // Distortion
        public float DistortionK1 { get; set; } = 0.0f;
        public float DistortionK2 { get; set; } = 0.0f;
        public bool DistortionChromaticAberration { get; set; } = false;

        // Power Management
        public bool VsyncEnabled { get; set; } = true;
        public string PowerManagement { get; set; } = "full";
        public int StandbyTimeout { get; set; } = 300;

        // Controllers
        public string LeftControllerSerial { get; set; } = "PixelVR_LEFT_001";
        public string RightControllerSerial { get; set; } = "PixelVR_RIGHT_001";
        public float LeftHapticAmplitude { get; set; } = 1.0f;
        public float RightHapticAmplitude { get; set; } = 1.0f;
        public bool EnableLeftHaptics { get; set; } = true;
        public bool EnableRightHaptics { get; set; } = true;
    }

    public class DriverSettingsManager
    {
        private string settingsPath;

        public DriverSettingsManager(string settingsPath)
        {
            this.settingsPath = settingsPath;
        }

        public DriverSettings LoadSettings()
        {
            try
            {
                if (!File.Exists(settingsPath))
                {
                    return new DriverSettings();
                }

                string json = File.ReadAllText(settingsPath);
                using (JsonDocument doc = JsonDocument.Parse(json))
                {
                    var settings = new DriverSettings();
                    JsonElement root = doc.RootElement;

                    if (root.TryGetProperty("driver_pixelvr", out JsonElement driverSection))
                    {
                        settings.WindowX = GetJsonInt(driverSection, "windowX", 0);
                        settings.WindowY = GetJsonInt(driverSection, "windowY", 0);
                        settings.WindowWidth = GetJsonInt(driverSection, "windowWidth", 1920);
                        settings.WindowHeight = GetJsonInt(driverSection, "windowHeight", 1080);
                        settings.RenderWidth = GetJsonInt(driverSection, "renderWidth", 1920);
                        settings.RenderHeight = GetJsonInt(driverSection, "renderHeight", 1080);
                        settings.DebugMode = GetJsonBool(driverSection, "debugMode", false);
                        settings.DirectMode = GetJsonBool(driverSection, "directMode", false);
                        settings.EdidVid = GetJsonInt(driverSection, "edidVid", 0);
                        settings.EdidPid = GetJsonInt(driverSection, "edidPid", 0);

                        settings.SerialNumber = GetJsonString(driverSection, "serialNumber", "PixelVR_HMD_001");
                        settings.ModelNumber = GetJsonString(driverSection, "modelNumber", "PixelVR v1.0");
                        settings.ManufacturerName = GetJsonString(driverSection, "manufacturerName", "PixelVR Project");

                        settings.DisplayFrequency = GetJsonFloat(driverSection, "displayFrequency", 90.0f);
                        settings.IpdMeters = GetJsonFloat(driverSection, "ipdMeters", 0.064f);
                        settings.FovLeftDegrees = GetJsonFloat(driverSection, "fovLeftDegrees", 50.0f);
                        settings.FovRightDegrees = GetJsonFloat(driverSection, "fovRightDegrees", 50.0f);
                        settings.FovTopDegrees = GetJsonFloat(driverSection, "fovTopDegrees", 50.0f);
                        settings.FovBottomDegrees = GetJsonFloat(driverSection, "fovBottomDegrees", 50.0f);

                        settings.TrackingScale = GetJsonFloat(driverSection, "trackingScale", 1.0f);
                        settings.PoseTimeOffset = GetJsonFloat(driverSection, "poseTimeOffset", 0.0f);
                        settings.EnablePositionTracking = GetJsonBool(driverSection, "enablePositionTracking", true);
                        settings.EnableRotationTracking = GetJsonBool(driverSection, "enableRotationTracking", true);

                        settings.EnablePSMTracking = GetJsonBool(driverSection, "enablePSMTracking", true);
                        settings.PsmTrackingTimeout = GetJsonInt(driverSection, "psmTrackingTimeout", 5000);
                        settings.PsmReconnectRetries = GetJsonInt(driverSection, "psmReconnectRetries", 5);

                        settings.EnableAsyncReprojection = GetJsonBool(driverSection, "enableAsyncReprojection", true);
                        settings.VerboseLogging = GetJsonBool(driverSection, "verboseLogging", false);
                        settings.EnablePerformanceMetrics = GetJsonBool(driverSection, "enablePerformanceMetrics", false);

                        settings.DistortionK1 = GetJsonFloat(driverSection, "distortionK1", 0.0f);
                        settings.DistortionK2 = GetJsonFloat(driverSection, "distortionK2", 0.0f);
                        settings.DistortionChromaticAberration = GetJsonBool(driverSection, "distortionChromaticAberration", false);

                        settings.VsyncEnabled = GetJsonBool(driverSection, "vsyncEnabled", true);
                        settings.PowerManagement = GetJsonString(driverSection, "powerManagement", "full");
                        settings.StandbyTimeout = GetJsonInt(driverSection, "standbyTimeout", 300);
                    }

                    if (root.TryGetProperty("driver_pixelvr_left_controller", out JsonElement leftCtrl))
                    {
                        settings.LeftControllerSerial = GetJsonString(leftCtrl, "serialNumber", "PixelVR_LEFT_001");
                        settings.LeftHapticAmplitude = GetJsonFloat(leftCtrl, "hapticAmplitude", 1.0f);
                        settings.EnableLeftHaptics = GetJsonBool(leftCtrl, "enableHaptics", true);
                    }

                    if (root.TryGetProperty("driver_pixelvr_right_controller", out JsonElement rightCtrl))
                    {
                        settings.RightControllerSerial = GetJsonString(rightCtrl, "serialNumber", "PixelVR_RIGHT_001");
                        settings.RightHapticAmplitude = GetJsonFloat(rightCtrl, "hapticAmplitude", 1.0f);
                        settings.EnableRightHaptics = GetJsonBool(rightCtrl, "enableHaptics", true);
                    }

                    return settings;
                }
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Error loading settings: {ex.Message}");
                return new DriverSettings();
            }
        }

        public void SaveSettings(DriverSettings settings)
        {
            try
            {
                var options = new JsonSerializerOptions { WriteIndented = true };

                var driverSection = new Dictionary<string, object>
                {
                    { "enable", true },
                    { "windowX", settings.WindowX },
                    { "windowY", settings.WindowY },
                    { "windowWidth", settings.WindowWidth },
                    { "windowHeight", settings.WindowHeight },
                    { "renderWidth", settings.RenderWidth },
                    { "renderHeight", settings.RenderHeight },
                    { "debugMode", settings.DebugMode },
                    { "directMode", settings.DirectMode },
                    { "edidVid", settings.DirectMode ? settings.EdidVid : 0 },
                    { "edidPid", settings.DirectMode ? settings.EdidPid : 0 },
                    { "serialNumber", settings.SerialNumber },
                    { "modelNumber", settings.ModelNumber },
                    { "manufacturerName", settings.ManufacturerName },
                    { "displayFrequency", settings.DisplayFrequency },
                    { "ipdMeters", settings.IpdMeters },
                    { "fovLeftDegrees", settings.FovLeftDegrees },
                    { "fovRightDegrees", settings.FovRightDegrees },
                    { "fovTopDegrees", settings.FovTopDegrees },
                    { "fovBottomDegrees", settings.FovBottomDegrees },
                    { "trackingScale", settings.TrackingScale },
                    { "poseTimeOffset", settings.PoseTimeOffset },
                    { "enablePositionTracking", settings.EnablePositionTracking },
                    { "enableRotationTracking", settings.EnableRotationTracking },
                    { "enablePSMTracking", settings.EnablePSMTracking },
                    { "psmTrackingTimeout", settings.PsmTrackingTimeout },
                    { "psmReconnectRetries", settings.PsmReconnectRetries },
                    { "enableAsyncReprojection", settings.EnableAsyncReprojection },
                    { "enableInteractionProfile", true },
                    { "loadPriority", settings.LoadPriority },
                    { "blocked_by_safe_mode", false },
                    { "verboseLogging", settings.VerboseLogging },
                    { "logFilePath", settings.LogFilePath },
                    { "enablePerformanceMetrics", settings.EnablePerformanceMetrics },
                    { "distortionK1", settings.DistortionK1 },
                    { "distortionK2", settings.DistortionK2 },
                    { "distortionChromaticAberration", settings.DistortionChromaticAberration },
                    { "vsyncEnabled", settings.VsyncEnabled },
                    { "powerManagement", settings.PowerManagement },
                    { "standbyTimeout", settings.StandbyTimeout },
                    { "settingsCheckInterval", 500 }
                };

                var leftCtrl = new Dictionary<string, object>
                {
                    { "serialNumber", settings.LeftControllerSerial },
                    { "hapticAmplitude", settings.LeftHapticAmplitude },
                    { "batteryUpdateInterval", 60 },
                    { "enableHaptics", settings.EnableLeftHaptics },
                    { "vibrationIntensity", 1.0 }
                };

                var rightCtrl = new Dictionary<string, object>
                {
                    { "serialNumber", settings.RightControllerSerial },
                    { "hapticAmplitude", settings.RightHapticAmplitude },
                    { "batteryUpdateInterval", 60 },
                    { "enableHaptics", settings.EnableRightHaptics },
                    { "vibrationIntensity", 1.0 }
                };

                var root = new Dictionary<string, object>
                {
                    { "driver_pixelvr", driverSection },
                    { "driver_pixelvr_left_controller", leftCtrl },
                    { "driver_pixelvr_right_controller", rightCtrl }
                };

                string json = JsonSerializer.Serialize(root, options);
                File.WriteAllText(settingsPath, json);
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Error saving settings: {ex.Message}");
            }
        }

        private int GetJsonInt(JsonElement element, string propertyName, int defaultValue)
        {
            if (element.TryGetProperty(propertyName, out JsonElement prop) && prop.ValueKind == JsonValueKind.Number)
                return prop.GetInt32();
            return defaultValue;
        }

        private float GetJsonFloat(JsonElement element, string propertyName, float defaultValue)
        {
            if (element.TryGetProperty(propertyName, out JsonElement prop))
            {
                if (prop.ValueKind == JsonValueKind.Number)
                    return prop.GetSingle();
            }
            return defaultValue;
        }

        private bool GetJsonBool(JsonElement element, string propertyName, bool defaultValue)
        {
            if (element.TryGetProperty(propertyName, out JsonElement prop) && (prop.ValueKind == JsonValueKind.True || prop.ValueKind == JsonValueKind.False))
                return prop.GetBoolean();
            return defaultValue;
        }

        private string GetJsonString(JsonElement element, string propertyName, string defaultValue)
        {
            if (element.TryGetProperty(propertyName, out JsonElement prop) && prop.ValueKind == JsonValueKind.String)
                return prop.GetString() ?? defaultValue;
            return defaultValue;
        }
    }
}