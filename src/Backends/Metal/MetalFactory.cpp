#include "MetalDevice.h"
#include "dyf/Platform/Log.h"
#include <memory>

namespace dyf::RHI
{
IDevice* IDevice::Create(const DeviceDesc& desc)
{
    Platform::Log::Initialize();
    std::unique_ptr<IDevice> device = std::make_unique<Backends::MetalDevice>();
    Platform::Log::Write(Platform::LogLevel::Info, "RHI", "Initializing Metal backend");
    device->m_desc = desc;
    if(device->Initialize(nullptr, device->GetDesc()) != 0)
    {
        Platform::Log::Write(Platform::LogLevel::Error, "RHI", "Device initialization failed");
        device.reset();
    }
    return device.release();
}
}
