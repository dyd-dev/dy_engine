#include "RHIFactory.h"

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

RHIDevice* CreateRHIDevice()
{
#if defined(ENABLE_D3D12)
	return new D3D12Device();
#elif defined(ENABLE_METAL)
	return new MetalDevice();
#elif defined(ENABLE_VULKAN)
	return new VulkanDevice();
#elif defined(ENABLE_SDL3)
	return new SDL3Device();
#else
	return nullptr;
#endif
}