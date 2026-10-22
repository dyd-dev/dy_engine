#include "dyf/RHI/IDevice.h"
#include "FormatCapabilities.h"
#if defined(ENABLE_VULKAN)
#include "../Backends/Vulkan/VulkanDevice.h"
#elif defined(ENABLE_D3D12)
#include "../Backends/D3D12/D3D12Device.h"
#elif defined(ENABLE_METAL)
#include "../Backends/Metal/MetalDevice.h"
#endif

using namespace dyf::RHI;

bool IDevice::Supports(Feature feature) const {return SupportsNative(feature);}

bool dyf::RHI::Detail::SupportsDdsCompression(const IDevice& device,Format format) noexcept
{
#if defined(ENABLE_VULKAN)
    if(const auto* native=dynamic_cast<const dyf::Backends::VulkanDevice*>(&device))
        return native->SupportsDdsCompression(format);
#else
    (void)device;(void)format;
#endif
    return false;
}

bool dyf::RHI::Detail::SupportsLinearFiltering(const IDevice& device,Format format) noexcept
{
#if defined(ENABLE_VULKAN)
    if(const auto* native=dynamic_cast<const dyf::Backends::VulkanDevice*>(&device))
        return native->SupportsLinearFiltering(format);
#elif defined(ENABLE_D3D12)
    if(const auto* native=dynamic_cast<const dyf::Backends::D3D12Device*>(&device))
        return native->SupportsLinearFiltering(format);
#elif defined(ENABLE_METAL)
    if(const auto* native=dynamic_cast<const dyf::Backends::MetalDevice*>(&device))
        return native->SupportsLinearFiltering(format);
#else
    (void)device;(void)format;
#endif
    return false;
}
