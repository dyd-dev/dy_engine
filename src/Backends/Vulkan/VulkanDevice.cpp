#include "VulkanDevice.h"
#include "VulkanQuery.h"

#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/ResourceSet.h"
#include "dyf/RHI/Shader.h"
#include "dyf/RHI/Texture.h"
#include "dyf/RHI/Readback.h"
#include "VulkanCommandList.h"
#include "VulkanContext.h"
#include "VulkanPipeline.h"
#include "VulkanResources.h"
#include "VulkanSwapchain.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace dyf::Backends
{
	struct VulkanObjectDeleter
	{
		template<typename Object>
		void operator()(Object* object) const
		{
			delete object;
		}
	};

	namespace
	{
		constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";

		void LogVulkanFailure(const char* operation, VkResult result)
		{
			std::fprintf(stderr, "%s failed (VkResult %d)\n", operation, static_cast<int>(result));
		}

		bool ValidationLayerAvailable()
		{
			uint32_t count = 0;
			if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) return false;
			std::vector<VkLayerProperties> layers(count);
			if (count > 0 && vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) return false;
			return std::any_of(layers.begin(), layers.end(), [](const VkLayerProperties& layer) {
				return std::strcmp(layer.layerName, kValidationLayerName) == 0;
			});
		}

		VkPresentModeKHR ToPresentMode(dyf::RHI::PresentMode mode)
		{
			switch (mode)
			{
			case dyf::RHI::PresentMode::Immediate: return VK_PRESENT_MODE_IMMEDIATE_KHR;
			case dyf::RHI::PresentMode::Mailbox: return VK_PRESENT_MODE_MAILBOX_KHR;
			case dyf::RHI::PresentMode::Fifo: return VK_PRESENT_MODE_FIFO_KHR;
			default: return VK_PRESENT_MODE_MAX_ENUM_KHR;
			}
		}
	}

	struct VulkanDevice::Impl
	{
		explicit Impl(VulkanDevice& owner) : m_owner(owner) {}
		~Impl();
        const VulkanContext& Context() const {return m_context;}
		bool IsLost() const {return m_submissionFaulted;}
        bool Supports(RHI::Feature feature) const
        {
            VkPhysicalDeviceFeatures supported{};
            vkGetPhysicalDeviceFeatures(m_context.physicalDevice, &supported);
            switch(feature)
            {
            case RHI::Feature::Compute: {
                uint32_t count=0;
                vkGetPhysicalDeviceQueueFamilyProperties(m_context.physicalDevice,&count,nullptr);
                std::vector<VkQueueFamilyProperties> families(count);
                vkGetPhysicalDeviceQueueFamilyProperties(m_context.physicalDevice,&count,families.data());
                return (families[m_context.queueIndices.graphicsFamily].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
            }
            case RHI::Feature::Rasterization:
            case RHI::Feature::SamplerLodBias:
            case RHI::Feature::FractionalDepthBias: return true;
            case RHI::Feature::DescriptorIndexing: return supported.shaderSampledImageArrayDynamicIndexing == VK_TRUE;
            case RHI::Feature::Wireframe: return supported.fillModeNonSolid == VK_TRUE;
            case RHI::Feature::DepthBiasClamp: return supported.depthBiasClamp == VK_TRUE;
            default: return false;
            }
        }
        uint64_t GetLastSubmission() const {return m_lastSubmission;}
        uint64_t GetCompletedSubmission() {CollectCompletedSubmissions(); return m_submissionFaulted?0:m_completedSubmission;}
        void DiscardCommandList(RHI::ICommandList* list) {auto& active=m_acquiredCommandLists;active.erase(std::remove_if(active.begin(),active.end(),[list](const auto& value){return value.get()==list;}),active.end());}
        void WaitForShutdown() { if(m_context.device != VK_NULL_HANDLE) vkDeviceWaitIdle(m_context.device); }
        bool WaitIdle() { if(m_context.device == VK_NULL_HANDLE || vkDeviceWaitIdle(m_context.device) != VK_SUCCESS) return false; CollectCompletedSubmissions(); return !m_submissionFaulted; }
        void ClearSwapchain() { DestroyCurrentSwapchain(); DestroyRetiredSwapchains(); m_hasSwapchainDesc = false; }

		int Initialize(const void* windowHandle, const dyf::RHI::DeviceDesc& desc);
		bool CreateSwapchain(const dyf::RHI::SwapchainDesc& desc, VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE);
		bool BeginFrame();
		dyf::RHI::ICommandList* AcquireCommandList();
		bool Submit(dyf::RHI::ICommandList** commandLists, uint32_t count);
		bool Present();
		dyf::RHI::TextureHandle GetBackBuffer() const { return m_backBuffer; }
		bool ReadTexture(dyf::RHI::TextureHandle texture, dyf::RHI::TextureReadback& result);

		dyf::RHI::BufferHandle CreateBuffer(const dyf::RHI::BufferDesc& desc);
		dyf::RHI::TextureHandle CreateTexture(const dyf::RHI::TextureDesc& desc);
		dyf::RHI::ShaderHandle CreateShader(const dyf::RHI::ShaderDesc& desc);
		RHI::PipelineHandle CreateComputePipeline(const RHI::ComputePipelineDesc& desc) {try {auto* pipeline=new VulkanPipeline(m_context,desc);m_pipelines.push_back(pipeline);return pipeline;}catch(const std::exception&){return nullptr;}}
        dyf::RHI::PipelineHandle CreateGraphicsPipeline(const dyf::RHI::GraphicsPipelineDesc& desc);
		dyf::RHI::ResourceSetHandle CreateResourceSet(const dyf::RHI::ResourceSetDesc& desc);
		void DestroyBuffer(dyf::RHI::BufferHandle buffer);
		void DestroyTexture(dyf::RHI::TextureHandle texture);
		void DestroyShader(dyf::RHI::ShaderHandle shader);
		void DestroyPipeline(dyf::RHI::PipelineHandle pipeline);
		void DestroyResourceSet(dyf::RHI::ResourceSetHandle resourceSet);
		bool UpdateBuffer(dyf::RHI::ICommandList& commandList, dyf::RHI::BufferHandle buffer, uint32_t offset, const void* data, uint32_t size);
		bool UpdateTexture(
			dyf::RHI::ICommandList& commandList,
			dyf::RHI::TextureHandle texture,
			uint32_t mipLevel,
			uint32_t arrayLayer,
			const void* data,
			uint32_t dataSize,
			uint32_t rowPitch,
			uint32_t slicePitch);

	private:
		VulkanDevice& m_owner;
		bool CreateInstance();
		VkResult CreateSurface();
		bool ReportSwapchainFailure(const VulkanSwapchain::InitializationStatus& status);
		bool PickPhysicalDevice();
		bool CreateLogicalDevice();
		bool CreateSwapchainSyncObjects();
		bool RecreateSwapchain();
		VkSwapchainKHR RetireSwapchainGeneration();
		void UpdateBackBuffers();
		void CollectCompletedSubmissions();
		void DestroySwapchainSyncObjects();
		void DestroyCurrentSwapchain();
		void DestroyRetiredSwapchains();
		void DestroyDeviceResources();

		VulkanContext m_context;
		VulkanSwapchain m_swapchain;
		void* m_windowHandle = nullptr;
		uint32_t m_maxFramesInFlight = 0;
        uint32_t m_adapterIndex=0;
        bool m_enableValidation=false;

		std::vector<VkSemaphore> m_imageAvailableSemaphores;
		std::vector<VkSemaphore> m_renderFinishedSemaphores;
		std::vector<VkFence> m_frameSlots;
		std::vector<VkFence> m_imagesInFlight;

		uint64_t m_lastSubmission=0, m_completedSubmission=0;
        struct SubmissionRecord
		{
            uint64_t serial=0;
			VkFence fence = VK_NULL_HANDLE;
			std::vector<std::unique_ptr<VulkanCommandList, VulkanObjectDeleter>> commandLists;
			uint32_t frameSlot = std::numeric_limits<uint32_t>::max();
			uint32_t imageIndex = std::numeric_limits<uint32_t>::max();
		};
		std::vector<SubmissionRecord> m_submissions;
		std::vector<std::unique_ptr<VulkanCommandList, VulkanObjectDeleter>> m_acquiredCommandLists;

		struct RetiredSwapchainGeneration
		{
			VulkanSwapchain swapchain;
			std::vector<VkSemaphore> imageAvailableSemaphores;
			std::vector<VkSemaphore> renderFinishedSemaphores;
			std::vector<std::unique_ptr<VulkanTexture, VulkanObjectDeleter>> backBuffers;
		};
		std::vector<RetiredSwapchainGeneration> m_retiredSwapchains;
		// 새 swapchain의 첫 present가 끝났다는 증거: 같은 이미지의 acquire를 기다린 제출.
		// https://docs.vulkan.org/samples/latest/samples/api/swapchain_recreation/README.html
		// 재획득 전에 다시 resize되면 다음 세대에서 증거를 얻을 때까지 여러 세대를 보유한다.
		uint32_t m_retiredReleaseImage = UINT32_MAX;
		bool m_retiredReleaseAcquired = false;
		uint64_t m_retiredReleaseSubmission = 0;

		std::vector<dyf::RHI::BufferHandle> m_buffers;
		std::vector<dyf::RHI::TextureHandle> m_textures;
		std::vector<dyf::RHI::ShaderHandle> m_shaders;
		std::vector<dyf::RHI::PipelineHandle> m_pipelines;
		std::vector<dyf::RHI::ResourceSetHandle> m_resourceSets;

		std::vector<std::unique_ptr<VulkanTexture, VulkanObjectDeleter>> m_backBuffers;
		dyf::RHI::TextureHandle m_backBuffer = nullptr;
		dyf::RHI::SwapchainDesc m_swapchainDesc = {};
		VkSwapchainKHR m_recreationOldSwapchain = VK_NULL_HANDLE;
		uint32_t m_currentFrameSlot = 0;
		uint32_t m_currentImageIndex = 0;
		bool m_hasSwapchainDesc = false;
		bool m_frameReady = false;
		bool m_imageAcquired = false;
		bool m_presentPending = false;
		bool m_swapchainNeedsRecreate = false;
		bool m_recreateAfterPresent = false;
		bool m_submissionFaulted = false;
	};

	VulkanDevice::VulkanDevice()
		: m_impl(std::make_unique<Impl>(*this))
	{
	}

	VulkanDevice::~VulkanDevice() { m_impl->WaitForShutdown(); ReleaseResources(); }


uint64_t VulkanDevice::GetLastSubmissionNative() const {return m_impl->GetLastSubmission();}
uint64_t VulkanDevice::GetCompletedSubmissionNative() {return m_impl->GetCompletedSubmission();}
void VulkanDevice::DiscardCommandListNative(RHI::ICommandList* list) {m_impl->DiscardCommandList(list);}

bool VulkanDevice::SupportsNative(RHI::Feature feature) const
{return feature==RHI::Feature::TimestampQuery ? GetTimestampValidBitsNative()!=0 : m_impl->Supports(feature);}
RHI::TimestampQueryHandle VulkanDevice::CreateTimestampQueryNative(const RHI::TimestampQueryDesc& desc)
{
    try {return new VulkanTimestampQuery(m_impl->Context().device,desc.count);}
    catch(const std::exception&) {return nullptr;}
}
void VulkanDevice::DestroyTimestampQueryNative(RHI::TimestampQueryHandle query)
{delete static_cast<VulkanTimestampQuery*>(query);}
bool VulkanDevice::ReadTimestampsNative(RHI::TimestampQueryHandle query,uint32_t first,uint32_t count,uint64_t* ticks)
{
    return vkGetQueryPoolResults(m_impl->Context().device,static_cast<VulkanTimestampQuery*>(query)->pool,
        first,count,count*sizeof(uint64_t),ticks,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)==VK_SUCCESS;
}
double VulkanDevice::GetTimestampPeriodNative() const
{
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(m_impl->Context().physicalDevice,&properties);
    return properties.limits.timestampPeriod;
}
uint32_t VulkanDevice::GetTimestampValidBitsNative() const
{
    const auto& context=m_impl->Context();
    uint32_t count=0;
    vkGetPhysicalDeviceQueueFamilyProperties(context.physicalDevice,&count,nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(context.physicalDevice,&count,families.data());
    return families[context.queueIndices.graphicsFamily].timestampValidBits;
}
bool VulkanDevice::IsLostNative() const {return m_impl->IsLost();}
bool VulkanDevice::WaitIdleNative() { return m_impl->WaitIdle(); }
void VulkanDevice::DestroySwapchainNative() { m_impl->ClearSwapchain(); }


    uint64_t VulkanDevice::GetLimitNative(RHI::Limit limit) const
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(m_impl->Context().physicalDevice, &properties);
        const auto& limits=properties.limits;
        switch(limit)
        {
        case RHI::Limit::InlineConstantBytes: return limits.maxPushConstantsSize;
        case RHI::Limit::Texture2DDimension: return limits.maxImageDimension2D;
        case RHI::Limit::UniformBufferOffsetAlignment: return limits.minUniformBufferOffsetAlignment;
        case RHI::Limit::StorageBufferOffsetAlignment: return limits.minStorageBufferOffsetAlignment;
        case RHI::Limit::UniformBufferBytes: return limits.maxUniformBufferRange;
        case RHI::Limit::StorageBufferBytes: return limits.maxStorageBufferRange;
        case RHI::Limit::SamplerAnisotropy: {
            VkPhysicalDeviceFeatures features{};
            vkGetPhysicalDeviceFeatures(m_impl->Context().physicalDevice, &features);
            return features.samplerAnisotropy ? static_cast<uint64_t>(limits.maxSamplerAnisotropy) : 1;
        }
        }
        return 0;
    }

    bool VulkanDevice::SupportsSamplerNative(const RHI::SamplerDesc& desc) const
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(m_impl->Context().physicalDevice, &properties);
        return std::abs(desc.mipLodBias) <= properties.limits.maxSamplerLodBias;
    }

    bool VulkanDevice::SupportsPipelineLayoutNative(const RHI::PipelineLayoutDesc& desc) const
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(m_impl->Context().physicalDevice, &properties);
        const auto& limits=properties.limits;
        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(m_impl->Context().physicalDevice,&features);
        // 레이아웃 전체와 각 셰이더 단계의 실제 디스크립터 수를 네이티브 한도와 비교한다.
        uint64_t counts[4][5]={};
        for(uint32_t i=0;i<desc.bindingCount;++i)
        {
            const auto& binding=desc.bindings[i];
            uint32_t kind=0;
            switch(binding.type)
            {
            case RHI::ResourceBindingType::StaticSampler: kind=0;break;
            case RHI::ResourceBindingType::ConstantBuffer: kind=1;break;
            case RHI::ResourceBindingType::ReadOnlyStorageBuffer:
            case RHI::ResourceBindingType::ReadWriteStorageBuffer: kind=2;break;
            case RHI::ResourceBindingType::SampledTexture: kind=3;break;
            case RHI::ResourceBindingType::StorageTexture: kind=4;break;
            default:return false;
            }
            if(binding.type==RHI::ResourceBindingType::ReadWriteStorageBuffer ||
                binding.type==RHI::ResourceBindingType::StorageTexture)
            {
                if((binding.stages&RHI::ShaderStageFlags::Vertex)!=RHI::ShaderStageFlags::None &&
                    !features.vertexPipelineStoresAndAtomics)return false;
                if((binding.stages&RHI::ShaderStageFlags::Fragment)!=RHI::ShaderStageFlags::None &&
                    !features.fragmentStoresAndAtomics)return false;
            }
            counts[0][kind]+=binding.count;
            for(uint32_t stage=0;stage<3;++stage)
                if((static_cast<uint32_t>(binding.stages)&(1u<<stage))!=0) counts[stage+1][kind]+=binding.count;
        }
        const uint64_t totalLimits[]={limits.maxDescriptorSetSamplers,limits.maxDescriptorSetUniformBuffers,
            limits.maxDescriptorSetStorageBuffers,limits.maxDescriptorSetSampledImages,limits.maxDescriptorSetStorageImages};
        const uint64_t stageLimits[]={limits.maxPerStageDescriptorSamplers,limits.maxPerStageDescriptorUniformBuffers,
            limits.maxPerStageDescriptorStorageBuffers,limits.maxPerStageDescriptorSampledImages,limits.maxPerStageDescriptorStorageImages};
        for(uint32_t kind=0;kind<5;++kind) if(counts[0][kind]>totalLimits[kind])return false;
        for(uint32_t stage=1;stage<4;++stage)
        {
            uint64_t resources=0;
            for(uint32_t kind=0;kind<5;++kind)
            {
                if(counts[stage][kind]>stageLimits[kind])return false;
                if(kind!=0)resources+=counts[stage][kind];
            }
            if(resources>limits.maxPerStageResources)return false;
        }
        return true;
    }

    bool VulkanDevice::SupportsGraphicsPipelineNative(const RHI::GraphicsPipelineDesc& desc) const
    {
        const auto physical=m_impl->Context().physicalDevice;
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        const auto& limits=properties.limits;
        if(desc.vertexBufferCount>limits.maxVertexInputBindings || desc.vertexAttributeCount>limits.maxVertexInputAttributes ||
            desc.colorAttachmentCount>limits.maxColorAttachments)return false;
        for(uint32_t i=0;i<desc.vertexBufferCount;++i)
            if(desc.vertexBuffers[i].binding>=limits.maxVertexInputBindings ||
                desc.vertexBuffers[i].stride>limits.maxVertexInputBindingStride)return false;
        const auto hasFormat=[&](RHI::Format format,VkFormatFeatureFlags feature,bool buffer) {
            VkFormatProperties supported{};
            vkGetPhysicalDeviceFormatProperties(physical,ToVulkanFormat(format),&supported);
            return ((buffer?supported.bufferFeatures:supported.optimalTilingFeatures)&feature)==feature;
        };
        for(uint32_t i=0;i<desc.vertexAttributeCount;++i)
        {
            const auto& attribute=desc.vertexAttributes[i];
            if(attribute.location>=limits.maxVertexInputAttributes || attribute.offset>limits.maxVertexInputAttributeOffset ||
                !hasFormat(attribute.format,VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT,true))return false;
        }
        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(physical,&features);
        const auto sameBlend=[](const RHI::ColorAttachmentDesc& a,const RHI::ColorAttachmentDesc& b) {
            return a.writeMask==b.writeMask && a.blend.enabled==b.blend.enabled &&
                a.blend.sourceColor==b.blend.sourceColor && a.blend.destinationColor==b.blend.destinationColor &&
                a.blend.colorOp==b.blend.colorOp && a.blend.sourceAlpha==b.blend.sourceAlpha &&
                a.blend.destinationAlpha==b.blend.destinationAlpha && a.blend.alphaOp==b.blend.alphaOp;
        };
        for(uint32_t i=0;i<desc.colorAttachmentCount;++i)
        {
            const auto& attachment=desc.colorAttachments[i];
            if(i && !features.independentBlend && !sameBlend(attachment,desc.colorAttachments[0]))return false;
            VkFormatFeatureFlags required=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
            if(attachment.blend.enabled)required|=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
            if(!hasFormat(attachment.format,required,false))return false;
        }
        return desc.depthStencil.format==RHI::Format::Unknown ||
            hasFormat(desc.depthStencil.format,VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT,false);
    }

    bool VulkanDevice::CreateSwapchainNative(const dyf::RHI::SwapchainDesc& desc)
	{
		return m_impl->CreateSwapchain(desc);
	}

	bool VulkanDevice::BeginFrameNative()
	{
		return m_impl->BeginFrame();
	}

	dyf::RHI::ICommandList* VulkanDevice::AcquireCommandListNative()
	{
		return m_impl->AcquireCommandList();
	}

	bool VulkanDevice::SubmitNative(dyf::RHI::ICommandList** commandLists, uint32_t count)
	{
		return m_impl->Submit(commandLists, count);
	}

	bool VulkanDevice::PresentNative()
	{
		return m_impl->Present();
	}

	dyf::RHI::TextureHandle VulkanDevice::GetBackBufferNative()
	{
		return m_impl->GetBackBuffer();
	}

	dyf::RHI::BufferHandle VulkanDevice::CreateBufferNative(const dyf::RHI::BufferDesc& desc)
	{
		return m_impl->CreateBuffer(desc);
	}

	dyf::RHI::TextureHandle VulkanDevice::CreateTextureNative(const dyf::RHI::TextureDesc& desc)
	{
		return m_impl->CreateTexture(desc);
	}

	dyf::RHI::ShaderHandle VulkanDevice::CreateShaderNative(const dyf::RHI::ShaderDesc& desc)
	{
		return m_impl->CreateShader(desc);
	}

