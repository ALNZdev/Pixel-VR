using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO.Ports;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;

namespace PixelVR
{
    // Debe coincidir EXACTAMENTE con allData de main_hmd.cpp
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct AllData
    {
        public float hmd_qW, hmd_qX, hmd_qY, hmd_qZ;
        public float hmd_error;

        public float left_qW, left_qX, left_qY, left_qZ;
        [MarshalAs(UnmanagedType.I1)] public bool left_a;
        [MarshalAs(UnmanagedType.I1)] public bool left_b;
        [MarshalAs(UnmanagedType.I1)] public bool left_system;
        [MarshalAs(UnmanagedType.I1)] public bool left_grip;
        [MarshalAs(UnmanagedType.I1)] public bool left_trigger;
        [MarshalAs(UnmanagedType.I1)] public bool left_stick_click;
        public float left_stick_x, left_stick_y;
        [MarshalAs(UnmanagedType.I1)] public bool left_status;
        public float left_error;

        public float right_qW, right_qX, right_qY, right_qZ;
        [MarshalAs(UnmanagedType.I1)] public bool right_a;
        [MarshalAs(UnmanagedType.I1)] public bool right_b;
        [MarshalAs(UnmanagedType.I1)] public bool right_system;
        [MarshalAs(UnmanagedType.I1)] public bool right_grip;
        [MarshalAs(UnmanagedType.I1)] public bool right_trigger;
        [MarshalAs(UnmanagedType.I1)] public bool right_stick_click;
        public float right_stick_x, right_stick_y;
        [MarshalAs(UnmanagedType.I1)] public bool right_status;
        public float right_error;
    }

    public sealed class USBCDCManager : IDisposable
    {
        private const byte SYNC1 = 0xAA;
        private const byte SYNC2 = 0x55;
        private const byte END1 = 0xFF;
        private const byte END2 = 0xFE;

        private SerialPort? _port;
        private CancellationTokenSource? _cts;
        private Task? _readTask;
        private bool _disposed;
        private readonly object _lockObject = new();

        private static readonly int StructSize = Marshal.SizeOf<AllData>();
        private static readonly int PacketSize = 2 + StructSize + 2;

        private long _packetsReceived;
        private long _packetErrors;
        private long _bytesReceived;
        private readonly Stopwatch _statsTimer = Stopwatch.StartNew();

        // Reconexión automática
        private bool _autoReconnectEnabled = false;
        private string? _lastConnectedPort;
        private int _lastBaudRate = 115200;
        private CancellationTokenSource? _reconnectCts;
        private Task? _reconnectTask;
        private const int RECONNECT_INTERVAL_MS = 2000;
        private const int CONSECUTIVE_READ_ERRORS_THRESHOLD = 10;
        private int _consecutiveReadErrors = 0;

        public event EventHandler<AllData>? DataReceived;
        public event EventHandler<string>? ErrorOccurred;
        public event EventHandler? ConnectionStatusChanged;
        public event EventHandler<string>? ReconnectAttempt;

        public bool IsConnected => _port?.IsOpen ?? false;

        public static string[] GetAvailableComPorts() => SerialPort.GetPortNames();

        public USBCDCManager()
        {
            Debug.WriteLine($"[USB-CDC] Struct: {StructSize} bytes, Packet: {PacketSize} bytes");
        }

        public bool Connect(string portName, int baudRate = 115200, bool enableAutoReconnect = false)
        {
            if (_disposed) return false;

            if (_port?.IsOpen == true)
                Disconnect();

            _lastConnectedPort = portName;
            _lastBaudRate = baudRate;
            _autoReconnectEnabled = enableAutoReconnect;

            try
            {
                _port = new SerialPort(portName, baudRate, Parity.None, 8, StopBits.One)
                {
                    Handshake = Handshake.None,
                    ReadTimeout = 500,
                    WriteTimeout = 500,
                    DtrEnable = false,
                    RtsEnable = false,
                    ReadBufferSize = 16384,
                    WriteBufferSize = 16384
                };

                _port.Open();
                Thread.Sleep(100);

                _packetsReceived = 0;
                _packetErrors = 0;
                _bytesReceived = 0;
                _consecutiveReadErrors = 0;
                _statsTimer.Restart();

                _cts = new CancellationTokenSource();
                _readTask = Task.Run(() => ReadLoopAsync(_cts.Token), _cts.Token);

                if (_autoReconnectEnabled)
                {
                    _reconnectCts = new CancellationTokenSource();
                    _reconnectTask = Task.Run(() => AutoReconnectLoopAsync(_reconnectCts.Token), _reconnectCts.Token);
                }

                Debug.WriteLine($"[USB-CDC] ✓ Conectado a {portName}");
                ConnectionStatusChanged?.Invoke(this, EventArgs.Empty);
                return true;
            }
            catch (Exception ex)
            {
                ErrorOccurred?.Invoke(this, $"Error conectando a {portName}: {ex.Message}");
                return false;
            }
        }

