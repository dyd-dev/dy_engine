#include "VulkanDevice.h"
#include "VulkanResources.h"
#include "RHI/IPipelineState.h"
#include "RHI/ITexture.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#if defined(_WIN32)
#include <windows.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define SDL_Log(fmt, ...) { printf(fmt, ##__VA_ARGS__); printf("\n"); }

namespace {
	constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";
	constexpr uint32_t kFallbackTextureWidth = 2;
	constexpr uint32_t kFallbackTextureHeight = 2;
	bool IsValidationEnabled() {
#if defined(_DEBUG)
		return true;
#else
		return false;
#endif
	}

	class VulkanTexture final : public dy::RHI::ITexture
	{
	public:
		explicit VulkanTexture(const dy::RHI::TextureDesc& desc)
			: m_width(desc.width), m_height(desc.height), m_format(desc.format)
		{
		}

		uint32_t GetWidth() const override { return m_width; }
		uint32_t GetHeight() const override { return m_height; }
		dy::RHI::Format GetFormat() const override { return m_format; }

		void Update(uint32_t width, uint32_t height, dy::RHI::Format format)
		{
			m_width = width;
			m_height = height;
			m_format = format;
		}

	private:
		uint32_t m_width = 0;
		uint32_t m_height = 0;
		dy::RHI::Format m_format = dy::RHI::Format::Unknown;
	};

	class VulkanPipelineState final : public dy::RHI::IPipelineState
	{
	public:
		explicit VulkanPipelineState(const dy::RHI::GraphicsPipelineDesc&) {}
	};
}

VulkanDevice::VulkanDevice() {
	m_commandList = new VulkanCommandList();
}

VulkanDevice::~VulkanDevice() {
	DestroyDeviceResources();
}

int VulkanDevice::InitializeForTest(const void* windowHandle, const std::string& shaderDir) {
    m_windowHandle = const_cast<void*>(windowHandle);
    if (!m_windowHandle) return -1;
    m_shaderOutputDirectory = shaderDir;
    m_useVertexInput = true;
    try {
        if (!CreateInstance()) return -1;
        if (!CreateSurface()) return -1;
        if (!PickPhysicalDevice()) return -1;
        if (!CreateLogicalDevice()) return -1;
        if (!CreateCommandPool()) return -1;
        m_swapchain.Initialize(m_context, m_windowHandle);
        if (!CreateTextureSampler()) return -1;
        if (!CreateDescriptorSetLayout()) return -1;
        if (!CreateDescriptorPool()) return -1;
        m_pipeline.Initialize(m_context, m_swapchain.GetImageFormat(), m_swapchain.GetExtent(), m_descriptorSetLayout, m_shaderOutputDirectory, m_useVertexInput);
        if (!CreateFramebuffers()) return -1;
        if (!CreateCommandBuffer()) return -1;
        if (!CreateSyncObjects()) return -1;
    } catch (const std::exception& e) {
        SDL_Log("Vulkan Initialization failed: %s", e.what());
        return -1;
    }
    return 0;
}

bool VulkanDevice::UploadTestMesh(const std::vector<float>& vertices, const std::vector<uint32_t>& indices) {
    m_indexCount = static_cast<uint32_t>(indices.size());
    
    // Vertex Buffer
    VkDeviceSize bufferSize = sizeof(vertices[0]) * vertices.size();
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingBufferMemory;
    VulkanResources::CreateBuffer(m_context, bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer, stagingBufferMemory);
    void* data;
    vkMapMemory(m_context.device, stagingBufferMemory, 0, bufferSize, 0, &data);
    memcpy(data, vertices.data(), (size_t)bufferSize);
    vkUnmapMemory(m_context.device, stagingBufferMemory);
    
    VulkanResources::CreateBuffer(m_context, bufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_vertexBuffer, m_vertexBufferMemory);
    VulkanResources::CopyBuffer(m_context, m_commandPool, stagingBuffer, m_vertexBuffer, bufferSize);
    
    vkDestroyBuffer(m_context.device, stagingBuffer, nullptr);
    vkFreeMemory(m_context.device, stagingBufferMemory, nullptr);
    
    // Index Buffer
    bufferSize = sizeof(indices[0]) * indices.size();
    VulkanResources::CreateBuffer(m_context, bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer, stagingBufferMemory);
    vkMapMemory(m_context.device, stagingBufferMemory, 0, bufferSize, 0, &data);
    memcpy(data, indices.data(), (size_t)bufferSize);
    vkUnmapMemory(m_context.device, stagingBufferMemory);
    
    VulkanResources::CreateBuffer(m_context, bufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_indexBuffer, m_indexBufferMemory);
    VulkanResources::CopyBuffer(m_context, m_commandPool, stagingBuffer, m_indexBuffer, bufferSize);
    
    vkDestroyBuffer(m_context.device, stagingBuffer, nullptr);
    vkFreeMemory(m_context.device, stagingBufferMemory, nullptr);
    
    return true;
}

