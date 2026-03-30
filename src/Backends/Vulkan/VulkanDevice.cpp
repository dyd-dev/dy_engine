#include "VulkanDevice.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define SDL_Log(fmt, ...) { printf(fmt, ##__VA_ARGS__); printf("\n"); }

#ifndef VULKAN_SHADER_DIR
#define VULKAN_SHADER_DIR "."
#endif

#ifndef VULKAN_SHADER_SOURCE_DIR
#define VULKAN_SHADER_SOURCE_DIR "."
#endif

#ifndef VULKAN_GLSLANG_VALIDATOR_PATH
#define VULKAN_GLSLANG_VALIDATOR_PATH "glslangValidator"
#endif

namespace
{
	constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";

	bool IsValidationEnabled()
	{
#if defined(_DEBUG)
		return true;
#else
		return false;
#endif
	}
}

VulkanDevice::~VulkanDevice()
{
	DestroyDeviceResources();
}

bool VulkanDevice::Initialize(const WindowHandle& windowHandle, uint32_t width, uint32_t height)
{
	m_window = static_cast<GLFWwindow*>(windowHandle.windowPointer);
	m_windowWidth = width;
	m_windowHeight = height;
	m_shaderSourceDirectory = std::filesystem::path(VULKAN_SHADER_SOURCE_DIR);
	m_shaderOutputDirectory = std::filesystem::path(VULKAN_SHADER_DIR);

	// 기본 경로에서 못 찾을 경우 상대 경로로 한 번 더 시도
	if (!std::filesystem::exists(m_shaderSourceDirectory / "triangle.vert")) {
		std::filesystem::path fallbackPath = "src/Backends/Vulkan/Shaders";
		if (std::filesystem::exists(fallbackPath / "triangle.vert")) {
			m_shaderSourceDirectory = fallbackPath;
		}
	}

	if (!m_window)
	{
		SDL_Log("VulkanDevice Initialize failed: invalid window handle.");
		return false;
	}

	if (!CreateInstance()) return false;
	if (!CreateSurface()) return false;
	if (!PickPhysicalDevice()) return false;
	if (!CreateLogicalDevice()) return false;
	if (!CreateSwapchain()) return false;
	if (!CreateRenderPass()) return false;
	if (!CreateDescriptorSetLayout()) return false;
	if (!CreateGraphicsPipeline()) return false;
	if (!CreateCommandPool()) return false;
	if (!CreateTextureImage()) return false;
	if (!CreateTextureImageView()) return false;
	if (!CreateTextureSampler()) return false;
	if (!CreateFramebuffers()) return false;
	if (!CreateCommandBuffer()) return false;
	if (!CreateSyncObjects()) return false;
	if (!CreateDescriptorPool()) return false;
	if (!CreateDescriptorSets()) return false;

	return true;
}

void VulkanDevice::BeginFrame()
{
	m_frameReady = false;
	m_frameSubmitted = false;

	if (!m_device || !m_swapchain)
	{
		return;
	}

	CheckShaderHotReload();

	vkWaitForFences(m_device, 1, &m_inFlightFences[m_currentFrameIndex], VK_TRUE, UINT64_MAX);

	const VkResult acquireResult = vkAcquireNextImageKHR(
		m_device,
		m_swapchain,
		UINT64_MAX,
		m_imageAvailableSemaphores[m_currentFrameIndex],
		VK_NULL_HANDLE,
		&m_currentImageIndex
	);

	if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
	{
		RecreateSwapchain();
		return;
	}

	if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
	{
		SDL_Log("vkAcquireNextImageKHR failed with result %d", static_cast<int>(acquireResult));
		return;
	}

	if (m_imagesInFlight[m_currentImageIndex] != VK_NULL_HANDLE)
	{
		vkWaitForFences(m_device, 1, &m_imagesInFlight[m_currentImageIndex], VK_TRUE, UINT64_MAX);
	}

	vkResetFences(m_device, 1, &m_inFlightFences[m_currentFrameIndex]);
	m_frameReady = true;
}

void VulkanDevice::EndFrame()
{
}

void VulkanDevice::SubmitCommandList(RHICommandList* cmd)
{
	if (!m_frameReady || !cmd)
	{
		return;
	}

	VulkanCommandList* vulkanCmd = static_cast<VulkanCommandList*>(cmd);
	RecordCommandBuffer(*vulkanCmd);

	const VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
	const VkSubmitInfo submitInfo =
	{
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &m_imageAvailableSemaphores[m_currentFrameIndex],
		.pWaitDstStageMask = waitStages,
		.commandBufferCount = 1,
		.pCommandBuffers = &m_commandBuffers[m_currentFrameIndex],
		.signalSemaphoreCount = 1,
		.pSignalSemaphores = &m_renderFinishedSemaphores[m_currentFrameIndex]
	};

	m_imagesInFlight[m_currentImageIndex] = m_inFlightFences[m_currentFrameIndex];

	const VkResult submitResult = vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_inFlightFences[m_currentFrameIndex]);
	if (submitResult != VK_SUCCESS)
	{
		SDL_Log("vkQueueSubmit failed with result %d", static_cast<int>(submitResult));
		return;
	}

	m_frameSubmitted = true;
}

