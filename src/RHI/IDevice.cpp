#if defined(ENABLE_D3D12)
#include "Backends/D3D12/D3D12Device.h"
#elif defined(ENABLE_METAL)
#include "Backends/Metal/MetalDevice.h"
#elif defined(ENABLE_VULKAN)
#include "Backends/Vulkan/VulkanDevice.h"
#elif defined(ENABLE_SDL3)
#include "Backends/SDL3/SDL3Device.h"
#else
#include "Backends/Null/NullDevice.h"
#endif

dy::RHI::IDevice* dy::RHI::IDevice::Create(const void *windowHandle)
{
	IDevice *device = nullptr;
#if defined(ENABLE_D3D12)
	device = new D3D12Device();
#elif defined(ENABLE_METAL)
	device = new MetalDevice();
#elif defined(ENABLE_VULKAN)
	device = new VulkanDevice();
#elif defined(ENABLE_SDL3)
	device = new SDL3Device();
#else
	device = new Backends::NullDevice();
#endif
	if(device) device->Initialize(windowHandle);
	return device;
}