using Android.App;
using Android.Content;
using Android.Net;
using Android.OS;
using Android.Widget;
using System;

namespace PixelVRClient;

[Activity(
    Label = "PixelVR Client",
    MainLauncher = true,
    Exported = true
)]
public class MainActivity : Activity
{
    private EditText? _portText;
    private TextView? _statusText;
    private const string VlcPackage = "org.videolan.vlc";

    protected override void OnCreate(Bundle? savedInstanceState)
    {
        base.OnCreate(savedInstanceState);
        BuildUI();
    }

    void BuildUI()
    {
        var root = new LinearLayout(this)
        {
            Orientation = Orientation.Vertical
        };

        // SetPadding expects (left, top, right, bottom)
        root.SetPadding(24, 24, 24, 24);

        var title = new TextView(this) { Text = "PixelVR Client", TextSize = 20f };
        var info = new TextView(this) { Text = "Abre VLC para recibir RTP H.264", TextSize = 14f };

        var portLabel = new TextView(this) { Text = "Puerto RTP:" };
        _portText = new EditText(this) { Text = "5000", InputType = Android.Text.InputTypes.ClassNumber };

        var openBtn = new Button(this) { Text = "Abrir stream en VLC" };
        openBtn.Click += (_, _) => OpenVlc();

        var checkBtn = new Button(this) { Text = "Comprobar VLC" };
        checkBtn.Click += (_, _) => CheckVlc();

        _statusText = new TextView(this) { Text = "Estado: listo" };

        root.AddView(title);
        root.AddView(info);
        root.AddView(portLabel);
        root.AddView(_portText);
        root.AddView(openBtn);
        root.AddView(checkBtn);
        root.AddView(_statusText);

        SetContentView(root);
    }

    void OpenVlc()
    {
        // Use null-safe access and fallback
        var portStr = _portText?.Text?.Trim();
        if (string.IsNullOrEmpty(portStr))
            portStr = "5000";

        if (!int.TryParse(portStr, out int port) || port < 1 || port > 65535)
        {
            SetStatus("Puerto no válido");
            return;
        }

        // VLC can listen locally: rtp://@:5000
        var url = $"rtp://@:{port}";

        // Disambiguate Uri by using Android.Net.Uri explicitly
        Android.Net.Uri uri = Android.Net.Uri.Parse(url);

        var intent = new Intent(Intent.ActionView);
        intent.SetDataAndType(uri, "application/x-rtp");
        intent.SetPackage(VlcPackage);

        try
        {
            StartActivity(intent);
            SetStatus($"VLC abierto en {url}");
        }
        catch (ActivityNotFoundException)
        {
            SetStatus("VLC no instalado");
            ShowInstallAlert();
        }
        catch (Exception ex)
        {
            SetStatus($"Error al abrir VLC: {ex.Message}");
        }
    }

    void CheckVlc()
    {
        try
        {
            var pm = PackageManager;
            if (pm == null)
            {
                SetStatus("Error: PackageManager no disponible");
                return;
            }

            try
            {
                // Use a safe overload for GetPackageInfo
                var packageInfo = pm.GetPackageInfo(VlcPackage, 0);
                if (packageInfo != null)
                    SetStatus("VLC está instalado");
                else
                    SetStatus("VLC no está instalado");
            }
            catch (Android.Content.PM.PackageManager.NameNotFoundException)
            {
                SetStatus("VLC no está instalado");
            }
        }
        catch (Exception ex)
        {
            SetStatus($"Error comprobando VLC: {ex.Message}");
        }
    }

    void ShowInstallAlert()
    {
        new AlertDialog.Builder(this)
            .SetTitle("VLC no está instalado")
            .SetMessage("Instala VLC para Android y vuelve a intentar abrir el stream.")
            .SetPositiveButton("OK", (_, _) => { })
            .Show();
    }

    void SetStatus(string message)
    {
        // Null-safe update
        if (_statusText != null)
            _statusText.Text = $"Estado: {message}";
    }
}