void VulkanDevice::Present()
{
	if (!m_frameSubmitted)
	{
		return;
	}

	const VkPresentInfoKHR presentInfo =
	{
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &m_renderFinishedSemaphores[m_currentFrameIndex],
		.swapchainCount = 1,
		.pSwapchains = &m_swapchain,
		.pImageIndices = &m_currentImageIndex
	};

	const VkResult presentResult = vkQueuePresentKHR(m_presentQueue, &presentInfo);
	if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR)
	{
		RecreateSwapchain();
	}
	else if (presentResult != VK_SUCCESS)
	{
		SDL_Log("vkQueuePresentKHR failed with result %d", static_cast<int>(presentResult));
	}

	m_currentFrameIndex = (m_currentFrameIndex + 1) % kMaxFramesInFlight;
}

bool VulkanDevice::CreateInstance()
{
	uint32_t extensionCount = 0;
	const char** extensions = glfwGetRequiredInstanceExtensions(&extensionCount);
	if (!extensions || extensionCount == 0)
	{
		printf("glfwGetRequiredInstanceExtensions failed\n");
		return false;
	}

	std::vector<const char*> enabledExtensions(extensions, extensions + extensionCount);
	std::vector<const char*> enabledLayers;
	if (IsValidationEnabled())
	{
		enabledLayers.push_back(kValidationLayerName);
	}

	const VkApplicationInfo appInfo =
	{
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "SuRengine Vulkan Triangle",
		.applicationVersion = VK_MAKE_VERSION(1, 0, 0),
		.pEngineName = "SuRengine",
		.engineVersion = VK_MAKE_VERSION(1, 0, 0),
		.apiVersion = VK_API_VERSION_1_0
	};

	const VkInstanceCreateInfo createInfo =
	{
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &appInfo,
		.enabledLayerCount = static_cast<uint32_t>(enabledLayers.size()),
		.ppEnabledLayerNames = enabledLayers.empty() ? nullptr : enabledLayers.data(),
		.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size()),
		.ppEnabledExtensionNames = enabledExtensions.data()
	};

	const VkResult result = vkCreateInstance(&createInfo, nullptr, &m_instance);
	if (result != VK_SUCCESS)
	{
		SDL_Log("vkCreateInstance failed with result %d", static_cast<int>(result));
		return false;
	}

	return true;
}

bool VulkanDevice::CreateSurface()
{
	if (glfwCreateWindowSurface(m_instance, m_window, nullptr, &m_surface) != VK_SUCCESS)
	{
		printf("glfwCreateWindowSurface failed\n");
		return false;
	}

	return true;
}

bool VulkanDevice::PickPhysicalDevice()
{
	uint32_t deviceCount = 0;
	vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
	if (deviceCount == 0)
	{
		SDL_Log("No Vulkan physical device was found.");
		return false;
	}

	std::vector<VkPhysicalDevice> devices(deviceCount);
	vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

	for (VkPhysicalDevice device : devices)
	{
		const QueueFamilyIndices indices = FindQueueFamilies(device);
		const SwapchainSupportDetails swapchainSupport = QuerySwapchainSupport(device);
		if (!indices.IsComplete() || swapchainSupport.formats.empty() || swapchainSupport.presentModes.empty())
		{
			continue;
		}

		m_physicalDevice = device;
		return true;
	}

	SDL_Log("No suitable Vulkan GPU supports graphics and present.");
	return false;
}

bool VulkanDevice::CreateLogicalDevice()
{
	const QueueFamilyIndices indices = FindQueueFamilies(m_physicalDevice);
	if (!indices.IsComplete())
	{
		return false;
	}

	std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
	std::vector<uint32_t> uniqueFamilies;
	uniqueFamilies.push_back(indices.graphicsFamily);
	if (indices.presentFamily != indices.graphicsFamily)
	{
		uniqueFamilies.push_back(indices.presentFamily);
	}

	const float queuePriority = 1.0f;
	for (uint32_t family : uniqueFamilies)
	{
		queueCreateInfos.push_back(VkDeviceQueueCreateInfo
		{
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.queueFamilyIndex = family,
			.queueCount = 1,
			.pQueuePriorities = &queuePriority
		});
	}

	const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	const VkPhysicalDeviceFeatures deviceFeatures = {};

	const VkDeviceCreateInfo createInfo =
	{
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size()),
		.pQueueCreateInfos = queueCreateInfos.data(),
		.enabledExtensionCount = 1,
		.ppEnabledExtensionNames = deviceExtensions,
		.pEnabledFeatures = &deviceFeatures
	};

	const VkResult result = vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device);
	if (result != VK_SUCCESS)
	{
		SDL_Log("vkCreateDevice failed with result %d", static_cast<int>(result));
		return false;
	}

	vkGetDeviceQueue(m_device, indices.graphicsFamily, 0, &m_graphicsQueue);
	vkGetDeviceQueue(m_device, indices.presentFamily, 0, &m_presentQueue);
	return true;
}

