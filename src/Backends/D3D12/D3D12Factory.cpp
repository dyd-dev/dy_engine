#include "D3D12Device.h"
#include <cstdio>
#include <memory>

namespace dyf::RHI
{
    IDevice* IDevice::Create(const DeviceDesc& desc)
    {
        std::unique_ptr<IDevice> device = std::make_unique<dyf::Backends::D3D12Device>();
        std::fprintf(stderr, "[RHI] Initializing D3D12 backend\n");
        device->m_desc = desc;
        if(device->Initialize(nullptr, device->GetDesc()) != 0)
        {
            std::fprintf(stderr, "[RHI] Device initialization failed\n");
            device.reset();
        }
        return device.release();
    }
}
