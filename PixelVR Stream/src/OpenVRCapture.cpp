#include "OpenVRCapture.h"
#include <wrl.h>
#include <iostream>

using Microsoft::WRL::ComPtr;

OpenVRCapture::OpenVRCapture()
{
}

OpenVRCapture::~OpenVRCapture()
{
    Shutdown();
}

bool OpenVRCapture::Init(ID3D11Device* sharedDevice)
{
#ifdef USE_OPENVR
    vr::EVRInitError err = vr::VRInitError_None;
    m_system = vr::VR_Init(&err, vr::VRApplication_Background);
    if (err != vr::VRInitError_None || m_system == nullptr)
    {
        m_lastError = "OpenVR failed to initialize.";
        return false;
    }

    m_compositor = vr::VRCompositor();
    if (m_compositor == nullptr)
    {
        m_lastError = "IVRCompositor not available.";
        vr::VR_Shutdown();
        m_system = nullptr;
        return false;
    }
#else
    m_lastError = "OpenVR support compiled out.";
    return false;
#endif

    if (sharedDevice)
    {
        // use provided device
        m_d3dDevice = sharedDevice;
        m_d3dDevice->AddRef();
        m_d3dDevice->GetImmediateContext(&m_d3dContext);
        m_ownDevice = false;
        return true;
    }

    // else create our own D3D11 device (less desirable)
    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        creationFlags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &m_d3dDevice,
        &obtained,
        &m_d3dContext);

    if (FAILED(hr))
    {
        m_lastError = "D3D11CreateDevice failed for OpenVRCapture";
        return false;
    }
    m_ownDevice = true;
    return true;
}

void OpenVRCapture::Shutdown()
{
#ifdef USE_OPENVR
    if (m_system)
    {
        vr::VR_Shutdown();
        m_system = nullptr;
        m_compositor = nullptr;
    }
#endif

    if (m_d3dContext)
    {
        m_d3dContext->Release();
        m_d3dContext = nullptr;
    }
    if (m_d3dDevice)
    {
        m_d3dDevice->Release();
        m_d3dDevice = nullptr;
    }
    m_ownDevice = false;
}

void OpenVRCapture::ReleaseMirrorResource(void* pMirror)
{
#ifdef USE_OPENVR
    if (!pMirror) return;
    // The compositor expects ReleaseMirrorTextureD3D11 with the SRV pointer passed earlier
    ID3D11ShaderResourceView* srv = reinterpret_cast<ID3D11ShaderResourceView*>(pMirror);
    m_compositor->ReleaseMirrorTextureD3D11(srv);
#endif
}

bool OpenVRCapture::CaptureFrame(ID3D11Texture2D** outTexture, std::string& outError)
{
    if (!outTexture) return false;
    *outTexture = nullptr;
#ifdef USE_OPENVR
    if (!m_compositor)
    {
        outError = "Compositor not initialized";
        return false;
    }

    if (!m_d3dDevice)
    {
        outError = "No D3D11 device available in OpenVRCapture";
        return false;
    }

    void* pMirror = nullptr;
    // GetMirrorTextureD3D11 signature varies; most common:
    // EVRCompositorError GetMirrorTextureD3D11(EVREye eEye, void* pD3D11DeviceOrResource, void** ppD3D11ShaderResourceView)
    vr::EVRCompositorError err = m_compositor->GetMirrorTextureD3D11(vr::Eye_Left, static_cast<void*>(m_d3dDevice), &pMirror);

    if (err != vr::VRCompositorError_None || pMirror == nullptr)
    {
        outError = "GetMirrorTextureD3D11 failed or returned null";
        return false;
    }

    // pMirror is typically an ID3D11ShaderResourceView*. Try to treat it as such.
    ID3D11ShaderResourceView* srv = reinterpret_cast<ID3D11ShaderResourceView*>(pMirror);
    if (srv)
    {
        // GetResource -> ID3D11Resource -> QI to ID3D11Texture2D
        ID3D11Resource* res = nullptr;
        srv->GetResource(&res);
        if (!res)
        {
            // Release mirror and fail
            ReleaseMirrorResource(pMirror);
            outError = "GetResource() from SRV returned null";
            return false;
        }

        ID3D11Texture2D* tex = nullptr;
        HRESULT hr = res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
        // release intermediate resource
        res->Release();

        if (FAILED(hr) || !tex)
        {
            // Release mirror and fail
            ReleaseMirrorResource(pMirror);
            outError = "Could not QI resource to ID3D11Texture2D";
            return false;
        }

        // We have a texture; the compositor still owns the mirror; to be safe copy the texture:
        // Option A: return tex directly (caller must not ReleaseMirrorTextureD3D11 until done)
        // Option B: create a new texture and CopyResource (safer). We'll create a new texture and copy, to avoid lifetime issues.

        // Create a target texture with default bind flags (GPU local) same desc as tex
        D3D11_TEXTURE2D_DESC desc = {};
        tex->GetDesc(&desc);

        // create a texture we own (staging or default with CPU read not necessary if we use MF CreateDXGIBuffer)
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;

        ID3D11Texture2D* ownTex = nullptr;
        HRESULT hr2 = m_d3dDevice->CreateTexture2D(&desc, nullptr, &ownTex);
        if (SUCCEEDED(hr2) && ownTex)
        {
            // Copy from compositor texture to our texture
            m_d3dContext->CopyResource(ownTex, tex);
            // release the compositor texture reference
            tex->Release();
            // release the mirror SRV properly
            ReleaseMirrorResource(pMirror);
            // return our own texture (AddRef already from CreateTexture2D)
            *outTexture = ownTex;
            return true;
        }
        else
        {
            // fallback: return the compositor texture but note that caller must not ReleaseMirrorTextureD3D11; to avoid complexity, fail
            tex->Release();
            ReleaseMirrorResource(pMirror);
            outError = "Failed to create local copy texture";
            return false;
        }
    }
    else
    {
        outError = "Mirror resource is not a shader resource view";
        return false;
    }
#else
    outError = "OpenVR compiled out";
    return false;
#endif
}