int VulkanDevice::Initialize(const void *windowHandle) {
	m_windowHandle = const_cast<void*>(windowHandle);
	if (!m_windowHandle) return -1;

	m_shaderOutputDirectory = VULKAN_SHADER_DIR;

	try {
		if (!CreateInstance()) return -1;
		if (!CreateSurface()) return -1;
		if (!PickPhysicalDevice()) return -1;
		if (!CreateLogicalDevice()) return -1;

		m_swapchain.Initialize(m_context, m_windowHandle);
		UpdateBackBufferMetadata();
		
		if (!CreateDescriptorSetLayout()) return -1;
		m_pipeline.Initialize(m_context, m_swapchain.GetImageFormat(), m_swapchain.GetExtent(), m_descriptorSetLayout, m_shaderOutputDirectory, m_useVertexInput);
		
		if (!CreateCommandPool()) return -1;
		if (!CreateTextureImage()) return -1;
		if (!CreateTextureImageView()) return -1;
		if (!CreateTextureSampler()) return -1;
		if (!CreateFramebuffers()) return -1;
		if (!CreateCommandBuffer()) return -1;
		if (!CreateSyncObjects()) return -1;
		if (!CreateDescriptorPool()) return -1;
		if (!CreateDescriptorSets()) return -1;
		
		if (!CreateMeshBuffers()) return -1;
	} catch (const std::exception& e) {
		SDL_Log("Vulkan Initialization failed: %s", e.what());
		return -1;
	}

	return 0;
}

dy::RHI::ITexture* VulkanDevice::CreateTexture(const dy::RHI::TextureDesc& desc) {
	VulkanTexture* texture = new VulkanTexture(desc);
	m_ownedTextures.push_back(texture);
	return texture;
}

dy::RHI::IPipelineState* VulkanDevice::CreateGraphicsPipeline(const dy::RHI::GraphicsPipelineDesc& desc) {
	VulkanPipelineState* pipelineState = new VulkanPipelineState(desc);
	m_ownedPipelineStates.push_back(pipelineState);
	return pipelineState;
}

dy::RHI::DescriptorIndex VulkanDevice::AllocateDescriptorSlot() {
	return m_nextDescriptorIndex++;
}

void VulkanDevice::UpdateDescriptorSlot(dy::RHI::DescriptorIndex, dy::RHI::ITexture*) {
}

void VulkanDevice::DestroyTexture(dy::RHI::ITexture* texture) {
	if (!texture || texture == m_backBuffer) return;
	const auto it = std::find(m_ownedTextures.begin(), m_ownedTextures.end(), texture);
	if (it != m_ownedTextures.end()) {
		delete *it;
		m_ownedTextures.erase(it);
	}
}

void VulkanDevice::DestroyPipelineState(dy::RHI::IPipelineState* pipeline) {
	if (!pipeline) return;
	const auto it = std::find(m_ownedPipelineStates.begin(), m_ownedPipelineStates.end(), pipeline);
	if (it != m_ownedPipelineStates.end()) {
		delete *it;
		m_ownedPipelineStates.erase(it);
	}
}

