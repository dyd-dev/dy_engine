#include "dyf/Platform/Log.h"
#include <memory>
#if defined(ENABLE_D3D12)
#include "Backends/D3D12/D3D12Device.h"
#elif defined(ENABLE_METAL)
#include "Backends/Metal/MetalDevice.h"
#elif defined(ENABLE_VULKAN)
#include "Backends/Vulkan/VulkanDevice.h"
#else
#include "Backends/Null/NullDevice.h"
#endif

using namespace dyf::RHI;

IDevice* IDevice::Create(const DeviceDesc& desc)
{
    dyf::Platform::Log::Initialize();
	std::unique_ptr<IDevice> device;
#if defined(ENABLE_D3D12)
	device = std::make_unique<dyf::Backends::D3D12Device>();
    dyf::Platform::Log::Write(dyf::Platform::LogLevel::Info, "RHI", "Initializing D3D12 backend");
#elif defined(ENABLE_METAL)
    device = std::make_unique<dyf::Backends::MetalDevice>();
    dyf::Platform::Log::Write(dyf::Platform::LogLevel::Info, "RHI", "Initializing Metal backend");
#elif defined(ENABLE_VULKAN)
	device = std::make_unique<dyf::Backends::VulkanDevice>();
    dyf::Platform::Log::Write(dyf::Platform::LogLevel::Info, "RHI", "Initializing Vulkan backend");
#else
	device = std::make_unique<dyf::Backends::NullDevice>();
    dyf::Platform::Log::Write(dyf::Platform::LogLevel::Info, "RHI", "Initializing Null backend");
#endif
	device->m_desc = desc;
	if(device->Initialize(nullptr, device->GetDesc()) != 0)
	{
		dyf::Platform::Log::Write(dyf::Platform::LogLevel::Error, "RHI", "Device initialization failed");
		device.reset();
	}
	return device.release();
}

bool IDevice::Supports(Feature feature) const {return SupportsNative(feature);}
