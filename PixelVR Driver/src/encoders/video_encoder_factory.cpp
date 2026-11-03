#include "video_encoder.h"
#include "video_encoder_nvidia.h"
#include "video_encoder_amd.h"
#include "video_encoder_intel.h"
#include "video_encoder_mf.h"
#include "driverlog.h"
#include <algorithm>

std::unique_ptr<IStreamVideoEncoder> VideoEncoderFactory::CreateBestForAdapter(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    const std::string& adapterName)
{
    if (!device || !context)
    {
        DriverLog("[EncoderFactory] Error: device o context es nullptr");
        return nullptr;
    }

    DriverLog("[EncoderFactory] Detectando encoder para adaptador: %s", adapterName.c_str());
    DriverLog("[EncoderFactory] Usando Media Foundation H.264 (funciona en NVIDIA/AMD/Intel)");
    (void)adapterName;
    return std::make_unique<MediaFoundationEncoder>();
}

std::vector<std::string> VideoEncoderFactory::ListAvailableEncoders()
{
    std::vector<std::string> available;

    DriverLog("[EncoderFactory] Listando encoders disponibles...");

    available.push_back("Media Foundation H.264");
    DriverLog("[EncoderFactory] ✓ Media Foundation H.264");

    if (NvidiaEncoder::IsAvailable())
    {
        available.push_back("NVIDIA NVENC");
        DriverLog("[EncoderFactory] ✓ NVIDIA NVENC");
    }
    else
    {
        DriverLog("[EncoderFactory] ✗ NVIDIA NVENC (SDK no instalado)");
    }

    if (AMDEncoder::IsAvailable())
    {
        available.push_back("AMD VCN");
        DriverLog("[EncoderFactory] ✓ AMD VCN");
    }
    else
    {
        DriverLog("[EncoderFactory] ✗ AMD VCN (SDK no instalado)");
    }

    if (IntelEncoder::IsAvailable())
    {
        available.push_back("Intel QuickSync");
        DriverLog("[EncoderFactory] ✓ Intel QuickSync");
    }
    else
    {
        DriverLog("[EncoderFactory] ✗ Intel QuickSync (SDK no instalado)");
    }

    if (available.empty())
    {
        DriverLog("[EncoderFactory] ⚠️  NO HAY ENCODERS DISPONIBLES");
        DriverLog("[EncoderFactory] Instala al menos uno:");
        DriverLog("[EncoderFactory]   - NVIDIA Video Codec SDK");
        DriverLog("[EncoderFactory]   - AMD AMF SDK");
        DriverLog("[EncoderFactory]   - Intel Media SDK / oneVPL");
    }

    return available;
}