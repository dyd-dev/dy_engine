#if defined(ENABLE_D3D12)
#include "Backends/D3D12/D3D12Device.h"
#elif defined(ENABLE_METAL)
#include "Backends/Metal/MetalDevice.h"
#elif defined(ENABLE_VULKAN)
#include "Backends/Vulkan/VulkanDevice.h"
#else
#include "Backends/Null/NullDevice.h"
#endif

#include <iostream>
#include <mutex>
#include <unordered_set>

using namespace dy::RHI;

struct IDevice::ResourceCounterState
{
	mutable std::mutex mutex;
	ResourceAllocationCounters counters = {};
	std::unordered_set<const void*> buffers;
	std::unordered_set<const void*> textures;
	std::unordered_set<const void*> pipelines;
};

IDevice::IDevice()
	: m_resourceCounterState(std::make_unique<ResourceCounterState>())
{
}

IDevice::~IDevice()
{
	const ResourceAllocationCounters counters = GetResourceAllocationCounters();
	if(counters.GetTotalLive() != 0u)
	{
		std::cerr << "[dy_engine] live GPU resource objects at device shutdown: buffers="
			<< counters.buffers.live << ", textures=" << counters.textures.live
			<< ", pipelines=" << counters.pipelines.live << '\n';
	}
}

ResourceAllocationCounters IDevice::GetResourceAllocationCounters() const
{
	if(m_resourceCounterState == nullptr) return {};
	std::lock_guard<std::mutex> lock(m_resourceCounterState->mutex);
	return m_resourceCounterState->counters;
}

bool IDevice::TrackResourceCreated(ResourceKind kind, const void* resource)
{
	if(resource == nullptr || m_resourceCounterState == nullptr) return false;
	std::lock_guard<std::mutex> lock(m_resourceCounterState->mutex);
	std::unordered_set<const void*>* resources = nullptr;
	ResourceAllocationCounter* counter = nullptr;
	switch(kind)
	{
	case ResourceKind::Buffer:
		resources = &m_resourceCounterState->buffers;
		counter = &m_resourceCounterState->counters.buffers;
		break;
	case ResourceKind::Texture:
		resources = &m_resourceCounterState->textures;
		counter = &m_resourceCounterState->counters.textures;
		break;
	case ResourceKind::Pipeline:
		resources = &m_resourceCounterState->pipelines;
		counter = &m_resourceCounterState->counters.pipelines;
		break;
	}
	if(resources == nullptr || counter == nullptr || !resources->insert(resource).second) return false;
	++counter->live;
	++counter->created;
	return true;
}

bool IDevice::TrackResourceDestroyed(ResourceKind kind, const void* resource)
{
	if(resource == nullptr || m_resourceCounterState == nullptr) return false;
	std::lock_guard<std::mutex> lock(m_resourceCounterState->mutex);
	std::unordered_set<const void*>* resources = nullptr;
	ResourceAllocationCounter* counter = nullptr;
	switch(kind)
	{
	case ResourceKind::Buffer:
		resources = &m_resourceCounterState->buffers;
		counter = &m_resourceCounterState->counters.buffers;
		break;
	case ResourceKind::Texture:
		resources = &m_resourceCounterState->textures;
		counter = &m_resourceCounterState->counters.textures;
		break;
	case ResourceKind::Pipeline:
		resources = &m_resourceCounterState->pipelines;
		counter = &m_resourceCounterState->counters.pipelines;
		break;
	}
	if(resources == nullptr || counter == nullptr || resources->erase(resource) == 0u) return false;
	if(counter->live > 0u) --counter->live;
	++counter->destroyed;
	return true;
}

bool IDevice::TrackBufferCreated(IBuffer* buffer) { return TrackResourceCreated(ResourceKind::Buffer, buffer); }
bool IDevice::TrackTextureCreated(ITexture* texture) { return TrackResourceCreated(ResourceKind::Texture, texture); }
bool IDevice::TrackPipelineCreated(IPipelineState* pipeline) { return TrackResourceCreated(ResourceKind::Pipeline, pipeline); }
bool IDevice::TrackBufferDestroyed(IBuffer* buffer) { return TrackResourceDestroyed(ResourceKind::Buffer, buffer); }
bool IDevice::TrackTextureDestroyed(ITexture* texture) { return TrackResourceDestroyed(ResourceKind::Texture, texture); }
bool IDevice::TrackPipelineDestroyed(IPipelineState* pipeline) { return TrackResourceDestroyed(ResourceKind::Pipeline, pipeline); }

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
