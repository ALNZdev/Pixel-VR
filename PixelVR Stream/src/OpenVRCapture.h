#pragma once
#include <string>
#include <d3d11.h>

#ifdef USE_OPENVR
#include <openvr.h>
#endif

// Wrapper para capturar la textura espejo del compositor OpenVR.
// Cuando CaptureFrame retorna true, outTexture contiene un ID3D11Texture2D* con AddRef (el llamador debe Release()).
class OpenVRCapture
{
public:
    OpenVRCapture();
    ~OpenVRCapture();

    // Inicializa OpenVR y opcionalmente usa el dispositivo D3D11 proporcionado (AddRef en init).
    // Si sharedDevice == nullptr, la clase creará un device propio (pero mejor pasar el device del encoder).
    bool Init(ID3D11Device* sharedDevice = nullptr);

    void Shutdown();

    // Captura un frame: obtiene la textura del compositor (AddRef).
    // Retorna true si ok y setea outError para mensajes legibles.
    bool CaptureFrame(ID3D11Texture2D** outTexture, std::string& outError);

    std::string LastError() const { return m_lastError; }

private:
    void ReleaseMirrorResource(void* pMirror);

private:
    std::string m_lastError;

#ifdef USE_OPENVR
    vr::IVRSystem* m_system = nullptr;
    vr::IVRCompositor* m_compositor = nullptr;
#endif

    // D3D11 device/context (si se pasa sharedDevice en Init se AddRef; si no, los creamos)
    ID3D11Device* m_d3dDevice = nullptr;
    ID3D11DeviceContext* m_d3dContext = nullptr;

    bool m_ownDevice = false; // true si OpenVRCapture creó el device (libera al Shutdown)
};