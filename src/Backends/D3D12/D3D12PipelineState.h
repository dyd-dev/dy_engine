#pragma once
#include "RHI/IPipelineState.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace dy::Backends
{
    class D3D12PipelineState : public RHI::IPipelineState
    {
    public:
        D3D12PipelineState(Microsoft::WRL::ComPtr<ID3D12PipelineState> pso,
            Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature)
            : m_pso(pso), m_rootSignature(rootSignature) {
        }

        ~D3D12PipelineState() override = default;

        ID3D12PipelineState* GetNativePSO() const { return m_pso.Get(); }
        ID3D12RootSignature* GetNativeRootSignature() const { return m_rootSignature.Get(); }

    private:
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    };
}