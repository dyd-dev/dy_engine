#if defined(ENABLE_D3D12)
#include "Backends/D3D12/D3D12Device.h"
#elif defined(ENABLE_METAL)
#include "Backends/Metal/MetalDevice.h"
#elif defined(ENABLE_VULKAN)
#include "Backends/Vulkan/VulkanDevice.h"
#else
#include "Backends/Null/NullDevice.h"
#endif

using namespace dy::RHI;

IDevice* IDevice::Create(const void* windowHandle, const DeviceDesc& desc)
{
	IDevice *device = nullptr;
#if defined(ENABLE_D3D12)
	device = new dy::Backends::D3D12Device();
#elif defined(ENABLE_METAL)
    device = new dy::Backends::MetalDevice();
#elif defined(ENABLE_VULKAN)
	device = new dy::Backends::VulkanDevice();
#else
	device = new dy::Backends::NullDevice();
#endif
	if(device)
	{
		device->SetDesc(desc);
		device->Initialize(windowHandle, device->GetDesc());
	}
	return device;
}
