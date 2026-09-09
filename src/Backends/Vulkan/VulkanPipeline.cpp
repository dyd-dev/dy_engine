#include "VulkanPipeline.h"

#include "VulkanResources.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

namespace dy::Backends
{
	namespace
	{
		VkShaderStageFlags ToShaderStages(dy::RHI::ShaderStageFlags stages)
		{
			VkShaderStageFlags result = 0;
			if ((stages & dy::RHI::ShaderStageFlags::Vertex) != dy::RHI::ShaderStageFlags::None) result |= VK_SHADER_STAGE_VERTEX_BIT;
			if ((stages & dy::RHI::ShaderStageFlags::Fragment) != dy::RHI::ShaderStageFlags::None) result |= VK_SHADER_STAGE_FRAGMENT_BIT;
			return result;
		}

		VkDescriptorType ToDescriptorType(dy::RHI::ResourceBindingType type)
		{
			switch (type)
			{
			case dy::RHI::ResourceBindingType::ConstantBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			case dy::RHI::ResourceBindingType::ReadOnlyStorageBuffer:
			case dy::RHI::ResourceBindingType::ReadWriteStorageBuffer:
				return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			case dy::RHI::ResourceBindingType::SampledTexture: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			case dy::RHI::ResourceBindingType::StorageTexture: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			case dy::RHI::ResourceBindingType::StaticSampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
			default: throw std::runtime_error("Unsupported Vulkan resource binding type");
			}
		}

		VkFilter ToFilter(dy::RHI::SamplerFilter filter)
		{
			switch (filter)
			{
			case dy::RHI::SamplerFilter::Nearest: return VK_FILTER_NEAREST;
			case dy::RHI::SamplerFilter::Linear: return VK_FILTER_LINEAR;
			default: throw std::runtime_error("Vulkan sampler filter is undefined");
			}
		}

		VkSamplerMipmapMode ToMipmapMode(dy::RHI::SamplerFilter filter)
		{
			switch (filter)
			{
			case dy::RHI::SamplerFilter::Nearest: return VK_SAMPLER_MIPMAP_MODE_NEAREST;
			case dy::RHI::SamplerFilter::Linear: return VK_SAMPLER_MIPMAP_MODE_LINEAR;
			default: throw std::runtime_error("Vulkan sampler mip filter is undefined");
			}
		}

		VkSamplerAddressMode ToAddressMode(dy::RHI::SamplerAddressMode mode)
		{
			switch (mode)
			{
			case dy::RHI::SamplerAddressMode::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
			case dy::RHI::SamplerAddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
			case dy::RHI::SamplerAddressMode::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			case dy::RHI::SamplerAddressMode::ClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
			default: throw std::runtime_error("Vulkan sampler address mode is undefined");
			}
		}

		VkBorderColor ToBorderColor(dy::RHI::SamplerBorderColor color)
		{
			switch (color)
			{
			case dy::RHI::SamplerBorderColor::TransparentBlack: return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
			case dy::RHI::SamplerBorderColor::OpaqueBlack: return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
			case dy::RHI::SamplerBorderColor::OpaqueWhite: return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
			default: throw std::runtime_error("Vulkan sampler border color is undefined");
			}
		}

		VkSampler CreateSampler(const VulkanContext& context, const dy::RHI::SamplerDesc& desc)
		{
			const bool usesBorder = desc.addressU == dy::RHI::SamplerAddressMode::ClampToBorder ||
				desc.addressV == dy::RHI::SamplerAddressMode::ClampToBorder ||
				desc.addressW == dy::RHI::SamplerAddressMode::ClampToBorder;
			if (desc.maxAnisotropy == 0 ||
				!std::isfinite(desc.mipLodBias) || !std::isfinite(desc.minLod) || !std::isfinite(desc.maxLod) ||
				desc.minLod > desc.maxLod ||
				(usesBorder && desc.borderColor == dy::RHI::SamplerBorderColor::Undefined))
			{
				throw std::runtime_error("Invalid Vulkan static sampler description");
			}

			VkPhysicalDeviceProperties properties{};
			vkGetPhysicalDeviceProperties(context.physicalDevice, &properties);
			VkPhysicalDeviceFeatures features{};
			vkGetPhysicalDeviceFeatures(context.physicalDevice, &features);
			if (desc.maxAnisotropy > 1 &&
				(!features.samplerAnisotropy || static_cast<float>(desc.maxAnisotropy) > properties.limits.maxSamplerAnisotropy))
			{
				throw std::runtime_error("Requested Vulkan sampler anisotropy is unsupported");
			}

			VkSamplerCreateInfo info{};
			info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
			info.magFilter = ToFilter(desc.magFilter);
			info.minFilter = ToFilter(desc.minFilter);
			info.mipmapMode = ToMipmapMode(desc.mipFilter);
			info.addressModeU = ToAddressMode(desc.addressU);
			info.addressModeV = ToAddressMode(desc.addressV);
			info.addressModeW = ToAddressMode(desc.addressW);
			info.anisotropyEnable = desc.maxAnisotropy > 1 ? VK_TRUE : VK_FALSE;
			info.maxAnisotropy = static_cast<float>(desc.maxAnisotropy);
			info.compareEnable = VK_FALSE;
			info.mipLodBias = desc.mipLodBias;
			info.minLod = desc.minLod;
			info.maxLod = desc.maxLod;
			info.borderColor = usesBorder ? ToBorderColor(desc.borderColor) : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;

			VkSampler sampler = VK_NULL_HANDLE;
			if (vkCreateSampler(context.device, &info, nullptr, &sampler) != VK_SUCCESS)
			{
				throw std::runtime_error("Failed to create Vulkan static sampler");
			}
			return sampler;
		}

