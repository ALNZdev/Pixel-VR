using MaterialDesignThemes.Wpf.Transitions;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Management;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using Application = System.Windows.Application;
using Button = System.Windows.Controls.Button;
using Color = System.Windows.Media.Color;
using MessageBox = System.Windows.MessageBox;
using System.Runtime.InteropServices;
using System.Text;

namespace PixelVR
{
    public partial class MainWindow : Window
    {
        private PipeManager? pipeManager;
        private USBCDCManager? usbcdcManager;
        private uint leftButtons = 0;
        private uint rightButtons = 0;
        private bool driverTestActive = false;
        private bool usbcdcDrivingActive = false;

        private DriverSettingsManager? settingsManager;
        private DriverSettings? currentSettings;
        
        private PhoneStreamController? phoneStreamController;
        private bool phoneStreamModeActive = false;

        // Evita que los eventos ValueChanged/Checked disparados por WPF durante
        // InitializeComponent() (al setear los valores iniciales del XAML) intenten
        // tocar controles que todavía no fueron asignados (ej. lblConfigStatus),
        // lo cual provoca NullReferenceException al abrir la app.
        private bool uiReady = false;
        private const string DriverSettingsPath = @"C:\Users\gabum\Desktop\Pixel VR\PixelVR Driver\pixelvr\resources\settings\default.vrsettings";

        private static readonly SolidColorBrush NavAccentColor = new SolidColorBrush(Color.FromRgb(0x7C, 0x6A, 0xF7));
        private static readonly SolidColorBrush NavDimColor = new SolidColorBrush(Color.FromRgb(0x5C, 0x62, 0x80));
        private static readonly SolidColorBrush NavActiveText = new SolidColorBrush(Color.FromRgb(0x9E, 0xA3, 0xB8));

        public MainWindow()
        {
            InitializeComponent();
            InitializeConnectionStatus();
            LoadAvailableScreens();
            InitializeControllerData();
            InitializeUSBCDC();

            settingsManager = new DriverSettingsManager(DriverSettingsPath);
            currentSettings = settingsManager.LoadSettings();
            InitializeSettingsUI();

            // Inicialización del controlador del stream al teléfono
            phoneStreamController = new PhoneStreamController(Dispatcher);
            phoneStreamController.StatusChanged += PhoneStreamController_StatusChanged;

            SelectNavSection("main");
            this.Loaded += MainWindow_Loaded;

            // A partir de acá sí es seguro reaccionar a los eventos de los
            // controles (sliders, checkboxes, etc.) generados por el usuario.
            uiReady = true;
        }

        private void MainWindow_Loaded(object sender, RoutedEventArgs e)
        {
        }

        // ==================== CONFIGURACIÓN DRIVER ====================

        private void InitializeSettingsUI()
        {
            if (currentSettings == null)
                currentSettings = new DriverSettings();

            sliderDisplayFreq.Value = currentSettings.DisplayFrequency;
            sliderIpd.Value = currentSettings.IpdMeters;
            sliderFovLeft.Value = currentSettings.FovLeftDegrees;
            sliderFovRight.Value = currentSettings.FovRightDegrees;
            sliderFovTop.Value = currentSettings.FovTopDegrees;
            sliderFovBottom.Value = currentSettings.FovBottomDegrees;
            sliderTrackingScale.Value = currentSettings.TrackingScale;
            sliderLeftHaptic.Value = currentSettings.LeftHapticAmplitude;
            sliderRightHaptic.Value = currentSettings.RightHapticAmplitude;

            txtRenderWidth.Text = currentSettings.RenderWidth.ToString();
            txtRenderHeight.Text = currentSettings.RenderHeight.ToString();
            txtPsmTimeout.Text = currentSettings.PsmTrackingTimeout.ToString();
            txtPsmRetries.Text = currentSettings.PsmReconnectRetries.ToString();

            chkEnablePosTracking.IsChecked = currentSettings.EnablePositionTracking;
            chkEnableRotTracking.IsChecked = currentSettings.EnableRotationTracking;
            chkEnablePSMTracking.IsChecked = currentSettings.EnablePSMTracking;

            chkLeftHaptics.IsChecked = currentSettings.EnableLeftHaptics;
            chkRightHaptics.IsChecked = currentSettings.EnableRightHaptics;

            chkDebugMode.IsChecked = currentSettings.DebugMode;
            chkDirectMode.IsChecked = currentSettings.DirectMode;
            chkVerboseLogging.IsChecked = currentSettings.VerboseLogging;
            chkAsyncReprojection.IsChecked = currentSettings.EnableAsyncReprojection;
            chkVsyncEnabled.IsChecked = currentSettings.VsyncEnabled;

            lblDisplayFreq.Text = currentSettings.DisplayFrequency.ToString("F0");
            lblIpd.Text = currentSettings.IpdMeters.ToString("F3");
            lblFovLeft.Text = currentSettings.FovLeftDegrees.ToString("F0");
            lblFovRight.Text = currentSettings.FovRightDegrees.ToString("F0");
            lblFovTop.Text = currentSettings.FovTopDegrees.ToString("F0");
            lblFovBottom.Text = currentSettings.FovBottomDegrees.ToString("F0");
            lblTrackingScale.Text = currentSettings.TrackingScale.ToString("F1");
            lblLeftHaptic.Text = currentSettings.LeftHapticAmplitude.ToString("F1");
            lblRightHaptic.Text = currentSettings.RightHapticAmplitude.ToString("F1");

            lblConfigStatus.Text = "Configuración cargada";
            lblConfigStatus.Foreground = new SolidColorBrush(Color.FromRgb(34, 201, 147));
        }

        private void UpdateSettingsLabels()
        {
            if (!uiReady || lblConfigStatus == null)
                return;

            lblConfigStatus.Text = "Cambios sin guardar";
            lblConfigStatus.Foreground = new SolidColorBrush(Color.FromRgb(240, 82, 82));
        }

        private void btnLoadSettings_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                if (settingsManager == null)
                    settingsManager = new DriverSettingsManager(DriverSettingsPath);

                currentSettings = settingsManager.LoadSettings();
                InitializeSettingsUI();

                lblConfigStatus.Text = "Configuración cargada correctamente";
                lblConfigStatus.Foreground = new SolidColorBrush(Color.FromRgb(34, 201, 147));
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Error al cargar configuración: {ex.Message}", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void btnSaveSettings_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                if (currentSettings == null)
                    currentSettings = new DriverSettings();

                currentSettings.DisplayFrequency = (float)sliderDisplayFreq.Value;
                currentSettings.IpdMeters = (float)sliderIpd.Value;
                currentSettings.FovLeftDegrees = (float)sliderFovLeft.Value;
                currentSettings.FovRightDegrees = (float)sliderFovRight.Value;
                currentSettings.FovTopDegrees = (float)sliderFovTop.Value;
                currentSettings.FovBottomDegrees = (float)sliderFovBottom.Value;
                currentSettings.TrackingScale = (float)sliderTrackingScale.Value;

