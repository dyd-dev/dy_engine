#pragma once
#include "RHI/GraphicsPipeline.h"
#include <cstdint>
#include <vector>

// 전방 선언을 사용
struct ID3D12PipelineState;
struct ID3D12RootSignature;

namespace dy::Backends
{
    // 내부 구현체 숨기기 (Pimpl)
    struct D3D12PipelineStateInternal;

    class D3D12PipelineState : public RHI::IPipelineState
    {
    public:
        // ComPtr 대신 원시 포인터로 받습니다.
        D3D12PipelineState(
            ID3D12PipelineState* pso,
            ID3D12RootSignature* rootSignature,
            RHI::PrimitiveTopology topology,
            const RHI::VertexBindingDesc* vertexBindings,
            uint32_t vertexBindingCount,
            uint32_t stencilReference);
        ~D3D12PipelineState() override;

        ID3D12PipelineState* GetNativePSO() const;
        ID3D12RootSignature* GetNativeRootSignature() const;
        RHI::PrimitiveTopology GetPrimitiveTopology() const;
        bool GetVertexStride(uint32_t slot, uint32_t& stride) const;
        uint32_t GetStencilReference() const;

    private:
        RHI::PrimitiveTopology m_topology = RHI::PrimitiveTopology::TriangleList;
        std::vector<RHI::VertexBindingDesc> m_vertexBindings;
        uint32_t m_stencilReference = 0;
        D3D12PipelineStateInternal* m_internal; // ComPtr들은 이 안에 숨깁니다.
    };
}
