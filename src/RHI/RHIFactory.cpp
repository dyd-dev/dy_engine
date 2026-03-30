#include "RHIFactory.h"

#if defined(ENABLE_D3D12)
#include "Backends/D3D12/D3D12Device.h"
#elif defined(ENABLE_VULKAN)
#include "Backends/Vulkan/VulkanDevice.h"
#endif

RHIDevice* CreateRHIDevice()
{
#if defined(ENABLE_D3D12)
	return new D3D12Device();
#elif defined(ENABLE_VULKAN)
	return new VulkanDevice();
#else
	// No RHI backend enabled. CreateRHIDevice() will return nullptr.
	return nullptr;
#endif
}