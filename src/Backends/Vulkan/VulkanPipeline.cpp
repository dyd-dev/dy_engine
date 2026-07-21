#include "VulkanPipeline.h"
#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace dy::Backends
{

VulkanShader::VulkanShader(VkDevice device, const dy::RHI::ShaderDesc& desc)
	: m_device(device)
	, m_stage(desc.stage)
	, m_entryPoint(desc.entryPoint)
{
	VkShaderModuleCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	createInfo.codeSize = desc.binarySize;
	std::vector<uint32_t> words(desc.binarySize / sizeof(uint32_t));
	std::memcpy(words.data(), desc.binary, desc.binarySize);
	createInfo.pCode = words.data();

	if(vkCreateShaderModule(device, &createInfo, nullptr, &m_module) != VK_SUCCESS)
	{
		throw std::runtime_error("failed to create shader module");
	}
}

VulkanShader::~VulkanShader()
{
	if(m_module != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, m_module, nullptr);
}

namespace
{
	VkPipelineRasterizationStateCreateInfo CreateRasterizerState(const dy::RHI::GraphicsPipelineDesc& desc)
	{
		VkPipelineRasterizationStateCreateInfo rasterizer{};
		rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		rasterizer.polygonMode = desc.wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
		rasterizer.cullMode = VK_CULL_MODE_NONE;
		rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		rasterizer.lineWidth = 1.0f;
		rasterizer.depthBiasEnable = desc.depthBias != 0 || desc.depthBiasSlope != 0.0f || desc.depthBiasClamp != 0.0f;
		rasterizer.depthBiasConstantFactor = static_cast<float>(desc.depthBias);
		rasterizer.depthBiasSlopeFactor = desc.depthBiasSlope;
		rasterizer.depthBiasClamp = desc.depthBiasClamp;
		return rasterizer;
	}

	VkPipelineColorBlendAttachmentState CreateAlphaBlendAttachmentState()
	{
		VkPipelineColorBlendAttachmentState colorBlendAttachment{};
		colorBlendAttachment.blendEnable = VK_TRUE;
		colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
		colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
		colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		return colorBlendAttachment;
	}
}

void VulkanPipeline::Initialize(
	const VulkanContext& context,
	VkRenderPass renderPass,
	VkExtent2D,
	VkDescriptorSetLayout descriptorSetLayout,
	const dy::RHI::GraphicsPipelineDesc& desc,
	uint32_t pushConstantSize,
	VkDescriptorSetLayout bindlessDescriptorSetLayout)
{
	const VulkanShader* vertexShader = dynamic_cast<const VulkanShader*>(desc.vertexShader);
	const VulkanShader* fragmentShader = dynamic_cast<const VulkanShader*>(desc.fragmentShader);
	if(vertexShader == nullptr) throw std::runtime_error("graphics pipeline vertex shader is invalid");

	const bool hasFragmentShader = desc.fragmentShader != nullptr;
	if(hasFragmentShader && fragmentShader == nullptr) throw std::runtime_error("graphics pipeline fragment shader is invalid");
	const bool hasColorAttachment = desc.renderTargetFormat != dy::RHI::Format::Unknown;

	VkPipelineShaderStageCreateInfo shaderStages[2]{};
	shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	shaderStages[0].module = vertexShader->GetModule();
	shaderStages[0].pName = vertexShader->GetEntryPoint();
	if (hasFragmentShader) {
		shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		shaderStages[1].module = fragmentShader->GetModule();
		shaderStages[1].pName = fragmentShader->GetEntryPoint();
	}

	std::vector<VkVertexInputBindingDescription> vertexBindings;
	vertexBindings.reserve(desc.inputAssembly.vertexBindingCount);
	for(uint32_t bindingIndex = 0; bindingIndex < desc.inputAssembly.vertexBindingCount; ++bindingIndex)
	{
		const dy::RHI::VertexBindingDesc& binding = desc.inputAssembly.vertexBindings[bindingIndex];
		if(binding.stride == 0) throw std::runtime_error("Vulkan vertex binding stride is zero");
		for(uint32_t previous = 0; previous < bindingIndex; ++previous)
		{
			if(desc.inputAssembly.vertexBindings[previous].slot == binding.slot)
				throw std::runtime_error("duplicate Vulkan vertex binding slot");
		}

		VkVertexInputBindingDescription nativeBinding{};
		nativeBinding.binding = binding.slot;
		nativeBinding.stride = binding.stride;
		nativeBinding.inputRate = binding.inputRate == dy::RHI::VertexInputRate::PerInstance
			? VK_VERTEX_INPUT_RATE_INSTANCE
			: VK_VERTEX_INPUT_RATE_VERTEX;
		vertexBindings.push_back(nativeBinding);
	}

	std::vector<VkVertexInputAttributeDescription> vertexAttributes;
	vertexAttributes.reserve(desc.inputAssembly.vertexAttributeCount);
	for(uint32_t attributeIndex = 0; attributeIndex < desc.inputAssembly.vertexAttributeCount; ++attributeIndex)
	{
		const dy::RHI::VertexAttributeDesc& attribute = desc.inputAssembly.vertexAttributes[attributeIndex];
		VkFormat format = VK_FORMAT_UNDEFINED;
		switch(attribute.format)
		{
		case dy::RHI::Format::R32_FLOAT: format = VK_FORMAT_R32_SFLOAT; break;
		case dy::RHI::Format::R32G32_FLOAT: format = VK_FORMAT_R32G32_SFLOAT; break;
		case dy::RHI::Format::R32G32B32_FLOAT: format = VK_FORMAT_R32G32B32_SFLOAT; break;
		case dy::RHI::Format::R32G32B32A32_FLOAT: format = VK_FORMAT_R32G32B32A32_SFLOAT; break;
		case dy::RHI::Format::R8G8B8A8_UNORM: format = VK_FORMAT_R8G8B8A8_UNORM; break;
		default: throw std::runtime_error("unsupported Vulkan vertex attribute format");
		}

		bool hasBinding = false;
		for(uint32_t bindingIndex = 0; bindingIndex < desc.inputAssembly.vertexBindingCount; ++bindingIndex)
		{
			if(desc.inputAssembly.vertexBindings[bindingIndex].slot == attribute.binding)
			{
				hasBinding = true;
				break;
			}
		}
		if(!hasBinding) throw std::runtime_error("Vulkan vertex attribute references a missing binding");

		VkVertexInputAttributeDescription nativeAttribute{};
		nativeAttribute.location = attribute.location;
		nativeAttribute.binding = attribute.binding;
		nativeAttribute.format = format;
		nativeAttribute.offset = attribute.offset;
		vertexAttributes.push_back(nativeAttribute);
	}

	VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
	vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInputInfo.vertexBindingDescriptionCount = static_cast<uint32_t>(vertexBindings.size());
	vertexInputInfo.pVertexBindingDescriptions = vertexBindings.data();
	vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexAttributes.size());
	vertexInputInfo.pVertexAttributeDescriptions = vertexAttributes.data();

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	switch(desc.inputAssembly.topology)
	{
	case dy::RHI::PrimitiveTopology::PointList: inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
	case dy::RHI::PrimitiveTopology::LineList: inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
	case dy::RHI::PrimitiveTopology::LineStrip: inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
	case dy::RHI::PrimitiveTopology::TriangleStrip: inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
	case dy::RHI::PrimitiveTopology::TriangleList: inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
	}

	VkPipelineViewportStateCreateInfo viewportState{};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.pViewports = nullptr;
	viewportState.scissorCount = 1;
	viewportState.pScissors = nullptr;

	VkPipelineRasterizationStateCreateInfo rasterizer = CreateRasterizerState(desc);

	VkPipelineMultisampleStateCreateInfo multisampling{};
	multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineColorBlendAttachmentState colorBlendAttachment = CreateAlphaBlendAttachmentState();
	VkPipelineColorBlendStateCreateInfo colorBlending{};
	colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlending.attachmentCount = hasColorAttachment ? 1u : 0u;
	colorBlending.pAttachments = hasColorAttachment ? &colorBlendAttachment : nullptr;

	const VkDynamicState dynamicStates[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR
	};
	VkPipelineDynamicStateCreateInfo dynamicState{};
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = static_cast<uint32_t>(sizeof(dynamicStates) / sizeof(dynamicStates[0]));
	dynamicState.pDynamicStates = dynamicStates;

	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = desc.depthEnable ? VK_TRUE : VK_FALSE;
	depthStencil.depthWriteEnable = desc.depthEnable ? VK_TRUE : VK_FALSE;
	depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	depthStencil.depthBoundsTestEnable = VK_FALSE;
	depthStencil.stencilTestEnable = VK_FALSE;

	VkPushConstantRange pushConstantRange{};
	pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	pushConstantRange.offset = 0;
	pushConstantRange.size = pushConstantSize;

	std::array<VkDescriptorSetLayout, 2> descriptorSetLayouts = {
		descriptorSetLayout,
		bindlessDescriptorSetLayout
	};
	VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = desc.enableBindlessTextures && bindlessDescriptorSetLayout != VK_NULL_HANDLE ? 2u : 1u;
	pipelineLayoutInfo.pSetLayouts = descriptorSetLayouts.data();
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

	if (vkCreatePipelineLayout(context.device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
		throw std::runtime_error("failed to create pipeline layout");
	}

	VkGraphicsPipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.stageCount = hasFragmentShader ? 2u : 1u;
	pipelineInfo.pStages = shaderStages;
	pipelineInfo.pVertexInputState = &vertexInputInfo;
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &rasterizer;
	pipelineInfo.pMultisampleState = &multisampling;
	pipelineInfo.pDepthStencilState = &depthStencil;
	pipelineInfo.pColorBlendState = &colorBlending;
	pipelineInfo.pDynamicState = &dynamicState;
	pipelineInfo.layout = m_pipelineLayout;
	pipelineInfo.renderPass = renderPass;
	pipelineInfo.subpass = 0;

	if (vkCreateGraphicsPipelines(context.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_graphicsPipeline) != VK_SUCCESS) {
		vkDestroyPipelineLayout(context.device, m_pipelineLayout, nullptr);
		m_pipelineLayout = VK_NULL_HANDLE;
		throw std::runtime_error("failed to create graphics pipeline");
	}

}

void VulkanPipeline::Cleanup(VkDevice device)
{
	if (m_graphicsPipeline != VK_NULL_HANDLE) {
		vkDestroyPipeline(device, m_graphicsPipeline, nullptr);
		m_graphicsPipeline = VK_NULL_HANDLE;
	}
	if (m_pipelineLayout != VK_NULL_HANDLE) {
		vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
		m_pipelineLayout = VK_NULL_HANDLE;
	}
}

}
