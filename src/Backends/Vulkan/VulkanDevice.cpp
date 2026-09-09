#include "VulkanDevice.h"

#include "RHI/Buffer.h"
#include "RHI/ICommandList.h"
#include "RHI/Pipeline.h"
#include "RHI/ResourceSet.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include "RHI/Readback.h"
#include "VulkanCommandList.h"
#include "VulkanContext.h"
#include "VulkanPipeline.h"
#include "VulkanResources.h"
#include "VulkanSwapchain.h"

#include <GLFW/glfw3.h>

#include <algorithm>
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

namespace dy::Backends
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
			std::printf("%s failed (VkResult %d)\n", operation, static_cast<int>(result));
		}

		bool ValidationLayerAvailable()
		{
#if defined(_DEBUG)
			uint32_t count = 0;
			if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) return false;
			std::vector<VkLayerProperties> layers(count);
			if (count > 0 && vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) return false;
			return std::any_of(layers.begin(), layers.end(), [](const VkLayerProperties& layer) {
				return std::strcmp(layer.layerName, kValidationLayerName) == 0;
			});
#else
			return false;
#endif
		}

		VkPresentModeKHR ToPresentMode(dy::RHI::PresentMode mode)
		{
			switch (mode)
			{
			case dy::RHI::PresentMode::Immediate: return VK_PRESENT_MODE_IMMEDIATE_KHR;
			case dy::RHI::PresentMode::Mailbox: return VK_PRESENT_MODE_MAILBOX_KHR;
			case dy::RHI::PresentMode::Fifo: return VK_PRESENT_MODE_FIFO_KHR;
			default: return VK_PRESENT_MODE_MAX_ENUM_KHR;
			}
		}
	}

	struct VulkanDevice::Impl
	{
		~Impl();

		int Initialize(const void* windowHandle, const dy::RHI::DeviceDesc& desc);
		bool CreateSwapchain(const dy::RHI::SwapchainDesc& desc, VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE);
		bool BeginFrame();
		dy::RHI::ICommandList* AcquireCommandList();
		bool Submit(dy::RHI::ICommandList** commandLists, uint32_t count);
		void Present();
		dy::RHI::TextureHandle GetBackBuffer() const { return m_backBuffer; }
		bool ReadTexture(dy::RHI::TextureHandle texture, dy::RHI::TextureReadback& result);

		dy::RHI::BufferHandle CreateBuffer(const dy::RHI::BufferDesc& desc);
		dy::RHI::TextureHandle CreateTexture(const dy::RHI::TextureDesc& desc);
		dy::RHI::ShaderHandle CreateShader(const dy::RHI::ShaderDesc& desc);
		dy::RHI::PipelineHandle CreateGraphicsPipeline(const dy::RHI::GraphicsPipelineDesc& desc);
		dy::RHI::ResourceSetHandle CreateResourceSet(const dy::RHI::ResourceSetDesc& desc);
		void DestroyBuffer(dy::RHI::BufferHandle buffer);
		void DestroyTexture(dy::RHI::TextureHandle texture);
		void DestroyShader(dy::RHI::ShaderHandle shader);
		void DestroyPipeline(dy::RHI::PipelineHandle pipeline);
		void DestroyResourceSet(dy::RHI::ResourceSetHandle resourceSet);
		bool UpdateBuffer(dy::RHI::ICommandList& commandList, dy::RHI::BufferHandle buffer, uint32_t offset, const void* data, uint32_t size);
		bool UpdateTexture(
			dy::RHI::ICommandList& commandList,
			dy::RHI::TextureHandle texture,
			uint32_t mipLevel,
			uint32_t arrayLayer,
			const void* data,
			uint32_t dataSize,
			uint32_t rowPitch,
			uint32_t slicePitch);

	private:
		bool CreateInstance();
		bool CreateSurface();
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

		std::vector<VkSemaphore> m_imageAvailableSemaphores;
		std::vector<VkSemaphore> m_renderFinishedSemaphores;
		std::vector<VkFence> m_frameSlots;
		std::vector<VkFence> m_imagesInFlight;

		struct SubmissionRecord
		{
			VkFence fence = VK_NULL_HANDLE;
			std::vector<std::unique_ptr<VulkanCommandList, VulkanObjectDeleter>> commandLists;
			std::vector<std::unique_ptr<VulkanBuffer, VulkanObjectDeleter>> retiredBuffers;
			std::vector<std::unique_ptr<VulkanTexture, VulkanObjectDeleter>> retiredTextures;
			std::vector<std::unique_ptr<VulkanShader, VulkanObjectDeleter>> retiredShaders;
			std::vector<std::unique_ptr<VulkanPipeline, VulkanObjectDeleter>> retiredPipelines;
			std::vector<std::unique_ptr<VulkanResourceSet, VulkanObjectDeleter>> retiredResourceSets;
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

		std::vector<dy::RHI::BufferHandle> m_buffers;
		std::vector<dy::RHI::TextureHandle> m_textures;
		std::vector<dy::RHI::ShaderHandle> m_shaders;
		std::vector<dy::RHI::PipelineHandle> m_pipelines;
		std::vector<dy::RHI::ResourceSetHandle> m_resourceSets;

		std::vector<std::unique_ptr<VulkanTexture, VulkanObjectDeleter>> m_backBuffers;
		dy::RHI::TextureHandle m_backBuffer = nullptr;
		dy::RHI::SwapchainDesc m_swapchainDesc = {};
		VkSwapchainKHR m_recreationOldSwapchain = VK_NULL_HANDLE;
		uint32_t m_currentFrameSlot = 0;
		uint32_t m_currentImageIndex = 0;
		uint32_t m_pendingPresentImageIndex = 0;
		bool m_hasSwapchainDesc = false;
		bool m_frameReady = false;
		bool m_imageAcquired = false;
		bool m_presentPending = false;
		bool m_swapchainNeedsRecreate = false;
		bool m_recreateAfterPresent = false;
		bool m_submissionFaulted = false;
	};

	VulkanDevice::VulkanDevice()
		: m_impl(std::make_unique<Impl>())
	{
	}

	VulkanDevice::~VulkanDevice() = default;

	bool VulkanDevice::CreateSwapchain(const dy::RHI::SwapchainDesc& desc)
	{
		return m_impl->CreateSwapchain(desc);
	}

	bool VulkanDevice::BeginFrame()
	{
		return m_impl->BeginFrame();
	}

	dy::RHI::ICommandList* VulkanDevice::AcquireCommandList()
	{
		return m_impl->AcquireCommandList();
	}

	bool VulkanDevice::Submit(dy::RHI::ICommandList** commandLists, uint32_t count)
	{
		return m_impl->Submit(commandLists, count);
	}

	void VulkanDevice::Present()
	{
		m_impl->Present();
	}

	dy::RHI::TextureHandle VulkanDevice::GetBackBuffer()
	{
		return m_impl->GetBackBuffer();
	}

	dy::RHI::BufferHandle VulkanDevice::CreateBuffer(const dy::RHI::BufferDesc& desc)
	{
		return m_impl->CreateBuffer(desc);
	}

	dy::RHI::TextureHandle VulkanDevice::CreateTexture(const dy::RHI::TextureDesc& desc)
	{
		return m_impl->CreateTexture(desc);
	}

	dy::RHI::ShaderHandle VulkanDevice::CreateShader(const dy::RHI::ShaderDesc& desc)
	{
		return m_impl->CreateShader(desc);
	}

	dy::RHI::PipelineHandle VulkanDevice::CreateGraphicsPipeline(const dy::RHI::GraphicsPipelineDesc& desc)
	{
		return m_impl->CreateGraphicsPipeline(desc);
	}

	dy::RHI::ResourceSetHandle VulkanDevice::CreateResourceSet(const dy::RHI::ResourceSetDesc& desc)
	{
		return m_impl->CreateResourceSet(desc);
	}

	void VulkanDevice::DestroyBuffer(dy::RHI::BufferHandle buffer)
	{
		m_impl->DestroyBuffer(buffer);
	}

	void VulkanDevice::DestroyTexture(dy::RHI::TextureHandle texture)
	{
		m_impl->DestroyTexture(texture);
	}

	void VulkanDevice::DestroyShader(dy::RHI::ShaderHandle shader)
	{
		m_impl->DestroyShader(shader);
	}

	void VulkanDevice::DestroyPipeline(dy::RHI::PipelineHandle pipeline)
	{
		m_impl->DestroyPipeline(pipeline);
	}

	void VulkanDevice::DestroyResourceSet(dy::RHI::ResourceSetHandle resourceSet)
	{
		m_impl->DestroyResourceSet(resourceSet);
	}

	bool VulkanDevice::UpdateBuffer(dy::RHI::ICommandList& commandList, dy::RHI::BufferHandle buffer, uint32_t offset, const void* data, uint32_t size)
	{
		return m_impl->UpdateBuffer(commandList, buffer, offset, data, size);
	}

	bool VulkanDevice::UpdateTexture(
		dy::RHI::ICommandList& commandList,
		dy::RHI::TextureHandle texture,
		uint32_t mipLevel,
		uint32_t arrayLayer,
		const void* data,
		uint32_t dataSize,
		uint32_t rowPitch,
		uint32_t slicePitch)
	{
		return m_impl->UpdateTexture(commandList, texture, mipLevel, arrayLayer, data, dataSize, rowPitch, slicePitch);
	}

	int VulkanDevice::Initialize(const void* windowHandle, const dy::RHI::DeviceDesc& desc)
	{
		return m_impl->Initialize(windowHandle, desc);
	}

	VulkanDevice::Impl::~Impl()
	{
		DestroyDeviceResources();
	}

	int VulkanDevice::Impl::Initialize(const void* windowHandle, const dy::RHI::DeviceDesc& desc)
	{
		m_windowHandle = const_cast<void*>(windowHandle);
		m_maxFramesInFlight = desc.maxFramesInFlight;
		if (m_windowHandle == nullptr || m_maxFramesInFlight == 0) return -1;

		if (!CreateInstance() || !CreateSurface() || !PickPhysicalDevice() || !CreateLogicalDevice()) return -1;
		m_frameSlots.assign(m_maxFramesInFlight, VK_NULL_HANDLE);
		return 0;
	}

	bool VulkanDevice::Impl::CreateSwapchain(const dy::RHI::SwapchainDesc& desc, VkSwapchainKHR oldSwapchain)
	{
		if (m_context.device == VK_NULL_HANDLE || m_windowHandle == nullptr || m_submissionFaulted || desc.minimumImageCount == 0) return false;
		CollectCompletedSubmissions();
		if (m_submissionFaulted || !m_submissions.empty() || m_frameReady || m_imageAcquired || m_presentPending ||
			m_swapchain.GetHandle() != VK_NULL_HANDLE)
		{
			return false;
		}

		const VkFormat requestedFormat = ToVulkanFormat(desc.format);
		if (desc.format != dy::RHI::Format::Unknown && requestedFormat == VK_FORMAT_UNDEFINED) return false;
		const VkPresentModeKHR requestedPresentMode = ToPresentMode(desc.presentMode);
		if (requestedPresentMode == VK_PRESENT_MODE_MAX_ENUM_KHR) return false;

		bool oldSwapchainRetired = false;
		try
		{
			if (!m_swapchain.Initialize(
				m_context,
				m_windowHandle,
				requestedFormat,
				requestedPresentMode,
				desc.minimumImageCount,
				desc.allowReadback,
				m_hasSwapchainDesc ? 0 : desc.initialWidth,
				m_hasSwapchainDesc ? 0 : desc.initialHeight,
				oldSwapchain,
				oldSwapchainRetired))
			{
				if (oldSwapchainRetired) m_recreationOldSwapchain = VK_NULL_HANDLE;
				return false;
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
			std::printf("Vulkan swapchain creation failed: %s\n", exception.what());
			if (oldSwapchainRetired) m_recreationOldSwapchain = VK_NULL_HANDLE;
			DestroyCurrentSwapchain();
			return false;
		}

		m_swapchainDesc = desc;
		m_hasSwapchainDesc = true;
		m_currentFrameSlot = 0;
		m_currentImageIndex = 0;
		m_pendingPresentImageIndex = 0;
		m_frameReady = false;
		m_imageAcquired = false;
		m_presentPending = false;
		m_swapchainNeedsRecreate = false;
		m_recreateAfterPresent = false;
		return true;
	}

	bool VulkanDevice::Impl::BeginFrame()
	{
		if (m_context.device == VK_NULL_HANDLE || m_presentPending || m_submissionFaulted) return false;
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
				if (result == VK_ERROR_DEVICE_LOST) m_submissionFaulted = true;
				return false;
			}
			m_imageAcquired = true;
			m_recreateAfterPresent = result == VK_SUBOPTIMAL_KHR;
		}

		if (m_currentImageIndex >= m_imagesInFlight.size() || m_currentImageIndex >= m_backBuffers.size())
		{
			m_submissionFaulted = true;
			return false;
		}
		if (m_imagesInFlight[m_currentImageIndex] != VK_NULL_HANDLE) return false;

		m_backBuffer = m_backBuffers[m_currentImageIndex].get();
		m_frameReady = true;
		return true;
	}

	dy::RHI::ICommandList* VulkanDevice::Impl::AcquireCommandList()
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
			std::printf("Vulkan command-list acquisition failed: %s\n", exception.what());
			return nullptr;
		}
	}

	bool VulkanDevice::Impl::Submit(dy::RHI::ICommandList** commandLists, uint32_t count)
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
		if (frameSubmission && (!m_frameReady || !m_imageAcquired || m_presentPending ||
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
		if (frameSubmission)
		{
			const auto key = std::make_pair(activeBackBuffer, 0u);
			const auto found = resourceStates.textureSubresources.find(key);
			const dy::RHI::ResourceState finalState = found == resourceStates.textureSubresources.end()
				? activeBackBuffer->GetState(0, 0) : found->second;
			if (finalState != dy::RHI::ResourceState::Present) return false;
		}

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
		if (frameSubmission)
		{
			submitInfo.waitSemaphoreCount = 1;
			submitInfo.pWaitSemaphores = &m_imageAvailableSemaphores[m_currentFrameSlot];
			submitInfo.pWaitDstStageMask = &waitStage;
			submitInfo.signalSemaphoreCount = 1;
			submitInfo.pSignalSemaphores = &m_renderFinishedSemaphores[m_currentImageIndex];
		}

		const VkResult result = vkQueueSubmit(m_context.graphicsQueue, 1, &submitInfo, submission.fence);
		if (result != VK_SUCCESS)
		{
			LogVulkanFailure("vkQueueSubmit", result);
			if (result == VK_ERROR_DEVICE_LOST) m_submissionFaulted = true;
			vkDestroyFence(m_context.device, submission.fence, nullptr);
			return false;
		}
		for (VulkanCommandList* commandList : selected) commandList->CommitResourceStates();

		if (frameSubmission)
		{
			submission.frameSlot = m_currentFrameSlot;
			submission.imageIndex = m_currentImageIndex;
			m_frameSlots[m_currentFrameSlot] = submission.fence;
			m_imagesInFlight[m_currentImageIndex] = submission.fence;
			m_pendingPresentImageIndex = m_currentImageIndex;
			m_presentPending = true;
			m_frameReady = false;
			m_imageAcquired = false;
			m_currentFrameSlot = (m_currentFrameSlot + 1) % m_maxFramesInFlight;
		}

		m_submissions.push_back(std::move(submission));
		return true;
	}

	void VulkanDevice::Impl::Present()
	{
		if (!m_presentPending || m_swapchain.GetHandle() == VK_NULL_HANDLE || m_submissionFaulted) return;
		VkSwapchainKHR swapchain = m_swapchain.GetHandle();
		VkPresentInfoKHR info{};
		info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		info.waitSemaphoreCount = 1;
		info.pWaitSemaphores = &m_renderFinishedSemaphores[m_pendingPresentImageIndex];
		info.swapchainCount = 1;
		info.pSwapchains = &swapchain;
		info.pImageIndices = &m_pendingPresentImageIndex;
		const VkResult result = vkQueuePresentKHR(m_context.presentQueue, &info);
		m_presentPending = false;
		if ((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) && m_pendingPresentImageIndex < m_backBuffers.size())
		{
			m_backBuffers[m_pendingPresentImageIndex]->MarkPresented();
		}
		if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || m_recreateAfterPresent)
		{
			m_swapchainNeedsRecreate = true;
			m_recreateAfterPresent = false;
		}
		else if (result != VK_SUCCESS)
		{
			LogVulkanFailure("vkQueuePresentKHR", result);
			if (result == VK_ERROR_DEVICE_LOST) m_submissionFaulted = true;
			else m_swapchainNeedsRecreate = true;
		}
	}

	bool VulkanDevice::ReadTexture(dy::RHI::TextureHandle texture, dy::RHI::TextureReadback& result)
	{
		return m_impl->ReadTexture(texture, result);
	}

	bool VulkanDevice::Impl::ReadTexture(dy::RHI::TextureHandle texture, dy::RHI::TextureReadback& result)
	{
		if(texture == nullptr || m_submissionFaulted || !m_acquiredCommandLists.empty()) return false;
		const bool backBuffer = texture == m_backBuffer;
		if(backBuffer)
		{
			if(!m_swapchainDesc.allowReadback || !m_presentPending) return false;
		}
		else if(std::find(m_textures.begin(), m_textures.end(), texture) == m_textures.end()) return false;
		const auto& desc = texture->GetDesc();
		if(!dy::RHI::IsReadbackFormat(desc.format) || !desc.width || !desc.height ||
			desc.width > UINT32_MAX / 4u) return false;
		const uint64_t bytes = static_cast<uint64_t>(desc.width) * desc.height * 4u;
		if(bytes > std::numeric_limits<size_t>::max()) return false;
		auto* image = static_cast<VulkanTexture*>(texture);
		const auto state = image->GetState(0, 0);
		if(state == dy::RHI::ResourceState::Undefined) return false;

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
		dy::RHI::TextureReadback output;
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

	dy::RHI::BufferHandle VulkanDevice::Impl::CreateBuffer(const dy::RHI::BufferDesc& desc)
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
			std::printf("Vulkan buffer creation failed: %s\n", exception.what());
			return nullptr;
		}
	}

	dy::RHI::TextureHandle VulkanDevice::Impl::CreateTexture(const dy::RHI::TextureDesc& desc)
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
			std::printf("Vulkan texture creation failed: %s\n", exception.what());
			return nullptr;
		}
	}

	dy::RHI::ShaderHandle VulkanDevice::Impl::CreateShader(const dy::RHI::ShaderDesc& desc)
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
			std::printf("Vulkan shader creation failed: %s\n", exception.what());
			return nullptr;
		}
	}

	dy::RHI::PipelineHandle VulkanDevice::Impl::CreateGraphicsPipeline(const dy::RHI::GraphicsPipelineDesc& desc)
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
			std::printf("Vulkan pipeline creation failed: %s\n", exception.what());
			return nullptr;
		}
	}

	dy::RHI::ResourceSetHandle VulkanDevice::Impl::CreateResourceSet(const dy::RHI::ResourceSetDesc& desc)
	{
		if (std::find(m_pipelines.begin(), m_pipelines.end(), desc.pipeline) == m_pipelines.end() ||
			(desc.bindingCount > 0 && desc.bindings == nullptr))
		{
			return nullptr;
		}
		for (uint32_t i = 0; i < desc.bindingCount; ++i)
		{
			const dy::RHI::ResourceBinding& binding = desc.bindings[i];
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
			std::printf("Vulkan resource-set creation failed: %s\n", exception.what());
			return nullptr;
		}
	}

	void VulkanDevice::Impl::DestroyBuffer(dy::RHI::BufferHandle buffer)
	{
		const auto found = std::find(m_buffers.begin(), m_buffers.end(), buffer);
		if (found == m_buffers.end()) return;
		std::unique_ptr<VulkanBuffer, VulkanObjectDeleter> retired(
			static_cast<VulkanBuffer*>(*found));
		m_buffers.erase(found);
		if (m_submissions.empty()) return;
		m_submissions.back().retiredBuffers.push_back(std::move(retired));
	}

	void VulkanDevice::Impl::DestroyTexture(dy::RHI::TextureHandle texture)
	{
		const auto found = std::find(m_textures.begin(), m_textures.end(), texture);
		if (found == m_textures.end()) return;
		std::unique_ptr<VulkanTexture, VulkanObjectDeleter> retired(
			static_cast<VulkanTexture*>(*found));
		m_textures.erase(found);
		if (m_submissions.empty()) return;
		m_submissions.back().retiredTextures.push_back(std::move(retired));
	}

	void VulkanDevice::Impl::DestroyShader(dy::RHI::ShaderHandle shader)
	{
		const auto found = std::find(m_shaders.begin(), m_shaders.end(), shader);
		if (found == m_shaders.end()) return;
		std::unique_ptr<VulkanShader, VulkanObjectDeleter> retired(
			static_cast<VulkanShader*>(*found));
		m_shaders.erase(found);
		if (m_submissions.empty()) return;
		m_submissions.back().retiredShaders.push_back(std::move(retired));
	}

	void VulkanDevice::Impl::DestroyPipeline(dy::RHI::PipelineHandle pipeline)
	{
		const auto found = std::find(m_pipelines.begin(), m_pipelines.end(), pipeline);
		if (found == m_pipelines.end()) return;
		std::unique_ptr<VulkanPipeline, VulkanObjectDeleter> retired(
			static_cast<VulkanPipeline*>(*found));
		m_pipelines.erase(found);
		if (m_submissions.empty()) return;
		m_submissions.back().retiredPipelines.push_back(std::move(retired));
	}

	void VulkanDevice::Impl::DestroyResourceSet(dy::RHI::ResourceSetHandle resourceSet)
	{
		const auto found = std::find(m_resourceSets.begin(), m_resourceSets.end(), resourceSet);
		if (found == m_resourceSets.end()) return;
		std::unique_ptr<VulkanResourceSet, VulkanObjectDeleter> retired(
			static_cast<VulkanResourceSet*>(*found));
		m_resourceSets.erase(found);
		if (m_submissions.empty()) return;
		m_submissions.back().retiredResourceSets.push_back(std::move(retired));
	}

	bool VulkanDevice::Impl::UpdateBuffer(
		dy::RHI::ICommandList& commandList,
		dy::RHI::BufferHandle buffer,
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
		dy::RHI::ICommandList& commandList,
		dy::RHI::TextureHandle texture,
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
		uint32_t extensionCount = 0;
		const char** requiredExtensions = glfwGetRequiredInstanceExtensions(&extensionCount);
		if (requiredExtensions == nullptr || extensionCount == 0) return false;
		std::vector<const char*> extensions(requiredExtensions, requiredExtensions + extensionCount);
		std::vector<const char*> layers;
		if (ValidationLayerAvailable()) layers.push_back(kValidationLayerName);

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

	bool VulkanDevice::Impl::CreateSurface()
	{
#if defined(_WIN32)
		VkWin32SurfaceCreateInfoKHR info{};
		info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
		info.hinstance = GetModuleHandle(nullptr);
		info.hwnd = static_cast<HWND>(m_windowHandle);
		return vkCreateWin32SurfaceKHR(m_context.instance, &info, nullptr, &m_context.surface) == VK_SUCCESS;
#else
		return glfwCreateWindowSurface(
			m_context.instance,
			static_cast<GLFWwindow*>(m_windowHandle),
			nullptr,
			&m_context.surface) == VK_SUCCESS;
#endif
	}

	bool VulkanDevice::Impl::PickPhysicalDevice()
	{
		uint32_t deviceCount = 0;
		if (vkEnumeratePhysicalDevices(m_context.instance, &deviceCount, nullptr) != VK_SUCCESS || deviceCount == 0) return false;
		std::vector<VkPhysicalDevice> devices(deviceCount);
		if (vkEnumeratePhysicalDevices(m_context.instance, &deviceCount, devices.data()) != VK_SUCCESS) return false;

		for (VkPhysicalDevice device : devices)
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
				VkBool32 presentSupport = VK_FALSE;
				vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_context.surface, &presentSupport);
				if (presentSupport) indices.presentFamily = i;
				if (indices.IsComplete()) break;
			}

			const VulkanSwapchain::SwapchainSupportDetails swapchainSupport =
				VulkanSwapchain::QuerySwapchainSupport(device, m_context.surface);
			if (!indices.IsComplete() || swapchainSupport.formats.empty() || swapchainSupport.presentModes.empty()) continue;

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
			if (vkCreateSemaphore(m_context.device, &info, nullptr, &semaphore) != VK_SUCCESS)
			{
				DestroySwapchainSyncObjects();
				return false;
			}
		}
		for (VkSemaphore& semaphore : m_renderFinishedSemaphores)
		{
			if (vkCreateSemaphore(m_context.device, &info, nullptr, &semaphore) != VK_SUCCESS)
			{
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
		dy::RHI::TextureDesc desc{};
		desc.width = extent.width;
		desc.height = extent.height;
		desc.depthOrArraySize = 1;
		desc.mipLevels = 1;
		desc.format = FromVulkanColorFormat(m_swapchain.GetImageFormat());
		desc.usage = dy::RHI::TextureUsage::RenderTarget;
		if (desc.format == dy::RHI::Format::Unknown) throw std::runtime_error("Unsupported Vulkan swapchain format");

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
			if (status == VK_NOT_READY)
			{
				++i;
				continue;
			}
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
			vkDestroyFence(m_context.device, submission.fence, nullptr);
			m_submissions.erase(m_submissions.begin() + static_cast<std::ptrdiff_t>(i));
		}
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
		m_pendingPresentImageIndex = 0;
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

		for (dy::RHI::ResourceSetHandle resourceSet : m_resourceSets)
			VulkanObjectDeleter{}(static_cast<VulkanResourceSet*>(resourceSet));
		m_resourceSets.clear();
		for (dy::RHI::PipelineHandle pipeline : m_pipelines)
			VulkanObjectDeleter{}(static_cast<VulkanPipeline*>(pipeline));
		m_pipelines.clear();
		for (dy::RHI::ShaderHandle shader : m_shaders)
			VulkanObjectDeleter{}(static_cast<VulkanShader*>(shader));
		m_shaders.clear();
		for (dy::RHI::TextureHandle texture : m_textures)
			VulkanObjectDeleter{}(static_cast<VulkanTexture*>(texture));
		m_textures.clear();
		for (dy::RHI::BufferHandle buffer : m_buffers)
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
