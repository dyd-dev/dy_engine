#include "VulkanDevice.h"
#include "VulkanResources.h"
#include "RHI/IBuffer.h"
#include "RHI/IPipelineState.h"
#include "RHI/ITexture.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <fstream>
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
	constexpr uint32_t kAcquireTimeoutNanoseconds = 16666667u;
	bool IsValidationEnabled() {
#if defined(_DEBUG)
		return true;
#else
		return false;
#endif
	}

	const char* VkResultToString(VkResult result)
	{
		switch (result)
		{
		case VK_SUCCESS: return "VK_SUCCESS";
		case VK_NOT_READY: return "VK_NOT_READY";
		case VK_TIMEOUT: return "VK_TIMEOUT";
		case VK_EVENT_SET: return "VK_EVENT_SET";
		case VK_EVENT_RESET: return "VK_EVENT_RESET";
		case VK_INCOMPLETE: return "VK_INCOMPLETE";
		case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
		case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
		case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
		case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
		case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
		default: return "VK_ERROR_UNKNOWN";
		}
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

	VkBufferUsageFlags ToVkBufferUsage(dy::RHI::BufferUsage usage)
	{
		VkBufferUsageFlags flags = 0;
		if ((usage & dy::RHI::BufferUsage::Vertex) != dy::RHI::BufferUsage::None) flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
		if ((usage & dy::RHI::BufferUsage::Index) != dy::RHI::BufferUsage::None) flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
		if ((usage & dy::RHI::BufferUsage::Constant) != dy::RHI::BufferUsage::None) flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		if ((usage & dy::RHI::BufferUsage::Storage) != dy::RHI::BufferUsage::None) flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		if ((usage & dy::RHI::BufferUsage::Indirect) != dy::RHI::BufferUsage::None) flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
		if (flags == 0) return static_cast<VkBufferUsageFlags>(VK_BUFFER_USAGE_TRANSFER_DST_BIT);
		return flags;
	}

	class VulkanBuffer final : public dy::RHI::IBuffer
	{
	public:
		VulkanBuffer(const VulkanContext& context, const dy::RHI::BufferDesc& desc)
			: m_device(context.device), m_size(desc.size), m_stride(desc.stride), m_usage(desc.usage)
		{
			VulkanResources::CreateBuffer(
				context,
				desc.size,
				ToVkBufferUsage(desc.usage),
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				m_buffer,
				m_memory
			);
		}

		~VulkanBuffer() override
		{
			if (m_mapped != nullptr) vkUnmapMemory(m_device, m_memory);
			if (m_buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, m_buffer, nullptr);
			if (m_memory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_memory, nullptr);
		}

		void* Map(uint32_t offset, uint32_t size) override
		{
			if (m_mapped != nullptr) return static_cast<uint8_t*>(m_mapped) + offset;
			const VkDeviceSize mapSize = size == 0 ? VK_WHOLE_SIZE : static_cast<VkDeviceSize>(size);
			vkMapMemory(m_device, m_memory, offset, mapSize, 0, &m_mapped);
			return m_mapped;
		}

		void Unmap() override
		{
			if (m_mapped == nullptr) return;
			vkUnmapMemory(m_device, m_memory);
			m_mapped = nullptr;
		}

		uint32_t GetSize() const override { return m_size; }
		uint32_t GetStride() const { return m_stride; }
		dy::RHI::BufferUsage GetUsage() const { return m_usage; }
		VkBuffer GetHandle() const { return m_buffer; }

	private:
		VkDevice m_device = VK_NULL_HANDLE;
		VkBuffer m_buffer = VK_NULL_HANDLE;
		VkDeviceMemory m_memory = VK_NULL_HANDLE;
		void* m_mapped = nullptr;
		uint32_t m_size = 0;
		uint32_t m_stride = 0;
		dy::RHI::BufferUsage m_usage = dy::RHI::BufferUsage::None;
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
        UpdateBackBufferMetadata();
        if (!CreateTextureSampler()) return -1;
        // Shadow Map 리소스는 DescriptorSetLayout/Sets에서 참조하므로 그 전에 생성.
        m_shadowMapFormat = FindDepthFormat();
        if (!CreateShadowRenderPass()) return -1;
        if (!CreateShadowMapResources()) return -1;
        if (!CreateShadowMatrixBuffers()) return -1;
        if (!CreateDescriptorSetLayout()) return -1;
        if (!CreateLightingVolumeBuffers()) return -1;
        if (!CreateDescriptorPool()) return -1;
        if (!CreateDescriptorSets()) return -1;
        m_depthFormat = FindDepthFormat();
        m_pipeline.Initialize(m_context, m_swapchain.GetImageFormat(), m_depthFormat, m_swapchain.GetExtent(), m_descriptorSetLayout, m_shaderOutputDirectory, m_useVertexInput);
        if (!CreateShadowPipeline()) return -1;
        if (!CreateDepthResources()) return -1;
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

    if (m_vertexBuffer != VK_NULL_HANDLE || m_indexBuffer != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_context.device);
    }

    if (m_vertexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_context.device, m_vertexBuffer, nullptr);
        vkFreeMemory(m_context.device, m_vertexBufferMemory, nullptr);
        m_vertexBuffer = VK_NULL_HANDLE;
        m_vertexBufferMemory = VK_NULL_HANDLE;
    }
    if (m_indexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_context.device, m_indexBuffer, nullptr);
        vkFreeMemory(m_context.device, m_indexBufferMemory, nullptr);
        m_indexBuffer = VK_NULL_HANDLE;
        m_indexBufferMemory = VK_NULL_HANDLE;
    }
    
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

void VulkanDevice::SetLightingVolumeProfile(const VulkanLightingVolumeProfile& profile) {
	m_lightingVolumeProfile = profile;
}

