#include "D3D12PipelineState.h"
#include <d3d12.h>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

namespace dy::Backends
{
    // ComPtr들을 보관하는 실제 창고
    struct D3D12PipelineStateInternal
    {
        ComPtr<ID3D12PipelineState> pso;
        ComPtr<ID3D12RootSignature> rootSignature;
    };

    D3D12PipelineState::D3D12PipelineState(
        ID3D12PipelineState* pso,
        ID3D12RootSignature* rootSignature,
        RHI::PrimitiveTopology topology,
        const RHI::VertexBindingDesc* vertexBindings,
        uint32_t vertexBindingCount,
        uint32_t stencilReference)
        : m_topology(topology)
        , m_stencilReference(stencilReference)
    {
        m_internal = new D3D12PipelineStateInternal();
        m_internal->pso = pso;
        m_internal->rootSignature = rootSignature;
        if(vertexBindings != nullptr && vertexBindingCount > 0)
        {
            m_vertexBindings.assign(vertexBindings, vertexBindings + vertexBindingCount);
        }
    }

    D3D12PipelineState::~D3D12PipelineState()
    {
        delete m_internal;
    }

    ID3D12PipelineState* D3D12PipelineState::GetNativePSO() const
    {
        return m_internal->pso.Get();
    }

    ID3D12RootSignature* D3D12PipelineState::GetNativeRootSignature() const
    {
        return m_internal->rootSignature.Get();
    }

    RHI::PrimitiveTopology D3D12PipelineState::GetPrimitiveTopology() const
    {
        return m_topology;
    }

    bool D3D12PipelineState::GetVertexStride(uint32_t slot, uint32_t& stride) const
    {
        for(const RHI::VertexBindingDesc& binding : m_vertexBindings)
        {
            if(binding.slot != slot) continue;
            stride = binding.stride;
            return true;
        }
        return false;
    }

    uint32_t D3D12PipelineState::GetStencilReference() const
    {
        return m_stencilReference;
    }
}