dy::RHI::ITexture* VulkanDevice::GetBackBuffer() {
	return m_backBuffer;
}

void VulkanDevice::BeginFrame() {
	m_frameReady = false;
	m_frameSubmitted = false;
	if (m_commandList != nullptr) {
		m_commandList->Begin();
	}

	if (!m_context.device || m_swapchain.GetHandle() == VK_NULL_HANDLE) return;

	vkWaitForFences(m_context.device, 1, &m_inFlightFences[m_currentFrameIndex], VK_TRUE, UINT64_MAX);

	const VkResult acquireResult = vkAcquireNextImageKHR(
		m_context.device, m_swapchain.GetHandle(), UINT64_MAX,
		m_imageAvailableSemaphores[m_currentFrameIndex],
		VK_NULL_HANDLE, &m_currentImageIndex
	);

	if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
		RecreateSwapchain();
		return;
	} else if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
		SDL_Log("Failed to acquire swapchain image!");
		return;
	}

	if (m_imagesInFlight[m_currentImageIndex] != VK_NULL_HANDLE) {
		vkWaitForFences(m_context.device, 1, &m_imagesInFlight[m_currentImageIndex], VK_TRUE, UINT64_MAX);
	}
	m_imagesInFlight[m_currentImageIndex] = m_inFlightFences[m_currentFrameIndex];

	vkResetFences(m_context.device, 1, &m_inFlightFences[m_currentFrameIndex]);
	m_frameReady = true;
}

void VulkanDevice::Submit(dy::RHI::ICommandList** cmdLists, uint32_t count) {
	if (!m_frameReady || count == 0 || !cmdLists || !cmdLists[0]) return;

	VulkanCommandList* vulkanCmd = static_cast<VulkanCommandList*>(cmdLists[0]);
	RecordCommandBuffer(*vulkanCmd);

	const VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
	const VkSubmitInfo submitInfo = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &m_imageAvailableSemaphores[m_currentFrameIndex],
		.pWaitDstStageMask = waitStages,
		.commandBufferCount = 1,
		.pCommandBuffers = &m_commandBuffers[m_currentFrameIndex],
		.signalSemaphoreCount = 1,
		.pSignalSemaphores = &m_renderFinishedSemaphores[m_currentImageIndex]
	};

	if (vkQueueSubmit(m_context.graphicsQueue, 1, &submitInfo, m_inFlightFences[m_currentFrameIndex]) != VK_SUCCESS) {
		SDL_Log("failed to submit draw command buffer!");
		return;
	}

	m_frameSubmitted = true;
}

void VulkanDevice::Present() {
	if (!m_frameSubmitted) return;

	VkSwapchainKHR swapchainHandle = m_swapchain.GetHandle();
	const VkPresentInfoKHR presentInfo = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &m_renderFinishedSemaphores[m_currentImageIndex],
		.swapchainCount = 1,
		.pSwapchains = &swapchainHandle,
		.pImageIndices = &m_currentImageIndex
	};

	const VkResult presentResult = vkQueuePresentKHR(m_context.presentQueue, &presentInfo);
	if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
		RecreateSwapchain();
	}

	m_currentFrameIndex = (m_currentFrameIndex + 1) % kMaxFramesInFlight;
}

bool VulkanDevice::CreateInstance() {
	uint32_t extensionCount = 0;
	const char** extensions = glfwGetRequiredInstanceExtensions(&extensionCount);
	std::vector<const char*> enabledExtensions;
	if (extensions != nullptr) {
		enabledExtensions.assign(extensions, extensions + extensionCount);
	}
	std::vector<const char*> enabledLayers;
	if (IsValidationEnabled()) enabledLayers.push_back(kValidationLayerName);

	const VkApplicationInfo appInfo = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "dy_engine Vulkan",
		.applicationVersion = VK_MAKE_VERSION(1, 0, 0),
		.pEngineName = "dy_engine",
		.engineVersion = VK_MAKE_VERSION(1, 0, 0),
		.apiVersion = VK_API_VERSION_1_0
	};

	const VkInstanceCreateInfo createInfo = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &appInfo,
		.enabledLayerCount = static_cast<uint32_t>(enabledLayers.size()),
		.ppEnabledLayerNames = enabledLayers.empty() ? nullptr : enabledLayers.data(),
		.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size()),
		.ppEnabledExtensionNames = enabledExtensions.data()
	};

	return vkCreateInstance(&createInfo, nullptr, &m_context.instance) == VK_SUCCESS;
}