bool VulkanDevice::CreateSwapchain()
{
	const SwapchainSupportDetails support = QuerySwapchainSupport(m_physicalDevice);
	if (support.formats.empty() || support.presentModes.empty())
	{
		SDL_Log("Swapchain support is incomplete.");
		return false;
	}

	const VkSurfaceFormatKHR surfaceFormat = ChooseSwapSurfaceFormat(support.formats);
	const VkPresentModeKHR presentMode = ChoosePresentMode(support.presentModes);
	const VkExtent2D extent = ChooseSwapExtent(support.capabilities);

	uint32_t imageCount = support.capabilities.minImageCount + 1;
	if (support.capabilities.maxImageCount > 0)
	{
		imageCount = std::min(imageCount, support.capabilities.maxImageCount);
	}

	const QueueFamilyIndices indices = FindQueueFamilies(m_physicalDevice);
	const uint32_t queueFamilyIndices[] = { indices.graphicsFamily, indices.presentFamily };

	VkSwapchainCreateInfoKHR createInfo =
	{
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface = m_surface,
		.minImageCount = imageCount,
		.imageFormat = surfaceFormat.format,
		.imageColorSpace = surfaceFormat.colorSpace,
		.imageExtent = extent,
		.imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		.preTransform = support.capabilities.currentTransform,
		.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		.presentMode = presentMode,
		.clipped = VK_TRUE
	};

	if (indices.graphicsFamily != indices.presentFamily)
	{
		createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
		createInfo.queueFamilyIndexCount = 2;
		createInfo.pQueueFamilyIndices = queueFamilyIndices;
	}
	else
	{
		createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	}

	const VkResult createResult = vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &m_swapchain);
	if (createResult != VK_SUCCESS)
	{
		SDL_Log("vkCreateSwapchainKHR failed with result %d", static_cast<int>(createResult));
		return false;
	}

	vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, nullptr);
	m_swapchainImages.resize(imageCount);
	vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, m_swapchainImages.data());

	m_swapchainImageFormat = surfaceFormat.format;
	m_swapchainExtent = extent;
	m_imagesInFlight.assign(m_swapchainImages.size(), VK_NULL_HANDLE);

	m_swapchainImageViews.resize(m_swapchainImages.size());
	for (size_t i = 0; i < m_swapchainImages.size(); ++i)
	{
		const VkImageViewCreateInfo viewInfo =
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = m_swapchainImages[i],
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = m_swapchainImageFormat,
			.components =
			{
				.r = VK_COMPONENT_SWIZZLE_IDENTITY,
				.g = VK_COMPONENT_SWIZZLE_IDENTITY,
				.b = VK_COMPONENT_SWIZZLE_IDENTITY,
				.a = VK_COMPONENT_SWIZZLE_IDENTITY
			},
			.subresourceRange =
			{
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1
			}
		};

		if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_swapchainImageViews[i]) != VK_SUCCESS)
		{
			SDL_Log("vkCreateImageView failed for swapchain image %u", static_cast<unsigned>(i));
			return false;
		}
	}

	return true;
}

bool VulkanDevice::CreateRenderPass()
{
	const VkAttachmentDescription colorAttachment =
	{
		.format = m_swapchainImageFormat,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
	};

	const VkAttachmentReference colorAttachmentRef =
	{
		.attachment = 0,
		.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
	};

	const VkSubpassDescription subpass =
	{
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &colorAttachmentRef
	};

	const VkSubpassDependency dependency =
	{
		.srcSubpass = VK_SUBPASS_EXTERNAL,
		.dstSubpass = 0,
		.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
	};

	const VkRenderPassCreateInfo renderPassInfo =
	{
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &colorAttachment,
		.subpassCount = 1,
		.pSubpasses = &subpass,
		.dependencyCount = 1,
		.pDependencies = &dependency
	};

	const VkResult result = vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass);
	if (result != VK_SUCCESS)
	{
		SDL_Log("vkCreateRenderPass failed with result %d", static_cast<int>(result));
		return false;
	}

	return true;
}

