#include "D3D12PipelineState.h"

#include <d3d12.h>
#include <limits>
#include <utility>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

namespace dy::Backends
{
    struct D3D12PipelineStateInternal
    {
        ComPtr<ID3D12PipelineState> pipelineState;
        ComPtr<ID3D12RootSignature> rootSignature;
        std::vector<D3D12PipelineBinding> bindings;
        std::vector<D3D12VertexBinding> vertexBindings;
        uint32_t inlineConstantRootParameter = std::numeric_limits<uint32_t>::max();
        uint32_t descriptorCount = 0;
        uint32_t primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
        bool stencilEnabled = false;
        bool requiresDepthWrite = false;
    };

    D3D12PipelineState::D3D12PipelineState(
        const RHI::PipelineLayoutDesc& layout,
        ID3D12PipelineState* pipelineState,
        ID3D12RootSignature* rootSignature,
        std::vector<D3D12PipelineBinding> bindings,
        std::vector<D3D12VertexBinding> vertexBindings,
        uint32_t inlineConstantRootParameter,
        uint32_t descriptorCount,
        uint32_t primitiveTopology,
        bool stencilEnabled,
        bool requiresDepthWrite)
        : RHI::Pipeline(layout)
        , m_internal(new D3D12PipelineStateInternal())
    {
        m_internal->pipelineState = pipelineState;
        m_internal->rootSignature = rootSignature;
        m_internal->bindings = std::move(bindings);
        m_internal->vertexBindings = std::move(vertexBindings);
        m_internal->inlineConstantRootParameter = inlineConstantRootParameter;
        m_internal->descriptorCount = descriptorCount;
        m_internal->primitiveTopology = primitiveTopology;
        m_internal->stencilEnabled = stencilEnabled;
        m_internal->requiresDepthWrite = requiresDepthWrite;
    }

    D3D12PipelineState::~D3D12PipelineState()
    {
        delete m_internal;
    }

    ID3D12PipelineState* D3D12PipelineState::GetNativePipelineState() const
    {
        return m_internal->pipelineState.Get();
    }

    ID3D12RootSignature* D3D12PipelineState::GetNativeRootSignature() const
    {
        return m_internal->rootSignature.Get();
    }

    const std::vector<D3D12PipelineBinding>& D3D12PipelineState::GetBindings() const
    {
        return m_internal->bindings;
    }

    uint32_t D3D12PipelineState::GetInlineConstantRootParameter() const
    {
        return m_internal->inlineConstantRootParameter;
    }

    uint32_t D3D12PipelineState::GetDescriptorCount() const
    {
        return m_internal->descriptorCount;
    }

    uint32_t D3D12PipelineState::GetPrimitiveTopology() const
    {
        return m_internal->primitiveTopology;
    }

    uint32_t D3D12PipelineState::GetVertexStride(uint32_t binding) const
    {
        for (const D3D12VertexBinding& vertexBinding : m_internal->vertexBindings)
        {
            if (vertexBinding.binding == binding) return vertexBinding.stride;
        }
        return 0;
    }

    bool D3D12PipelineState::IsStencilEnabled() const
    {
        return m_internal->stencilEnabled;
    }

    bool D3D12PipelineState::RequiresDepthWrite() const
    {
        return m_internal->requiresDepthWrite;
    }
}
