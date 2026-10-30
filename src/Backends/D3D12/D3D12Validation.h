#pragma once

#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/Texture.h"

namespace dyf::Backends::D3D12Validation
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
            return D3D12Validation::HasUsage(desc.usage, RHI::BufferUsage::Vertex);
        case RHI::ResourceState::IndexBuffer:
            return D3D12Validation::HasUsage(desc.usage, RHI::BufferUsage::Index);
        case RHI::ResourceState::IndirectArgument:
            return D3D12Validation::HasUsage(desc.usage, RHI::BufferUsage::Indirect);
        case RHI::ResourceState::ConstantBuffer:
            return D3D12Validation::HasUsage(desc.usage, RHI::BufferUsage::Constant);
        case RHI::ResourceState::ShaderResource:
        case RHI::ResourceState::UnorderedAccess:
            return D3D12Validation::HasUsage(desc.usage, RHI::BufferUsage::Storage);
        default:
            return false;
        }
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
