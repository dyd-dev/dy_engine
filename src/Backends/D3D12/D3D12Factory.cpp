#include "D3D12Device.h"
#include "dyf/Platform/Log.h"
#include <memory>

namespace dyf::RHI
{
    IDevice* IDevice::Create(const DeviceDesc& desc)
    {
        dyf::Platform::Log::Initialize();
        std::unique_ptr<IDevice> device = std::make_unique<dyf::Backends::D3D12Device>();
        dyf::Platform::Log::Write(dyf::Platform::LogLevel::Info, "RHI", "Initializing D3D12 backend");
        device->m_desc = desc;
        if(device->Initialize(nullptr, device->GetDesc()) != 0)
        {
            dyf::Platform::Log::Write(dyf::Platform::LogLevel::Error, "RHI", "Device initialization failed");
            device.reset();
        }
        return device.release();
    }
}
