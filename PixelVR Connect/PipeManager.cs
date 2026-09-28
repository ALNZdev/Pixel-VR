using System;
using System.Diagnostics;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;

namespace PixelVR
{
    public sealed class PipeManager : IDisposable
    {
        // ─── Tipos de datos para SteamVR ──────────────────────────────────────
        public struct Quaternion
        {
            public float W, X, Y, Z;
        }

        // Paquete de datos para el driver SteamVR
        // Incluye HMD + Controles Izquierdo + Controles Derecho.
        //
        // Los campos de orientación/botones/error/status coinciden EXACTAMENTE
        // con el protocolo del microcontrolador (allData en main_hmd.cpp,
        // AllData en main4.py y en USBCDCManager.cs): cuaterniones *_qw/qx/qy/qz,
        // botones individuales (sin bitmask), stick_x/y, status y error.
        //
        // Los campos de POSICIÓN (hmdPosX/Y/Z, lPosX/Y/Z, rPosX/Y/Z) no vienen
        // del sensor — los decide la app (colocación fija o sliders de prueba).
        // Debe coincidir EXACTAMENTE con AllData de pipe_handler.h.
        [StructLayout(LayoutKind.Sequential, Pack = 1)]
        public struct FullSystemPacket
        {
            // HMD
            public float hmdPosX, hmdPosY, hmdPosZ;
            public float hmd_qw, hmd_qx, hmd_qy, hmd_qz;
            public float hmd_error;

            // Left Controller
            public float lPosX, lPosY, lPosZ;
            public float left_qw, left_qx, left_qy, left_qz;
            [MarshalAs(UnmanagedType.I1)] public bool left_a;
            [MarshalAs(UnmanagedType.I1)] public bool left_b;
            [MarshalAs(UnmanagedType.I1)] public bool left_system;
            [MarshalAs(UnmanagedType.I1)] public bool left_grip;
            [MarshalAs(UnmanagedType.I1)] public bool left_trigger;
            [MarshalAs(UnmanagedType.I1)] public bool left_stick_click;
            public float left_stick_x, left_stick_y;
            [MarshalAs(UnmanagedType.I1)] public bool left_status;
            public float left_error;

            // Right Controller
            public float rPosX, rPosY, rPosZ;
            public float right_qw, right_qx, right_qy, right_qz;
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

        // ─── Campos privados ──────────────────────────────────────────────────
        private readonly string _pipeName = "PixelVR_Pipe";
        private readonly object _syncRoot = new object();
        private readonly CancellationTokenSource _cts = new();

        private NamedPipeServerStream? _pipeServer;
        private bool _isRunning = true;
        private long _packetsSent = 0;
        private long _sendErrors = 0;
        private Stopwatch _statsTimer = Stopwatch.StartNew();

        // ─── Constructor ──────────────────────────────────────────────────────
        public PipeManager()
        {
            Debug.WriteLine($"[PipeManager] Iniciando servidor en pipe: {_pipeName}");
            StartServer();
        }

