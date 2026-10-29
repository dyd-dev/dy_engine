#pragma once

#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/Binding.h"
#include "dyf/RHI/Texture.h"
#include <algorithm>

namespace dyf::Backends::VulkanValidation
{
    [[nodiscard]] inline uint32_t SelectSwapchainImageCount(
        uint32_t requestedMinimum, uint32_t surfaceMinimum, uint32_t surfaceMaximum)
    {
        if(!requestedMinimum) return 0;
        uint32_t imageCount = std::max(requestedMinimum, surfaceMinimum);
        // The caller's minimum is required; only extra buffering may follow the surface limit.
        if(surfaceMaximum && imageCount > surfaceMaximum) return 0;
        imageCount = std::max(imageCount, 3u);
        if(surfaceMaximum && imageCount > surfaceMaximum) imageCount = surfaceMaximum;
        return imageCount;
    }

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
            return VulkanValidation::HasUsage(desc.usage, RHI::BufferUsage::Vertex);
        case RHI::ResourceState::IndexBuffer:
            return VulkanValidation::HasUsage(desc.usage, RHI::BufferUsage::Index);
        case RHI::ResourceState::IndirectArgument:
            return VulkanValidation::HasUsage(desc.usage, RHI::BufferUsage::Indirect);
        case RHI::ResourceState::ConstantBuffer:
            return VulkanValidation::HasUsage(desc.usage, RHI::BufferUsage::Constant);
        case RHI::ResourceState::ShaderResource:
        case RHI::ResourceState::UnorderedAccess:
            return VulkanValidation::HasUsage(desc.usage, RHI::BufferUsage::Storage);
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
        return VulkanValidation::ResolveSubresources(
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
}