        public bool SendCommand(string command)
        {
            if (string.IsNullOrWhiteSpace(command)) return false;

            lock (_lockObject)
            {
                if (_port?.IsOpen != true) return false;

                try
                {
                    _port.WriteLine(command.Trim());
                    _port.BaseStream.Flush();
                    Debug.WriteLine($"[USB-CDC] → comando enviado: {command.Trim()}");
                    return true;
                }
                catch (Exception ex)
                {
                    ErrorOccurred?.Invoke(this, $"Error enviando comando '{command}': {ex.Message}");
                    return false;
                }
            }
        }

        public void Disconnect()
        {
            lock (_lockObject)
            {
                _autoReconnectEnabled = false;

                _reconnectCts?.Cancel();
                try { _reconnectTask?.Wait(2000); } catch { }

                _cts?.Cancel();
                try { _readTask?.Wait(2000); } catch { }

                try
                {
                    if (_port?.IsOpen == true) _port.Close();
                }
                catch { }

                _port = null;
                _cts = null;
                _readTask = null;
                _reconnectCts = null;
                _reconnectTask = null;
            }

            Debug.WriteLine("[USB-CDC] ✓ Desconectado");
            ConnectionStatusChanged?.Invoke(this, EventArgs.Empty);
        }

        public (long packetsReceived, long packetErrors, double packetsPerSecond) GetStatistics()
        {
            double pps = _statsTimer.Elapsed.TotalSeconds > 0
                ? _packetsReceived / _statsTimer.Elapsed.TotalSeconds
                : 0;

            return (_packetsReceived, _packetErrors, pps);
        }

        private async Task AutoReconnectLoopAsync(CancellationToken ct)
        {
            try
            {
                while (!ct.IsCancellationRequested && _autoReconnectEnabled)
                {
                    if (_consecutiveReadErrors >= CONSECUTIVE_READ_ERRORS_THRESHOLD)
                    {
                        Debug.WriteLine($"[USB-CDC] ⚠ {_consecutiveReadErrors} errores consecutivos. Intentando reconectar...");
                        await AttemptReconnect(ct);
                        _consecutiveReadErrors = 0;
                        continue;
                    }

                    if (_port?.IsOpen != true && _autoReconnectEnabled)
                    {
                        Debug.WriteLine("[USB-CDC] ⚠ Conexión perdida. Intentando reconectar...");
                        await AttemptReconnect(ct);
                        continue;
                    }

                    await Task.Delay(500, ct);
                }
            }
            catch (OperationCanceledException)
            {
                Debug.WriteLine("[USB-CDC] Loop de reconexión cancelado");
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"[USB-CDC] Error en loop de reconexión: {ex.Message}");
            }
        }

        private async Task AttemptReconnect(CancellationToken ct)
        {
            if (string.IsNullOrWhiteSpace(_lastConnectedPort) || !_autoReconnectEnabled)
                return;

            int attempts = 0;
            const int maxAttempts = 10;

            while (attempts < maxAttempts && _autoReconnectEnabled && !ct.IsCancellationRequested)
            {
                try
                {
                    attempts++;

                    string message = $"Reconectando a {_lastConnectedPort}... (Intento {attempts}/{maxAttempts})";
                    Debug.WriteLine($"[USB-CDC] {message}");
                    ReconnectAttempt?.Invoke(this, message);

                    lock (_lockObject)
                    {
                        _cts?.Cancel();
                        try { _readTask?.Wait(1000); } catch { }

                        try
                        {
                            if (_port?.IsOpen == true)
                                _port.Close();
                        }
                        catch { }

                        _port?.Dispose();
                        _port = null;
                        _cts = null;
                        _readTask = null;
                    }

                    await Task.Delay(500, ct);

                    _port = new SerialPort(_lastConnectedPort, _lastBaudRate, Parity.None, 8, StopBits.One)
                    {
                        Handshake = Handshake.None,
                        ReadTimeout = 500,
                        WriteTimeout = 500,
                        DtrEnable = false,
                        RtsEnable = false,
                        ReadBufferSize = 16384,
                        WriteBufferSize = 16384
                    };

                    _port.Open();
                    await Task.Delay(100, ct);

                    _consecutiveReadErrors = 0;

                    _cts = new CancellationTokenSource();
                    _readTask = Task.Run(() => ReadLoopAsync(_cts.Token), _cts.Token);

                    Debug.WriteLine($"[USB-CDC] ✓ Reconectado a {_lastConnectedPort}");
                    ReconnectAttempt?.Invoke(this, $"✓ Reconectado a {_lastConnectedPort}");
                    ConnectionStatusChanged?.Invoke(this, EventArgs.Empty);
                    return;
                }
                catch (Exception ex)
                {
                    Debug.WriteLine($"[USB-CDC] Error en intento de reconexión {attempts}: {ex.Message}");

                    lock (_lockObject)
                    {
                        try
                        {
                            if (_port?.IsOpen == true)
                                _port.Close();
                        }
                        catch { }

                        _port?.Dispose();
                        _port = null;
                    }

                    if (attempts < maxAttempts)
                    {
                        await Task.Delay(RECONNECT_INTERVAL_MS, ct);
                    }
                }
            }

            if (attempts >= maxAttempts)
            {
                string failMessage = $"✗ No se pudo reconectar a {_lastConnectedPort} después de {maxAttempts} intentos";
                Debug.WriteLine($"[USB-CDC] {failMessage}");
                ReconnectAttempt?.Invoke(this, failMessage);
                ErrorOccurred?.Invoke(this, failMessage);
            }
        }