bool VulkanDevice::CreateGraphicsPipeline()
{
	const std::string shaderDirectory = m_shaderOutputDirectory.empty()
		? std::string(VULKAN_SHADER_DIR)
		: m_shaderOutputDirectory.string();
	const std::string vertPath = shaderDirectory + "/triangle.vert.spv";
	const std::string fragPath = shaderDirectory + "/triangle.frag.spv";

	const VkShaderModule vertShaderModule = LoadShaderModule(vertPath.c_str());
	const VkShaderModule fragShaderModule = LoadShaderModule(fragPath.c_str());
	if (vertShaderModule == VK_NULL_HANDLE || fragShaderModule == VK_NULL_HANDLE)
	{
		if (vertShaderModule != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, vertShaderModule, nullptr);
		if (fragShaderModule != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, fragShaderModule, nullptr);
		return false;
	}

	const VkPipelineShaderStageCreateInfo vertShaderStageInfo =
	{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_VERTEX_BIT,
		.module = vertShaderModule,
		.pName = "main"
	};

	const VkPipelineShaderStageCreateInfo fragShaderStageInfo =
	{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
		.module = fragShaderModule,
		.pName = "main"
	};

	const VkPipelineShaderStageCreateInfo shaderStages[] = { vertShaderStageInfo, fragShaderStageInfo };

	const VkPipelineVertexInputStateCreateInfo vertexInputInfo =
	{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
	};

	const VkPipelineInputAssemblyStateCreateInfo inputAssembly =
	{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		.primitiveRestartEnable = VK_FALSE
	};

	const VkViewport viewport =
	{
		.x = 0.0f,
		.y = 0.0f,
		.width = static_cast<float>(m_swapchainExtent.width),
		.height = static_cast<float>(m_swapchainExtent.height),
		.minDepth = 0.0f,
		.maxDepth = 1.0f
	};

	const VkRect2D scissor =
	{
		.offset = { 0, 0 },
		.extent = m_swapchainExtent
	};

	const VkPipelineViewportStateCreateInfo viewportState =
	{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.pViewports = &viewport,
		.scissorCount = 1,
		.pScissors = &scissor
	};

	const VkPipelineRasterizationStateCreateInfo rasterizer =
	{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.depthClampEnable = VK_FALSE,
		.rasterizerDiscardEnable = VK_FALSE,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0f
	};

	const VkPipelineMultisampleStateCreateInfo multisampling =
	{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
	};

	const VkPipelineColorBlendAttachmentState colorBlendAttachment =
	{
		.blendEnable = VK_TRUE,
		.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
		.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.colorBlendOp = VK_BLEND_OP_ADD,
		.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
		.alphaBlendOp = VK_BLEND_OP_ADD,
		.colorWriteMask =
			VK_COLOR_COMPONENT_R_BIT |
			VK_COLOR_COMPONENT_G_BIT |
			VK_COLOR_COMPONENT_B_BIT |
			VK_COLOR_COMPONENT_A_BIT
	};

	const VkPipelineColorBlendStateCreateInfo colorBlending =
	{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &colorBlendAttachment
	};

	const VkPipelineLayoutCreateInfo pipelineLayoutInfo =
	{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &m_descriptorSetLayout
	};

	VkPipelineLayout newPipelineLayout = VK_NULL_HANDLE;
	if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &newPipelineLayout) != VK_SUCCESS)
	{
		SDL_Log("vkCreatePipelineLayout failed.");
		vkDestroyShaderModule(m_device, fragShaderModule, nullptr);
		vkDestroyShaderModule(m_device, vertShaderModule, nullptr);
		return false;
	}

	VkPipeline newGraphicsPipeline = VK_NULL_HANDLE;
	const VkGraphicsPipelineCreateInfo pipelineInfo =
	{
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2,
		.pStages = shaderStages,
		.pVertexInputState = &vertexInputInfo,
		.pInputAssemblyState = &inputAssembly,
		.pViewportState = &viewportState,
		.pRasterizationState = &rasterizer,
		.pMultisampleState = &multisampling,
		.pColorBlendState = &colorBlending,
		.layout = newPipelineLayout,
		.renderPass = m_renderPass,
		.subpass = 0
	};

	const VkResult pipelineResult = vkCreateGraphicsPipelines(
		m_device,
		VK_NULL_HANDLE,
		1,
		&pipelineInfo,
		nullptr,
		&newGraphicsPipeline
	);

	vkDestroyShaderModule(m_device, fragShaderModule, nullptr);
	vkDestroyShaderModule(m_device, vertShaderModule, nullptr);

	if (pipelineResult != VK_SUCCESS)
	{
		vkDestroyPipelineLayout(m_device, newPipelineLayout, nullptr);
		SDL_Log("vkCreateGraphicsPipelines failed with result %d", static_cast<int>(pipelineResult));
		return false;
	}

	if (m_graphicsPipeline != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(m_device, m_graphicsPipeline, nullptr);
	}

	if (m_pipelineLayout != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
	}

	m_pipelineLayout = newPipelineLayout;
	m_graphicsPipeline = newGraphicsPipeline;
	return true;
}

bool VulkanDevice::CreateFramebuffers()
{
	m_swapchainFramebuffers.resize(m_swapchainImageViews.size());
	for (size_t i = 0; i < m_swapchainImageViews.size(); ++i)
	{
		const VkImageView attachments[] = { m_swapchainImageViews[i] };

		const VkFramebufferCreateInfo framebufferInfo =
		{
			.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass = m_renderPass,
			.attachmentCount = 1,
			.pAttachments = attachments,
			.width = m_swapchainExtent.width,
			.height = m_swapchainExtent.height,
			.layers = 1
		};

		if (vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_swapchainFramebuffers[i]) != VK_SUCCESS)
		{
			SDL_Log("vkCreateFramebuffer failed for framebuffer %u", static_cast<unsigned>(i));
			return false;
		}
	}

	return true;
}

bool VulkanDevice::CreateCommandPool()
{
	const QueueFamilyIndices indices = FindQueueFamilies(m_physicalDevice);
	const VkCommandPoolCreateInfo poolInfo =
	{
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = indices.graphicsFamily
	};

	if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS)
	{
		SDL_Log("vkCreateCommandPool failed.");
		return false;
	}

	return true;
}

bool VulkanDevice::CreateCommandBuffer()
{
	const VkCommandBufferAllocateInfo allocInfo =
	{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = m_commandPool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = kMaxFramesInFlight
	};

	m_commandBuffers.resize(kMaxFramesInFlight);
	if (vkAllocateCommandBuffers(m_device, &allocInfo, m_commandBuffers.data()) != VK_SUCCESS)
	{
		SDL_Log("vkAllocateCommandBuffers failed.");
		return false;
	}

	return true;
}

bool VulkanDevice::CreateSyncObjects()
{
	const VkSemaphoreCreateInfo semaphoreInfo =
	{
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
	};

	const VkFenceCreateInfo fenceInfo =
	{
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.flags = VK_FENCE_CREATE_SIGNALED_BIT
	};

	m_imageAvailableSemaphores.resize(kMaxFramesInFlight, VK_NULL_HANDLE);
	m_renderFinishedSemaphores.resize(kMaxFramesInFlight, VK_NULL_HANDLE);
	m_inFlightFences.resize(kMaxFramesInFlight, VK_NULL_HANDLE);

	for (uint32_t frameIndex = 0; frameIndex < kMaxFramesInFlight; ++frameIndex)
	{
		if (vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_imageAvailableSemaphores[frameIndex]) != VK_SUCCESS ||
			vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_renderFinishedSemaphores[frameIndex]) != VK_SUCCESS ||
			vkCreateFence(m_device, &fenceInfo, nullptr, &m_inFlightFences[frameIndex]) != VK_SUCCESS)
		{
			SDL_Log("Failed to create Vulkan synchronization objects.");
			return false;
		}
	}

	return true;
}

