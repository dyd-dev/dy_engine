#pragma once
#include "RHI/GraphicsPipeline.h"
#include "RHI/IResourceSet.h"
#include "RHI/ISampler.h"
#include <cstdint>
#include <vector>

// 전방 선언을 사용
struct ID3D12PipelineState;
struct ID3D12RootSignature;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;

namespace dy::Backends
{
    // 내부 구현체 숨기기 (Pimpl)
    struct D3D12PipelineStateInternal;

    struct D3D12ResourceBinding
    {
        RHI::ResourceBindingDesc desc = {};
        uint32_t resourceRootParameter = 0;
        uint32_t samplerRootParameter = 0;
        uint32_t heapOffset = 0;
        uint32_t samplerHeapOffset = 0;
    };

    struct D3D12InlineConstantRange
    {
        RHI::InlineConstantRangeDesc desc = {};
        uint32_t rootParameter = 0;
    };

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
            uint32_t stencilReference,
            std::vector<D3D12ResourceBinding> resourceBindings,
            std::vector<D3D12InlineConstantRange> inlineConstantRanges);
        ~D3D12PipelineState() override;

        ID3D12PipelineState* GetNativePSO() const;
        ID3D12RootSignature* GetNativeRootSignature() const;
        RHI::PrimitiveTopology GetPrimitiveTopology() const;
        bool GetVertexStride(uint32_t slot, uint32_t& stride) const;
        uint32_t GetStencilReference() const;
        const std::vector<D3D12ResourceBinding>& GetResourceBindings() const { return m_resourceBindings; }
        const std::vector<D3D12InlineConstantRange>& GetInlineConstantRanges() const { return m_inlineConstantRanges; }

    private:
        RHI::PrimitiveTopology m_topology = RHI::PrimitiveTopology::TriangleList;
        std::vector<RHI::VertexBindingDesc> m_vertexBindings;
        std::vector<D3D12ResourceBinding> m_resourceBindings;
        std::vector<D3D12InlineConstantRange> m_inlineConstantRanges;
        uint32_t m_stencilReference = 0;
        D3D12PipelineStateInternal* m_internal; // ComPtr들은 이 안에 숨깁니다.
    };

    class D3D12Sampler final : public RHI::ISampler
    {
    public:
        explicit D3D12Sampler(const RHI::SamplerDesc& desc);
        ~D3D12Sampler() override;
        const void* GetNativeDesc() const;

    private:
        struct Internal;
        Internal* m_internal = nullptr;
    };

    class D3D12ResourceSet final : public RHI::IResourceSet
    {
    public:
        D3D12ResourceSet(ID3D12Device* device, D3D12PipelineState* pipeline);
        ~D3D12ResourceSet() override;
        bool IsValid() const;
        bool Update(ID3D12Device* device, const RHI::ResourceSetWrite* writes, uint32_t writeCount);
        void Bind(ID3D12GraphicsCommandList* commandList, D3D12PipelineState* activePipeline);

    private:
        struct Internal;
        Internal* m_internal = nullptr;
    };
}
