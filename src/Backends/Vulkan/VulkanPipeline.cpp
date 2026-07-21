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

	VkPipelineRasterizationStateCreateInfo rasterizer{};
	rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	switch(desc.rasterization.fillMode)
	{
	case dy::RHI::FillMode::Solid: rasterizer.polygonMode = VK_POLYGON_MODE_FILL; break;
	case dy::RHI::FillMode::Wireframe: rasterizer.polygonMode = VK_POLYGON_MODE_LINE; break;
	default: throw std::runtime_error("invalid Vulkan fill mode");
	}
	switch(desc.rasterization.cullMode)
	{
	case dy::RHI::CullMode::None: rasterizer.cullMode = VK_CULL_MODE_NONE; break;
	case dy::RHI::CullMode::Front: rasterizer.cullMode = VK_CULL_MODE_FRONT_BIT; break;
	case dy::RHI::CullMode::Back: rasterizer.cullMode = VK_CULL_MODE_BACK_BIT; break;
	default: throw std::runtime_error("invalid Vulkan cull mode");
	}
	switch(desc.rasterization.frontFace)
	{
	case dy::RHI::FrontFace::CounterClockwise: rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; break;
	case dy::RHI::FrontFace::Clockwise: rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE; break;
	default: throw std::runtime_error("invalid Vulkan front face");
	}
	rasterizer.lineWidth = 1.0f;
	rasterizer.depthBiasEnable = desc.rasterization.depthBias != 0 ||
		desc.rasterization.depthBiasSlope != 0.0f ||
		desc.rasterization.depthBiasClamp != 0.0f;
	rasterizer.depthBiasConstantFactor = static_cast<float>(desc.rasterization.depthBias);
	rasterizer.depthBiasSlopeFactor = desc.rasterization.depthBiasSlope;
	rasterizer.depthBiasClamp = desc.rasterization.depthBiasClamp;

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
	depthStencil.depthTestEnable = desc.depthStencil.depthTestEnable ? VK_TRUE : VK_FALSE;
	depthStencil.depthWriteEnable = desc.depthStencil.depthWriteEnable ? VK_TRUE : VK_FALSE;
	switch(desc.depthStencil.depthCompareOp)
	{
	case dy::RHI::CompareOp::Never: depthStencil.depthCompareOp = VK_COMPARE_OP_NEVER; break;
	case dy::RHI::CompareOp::Less: depthStencil.depthCompareOp = VK_COMPARE_OP_LESS; break;
	case dy::RHI::CompareOp::Equal: depthStencil.depthCompareOp = VK_COMPARE_OP_EQUAL; break;
	case dy::RHI::CompareOp::LessEqual: depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL; break;
	case dy::RHI::CompareOp::Greater: depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER; break;
	case dy::RHI::CompareOp::NotEqual: depthStencil.depthCompareOp = VK_COMPARE_OP_NOT_EQUAL; break;
	case dy::RHI::CompareOp::GreaterEqual: depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; break;
	case dy::RHI::CompareOp::Always: depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS; break;
	default: throw std::runtime_error("invalid Vulkan depth compare operation");
	}
	depthStencil.depthBoundsTestEnable = VK_FALSE;
	depthStencil.stencilTestEnable = desc.depthStencil.stencilTestEnable ? VK_TRUE : VK_FALSE;
	const dy::RHI::StencilFaceDesc* stencilFaces[] = { &desc.depthStencil.frontFace, &desc.depthStencil.backFace };
	VkStencilOpState* nativeStencilFaces[] = { &depthStencil.front, &depthStencil.back };
	for(uint32_t faceIndex = 0; faceIndex < 2; ++faceIndex)
	{
		const dy::RHI::StencilFaceDesc& source = *stencilFaces[faceIndex];
		VkStencilOpState& target = *nativeStencilFaces[faceIndex];
		switch(source.compareOp)
		{
		case dy::RHI::CompareOp::Never: target.compareOp = VK_COMPARE_OP_NEVER; break;
		case dy::RHI::CompareOp::Less: target.compareOp = VK_COMPARE_OP_LESS; break;
		case dy::RHI::CompareOp::Equal: target.compareOp = VK_COMPARE_OP_EQUAL; break;
		case dy::RHI::CompareOp::LessEqual: target.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL; break;
		case dy::RHI::CompareOp::Greater: target.compareOp = VK_COMPARE_OP_GREATER; break;
		case dy::RHI::CompareOp::NotEqual: target.compareOp = VK_COMPARE_OP_NOT_EQUAL; break;
		case dy::RHI::CompareOp::GreaterEqual: target.compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; break;
		case dy::RHI::CompareOp::Always: target.compareOp = VK_COMPARE_OP_ALWAYS; break;
		default: throw std::runtime_error("invalid Vulkan stencil compare operation");
		}
		const dy::RHI::StencilOp operations[] = { source.failOp, source.depthFailOp, source.passOp };
		VkStencilOp* nativeOperations[] = { &target.failOp, &target.depthFailOp, &target.passOp };
		for(uint32_t operationIndex = 0; operationIndex < 3; ++operationIndex)
		{
			switch(operations[operationIndex])
			{
			case dy::RHI::StencilOp::Keep: *nativeOperations[operationIndex] = VK_STENCIL_OP_KEEP; break;
			case dy::RHI::StencilOp::Zero: *nativeOperations[operationIndex] = VK_STENCIL_OP_ZERO; break;
			case dy::RHI::StencilOp::Replace: *nativeOperations[operationIndex] = VK_STENCIL_OP_REPLACE; break;
			case dy::RHI::StencilOp::IncrementClamp: *nativeOperations[operationIndex] = VK_STENCIL_OP_INCREMENT_AND_CLAMP; break;
			case dy::RHI::StencilOp::DecrementClamp: *nativeOperations[operationIndex] = VK_STENCIL_OP_DECREMENT_AND_CLAMP; break;
			case dy::RHI::StencilOp::Invert: *nativeOperations[operationIndex] = VK_STENCIL_OP_INVERT; break;
			case dy::RHI::StencilOp::IncrementWrap: *nativeOperations[operationIndex] = VK_STENCIL_OP_INCREMENT_AND_WRAP; break;
			case dy::RHI::StencilOp::DecrementWrap: *nativeOperations[operationIndex] = VK_STENCIL_OP_DECREMENT_AND_WRAP; break;
			default: throw std::runtime_error("invalid Vulkan stencil operation");
			}
		}
		target.compareMask = desc.depthStencil.stencilReadMask;
		target.writeMask = desc.depthStencil.stencilWriteMask;
		target.reference = desc.depthStencil.stencilReference;
	}

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