bool VulkanDevice::CheckShaderHotReload()
{
	if (m_shaderSourceDirectory.empty())
	{
		return false;
	}

	const std::filesystem::path vertexSourcePath = m_shaderSourceDirectory / "triangle.vert";
	const std::filesystem::path fragmentSourcePath = m_shaderSourceDirectory / "triangle.frag";

	std::error_code vertexError;
	std::error_code fragmentError;
	
	// 파일 존재 여부 먼저 확인 (타임스탬프 오류 방지)
	if (!std::filesystem::exists(vertexSourcePath) || !std::filesystem::exists(fragmentSourcePath))
	{
		return false;
	}

	const auto vertexTime = std::filesystem::last_write_time(vertexSourcePath, vertexError);
	const auto fragmentTime = std::filesystem::last_write_time(fragmentSourcePath, fragmentError);
	
	if (vertexError || fragmentError)
	{
		return false;
	}

	// 성공적으로 타임스탬프를 읽었을 때만 초기화 완료로 표시
	if (!m_shaderHotReloadInitialized)
	{
		m_vertexShaderTimestamp = vertexTime;
		m_fragmentShaderTimestamp = fragmentTime;
		m_shaderHotReloadInitialized = true;
		return false;
	}

	if (vertexTime == m_vertexShaderTimestamp && fragmentTime == m_fragmentShaderTimestamp)
	{
		return false;
	}

	m_vertexShaderTimestamp = vertexTime;
	m_fragmentShaderTimestamp = fragmentTime;
	return ReloadShaders();
}

bool VulkanDevice::ReloadShaders()
{
	const std::filesystem::path vertexSourcePath = m_shaderSourceDirectory / "triangle.vert";
	const std::filesystem::path fragmentSourcePath = m_shaderSourceDirectory / "triangle.frag";
	const std::filesystem::path vertexOutputPath = m_shaderOutputDirectory / "triangle.vert.spv";
	const std::filesystem::path fragmentOutputPath = m_shaderOutputDirectory / "triangle.frag.spv";

	if (!CompileShaderSource(vertexSourcePath, vertexOutputPath, "vertex"))
	{
		return false;
	}

	if (!CompileShaderSource(fragmentSourcePath, fragmentOutputPath, "fragment"))
	{
		return false;
	}

	vkDeviceWaitIdle(m_device);
	if (!CreateGraphicsPipeline())
	{
		return false;
	}

	return true;
}

bool VulkanDevice::CompileShaderSource(const std::filesystem::path& sourcePath, const std::filesystem::path& outputPath, const char* stageLabel) const
{
	std::error_code createDirectoryError;
	std::filesystem::create_directories(outputPath.parent_path(), createDirectoryError);
	if (createDirectoryError)
	{
		return false;
	}

	// 윈도우 쉘(cmd)과 리눅스 쉘에서 인식 가능한 경로 형태로 변환
	std::filesystem::path vPath = std::filesystem::path(VULKAN_GLSLANG_VALIDATOR_PATH);
	std::filesystem::path sPath = sourcePath;
	std::filesystem::path oPath = outputPath;

	vPath.make_preferred();
	sPath.make_preferred();
	oPath.make_preferred();

	std::string commandLine;
#if defined(_WIN32)
	// 윈도우 cmd /c 특성 대응: 전체 명령줄을 다시 한 번 따옴표로 감싸야 함
	commandLine = "\"\"" + vPath.string() + "\" -V \"" + sPath.string() + "\" -o \"" + oPath.string() + "\"\"";
#else
	// 리눅스/Mac(sh/bash)에서는 각각의 경로만 따옴표로 감싸면 됨
	commandLine = "\"" + vPath.string() + "\" -V \"" + sPath.string() + "\" -o \"" + oPath.string() + "\"";
#endif

	int result = std::system(commandLine.c_str());

	return (result == 0);
}

bool VulkanDevice::RecreateSwapchain()
{
	if (!m_device)
	{
		return false;
	}

	int pixelWidth = 0;
	int pixelHeight = 0;
	glfwGetFramebufferSize(m_window, &pixelWidth, &pixelHeight);
	if (pixelWidth <= 0 || pixelHeight <= 0)
	{
		return false;
	}

	vkDeviceWaitIdle(m_device);
	DestroySwapchainResources();

	return CreateSwapchain() &&
		CreateRenderPass() &&
		CreateGraphicsPipeline() &&
		CreateFramebuffers();
}

void VulkanDevice::DestroySwapchainResources()
{
	for (VkFramebuffer framebuffer : m_swapchainFramebuffers)
	{
		if (framebuffer != VK_NULL_HANDLE)
		{
			vkDestroyFramebuffer(m_device, framebuffer, nullptr);
		}
	}
	m_swapchainFramebuffers.clear();

	if (m_graphicsPipeline != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(m_device, m_graphicsPipeline, nullptr);
		m_graphicsPipeline = VK_NULL_HANDLE;
	}

	if (m_pipelineLayout != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
		m_pipelineLayout = VK_NULL_HANDLE;
	}

	if (m_renderPass != VK_NULL_HANDLE)
	{
		vkDestroyRenderPass(m_device, m_renderPass, nullptr);
		m_renderPass = VK_NULL_HANDLE;
	}

	for (VkImageView imageView : m_swapchainImageViews)
	{
		if (imageView != VK_NULL_HANDLE)
		{
			vkDestroyImageView(m_device, imageView, nullptr);
		}
	}
	m_swapchainImageViews.clear();
	m_swapchainImages.clear();
	m_imagesInFlight.clear();

	if (m_swapchain != VK_NULL_HANDLE)
	{
		vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
		m_swapchain = VK_NULL_HANDLE;
	}
}

