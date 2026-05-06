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

    D3D12PipelineState::D3D12PipelineState(ID3D12PipelineState* pso, ID3D12RootSignature* rootSignature)
    {
        m_internal = new D3D12PipelineStateInternal();
        m_internal->pso = pso;
        m_internal->rootSignature = rootSignature;
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
}