#pragma once

#include "RHI/Pipeline.h"

#include <cstdint>
#include <vector>

struct ID3D12PipelineState;
struct ID3D12RootSignature;

namespace dy::Backends
{
    struct D3D12ObjectDeleter;
    struct D3D12PipelineStateInternal;

    struct D3D12PipelineBinding
    {
        RHI::ResourceBindingLayout layout = {};
        uint32_t rootParameter = 0;
        uint32_t descriptorOffset = 0;
    };

    struct D3D12VertexBinding
    {
        uint32_t binding = 0;
        uint32_t stride = 0;
    };

    class D3D12PipelineState final : public RHI::Pipeline
    {
    public:
        D3D12PipelineState(
            const RHI::PipelineLayoutDesc& layout,
            ID3D12PipelineState* pipelineState,
            ID3D12RootSignature* rootSignature,
            std::vector<D3D12PipelineBinding> bindings,
            std::vector<D3D12VertexBinding> vertexBindings,
            uint32_t inlineConstantRootParameter,
            uint32_t descriptorCount,
            uint32_t primitiveTopology,
            bool stencilEnabled,
            bool requiresDepthWrite);

        ID3D12PipelineState* GetNativePipelineState() const;
        ID3D12RootSignature* GetNativeRootSignature() const;
        const std::vector<D3D12PipelineBinding>& GetBindings() const;
        uint32_t GetInlineConstantRootParameter() const;
        uint32_t GetDescriptorCount() const;
        uint32_t GetPrimitiveTopology() const;
        uint32_t GetVertexStride(uint32_t binding) const;
        bool IsStencilEnabled() const;
        bool RequiresDepthWrite() const;

    private:
        friend struct D3D12ObjectDeleter;

        ~D3D12PipelineState() override;

        D3D12PipelineStateInternal* m_internal = nullptr;
    };
}
