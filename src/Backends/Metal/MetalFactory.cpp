#include "MetalDevice.h"
#include <cstdio>
#include <memory>

namespace dyf::RHI
{
IDevice* IDevice::Create(const DeviceDesc& desc)
{
    std::unique_ptr<IDevice> device = std::make_unique<Backends::MetalDevice>();
    std::fprintf(stderr, "[RHI] Initializing Metal backend\n");
    device->m_desc = desc;
    if(device->Initialize(nullptr, device->GetDesc()) != 0)
    {
        std::fprintf(stderr, "[RHI] Device initialization failed\n");
        device.reset();
    }
    return device.release();
}
}
