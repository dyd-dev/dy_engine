#include "VulkanPipeline.h"
#include <stdexcept>
#include <vector>

namespace dy::Backends
{

void VulkanPipeline::Initialize(
	const VulkanContext& context,
	const VkFormat* colorAttachmentFormats,
	uint32_t colorAttachmentCount,
	VkFormat depthAttachmentFormat,
	VkDescriptorSetLayout descriptorSetLayout,
	const dy::RHI::GraphicsPipelineDesc& desc,
	VkDescriptorSetLayout bindlessDescriptorSetLayout)
{
	const bool hasColorAttachment = colorAttachmentCount > 0u;
	const bool hasPixelShader = desc.pixelShader != nullptr && desc.pixelShaderSize > 0u;
	if (desc.vertexShader == nullptr || desc.vertexShaderSize == 0
		|| (hasColorAttachment && (colorAttachmentFormats == nullptr || !hasPixelShader))) {
		throw std::runtime_error("graphics pipeline shader bytecode is missing");
	}

	VkShaderModuleCreateInfo vertexShaderInfo{};
	vertexShaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	vertexShaderInfo.codeSize = desc.vertexShaderSize;
	vertexShaderInfo.pCode = static_cast<const uint32_t*>(desc.vertexShader);
	VkShaderModuleCreateInfo pixelShaderInfo{};
	pixelShaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	pixelShaderInfo.codeSize = desc.pixelShaderSize;
	pixelShaderInfo.pCode = static_cast<const uint32_t*>(desc.pixelShader);

	VkPipelineShaderStageCreateInfo shaderStages[2]{};
	shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shaderStages[0].pNext = &vertexShaderInfo;
	shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	shaderStages[0].pName = "main";
	if(hasPixelShader)
	{
		shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		shaderStages[1].pNext = &pixelShaderInfo;
		shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		shaderStages[1].pName = "main";
	}

	// Geometry is pulled from storage buffers by the Vulkan backend shader path.
	VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
	vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo viewportState{};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;

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

	VkPipelineMultisampleStateCreateInfo multisampling{};
	multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineColorBlendAttachmentState colorBlendAttachment{};
	colorBlendAttachment.blendEnable = desc.blendEnable ? VK_TRUE : VK_FALSE;
	colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
	colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
	colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	const std::vector<VkPipelineColorBlendAttachmentState> colorBlendAttachments(
		colorAttachmentCount,
		colorBlendAttachment);
	VkPipelineColorBlendStateCreateInfo colorBlending{};
	colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlending.attachmentCount = colorAttachmentCount;
	colorBlending.pAttachments = hasColorAttachment ? colorBlendAttachments.data() : nullptr;

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

	VkDescriptorSetLayout descriptorSetLayouts[] = {
		descriptorSetLayout,
		bindlessDescriptorSetLayout
	};
	VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = desc.enableBindlessTextures && bindlessDescriptorSetLayout != VK_NULL_HANDLE ? 2u : 1u;
	pipelineLayoutInfo.pSetLayouts = descriptorSetLayouts;

	if (vkCreatePipelineLayout(context.device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
		throw std::runtime_error("failed to create pipeline layout");
	}

	VkPipelineRenderingCreateInfo renderingInfo{};
	renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
	renderingInfo.colorAttachmentCount = colorAttachmentCount;
	renderingInfo.pColorAttachmentFormats = colorAttachmentFormats;
	renderingInfo.depthAttachmentFormat = depthAttachmentFormat;

	VkGraphicsPipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.pNext = &renderingInfo;
	pipelineInfo.stageCount = hasPixelShader ? 2u : 1u;
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