void VulkanDevice::SetShadowLightMatrix(const float* lightViewProjColumnMajor) {
	if (lightViewProjColumnMajor == nullptr) return;
	memcpy(m_shadowLightViewProj, lightViewProjColumnMajor, sizeof(m_shadowLightViewProj));
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
		m_depthFormat = FindDepthFormat();
		m_pipeline.Initialize(m_context, m_swapchain.GetImageFormat(), m_depthFormat, m_swapchain.GetExtent(), m_descriptorSetLayout, m_shaderOutputDirectory, m_useVertexInput);
		if (!CreateDepthResources()) return -1;
		
		if (!CreateCommandPool()) return -1;
		if (!CreateTextureImage()) return -1;
		if (!CreateTextureImageView()) return -1;
		if (!CreateTextureSampler()) return -1;
		if (!CreateLightingVolumeBuffers()) return -1;
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

dy::RHI::IBuffer* VulkanDevice::CreateBuffer(const dy::RHI::BufferDesc& desc) {
	VulkanBuffer* buffer = new VulkanBuffer(m_context, desc);
	m_ownedBuffers.push_back(buffer);
	return buffer;
}

void VulkanDevice::DestroyBuffer(dy::RHI::IBuffer* buffer) {
	if (!buffer) return;
	const auto it = std::find(m_ownedBuffers.begin(), m_ownedBuffers.end(), buffer);
	if (it == m_ownedBuffers.end()) return;
	vkDeviceWaitIdle(m_context.device);
	delete *it;
	m_ownedBuffers.erase(it);
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
		m_context.device, m_swapchain.GetHandle(), kAcquireTimeoutNanoseconds,
		m_imageAvailableSemaphores[m_currentFrameIndex],
		VK_NULL_HANDLE, &m_currentImageIndex
	);

	if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
		RecreateSwapchain();
		return;
	} else if (acquireResult == VK_NOT_READY || acquireResult == VK_TIMEOUT) {
		return;
	} else if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
		SDL_Log("Failed to acquire swapchain image: %s (%d)", VkResultToString(acquireResult), static_cast<int>(acquireResult));
		return;
	}

	if (m_currentImageIndex >= m_imagesInFlight.size()) {
		SDL_Log("Swapchain image index %u is out of tracked range %zu. Recreating swapchain.", m_currentImageIndex, m_imagesInFlight.size());
		RecreateSwapchain();
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
	UpdateLightingVolumeBuffer();
	UpdateShadowMatrixBuffer();
	RecordCommandBuffer(*vulkanCmd);

	const VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = &m_imageAvailableSemaphores[m_currentFrameIndex];
	submitInfo.pWaitDstStageMask = waitStages;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &m_commandBuffers[m_currentFrameIndex];
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = &m_renderFinishedSemaphores[m_currentImageIndex];

	if (vkQueueSubmit(m_context.graphicsQueue, 1, &submitInfo, m_inFlightFences[m_currentFrameIndex]) != VK_SUCCESS) {
		SDL_Log("failed to submit draw command buffer!");
		return;
	}

	m_frameSubmitted = true;
}

void VulkanDevice::Present() {
	if (!m_frameSubmitted) return;

	VkSwapchainKHR swapchainHandle = m_swapchain.GetHandle();
	VkPresentInfoKHR presentInfo{};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = &m_renderFinishedSemaphores[m_currentImageIndex];
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = &swapchainHandle;
	presentInfo.pImageIndices = &m_currentImageIndex;

	const VkResult presentResult = vkQueuePresentKHR(m_context.presentQueue, &presentInfo);
	if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
		RecreateSwapchain();
	} else if (presentResult != VK_SUCCESS) {
		SDL_Log("Failed to present swapchain image: %s (%d)", VkResultToString(presentResult), static_cast<int>(presentResult));
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

	VkApplicationInfo appInfo{};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "dy_engine Vulkan";
	appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.pEngineName = "dy_engine";
	appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.apiVersion = VK_API_VERSION_1_0;

	VkInstanceCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	createInfo.pApplicationInfo = &appInfo;
	createInfo.enabledLayerCount = static_cast<uint32_t>(enabledLayers.size());
	createInfo.ppEnabledLayerNames = enabledLayers.empty() ? nullptr : enabledLayers.data();
	createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
	createInfo.ppEnabledExtensionNames = enabledExtensions.data();

	return vkCreateInstance(&createInfo, nullptr, &m_context.instance) == VK_SUCCESS;
}

bool VulkanDevice::CreateSurface() {
#if defined(_WIN32)
	VkWin32SurfaceCreateInfoKHR createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
	createInfo.hinstance = GetModuleHandle(nullptr);
	createInfo.hwnd = static_cast<HWND>(m_windowHandle);
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
		VkDeviceQueueCreateInfo queueCreateInfo{};
		queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queueCreateInfo.queueFamilyIndex = family;
		queueCreateInfo.queueCount = 1;
		queueCreateInfo.pQueuePriorities = &queuePriority;
		queueCreateInfos.push_back(queueCreateInfo);
	}

	const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	const VkPhysicalDeviceFeatures deviceFeatures = {};
	VkDeviceCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
	createInfo.pQueueCreateInfos = queueCreateInfos.data();
	createInfo.enabledExtensionCount = 1;
	createInfo.ppEnabledExtensionNames = deviceExtensions;
	createInfo.pEnabledFeatures = &deviceFeatures;

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
	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	vkBeginCommandBuffer(commandBuffer, &beginInfo);

	// Pass 1: Shadow Pass (광원 시점에서 깊이 텍스처 작성)
	if (m_shadowRenderPass != VK_NULL_HANDLE && m_shadowPipeline != VK_NULL_HANDLE) {
		RecordShadowPass(commandBuffer, commandList);
	}

	// Pass 2: Main Pass (카메라 시점에서 색 + ShadowMap sample)
	RecordMainPass(commandBuffer, commandList);

	vkEndCommandBuffer(commandBuffer);
}