		VkPrimitiveTopology ToTopology(dy::RHI::PrimitiveTopology topology)
		{
			switch (topology)
			{
			case dy::RHI::PrimitiveTopology::PointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
			case dy::RHI::PrimitiveTopology::LineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
			case dy::RHI::PrimitiveTopology::TriangleList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			case dy::RHI::PrimitiveTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
			default: throw std::runtime_error("Vulkan primitive topology is undefined");
			}
		}

		VkPolygonMode ToPolygonMode(dy::RHI::FillMode mode)
		{
			switch (mode)
			{
			case dy::RHI::FillMode::Solid: return VK_POLYGON_MODE_FILL;
			case dy::RHI::FillMode::Wireframe: return VK_POLYGON_MODE_LINE;
			default: throw std::runtime_error("Vulkan fill mode is undefined");
			}
		}

		VkCullModeFlags ToCullMode(dy::RHI::CullMode mode)
		{
			switch (mode)
			{
			case dy::RHI::CullMode::None: return VK_CULL_MODE_NONE;
			case dy::RHI::CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
			case dy::RHI::CullMode::Back: return VK_CULL_MODE_BACK_BIT;
			default: throw std::runtime_error("Vulkan cull mode is undefined");
			}
		}

		VkFrontFace ToFrontFace(dy::RHI::FrontFace face)
		{
			switch (face)
			{
			case dy::RHI::FrontFace::CounterClockwise: return VK_FRONT_FACE_CLOCKWISE;
			case dy::RHI::FrontFace::Clockwise: return VK_FRONT_FACE_COUNTER_CLOCKWISE;
			default: throw std::runtime_error("Vulkan front face is undefined");
			}
		}

		VkCompareOp ToCompareOp(dy::RHI::CompareOp op)
		{
			switch (op)
			{
			case dy::RHI::CompareOp::Never: return VK_COMPARE_OP_NEVER;
			case dy::RHI::CompareOp::Less: return VK_COMPARE_OP_LESS;
			case dy::RHI::CompareOp::Equal: return VK_COMPARE_OP_EQUAL;
			case dy::RHI::CompareOp::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
			case dy::RHI::CompareOp::Greater: return VK_COMPARE_OP_GREATER;
			case dy::RHI::CompareOp::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
			case dy::RHI::CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
			case dy::RHI::CompareOp::Always: return VK_COMPARE_OP_ALWAYS;
			default: throw std::runtime_error("Vulkan compare operation is undefined");
			}
		}

		VkStencilOp ToStencilOp(dy::RHI::StencilOp op)
		{
			switch (op)
			{
			case dy::RHI::StencilOp::Keep: return VK_STENCIL_OP_KEEP;
			case dy::RHI::StencilOp::Zero: return VK_STENCIL_OP_ZERO;
			case dy::RHI::StencilOp::Replace: return VK_STENCIL_OP_REPLACE;
			case dy::RHI::StencilOp::IncrementClamp: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
			case dy::RHI::StencilOp::DecrementClamp: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
			case dy::RHI::StencilOp::Invert: return VK_STENCIL_OP_INVERT;
			case dy::RHI::StencilOp::IncrementWrap: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
			case dy::RHI::StencilOp::DecrementWrap: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
			default: throw std::runtime_error("Vulkan stencil operation is undefined");
			}
		}

