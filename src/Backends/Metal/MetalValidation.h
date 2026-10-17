#pragma once

#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/ResourceSet.h"
#include "dyf/RHI/Texture.h"
#include <algorithm>
#include <set>

namespace dyf::Backends::MetalValidation
{
    [[nodiscard]] inline bool MayWriteStencil(const RHI::DepthStencilState& state)
    {
        const auto changesValue = [](const RHI::StencilFaceState& face)
        {
            return face.failOp != RHI::StencilOp::Keep ||
                face.depthFailOp != RHI::StencilOp::Keep || face.passOp != RHI::StencilOp::Keep;
        };
        return state.stencilEnabled && state.stencilWriteMask != 0 &&
            (changesValue(state.front) || changesValue(state.back));
    }

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
        case RHI::Format::R32_FLOAT:
            return 4;
        case RHI::Format::R16G16B16A16_FLOAT:
        case RHI::Format::R32G32_FLOAT:
        case RHI::Format::D32_FLOAT_S8_UINT:
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
            RHI::HasStencil(format);
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
            return MetalValidation::HasUsage(desc.usage, RHI::BufferUsage::Vertex);
        case RHI::ResourceState::IndexBuffer:
            return MetalValidation::HasUsage(desc.usage, RHI::BufferUsage::Index);
        case RHI::ResourceState::IndirectArgument:
            return MetalValidation::HasUsage(desc.usage, RHI::BufferUsage::Indirect);
        case RHI::ResourceState::ConstantBuffer:
            return MetalValidation::HasUsage(desc.usage, RHI::BufferUsage::Constant);
        case RHI::ResourceState::ShaderResource:
        case RHI::ResourceState::UnorderedAccess:
            return MetalValidation::HasUsage(desc.usage, RHI::BufferUsage::Storage);
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
            return MetalValidation::HasUsage(desc.usage, RHI::TextureUsage::ShaderResource);
        case RHI::ResourceState::UnorderedAccess:
            return MetalValidation::HasUsage(desc.usage, RHI::TextureUsage::Storage);
        case RHI::ResourceState::RenderTarget:
        case RHI::ResourceState::Present:
            return MetalValidation::HasUsage(desc.usage, RHI::TextureUsage::RenderTarget);
        case RHI::ResourceState::DepthRead:
        case RHI::ResourceState::DepthWrite:
            return MetalValidation::HasUsage(desc.usage, RHI::TextureUsage::DepthStencil);
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
        return MetalValidation::ResolveSubresources(
            texture, binding.subresources,
            firstMip, mipCount, firstLayer, layerCount) &&
            (type != RHI::ResourceBindingType::StorageTexture || mipCount == 1);
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
        if(desc.pipeline == nullptr || (desc.bindingCount != 0 && desc.bindings == nullptr))
            return false;
        const RHI::PipelineLayoutDesc& layout = desc.pipeline->GetLayout();

        std::set<std::pair<uint32_t, uint32_t>> populated;
        for(uint32_t index = 0; index < desc.bindingCount; ++index)
        {
            const RHI::ResourceBinding& binding = desc.bindings[index];
            const RHI::ResourceBindingLayout* declaration =
                MetalValidation::FindLayoutBinding(layout, binding.binding);
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
                const RHI::BufferUsage required =
                    declaration->type == RHI::ResourceBindingType::ConstantBuffer
                    ? RHI::BufferUsage::Constant
                    : RHI::BufferUsage::Storage;
                if(binding.buffer == nullptr || binding.texture != nullptr || binding.size == 0 ||
                    !MetalValidation::HasUsage(binding.buffer->GetDesc().usage, required) ||
                    binding.offset > binding.buffer->GetDesc().size ||
                    binding.size > binding.buffer->GetDesc().size - binding.offset)
                {
                    return false;
                }
                break;
            }
            case RHI::ResourceBindingType::SampledTexture:
            case RHI::ResourceBindingType::StorageTexture:
            {
                const RHI::TextureUsage required =
                    declaration->type == RHI::ResourceBindingType::SampledTexture
                    ? RHI::TextureUsage::ShaderResource : RHI::TextureUsage::Storage;
                uint32_t firstMip = 0;
                uint32_t mipCount = 0;
                uint32_t firstLayer = 0;
                uint32_t layerCount = 0;
                if(binding.texture == nullptr || binding.buffer != nullptr ||
                    !MetalValidation::HasUsage(binding.texture->GetDesc().usage, required))
                {
                    return false;
                }
                if(!MetalValidation::ResolveBindingSubresources(
                    binding.texture, binding, declaration->type,
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