void VulkanDevice::RecordShadowPass(VkCommandBuffer commandBuffer, const VulkanCommandList& commandList) {
	VkClearValue shadowClear = {};
	shadowClear.depthStencil = { 1.0f, 0 };

	VkRenderPassBeginInfo shadowPassInfo{};
	shadowPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	shadowPassInfo.renderPass = m_shadowRenderPass;
	shadowPassInfo.framebuffer = m_shadowFramebuffer;
	shadowPassInfo.renderArea.offset = {0, 0};
	shadowPassInfo.renderArea.extent = { kShadowMapResolution, kShadowMapResolution };
	shadowPassInfo.clearValueCount = 1;
	shadowPassInfo.pClearValues = &shadowClear;

	vkCmdBeginRenderPass(commandBuffer, &shadowPassInfo, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline);
	if (!m_descriptorSets.empty()) {
		vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipelineLayout, 0, 1, &m_descriptorSets[m_currentFrameIndex], 0, nullptr);
	}

	// Shadow Pass 전체에 ShadowMap 해상도 viewport/scissor 고정.
	VkViewport shadowViewport{};
	shadowViewport.x = 0.0f;
	shadowViewport.y = 0.0f;
	shadowViewport.width = static_cast<float>(kShadowMapResolution);
	shadowViewport.height = static_cast<float>(kShadowMapResolution);
	shadowViewport.minDepth = 0.0f;
	shadowViewport.maxDepth = 1.0f;
	VkRect2D shadowScissor{};
	shadowScissor.offset = {0, 0};
	shadowScissor.extent = { kShadowMapResolution, kShadowMapResolution };
	vkCmdSetViewport(commandBuffer, 0, 1, &shadowViewport);
	vkCmdSetScissor(commandBuffer, 0, 1, &shadowScissor);

	for (const VulkanCommandList::DrawCall& drawCall : commandList.m_drawCalls) {
		const VulkanBuffer* vertexBuffer = dynamic_cast<const VulkanBuffer*>(drawCall.vertexBuffer);
		const VulkanBuffer* indexBuffer = dynamic_cast<const VulkanBuffer*>(drawCall.indexBuffer);
		if (vertexBuffer != nullptr) {
			VkBuffer vertexBuffers[] = { vertexBuffer->GetHandle() };
			VkDeviceSize offsets[] = { drawCall.vertexBufferOffset };
			vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
		} else if (m_useVertexInput && m_vertexBuffer != VK_NULL_HANDLE) {
			VkBuffer vertexBuffers[] = { m_vertexBuffer };
			VkDeviceSize offsets[] = { 0 };
			vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
		}
		if (indexBuffer != nullptr) {
			vkCmdBindIndexBuffer(commandBuffer, indexBuffer->GetHandle(), drawCall.indexOffset, VK_INDEX_TYPE_UINT32);
		} else if (drawCall.indexed && m_indexBuffer != VK_NULL_HANDLE) {
			vkCmdBindIndexBuffer(commandBuffer, m_indexBuffer, 0, VK_INDEX_TYPE_UINT32);
		}

		// Shadow Pass에서는 동일한 push constants를 사용 (shadow.vert가 model 행렬만 읽음)
		if (drawCall.pushConstantSize > 0) {
			vkCmdPushConstants(
				commandBuffer,
				m_shadowPipelineLayout,
				VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				0,
				drawCall.pushConstantSize,
				drawCall.pushConstants.data());
		}

		if (drawCall.indexed) {
			if (drawCall.indexCount > 0 && (indexBuffer != nullptr || m_indexBuffer != VK_NULL_HANDLE)) {
				vkCmdDrawIndexed(commandBuffer, drawCall.indexCount, drawCall.instanceCount, drawCall.firstIndex, drawCall.baseVertex, drawCall.startInstance);
			}
		} else {
			vkCmdDraw(commandBuffer, drawCall.vertexCount, drawCall.instanceCount, drawCall.startVertex, drawCall.startInstance);
		}
	}

	vkCmdEndRenderPass(commandBuffer);
	// finalLayout이 SHADER_READ_ONLY_OPTIMAL이라 자동 전이 — 별도 Barrier 불필요.
}