		VkStencilOpState ToStencilState(const dy::RHI::StencilFaceState& state, uint8_t readMask, uint8_t writeMask)
		{
			VkStencilOpState result{};
			result.failOp = ToStencilOp(state.failOp);
			result.passOp = ToStencilOp(state.passOp);
			result.depthFailOp = ToStencilOp(state.depthFailOp);
			result.compareOp = ToCompareOp(state.compareOp);
			result.compareMask = readMask;
			result.writeMask = writeMask;
			return result;
		}

		VkBlendFactor ToBlendFactor(dy::RHI::BlendFactor factor)
		{
			switch (factor)
			{
			case dy::RHI::BlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
			case dy::RHI::BlendFactor::One: return VK_BLEND_FACTOR_ONE;
			case dy::RHI::BlendFactor::SourceColor: return VK_BLEND_FACTOR_SRC_COLOR;
			case dy::RHI::BlendFactor::OneMinusSourceColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
			case dy::RHI::BlendFactor::DestinationColor: return VK_BLEND_FACTOR_DST_COLOR;
			case dy::RHI::BlendFactor::OneMinusDestinationColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
			case dy::RHI::BlendFactor::SourceAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
			case dy::RHI::BlendFactor::OneMinusSourceAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			case dy::RHI::BlendFactor::DestinationAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
			case dy::RHI::BlendFactor::OneMinusDestinationAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
			default: throw std::runtime_error("Vulkan blend factor is undefined");
			}
		}

		VkBlendOp ToBlendOp(dy::RHI::BlendOp op)
		{
			switch (op)
			{
			case dy::RHI::BlendOp::Add: return VK_BLEND_OP_ADD;
			case dy::RHI::BlendOp::Subtract: return VK_BLEND_OP_SUBTRACT;
			case dy::RHI::BlendOp::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
			case dy::RHI::BlendOp::Min: return VK_BLEND_OP_MIN;
			case dy::RHI::BlendOp::Max: return VK_BLEND_OP_MAX;
			default: throw std::runtime_error("Vulkan blend operation is undefined");
			}
		}

		VkColorComponentFlags ToColorWriteMask(dy::RHI::ColorWriteMask mask)
		{
			const uint8_t value = static_cast<uint8_t>(mask);
			VkColorComponentFlags result = 0;
			if ((value & static_cast<uint8_t>(dy::RHI::ColorWriteMask::Red)) != 0) result |= VK_COLOR_COMPONENT_R_BIT;
			if ((value & static_cast<uint8_t>(dy::RHI::ColorWriteMask::Green)) != 0) result |= VK_COLOR_COMPONENT_G_BIT;
			if ((value & static_cast<uint8_t>(dy::RHI::ColorWriteMask::Blue)) != 0) result |= VK_COLOR_COMPONENT_B_BIT;
			if ((value & static_cast<uint8_t>(dy::RHI::ColorWriteMask::Alpha)) != 0) result |= VK_COLOR_COMPONENT_A_BIT;
			return result;
		}

		VkVertexInputRate ToInputRate(dy::RHI::VertexStepMode mode)
		{
			switch (mode)
			{
			case dy::RHI::VertexStepMode::Vertex: return VK_VERTEX_INPUT_RATE_VERTEX;
			case dy::RHI::VertexStepMode::Instance: return VK_VERTEX_INPUT_RATE_INSTANCE;
			default: throw std::runtime_error("Vulkan vertex step mode is undefined");
			}
		}
	}

	VulkanShader::VulkanShader(const VulkanContext& context, const dy::RHI::ShaderDesc& desc)
		: dy::RHI::Shader(desc)
		, m_device(context.device)
	{
		if (desc.stage == dy::RHI::ShaderStage::Unknown || desc.entryPoint == nullptr || desc.entryPoint[0] == '\0' ||
			desc.binary == nullptr || desc.binarySize == 0 || (desc.binarySize % sizeof(uint32_t)) != 0)
		{
			throw std::runtime_error("Invalid Vulkan shader description");
		}

		VkShaderModuleCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		info.codeSize = desc.binarySize;
		// ShaderDesc is a byte view on every backend; do not impose Vulkan's
		// uint32_t alignment requirement on callers or embedded byte arrays.
		std::vector<uint32_t> words(desc.binarySize / sizeof(uint32_t));
		std::memcpy(words.data(), desc.binary, desc.binarySize);
		info.pCode = words.data();
		if (vkCreateShaderModule(m_device, &info, nullptr, &m_module) != VK_SUCCESS)
		{
			throw std::runtime_error("Failed to create Vulkan shader module");
		}
	}