void VulkanDevice::DestroyDeviceResources()
{
	if (m_device != VK_NULL_HANDLE)
	{
		vkDeviceWaitIdle(m_device);
	}

	m_imagesInFlight.clear();

	for (VkSemaphore semaphore : m_imageAvailableSemaphores)
	{
		if (semaphore != VK_NULL_HANDLE)
		{
			vkDestroySemaphore(m_device, semaphore, nullptr);
		}
	}
	m_imageAvailableSemaphores.clear();

	for (VkSemaphore semaphore : m_renderFinishedSemaphores)
	{
		if (semaphore != VK_NULL_HANDLE)
		{
			vkDestroySemaphore(m_device, semaphore, nullptr);
		}
	}
	m_renderFinishedSemaphores.clear();

	for (VkFence fence : m_inFlightFences)
	{
		if (fence != VK_NULL_HANDLE)
		{
			vkDestroyFence(m_device, fence, nullptr);
		}
	}
	m_inFlightFences.clear();

	if (m_commandPool != VK_NULL_HANDLE)
	{
		vkDestroyCommandPool(m_device, m_commandPool, nullptr);
		m_commandPool = VK_NULL_HANDLE;
		m_commandBuffers.clear();
	}

	DestroySwapchainResources();

	if (m_textureSampler != VK_NULL_HANDLE) {
		vkDestroySampler(m_device, m_textureSampler, nullptr);
		m_textureSampler = VK_NULL_HANDLE;
	}
	if (m_textureImageView != VK_NULL_HANDLE) {
		vkDestroyImageView(m_device, m_textureImageView, nullptr);
		m_textureImageView = VK_NULL_HANDLE;
	}
	if (m_textureImage != VK_NULL_HANDLE) {
		vkDestroyImage(m_device, m_textureImage, nullptr);
		m_textureImage = VK_NULL_HANDLE;
	}
	if (m_textureImageMemory != VK_NULL_HANDLE) {
		vkFreeMemory(m_device, m_textureImageMemory, nullptr);
		m_textureImageMemory = VK_NULL_HANDLE;
	}
	if (m_descriptorPool != VK_NULL_HANDLE) {
		vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
		m_descriptorPool = VK_NULL_HANDLE;
	}
	if (m_descriptorSetLayout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
		m_descriptorSetLayout = VK_NULL_HANDLE;
	}

	if (m_device != VK_NULL_HANDLE)
	{
		vkDestroyDevice(m_device, nullptr);
		m_device = VK_NULL_HANDLE;
	}

	if (m_surface != VK_NULL_HANDLE)
	{
		vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
		m_surface = VK_NULL_HANDLE;
	}

	if (m_instance != VK_NULL_HANDLE)
	{
		vkDestroyInstance(m_instance, nullptr);
		m_instance = VK_NULL_HANDLE;
	}
}

void VulkanDevice::RecordCommandBuffer(const VulkanCommandList& commandList)
{
	VkCommandBuffer commandBuffer = m_commandBuffers[m_currentFrameIndex];
	vkResetCommandBuffer(commandBuffer, 0);

	const VkCommandBufferBeginInfo beginInfo =
	{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
	};

	vkBeginCommandBuffer(commandBuffer, &beginInfo);

	VkClearValue clearValue = {};
	clearValue.color = commandList.GetClearColor();

	const VkRenderPassBeginInfo renderPassInfo =
	{
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = m_renderPass,
		.framebuffer = m_swapchainFramebuffers[m_currentImageIndex],
		.renderArea =
		{
			.offset = { 0, 0 },
			.extent = m_swapchainExtent
		},
		.clearValueCount = 1,
		.pClearValues = &clearValue
	};

	vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_graphicsPipeline);
	vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_descriptorSets[m_currentFrameIndex], 0, nullptr);
	vkCmdDraw(commandBuffer, 6, 1, 0, 0);
	vkCmdEndRenderPass(commandBuffer);

	vkEndCommandBuffer(commandBuffer);
}

VulkanDevice::QueueFamilyIndices VulkanDevice::FindQueueFamilies(VkPhysicalDevice device) const
{
	QueueFamilyIndices indices;

	uint32_t queueFamilyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
	std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

	for (uint32_t index = 0; index < queueFamilyCount; ++index)
	{
		if (queueFamilies[index].queueFlags & VK_QUEUE_GRAPHICS_BIT)
		{
			indices.graphicsFamily = index;
		}

		VkBool32 presentSupported = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR(device, index, m_surface, &presentSupported);
		if (presentSupported)
		{
			indices.presentFamily = index;
		}

		if (indices.IsComplete())
		{
			break;
		}
	}

	return indices;
}

VulkanDevice::SwapchainSupportDetails VulkanDevice::QuerySwapchainSupport(VkPhysicalDevice device) const
{
	SwapchainSupportDetails details;

	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, m_surface, &details.capabilities);

	uint32_t formatCount = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_surface, &formatCount, nullptr);
	if (formatCount > 0)
	{
		details.formats.resize(formatCount);
		vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_surface, &formatCount, details.formats.data());
	}

	uint32_t presentModeCount = 0;
	vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_surface, &presentModeCount, nullptr);
	if (presentModeCount > 0)
	{
		details.presentModes.resize(presentModeCount);
		vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_surface, &presentModeCount, details.presentModes.data());
	}

	return details;
}