                currentSettings.LeftHapticAmplitude = (float)sliderLeftHaptic.Value;
                currentSettings.RightHapticAmplitude = (float)sliderRightHaptic.Value;

                if (int.TryParse(txtRenderWidth.Text, out int renderWidth))
                    currentSettings.RenderWidth = renderWidth;

                if (int.TryParse(txtRenderHeight.Text, out int renderHeight))
                    currentSettings.RenderHeight = renderHeight;

                if (int.TryParse(txtPsmTimeout.Text, out int psmTimeout))
                    currentSettings.PsmTrackingTimeout = psmTimeout;

                if (int.TryParse(txtPsmRetries.Text, out int psmRetries))
                    currentSettings.PsmReconnectRetries = psmRetries;

                currentSettings.EnablePositionTracking = chkEnablePosTracking.IsChecked == true;
                currentSettings.EnableRotationTracking = chkEnableRotTracking.IsChecked == true;
                currentSettings.EnablePSMTracking = chkEnablePSMTracking.IsChecked == true;

                currentSettings.EnableLeftHaptics = chkLeftHaptics.IsChecked == true;
                currentSettings.EnableRightHaptics = chkRightHaptics.IsChecked == true;

                currentSettings.DebugMode = chkDebugMode.IsChecked == true;
                currentSettings.DirectMode = chkDirectMode.IsChecked == true;
                currentSettings.VerboseLogging = chkVerboseLogging.IsChecked == true;
                currentSettings.EnableAsyncReprojection = chkAsyncReprojection.IsChecked == true;
                currentSettings.VsyncEnabled = chkVsyncEnabled.IsChecked == true;

                if (settingsManager == null)
                    settingsManager = new DriverSettingsManager(DriverSettingsPath);

                settingsManager.SaveSettings(currentSettings);

                lblConfigStatus.Text = "Configuración guardada correctamente";
                lblConfigStatus.Foreground = new SolidColorBrush(Color.FromRgb(34, 201, 147));

                MessageBox.Show("Configuración guardada. Reinicia SteamVR para aplicar los ajustes del driver.", "PixelVR", MessageBoxButton.OK, MessageBoxImage.Information);
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Error al guardar la configuración: {ex.Message}", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void btnResetDefaults_Click(object sender, RoutedEventArgs e)
        {
            if (MessageBox.Show("¿Restablecer los valores predeterminados del driver?", "PixelVR", MessageBoxButton.YesNo, MessageBoxImage.Question) == MessageBoxResult.Yes)
            {
                currentSettings = new DriverSettings();
                InitializeSettingsUI();

                lblConfigStatus.Text = "Valores predeterminados restaurados";
                lblConfigStatus.Foreground = new SolidColorBrush(Color.FromRgb(250, 180, 50));
            }
        }

        // ==================== USB-CDC INITIALIZATION ====================

        private void InitializeUSBCDC()
        {
            usbcdcManager = new USBCDCManager();
            usbcdcManager.DataReceived += UsbcdcManager_DataReceived;
            usbcdcManager.ErrorOccurred += UsbcdcManager_ErrorOccurred;
            usbcdcManager.ConnectionStatusChanged += UsbcdcManager_ConnectionStatusChanged;

            RefreshUSBCDCPorts();
            RefreshScreensForUSB();

            hmsScreenModeComboBox.SelectedIndex = 0;
            btnStartUSBCDC.IsEnabled = false;
            btnStopUSBCDC.IsEnabled = false;
        }

        private void RefreshUSBCDCPorts()
        {
            string previousValue = usbcdcComboBox.Text;

            usbcdcComboBox.Items.Clear();

            var ports = USBCDCManager.GetAvailableComPorts();

            if (ports.Length == 0)
            {
                usbcdcComboBox.Text = previousValue;
                btnStartUSBCDC.IsEnabled = false;
            }
            else
            {
                foreach (var port in ports)
                {
                    usbcdcComboBox.Items.Add(port);
                }

                if (!string.IsNullOrWhiteSpace(previousValue))
                {
                    usbcdcComboBox.Text = previousValue;
                }
                else
                {
                    usbcdcComboBox.SelectedIndex = 0;
                }

                btnStartUSBCDC.IsEnabled = true;
            }

            UpdateStartButtonState();
        }

        private void RefreshScreensForUSB()
        {
            hmdScreenComboBox.Items.Clear();
            foreach (var screen in System.Windows.Forms.Screen.AllScreens)
            {
                hmdScreenComboBox.Items.Add($"{screen.DeviceName} ({screen.Bounds.Width}x{screen.Bounds.Height})");
            }

            if (hmdScreenComboBox.Items.Count > 0)
                hmdScreenComboBox.SelectedIndex = 0;
        }

        private string? GetSelectedSerialPort()
        {
            string port = usbcdcComboBox.Text?.Trim() ?? string.Empty;

            if (string.IsNullOrWhiteSpace(port))
                return null;

            if (port.Contains("No hay puertos", StringComparison.OrdinalIgnoreCase))
                return null;

            return port;
        }

        private void hmdScreenComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            UpdateStartButtonState();
        }