	VulkanShader::~VulkanShader()
	{
		if (m_module != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, m_module, nullptr);
	}

	VulkanPipeline::VulkanPipeline(const VulkanContext& context, const dy::RHI::GraphicsPipelineDesc& desc)
		: dy::RHI::Pipeline(desc.layout)
		, m_device(context.device)
		, m_usesStencil(desc.depthStencil.stencilEnabled)
		, m_requiresDepthWrite(desc.depthStencil.depthWriteEnabled || desc.depthStencil.stencilEnabled)
	{
		try
		{
			CreateDescriptorLayouts(context, desc.layout);
			CreatePipelineLayout(desc.layout);
			CreatePipeline(desc);
		}
		catch (...)
		{
			Cleanup();
			throw;
		}
	}

	VulkanPipeline::~VulkanPipeline()
	{
		Cleanup();
	}

	void VulkanPipeline::CreateDescriptorLayouts(const VulkanContext& context, const dy::RHI::PipelineLayoutDesc& desc)
	{
		if (desc.bindingCount > 0 && desc.bindings == nullptr) throw std::runtime_error("Vulkan pipeline bindings are missing");
		VkPhysicalDeviceProperties properties{};
		vkGetPhysicalDeviceProperties(context.physicalDevice, &properties);
		if (desc.inlineConstantSize > properties.limits.maxPushConstantsSize ||
			(desc.inlineConstantSize % sizeof(uint32_t)) != 0)
		{
			throw std::runtime_error("Vulkan pipeline layout exceeds native limits");
		}
		if (desc.bindingCount == 0) return;

		std::vector<VkDescriptorSetLayoutBinding> bindings;
		std::vector<std::vector<VkSampler>> immutableSamplers;
		std::set<uint32_t> usedBindings;
		for (uint32_t i = 0; i < desc.bindingCount; ++i)
		{
			const dy::RHI::ResourceBindingLayout& source = desc.bindings[i];
			if (source.count == 0 || ToShaderStages(source.stages) == 0 || !usedBindings.insert(source.binding).second)
			{
				throw std::runtime_error("Invalid or duplicate Vulkan descriptor binding");
			}

			VkDescriptorSetLayoutBinding binding{};
			binding.binding = source.binding;
			binding.descriptorType = ToDescriptorType(source.type);
			binding.descriptorCount = source.count;
			binding.stageFlags = ToShaderStages(source.stages);
			bindings.push_back(binding);
			immutableSamplers.emplace_back();
			if (source.type == dy::RHI::ResourceBindingType::StaticSampler)
			{
				const VkSampler sampler = CreateSampler(context, source.staticSampler);
				m_staticSamplers.push_back(sampler);
				immutableSamplers.back().assign(source.count, sampler);
			}
		}

		for (size_t i = 0; i < bindings.size(); ++i)
		{
			if (!immutableSamplers[i].empty()) bindings[i].pImmutableSamplers = immutableSamplers[i].data();
		}

		VkDescriptorSetLayoutCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		info.bindingCount = static_cast<uint32_t>(bindings.size());
		info.pBindings = bindings.data();
		if (vkCreateDescriptorSetLayout(m_device, &info, nullptr, &m_setLayout) != VK_SUCCESS)
		{
			throw std::runtime_error("Failed to create Vulkan descriptor set layout");
		}
	}

	void VulkanPipeline::CreatePipelineLayout(const dy::RHI::PipelineLayoutDesc& desc)
	{
		VkPushConstantRange range{};
		if (desc.inlineConstantSize > 0)
		{
			range.stageFlags = ToShaderStages(desc.inlineConstantStages);
			if (range.stageFlags == 0) throw std::runtime_error("Vulkan inline constant stages are undefined");
			range.offset = 0;
			range.size = desc.inlineConstantSize;
		}

		VkPipelineLayoutCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		info.setLayoutCount = m_setLayout == VK_NULL_HANDLE ? 0u : 1u;
		info.pSetLayouts = m_setLayout == VK_NULL_HANDLE ? nullptr : &m_setLayout;
		info.pushConstantRangeCount = desc.inlineConstantSize > 0 ? 1u : 0u;
		info.pPushConstantRanges = desc.inlineConstantSize > 0 ? &range : nullptr;
		if (vkCreatePipelineLayout(m_device, &info, nullptr, &m_pipelineLayout) != VK_SUCCESS)
		{
			throw std::runtime_error("Failed to create Vulkan pipeline layout");
		}
	}