bool VulkanDevice::CreateSurface() {
#if defined(_WIN32)
	const VkWin32SurfaceCreateInfoKHR createInfo = {
		.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
		.hinstance = GetModuleHandle(nullptr),
		.hwnd = static_cast<HWND>(m_windowHandle)
	};
	return vkCreateWin32SurfaceKHR(m_context.instance, &createInfo, nullptr, &m_context.surface) == VK_SUCCESS;
#else
	return glfwCreateWindowSurface(m_context.instance, static_cast<GLFWwindow*>(m_windowHandle), nullptr, &m_context.surface) == VK_SUCCESS;
#endif
}

bool VulkanDevice::PickPhysicalDevice() {
	uint32_t deviceCount = 0;
	vkEnumeratePhysicalDevices(m_context.instance, &deviceCount, nullptr);
	std::vector<VkPhysicalDevice> devices(deviceCount);
	vkEnumeratePhysicalDevices(m_context.instance, &deviceCount, devices.data());

	for (VkPhysicalDevice device : devices) {
		uint32_t count = 0; vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
		std::vector<VkQueueFamilyProperties> families(count); vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
		
		VulkanContext::QueueFamilyIndices indices;
		for (uint32_t i = 0; i < count; ++i) {
			if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) indices.graphicsFamily = i;
			VkBool32 presentSupport = false; vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_context.surface, &presentSupport);
			if (presentSupport) indices.presentFamily = i;
			if (indices.IsComplete()) break;
		}

		auto swapchainSupport = VulkanSwapchain::QuerySwapchainSupport(device, m_context.surface);
		if (indices.IsComplete() && !swapchainSupport.formats.empty() && !swapchainSupport.presentModes.empty()) {
			m_context.physicalDevice = device;
			m_context.queueIndices = indices;
			return true;
		}
	}
	return false;
}

bool VulkanDevice::CreateLogicalDevice() {
	std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
	std::vector<uint32_t> uniqueFamilies = { m_context.queueIndices.graphicsFamily };
	if (m_context.queueIndices.presentFamily != m_context.queueIndices.graphicsFamily) uniqueFamilies.push_back(m_context.queueIndices.presentFamily);

	const float queuePriority = 1.0f;
	for (uint32_t family : uniqueFamilies) {
		queueCreateInfos.push_back({
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.queueFamilyIndex = family,
			.queueCount = 1,
			.pQueuePriorities = &queuePriority
		});
	}

	const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	const VkPhysicalDeviceFeatures deviceFeatures = {};
	const VkDeviceCreateInfo createInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size()),
		.pQueueCreateInfos = queueCreateInfos.data(),
		.enabledExtensionCount = 1,
		.ppEnabledExtensionNames = deviceExtensions,
		.pEnabledFeatures = &deviceFeatures
	};

	if (vkCreateDevice(m_context.physicalDevice, &createInfo, nullptr, &m_context.device) != VK_SUCCESS) return false;

	vkGetDeviceQueue(m_context.device, m_context.queueIndices.graphicsFamily, 0, &m_context.graphicsQueue);
	vkGetDeviceQueue(m_context.device, m_context.queueIndices.presentFamily, 0, &m_context.presentQueue);
	return true;
}

bool VulkanDevice::CreateMeshBuffers() {
	return true;
}