        private void hmsScreenModeComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            UpdateStartButtonState();
        }

        private void usbcdcComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            UpdateStartButtonState();
        }

        private void usbcdcComboBox_TextChanged(object sender, TextChangedEventArgs e)
        {
            UpdateStartButtonState();
        }

        private void UpdateStartButtonState()
        {
            if (usbcdcManager?.IsConnected == true)
            {
                btnStartUSBCDC.IsEnabled = false;
                return;
            }

            bool hasValidPort = GetSelectedSerialPort() != null;
            bool hasValidScreen = hmdScreenComboBox.SelectedItem != null;

            btnStartUSBCDC.IsEnabled = hasValidPort && hasValidScreen;
        }

        private void btnRefreshUSBCDCPorts_Click(object sender, RoutedEventArgs e)
        {
            RefreshUSBCDCPorts();
        }

        private void btnStartUSBCDC_Click(object sender, RoutedEventArgs e)
        {
            string? selectedPort = GetSelectedSerialPort();
            if (selectedPort == null)
            {
                MessageBox.Show("Escribe o selecciona un puerto COM válido.", "USB-CDC", MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }

            if (!usbcdcManager!.Connect(selectedPort, 115200))
            {
                MessageBox.Show($"Error al conectar al puerto {selectedPort}.", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
                return;
            }

            if (!StartSteamVRForUSBCDC())
            {
                usbcdcManager.Disconnect();
                return;
            }

            pipeManager ??= new PipeManager();

            btnStartUSBCDC.IsEnabled = false;
            btnStopUSBCDC.IsEnabled = true;
            usbcdcComboBox.IsEnabled = false;
            hmdScreenComboBox.IsEnabled = false;
            hmsScreenModeComboBox.IsEnabled = false;
            txtStatusBar.Text = $"USB-CDC activo • SteamVR en pantalla seleccionada • Datos fluyendo...";
            usbcdcDrivingActive = true;
        }

        private void btnStopUSBCDC_Click(object sender, RoutedEventArgs e)
        {
            StopSteamVRForUSBCDC();
            usbcdcManager?.Disconnect();

            pipeManager?.Dispose();
            pipeManager = null;

            btnStartUSBCDC.IsEnabled = true;
            btnStopUSBCDC.IsEnabled = false;
            usbcdcComboBox.IsEnabled = true;
            hmdScreenComboBox.IsEnabled = true;
            hmsScreenModeComboBox.IsEnabled = true;
            txtStatusBar.Text = "USB-CDC desconectado";
            usbcdcDrivingActive = false;
        }

        private bool StartSteamVRForUSBCDC()
        {
            try
            {
                if (hmdScreenComboBox.SelectedIndex < 0)
                {
                    MessageBox.Show(
                        "Selecciona una pantalla válida para SteamVR.",
                        "SteamVR",
                        MessageBoxButton.OK,
                        MessageBoxImage.Warning);

                    return false;
                }

                var screens = System.Windows.Forms.Screen.AllScreens;

                if (hmdScreenComboBox.SelectedIndex >= screens.Length)
                {
                    MessageBox.Show(
                        "La pantalla seleccionada ya no está disponible.",
                        "SteamVR",
                        MessageBoxButton.OK,
                        MessageBoxImage.Warning);

                    return false;
                }

                var selectedScreen =
                    screens[hmdScreenComboBox.SelectedIndex];

                int modeIndex = hmsScreenModeComboBox.SelectedIndex;

                bool requestedDirectMode = modeIndex == 1;
                bool debugMode = modeIndex == 2;

                if (!ConfigureDisplaySettingsForSteamVR(
                        selectedScreen,
                        requestedDirectMode,
                        debugMode,
                        out bool useDirectMode))
                {
                    return false;
                }

                System.Diagnostics.Process.Start(
                    new System.Diagnostics.ProcessStartInfo
                    {
                        FileName = "steam://rungameid/250820",
                        UseShellExecute = true
                    });

                pipeManager ??= new PipeManager();

                usbcdcDrivingActive = true;

                string modeText = useDirectMode
                    ? "Direct Mode"
                    : "Modo ventana";

                MessageBox.Show(
                    $"SteamVR iniciado en {modeText}.\n" +
                    "Flujo USB-CDC → Pipe → SteamVR activo.",
                    "USB-CDC + SteamVR",
                    MessageBoxButton.OK,
                    MessageBoxImage.Information);

                return true;
            }
            catch (Exception ex)
            {
                MessageBox.Show(
                    $"Error al iniciar SteamVR: {ex.Message}",
                    "Error",
                    MessageBoxButton.OK,
                    MessageBoxImage.Error);

                return false;
            }
        }

        private void StopSteamVRForUSBCDC()
        {
            usbcdcDrivingActive = false;

            if (pipeManager != null)
            {
                pipeManager.Dispose();
                pipeManager = null;
            }

            CloseSteamVR();
            Debug.WriteLine("SteamVR detenido");
        }

        private void UsbcdcManager_DataReceived(object? sender, AllData data)
        {
            if (usbcdcDrivingActive && pipeManager != null)
            {
                var packet = new PipeManager.FullSystemPacket
                {
                    hmdPosX = 0.0f,
                    hmdPosY = 1.70f,
                    hmdPosZ = 0.0f,
                    hmd_qw = data.hmd_qW,
                    hmd_qx = data.hmd_qX,
                    hmd_qy = data.hmd_qY,
                    hmd_qz = data.hmd_qZ,
                    hmd_error = data.hmd_error,

                    lPosX = -0.2f,
                    lPosY = 1.2f,
                    lPosZ = -0.4f,
                    left_qw = data.left_qW,
                    left_qx = data.left_qX,
                    left_qy = data.left_qY,
                    left_qz = data.left_qZ,
                    left_a = data.left_a,
                    left_b = data.left_b,
                    left_system = data.left_system,
                    left_grip = data.left_grip,
                    left_trigger = data.left_trigger,
                    left_stick_click = data.left_stick_click,
                    left_stick_x = data.left_stick_x,
                    left_stick_y = data.left_stick_y,
                    left_status = data.left_status,
                    left_error = data.left_error,

                    rPosX = 0.2f,
                    rPosY = 1.2f,
                    rPosZ = -0.4f,
                    right_qw = data.right_qW,
                    right_qx = data.right_qX,
                    right_qy = data.right_qY,
                    right_qz = data.right_qZ,
                    right_a = data.right_a,
                    right_b = data.right_b,
                    right_system = data.right_system,
                    right_grip = data.right_grip,
                    right_trigger = data.right_trigger,
                    right_stick_click = data.right_stick_click,
                    right_stick_x = data.right_stick_x,
                    right_stick_y = data.right_stick_y,
                    right_status = data.right_status,
                    right_error = data.right_error
                };

                pipeManager.WriteData(packet);
            }

            Dispatcher.BeginInvoke(() => UpdateUIFromAllData(data));
        }

        private void UpdateUIFromAllData(AllData data)
        {
            var hmdQuat = SensorDataProcessor.NormalizeQuaternion(
                data.hmd_qW, data.hmd_qX, data.hmd_qY, data.hmd_qZ);
            var hmdEuler = SensorDataProcessor.QuaternionToEuler(
                hmdQuat.W, hmdQuat.X, hmdQuat.Y, hmdQuat.Z);

            VRHeadset_connection.Content = "Online";
            VRHeadsetCircle.Fill = new SolidColorBrush(Color.FromRgb(34, 201, 147));
            VRRotationPitch.Content = hmdEuler.pitch.ToString("F2");
            VRRotationYaw.Content = hmdEuler.yaw.ToString("F2");
            VRRotationRoll.Content = hmdEuler.roll.ToString("F2");

            var leftQuat = SensorDataProcessor.NormalizeQuaternion(
                data.left_qW, data.left_qX, data.left_qY, data.left_qZ);
            var leftEuler = SensorDataProcessor.QuaternionToEuler(
                leftQuat.W, leftQuat.X, leftQuat.Y, leftQuat.Z);
            leftRotationPitch.Content = leftEuler.pitch.ToString("F2");
            leftRotationYaw.Content = leftEuler.yaw.ToString("F2");
            leftRotationRoll.Content = leftEuler.roll.ToString("F2");
            leftController_connection.Content = data.left_status ? "Online" : "Offline";
            leftcontrollCircle.Fill = data.left_status
                ? new SolidColorBrush(Color.FromRgb(34, 201, 147))
                : new SolidColorBrush(Color.FromRgb(222, 46, 46));

            var rightQuat = SensorDataProcessor.NormalizeQuaternion(
                data.right_qW, data.right_qX, data.right_qY, data.right_qZ);
            var rightEuler = SensorDataProcessor.QuaternionToEuler(
                rightQuat.W, rightQuat.X, rightQuat.Y, rightQuat.Z);
            rightRotationPitch.Content = rightEuler.pitch.ToString("F2");
            rightRotationYaw.Content = rightEuler.yaw.ToString("F2");
            rightRotationRoll.Content = rightEuler.roll.ToString("F2");
            rightController_connection.Content = data.right_status ? "Online" : "Offline";
            rightcontrollCircle.Fill = data.right_status
                ? new SolidColorBrush(Color.FromRgb(34, 201, 147))
                : new SolidColorBrush(Color.FromRgb(222, 46, 46));
        }

        private static (bool a, bool b, bool trigger, bool grip, bool stickClick, bool system) DecodeButtons(uint buttons)
        {
            return (
                a: (buttons & (1u << 0)) != 0,
                b: (buttons & (1u << 1)) != 0,
                trigger: (buttons & (1u << 2)) != 0,
                grip: (buttons & (1u << 3)) != 0,
                stickClick: (buttons & (1u << 4)) != 0,
                system: (buttons & (1u << 5)) != 0
            );
        }

        private void UsbcdcManager_ErrorOccurred(object? sender, string errorMessage)
        {
            Dispatcher.Invoke(() =>
            {
                MessageBox.Show($"Error USB-CDC: {errorMessage}", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
                Debug.WriteLine($"Error USB-CDC: {errorMessage}");
            });
        }

        private void UsbcdcManager_ConnectionStatusChanged(object? sender, EventArgs e)
        {
            Dispatcher.Invoke(() =>
            {
                bool connected = usbcdcManager?.IsConnected == true;

                txtStatusBar.Text = connected
                    ? "USB-CDC: Conectado"
                    : "USB-CDC: Desconectado";

                if (usbcdcStatusText != null)
                {
                    usbcdcStatusText.Text = connected ? "Conectado" : "Desconectado";
                    usbcdcStatusText.Foreground = connected
                        ? new SolidColorBrush(Color.FromRgb(34, 201, 147))
                        : new SolidColorBrush(Color.FromRgb(222, 46, 46));
                }
            });
        }

        // ==================== NAVEGACIÓN PRINCIPAL ====================

        public void SelectNavSection(string section)
        {
            if (mainTabs == null)
                return;

            switch (section.ToLowerInvariant())
            {
                case "main":
                case "inicio":
                    mainTabs.SelectedIndex = 0;
                    break;

                case "driver":
                    mainTabs.SelectedIndex = 1;
                    break;

                case "config":
                    mainTabs.SelectedIndex = 2;
                    break;
            }
        }

        // ==================== ESTADO DE CONEXIÓN ====================

        private void InitializeConnectionStatus()
        {
            leftController_connection.Content = "Offline";
            rightController_connection.Content = "Offline";
            leftcontrollCircle.Fill = new SolidColorBrush(Color.FromRgb(222, 46, 46));
            rightcontrollCircle.Fill = new SolidColorBrush(Color.FromRgb(222, 46, 46));

            VRHeadset_connection.Content = "Offline";
            VRHeadsetCircle.Fill = new SolidColorBrush(Color.FromRgb(240, 82, 82));
        }

        private void LoadAvailableScreens()
        {
            hmdScreenComboBox.Items.Clear();
            foreach (var screen in System.Windows.Forms.Screen.AllScreens)
            {
                hmdScreenComboBox.Items.Add($"{screen.DeviceName} ({screen.Bounds.Width}x{screen.Bounds.Height})");
            }
            if (hmdScreenComboBox.Items.Count > 0)
                hmdScreenComboBox.SelectedIndex = 0;
        }

        private void InitializeControllerData()
        {
            leftPositionX.Content = "--"; leftPositionY.Content = "--"; leftPositionZ.Content = "--";
            leftRotationPitch.Content = "--"; leftRotationYaw.Content = "--"; leftRotationRoll.Content = "--";

            rightPositionX.Content = "--"; rightPositionY.Content = "--"; rightPositionZ.Content = "--";
            rightRotationPitch.Content = "--"; rightRotationYaw.Content = "--"; rightRotationRoll.Content = "--";

            VRPositionX.Content = "--"; VRPositionY.Content = "--"; VRPositionZ.Content = "--";
            VRRotationPitch.Content = "--"; VRRotationYaw.Content = "--"; VRRotationRoll.Content = "--";
        }

        // ==================== DRIVER TEST ====================

        private void MenuStartDriverTest_Click(object sender, RoutedEventArgs e) => StartDriverTest();
        private void MenuStopDriverTest_Click(object sender, RoutedEventArgs e) => StopDriverTest();
        private void MenuResetDriverTest_Click(object sender, RoutedEventArgs e) => ResetDriverTest();

        private void StartDriverTest()
        {
            try
            {
                if (hmdScreenComboBox.SelectedIndex < 0)
                {
                    MessageBox.Show(
                        "Selecciona una pantalla válida.",
                        "Driver Test",
                        MessageBoxButton.OK,
                        MessageBoxImage.Warning);

                    return;
                }

                var screens = System.Windows.Forms.Screen.AllScreens;

                if (hmdScreenComboBox.SelectedIndex >= screens.Length)
                {
                    MessageBox.Show(
                        "La pantalla seleccionada ya no está disponible.",
                        "Driver Test",
                        MessageBoxButton.OK,
                        MessageBoxImage.Warning);

                    return;
                }

                var selectedScreen =
                    screens[hmdScreenComboBox.SelectedIndex];

                int modeIndex = hmsScreenModeComboBox.SelectedIndex;

                bool requestedDirectMode = modeIndex == 1;
                bool debugMode = modeIndex == 2;

                if (!ConfigureDisplaySettingsForSteamVR(
                        selectedScreen,
                        requestedDirectMode,
                        debugMode,
                        out bool useDirectMode))
                {
                    return;
                }

                System.Diagnostics.Process.Start(
                    new System.Diagnostics.ProcessStartInfo
                    {
                        FileName = "steam://rungameid/250820",
                        UseShellExecute = true
                    });

                pipeManager?.Dispose();
                pipeManager = new PipeManager();

                driverTestActive = true;
                InitializeSliders();

                string modeText = useDirectMode
                    ? "Direct Mode"
                    : "Modo ventana";

                MessageBox.Show(
                    $"Driver Test iniciado en {modeText}.\n" +
                    "Conectando a SteamVR...",
                    "Driver Test",
                    MessageBoxButton.OK,
                    MessageBoxImage.Information);
            }
            catch (Exception ex)
            {
                MessageBox.Show(
                    $"Error al iniciar Driver Test: {ex.Message}",
                    "Error",
                    MessageBoxButton.OK,
                    MessageBoxImage.Error);
            }
        }

        private void StopDriverTest()
        {
            driverTestActive = false;

            if (pipeManager != null)
            {
                pipeManager.Dispose();
                pipeManager = null;
            }

            CloseSteamVR();
            Debug.WriteLine("Named Pipe detenido y recursos liberados.");
        }

        private async void ResetDriverTest()
        {
            if (!driverTestActive) return;

            leftXSlider.Value = -0.2; leftYSlider.Value = 1.2; leftZSlider.Value = -0.4;
            leftPitchSlider.Value = 0; leftYawSlider.Value = 0; leftRollSlider.Value = 0;
            leftStickX.Value = 0; leftStickY.Value = 0;

            rightXSlider.Value = 0.2; rightYSlider.Value = 1.2; rightZSlider.Value = -0.4;
            rightPitchSlider.Value = 0; rightYawSlider.Value = 0; rightRollSlider.Value = 0;
            rightStickX.Value = 0; rightStickY.Value = 0;

            leftButtons = 0;
            rightButtons = 0;

            StopDriverTest();
            await Task.Delay(2000);
            StartDriverTest();
        }

        private void InitializeSliders()
        {
            leftXSlider.Value = -0.2;
            leftYSlider.Value = 1.2;
            leftZSlider.Value = -0.4;
            leftPitchSlider.Value = 27;
            leftYawSlider.Value = 7;
            leftRollSlider.Value = 10;
            leftStickX.Value = 0;
            leftStickY.Value = 0;

            rightXSlider.Value = 0.2;
            rightYSlider.Value = 1.2;
            rightZSlider.Value = -0.4;
            rightPitchSlider.Value = 27;
            rightYawSlider.Value = 7;
            rightRollSlider.Value = 10;
            rightStickX.Value = 0;
            rightStickY.Value = 0;

            hmdXSlider.Value = 0;
            hmdYSlider.Value = 1.70;
            hmdZSlider.Value = 0;
            hmdPitchSlider.Value = 0;
            hmdYawSlider.Value = 0;
            hmdRollSlider.Value = 0;
        }

        private void SyncAllDataToDriver()
        {
            if (pipeManager == null || !driverTestActive) return;

            var hmdRot = EulerToQuaternion(hmdPitchSlider.Value, hmdYawSlider.Value, hmdRollSlider.Value);
            var leftRot = EulerToQuaternion(leftPitchSlider.Value, leftYawSlider.Value, leftRollSlider.Value);
            var rightRot = EulerToQuaternion(rightPitchSlider.Value, rightYawSlider.Value, rightRollSlider.Value);

            var leftBtn = DecodeButtons(leftButtons);
            var rightBtn = DecodeButtons(rightButtons);

            var packet = new PipeManager.FullSystemPacket
            {
                hmdPosX = (float)hmdXSlider.Value,
                hmdPosY = (float)hmdYSlider.Value,
                hmdPosZ = (float)hmdZSlider.Value,
                hmd_qw = hmdRot.W,
                hmd_qx = hmdRot.X,
                hmd_qy = hmdRot.Y,
                hmd_qz = hmdRot.Z,
                hmd_error = 0f,

                lPosX = (float)leftXSlider.Value,
                lPosY = (float)leftYSlider.Value,
                lPosZ = (float)leftZSlider.Value,
                left_qw = leftRot.W,
                left_qx = leftRot.X,
                left_qy = leftRot.Y,
                left_qz = leftRot.Z,
                left_a = leftBtn.a,
                left_b = leftBtn.b,
                left_system = leftBtn.system,
                left_grip = leftBtn.grip,
                left_trigger = leftBtn.trigger,
                left_stick_click = leftBtn.stickClick,
                left_stick_x = (float)leftStickX.Value,
                left_stick_y = (float)leftStickY.Value,
                left_status = true,
                left_error = 0f,

                rPosX = (float)rightXSlider.Value,
                rPosY = (float)rightYSlider.Value,
                rPosZ = (float)rightZSlider.Value,
                right_qw = rightRot.W,
                right_qx = rightRot.X,
                right_qy = rightRot.Y,
                right_qz = rightRot.Z,
                right_a = rightBtn.a,
                right_b = rightBtn.b,
                right_system = rightBtn.system,
                right_grip = rightBtn.grip,
                right_trigger = rightBtn.trigger,
                right_stick_click = rightBtn.stickClick,
                right_stick_x = (float)rightStickX.Value,
                right_stick_y = (float)rightStickY.Value,
                right_status = true,
                right_error = 0f
            };

            pipeManager.WriteData(packet);
        }

        private PipeManager.Quaternion EulerToQuaternion(double pitch, double yaw, double roll)
        {
            double p = pitch * Math.PI / 180.0;
            double y = yaw * Math.PI / 180.0;
            double r = roll * Math.PI / 180.0;

            double cp = Math.Cos(p / 2), sp = Math.Sin(p / 2);
            double cy = Math.Cos(y / 2), sy = Math.Sin(y / 2);
            double cr = Math.Cos(r / 2), sr = Math.Sin(r / 2);

            return new PipeManager.Quaternion
            {
                W = (float)(cp * cy * cr + sp * sy * sr),
                X = (float)(sp * cy * cr - cp * sy * sr),
                Y = (float)(cp * sy * cr + sp * cy * sr),
                Z = (float)(cp * cy * sr - sp * sy * cr)
            };
        }

        private void leftXSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        { leftXLabel.Content = leftXSlider.Value.ToString("F2"); SyncAllDataToDriver(); }
        private void leftYSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        { leftYLabel.Content = leftYSlider.Value.ToString("F2"); SyncAllDataToDriver(); }
        private void leftZSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        { leftZLabel.Content = leftZSlider.Value.ToString("F2"); SyncAllDataToDriver(); }
        private void leftPitchSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        { leftPitchLabel.Content = leftPitchSlider.Value.ToString("F0"); SyncAllDataToDriver(); }
        private void leftYawSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        { leftYawLabel.Content = leftYawSlider.Value.ToString("F0"); SyncAllDataToDriver(); }
        private void leftRollSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        { leftRollLabel.Content = leftRollSlider.Value.ToString("F0"); SyncAllDataToDriver(); }
        private void leftStickX_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        { leftStickXLabel.Content = leftStickX.Value.ToString("F2"); SyncAllDataToDriver(); }
        private void leftStickY_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        { leftStickYLabel.Content = leftStickY.Value.ToString("F2"); SyncAllDataToDriver(); }

        private void X_Button_Down(object sender, System.Windows.Input.MouseButtonEventArgs e) => PressButton(ref leftButtons, 0);
        private void X_Button_Up(object sender, System.Windows.Input.MouseEventArgs e) => ReleaseButton(ref leftButtons, 0);

        private void Y_Button_Down(object sender, System.Windows.Input.MouseButtonEventArgs e) => PressButton(ref leftButtons, 1);
        private void Y_Button_Up(object sender, System.Windows.Input.MouseEventArgs e) => ReleaseButton(ref leftButtons, 1);

        private void leftTriggerButton_Down(object sender, System.Windows.Input.MouseButtonEventArgs e) => PressButton(ref leftButtons, 2);
        private void leftTriggerButton_Up(object sender, System.Windows.Input.MouseEventArgs e) => ReleaseButton(ref leftButtons, 2);

        private void leftGripButton_Down(object sender, System.Windows.Input.MouseButtonEventArgs e) => PressButton(ref leftButtons, 3);
        private void leftGripButton_Up(object sender, System.Windows.Input.MouseEventArgs e) => ReleaseButton(ref leftButtons, 3);

        private void leftStickButton_Down(object sender, System.Windows.Input.MouseButtonEventArgs e) => PressButton(ref leftButtons, 4);
        private void leftStickButton_Up(object sender, System.Windows.Input.MouseEventArgs e) => ReleaseButton(ref leftButtons, 4);

        private void leftSystemButton_Down(object sender, System.Windows.Input.MouseButtonEventArgs e) => PressButton(ref leftButtons, 5);
        private void leftSystemButton_Up(object sender, System.Windows.Input.MouseEventArgs e) => ReleaseButton(ref leftButtons, 5);

        private void rightXSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        { rightXLabel.Content = rightXSlider.Value.ToString("F2"); SyncAllDataToDriver(); }
        private void rightYSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        { rightYLabel.Content = rightYSlider.Value.ToString("F2"); SyncAllDataToDriver(); }
        private void rightZSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        { rightZLabel.Content = rightZSlider.Value.ToString("F2"); SyncAllDataToDriver(); }
        private void rightPitchSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        { rightPitchLabel.Content = rightPitchSlider.Value.ToString("F0"); SyncAllDataToDriver(); }
        private void rightYawSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        { rightYawLabel.Content = rightYawSlider.Value.ToString("F0"); SyncAllDataToDriver(); }
        private void rightRollSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        { rightRollLabel.Content = rightRollSlider.Value.ToString("F0"); SyncAllDataToDriver(); }
        private void rightStickX_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        { rightStickXLabel.Content = rightStickX.Value.ToString("F2"); SyncAllDataToDriver(); }
        private void rightStickY_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        { rightStickYLabel.Content = rightStickY.Value.ToString("F2"); SyncAllDataToDriver(); }

        private void A_Button_Down(object sender, System.Windows.Input.MouseButtonEventArgs e) => PressButton(ref rightButtons, 0);
        private void A_Button_Up(object sender, System.Windows.Input.MouseEventArgs e) => ReleaseButton(ref rightButtons, 0);

        private void B_Button_Down(object sender, System.Windows.Input.MouseButtonEventArgs e) => PressButton(ref rightButtons, 1);
        private void B_Button_Up(object sender, System.Windows.Input.MouseEventArgs e) => ReleaseButton(ref rightButtons, 1);

        private void rightTriggerButton_Down(object sender, System.Windows.Input.MouseButtonEventArgs e) => PressButton(ref rightButtons, 2);
        private void rightTriggerButton_Up(object sender, System.Windows.Input.MouseEventArgs e) => ReleaseButton(ref rightButtons, 2);

        private void rightGripButton_Down(object sender, System.Windows.Input.MouseButtonEventArgs e) => PressButton(ref rightButtons, 3);
        private void rightGripButton_Up(object sender, System.Windows.Input.MouseEventArgs e) => ReleaseButton(ref rightButtons, 3);

        private void rightStickButton_Down(object sender, System.Windows.Input.MouseButtonEventArgs e) => PressButton(ref rightButtons, 4);
        private void rightStickButton_Up(object sender, System.Windows.Input.MouseEventArgs e) => ReleaseButton(ref rightButtons, 4);

        private void rightSystemButton_Down(object sender, System.Windows.Input.MouseButtonEventArgs e) => PressButton(ref rightButtons, 5);
        private void rightSystemButton_Up(object sender, System.Windows.Input.MouseEventArgs e) => ReleaseButton(ref rightButtons, 5);

        private void hmdXSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        { hmdXLabel.Content = e.NewValue.ToString("F2"); SyncAllDataToDriver(); }

        private void hmdYSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        { hmdYLabel.Content = e.NewValue.ToString("F2"); SyncAllDataToDriver(); }

        private void hmdZSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        { hmdZLabel.Content = e.NewValue.ToString("F2"); SyncAllDataToDriver(); }

        private void hmdPitchSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        { hmdPitchLabel.Content = e.NewValue.ToString("F2"); SyncAllDataToDriver(); }

        private void hmdYawSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        { hmdYawLabel.Content = e.NewValue.ToString("F2"); SyncAllDataToDriver(); }

        private void hmdRollSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        { hmdRollLabel.Content = e.NewValue.ToString("F2"); SyncAllDataToDriver(); }

        private void PressButton(ref uint buttonState, int bit)
        {
            if (!driverTestActive) return;
            buttonState |= (uint)(1 << bit);
            SyncAllDataToDriver();
        }

        private void ReleaseButton(ref uint buttonState, int bit)
        {
            if (!driverTestActive) return;
            buttonState &= ~(uint)(1 << bit);
            SyncAllDataToDriver();
        }

        private void Window_Closing(object sender, System.ComponentModel.CancelEventArgs e)
        {
            driverTestActive = false;
            usbcdcDrivingActive = false;

            pipeManager?.Dispose();
            pipeManager = null;
            usbcdcManager?.Disconnect();

            try
            {
                StopPhoneStream();
                phoneStreamController?.Dispose();
                phoneStreamController = null;
            }
            catch { }
        }

        private void CloseSteamVR()
        {
            try
            {
                Process.Start(new ProcessStartInfo
                {
                    FileName = "steam://closedashboard",
                    UseShellExecute = true,
                    WindowStyle = ProcessWindowStyle.Hidden
                });

                System.Threading.Thread.Sleep(1000);
                CloseSpecificVRProcesses();
                Debug.WriteLine("SteamVR cerrado exitosamente.");
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"Error: {ex.Message}");
                CloseSpecificVRProcesses();
            }
        }

        private void CloseSpecificVRProcesses()
        {
            var vrProcesses = new Dictionary<string, bool>
            {
                { "vrserver",     true  },
                { "vrcompositor", false },
                { "vrmonitor",    true  },
                { "vrdashboard",  false },
                { "vrwebhelper",  false },
                { "vrstartup",    false }
            };

            foreach (var proc in vrProcesses)
            {
                try
                {
                    var processes = Process.GetProcessesByName(proc.Key);
                    foreach (var process in processes)
                    {
                        if (proc.Value)
                        {
                            if (!process.CloseMainWindow())
                            {
                                System.Threading.Thread.Sleep(1000);
                                if (!process.HasExited) process.Kill();
                            }
                            process.WaitForExit(2000);
                        }
                        else
                        {
                            process.Kill();
                            process.WaitForExit(1000);
                        }
                        Debug.WriteLine($"Proceso VR cerrado: {proc.Key}");
                        process.Dispose();
                    }
                }
                catch { }
            }
        }

        private void sliderDisplayFreq_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e) { if (lblDisplayFreq == null) return; lblDisplayFreq.Text = sliderDisplayFreq.Value.ToString("F0"); UpdateSettingsLabels(); }
        private void sliderIpd_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e) { if (lblIpd == null) return; lblIpd.Text = sliderIpd.Value.ToString("F3"); UpdateSettingsLabels(); }
        private void sliderFovLeft_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e) { if (lblFovLeft == null) return; lblFovLeft.Text = sliderFovLeft.Value.ToString("F0"); UpdateSettingsLabels(); }
        private void sliderFovRight_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e) { if (lblFovRight == null) return; lblFovRight.Text = sliderFovRight.Value.ToString("F0"); UpdateSettingsLabels(); }
        private void sliderFovTop_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e) { if (lblFovTop == null) return; lblFovTop.Text = sliderFovTop.Value.ToString("F0"); UpdateSettingsLabels(); }
        private void sliderFovBottom_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e) { if (lblFovBottom == null) return; lblFovBottom.Text = sliderFovBottom.Value.ToString("F0"); UpdateSettingsLabels(); }
        private void sliderTrackingScale_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e) { if (lblTrackingScale == null) return; lblTrackingScale.Text = sliderTrackingScale.Value.ToString("F1"); UpdateSettingsLabels(); }
        private void sliderLeftHaptic_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e) { if (lblLeftHaptic == null) return; lblLeftHaptic.Text = sliderLeftHaptic.Value.ToString("F1"); UpdateSettingsLabels(); }
        private void sliderRightHaptic_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e) { if (lblRightHaptic == null) return; lblRightHaptic.Text = sliderRightHaptic.Value.ToString("F1"); UpdateSettingsLabels(); }
    
        private bool ConfigureDisplaySettingsForSteamVR(System.Windows.Forms.Screen selectedScreen, bool requestedDirectMode, bool debugMode, out bool useDirectMode) {
            useDirectMode = false;
            if (selectedScreen == null) {
                MessageBox.Show("No se seleccionó una pantalla válida.", "PixelVR", MessageBoxButton.OK, MessageBoxImage.Warning);
                return false;
            }

            var monitorIds = MonitorHardwareID.GetIdsFromDeviceName(selectedScreen.DeviceName);

            if (requestedDirectMode) {
                if (!monitorIds.IsValid)
                {
                    /*
                     * Nunca reutilizar un VID/PID antiguo de otro monitor.
                    * En este caso se fuerza el modo ventana.
                    */
                    MessageBox.Show(
                        "No se pudo obtener un VID/PID válido para la pantalla seleccionada.\n\n" +
                        "El modo directo requiere un monitor físico correctamente identificado. " +
                        "Se utilizará modo ventana.",
                        "Direct Mode no disponible",
                        MessageBoxButton.OK,
                        MessageBoxImage.Warning);
                    useDirectMode = false;
                }
                else
                {
                    useDirectMode = true;

                    Debug.WriteLine(
                        $"Direct Mode: monitor={selectedScreen.DeviceName}, " +
                        $"VID=0x{monitorIds.VendorID:X4}, " +
                        $"PID=0x{monitorIds.ProductID:X4}");
                }
            }

            DriverSettings settings =
                settingsManager?.LoadSettings()
                ?? new DriverSettings();

            settings.WindowX = selectedScreen.Bounds.X;
            settings.WindowY = selectedScreen.Bounds.Y;
            settings.WindowWidth = selectedScreen.Bounds.Width;
            settings.WindowHeight = selectedScreen.Bounds.Height;

            /*
            * El tamaño recomendado puede ser menor que la pantalla física,
            * pero conservar el tamaño de la pantalla seleccionada es válido
            * para la configuración actual del driver.
            */
            settings.RenderWidth = selectedScreen.Bounds.Width;
            settings.RenderHeight = selectedScreen.Bounds.Height;

            settings.DebugMode = debugMode;

            settings.DirectMode = useDirectMode;

            /*
            * En modo ventana se limpian expresamente los valores EDID.
            * Así no se reutiliza un EDID de una ejecución anterior.
            */
            settings.EdidVid = useDirectMode
                ? monitorIds.VendorID
                : 0;
    
            settings.EdidPid = useDirectMode
                ? monitorIds.ProductID
                : 0;

            settingsManager ??=
                new DriverSettingsManager(DriverSettingsPath);

            settingsManager.SaveSettings(settings);

            Debug.WriteLine(
                $"PixelVR display config: " +
                $"mode={(useDirectMode ? "direct" : "desktop")}, " +
                $"screen={selectedScreen.DeviceName}, " +
                $"bounds=({selectedScreen.Bounds.X}," +
                $"{selectedScreen.Bounds.Y} " +
                $"{selectedScreen.Bounds.Width}x" +
                $"{selectedScreen.Bounds.Height}), " +
                $"VID=0x{settings.EdidVid:X4}, " +
                $"PID=0x{settings.EdidPid:X4}");

            return true;
        }

        private void PhoneStreamController_StatusChanged(object? sender, string message)
        {
            // Actualiza la UI solo si el modo teléfono está activo
            if (!phoneStreamModeActive)
                return;

            txtPhoneStreamStatus.Text = $"Estado: {message}";
            txtStatusBar.Text = $"Teléfono VR: {message}";
        }

        private void StartPhoneStream(
            string phoneIp,
            int videoPort = 5000,
            int controlPort = 6000,
            int width = 1920,
            int height = 1080,
            int fps = 60,
            int bitrateKbps = 10000,
            int gopSeconds = 2)
        {
            if (phoneStreamController == null)
            {
                MessageBox.Show(
                    "El controlador del teléfono no está inicializado.",
                    "PixelVR",
                    MessageBoxButton.OK,
                    MessageBoxImage.Error);
                return;
            }

            if (string.IsNullOrWhiteSpace(phoneIp))
            {
                MessageBox.Show(
                    "Introduce la dirección IP del teléfono.",
                    "Stream al teléfono",
                    MessageBoxButton.OK,
                    MessageBoxImage.Warning);
                return;
            }

            try
            {
                bool started = phoneStreamController.Start(
                    phoneIp,
                    videoPort,
                    controlPort,
                    width,
                    height,
                    fps,
                    bitrateKbps,
                    gopSeconds);

                if (!started)
                {
                    MessageBox.Show(
                        "No se pudo iniciar el stream AMF/OpenVR. Revisa logs y que PixelVRStream.dll esté presente.",
                        "Stream al teléfono",
                        MessageBoxButton.OK,
                        MessageBoxImage.Error);
                    return;
                }

                phoneStreamModeActive = true;
                txtPhoneStreamStatus.Text = $"Estado: Activo ({width}x{height}@{fps})";
                btnStartPhoneStream.IsEnabled = false;
                btnStopPhoneStream.IsEnabled = true;
                txtPhoneIp.IsEnabled = false;
            }
            catch (DllNotFoundException)
            {
                MessageBox.Show(
                    "No se encontró PixelVRStream.dll. Copia la DLL junto al ejecutable de PixelVR Connect (x64).",
                    "PixelVRStream.dll",
                    MessageBoxButton.OK,
                    MessageBoxImage.Error);
            }
            catch (BadImageFormatException)
            {
                MessageBox.Show(
                    "PixelVRStream.dll no coincide con la arquitectura de la aplicación. Compila todo en x64.",
                    "Arquitectura incorrecta",
                    MessageBoxButton.OK,
                    MessageBoxImage.Error);
            }
            catch (Exception ex)
            {
                MessageBox.Show(ex.Message, "Error iniciando el stream", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void StopPhoneStream()
        {
            if (phoneStreamController == null)
                return;

            phoneStreamController.Stop();
            phoneStreamModeActive = false;
            txtPhoneStreamStatus.Text = "Estado: Inactivo";
            btnStartPhoneStream.IsEnabled = true;
            btnStopPhoneStream.IsEnabled = false;
            txtPhoneIp.IsEnabled = true;
        }

        private void btnStartPhoneStream_Click(object sender, RoutedEventArgs e)
        {
            string ip = txtPhoneIp.Text.Trim();
            int bitrate = (int)sldBitrate.Value;

            // resolución
            int width = 1920, height = 1080;
            switch (cmbResolution.SelectedIndex)
            {
                case 1: width = 1280; height = 720; break;
                case 2: width = 960; height = 540; break;
            }

            int fps = 60;
            if (cmbFps.SelectedIndex == 1) fps = 45;
            else if (cmbFps.SelectedIndex == 2) fps = 30;

            StartPhoneStream(phoneIp: ip,
                             videoPort: 5000,
                             controlPort: 6000,
                             width: width,
                             height: height,
                             fps: fps,
                             bitrateKbps: bitrate,
                             gopSeconds: 2);
            lblBitrate.Text = bitrate.ToString();
        }

        private void btnStopPhoneStream_Click(object sender, RoutedEventArgs e)
        {
            StopPhoneStream();
        }

        private void btnRequestKeyframe_Click(object sender, RoutedEventArgs e)
        {
            phoneStreamController?.RequestKeyframe();
        }

        private void btnOpenTetheringHelp_Click(object sender, RoutedEventArgs e)
        {
            MessageBox.Show("Activa 'USB tethering' en Ajustes > Red e Internet > Zona con cobertura inalámbrica y tethering.\n\n" +
                            "Conecta el teléfono por USB y anota la IP (ej. 192.168.42.2).", "Ayuda USB tethering", MessageBoxButton.OK, MessageBoxImage.Information);
        }
    }

    public sealed class MonitorHardwareID
    {
        public ushort VendorID { get; init; }
        public ushort ProductID { get; init; }

        public bool IsValid =>
            VendorID != 0 &&
            ProductID != 0;

        private const int DisplayDeviceAttachedToDesktop = 0x00000001;

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct DISPLAY_DEVICE
        {
            public int cb;

            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
            public string DeviceName;

            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
            public string DeviceString;

            public int StateFlags;

            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
            public string DeviceID;

            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
            public string DeviceKey;
        }

        [DllImport(
            "user32.dll",
            CharSet = CharSet.Unicode,
            SetLastError = true)]
        private static extern bool EnumDisplayDevices(
            string? lpDevice,
            uint iDevNum,
            ref DISPLAY_DEVICE lpDisplayDevice,
            uint dwFlags);

        public static MonitorHardwareID GetIdsFromDeviceName(
            string deviceName)
        {
            if (string.IsNullOrWhiteSpace(deviceName))
                return Empty();

            string normalizedName =
                deviceName.Trim().ToUpperInvariant();

            for (uint index = 0; ; index++)
            {
                var displayDevice = new DISPLAY_DEVICE
                {
                    cb = Marshal.SizeOf<DISPLAY_DEVICE>()
                };

                if (!EnumDisplayDevices(
                        null,
                        index,
                        ref displayDevice,
                        0))
                {
                    break;
                }

                if ((displayDevice.StateFlags &
                     DisplayDeviceAttachedToDesktop) == 0)
                {
                    continue;
                }

                if (!string.Equals(
                        displayDevice.DeviceName?.Trim(),
                        normalizedName,
                        StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }

                // Enumerar el monitor físico asociado a DISPLAY1,
                // DISPLAY2, etc.
                for (uint monitorIndex = 0; ; monitorIndex++)
                {
                    var monitorDevice = new DISPLAY_DEVICE
                    {
                        cb = Marshal.SizeOf<DISPLAY_DEVICE>()
                    };

                    if (!EnumDisplayDevices(
                            displayDevice.DeviceName,
                            monitorIndex,
                            ref monitorDevice,
                            0))
                    {
                        break;
                    }

                    if ((monitorDevice.StateFlags &
                         DisplayDeviceAttachedToDesktop) == 0)
                    {
                        continue;
                    }

                    var ids = ParseDeviceId(monitorDevice.DeviceID);

                    if (ids.IsValid)
                        return ids;
                }

                break;
            }

            return Empty();
        }

        private static MonitorHardwareID ParseDeviceId(
            string? deviceId)
        {
            if (string.IsNullOrWhiteSpace(deviceId))
                return Empty();

            /*
             * Ejemplo de DeviceID:
             *
             * DISPLAY\DEL4098\5&10A589A&0&UID4352
             *
             * El bloque DEL4098 contiene:
             * - DEL: fabricante EISA
             * - 4098: product ID hexadecimal
             */
            string[] parts = deviceId.Split('\\');

            if (parts.Length < 2)
                return Empty();

            string hardwarePart = parts[1];

            if (hardwarePart.Length < 4)
                return Empty();

            string vendorPart = hardwarePart.Substring(0, 3);
            string productPart = hardwarePart.Substring(3);

            if (!ushort.TryParse(
                    productPart,
                    System.Globalization.NumberStyles.HexNumber,
                    null,
                    out ushort productId))
            {
                return Empty();
            }

            ushort vendorId = EncodeEisaVendor(vendorPart);

            return new MonitorHardwareID
            {
                VendorID = vendorId,
                ProductID = productId
            };
        }

        private static ushort EncodeEisaVendor(string vendor)
        {
            if (string.IsNullOrWhiteSpace(vendor) ||
                vendor.Length != 3)
            {
                return 0;
            }

            vendor = vendor.ToUpperInvariant();

            if (vendor[0] < 'A' || vendor[0] > 'Z' ||
                vendor[1] < 'A' || vendor[1] > 'Z' ||
                vendor[2] < 'A' || vendor[2] > 'Z')
            {
                return 0;
            }

            int value =
                ((vendor[0] - '@') << 10) |
                ((vendor[1] - '@') << 5) |
                (vendor[2] - '@');

            // EDID usa el valor en orden little-endian para esta propiedad.
            return (ushort)((value >> 8) | (value << 8));
        }

        private static MonitorHardwareID Empty()
        {
            return new MonitorHardwareID
            {
                VendorID = 0,
                ProductID = 0
            };
        }
    }
}