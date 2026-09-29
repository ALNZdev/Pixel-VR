using System;
using System.Windows;
using System.Windows.Threading;

namespace PixelVR
{
    internal sealed class PhoneStreamController : IDisposable
    {
        private readonly Dispatcher _dispatcher;
        private PixelVRStream.StatusCallback? _nativeCallback;
        private bool _running;

        public PhoneStreamController(Dispatcher dispatcher)
        {
            _dispatcher = dispatcher;

            // Mantener referencia administrada a delegado para que no lo recoja GC.
            _nativeCallback = OnNativeStatus;
            PixelVRStream.RegisterStatusCallback(_nativeCallback);
        }

        public bool IsRunning => _running;

        public event EventHandler<string>? StatusChanged;

        public bool Start(
            string phoneIp,
            int videoPort,
            int controlPort,
            int width,
            int height,
            int fps,
            int bitrateKbps,
            int gopSeconds)
        {
            if (string.IsNullOrWhiteSpace(phoneIp))
                throw new ArgumentException("La IP del teléfono es obligatoria.");

            if (PixelVRStream.IsStreamRunning())
            {
                _running = true;
                return true;
            }

            bool started = PixelVRStream.StartStream(
                phoneIp,
                videoPort,
                controlPort,
                width,
                height,
                fps,
                bitrateKbps,
                gopSeconds);

            _running = started;
            return started;
        }

        public void Stop()
        {
            if (!_running)
                return;

            PixelVRStream.StopStream();
            _running = false;
        }

        public void RequestKeyframe()
        {
            if (_running)
                PixelVRStream.RequestKeyframe();
        }

        private void OnNativeStatus(string message)
        {
            // Marshallea al hilo UI
            _dispatcher.BeginInvoke(
                DispatcherPriority.Background,
                new Action(() =>
                {
                    StatusChanged?.Invoke(this, message);
                }));
        }

        public void Dispose()
        {
            Stop();

            PixelVRStream.RegisterStatusCallback(null);
            _nativeCallback = null;
        }
    }
}