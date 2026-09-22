#pragma once

#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/ResourceSet.h"
#include "dyf/RHI/Shader.h"
#include "dyf/RHI/Texture.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace dyf::RHI
{
    [[nodiscard]] inline bool HasUsage(RHI::BufferUsage value, RHI::BufferUsage usage)
    {
    	return (value & usage) != RHI::BufferUsage::None;
    }

    [[nodiscard]] inline bool HasUsage(RHI::TextureUsage value, RHI::TextureUsage usage)
    {
    	return (value & usage) != RHI::TextureUsage::None;
    }

    [[nodiscard]] inline uint32_t FormatSize(RHI::Format format)
    {
    	switch(format)
    	{
    	case RHI::Format::R8G8B8A8_UNORM:
    	case RHI::Format::B8G8R8A8_UNORM:
    	case RHI::Format::R8G8B8A8_UNORM_SRGB:
    	case RHI::Format::B8G8R8A8_UNORM_SRGB:
    	case RHI::Format::D32_FLOAT:
    	case RHI::Format::D24_UNORM_S8_UINT:
    	case RHI::Format::R32_UINT:
    		return 4;
    	case RHI::Format::R16G16B16A16_FLOAT:
    	case RHI::Format::R32G32_FLOAT:
    		return 8;
    	case RHI::Format::R32G32B32_FLOAT:
    		return 12;
    	case RHI::Format::R32G32B32A32_FLOAT:
    		return 16;
    	case RHI::Format::R16_UINT:
    		return 2;
    	default:
    		return 0;
    	}
    }

    [[nodiscard]] inline bool IsDepthFormat(RHI::Format format)
    {
    	return format == RHI::Format::D32_FLOAT ||
    		format == RHI::Format::D24_UNORM_S8_UINT;
    }

    [[nodiscard]] inline bool IsColorFormat(RHI::Format format)
    {
    	return FormatSize(format) != 0 && !IsDepthFormat(format);
    }

    [[nodiscard]] inline uint32_t MaximumMipCount(uint32_t width, uint32_t height)
    {
    	uint32_t dimension = std::max(width, height);
    	uint32_t result = 0;
    	while(dimension != 0)
    	{
    		++result;
    		dimension >>= 1u;
    	}
    	return result;
    }

    [[nodiscard]] inline bool IsBufferStateAllowed(
    	const RHI::BufferDesc& desc,
    	RHI::ResourceState state)
    {
    	switch(state)
    	{
    	case RHI::ResourceState::Undefined:
    	case RHI::ResourceState::Common:
    	case RHI::ResourceState::CopyDestination:
    		return true;
    	case RHI::ResourceState::VertexBuffer:
    		return HasUsage(desc.usage, RHI::BufferUsage::Vertex);
    	case RHI::ResourceState::IndexBuffer:
    		return HasUsage(desc.usage, RHI::BufferUsage::Index);
    	case RHI::ResourceState::ConstantBuffer:
    		return HasUsage(desc.usage, RHI::BufferUsage::Constant);
    	case RHI::ResourceState::ShaderResource:
    	case RHI::ResourceState::UnorderedAccess:
    		return HasUsage(desc.usage, RHI::BufferUsage::Storage);
    	default:
    		return false;
    	}
    }

    [[nodiscard]] inline bool IsTextureStateAllowed(
    	const RHI::TextureDesc& desc,
    	RHI::ResourceState state)
    {
    	switch(state)
    	{
    	case RHI::ResourceState::Undefined:
    	case RHI::ResourceState::Common:
    	case RHI::ResourceState::CopyDestination:
    		return true;
    	case RHI::ResourceState::ShaderResource:
    		return HasUsage(desc.usage, RHI::TextureUsage::ShaderResource);
    	case RHI::ResourceState::UnorderedAccess:
    		return HasUsage(desc.usage, RHI::TextureUsage::Storage);
    	case RHI::ResourceState::RenderTarget:
    		return HasUsage(desc.usage, RHI::TextureUsage::RenderTarget);
    	case RHI::ResourceState::DepthRead:
    	case RHI::ResourceState::DepthWrite:
    		return HasUsage(desc.usage, RHI::TextureUsage::DepthStencil);
    	case RHI::ResourceState::Present:
    		return HasUsage(desc.usage, RHI::TextureUsage::RenderTarget);
    	default:
    		return false;
    	}
    }

    [[nodiscard]] inline bool ResolveSubresources(
    	RHI::Texture* texture,
    	const RHI::TextureSubresourceRange& range,
    	uint32_t& firstMip,
    	uint32_t& mipCount,
    	uint32_t& firstLayer,
    	uint32_t& layerCount)
    {
    	firstMip = range.firstMipLevel;
    	firstLayer = range.firstArrayLayer;
    	if(firstMip >= texture->GetDesc().mipLevels ||
    		firstLayer >= texture->GetDesc().depthOrArraySize)
    	{
    		return false;
    	}
    	mipCount = range.mipLevelCount == 0
    		? texture->GetDesc().mipLevels - firstMip : range.mipLevelCount;
    	layerCount = range.arrayLayerCount == 0
    		? texture->GetDesc().depthOrArraySize - firstLayer : range.arrayLayerCount;
    	return mipCount <= texture->GetDesc().mipLevels - firstMip &&
    		layerCount <= texture->GetDesc().depthOrArraySize - firstLayer;
    }

    [[nodiscard]] inline bool ResolveBindingSubresources(
    	RHI::Texture* texture,
    	const RHI::ResourceBinding& binding,
    	RHI::ResourceBindingType type,
    	uint32_t& firstMip,
    	uint32_t& mipCount,
    	uint32_t& firstLayer,
    	uint32_t& layerCount)
    {
    	if(!ResolveSubresources(
    		texture, binding.subresources,
    		firstMip, mipCount, firstLayer, layerCount))
    	{
    		return false;
    	}
    	return type != RHI::ResourceBindingType::StorageTexture || mipCount == 1;
    }

    [[nodiscard]] inline bool IsValidStageFlags(RHI::ShaderStageFlags stages)
    {
    	constexpr auto all = RHI::ShaderStageFlags::Vertex |
    		RHI::ShaderStageFlags::Fragment | RHI::ShaderStageFlags::Compute |
    		RHI::ShaderStageFlags::Mesh;
    	return stages != RHI::ShaderStageFlags::None &&
    		(stages & all) == stages;
    }

    [[nodiscard]] inline bool IsValidSampler(const RHI::SamplerDesc& desc)
    {
    	const bool usesBorder =
    		desc.addressU == RHI::SamplerAddressMode::ClampToBorder ||
    		desc.addressV == RHI::SamplerAddressMode::ClampToBorder ||
    		desc.addressW == RHI::SamplerAddressMode::ClampToBorder;
    	return (desc.minFilter > RHI::SamplerFilter::Undefined && desc.minFilter <= RHI::SamplerFilter::Linear) &&
    		(desc.magFilter > RHI::SamplerFilter::Undefined && desc.magFilter <= RHI::SamplerFilter::Linear) &&
    		(desc.mipFilter > RHI::SamplerFilter::Undefined && desc.mipFilter <= RHI::SamplerFilter::Linear) &&
    		(desc.addressU > RHI::SamplerAddressMode::Undefined && desc.addressU <= RHI::SamplerAddressMode::ClampToBorder) &&
    		(desc.addressV > RHI::SamplerAddressMode::Undefined && desc.addressV <= RHI::SamplerAddressMode::ClampToBorder) &&
    		(desc.addressW > RHI::SamplerAddressMode::Undefined && desc.addressW <= RHI::SamplerAddressMode::ClampToBorder) &&
    		(!usesBorder || (desc.borderColor > RHI::SamplerBorderColor::Undefined && desc.borderColor <= RHI::SamplerBorderColor::OpaqueWhite)) &&
            (desc.maxAnisotropy <= 1 || (desc.minFilter == SamplerFilter::Linear &&
                desc.magFilter == SamplerFilter::Linear && desc.mipFilter == SamplerFilter::Linear)) &&
    		desc.maxAnisotropy != 0 && std::isfinite(desc.mipLodBias) &&
    		std::isfinite(desc.minLod) && std::isfinite(desc.maxLod) &&
    		desc.minLod <= desc.maxLod;
    }

    // RHI의 셰이더 슬롯 계약이다. API별 레지스터 이름을 호출자가 추론하지 않게 한다.
    inline uint32_t BindingNamespace(ResourceBindingType type)
    {
        switch(type)
        {
        case ResourceBindingType::ConstantBuffer: return 0;
        case ResourceBindingType::ReadOnlyStorageBuffer:
        case ResourceBindingType::SampledTexture: return 1;
        case ResourceBindingType::ReadWriteStorageBuffer:
        case ResourceBindingType::StorageTexture: return 2;
        case ResourceBindingType::StaticSampler: return 3;
        default: return 4;
        }
    }

    inline bool BindingRangesOverlap(uint32_t first, uint32_t count, uint32_t other, uint32_t otherCount)
    {
        return static_cast<uint64_t>(first) < static_cast<uint64_t>(other) + otherCount &&
            static_cast<uint64_t>(other) < static_cast<uint64_t>(first) + count;
    }

    [[nodiscard]] inline bool ValidatePipelineLayout(const RHI::PipelineLayoutDesc& desc)
    {
    	if((desc.bindingCount != 0 && desc.bindings == nullptr) ||
    		(desc.inlineConstantSize != 0 &&
    			(!IsValidStageFlags(desc.inlineConstantStages) ||
    				(desc.inlineConstantSize % sizeof(uint32_t)) != 0)))
    	{
    		return false;
    	}

    	std::set<uint32_t> occupied;
    	for(uint32_t index = 0; index < desc.bindingCount; ++index)
    	{
    		const RHI::ResourceBindingLayout& binding = desc.bindings[index];
    		if((binding.type == RHI::ResourceBindingType::Undefined || binding.type > RHI::ResourceBindingType::StaticSampler) ||
    			binding.count == 0 || !IsValidStageFlags(binding.stages)) return false;
    		if(!occupied.insert(binding.binding).second) return false;
            if(static_cast<uint64_t>(binding.binding) + binding.count > uint64_t{UINT32_MAX} + 1) return false;
            for(uint32_t previous = 0; previous < index; ++previous)
            {
                const auto& other = desc.bindings[previous];
                if(BindingNamespace(binding.type) == BindingNamespace(other.type) &&
                    BindingRangesOverlap(binding.binding,binding.count,other.binding,other.count)) return false;
            }
            if(binding.type == ResourceBindingType::ConstantBuffer && desc.inlineConstantSize &&
                BindingRangesOverlap(binding.binding,binding.count,desc.inlineConstantBinding,1)) return false;
    	}
    	return true;
    }

    [[nodiscard]] inline bool ValidateGraphicsPipelineDesc(
    	const RHI::GraphicsPipelineDesc& desc)
    {
        if ((desc.layout.inlineConstantStages & ShaderStageFlags::Compute) != ShaderStageFlags::None)
            return false;
        if (desc.layout.bindings)
            for (uint32_t i = 0; i < desc.layout.bindingCount; ++i)
                if ((desc.layout.bindings[i].stages & ShaderStageFlags::Compute) != ShaderStageFlags::None)
                    return false;
    	if((desc.vertexBufferCount != 0 && desc.vertexBuffers == nullptr) ||
    		(desc.vertexAttributeCount != 0 && desc.vertexAttributes == nullptr) ||
    		(desc.colorAttachmentCount != 0 && desc.colorAttachments == nullptr))
    	{
    		return false;
    	}

    	auto* vertexShader = dynamic_cast<RHI::Shader*>(desc.vertexShader);
    	if(vertexShader == nullptr || vertexShader->GetStage() != RHI::ShaderStage::Vertex ||
    		(desc.topology == RHI::PrimitiveTopology::Undefined || desc.topology > RHI::PrimitiveTopology::TriangleStrip) ||
    		(desc.raster.fillMode == RHI::FillMode::Undefined || desc.raster.fillMode > RHI::FillMode::Wireframe) ||
    		(desc.raster.cullMode == RHI::CullMode::Undefined || desc.raster.cullMode > RHI::CullMode::Back) ||
    		(desc.raster.frontFace == RHI::FrontFace::Undefined || desc.raster.frontFace > RHI::FrontFace::Clockwise) ||
    		!std::isfinite(desc.raster.depthBiasConstant) ||
    		!std::isfinite(desc.raster.depthBiasSlope) ||
    		!std::isfinite(desc.raster.depthBiasClamp))
    	{
    		return false;
    	}

    	if(desc.fragmentShader != nullptr)
    	{
    		auto* fragmentShader = dynamic_cast<RHI::Shader*>(desc.fragmentShader);
    		if(fragmentShader == nullptr ||
    			fragmentShader->GetStage() != RHI::ShaderStage::Fragment)
    		{
    			return false;
    		}
    	}
    	if(desc.colorAttachmentCount != 0 && desc.fragmentShader == nullptr)
    		return false;

    	std::set<uint32_t> vertexBindings;
    	for(uint32_t index = 0; index < desc.vertexBufferCount; ++index)
    	{
    		const RHI::VertexBufferLayout& layout = desc.vertexBuffers[index];
    		if(layout.stride == 0 || (layout.stepMode == RHI::VertexStepMode::Undefined || layout.stepMode > RHI::VertexStepMode::Instance) ||
    			!vertexBindings.insert(layout.binding).second)
    		{
    			return false;
    		}
    	}

    	std::set<uint32_t> locations;
    	for(uint32_t index = 0; index < desc.vertexAttributeCount; ++index)
    	{
    		const RHI::VertexAttribute& attribute = desc.vertexAttributes[index];
    		const RHI::VertexBufferLayout* layout = nullptr;
    		for(uint32_t layoutIndex = 0; layoutIndex < desc.vertexBufferCount; ++layoutIndex)
    		{
    			if(desc.vertexBuffers[layoutIndex].binding == attribute.binding)
    			{
    				layout = &desc.vertexBuffers[layoutIndex];
    				break;
    			}
    		}
    		const uint32_t size = FormatSize(attribute.format);
    		if(layout == nullptr || size == 0 || IsDepthFormat(attribute.format) ||
    			attribute.offset > layout->stride || size > layout->stride - attribute.offset ||
    			!locations.insert(attribute.location).second)
    		{
    			return false;
    		}
    	}

    	if(desc.depthStencil.format != RHI::Format::Unknown)
    	{
    		if(!IsDepthFormat(desc.depthStencil.format)) return false;
    		if(desc.depthStencil.depthTestEnabled &&
    			(desc.depthStencil.depthCompareOp == RHI::CompareOp::Undefined || desc.depthStencil.depthCompareOp > RHI::CompareOp::Always))
    		{
    			return false;
    		}
    		if(desc.depthStencil.stencilEnabled)
    		{
    			const auto validFace = [](const RHI::StencilFaceState& face)
    			{
    				return (face.failOp > RHI::StencilOp::Undefined && face.failOp <= RHI::StencilOp::DecrementWrap) &&
    					(face.depthFailOp > RHI::StencilOp::Undefined && face.depthFailOp <= RHI::StencilOp::DecrementWrap) &&
    					(face.passOp > RHI::StencilOp::Undefined && face.passOp <= RHI::StencilOp::DecrementWrap) &&
    					(face.compareOp > RHI::CompareOp::Undefined && face.compareOp <= RHI::CompareOp::Always);
    			};
    			if(desc.depthStencil.format != RHI::Format::D24_UNORM_S8_UINT ||
    				!validFace(desc.depthStencil.front) ||
    				!validFace(desc.depthStencil.back))
    			{
    				return false;
    			}
    		}
    	}
    	else if(desc.depthStencil.depthTestEnabled ||
    		desc.depthStencil.depthWriteEnabled || desc.depthStencil.stencilEnabled)
    	{
    		return false;
    	}

    	for(uint32_t index = 0; index < desc.colorAttachmentCount; ++index)
    	{
    		const RHI::ColorAttachmentDesc& attachment = desc.colorAttachments[index];
    		if(!IsColorFormat(attachment.format) || static_cast<uint8_t>(attachment.writeMask)>static_cast<uint8_t>(ColorWriteMask::All)) return false;
    		if(attachment.blend.enabled &&
    			((attachment.blend.sourceColor == RHI::BlendFactor::Undefined || attachment.blend.sourceColor > RHI::BlendFactor::OneMinusDestinationAlpha) ||
    				(attachment.blend.destinationColor == RHI::BlendFactor::Undefined || attachment.blend.destinationColor > RHI::BlendFactor::OneMinusDestinationAlpha) ||
    				(attachment.blend.colorOp == RHI::BlendOp::Undefined || attachment.blend.colorOp > RHI::BlendOp::Max) ||
    				(attachment.blend.sourceAlpha == RHI::BlendFactor::Undefined || attachment.blend.sourceAlpha > RHI::BlendFactor::OneMinusDestinationAlpha) ||
    				(attachment.blend.destinationAlpha == RHI::BlendFactor::Undefined || attachment.blend.destinationAlpha > RHI::BlendFactor::OneMinusDestinationAlpha) ||
    				(attachment.blend.alphaOp == RHI::BlendOp::Undefined || attachment.blend.alphaOp > RHI::BlendOp::Max)))
    		{
    			return false;
    		}
    	}
    	return true;
    }

    [[nodiscard]] inline bool ValidateMeshPipelineDesc(
    	const RHI::MeshPipelineDesc& desc)
    {
        if ((desc.layout.inlineConstantStages & ShaderStageFlags::Compute) != ShaderStageFlags::None)
            return false;
        if (desc.layout.bindings)
            for (uint32_t i = 0; i < desc.layout.bindingCount; ++i)
                if ((desc.layout.bindings[i].stages & ShaderStageFlags::Compute) != ShaderStageFlags::None)
                    return false;
    	if (desc.colorAttachmentCount != 0 && desc.colorAttachments == nullptr)
    		return false;

    	auto* meshShader = dynamic_cast<RHI::Shader*>(desc.meshShader);
    	if (meshShader == nullptr || meshShader->GetStage() != RHI::ShaderStage::Mesh ||
    		(desc.raster.fillMode == RHI::FillMode::Undefined || desc.raster.fillMode > RHI::FillMode::Wireframe) ||
    		(desc.raster.cullMode == RHI::CullMode::Undefined || desc.raster.cullMode > RHI::CullMode::Back) ||
    		(desc.raster.frontFace == RHI::FrontFace::Undefined || desc.raster.frontFace > RHI::FrontFace::Clockwise) ||
    		!std::isfinite(desc.raster.depthBiasConstant) ||
    		!std::isfinite(desc.raster.depthBiasSlope) ||
    		!std::isfinite(desc.raster.depthBiasClamp))
    	{
    		return false;
    	}

    	if (desc.fragmentShader != nullptr)
    	{
    		auto* fragmentShader = dynamic_cast<RHI::Shader*>(desc.fragmentShader);
    		if (fragmentShader == nullptr ||
    			fragmentShader->GetStage() != RHI::ShaderStage::Fragment)
    		{
    			return false;
    		}
    	}
    	if (desc.colorAttachmentCount != 0 && desc.fragmentShader == nullptr)
    		return false;

    	if (desc.depthStencil.format != RHI::Format::Unknown)
    	{
    		if (!IsDepthFormat(desc.depthStencil.format)) return false;
    		if (desc.depthStencil.depthTestEnabled &&
    			(desc.depthStencil.depthCompareOp == RHI::CompareOp::Undefined || desc.depthStencil.depthCompareOp > RHI::CompareOp::Always))
    		{
    			return false;
    		}
    		if (desc.depthStencil.stencilEnabled)
    		{
    			const auto validFace = [](const RHI::StencilFaceState& face)
    			{
    				return (face.failOp > RHI::StencilOp::Undefined && face.failOp <= RHI::StencilOp::DecrementWrap) &&
    					(face.depthFailOp > RHI::StencilOp::Undefined && face.depthFailOp <= RHI::StencilOp::DecrementWrap) &&
    					(face.passOp > RHI::StencilOp::Undefined && face.passOp <= RHI::StencilOp::DecrementWrap) &&
    					(face.compareOp > RHI::CompareOp::Undefined && face.compareOp <= RHI::CompareOp::Always);
    			};
    			if (!validFace(desc.depthStencil.front) || !validFace(desc.depthStencil.back))
    				return false;
    		}
    	}

    	for (uint32_t index = 0; index < desc.colorAttachmentCount; ++index)
    	{
    		const RHI::ColorAttachmentDesc& attachment = desc.colorAttachments[index];
    		if (attachment.format == RHI::Format::Unknown || IsDepthFormat(attachment.format) ||
    			(attachment.blend.enabled &&
    				((attachment.blend.sourceColor == RHI::BlendFactor::Undefined || attachment.blend.sourceColor > RHI::BlendFactor::OneMinusDestinationAlpha) ||
    				(attachment.blend.destinationColor == RHI::BlendFactor::Undefined || attachment.blend.destinationColor > RHI::BlendFactor::OneMinusDestinationAlpha) ||
    				(attachment.blend.colorOp == RHI::BlendOp::Undefined || attachment.blend.colorOp > RHI::BlendOp::Max) ||
    				(attachment.blend.sourceAlpha == RHI::BlendFactor::Undefined || attachment.blend.sourceAlpha > RHI::BlendFactor::OneMinusDestinationAlpha) ||
    				(attachment.blend.destinationAlpha == RHI::BlendFactor::Undefined || attachment.blend.destinationAlpha > RHI::BlendFactor::OneMinusDestinationAlpha) ||
    				(attachment.blend.alphaOp == RHI::BlendOp::Undefined || attachment.blend.alphaOp > RHI::BlendOp::Max))))
    		{
    			return false;
    		}
    	}
    	return true;
    }

    [[nodiscard]] inline const RHI::ResourceBindingLayout* FindLayoutBinding(
    	const RHI::PipelineLayoutDesc& layout,
    	uint32_t binding)
    {
    	for(uint32_t index = 0; index < layout.bindingCount; ++index)
    	{
    		const RHI::ResourceBindingLayout& candidate = layout.bindings[index];
    		if(candidate.binding == binding)
    			return &candidate;
    	}
    	return nullptr;
    }

    [[nodiscard]] inline bool ValidateResourceSetDesc(const RHI::ResourceSetDesc& desc)
    {
    	auto* pipeline = dynamic_cast<RHI::Pipeline*>(desc.pipeline);
    	if(pipeline == nullptr || (desc.bindingCount != 0 && desc.bindings == nullptr))
    		return false;
    	const RHI::PipelineLayoutDesc& layout = pipeline->GetLayout();

    	std::set<std::pair<uint32_t, uint32_t>> populated;
    	for(uint32_t index = 0; index < desc.bindingCount; ++index)
    	{
    		const RHI::ResourceBinding& binding = desc.bindings[index];
    		const RHI::ResourceBindingLayout* declaration =
    			FindLayoutBinding(layout, binding.binding);
    		if(declaration == nullptr ||
    			declaration->type == RHI::ResourceBindingType::StaticSampler ||
    			binding.arrayElement >= declaration->count ||
    			!populated.emplace(binding.binding, binding.arrayElement).second)
    		{
    			return false;
    		}

    		switch(declaration->type)
    		{
    		case RHI::ResourceBindingType::ConstantBuffer:
    		case RHI::ResourceBindingType::ReadOnlyStorageBuffer:
    		case RHI::ResourceBindingType::ReadWriteStorageBuffer:
    		{
    			auto* buffer = dynamic_cast<RHI::Buffer*>(binding.buffer);
    			const RHI::BufferUsage required =
    				declaration->type == RHI::ResourceBindingType::ConstantBuffer
    				? RHI::BufferUsage::Constant
    				: RHI::BufferUsage::Storage;
    			if(buffer == nullptr || binding.texture != nullptr || binding.size == 0 ||
    				!HasUsage(buffer->GetDesc().usage, required) ||
    				binding.offset > buffer->GetDesc().size ||
    				binding.size > buffer->GetDesc().size - binding.offset)
    			{
    				return false;
    			}
    			break;
    		}
    		case RHI::ResourceBindingType::SampledTexture:
    		case RHI::ResourceBindingType::StorageTexture:
    		{
    			auto* texture = dynamic_cast<RHI::Texture*>(binding.texture);
    			const RHI::TextureUsage required =
    				declaration->type == RHI::ResourceBindingType::SampledTexture
    				? RHI::TextureUsage::ShaderResource : RHI::TextureUsage::Storage;
    			uint32_t firstMip = 0;
    			uint32_t mipCount = 0;
    			uint32_t firstLayer = 0;
    			uint32_t layerCount = 0;
    			if(texture == nullptr || binding.buffer != nullptr ||
    				!HasUsage(texture->GetDesc().usage, required))
    			{
    				return false;
    			}
    			if(!ResolveBindingSubresources(
    				texture, binding, declaration->type,
    				firstMip, mipCount, firstLayer, layerCount)) return false;
    			break;
    		}
    		default:
    			return false;
    		}
    	}

    	for(uint32_t index = 0; index < layout.bindingCount; ++index)
    	{
    		const RHI::ResourceBindingLayout& declaration = layout.bindings[index];
    		if(declaration.type == RHI::ResourceBindingType::StaticSampler) continue;
    		for(uint32_t element = 0; element < declaration.count; ++element)
    		{
    			if(populated.find({declaration.binding, element}) == populated.end())
    				return false;
    		}
    	}
    	return true;
    }
}