	void VulkanPipeline::CreatePipeline(const dy::RHI::GraphicsPipelineDesc& desc)
	{
		if ((desc.vertexBufferCount > 0 && desc.vertexBuffers == nullptr) ||
			(desc.vertexAttributeCount > 0 && desc.vertexAttributes == nullptr) ||
			(desc.colorAttachmentCount > 0 && desc.colorAttachments == nullptr) ||
			!std::isfinite(desc.raster.depthBiasConstant) ||
			!std::isfinite(desc.raster.depthBiasSlope) ||
			!std::isfinite(desc.raster.depthBiasClamp))
		{
			throw std::runtime_error("Vulkan graphics pipeline arrays are missing");
		}
		const VulkanShader* vertexShader = dynamic_cast<const VulkanShader*>(desc.vertexShader);
		const VulkanShader* fragmentShader = dynamic_cast<const VulkanShader*>(desc.fragmentShader);
		if (vertexShader == nullptr || vertexShader->GetStage() != dy::RHI::ShaderStage::Vertex ||
			(desc.fragmentShader != nullptr && (fragmentShader == nullptr || fragmentShader->GetStage() != dy::RHI::ShaderStage::Fragment)))
		{
			throw std::runtime_error("Invalid Vulkan graphics shaders");
		}
		m_vertexBuffers.clear();
		if (desc.vertexBufferCount != 0)
		{
			m_vertexBuffers.assign(desc.vertexBuffers, desc.vertexBuffers + desc.vertexBufferCount);
		}
		m_colorFormats.clear();
		m_colorFormats.reserve(desc.colorAttachmentCount);
		for (uint32_t i = 0; i < desc.colorAttachmentCount; ++i)
		{
			m_colorFormats.push_back(desc.colorAttachments[i].format);
		}
		m_depthFormat = desc.depthStencil.format;

		std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages{};
		shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		shaderStages[0].module = vertexShader->GetModule();
		shaderStages[0].pName = vertexShader->GetEntryPoint();
		uint32_t shaderStageCount = 1;
		if (fragmentShader != nullptr)
		{
			shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
			shaderStages[1].module = fragmentShader->GetModule();
			shaderStages[1].pName = fragmentShader->GetEntryPoint();
			shaderStageCount = 2;
		}

		std::vector<VkVertexInputBindingDescription> vertexBindings;
		std::set<uint32_t> usedVertexBindings;
		vertexBindings.reserve(desc.vertexBufferCount);
		for (uint32_t i = 0; i < desc.vertexBufferCount; ++i)
		{
			const dy::RHI::VertexBufferLayout& source = desc.vertexBuffers[i];
			if (source.stride == 0 || !usedVertexBindings.insert(source.binding).second)
			{
				throw std::runtime_error("Invalid Vulkan vertex buffer layout");
			}
			VkVertexInputBindingDescription binding{};
			binding.binding = source.binding;
			binding.stride = source.stride;
			binding.inputRate = ToInputRate(source.stepMode);
			vertexBindings.push_back(binding);
		}

		std::vector<VkVertexInputAttributeDescription> vertexAttributes;
		std::set<uint32_t> usedLocations;
		vertexAttributes.reserve(desc.vertexAttributeCount);
		for (uint32_t i = 0; i < desc.vertexAttributeCount; ++i)
		{
			const dy::RHI::VertexAttribute& source = desc.vertexAttributes[i];
			const VkFormat format = ToVulkanFormat(source.format);
			if (format == VK_FORMAT_UNDEFINED || usedVertexBindings.count(source.binding) == 0 || !usedLocations.insert(source.location).second)
			{
				throw std::runtime_error("Invalid Vulkan vertex attribute");
			}
			VkVertexInputAttributeDescription attribute{};
			attribute.location = source.location;
			attribute.binding = source.binding;
			attribute.format = format;
			attribute.offset = source.offset;
			vertexAttributes.push_back(attribute);
		}

		VkPipelineVertexInputStateCreateInfo vertexInput{};
		vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(vertexBindings.size());
		vertexInput.pVertexBindingDescriptions = vertexBindings.empty() ? nullptr : vertexBindings.data();
		vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexAttributes.size());
		vertexInput.pVertexAttributeDescriptions = vertexAttributes.empty() ? nullptr : vertexAttributes.data();

		VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
		inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		inputAssembly.topology = ToTopology(desc.topology);

		VkPipelineViewportStateCreateInfo viewportState{};
		viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewportState.viewportCount = 1;
		viewportState.scissorCount = 1;