void VulkanDevice::RecordCommandBuffer(const VulkanCommandList& commandList) {
	VkCommandBuffer commandBuffer = m_commandBuffers[m_currentFrameIndex];
	vkResetCommandBuffer(commandBuffer, 0);
	VkCommandBufferBeginInfo beginInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	vkBeginCommandBuffer(commandBuffer, &beginInfo);

	VkClearValue clearValue = {}; clearValue.color = commandList.m_clearColor;
	VkRenderPassBeginInfo renderPassInfo = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = m_pipeline.GetRenderPass(),
		.framebuffer = m_swapchainFramebuffers[m_currentImageIndex],
		.renderArea = { .offset = {0, 0}, .extent = m_swapchain.GetExtent() },
		.clearValueCount = 1, .pClearValues = &clearValue
	};

	vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline.GetPipeline());

	if (!m_descriptorSets.empty()) {
		vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline.GetLayout(), 0, 1, &m_descriptorSets[m_currentFrameIndex], 0, nullptr);
	}

	        if (m_useVertexInput && m_vertexBuffer != VK_NULL_HANDLE) {
            VkBuffer vertexBuffers[] = {m_vertexBuffer};
            VkDeviceSize offsets[] = {0};
            vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
            vkCmdBindIndexBuffer(commandBuffer, m_indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        }

        for (const VulkanCommandList::DrawCall& drawCall : commandList.m_drawCalls) {
            if (drawCall.pushConstantSize > 0) {
                vkCmdPushConstants(
                    commandBuffer,
                    m_pipeline.GetLayout(),
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0,
                    drawCall.pushConstantSize,
                    drawCall.pushConstants.data()
                );
            }
            if (m_useVertexInput && m_indexCount > 0) {
                vkCmdDrawIndexed(commandBuffer, m_indexCount, 1, 0, 0, 0);
            } else {
                vkCmdDraw(commandBuffer, drawCall.vertexCount, drawCall.instanceCount, drawCall.startVertex, drawCall.startInstance);
            }
        }
	
	vkCmdEndRenderPass(commandBuffer);
	vkEndCommandBuffer(commandBuffer);
}

bool VulkanDevice::CreateFramebuffers() {
	const auto& views = m_swapchain.GetImageViews();
	m_swapchainFramebuffers.resize(views.size());
	for (size_t i = 0; i < views.size(); ++i) {
		VkImageView attachments[] = { views[i] };
		VkFramebufferCreateInfo fbInfo = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = m_pipeline.GetRenderPass(), .attachmentCount = 1, .pAttachments = attachments, .width = m_swapchain.GetExtent().width, .height = m_swapchain.GetExtent().height, .layers = 1 };
		if (vkCreateFramebuffer(m_context.device, &fbInfo, nullptr, &m_swapchainFramebuffers[i]) != VK_SUCCESS) return false;
	}
	return true;
}

bool VulkanDevice::CreateCommandPool() {
	VkCommandPoolCreateInfo poolInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = m_context.queueIndices.graphicsFamily };
	return vkCreateCommandPool(m_context.device, &poolInfo, nullptr, &m_commandPool) == VK_SUCCESS;
}

bool VulkanDevice::CreateCommandBuffer() {
	VkCommandBufferAllocateInfo allocInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = m_commandPool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = kMaxFramesInFlight };
	m_commandBuffers.resize(kMaxFramesInFlight);
	return vkAllocateCommandBuffers(m_context.device, &allocInfo, m_commandBuffers.data()) == VK_SUCCESS;
}

bool VulkanDevice::CreateSyncObjects() {
	VkSemaphoreCreateInfo semInfo = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	VkFenceCreateInfo fenceInfo = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
	
	m_imageAvailableSemaphores.resize(kMaxFramesInFlight); 
	m_inFlightFences.resize(kMaxFramesInFlight);
	m_renderFinishedSemaphores.resize(m_swapchain.GetImageCount()); 

	for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
		if (vkCreateSemaphore(m_context.device, &semInfo, nullptr, &m_imageAvailableSemaphores[i]) != VK_SUCCESS || 
			vkCreateFence(m_context.device, &fenceInfo, nullptr, &m_inFlightFences[i]) != VK_SUCCESS) return false;
	}

	for (size_t i = 0; i < m_swapchain.GetImageCount(); ++i) {
		if (vkCreateSemaphore(m_context.device, &semInfo, nullptr, &m_renderFinishedSemaphores[i]) != VK_SUCCESS) return false;
	}
	m_imagesInFlight.assign(m_swapchain.GetImageCount(), VK_NULL_HANDLE);
	return true;
}