RHI::PipelineHandle VulkanDevice::CreateComputePipelineNative(const RHI::ComputePipelineDesc& desc) {return m_impl->CreateComputePipeline(desc);}
	dyf::RHI::PipelineHandle VulkanDevice::CreateGraphicsPipelineNative(const dyf::RHI::GraphicsPipelineDesc& desc)
	{
		return m_impl->CreateGraphicsPipeline(desc);
	}

	dyf::RHI::ResourceSetHandle VulkanDevice::CreateResourceSetNative(const dyf::RHI::ResourceSetDesc& desc)
	{
		return m_impl->CreateResourceSet(desc);
	}

	void VulkanDevice::DestroyBufferNative(dyf::RHI::BufferHandle buffer)
	{
		m_impl->DestroyBuffer(buffer);
	}

	void VulkanDevice::DestroyTextureNative(dyf::RHI::TextureHandle texture)
	{
		m_impl->DestroyTexture(texture);
	}

	void VulkanDevice::DestroyShaderNative(dyf::RHI::ShaderHandle shader)
	{
		m_impl->DestroyShader(shader);
	}

	void VulkanDevice::DestroyPipelineNative(dyf::RHI::PipelineHandle pipeline)
	{
		m_impl->DestroyPipeline(pipeline);
	}

	void VulkanDevice::DestroyResourceSetNative(dyf::RHI::ResourceSetHandle resourceSet)
	{
		m_impl->DestroyResourceSet(resourceSet);
	}

	bool VulkanDevice::UpdateBufferNative(dyf::RHI::ICommandList& commandList, dyf::RHI::BufferHandle buffer, uint32_t offset, const void* data, uint32_t size)
	{
		return m_impl->UpdateBuffer(commandList, buffer, offset, data, size);
	}

	bool VulkanDevice::UpdateTextureNative(
		dyf::RHI::ICommandList& commandList,
		dyf::RHI::TextureHandle texture,
		uint32_t mipLevel,
		uint32_t arrayLayer,
		const void* data,
		uint32_t dataSize,
		uint32_t rowPitch,
		uint32_t slicePitch)
	{
		return m_impl->UpdateTexture(commandList, texture, mipLevel, arrayLayer, data, dataSize, rowPitch, slicePitch);
	}

	int VulkanDevice::Initialize(const void* windowHandle, const dyf::RHI::DeviceDesc& desc)
	{
		return m_impl->Initialize(windowHandle, desc);
	}

	VulkanDevice::Impl::~Impl()
	{
		DestroyDeviceResources();
	}

	int VulkanDevice::Impl::Initialize(const void* windowHandle, const dyf::RHI::DeviceDesc& desc)
	{
		m_windowHandle = const_cast<void*>(windowHandle);
		m_maxFramesInFlight = desc.maxFramesInFlight;
        m_adapterIndex=desc.adapterIndex;
        m_enableValidation=desc.enableValidation;
		if (m_maxFramesInFlight == 0) return -1;

		if (!CreateInstance() || !PickPhysicalDevice() || !CreateLogicalDevice()) return -1;
		m_frameSlots.assign(m_maxFramesInFlight, VK_NULL_HANDLE);
		return 0;
	}

	bool VulkanDevice::Impl::ReportSwapchainFailure(const VulkanSwapchain::InitializationStatus& status)
	{
		if (status.retryLater) return false;
		if (status.operation != nullptr)
		{
			LogVulkanFailure(status.operation, status.result);
			m_submissionFaulted = true;
		}
		else if (m_hasSwapchainDesc)
		{
			std::fprintf(stderr, "Vulkan swapchain recreation cannot satisfy the requested configuration.\n");
			m_submissionFaulted = true;
		}
		return false;
	}

	bool VulkanDevice::Impl::CreateSwapchain(const dyf::RHI::SwapchainDesc& desc, VkSwapchainKHR oldSwapchain)
	{
        if(m_windowHandle && m_windowHandle!=desc.window) return false;
        m_windowHandle=const_cast<void*>(desc.window);
        if(!m_windowHandle) return false;
        VulkanSwapchain::InitializationStatus status;
        if(m_context.surface==VK_NULL_HANDLE && !status.Check(CreateSurface(), "Vulkan surface creation"))
            return ReportSwapchainFailure(status);
        VkBool32 presentation=VK_FALSE;
        if(!status.Check(vkGetPhysicalDeviceSurfaceSupportKHR(m_context.physicalDevice,m_context.queueIndices.graphicsFamily,m_context.surface,&presentation), "vkGetPhysicalDeviceSurfaceSupportKHR") || !presentation)
            return ReportSwapchainFailure(status);
		if (m_context.device == VK_NULL_HANDLE || m_windowHandle == nullptr || m_submissionFaulted || desc.minimumImageCount == 0) return false;
		CollectCompletedSubmissions();
		if (m_submissionFaulted || !m_submissions.empty() || m_frameReady || m_imageAcquired || m_presentPending ||
			m_swapchain.GetHandle() != VK_NULL_HANDLE)
		{
			return false;
		}

		const VkFormat requestedFormat = ToVulkanFormat(desc.format);
		if (requestedFormat == VK_FORMAT_UNDEFINED) return false;
        VkColorSpaceKHR requestedColorSpace=VK_COLOR_SPACE_MAX_ENUM_KHR;
        switch(desc.colorSpace)
        {
        case RHI::ColorSpace::Srgb: requestedColorSpace=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;break;
        case RHI::ColorSpace::LinearSrgb: requestedColorSpace=VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;break;
        default:return false;
        }
        VkCompositeAlphaFlagBitsKHR requestedCompositeAlpha=VK_COMPOSITE_ALPHA_FLAG_BITS_MAX_ENUM_KHR;
        switch(desc.compositeAlpha)
        {
        case RHI::CompositeAlpha::Opaque: requestedCompositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;break;
        case RHI::CompositeAlpha::Premultiplied: requestedCompositeAlpha=VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;break;
        case RHI::CompositeAlpha::Postmultiplied: requestedCompositeAlpha=VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;break;
        case RHI::CompositeAlpha::Inherit: requestedCompositeAlpha=VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;break;
        default:return false;
        }
		const VkPresentModeKHR requestedPresentMode = ToPresentMode(desc.presentMode);
		if (requestedPresentMode == VK_PRESENT_MODE_MAX_ENUM_KHR) return false;

		bool oldSwapchainRetired = false;
		try
		{
			if (!m_swapchain.Initialize(
				m_context,
				m_windowHandle,
				requestedFormat,
                requestedColorSpace,
                requestedCompositeAlpha,
				requestedPresentMode,
				desc.minimumImageCount,
				desc.allowReadback,
				m_hasSwapchainDesc ? 0 : desc.initialWidth,
				m_hasSwapchainDesc ? 0 : desc.initialHeight,
				oldSwapchain,
				oldSwapchainRetired,
				status))
			{
				if (oldSwapchainRetired) m_recreationOldSwapchain = VK_NULL_HANDLE;
				return ReportSwapchainFailure(status);
			}
			if (oldSwapchainRetired) m_recreationOldSwapchain = VK_NULL_HANDLE;

			UpdateBackBuffers();
			if (!CreateSwapchainSyncObjects())
			{
				DestroyCurrentSwapchain();
				return false;
			}
		}
		catch (const std::exception& exception)
		{
			std::fprintf(stderr, "Vulkan swapchain creation failed: %s\n", exception.what());
			m_submissionFaulted = true;
			if (oldSwapchainRetired) m_recreationOldSwapchain = VK_NULL_HANDLE;
			DestroyCurrentSwapchain();
			return false;
		}

		m_swapchainDesc = desc;
		m_hasSwapchainDesc = true;
		m_currentFrameSlot = 0;
		m_currentImageIndex = 0;
		m_frameReady = false;
		m_imageAcquired = false;
		m_presentPending = false;
		m_swapchainNeedsRecreate = false;
		m_recreateAfterPresent = false;
		m_retiredReleaseImage = UINT32_MAX;
		m_retiredReleaseAcquired = false;
		m_retiredReleaseSubmission = 0;
		if (m_swapchain.GetImageCount() > desc.minimumImageCount)
		{
			char message[192];
			std::snprintf(message, sizeof(message),
				"Vulkan swapchain requested at least %u images; the surface/driver allocated %zu.",
				desc.minimumImageCount, m_swapchain.GetImageCount());
			m_owner.ReportDiagnostic(DiagnosticSeverity::Info, message);
		}
		return true;
	}

	bool VulkanDevice::Impl::BeginFrame()
	{
		if (m_context.device == VK_NULL_HANDLE || m_submissionFaulted) return false;
		CollectCompletedSubmissions();
		if (m_submissionFaulted) return false;
		if (m_frameReady)
		{
			return m_imageAcquired && m_swapchain.GetHandle() != VK_NULL_HANDLE &&
				m_currentFrameSlot < m_frameSlots.size() &&
				m_frameSlots[m_currentFrameSlot] == VK_NULL_HANDLE &&
				m_currentImageIndex < m_imagesInFlight.size() &&
				m_imagesInFlight[m_currentImageIndex] == VK_NULL_HANDLE &&
				m_currentImageIndex < m_backBuffers.size() &&
				m_backBuffer == m_backBuffers[m_currentImageIndex].get();
		}

		if (m_swapchainNeedsRecreate)
		{
			if (!m_submissions.empty() || m_imageAcquired || !RecreateSwapchain()) return false;
		}
		if (m_swapchain.GetHandle() == VK_NULL_HANDLE || m_currentFrameSlot >= m_frameSlots.size() ||
			m_frameSlots[m_currentFrameSlot] != VK_NULL_HANDLE)
		{
			return false;
		}

		if (!m_imageAcquired)
		{
			const VkResult result = vkAcquireNextImageKHR(
				m_context.device,
				m_swapchain.GetHandle(),
				0,
				m_imageAvailableSemaphores[m_currentFrameSlot],
				VK_NULL_HANDLE,
				&m_currentImageIndex);
			if (result == VK_ERROR_OUT_OF_DATE_KHR)
			{
				m_swapchainNeedsRecreate = true;
				return false;
			}
			if (result == VK_NOT_READY || result == VK_TIMEOUT) return false;
			if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
			{
				LogVulkanFailure("vkAcquireNextImageKHR", result);
				m_submissionFaulted = true;
				return false;
			}
			m_imageAcquired = true;
			m_recreateAfterPresent = result == VK_SUBOPTIMAL_KHR;
			if (!m_retiredSwapchains.empty() && m_currentImageIndex == m_retiredReleaseImage)
				m_retiredReleaseAcquired = true;
		}

		if (m_currentImageIndex >= m_imagesInFlight.size() || m_currentImageIndex >= m_backBuffers.size())
		{
			m_submissionFaulted = true;
			return false;
		}
		if (m_imagesInFlight[m_currentImageIndex] != VK_NULL_HANDLE) return false;

		m_backBuffer = m_backBuffers[m_currentImageIndex].get();
		m_frameReady = true;
        // RHI의 backbuffer 시작 상태 Present를 최초 Vulkan 이미지의 Undefined 레이아웃에 대응시킨다.
        if(m_backBuffers[m_currentImageIndex]->GetBarrierOldLayout(RHI::ResourceState::Present)==VK_IMAGE_LAYOUT_UNDEFINED)
        {
            auto* command=static_cast<VulkanCommandList*>(AcquireCommandList());
            if(!command)return false;
            RHI::ResourceBarrierDesc barrier{};
            barrier.texture=m_backBuffer;
            barrier.before=RHI::ResourceState::Present;
            barrier.after=RHI::ResourceState::Present;
            command->ResourceBarrierNative(&barrier,1);
            RHI::ICommandList* submitted=command;
            if(!command->CloseNative() || !Submit(&submitted,1))
            { m_submissionFaulted=true;return false; }
            m_backBuffers[m_currentImageIndex]->MarkLayoutInitialized();
        }
		return true;
	}

	dyf::RHI::ICommandList* VulkanDevice::Impl::AcquireCommandList()
	{
		CollectCompletedSubmissions();
		if (m_context.device == VK_NULL_HANDLE || m_submissionFaulted) return nullptr;
		try
		{
			std::unique_ptr<VulkanCommandList, VulkanObjectDeleter> commandList(
				new VulkanCommandList(m_context));
			VulkanCommandList* result = commandList.get();
			m_acquiredCommandLists.push_back(std::move(commandList));
			return result;
		}
		catch (const std::exception& exception)
		{
			std::fprintf(stderr, "Vulkan command-list acquisition failed: %s\n", exception.what());
			return nullptr;
		}
	}

	bool VulkanDevice::Impl::Submit(dyf::RHI::ICommandList** commandLists, uint32_t count)
	{
		if (commandLists == nullptr || count == 0) return false;
		CollectCompletedSubmissions();

		std::vector<VulkanCommandList*> selected;
		selected.reserve(count);
		for (uint32_t i = 0; i < count; ++i)
		{
			if (commandLists[i] == nullptr ||
				std::find(commandLists, commandLists + i, commandLists[i]) != commandLists + i)
			{
				return false;
			}
			const auto acquired = std::find_if(
				m_acquiredCommandLists.begin(),
				m_acquiredCommandLists.end(),
				[requested = commandLists[i]](
					const std::unique_ptr<VulkanCommandList, VulkanObjectDeleter>& candidate) {
					return candidate.get() == requested;
				});
			if (acquired == m_acquiredCommandLists.end() || !(*acquired)->IsClosed()) return false;
			selected.push_back(acquired->get());
		}

		std::vector<std::unique_ptr<VulkanCommandList, VulkanObjectDeleter>> consumed;
		consumed.reserve(count);
		for (VulkanCommandList* commandList : selected)
		{
			const auto acquired = std::find_if(
				m_acquiredCommandLists.begin(),
				m_acquiredCommandLists.end(),
				[commandList](
					const std::unique_ptr<VulkanCommandList, VulkanObjectDeleter>& candidate) {
					return candidate.get() == commandList;
				});
			consumed.push_back(std::move(*acquired));
			m_acquiredCommandLists.erase(acquired);
		}
		if (m_context.device == VK_NULL_HANDLE || m_submissionFaulted) return false;

		VulkanTexture* activeBackBuffer = nullptr;
		if (m_frameReady && m_imageAcquired && m_currentImageIndex < m_backBuffers.size())
		{
			activeBackBuffer = m_backBuffers[m_currentImageIndex].get();
		}
		bool frameSubmission = false;
		for (VulkanCommandList* commandList : selected)
		{
			if (!commandList->IsValid()) return false;
			for (VulkanTexture* image : commandList->GetReferencedSwapchainImages())
			{
				if (activeBackBuffer == nullptr || image != activeBackBuffer) return false;
				frameSubmission = true;
			}
		}
		if (frameSubmission && (!m_frameReady || !m_imageAcquired ||
			m_currentFrameSlot >= m_frameSlots.size() || m_frameSlots[m_currentFrameSlot] != VK_NULL_HANDLE ||
			m_currentImageIndex >= m_renderFinishedSemaphores.size() ||
			m_currentImageIndex >= m_imagesInFlight.size()))
		{
			return false;
		}

		VulkanSubmissionState resourceStates{};
		for (VulkanCommandList* commandList : selected)
		{
			if (!commandList->ValidateForSubmit(resourceStates)) return false;
		}
        // 중간 제출의 최종 상태는 제한하지 않는다. Present에서 공통 RHI가 검사한다.

		std::vector<VkCommandBuffer> nativeCommandBuffers;
		nativeCommandBuffers.reserve(count);
		for (VulkanCommandList* commandList : selected)
		{
			nativeCommandBuffers.push_back(commandList->GetCommandBuffer());
		}

		SubmissionRecord submission{};
		submission.commandLists = std::move(consumed);
		m_submissions.reserve(m_submissions.size() + 1);
		VkFenceCreateInfo fenceInfo{};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		if (vkCreateFence(m_context.device, &fenceInfo, nullptr, &submission.fence) != VK_SUCCESS) return false;

		const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = count;
		submitInfo.pCommandBuffers = nativeCommandBuffers.data();
		const bool waitsForAcquire = frameSubmission && !m_presentPending;
		if (waitsForAcquire)
		{
			submitInfo.waitSemaphoreCount = 1;
			submitInfo.pWaitSemaphores = &m_imageAvailableSemaphores[m_currentFrameSlot];
			submitInfo.pWaitDstStageMask = &waitStage;
            // 표시 신호는 Present의 마지막 제출에서만 보낸다.
		}

		const VkResult result = vkQueueSubmit(m_context.graphicsQueue, 1, &submitInfo, submission.fence);
		if (result != VK_SUCCESS)
		{
			LogVulkanFailure("vkQueueSubmit", result);
            m_submissionFaulted = true;
            // Keep an uncertain submission alive until shutdown drains the device.
            m_submissions.push_back(std::move(submission));
			return false;
		}
		for (VulkanCommandList* commandList : selected) commandList->CommitResourceStates();

		if (frameSubmission)
		{
            // 획득 세마포어는 첫 이미지 제출에서만 소비한다. 프레임 종료는 Present가 맡는다.
            m_presentPending = true;
		}

		submission.serial=++m_lastSubmission;
		if (waitsForAcquire && m_retiredReleaseAcquired && !m_retiredReleaseSubmission)
			m_retiredReleaseSubmission = submission.serial;
        m_submissions.push_back(std::move(submission));
		return true;
	}

    bool VulkanDevice::Impl::Present()
    {
        if(!m_frameReady || !m_imageAcquired || m_swapchain.GetHandle()==VK_NULL_HANDLE || m_submissionFaulted)return false;
        // 모든 이미지 작업 뒤에서 한 번만 표시 세마포어를 신호한다.
        SubmissionRecord completion{};
        m_submissions.reserve(m_submissions.size()+1);
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        const VkResult fenceCreated=vkCreateFence(m_context.device,&fenceInfo,nullptr,&completion.fence);
        if(fenceCreated!=VK_SUCCESS)
        {
            LogVulkanFailure("vkCreateFence(Present)",fenceCreated);
            m_submissionFaulted=true;
            return false;
        }
        const VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit{};
        submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
        const bool waitsForAcquire = !m_presentPending;
        if(waitsForAcquire)
        {
            submit.waitSemaphoreCount=1;
            submit.pWaitSemaphores=&m_imageAvailableSemaphores[m_currentFrameSlot];
            submit.pWaitDstStageMask=&waitStage;
        }
        submit.signalSemaphoreCount=1;
        submit.pSignalSemaphores=&m_renderFinishedSemaphores[m_currentImageIndex];
        const VkResult submitted=vkQueueSubmit(m_context.graphicsQueue,1,&submit,completion.fence);
        if(submitted!=VK_SUCCESS)
        {
            LogVulkanFailure("vkQueueSubmit(Present)",submitted);
            m_submissionFaulted=true;
            m_submissions.push_back(std::move(completion));
            return false;
        }
        completion.serial=++m_lastSubmission;
		if (waitsForAcquire && m_retiredReleaseAcquired && !m_retiredReleaseSubmission)
			m_retiredReleaseSubmission = completion.serial;
        completion.frameSlot=m_currentFrameSlot;
        completion.imageIndex=m_currentImageIndex;
        m_frameSlots[m_currentFrameSlot]=completion.fence;
        m_imagesInFlight[m_currentImageIndex]=completion.fence;
        m_submissions.push_back(std::move(completion));

        VkSwapchainKHR swapchain=m_swapchain.GetHandle();
        VkPresentInfoKHR info{};
        info.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        info.waitSemaphoreCount=1;
        info.pWaitSemaphores=&m_renderFinishedSemaphores[m_currentImageIndex];
        info.swapchainCount=1;
        info.pSwapchains=&swapchain;
        info.pImageIndices=&m_currentImageIndex;
        const VkResult result=vkQueuePresentKHR(m_context.presentQueue,&info);
		if ((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) && !m_retiredSwapchains.empty() &&
			m_retiredReleaseImage == UINT32_MAX)
			m_retiredReleaseImage = m_currentImageIndex;
        m_presentPending=false;
        m_frameReady=false;
        m_imageAcquired=false;
        m_currentFrameSlot=(m_currentFrameSlot+1)%m_maxFramesInFlight;
        const bool recreateRequestedByAcquire=m_recreateAfterPresent;
        m_recreateAfterPresent=false;
        if(result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR || result==VK_ERROR_OUT_OF_DATE_KHR)
        {
            if(result!=VK_SUCCESS || recreateRequestedByAcquire)m_swapchainNeedsRecreate=true;
            // 크기 변경으로 표시가 생략되어도 프레임은 정상 종료되었다.
            // 다음 BeginFrame이 재생성을 진행하며 준비되지 않은 동안은 프레임을 건너뛴다.
            return true;
        }
        // acquire의 SUBOPTIMAL 신호가 뒤따른 present의 실제 실패를 가리지 않게 한다.
        LogVulkanFailure("vkQueuePresentKHR",result);
        if(result==VK_ERROR_DEVICE_LOST)m_submissionFaulted=true;
        else m_swapchainNeedsRecreate=true;
        return false;
    }

	bool VulkanDevice::ReadTextureNative(dyf::RHI::TextureHandle texture, dyf::RHI::TextureReadback& result)
	{
		return m_impl->ReadTexture(texture, result);
	}

	bool VulkanDevice::Impl::ReadTexture(dyf::RHI::TextureHandle texture, dyf::RHI::TextureReadback& result)
	{
		if(texture == nullptr || m_submissionFaulted || !m_acquiredCommandLists.empty()) return false;
		const bool backBuffer = texture == m_backBuffer;
		if(backBuffer)
		{
			if(!m_swapchainDesc.allowReadback || !m_presentPending) return false;
		}
		else if(std::find(m_textures.begin(), m_textures.end(), texture) == m_textures.end()) return false;
		const auto& desc = texture->GetDesc();
		if(!dyf::RHI::IsReadbackFormat(desc.format) || !desc.width || !desc.height ||
			desc.width > UINT32_MAX / 4u) return false;
		const uint64_t bytes = static_cast<uint64_t>(desc.width) * desc.height * 4u;
		if(bytes > std::numeric_limits<size_t>::max()) return false;
		auto* image = static_cast<VulkanTexture*>(texture);
		const auto state = image->GetState(0, 0);
		if(state == dyf::RHI::ResourceState::Undefined) return false;

		struct Resources
		{
			VkDevice device;
			VkBuffer buffer = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			VkCommandPool pool = VK_NULL_HANDLE;
			~Resources()
			{
				if(pool) vkDestroyCommandPool(device, pool, nullptr);
				if(buffer) vkDestroyBuffer(device, buffer, nullptr);
				if(memory) vkFreeMemory(device, memory, nullptr);
			}
		} resources{m_context.device};
		VkBufferCreateInfo bufferInfo{};
		bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferInfo.size = bytes;
		bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if(vkCreateBuffer(m_context.device, &bufferInfo, nullptr, &resources.buffer) != VK_SUCCESS) return false;
		VkMemoryRequirements requirements{};
		vkGetBufferMemoryRequirements(m_context.device, resources.buffer, &requirements);
		VkPhysicalDeviceMemoryProperties properties{};
		vkGetPhysicalDeviceMemoryProperties(m_context.physicalDevice, &properties);
		uint32_t memoryType = UINT32_MAX;
		for(uint32_t i = 0; i < properties.memoryTypeCount; ++i)
		{
			if((requirements.memoryTypeBits & (1u << i)) &&
				(properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
			{
				memoryType = i;
				if(properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) break;
			}
		}
		if(memoryType == UINT32_MAX) return false;
		VkMemoryAllocateInfo allocation{};
		allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocation.allocationSize = requirements.size;
		allocation.memoryTypeIndex = memoryType;
		if(vkAllocateMemory(m_context.device, &allocation, nullptr, &resources.memory) != VK_SUCCESS ||
			vkBindBufferMemory(m_context.device, resources.buffer, resources.memory, 0) != VK_SUCCESS) return false;
		VkCommandPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.queueFamilyIndex = m_context.queueIndices.graphicsFamily;
		poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
		if(vkCreateCommandPool(m_context.device, &poolInfo, nullptr, &resources.pool) != VK_SUCCESS) return false;
		VkCommandBufferAllocateInfo commandInfo{};
		commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		commandInfo.commandPool = resources.pool;
		commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		commandInfo.commandBufferCount = 1;
		VkCommandBuffer command = VK_NULL_HANDLE;
		if(vkAllocateCommandBuffers(m_context.device, &commandInfo, &command) != VK_SUCCESS) return false;
		VkCommandBufferBeginInfo begin{};
		begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		if(vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) return false;
		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.oldLayout = ToVulkanImageLayout(state);
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image->GetImage();
		barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, 0, nullptr, 0, nullptr, 1, &barrier);
		VkBufferImageCopy copy{};
		copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.imageExtent = {desc.width, desc.height, 1};
		vkCmdCopyImageToBuffer(command, image->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			resources.buffer, 1, &copy);
		VkBufferMemoryBarrier hostRead{};
		hostRead.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		hostRead.srcQueueFamilyIndex = hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		hostRead.buffer = resources.buffer;
		hostRead.size = VK_WHOLE_SIZE;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
			0, 0, nullptr, 1, &hostRead, 0, nullptr);
		std::swap(barrier.oldLayout, barrier.newLayout);
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			0, 0, nullptr, 0, nullptr, 1, &barrier);
		if(vkEndCommandBuffer(command) != VK_SUCCESS) return false;
		// This diagnostic is deliberately synchronous. It does not consume the
		// presentation semaphore or change the application's tracked resource state.
		VkSubmitInfo submit{};
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &command;
		if(vkQueueSubmit(m_context.graphicsQueue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) return false;
		if(vkQueueWaitIdle(m_context.graphicsQueue) != VK_SUCCESS)
		{
			m_submissionFaulted = true;
			return false;
		}
		dyf::RHI::TextureReadback output;
		output.width = desc.width; output.height = desc.height;
		output.rowPitch = desc.width * 4u; output.format = desc.format;
		output.pixels.resize(static_cast<size_t>(bytes));
		void* mapped = nullptr;
		if(vkMapMemory(m_context.device, resources.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) return false;
		if(!(properties.memoryTypes[memoryType].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
		{
			VkMappedMemoryRange range{};
			range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
			range.memory = resources.memory;
			range.size = VK_WHOLE_SIZE;
			if(vkInvalidateMappedMemoryRanges(m_context.device, 1, &range) != VK_SUCCESS)
			{
				vkUnmapMemory(m_context.device, resources.memory);
				return false;
			}
		}
		std::memcpy(output.pixels.data(), mapped, output.pixels.size());
		vkUnmapMemory(m_context.device, resources.memory);
		result = std::move(output);
		return true;
	}

	dyf::RHI::BufferHandle VulkanDevice::Impl::CreateBuffer(const dyf::RHI::BufferDesc& desc)
	{
		try
		{
			std::unique_ptr<VulkanBuffer, VulkanObjectDeleter> buffer(
				new VulkanBuffer(m_context, desc));
			VulkanBuffer* result = buffer.get();
			m_buffers.push_back(result);
			buffer.release();
			return result;
		}
		catch (const std::exception& exception)
		{
			std::fprintf(stderr, "Vulkan buffer creation failed: %s\n", exception.what());
			return nullptr;
		}
	}

	dyf::RHI::TextureHandle VulkanDevice::Impl::CreateTexture(const dyf::RHI::TextureDesc& desc)
	{
		try
		{
			std::unique_ptr<VulkanTexture, VulkanObjectDeleter> texture(
				new VulkanTexture(m_context, desc));
			VulkanTexture* result = texture.get();
			m_textures.push_back(result);
			texture.release();
			return result;
		}
		catch (const std::exception& exception)
		{
			std::fprintf(stderr, "Vulkan texture creation failed: %s\n", exception.what());
			return nullptr;
		}
	}

	dyf::RHI::ShaderHandle VulkanDevice::Impl::CreateShader(const dyf::RHI::ShaderDesc& desc)
	{
		try
		{
			std::unique_ptr<VulkanShader, VulkanObjectDeleter> shader(
				new VulkanShader(m_context, desc));
			VulkanShader* result = shader.get();
			m_shaders.push_back(result);
			shader.release();
			return result;
		}
		catch (const std::exception& exception)
		{
			std::fprintf(stderr, "Vulkan shader creation failed: %s\n", exception.what());
			return nullptr;
		}
	}

	dyf::RHI::PipelineHandle VulkanDevice::Impl::CreateGraphicsPipeline(const dyf::RHI::GraphicsPipelineDesc& desc)
	{
		if (std::find(m_shaders.begin(), m_shaders.end(), desc.vertexShader) == m_shaders.end() ||
			(desc.fragmentShader != nullptr && std::find(m_shaders.begin(), m_shaders.end(), desc.fragmentShader) == m_shaders.end()))
		{
			return nullptr;
		}
		try
		{
			std::unique_ptr<VulkanPipeline, VulkanObjectDeleter> pipeline(
				new VulkanPipeline(m_context, desc));
			VulkanPipeline* result = pipeline.get();
			m_pipelines.push_back(result);
			pipeline.release();
			return result;
		}
		catch (const std::exception& exception)
		{
			std::fprintf(stderr, "Vulkan pipeline creation failed: %s\n", exception.what());
			return nullptr;
		}
	}

	dyf::RHI::ResourceSetHandle VulkanDevice::Impl::CreateResourceSet(const dyf::RHI::ResourceSetDesc& desc)
	{
		if (std::find(m_pipelines.begin(), m_pipelines.end(), desc.pipeline) == m_pipelines.end() ||
			(desc.bindingCount > 0 && desc.bindings == nullptr))
		{
			return nullptr;
		}
		for (uint32_t i = 0; i < desc.bindingCount; ++i)
		{
			const dyf::RHI::ResourceBinding& binding = desc.bindings[i];
			if ((binding.buffer != nullptr && std::find(m_buffers.begin(), m_buffers.end(), binding.buffer) == m_buffers.end()) ||
				(binding.texture != nullptr && std::find(m_textures.begin(), m_textures.end(), binding.texture) == m_textures.end()))
			{
				return nullptr;
			}
		}
		try
		{
			std::unique_ptr<VulkanResourceSet, VulkanObjectDeleter> resourceSet(
				new VulkanResourceSet(m_context, desc));
			VulkanResourceSet* result = resourceSet.get();
			m_resourceSets.push_back(result);
			resourceSet.release();
			return result;
		}
		catch (const std::exception& exception)
		{
			std::fprintf(stderr, "Vulkan resource-set creation failed: %s\n", exception.what());
			return nullptr;
		}
	}

	void VulkanDevice::Impl::DestroyBuffer(dyf::RHI::BufferHandle buffer)
	{
		const auto found = std::find(m_buffers.begin(), m_buffers.end(), buffer);
		if(found == m_buffers.end()) return;
		m_buffers.erase(found);
		VulkanObjectDeleter{}(static_cast<VulkanBuffer*>(buffer));
    }

	void VulkanDevice::Impl::DestroyTexture(dyf::RHI::TextureHandle texture)
	{
		const auto found = std::find(m_textures.begin(), m_textures.end(), texture);
		if(found == m_textures.end()) return;
		m_textures.erase(found);
		VulkanObjectDeleter{}(static_cast<VulkanTexture*>(texture));
    }

	void VulkanDevice::Impl::DestroyShader(dyf::RHI::ShaderHandle shader)
	{
		const auto found = std::find(m_shaders.begin(), m_shaders.end(), shader);
		if(found == m_shaders.end()) return;
		m_shaders.erase(found);
		VulkanObjectDeleter{}(static_cast<VulkanShader*>(shader));
    }

	void VulkanDevice::Impl::DestroyPipeline(dyf::RHI::PipelineHandle pipeline)
	{
		const auto found = std::find(m_pipelines.begin(), m_pipelines.end(), pipeline);
		if(found == m_pipelines.end()) return;
		m_pipelines.erase(found);
		VulkanObjectDeleter{}(static_cast<VulkanPipeline*>(pipeline));
    }

	void VulkanDevice::Impl::DestroyResourceSet(dyf::RHI::ResourceSetHandle resourceSet)
	{
		const auto found = std::find(m_resourceSets.begin(), m_resourceSets.end(), resourceSet);
		if(found == m_resourceSets.end()) return;
		m_resourceSets.erase(found);
		VulkanObjectDeleter{}(static_cast<VulkanResourceSet*>(resourceSet));
    }

	bool VulkanDevice::Impl::UpdateBuffer(
		dyf::RHI::ICommandList& commandList,
		dyf::RHI::BufferHandle buffer,
		uint32_t offset,
		const void* data,
		uint32_t size)
	{
		VulkanCommandList* nativeCommandList = dynamic_cast<VulkanCommandList*>(&commandList);
		VulkanBuffer* nativeBuffer = dynamic_cast<VulkanBuffer*>(buffer);
		if (nativeCommandList == nullptr || nativeBuffer == nullptr ||
			std::find(m_buffers.begin(), m_buffers.end(), buffer) == m_buffers.end() ||
			std::none_of(m_acquiredCommandLists.begin(), m_acquiredCommandLists.end(),
				[nativeCommandList](
					const std::unique_ptr<VulkanCommandList, VulkanObjectDeleter>& candidate) {
					return candidate.get() == nativeCommandList;
				}))
		{
			return false;
		}
		return nativeCommandList->RecordBufferUpdate(*nativeBuffer, offset, data, size);
	}

	bool VulkanDevice::Impl::UpdateTexture(
		dyf::RHI::ICommandList& commandList,
		dyf::RHI::TextureHandle texture,
		uint32_t mipLevel,
		uint32_t arrayLayer,
		const void* data,
		uint32_t dataSize,
		uint32_t rowPitch,
		uint32_t slicePitch)
	{
		VulkanCommandList* nativeCommandList = dynamic_cast<VulkanCommandList*>(&commandList);
		VulkanTexture* nativeTexture = dynamic_cast<VulkanTexture*>(texture);
		if (nativeCommandList == nullptr || nativeTexture == nullptr ||
			std::find(m_textures.begin(), m_textures.end(), texture) == m_textures.end() ||
			std::none_of(m_acquiredCommandLists.begin(), m_acquiredCommandLists.end(),
				[nativeCommandList](
					const std::unique_ptr<VulkanCommandList, VulkanObjectDeleter>& candidate) {
					return candidate.get() == nativeCommandList;
				}))
		{
			return false;
		}
		return nativeCommandList->RecordTextureUpdate(
			*nativeTexture,
			mipLevel,
			arrayLayer,
			data,
			dataSize,
			rowPitch,
			slicePitch);
	}

	bool VulkanDevice::Impl::CreateInstance()
	{

        uint32_t availableCount=0;
        if(vkEnumerateInstanceExtensionProperties(nullptr,&availableCount,nullptr)!=VK_SUCCESS)return false;
        std::vector<VkExtensionProperties> available(availableCount);
        if(vkEnumerateInstanceExtensionProperties(nullptr,&availableCount,available.data())!=VK_SUCCESS)return false;
        const auto supported=[&](const char* name) {
            return std::any_of(available.begin(),available.end(),[&](const auto& e){return std::strcmp(e.extensionName,name)==0;});
        };
        std::vector<const char*> extensions;
        if(supported(VK_KHR_SURFACE_EXTENSION_NAME))extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#if defined(_WIN32)
        if(supported("VK_KHR_win32_surface"))extensions.push_back("VK_KHR_win32_surface");
#else
        for(const char* name:{"VK_KHR_xlib_surface","VK_KHR_xcb_surface","VK_KHR_wayland_surface"})
            if(supported(name))extensions.push_back(name);
#endif
        if(supported(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME))
            extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
        const uint32_t extensionCount=static_cast<uint32_t>(extensions.size());

		std::vector<const char*> layers;
		if(m_enableValidation) {if(!ValidationLayerAvailable())return false;layers.push_back(kValidationLayerName);}

		VkApplicationInfo application{};
		application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		application.pApplicationName = "dy_engine";
		application.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
		application.pEngineName = "dy_engine";
		application.engineVersion = VK_MAKE_VERSION(1, 0, 0);
		application.apiVersion = VK_API_VERSION_1_3;

		VkInstanceCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		info.pApplicationInfo = &application;
		info.enabledExtensionCount = extensionCount;
		info.ppEnabledExtensionNames = extensions.data();
		info.enabledLayerCount = static_cast<uint32_t>(layers.size());
		info.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
		const VkResult result = vkCreateInstance(&info, nullptr, &m_context.instance);
		if (result != VK_SUCCESS) LogVulkanFailure("vkCreateInstance", result);
		return result == VK_SUCCESS;
	}

	VkResult VulkanDevice::Impl::CreateSurface()
	{
#if defined(_WIN32)
		VkWin32SurfaceCreateInfoKHR info{};
		info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
		info.hinstance = GetModuleHandle(nullptr);
		info.hwnd = static_cast<HWND>(m_windowHandle);
		return vkCreateWin32SurfaceKHR(m_context.instance, &info, nullptr, &m_context.surface);
#else
		return glfwCreateWindowSurface(
			m_context.instance,
			static_cast<GLFWwindow*>(m_windowHandle),
			nullptr,
			&m_context.surface);
#endif
	}

	bool VulkanDevice::Impl::PickPhysicalDevice()
	{
		uint32_t deviceCount = 0;
		if (vkEnumeratePhysicalDevices(m_context.instance, &deviceCount, nullptr) != VK_SUCCESS || deviceCount == 0) return false;
		std::vector<VkPhysicalDevice> devices(deviceCount);
		if (vkEnumeratePhysicalDevices(m_context.instance, &deviceCount, devices.data()) != VK_SUCCESS) return false;

		if(m_adapterIndex>=devices.size())return false;
        const std::array<VkPhysicalDevice,1> selected={devices[m_adapterIndex]};
        for (VkPhysicalDevice device : selected)
		{
			VkPhysicalDeviceProperties properties{};
			vkGetPhysicalDeviceProperties(device, &properties);
			if (properties.apiVersion < VK_API_VERSION_1_3) continue;

			VkPhysicalDeviceVulkan13Features vulkan13{};
			vulkan13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
			VkPhysicalDeviceFeatures2 features{};
			features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
			features.pNext = &vulkan13;
			vkGetPhysicalDeviceFeatures2(device, &features);
			if (!vulkan13.dynamicRendering || !vulkan13.synchronization2) continue;

			uint32_t familyCount = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
			std::vector<VkQueueFamilyProperties> families(familyCount);
			vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
			VulkanContext::QueueFamilyIndices indices{};
			for (uint32_t i = 0; i < familyCount; ++i)
			{
				if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) indices.graphicsFamily = i;
				if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) indices.presentFamily = i;
				if (indices.IsComplete()) break;
			}

			if (!indices.IsComplete()) continue;

			m_context.physicalDevice = device;
			m_context.queueIndices = indices;
			return true;
		}
		return false;
	}

	bool VulkanDevice::Impl::CreateLogicalDevice()
	{
		std::vector<uint32_t> queueFamilies = { m_context.queueIndices.graphicsFamily };
		if (m_context.queueIndices.presentFamily != m_context.queueIndices.graphicsFamily)
		{
			queueFamilies.push_back(m_context.queueIndices.presentFamily);
		}
		const float priority = 1.0f;
		std::vector<VkDeviceQueueCreateInfo> queueInfos;
		queueInfos.reserve(queueFamilies.size());
		for (uint32_t family : queueFamilies)
		{
			VkDeviceQueueCreateInfo queueInfo{};
			queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
			queueInfo.queueFamilyIndex = family;
			queueInfo.queueCount = 1;
			queueInfo.pQueuePriorities = &priority;
			queueInfos.push_back(queueInfo);
		}

		VkPhysicalDeviceFeatures supported{};
		vkGetPhysicalDeviceFeatures(m_context.physicalDevice, &supported);
		VkPhysicalDeviceFeatures enabled{};
		enabled.samplerAnisotropy = supported.samplerAnisotropy;
		enabled.fillModeNonSolid = supported.fillModeNonSolid;
		enabled.depthBiasClamp = supported.depthBiasClamp;
        enabled.independentBlend=supported.independentBlend;
        enabled.vertexPipelineStoresAndAtomics=supported.vertexPipelineStoresAndAtomics;
        enabled.fragmentStoresAndAtomics=supported.fragmentStoresAndAtomics;
		enabled.shaderSampledImageArrayDynamicIndexing = supported.shaderSampledImageArrayDynamicIndexing;

		VkPhysicalDeviceVulkan13Features vulkan13{};
		vulkan13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
		vulkan13.dynamicRendering = VK_TRUE;
		vulkan13.synchronization2 = VK_TRUE;
		const char* extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
		VkDeviceCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		info.pNext = &vulkan13;
		info.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
		info.pQueueCreateInfos = queueInfos.data();
		info.enabledExtensionCount = 1;
		info.ppEnabledExtensionNames = extensions;
		info.pEnabledFeatures = &enabled;
		if (vkCreateDevice(m_context.physicalDevice, &info, nullptr, &m_context.device) != VK_SUCCESS) return false;
		vkGetDeviceQueue(m_context.device, m_context.queueIndices.graphicsFamily, 0, &m_context.graphicsQueue);
		vkGetDeviceQueue(m_context.device, m_context.queueIndices.presentFamily, 0, &m_context.presentQueue);
		return true;
	}

	bool VulkanDevice::Impl::CreateSwapchainSyncObjects()
	{
		m_imageAvailableSemaphores.assign(m_maxFramesInFlight, VK_NULL_HANDLE);
		m_renderFinishedSemaphores.assign(m_swapchain.GetImageCount(), VK_NULL_HANDLE);
		m_imagesInFlight.assign(m_swapchain.GetImageCount(), VK_NULL_HANDLE);
		VkSemaphoreCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		for (VkSemaphore& semaphore : m_imageAvailableSemaphores)
		{
			const VkResult result = vkCreateSemaphore(m_context.device, &info, nullptr, &semaphore);
			if (result != VK_SUCCESS)
			{
				LogVulkanFailure("vkCreateSemaphore(acquire)", result);
				m_submissionFaulted = true;
				DestroySwapchainSyncObjects();
				return false;
			}
		}
		for (VkSemaphore& semaphore : m_renderFinishedSemaphores)
		{
			const VkResult result = vkCreateSemaphore(m_context.device, &info, nullptr, &semaphore);
			if (result != VK_SUCCESS)
			{
				LogVulkanFailure("vkCreateSemaphore(present)", result);
				m_submissionFaulted = true;
				DestroySwapchainSyncObjects();
				return false;
			}
		}
		return true;
	}

	void VulkanDevice::Impl::UpdateBackBuffers()
	{
		const std::vector<VkImage>& images = m_swapchain.GetImages();
		const std::vector<VkImageView>& views = m_swapchain.GetImageViews();
		if (images.size() != views.size() || images.empty()) throw std::runtime_error("Invalid Vulkan swapchain images");
		const VkExtent2D extent = m_swapchain.GetExtent();
		dyf::RHI::TextureDesc desc{};
		desc.width = extent.width;
		desc.height = extent.height;
		desc.depthOrArraySize = 1;
		desc.mipLevels = 1;
		desc.format = FromVulkanColorFormat(m_swapchain.GetImageFormat());
		desc.usage = dyf::RHI::TextureUsage::RenderTarget;
		if (desc.format == dyf::RHI::Format::Unknown) throw std::runtime_error("Unsupported Vulkan swapchain format");

		m_backBuffers.clear();
		m_backBuffers.reserve(images.size());
		for (size_t i = 0; i < images.size(); ++i)
		{
			m_backBuffers.push_back(
				std::unique_ptr<VulkanTexture, VulkanObjectDeleter>(
					new VulkanTexture(desc, images[i], views[i])));
		}
		m_backBuffer = m_backBuffers.front().get();
	}

	void VulkanDevice::Impl::CollectCompletedSubmissions()
	{
		if (m_context.device == VK_NULL_HANDLE) return;
		for (size_t i = 0; i < m_submissions.size();)
		{
			SubmissionRecord& submission = m_submissions[i];
			const VkResult status = vkGetFenceStatus(m_context.device, submission.fence);
            // 완료 번호는 앞선 모든 제출까지 완료되었음을 뜻한다.
            if(status==VK_NOT_READY)break;
			if (status != VK_SUCCESS)
			{
				LogVulkanFailure("vkGetFenceStatus", status);
				m_submissionFaulted = true;
				++i;
				continue;
			}

			if (submission.frameSlot < m_frameSlots.size() && m_frameSlots[submission.frameSlot] == submission.fence)
			{
				m_frameSlots[submission.frameSlot] = VK_NULL_HANDLE;
			}
			if (submission.imageIndex < m_imagesInFlight.size() && m_imagesInFlight[submission.imageIndex] == submission.fence)
			{
				m_imagesInFlight[submission.imageIndex] = VK_NULL_HANDLE;
			}
			m_completedSubmission=std::max(m_completedSubmission,submission.serial);
            vkDestroyFence(m_context.device, submission.fence, nullptr);
			m_submissions.erase(m_submissions.begin() + static_cast<std::ptrdiff_t>(i));
		}
		// acquire 세마포어를 소비한 제출 완료는 새 세대의 첫 present 완료도 보장한다.
		// 이전 세대는 생성 전에 모든 GPU 제출이 끝났으므로 이 시점에 안전하게 해제한다.
		if (!m_submissionFaulted && m_retiredReleaseSubmission &&
			m_completedSubmission >= m_retiredReleaseSubmission)
			DestroyRetiredSwapchains();
	}

	bool VulkanDevice::Impl::RecreateSwapchain()
	{
		if (!m_hasSwapchainDesc || m_context.device == VK_NULL_HANDLE || m_submissionFaulted) return false;
		CollectCompletedSubmissions();
		if (m_submissionFaulted || !m_submissions.empty() || m_frameReady || m_imageAcquired || m_presentPending) return false;
		if (m_swapchain.GetHandle() != VK_NULL_HANDLE) m_recreationOldSwapchain = RetireSwapchainGeneration();
		return CreateSwapchain(m_swapchainDesc, m_recreationOldSwapchain);
	}

	VkSwapchainKHR VulkanDevice::Impl::RetireSwapchainGeneration()
	{
		RetiredSwapchainGeneration generation{};
		const VkSwapchainKHR handle = m_swapchain.GetHandle();
		generation.swapchain = std::move(m_swapchain);
		generation.imageAvailableSemaphores = std::move(m_imageAvailableSemaphores);
		generation.renderFinishedSemaphores = std::move(m_renderFinishedSemaphores);
		generation.backBuffers = std::move(m_backBuffers);
		m_backBuffer = nullptr;
		m_imagesInFlight.clear();
		m_currentImageIndex = 0;
		m_recreateAfterPresent = false;
		m_retiredSwapchains.push_back(std::move(generation));
		return handle;
	}

	void VulkanDevice::Impl::DestroySwapchainSyncObjects()
	{
		if (m_context.device != VK_NULL_HANDLE)
		{
			for (VkSemaphore semaphore : m_imageAvailableSemaphores)
			{
				if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(m_context.device, semaphore, nullptr);
			}
			for (VkSemaphore semaphore : m_renderFinishedSemaphores)
			{
				if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(m_context.device, semaphore, nullptr);
			}
		}
		m_imageAvailableSemaphores.clear();
		m_renderFinishedSemaphores.clear();
		m_imagesInFlight.clear();
	}

	void VulkanDevice::Impl::DestroyCurrentSwapchain()
	{
		DestroySwapchainSyncObjects();
		m_backBuffer = nullptr;
		m_backBuffers.clear();
		if (m_context.device != VK_NULL_HANDLE) m_swapchain.Cleanup(m_context.device);
	}

	void VulkanDevice::Impl::DestroyRetiredSwapchains()
	{
		if (m_context.device == VK_NULL_HANDLE) return;
		for (RetiredSwapchainGeneration& generation : m_retiredSwapchains)
		{
			for (VkSemaphore semaphore : generation.imageAvailableSemaphores)
			{
				if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(m_context.device, semaphore, nullptr);
			}
			for (VkSemaphore semaphore : generation.renderFinishedSemaphores)
			{
				if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(m_context.device, semaphore, nullptr);
			}
			generation.backBuffers.clear();
			generation.swapchain.Cleanup(m_context.device);
		}
		m_retiredSwapchains.clear();
		m_retiredReleaseImage = UINT32_MAX;
		m_retiredReleaseAcquired = false;
		m_retiredReleaseSubmission = 0;
	}

	void VulkanDevice::Impl::DestroyDeviceResources()
	{
		if (m_context.device != VK_NULL_HANDLE)
		{
			vkDeviceWaitIdle(m_context.device);
			CollectCompletedSubmissions();
			for (SubmissionRecord& submission : m_submissions)
			{
				if (submission.fence != VK_NULL_HANDLE) vkDestroyFence(m_context.device, submission.fence, nullptr);
			}
			m_submissions.clear();
		}
		m_acquiredCommandLists.clear();
		std::fill(m_frameSlots.begin(), m_frameSlots.end(), VK_NULL_HANDLE);

		for (dyf::RHI::ResourceSetHandle resourceSet : m_resourceSets)
			VulkanObjectDeleter{}(static_cast<VulkanResourceSet*>(resourceSet));
		m_resourceSets.clear();
		for (dyf::RHI::PipelineHandle pipeline : m_pipelines)
			VulkanObjectDeleter{}(static_cast<VulkanPipeline*>(pipeline));
		m_pipelines.clear();
		for (dyf::RHI::ShaderHandle shader : m_shaders)
			VulkanObjectDeleter{}(static_cast<VulkanShader*>(shader));
		m_shaders.clear();
		for (dyf::RHI::TextureHandle texture : m_textures)
			VulkanObjectDeleter{}(static_cast<VulkanTexture*>(texture));
		m_textures.clear();
		for (dyf::RHI::BufferHandle buffer : m_buffers)
			VulkanObjectDeleter{}(static_cast<VulkanBuffer*>(buffer));
		m_buffers.clear();

		if (m_context.device != VK_NULL_HANDLE)
		{
			DestroyRetiredSwapchains();
			DestroyCurrentSwapchain();
			vkDestroyDevice(m_context.device, nullptr);
			m_context.device = VK_NULL_HANDLE;
		}
		if (m_context.surface != VK_NULL_HANDLE)
		{
			vkDestroySurfaceKHR(m_context.instance, m_context.surface, nullptr);
			m_context.surface = VK_NULL_HANDLE;
		}
		if (m_context.instance != VK_NULL_HANDLE)
		{
			vkDestroyInstance(m_context.instance, nullptr);
			m_context.instance = VK_NULL_HANDLE;
		}
	}
}
