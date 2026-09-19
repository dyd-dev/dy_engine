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
	IDevice *device = nullptr;
#if defined(ENABLE_D3D12)
	device = new dyf::Backends::D3D12Device();
#elif defined(ENABLE_METAL)
    device = new dyf::Backends::MetalDevice();
#elif defined(ENABLE_VULKAN)
	device = new dyf::Backends::VulkanDevice();
#else
	device = new dyf::Backends::NullDevice();
#endif
	if(device)
	{
		device->m_desc = desc;
		if(device->Initialize(nullptr, device->GetDesc()) != 0)
		{
			delete device;
			device = nullptr;
		}
	}
	return device;
}

bool IDevice::Supports(Feature feature) const {return SupportsNative(feature);}