        private async Task ReadLoopAsync(CancellationToken ct)
        {
            var acc = new List<byte>(PacketSize * 10);
            var tempBuf = new byte[4096];

            try
            {
                while (!ct.IsCancellationRequested)
                {
                    lock (_lockObject)
                    {
                        if (_port?.IsOpen != true)
                            break;

                        int available = _port.BytesToRead;
                        if (available > 0)
                        {
                            try
                            {
                                int toRead = Math.Min(tempBuf.Length, available);
                                int n = _port.Read(tempBuf, 0, toRead);
                                _bytesReceived += n;
                                _consecutiveReadErrors = 0;

                                for (int i = 0; i < n; i++)
                                    acc.Add(tempBuf[i]);

                                ProcessAccumulator(acc);
                            }
                            catch (TimeoutException)
                            {
                            }
                            catch (Exception ex)
                            {
                                Debug.WriteLine($"[USB-CDC] Error leyendo: {ex.Message}");
                                _consecutiveReadErrors++;
                                _packetErrors++;
                            }
                        }
                    }

                    if (acc.Count < PacketSize)
                    {
                        await Task.Delay(1, ct);
                    }
                }
            }
            catch (OperationCanceledException)
            {
                Debug.WriteLine("[USB-CDC] Lectura cancelada");
            }
            catch (Exception ex)
            {
                ErrorOccurred?.Invoke(this, $"Error crítico: {ex.Message}");
            }
        }

        private void ProcessAccumulator(List<byte> acc)
        {
            while (acc.Count >= PacketSize)
            {
                int headerAt = -1;
                int limit = Math.Min(acc.Count - PacketSize + 1, 10000);

                for (int i = 0; i < limit; i++)
                {
                    if (acc[i] == SYNC1 && i + 1 < acc.Count && acc[i + 1] == SYNC2)
                    {
                        headerAt = i;
                        break;
                    }
                }

                if (headerAt < 0)
                {
                    int discard = Math.Max(0, acc.Count - 1);
                    if (discard > 0) acc.RemoveRange(0, discard);
                    return;
                }

                if (headerAt > 0)
                {
                    acc.RemoveRange(0, headerAt);
                    continue;
                }

                if (acc.Count < PacketSize) return;

                int footerAt = 2 + StructSize;
                if (footerAt + 1 >= acc.Count) return;

                if (acc[footerAt] != END1 || acc[footerAt + 1] != END2)
                {
                    _packetErrors++;
                    acc.RemoveRange(0, 1);
                    continue;
                }

                try
                {
                    byte[] raw = new byte[StructSize];
                    for (int i = 0; i < StructSize; i++)
                        raw[i] = acc[2 + i];

                    var gch = GCHandle.Alloc(raw, GCHandleType.Pinned);
                    try
                    {
                        var data = Marshal.PtrToStructure<AllData>(gch.AddrOfPinnedObject());
                        DataReceived?.Invoke(this, data);
                        _packetsReceived++;

                        if (_packetsReceived % 100 == 0)
                        {
                            double fps = _packetsReceived / _statsTimer.Elapsed.TotalSeconds;
                            Debug.WriteLine($"[USB-CDC] {fps:F1} pps - HMD Q({data.hmd_qW:F3},{data.hmd_qX:F3},{data.hmd_qY:F3},{data.hmd_qZ:F3})");
                        }
                    }
                    finally
                    {
                        gch.Free();
                    }
                }
                catch (Exception ex)
                {
                    Debug.WriteLine($"[USB-CDC] Error deserializando: {ex.Message}");
                    _packetErrors++;
                }

                acc.RemoveRange(0, PacketSize);
            }
        }

        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;
            Disconnect();
            _cts?.Dispose();
            _reconnectCts?.Dispose();
        }
    }
}