        // ─── Iniciar servidor de pipe ─────────────────────────────────────────
        private void StartServer()
        {
            _ = Task.Run(async () =>
            {
                while (_isRunning && !_cts.IsCancellationRequested)
                {
                    NamedPipeServerStream? server = null;
                    try
                    {
                        server = new NamedPipeServerStream(
                            _pipeName,
                            PipeDirection.Out,
                            1,
                            PipeTransmissionMode.Byte,
                            PipeOptions.Asynchronous | PipeOptions.WriteThrough);

                        lock (_syncRoot)
                        {
                            _pipeServer = server;
                        }

                        Debug.WriteLine($"[PipeManager] Esperando conexión del driver...");
                        await server.WaitForConnectionAsync(_cts.Token);
                        Debug.WriteLine($"[PipeManager] ✓ Driver conectado al pipe");

                        // Mantener el servidor activo mientras esté conectado
                        while (_isRunning && server.IsConnected && !_cts.IsCancellationRequested)
                        {
                            await Task.Delay(100, _cts.Token);
                        }

                        if (!server.IsConnected)
                        {
                            Debug.WriteLine("[PipeManager] Driver desconectado del pipe");
                        }
                    }
                    catch (OperationCanceledException)
                    {
                        Debug.WriteLine("[PipeManager] Servidor cancelado");
                        break;
                    }
                    catch (Exception ex)
                    {
                        Debug.WriteLine($"[PipeManager] Error en servidor: {ex.Message}");
                    }
                    finally
                    {
                        lock (_syncRoot)
                        {
                            if (ReferenceEquals(_pipeServer, server))
                            {
                                _pipeServer = null;
                            }
                        }

                        server?.Dispose();

                        // Esperar antes de reintentar conexión
                        if (_isRunning && !_cts.IsCancellationRequested)
                        {
                            await Task.Delay(500, _cts.Token);
                        }
                    }
                }
            }, _cts.Token);
        }

        // ─── Enviar datos al driver ───────────────────────────────────────────
        public void WriteData(FullSystemPacket packet)
        {
            NamedPipeServerStream? server;
            lock (_syncRoot)
            {
                server = _pipeServer;
            }

            if (server == null || !server.IsConnected)
            {
                return; // Driver no conectado
            }

            IntPtr ptr = IntPtr.Zero;
            try
            {
                int size = Marshal.SizeOf<FullSystemPacket>();
                byte[] buffer = new byte[size];
                ptr = Marshal.AllocHGlobal(size);
                Marshal.StructureToPtr(packet, ptr, false);
                Marshal.Copy(ptr, buffer, 0, size);

                lock (_syncRoot)
                {
                    if (_pipeServer == null || !ReferenceEquals(_pipeServer, server) || !_pipeServer.IsConnected)
                    {
                        return;
                    }

                    _pipeServer.Write(buffer, 0, buffer.Length);
                    _pipeServer.Flush();
                    _packetsSent++;

                    if (_packetsSent % 500 == 0)
                    {
                        double fps = _packetsSent / _statsTimer.Elapsed.TotalSeconds;
                        Debug.WriteLine(
                            $"[PipeManager] Paquetes enviados: {_packetsSent}, Errores: {_sendErrors}, {fps:F1} pps");
                    }
                }
            }
            catch (Exception ex)
            {
                _sendErrors++;
                Debug.WriteLine($"[PipeManager] ✗ Error escribiendo en pipe: {ex.Message}");
                TryDisconnect(server);
            }
            finally
            {
                if (ptr != IntPtr.Zero)
                {
                    Marshal.FreeHGlobal(ptr);
                }
            }
        }

        // ─── Intentar desconectar ─────────────────────────────────────────────
        private static void TryDisconnect(NamedPipeServerStream server)
        {
            try
            {
                if (server?.IsConnected == true)
                {
                    server.Disconnect();
                }
            }
            catch { /* ignorar errores de desconexión */ }
        }

        // ─── Obtener estadísticas ─────────────────────────────────────────────
        public (long packetsSent, long sendErrors, double packetsPerSecond) GetStatistics()
        {
            double pps = _statsTimer.Elapsed.TotalSeconds > 0
                ? _packetsSent / _statsTimer.Elapsed.TotalSeconds
                : 0;
            return (_packetsSent, _sendErrors, pps);
        }

        // ─── IDisposable ──────────────────────────────────────────────────────
        public void Dispose()
        {
            if (!_isRunning)
            {
                return;
            }

            _isRunning = false;
            _cts.Cancel();

            lock (_syncRoot)
            {
                _pipeServer?.Dispose();
                _pipeServer = null;
            }

            _cts.Dispose();
            Debug.WriteLine($"[PipeManager] Recursos liberados. Total enviado: {_packetsSent} paquetes");
        }
    }
}