bool VulkanDevice::CreateTextureImage() {
	int w, h, c;
	stbi_uc* pixels = stbi_load("assets/jj.jpeg", &w, &h, &c, STBI_rgb_alpha);
	std::array<stbi_uc, kFallbackTextureWidth * kFallbackTextureHeight * 4> fallbackPixels = {
		255, 0, 0, 255,       0, 255, 0, 255,
		0, 0, 255, 255,       255, 255, 0, 255
	};
	if (!pixels) {
		w = static_cast<int>(kFallbackTextureWidth);
		h = static_cast<int>(kFallbackTextureHeight);
		c = 4;
		pixels = fallbackPixels.data();
	}
	VkDeviceSize size = w * h * 4;
	VkBuffer staging; VkDeviceMemory stagingMem;
	VulkanResources::CreateBuffer(m_context, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, stagingMem);
	void* data; vkMapMemory(m_context.device, stagingMem, 0, size, 0, &data); memcpy(data, pixels, size); vkUnmapMemory(m_context.device, stagingMem);
	if (pixels != fallbackPixels.data()) {
		stbi_image_free(pixels);
	}
	VulkanResources::CreateImage(m_context, w, h, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_textureImage, m_textureImageMemory);
	VulkanResources::TransitionImageLayout(m_context, m_commandPool, m_textureImage, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VulkanResources::CopyBufferToImage(m_context, m_commandPool, staging, m_textureImage, w, h);
	VulkanResources::TransitionImageLayout(m_context, m_commandPool, m_textureImage, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	vkDestroyBuffer(m_context.device, staging, nullptr); vkFreeMemory(m_context.device, stagingMem, nullptr);
	return true;
}

bool VulkanDevice::CreateTextureImageView() { m_textureImageView = VulkanResources::CreateImageView(m_context.device, m_textureImage, VK_FORMAT_R8G8B8A8_SRGB); return true; }

bool VulkanDevice::CreateTextureSampler() {
	VkSamplerCreateInfo info = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT, .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK };
	return vkCreateSampler(m_context.device, &info, nullptr, &m_textureSampler) == VK_SUCCESS;
}

bool VulkanDevice::CreateDescriptorSetLayout() {
	VkDescriptorSetLayoutBinding b = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
	VkDescriptorSetLayoutCreateInfo info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b };
	return vkCreateDescriptorSetLayout(m_context.device, &info, nullptr, &m_descriptorSetLayout) == VK_SUCCESS;
}

bool VulkanDevice::CreateDescriptorPool() {
	VkDescriptorPoolSize s = { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = kMaxFramesInFlight };
	VkDescriptorPoolCreateInfo info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = kMaxFramesInFlight, .poolSizeCount = 1, .pPoolSizes = &s };
	return vkCreateDescriptorPool(m_context.device, &info, nullptr, &m_descriptorPool) == VK_SUCCESS;
}

bool VulkanDevice::CreateDescriptorSets() {
	std::vector<VkDescriptorSetLayout> layouts(kMaxFramesInFlight, m_descriptorSetLayout);
	VkDescriptorSetAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = m_descriptorPool, .descriptorSetCount = kMaxFramesInFlight, .pSetLayouts = layouts.data() };
	m_descriptorSets.resize(kMaxFramesInFlight);
	if (vkAllocateDescriptorSets(m_context.device, &alloc, m_descriptorSets.data()) != VK_SUCCESS) return false;
	for (size_t i = 0; i < kMaxFramesInFlight; i++) {
		VkDescriptorImageInfo img = { .sampler = m_textureSampler, .imageView = m_textureImageView, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkWriteDescriptorSet write = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = m_descriptorSets[i], .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &img };
		vkUpdateDescriptorSets(m_context.device, 1, &write, 0, nullptr);
	}
	return true;
}