void VulkanDevice::RecordMainPass(VkCommandBuffer commandBuffer, const VulkanCommandList& commandList) {
	std::array<VkClearValue, 2> clearValues = {};
	clearValues[0].color = commandList.m_clearColor;
	clearValues[1].depthStencil = { 1.0f, 0 };
	VkRenderPassBeginInfo renderPassInfo{};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	renderPassInfo.renderPass = m_pipeline.GetRenderPass();
	renderPassInfo.framebuffer = m_swapchainFramebuffers[m_currentImageIndex];
	renderPassInfo.renderArea.offset = {0, 0};
	renderPassInfo.renderArea.extent = m_swapchain.GetExtent();
	renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
	renderPassInfo.pClearValues = clearValues.data();

	vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline.GetPipeline());

	if (!m_descriptorSets.empty()) {
		vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline.GetLayout(), 0, 1, &m_descriptorSets[m_currentFrameIndex], 0, nullptr);
	}

        for (const VulkanCommandList::DrawCall& drawCall : commandList.m_drawCalls) {
            const VulkanBuffer* vertexBuffer = dynamic_cast<const VulkanBuffer*>(drawCall.vertexBuffer);
            const VulkanBuffer* indexBuffer = dynamic_cast<const VulkanBuffer*>(drawCall.indexBuffer);
            const bool hasBoundVertexBuffer = vertexBuffer != nullptr;
            const bool hasBoundIndexBuffer = indexBuffer != nullptr;
            if (hasBoundVertexBuffer) {
                VkBuffer vertexBuffers[] = { vertexBuffer->GetHandle() };
                VkDeviceSize offsets[] = { drawCall.vertexBufferOffset };
                vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
            } else if (m_useVertexInput && m_vertexBuffer != VK_NULL_HANDLE) {
                VkBuffer vertexBuffers[] = {m_vertexBuffer};
                VkDeviceSize offsets[] = {0};
                vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
            }

            if (hasBoundIndexBuffer) {
                const VkIndexType indexType = drawCall.indexFormat == dy::RHI::Format::R32_UINT ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT32;
                vkCmdBindIndexBuffer(commandBuffer, indexBuffer->GetHandle(), drawCall.indexOffset, indexType);
            } else if (drawCall.indexed && m_indexBuffer != VK_NULL_HANDLE) {
                vkCmdBindIndexBuffer(commandBuffer, m_indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            }

            VkViewport viewport{};
            if (drawCall.hasViewport) {
                viewport.x = drawCall.viewport.x;
                viewport.y = drawCall.viewport.y;
                viewport.width = drawCall.viewport.width;
                viewport.height = drawCall.viewport.height;
                viewport.minDepth = drawCall.viewport.minDepth;
                viewport.maxDepth = drawCall.viewport.maxDepth;
            } else {
                viewport.x = 0.0f;
                viewport.y = 0.0f;
                viewport.width = static_cast<float>(m_swapchain.GetExtent().width);
                viewport.height = static_cast<float>(m_swapchain.GetExtent().height);
                viewport.minDepth = 0.0f;
                viewport.maxDepth = 1.0f;
            }

            VkRect2D scissor{};
            if (drawCall.hasScissor) {
                scissor.offset = { drawCall.scissor.x, drawCall.scissor.y };
                scissor.extent = { drawCall.scissor.width, drawCall.scissor.height };
            } else {
                scissor.offset = { 0, 0 };
                scissor.extent = m_swapchain.GetExtent();
            }

            vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

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
            if (drawCall.indexed) {
                if (drawCall.indexCount > 0 && (hasBoundIndexBuffer || m_indexBuffer != VK_NULL_HANDLE)) {
                    vkCmdDrawIndexed(commandBuffer, drawCall.indexCount, drawCall.instanceCount, drawCall.firstIndex, drawCall.baseVertex, drawCall.startInstance);
                }
            } else {
                vkCmdDraw(commandBuffer, drawCall.vertexCount, drawCall.instanceCount, drawCall.startVertex, drawCall.startInstance);
            }
        }
	
	vkCmdEndRenderPass(commandBuffer);
}

bool VulkanDevice::CreateFramebuffers() {
	const auto& views = m_swapchain.GetImageViews();
	m_swapchainFramebuffers.resize(views.size());
	for (size_t i = 0; i < views.size(); ++i) {
		VkImageView attachments[] = { views[i], m_depthImageView };
		VkFramebufferCreateInfo fbInfo{};
		fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		fbInfo.renderPass = m_pipeline.GetRenderPass();
		fbInfo.attachmentCount = static_cast<uint32_t>(sizeof(attachments) / sizeof(attachments[0]));
		fbInfo.pAttachments = attachments;
		fbInfo.width = m_swapchain.GetExtent().width;
		fbInfo.height = m_swapchain.GetExtent().height;
		fbInfo.layers = 1;
		if (vkCreateFramebuffer(m_context.device, &fbInfo, nullptr, &m_swapchainFramebuffers[i]) != VK_SUCCESS) return false;
	}
	return true;
}

bool VulkanDevice::CreateCommandPool() {
	VkCommandPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = m_context.queueIndices.graphicsFamily;
	return vkCreateCommandPool(m_context.device, &poolInfo, nullptr, &m_commandPool) == VK_SUCCESS;
}

bool VulkanDevice::CreateCommandBuffer() {
	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = m_commandPool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = kMaxFramesInFlight;
	m_commandBuffers.resize(kMaxFramesInFlight);
	return vkAllocateCommandBuffers(m_context.device, &allocInfo, m_commandBuffers.data()) == VK_SUCCESS;
}

bool VulkanDevice::CreateSyncObjects() {
	VkSemaphoreCreateInfo semInfo{};
	semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	
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
	VkSamplerCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	info.magFilter = VK_FILTER_LINEAR;
	info.minFilter = VK_FILTER_LINEAR;
	info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
	return vkCreateSampler(m_context.device, &info, nullptr, &m_textureSampler) == VK_SUCCESS;
}

bool VulkanDevice::CreateLightingVolumeBuffers() {
	const VkDeviceSize bufferSize = sizeof(VulkanLightingVolumeProfile);
	m_lightingVolumeBuffers.resize(kMaxFramesInFlight, VK_NULL_HANDLE);
	m_lightingVolumeBufferMemories.resize(kMaxFramesInFlight, VK_NULL_HANDLE);
	m_lightingVolumeMappedBuffers.resize(kMaxFramesInFlight, nullptr);

	for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
		VulkanResources::CreateBuffer(
			m_context,
			bufferSize,
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			m_lightingVolumeBuffers[i],
			m_lightingVolumeBufferMemories[i]
		);

		if (vkMapMemory(m_context.device, m_lightingVolumeBufferMemories[i], 0, bufferSize, 0, &m_lightingVolumeMappedBuffers[i]) != VK_SUCCESS) {
			return false;
		}
		memcpy(m_lightingVolumeMappedBuffers[i], &m_lightingVolumeProfile, sizeof(m_lightingVolumeProfile));
	}

	return true;
}