VkSurfaceFormatKHR VulkanDevice::ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const
{
	for (const VkSurfaceFormatKHR& format : formats)
	{
		if (format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
		{
			return format;
		}
	}

	return formats.front();
}

VkPresentModeKHR VulkanDevice::ChoosePresentMode(const std::vector<VkPresentModeKHR>& presentModes) const
{
	for (VkPresentModeKHR presentMode : presentModes)
	{
		if (presentMode == VK_PRESENT_MODE_MAILBOX_KHR)
		{
			return presentMode;
		}
	}

	return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanDevice::ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) const
{
	if (capabilities.currentExtent.width != UINT32_MAX)
	{
		return capabilities.currentExtent;
	}

	int pixelWidth = 0;
	int pixelHeight = 0;
	glfwGetFramebufferSize(m_window, &pixelWidth, &pixelHeight);

	VkExtent2D actualExtent =
	{
		static_cast<uint32_t>(pixelWidth),
		static_cast<uint32_t>(pixelHeight)
	};

	actualExtent.width = std::clamp(
		actualExtent.width,
		capabilities.minImageExtent.width,
		capabilities.maxImageExtent.width
	);
	actualExtent.height = std::clamp(
		actualExtent.height,
		capabilities.minImageExtent.height,
		capabilities.maxImageExtent.height
	);

	return actualExtent;
}

VkShaderModule VulkanDevice::LoadShaderModule(const char* path) const
{
	std::ifstream shaderFile(path, std::ios::ate | std::ios::binary);
	if (!shaderFile.is_open())
	{
		SDL_Log("Failed to open shader file: %s", path);
		return VK_NULL_HANDLE;
	}

	const size_t fileSize = static_cast<size_t>(shaderFile.tellg());
	if (fileSize == 0 || (fileSize % sizeof(uint32_t)) != 0)
	{
		SDL_Log("Invalid shader bytecode size: %s", path);
		return VK_NULL_HANDLE;
	}

	std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));
	shaderFile.seekg(0);
	shaderFile.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(fileSize));
	shaderFile.close();

	const VkShaderModuleCreateInfo createInfo =
	{
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = fileSize,
		.pCode = buffer.data()
	};

	VkShaderModule shaderModule = VK_NULL_HANDLE;
	if (vkCreateShaderModule(m_device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS)
	{
		SDL_Log("vkCreateShaderModule failed for: %s", path);
		return VK_NULL_HANDLE;
	}

	return shaderModule;
}

bool VulkanDevice::CreateDescriptorSetLayout()
{
	VkDescriptorSetLayoutBinding samplerLayoutBinding = {};
	samplerLayoutBinding.binding = 0;
	samplerLayoutBinding.descriptorCount = 1;
	samplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	samplerLayoutBinding.pImmutableSamplers = nullptr;
	samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutCreateInfo layoutInfo = {};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = 1;
	layoutInfo.pBindings = &samplerLayoutBinding;

	if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS)
	{
		SDL_Log("vkCreateDescriptorSetLayout failed.");
		return false;
	}

	return true;
}

bool VulkanDevice::CreateDescriptorPool()
{
	VkDescriptorPoolSize poolSize = {};
	poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	poolSize.descriptorCount = static_cast<uint32_t>(kMaxFramesInFlight);

	VkDescriptorPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.poolSizeCount = 1;
	poolInfo.pPoolSizes = &poolSize;
	poolInfo.maxSets = static_cast<uint32_t>(kMaxFramesInFlight);

	if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
	{
		SDL_Log("vkCreateDescriptorPool failed.");
		return false;
	}
	return true;
}

bool VulkanDevice::CreateDescriptorSets()
{
	std::vector<VkDescriptorSetLayout> layouts(kMaxFramesInFlight, m_descriptorSetLayout);
	VkDescriptorSetAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = m_descriptorPool;
	allocInfo.descriptorSetCount = static_cast<uint32_t>(kMaxFramesInFlight);
	allocInfo.pSetLayouts = layouts.data();

	m_descriptorSets.resize(kMaxFramesInFlight);
	if (vkAllocateDescriptorSets(m_device, &allocInfo, m_descriptorSets.data()) != VK_SUCCESS)
	{
		SDL_Log("vkAllocateDescriptorSets failed.");
		return false;
	}

	for (size_t i = 0; i < kMaxFramesInFlight; i++)
	{
		VkDescriptorImageInfo imageInfo = {};
		imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		imageInfo.imageView = m_textureImageView;
		imageInfo.sampler = m_textureSampler;

		VkWriteDescriptorSet descriptorWrite = {};
		descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorWrite.dstSet = m_descriptorSets[i];
		descriptorWrite.dstBinding = 0;
		descriptorWrite.dstArrayElement = 0;
		descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		descriptorWrite.descriptorCount = 1;
		descriptorWrite.pImageInfo = &imageInfo;

		vkUpdateDescriptorSets(m_device, 1, &descriptorWrite, 0, nullptr);
	}

	return true;
}

uint32_t VulkanDevice::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const
{
	VkPhysicalDeviceMemoryProperties memProperties;
	vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);

	for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
	{
		if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
		{
			return i;
		}
	}
	return 0;
}