		VkPipelineRasterizationStateCreateInfo raster{};
		raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		raster.polygonMode = ToPolygonMode(desc.raster.fillMode);
		raster.cullMode = ToCullMode(desc.raster.cullMode);
		raster.frontFace = ToFrontFace(desc.raster.frontFace);
		raster.depthBiasEnable = (desc.raster.depthBiasConstant != 0.0f || desc.raster.depthBiasSlope != 0.0f || desc.raster.depthBiasClamp != 0.0f) ? VK_TRUE : VK_FALSE;
		raster.depthBiasConstantFactor = desc.raster.depthBiasConstant;
		raster.depthBiasSlopeFactor = desc.raster.depthBiasSlope;
		raster.depthBiasClamp = desc.raster.depthBiasClamp;
		raster.lineWidth = 1.0f;

		VkPipelineMultisampleStateCreateInfo multisample{};
		multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

		VkPipelineDepthStencilStateCreateInfo depthStencil{};
		depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
		depthStencil.depthTestEnable = desc.depthStencil.depthTestEnabled ? VK_TRUE : VK_FALSE;
		depthStencil.depthWriteEnable = desc.depthStencil.depthWriteEnabled ? VK_TRUE : VK_FALSE;
		if (desc.depthStencil.depthTestEnabled || desc.depthStencil.depthWriteEnabled)
		{
			depthStencil.depthCompareOp = ToCompareOp(desc.depthStencil.depthCompareOp);
		}
		depthStencil.stencilTestEnable = desc.depthStencil.stencilEnabled ? VK_TRUE : VK_FALSE;
		if (desc.depthStencil.stencilEnabled)
		{
			depthStencil.front = ToStencilState(desc.depthStencil.front, desc.depthStencil.stencilReadMask, desc.depthStencil.stencilWriteMask);
			depthStencil.back = ToStencilState(desc.depthStencil.back, desc.depthStencil.stencilReadMask, desc.depthStencil.stencilWriteMask);
		}

		std::vector<VkPipelineColorBlendAttachmentState> blendAttachments;
		std::vector<VkFormat> colorFormats;
		blendAttachments.reserve(desc.colorAttachmentCount);
		colorFormats.reserve(desc.colorAttachmentCount);
		for (uint32_t i = 0; i < desc.colorAttachmentCount; ++i)
		{
			const dy::RHI::ColorAttachmentDesc& source = desc.colorAttachments[i];
			const VkFormat format = ToVulkanFormat(source.format);
			if (format == VK_FORMAT_UNDEFINED) throw std::runtime_error("Invalid Vulkan color attachment format");
			colorFormats.push_back(format);

			VkPipelineColorBlendAttachmentState attachment{};
			attachment.blendEnable = source.blend.enabled ? VK_TRUE : VK_FALSE;
			if (source.blend.enabled)
			{
				attachment.srcColorBlendFactor = ToBlendFactor(source.blend.sourceColor);
				attachment.dstColorBlendFactor = ToBlendFactor(source.blend.destinationColor);
				attachment.colorBlendOp = ToBlendOp(source.blend.colorOp);
				attachment.srcAlphaBlendFactor = ToBlendFactor(source.blend.sourceAlpha);
				attachment.dstAlphaBlendFactor = ToBlendFactor(source.blend.destinationAlpha);
				attachment.alphaBlendOp = ToBlendOp(source.blend.alphaOp);
			}
			attachment.colorWriteMask = ToColorWriteMask(source.writeMask);
			blendAttachments.push_back(attachment);
		}

		VkPipelineColorBlendStateCreateInfo colorBlend{};
		colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		colorBlend.attachmentCount = static_cast<uint32_t>(blendAttachments.size());
		colorBlend.pAttachments = blendAttachments.empty() ? nullptr : blendAttachments.data();

		std::vector<VkDynamicState> dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		if (desc.depthStencil.stencilEnabled) dynamicStates.push_back(VK_DYNAMIC_STATE_STENCIL_REFERENCE);
		VkPipelineDynamicStateCreateInfo dynamicState{};
		dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
		dynamicState.pDynamicStates = dynamicStates.data();

		const VkFormat depthFormat = ToVulkanFormat(desc.depthStencil.format);
		if ((desc.depthStencil.depthTestEnabled || desc.depthStencil.depthWriteEnabled || desc.depthStencil.stencilEnabled) && depthFormat == VK_FORMAT_UNDEFINED)
		{
			throw std::runtime_error("Vulkan depth/stencil format is undefined");
		}
		VkPipelineRenderingCreateInfo rendering{};
		rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
		rendering.colorAttachmentCount = static_cast<uint32_t>(colorFormats.size());
		rendering.pColorAttachmentFormats = colorFormats.empty() ? nullptr : colorFormats.data();
		rendering.depthAttachmentFormat = depthFormat;
		rendering.stencilAttachmentFormat = desc.depthStencil.format == dy::RHI::Format::D24_UNORM_S8_UINT ? depthFormat : VK_FORMAT_UNDEFINED;

		VkGraphicsPipelineCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		info.pNext = &rendering;
		info.stageCount = shaderStageCount;
		info.pStages = shaderStages.data();
		info.pVertexInputState = &vertexInput;
		info.pInputAssemblyState = &inputAssembly;
		info.pViewportState = &viewportState;
		info.pRasterizationState = &raster;
		info.pMultisampleState = &multisample;
		info.pDepthStencilState = &depthStencil;
		info.pColorBlendState = &colorBlend;
		info.pDynamicState = &dynamicState;
		info.layout = m_pipelineLayout;
		info.renderPass = VK_NULL_HANDLE;
		if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &m_pipeline) != VK_SUCCESS)
		{
			throw std::runtime_error("Failed to create Vulkan graphics pipeline");
		}
	}

	void VulkanPipeline::Cleanup()
	{
		if (m_pipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_pipeline, nullptr);
		if (m_pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
		if (m_setLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
		for (VkSampler sampler : m_staticSamplers) vkDestroySampler(m_device, sampler, nullptr);
		m_pipeline = VK_NULL_HANDLE;
		m_pipelineLayout = VK_NULL_HANDLE;
		m_setLayout = VK_NULL_HANDLE;
		m_staticSamplers.clear();
	}

	VulkanResourceSet::VulkanResourceSet(const VulkanContext& context, const dy::RHI::ResourceSetDesc& desc)
		: dy::RHI::ResourceSet(desc)
		, m_device(context.device)
		, m_pipeline(dynamic_cast<VulkanPipeline*>(desc.pipeline))
	{
		if (m_pipeline == nullptr) throw std::runtime_error("Invalid Vulkan resource-set pipeline");
		if (desc.bindingCount > 0 && desc.bindings == nullptr) throw std::runtime_error("Vulkan resource bindings are missing");
		const dy::RHI::PipelineLayoutDesc& layout = m_pipeline->GetLayout();
		const VkDescriptorSetLayout setLayout = m_pipeline->GetSetLayout();
		if (setLayout == VK_NULL_HANDLE)
		{
			if (desc.bindingCount != 0) throw std::runtime_error("Vulkan pipeline has no descriptor bindings");
			return;
		}

		std::map<VkDescriptorType, uint32_t> descriptorCounts;
		for (uint32_t i = 0; i < layout.bindingCount; ++i)
		{
			descriptorCounts[ToDescriptorType(layout.bindings[i].type)] += layout.bindings[i].count;
		}
		std::vector<VkDescriptorPoolSize> poolSizes;
		poolSizes.reserve(descriptorCounts.size());
		for (const auto& entry : descriptorCounts)
		{
			VkDescriptorPoolSize size{};
			size.type = entry.first;
			size.descriptorCount = entry.second;
			poolSizes.push_back(size);
		}

		VkDescriptorPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolInfo.maxSets = 1;
		poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
		poolInfo.pPoolSizes = poolSizes.empty() ? nullptr : poolSizes.data();
		struct DescriptorPoolGuard
		{
			VkDevice device = VK_NULL_HANDLE;
			VkDescriptorPool pool = VK_NULL_HANDLE;
			~DescriptorPoolGuard()
			{
				if (pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, pool, nullptr);
			}
		};
		DescriptorPoolGuard guard{ m_device, VK_NULL_HANDLE };
		if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &guard.pool) != VK_SUCCESS)
		{
			throw std::runtime_error("Failed to create Vulkan resource-set pool");
		}
		m_pool = guard.pool;

		VkDescriptorSetAllocateInfo allocation{};
		allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocation.descriptorPool = m_pool;
		allocation.descriptorSetCount = 1;
		allocation.pSetLayouts = &setLayout;
		if (vkAllocateDescriptorSets(m_device, &allocation, &m_set) != VK_SUCCESS)
		{
			throw std::runtime_error("Failed to allocate Vulkan resource sets");
		}

		std::vector<VkDescriptorBufferInfo> bufferInfos;
		std::vector<VkDescriptorImageInfo> imageInfos;
		std::vector<VkWriteDescriptorSet> writes;
		bufferInfos.reserve(desc.bindingCount);
		imageInfos.reserve(desc.bindingCount);
		writes.reserve(desc.bindingCount);
		std::set<std::pair<uint32_t, uint32_t>> provided;
		for (uint32_t i = 0; i < desc.bindingCount; ++i)
		{
			const dy::RHI::ResourceBinding& source = desc.bindings[i];
			const dy::RHI::ResourceBindingLayout* bindingLayout = nullptr;
			for (uint32_t j = 0; j < layout.bindingCount; ++j)
			{
				const dy::RHI::ResourceBindingLayout& candidate = layout.bindings[j];
				if (candidate.binding == source.binding)
				{
					bindingLayout = &candidate;
					break;
				}
			}
			if (bindingLayout == nullptr || bindingLayout->type == dy::RHI::ResourceBindingType::StaticSampler ||
				source.arrayElement >= bindingLayout->count ||
				!provided.emplace(source.binding, source.arrayElement).second)
			{
				throw std::runtime_error("Invalid Vulkan resource binding");
			}

			VkWriteDescriptorSet write{};
			write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			write.dstSet = m_set;
			write.dstBinding = source.binding;
			write.dstArrayElement = source.arrayElement;
			write.descriptorCount = 1;
			write.descriptorType = ToDescriptorType(bindingLayout->type);
			if (bindingLayout->type == dy::RHI::ResourceBindingType::ConstantBuffer ||
				bindingLayout->type == dy::RHI::ResourceBindingType::ReadOnlyStorageBuffer ||
				bindingLayout->type == dy::RHI::ResourceBindingType::ReadWriteStorageBuffer)
			{
				VulkanBuffer* buffer = dynamic_cast<VulkanBuffer*>(source.buffer);
				const dy::RHI::BufferUsage requiredUsage = bindingLayout->type == dy::RHI::ResourceBindingType::ConstantBuffer
					? dy::RHI::BufferUsage::Constant
					: dy::RHI::BufferUsage::Storage;
				if (buffer == nullptr || source.texture != nullptr || source.size == 0 ||
					source.offset > buffer->GetDesc().size || source.size > buffer->GetDesc().size - source.offset ||
					(buffer->GetDesc().usage & requiredUsage) == dy::RHI::BufferUsage::None ||
					source.subresources.firstMipLevel != 0 || source.subresources.mipLevelCount != 0 ||
					source.subresources.firstArrayLayer != 0 || source.subresources.arrayLayerCount != 0)
				{
					throw std::runtime_error("Invalid Vulkan buffer binding");
				}
				VkDescriptorBufferInfo info{};
				info.buffer = buffer->GetHandle();
				info.offset = source.offset;
				info.range = source.size;
				bufferInfos.push_back(info);
				write.pBufferInfo = &bufferInfos.back();
			}
			else
			{
				VulkanTexture* texture = dynamic_cast<VulkanTexture*>(source.texture);
				const bool storageTexture = bindingLayout->type == dy::RHI::ResourceBindingType::StorageTexture;
				const dy::RHI::TextureUsage requiredUsage = storageTexture
					? dy::RHI::TextureUsage::Storage
					: dy::RHI::TextureUsage::ShaderResource;
				const bool validStorageRange = !storageTexture || source.subresources.mipLevelCount == 1;
				const VkImageView imageView = texture == nullptr || !validStorageRange
					? VK_NULL_HANDLE
					: texture->GetResourceView(source.subresources);
				if (texture == nullptr || source.buffer != nullptr || source.offset != 0 || source.size != 0 ||
					imageView == VK_NULL_HANDLE ||
					(texture->GetDesc().usage & requiredUsage) == dy::RHI::TextureUsage::None)
				{
					throw std::runtime_error("Invalid Vulkan texture binding");
				}
				VkDescriptorImageInfo info{};
				info.imageView = imageView;
				info.imageLayout = storageTexture ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				imageInfos.push_back(info);
				write.pImageInfo = &imageInfos.back();
			}
			writes.push_back(write);
		}

		for (uint32_t i = 0; i < layout.bindingCount; ++i)
		{
			const dy::RHI::ResourceBindingLayout& binding = layout.bindings[i];
			if (binding.type == dy::RHI::ResourceBindingType::StaticSampler) continue;
			for (uint32_t element = 0; element < binding.count; ++element)
			{
				if (provided.count({binding.binding, element}) == 0)
				{
					throw std::runtime_error("Vulkan resource set is incomplete");
				}
			}
		}

		if (!writes.empty()) vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
		guard.pool = VK_NULL_HANDLE;
	}

	VulkanResourceSet::~VulkanResourceSet()
	{
		if (m_pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, m_pool, nullptr);
	}
}