bool VulkanDevice::CreateDepthResources() {
	const VkExtent2D extent = m_swapchain.GetExtent();
	if (m_depthFormat == VK_FORMAT_UNDEFINED) {
		m_depthFormat = FindDepthFormat();
	}

	VulkanResources::CreateImage(
		m_context,
		extent.width,
		extent.height,
		m_depthFormat,
		VK_IMAGE_TILING_OPTIMAL,
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		m_depthImage,
		m_depthImageMemory
	);

	m_depthImageView = VulkanResources::CreateImageView(m_context.device, m_depthImage, m_depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
	return true;
}

bool VulkanDevice::CreateDescriptorSetLayout() {
	// binding 0: 텍스처 sampler (FS)
	// binding 1: LightingVolumeProfile UBO (VS+FS)
	// binding 2: Shadow Map sampler (FS)               ← Shadow Map 추가
	// binding 3: ShadowMatrix UBO (lightViewProj, VS)  ← Shadow Map 추가
	std::array<VkDescriptorSetLayoutBinding, 4> bindings = {};
	bindings[0].binding = 0;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	bindings[1].binding = 1;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	bindings[2].binding = 2;
	bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[2].descriptorCount = 1;
	bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	bindings[3].binding = 3;
	bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	bindings[3].descriptorCount = 1;
	bindings[3].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	VkDescriptorSetLayoutCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	info.bindingCount = static_cast<uint32_t>(bindings.size());
	info.pBindings = bindings.data();
	return vkCreateDescriptorSetLayout(m_context.device, &info, nullptr, &m_descriptorSetLayout) == VK_SUCCESS;
}

bool VulkanDevice::CreateDescriptorPool() {
	// 셋 하나당: COMBINED_IMAGE_SAMPLER 2개 (텍스처 + 그림자맵), UNIFORM_BUFFER 2개 (라이팅 + 그림자행렬)
	std::array<VkDescriptorPoolSize, 2> poolSizes = {};
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	poolSizes[0].descriptorCount = kMaxFramesInFlight * 2;
	poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	poolSizes[1].descriptorCount = kMaxFramesInFlight * 2;
	VkDescriptorPoolCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	info.maxSets = kMaxFramesInFlight;
	info.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
	info.pPoolSizes = poolSizes.data();
	return vkCreateDescriptorPool(m_context.device, &info, nullptr, &m_descriptorPool) == VK_SUCCESS;
}

bool VulkanDevice::CreateDescriptorSets() {
	std::vector<VkDescriptorSetLayout> layouts(kMaxFramesInFlight, m_descriptorSetLayout);
	VkDescriptorSetAllocateInfo alloc{};
	alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc.descriptorPool = m_descriptorPool;
	alloc.descriptorSetCount = kMaxFramesInFlight;
	alloc.pSetLayouts = layouts.data();
	m_descriptorSets.resize(kMaxFramesInFlight);
	if (vkAllocateDescriptorSets(m_context.device, &alloc, m_descriptorSets.data()) != VK_SUCCESS) return false;
	for (size_t i = 0; i < kMaxFramesInFlight; i++) {
		std::vector<VkWriteDescriptorSet> writes;
		writes.reserve(4);

		VkDescriptorImageInfo img{};
		img.sampler = m_textureSampler;
		img.imageView = m_textureImageView;
		img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		if (m_textureImageView != VK_NULL_HANDLE) {
			VkWriteDescriptorSet imageWrite{};
			imageWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			imageWrite.dstSet = m_descriptorSets[i];
			imageWrite.dstBinding = 0;
			imageWrite.descriptorCount = 1;
			imageWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			imageWrite.pImageInfo = &img;
			writes.push_back(imageWrite);
		}

		VkDescriptorBufferInfo lightingBufferInfo{};
		lightingBufferInfo.buffer = m_lightingVolumeBuffers[i];
		lightingBufferInfo.offset = 0;
		lightingBufferInfo.range = sizeof(VulkanLightingVolumeProfile);
		VkWriteDescriptorSet lightingWrite{};
		lightingWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		lightingWrite.dstSet = m_descriptorSets[i];
		lightingWrite.dstBinding = 1;
		lightingWrite.descriptorCount = 1;
		lightingWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		lightingWrite.pBufferInfo = &lightingBufferInfo;
		writes.push_back(lightingWrite);

		// binding 2: ShadowMap sampler. ShadowPass가 매 프레임 기록 후 SHADER_READ_ONLY_OPTIMAL로 전이된다.
		VkDescriptorImageInfo shadowImg{};
		shadowImg.sampler = m_shadowMapSampler;
		shadowImg.imageView = m_shadowMapView;
		shadowImg.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		if (m_shadowMapView != VK_NULL_HANDLE && m_shadowMapSampler != VK_NULL_HANDLE) {
			VkWriteDescriptorSet shadowWrite{};
			shadowWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			shadowWrite.dstSet = m_descriptorSets[i];
			shadowWrite.dstBinding = 2;
			shadowWrite.descriptorCount = 1;
			shadowWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			shadowWrite.pImageInfo = &shadowImg;
			writes.push_back(shadowWrite);
		}

		// binding 3: ShadowMatrix UBO (LightViewProj). frame당 별도 버퍼.
		VkDescriptorBufferInfo shadowMatrixInfo{};
		if (i < m_shadowMatrixBuffers.size() && m_shadowMatrixBuffers[i] != VK_NULL_HANDLE) {
			shadowMatrixInfo.buffer = m_shadowMatrixBuffers[i];
			shadowMatrixInfo.offset = 0;
			shadowMatrixInfo.range = sizeof(m_shadowLightViewProj);
			VkWriteDescriptorSet shadowMatrixWrite{};
			shadowMatrixWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			shadowMatrixWrite.dstSet = m_descriptorSets[i];
			shadowMatrixWrite.dstBinding = 3;
			shadowMatrixWrite.descriptorCount = 1;
			shadowMatrixWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			shadowMatrixWrite.pBufferInfo = &shadowMatrixInfo;
			writes.push_back(shadowMatrixWrite);
		}

		vkUpdateDescriptorSets(m_context.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
	}
	return true;
}

void VulkanDevice::UpdateLightingVolumeBuffer() {
	if (m_currentFrameIndex >= m_lightingVolumeMappedBuffers.size() || m_lightingVolumeMappedBuffers[m_currentFrameIndex] == nullptr) {
		return;
	}

	memcpy(m_lightingVolumeMappedBuffers[m_currentFrameIndex], &m_lightingVolumeProfile, sizeof(m_lightingVolumeProfile));
}

void VulkanDevice::UpdateShadowMatrixBuffer() {
	if (m_currentFrameIndex >= m_shadowMatrixMapped.size() || m_shadowMatrixMapped[m_currentFrameIndex] == nullptr) {
		return;
	}
	memcpy(m_shadowMatrixMapped[m_currentFrameIndex], m_shadowLightViewProj, sizeof(m_shadowLightViewProj));
}

// =====================================================================
//  Shadow Map 인프라 구현
// =====================================================================

bool VulkanDevice::CreateShadowRenderPass() {
	// Depth-only RenderPass.
	// Color attachment 없음 (FS에서 픽셀 색을 출력하지 않음).
	// finalLayout을 SHADER_READ_ONLY_OPTIMAL로 두면 RenderPass 종료 시 자동 전이 → 별도 Barrier 불필요.
	VkAttachmentDescription depthAttachment{};
	depthAttachment.format = m_shadowMapFormat;
	depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;            // FS에서 sample 해야 하니 STORE
	depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	depthAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	VkAttachmentReference depthAttachmentRef{};
	depthAttachmentRef.attachment = 0;
	depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 0;
	subpass.pDepthStencilAttachment = &depthAttachmentRef;

	// 두 개의 dependency: 시작 전(이전 Pass의 sample 끝나야 깊이 쓰기 시작), 종료 후(쓰기 끝나야 다음 sample)
	std::array<VkSubpassDependency, 2> dependencies = {};
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0;
	dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

	dependencies[1].srcSubpass = 0;
	dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

	VkRenderPassCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	info.attachmentCount = 1;
	info.pAttachments = &depthAttachment;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	info.dependencyCount = static_cast<uint32_t>(dependencies.size());
	info.pDependencies = dependencies.data();
	return vkCreateRenderPass(m_context.device, &info, nullptr, &m_shadowRenderPass) == VK_SUCCESS;
}

bool VulkanDevice::CreateShadowMapResources() {
	// Depth Image
	VulkanResources::CreateImage(
		m_context,
		kShadowMapResolution,
		kShadowMapResolution,
		m_shadowMapFormat,
		VK_IMAGE_TILING_OPTIMAL,
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		m_shadowMapImage,
		m_shadowMapMemory);

	m_shadowMapView = VulkanResources::CreateImageView(
		m_context.device, m_shadowMapImage, m_shadowMapFormat, VK_IMAGE_ASPECT_DEPTH_BIT);

	// Sampler: CLAMP_TO_BORDER + 흰색 border → frustum 밖 픽셀은 자동으로 그림자 없음
	VkSamplerCreateInfo samplerInfo{};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;     // 1.0 = 그림자 안 받음
	samplerInfo.compareEnable = VK_FALSE;                              // PCF는 셰이더 측에서 직접 계산
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	if (vkCreateSampler(m_context.device, &samplerInfo, nullptr, &m_shadowMapSampler) != VK_SUCCESS) {
		return false;
	}

	// Framebuffer
	VkFramebufferCreateInfo fbInfo{};
	fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fbInfo.renderPass = m_shadowRenderPass;
	fbInfo.attachmentCount = 1;
	fbInfo.pAttachments = &m_shadowMapView;
	fbInfo.width = kShadowMapResolution;
	fbInfo.height = kShadowMapResolution;
	fbInfo.layers = 1;
	return vkCreateFramebuffer(m_context.device, &fbInfo, nullptr, &m_shadowFramebuffer) == VK_SUCCESS;
}

bool VulkanDevice::CreateShadowMatrixBuffers() {
	const VkDeviceSize bufferSize = sizeof(m_shadowLightViewProj);
	m_shadowMatrixBuffers.resize(kMaxFramesInFlight, VK_NULL_HANDLE);
	m_shadowMatrixMemories.resize(kMaxFramesInFlight, VK_NULL_HANDLE);
	m_shadowMatrixMapped.resize(kMaxFramesInFlight, nullptr);

	for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
		VulkanResources::CreateBuffer(
			m_context,
			bufferSize,
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			m_shadowMatrixBuffers[i],
			m_shadowMatrixMemories[i]);

		if (vkMapMemory(m_context.device, m_shadowMatrixMemories[i], 0, bufferSize, 0, &m_shadowMatrixMapped[i]) != VK_SUCCESS) {
			return false;
		}
		memcpy(m_shadowMatrixMapped[i], m_shadowLightViewProj, sizeof(m_shadowLightViewProj));
	}
	return true;
}

bool VulkanDevice::CreateShadowPipeline() {
	// PipelineLayout은 메인 PSO와 동일한 DescriptorSetLayout + PushConstantRange를 공유.
	// (셰이더가 같은 binding 3을 읽기 때문에 호환 가능)
	VkPushConstantRange pushConstantRange{};
	pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	pushConstantRange.offset = 0;
	pushConstantRange.size = 128;

	VkPipelineLayoutCreateInfo layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutInfo.setLayoutCount = 1;
	layoutInfo.pSetLayouts = &m_descriptorSetLayout;
	layoutInfo.pushConstantRangeCount = 1;
	layoutInfo.pPushConstantRanges = &pushConstantRange;
	if (vkCreatePipelineLayout(m_context.device, &layoutInfo, nullptr, &m_shadowPipelineLayout) != VK_SUCCESS) {
		return false;
	}

	// Shadow Vertex Shader 로드
	std::ifstream file(m_shaderOutputDirectory + "/shadow.vert.spv", std::ios::ate | std::ios::binary);
	if (!file.is_open()) {
		SDL_Log("Failed to open shadow.vert.spv");
		return false;
	}
	const size_t fileSize = static_cast<size_t>(file.tellg());
	std::vector<char> spirv(fileSize);
	file.seekg(0);
	file.read(spirv.data(), fileSize);
	file.close();

	VkShaderModuleCreateInfo shaderInfo{};
	shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shaderInfo.codeSize = spirv.size();
	shaderInfo.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
	VkShaderModule vertModule = VK_NULL_HANDLE;
	if (vkCreateShaderModule(m_context.device, &shaderInfo, nullptr, &vertModule) != VK_SUCCESS) {
		return false;
	}

	VkPipelineShaderStageCreateInfo stage{};
	stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
	stage.module = vertModule;
	stage.pName = "main";

	// Vertex input은 메인 PSO와 동일한 stride 32 layout (pos+normal+uv)
	VkVertexInputBindingDescription binding{};
	binding.binding = 0;
	binding.stride = sizeof(float) * 8;
	binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	std::array<VkVertexInputAttributeDescription, 3> attrs{};
	attrs[0].binding = 0; attrs[0].location = 0; attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[0].offset = 0;
	attrs[1].binding = 0; attrs[1].location = 1; attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[1].offset = sizeof(float) * 3;
	attrs[2].binding = 0; attrs[2].location = 2; attrs[2].format = VK_FORMAT_R32G32_SFLOAT;    attrs[2].offset = sizeof(float) * 6;

	VkPipelineVertexInputStateCreateInfo vertexInput{};
	vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInput.vertexBindingDescriptionCount = 1;
	vertexInput.pVertexBindingDescriptions = &binding;
	vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
	vertexInput.pVertexAttributeDescriptions = attrs.data();

	VkPipelineInputAssemblyStateCreateInfo ia{};
	ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo viewportState{};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rast{};
	rast.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rast.polygonMode = VK_POLYGON_MODE_FILL;
	rast.cullMode = VK_CULL_MODE_NONE;             // 평면 메시도 통과해야 함
	rast.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rast.lineWidth = 1.0f;
	rast.depthBiasEnable = VK_TRUE;                // Slope-scaled bias로 acne 완화
	rast.depthBiasConstantFactor = 1.25f;
	rast.depthBiasSlopeFactor = 1.75f;

	VkPipelineMultisampleStateCreateInfo ms{};
	ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo ds{};
	ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	ds.depthTestEnable = VK_TRUE;
	ds.depthWriteEnable = VK_TRUE;
	ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	ds.depthBoundsTestEnable = VK_FALSE;
	ds.stencilTestEnable = VK_FALSE;

	// Color blend는 attachment가 0개라도 구조체는 필요하다 (attachmentCount=0)
	VkPipelineColorBlendStateCreateInfo cb{};
	cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	cb.attachmentCount = 0;
	cb.pAttachments = nullptr;

	const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dyn{};
	dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dyn.dynamicStateCount = 2;
	dyn.pDynamicStates = dynamicStates;

	VkGraphicsPipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.stageCount = 1;
	pipelineInfo.pStages = &stage;                 // FS 없음
	pipelineInfo.pVertexInputState = &vertexInput;
	pipelineInfo.pInputAssemblyState = &ia;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &rast;
	pipelineInfo.pMultisampleState = &ms;
	pipelineInfo.pDepthStencilState = &ds;
	pipelineInfo.pColorBlendState = &cb;
	pipelineInfo.pDynamicState = &dyn;
	pipelineInfo.layout = m_shadowPipelineLayout;
	pipelineInfo.renderPass = m_shadowRenderPass;
	pipelineInfo.subpass = 0;

	const VkResult result = vkCreateGraphicsPipelines(m_context.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_shadowPipeline);
	vkDestroyShaderModule(m_context.device, vertModule, nullptr);
	return result == VK_SUCCESS;
}

void VulkanDevice::DestroyShadowResources() {
	if (m_context.device == VK_NULL_HANDLE) return;

	for (size_t i = 0; i < m_shadowMatrixBuffers.size(); ++i) {
		if (i < m_shadowMatrixMapped.size() && m_shadowMatrixMapped[i] != nullptr && i < m_shadowMatrixMemories.size()) {
			vkUnmapMemory(m_context.device, m_shadowMatrixMemories[i]);
		}
		if (m_shadowMatrixBuffers[i] != VK_NULL_HANDLE) {
			vkDestroyBuffer(m_context.device, m_shadowMatrixBuffers[i], nullptr);
		}
		if (i < m_shadowMatrixMemories.size() && m_shadowMatrixMemories[i] != VK_NULL_HANDLE) {
			vkFreeMemory(m_context.device, m_shadowMatrixMemories[i], nullptr);
		}
	}
	m_shadowMatrixBuffers.clear();
	m_shadowMatrixMemories.clear();
	m_shadowMatrixMapped.clear();

	if (m_shadowPipeline != VK_NULL_HANDLE) {
		vkDestroyPipeline(m_context.device, m_shadowPipeline, nullptr);
		m_shadowPipeline = VK_NULL_HANDLE;
	}
	if (m_shadowPipelineLayout != VK_NULL_HANDLE) {
		vkDestroyPipelineLayout(m_context.device, m_shadowPipelineLayout, nullptr);
		m_shadowPipelineLayout = VK_NULL_HANDLE;
	}
	if (m_shadowFramebuffer != VK_NULL_HANDLE) {
		vkDestroyFramebuffer(m_context.device, m_shadowFramebuffer, nullptr);
		m_shadowFramebuffer = VK_NULL_HANDLE;
	}
	if (m_shadowMapSampler != VK_NULL_HANDLE) {
		vkDestroySampler(m_context.device, m_shadowMapSampler, nullptr);
		m_shadowMapSampler = VK_NULL_HANDLE;
	}
	if (m_shadowMapView != VK_NULL_HANDLE) {
		vkDestroyImageView(m_context.device, m_shadowMapView, nullptr);
		m_shadowMapView = VK_NULL_HANDLE;
	}
	if (m_shadowMapImage != VK_NULL_HANDLE) {
		vkDestroyImage(m_context.device, m_shadowMapImage, nullptr);
		m_shadowMapImage = VK_NULL_HANDLE;
	}
	if (m_shadowMapMemory != VK_NULL_HANDLE) {
		vkFreeMemory(m_context.device, m_shadowMapMemory, nullptr);
		m_shadowMapMemory = VK_NULL_HANDLE;
	}
	if (m_shadowRenderPass != VK_NULL_HANDLE) {
		vkDestroyRenderPass(m_context.device, m_shadowRenderPass, nullptr);
		m_shadowRenderPass = VK_NULL_HANDLE;
	}
}

void VulkanDevice::RecreateSwapchain() {
	vkDeviceWaitIdle(m_context.device);
	DestroySwapchainResources();
	m_swapchain.Initialize(m_context, m_windowHandle);
	UpdateBackBufferMetadata();
	m_depthFormat = FindDepthFormat();
	m_pipeline.Initialize(m_context, m_swapchain.GetImageFormat(), m_depthFormat, m_swapchain.GetExtent(), m_descriptorSetLayout, m_shaderOutputDirectory, m_useVertexInput);
	CreateDepthResources();
	CreateFramebuffers();
	m_imagesInFlight.assign(m_swapchain.GetImageCount(), VK_NULL_HANDLE);
}

void VulkanDevice::DestroySwapchainResources() {
	for (auto fb : m_swapchainFramebuffers) vkDestroyFramebuffer(m_context.device, fb, nullptr);
	m_swapchainFramebuffers.clear();
	if (m_depthImageView != VK_NULL_HANDLE) {
		vkDestroyImageView(m_context.device, m_depthImageView, nullptr);
		m_depthImageView = VK_NULL_HANDLE;
	}
	if (m_depthImage != VK_NULL_HANDLE) {
		vkDestroyImage(m_context.device, m_depthImage, nullptr);
		m_depthImage = VK_NULL_HANDLE;
	}
	if (m_depthImageMemory != VK_NULL_HANDLE) {
		vkFreeMemory(m_context.device, m_depthImageMemory, nullptr);
		m_depthImageMemory = VK_NULL_HANDLE;
	}
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
		for (dy::RHI::IBuffer* buffer : m_ownedBuffers) delete buffer;
		m_ownedBuffers.clear();
		if (m_vertexBuffer != VK_NULL_HANDLE) { vkDestroyBuffer(m_context.device, m_vertexBuffer, nullptr); vkFreeMemory(m_context.device, m_vertexBufferMemory, nullptr); }
		if (m_indexBuffer != VK_NULL_HANDLE) { vkDestroyBuffer(m_context.device, m_indexBuffer, nullptr); vkFreeMemory(m_context.device, m_indexBufferMemory, nullptr); }
		if (m_textureSampler != VK_NULL_HANDLE) vkDestroySampler(m_context.device, m_textureSampler, nullptr);
		if (m_textureImageView != VK_NULL_HANDLE) vkDestroyImageView(m_context.device, m_textureImageView, nullptr);
		if (m_textureImage != VK_NULL_HANDLE) { vkDestroyImage(m_context.device, m_textureImage, nullptr); vkFreeMemory(m_context.device, m_textureImageMemory, nullptr); }
		for (size_t i = 0; i < m_lightingVolumeBuffers.size(); ++i) {
			if (m_lightingVolumeMappedBuffers[i] != nullptr) {
				vkUnmapMemory(m_context.device, m_lightingVolumeBufferMemories[i]);
			}
			if (m_lightingVolumeBuffers[i] != VK_NULL_HANDLE) {
				vkDestroyBuffer(m_context.device, m_lightingVolumeBuffers[i], nullptr);
			}
			if (m_lightingVolumeBufferMemories[i] != VK_NULL_HANDLE) {
				vkFreeMemory(m_context.device, m_lightingVolumeBufferMemories[i], nullptr);
			}
		}
		m_lightingVolumeBuffers.clear();
		m_lightingVolumeBufferMemories.clear();
		m_lightingVolumeMappedBuffers.clear();
		DestroyShadowResources();
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
	dy::RHI::TextureDesc desc{};
	desc.width = extent.width;
	desc.height = extent.height;
	desc.depthOrArraySize = 1;
	desc.mipLevels = 1;
	desc.format = dy::RHI::Format::R8G8B8A8_UNORM;
	desc.usage = dy::RHI::TextureUsage::RenderTarget;

	if (m_backBuffer == nullptr) {
		m_backBuffer = new VulkanTexture(desc);
		return;
	}

	static_cast<VulkanTexture*>(m_backBuffer)->Update(desc.width, desc.height, desc.format);
}

VkFormat VulkanDevice::FindDepthFormat() const {
	const std::array<VkFormat, 3> candidates = {
		VK_FORMAT_D32_SFLOAT,
		VK_FORMAT_D32_SFLOAT_S8_UINT,
		VK_FORMAT_D24_UNORM_S8_UINT
	};

	for (VkFormat format : candidates) {
		if (IsDepthFormatSupported(format)) {
			return format;
		}
	}

	throw std::runtime_error("failed to find supported depth format!");
}

bool VulkanDevice::IsDepthFormatSupported(VkFormat format) const {
	VkFormatProperties properties{};
	vkGetPhysicalDeviceFormatProperties(m_context.physicalDevice, format, &properties);
	return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
}