void VulkanDevice::RecreateSwapchain() {
	vkDeviceWaitIdle(m_context.device);
	DestroySwapchainResources();
	m_swapchain.Initialize(m_context, m_windowHandle);
	UpdateBackBufferMetadata();
	m_pipeline.Initialize(m_context, m_swapchain.GetImageFormat(), m_swapchain.GetExtent(), m_descriptorSetLayout, m_shaderOutputDirectory, m_useVertexInput);
	CreateFramebuffers();
}

void VulkanDevice::DestroySwapchainResources() {
	for (auto fb : m_swapchainFramebuffers) vkDestroyFramebuffer(m_context.device, fb, nullptr);
	m_swapchainFramebuffers.clear();
	m_pipeline.Cleanup(m_context.device);
	m_swapchain.Cleanup(m_context.device);
}

void VulkanDevice::DestroyDeviceResources() {
	for (dy::RHI::IPipelineState* pipelineState : m_ownedPipelineStates) delete pipelineState;
	m_ownedPipelineStates.clear();

	for (dy::RHI::ITexture* texture : m_ownedTextures) delete texture;
	m_ownedTextures.clear();

	delete m_backBuffer;
	m_backBuffer = nullptr;

	delete m_commandList;
	m_commandList = nullptr;

	if (m_context.device != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(m_context.device);
		if (m_vertexBuffer != VK_NULL_HANDLE) { vkDestroyBuffer(m_context.device, m_vertexBuffer, nullptr); vkFreeMemory(m_context.device, m_vertexBufferMemory, nullptr); }
		if (m_indexBuffer != VK_NULL_HANDLE) { vkDestroyBuffer(m_context.device, m_indexBuffer, nullptr); vkFreeMemory(m_context.device, m_indexBufferMemory, nullptr); }
		if (m_textureSampler != VK_NULL_HANDLE) vkDestroySampler(m_context.device, m_textureSampler, nullptr);
		if (m_textureImageView != VK_NULL_HANDLE) vkDestroyImageView(m_context.device, m_textureImageView, nullptr);
		if (m_textureImage != VK_NULL_HANDLE) { vkDestroyImage(m_context.device, m_textureImage, nullptr); vkFreeMemory(m_context.device, m_textureImageMemory, nullptr); }
		if (m_descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_context.device, m_descriptorPool, nullptr);
		if (m_descriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_context.device, m_descriptorSetLayout, nullptr);
		for (auto s : m_imageAvailableSemaphores) vkDestroySemaphore(m_context.device, s, nullptr);
		for (auto s : m_renderFinishedSemaphores) vkDestroySemaphore(m_context.device, s, nullptr);
		for (auto f : m_inFlightFences) vkDestroyFence(m_context.device, f, nullptr);
		if (m_commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(m_context.device, m_commandPool, nullptr);
		DestroySwapchainResources();
		vkDestroyDevice(m_context.device, nullptr);
	}
	if (m_context.surface != VK_NULL_HANDLE) vkDestroySurfaceKHR(m_context.instance, m_context.surface, nullptr);
	if (m_context.instance != VK_NULL_HANDLE) vkDestroyInstance(m_context.instance, nullptr);
}

void VulkanDevice::UpdateBackBufferMetadata() {
	const VkExtent2D extent = m_swapchain.GetExtent();
	const dy::RHI::TextureDesc desc = {
		.width = extent.width,
		.height = extent.height,
		.depthOrArraySize = 1,
		.mipLevels = 1,
		.format = dy::RHI::Format::R8G8B8A8_UNORM,
		.usage = dy::RHI::TextureUsage::RenderTarget
	};

	if (m_backBuffer == nullptr) {
		m_backBuffer = new VulkanTexture(desc);
		return;
	}

	static_cast<VulkanTexture*>(m_backBuffer)->Update(desc.width, desc.height, desc.format);
}