void VulkanDevice::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory)
{
	VkBufferCreateInfo bufferInfo = {};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	vkCreateBuffer(m_device, &bufferInfo, nullptr, &buffer);

	VkMemoryRequirements memRequirements;
	vkGetBufferMemoryRequirements(m_device, buffer, &memRequirements);

	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, properties);

	vkAllocateMemory(m_device, &allocInfo, nullptr, &bufferMemory);
	vkBindBufferMemory(m_device, buffer, bufferMemory, 0);
}

VkCommandBuffer VulkanDevice::BeginSingleTimeCommands()
{
	VkCommandBufferAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandPool = m_commandPool;
	allocInfo.commandBufferCount = 1;

	VkCommandBuffer commandBuffer;
	vkAllocateCommandBuffers(m_device, &allocInfo, &commandBuffer);

	VkCommandBufferBeginInfo beginInfo = {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	vkBeginCommandBuffer(commandBuffer, &beginInfo);
	return commandBuffer;
}

void VulkanDevice::EndSingleTimeCommands(VkCommandBuffer commandBuffer)
{
	vkEndCommandBuffer(commandBuffer);

	VkSubmitInfo submitInfo = {};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;

	vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
	vkQueueWaitIdle(m_graphicsQueue);

	vkFreeCommandBuffers(m_device, m_commandPool, 1, &commandBuffer);
}

void VulkanDevice::CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size)
{
	VkCommandBuffer commandBuffer = BeginSingleTimeCommands();
	VkBufferCopy copyRegion = {};
	copyRegion.size = size;
	vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);
	EndSingleTimeCommands(commandBuffer);
}

void VulkanDevice::CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties, VkImage& image, VkDeviceMemory& imageMemory)
{
	VkImageCreateInfo imageInfo = {};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent.width = width;
	imageInfo.extent.height = height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.format = format;
	imageInfo.tiling = tiling;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = usage;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	vkCreateImage(m_device, &imageInfo, nullptr, &image);

	VkMemoryRequirements memRequirements;
	vkGetImageMemoryRequirements(m_device, image, &memRequirements);

	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, properties);

	vkAllocateMemory(m_device, &allocInfo, nullptr, &imageMemory);
	vkBindImageMemory(m_device, image, imageMemory, 0);
}

void VulkanDevice::TransitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout)
{
	VkCommandBuffer commandBuffer = BeginSingleTimeCommands();

	VkImageMemoryBarrier barrier = {};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;

	VkPipelineStageFlags sourceStage;
	VkPipelineStageFlags destinationStage;

	if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	} else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	} else {
		SDL_Log("unsupported layout transition!");
		return;
	}

	vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	EndSingleTimeCommands(commandBuffer);
}

void VulkanDevice::CopyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height)
{
	VkCommandBuffer commandBuffer = BeginSingleTimeCommands();
	VkBufferImageCopy region = {};
	region.bufferOffset = 0;
	region.bufferRowLength = 0;
	region.bufferImageHeight = 0;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.mipLevel = 0;
	region.imageSubresource.baseArrayLayer = 0;
	region.imageSubresource.layerCount = 1;
	region.imageOffset = {0, 0, 0};
	region.imageExtent = { width, height, 1 };

	vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	EndSingleTimeCommands(commandBuffer);
}

bool VulkanDevice::CreateTextureImage()
{
	int texWidth, texHeight, texChannels;
	stbi_uc* pixels = stbi_load("jj.jpeg", &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
	VkDeviceSize imageSize = texWidth * texHeight * 4;

	if (!pixels) {
		SDL_Log("failed to load texture image!");
		return false;
	}

	VkBuffer stagingBuffer;
	VkDeviceMemory stagingBufferMemory;
	CreateBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer, stagingBufferMemory);

	void* data;
	vkMapMemory(m_device, stagingBufferMemory, 0, imageSize, 0, &data);
	memcpy(data, pixels, static_cast<size_t>(imageSize));
	vkUnmapMemory(m_device, stagingBufferMemory);

	stbi_image_free(pixels);

	CreateImage(texWidth, texHeight, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_textureImage, m_textureImageMemory);

	TransitionImageLayout(m_textureImage, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	CopyBufferToImage(stagingBuffer, m_textureImage, static_cast<uint32_t>(texWidth), static_cast<uint32_t>(texHeight));
	TransitionImageLayout(m_textureImage, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	vkDestroyBuffer(m_device, stagingBuffer, nullptr);
	vkFreeMemory(m_device, stagingBufferMemory, nullptr);
	
	return true;
}

VkImageView VulkanDevice::CreateImageView(VkImage image, VkFormat format)
{
	VkImageViewCreateInfo viewInfo = {};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = format;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.baseMipLevel = 0;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.baseArrayLayer = 0;
	viewInfo.subresourceRange.layerCount = 1;

	VkImageView imageView;
	vkCreateImageView(m_device, &viewInfo, nullptr, &imageView);
	return imageView;
}

bool VulkanDevice::CreateTextureImageView()
{
	m_textureImageView = CreateImageView(m_textureImage, VK_FORMAT_R8G8B8A8_SRGB);
	return true;
}

bool VulkanDevice::CreateTextureSampler()
{
	VkSamplerCreateInfo samplerInfo = {};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.anisotropyEnable = VK_FALSE;
	samplerInfo.maxAnisotropy = 1.0f;
	samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
	samplerInfo.unnormalizedCoordinates = VK_FALSE;
	samplerInfo.compareEnable = VK_FALSE;
	samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

	if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_textureSampler) != VK_SUCCESS)
	{
		SDL_Log("vkCreateSampler failed.");
		return false;
	}

	return true;
}


