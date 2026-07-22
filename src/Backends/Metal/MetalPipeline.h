//
//  MetalPipeline.h
//  
//
//  Created by 정준혁 on 4/8/26.
//

#pragma once
#include "RHI/GraphicsPipeline.h"
#include "RHI/IResourceSet.h"
#include "RHI/ISampler.h"
#include <cstdint>
#include <vector>

namespace dy::Backends
{
    class MetalShader final : public RHI::IShader
    {
    public:
        MetalShader(const RHI::ShaderDesc& desc, void* device);
        ~MetalShader() override;

        [[nodiscard]] RHI::ShaderStage GetStage() const;
        void* GetNativeFunction() const;
        bool IsValid() const;

    private:
        struct Impl;
        Impl* m_impl = nullptr;
    };

    class MetalPipeline : public RHI::IPipelineState
    {
    public:
        MetalPipeline(const RHI::GraphicsPipelineDesc& desc, void* device);
        ~MetalPipeline() override;

        void* GetNativePipeline() const;
        void* GetNativeDepthStencil() const;
        const RHI::RasterizationDesc& GetRasterization() const;
        uint32_t GetStencilReference() const;
        RHI::PrimitiveTopology GetPrimitiveTopology() const;
        const std::vector<RHI::ResourceBindingDesc>& GetResourceBindings() const;
        const std::vector<RHI::InlineConstantRangeDesc>& GetInlineConstantRanges() const;
        static uint32_t GetNativeVertexBufferIndex(uint32_t slot);
        bool IsValid() const;

    private:
        struct Impl;
        Impl* m_impl = nullptr;
    };

    class MetalSampler final : public RHI::ISampler
    {
    public:
        MetalSampler(const RHI::SamplerDesc& desc, void* device);
        ~MetalSampler() override;
        void* GetNativeSampler() const;
        bool IsValid() const;

    private:
        struct Impl;
        Impl* m_impl = nullptr;
    };

    class MetalResourceSet final : public RHI::IResourceSet
    {
    public:
        explicit MetalResourceSet(MetalPipeline* pipeline);
        ~MetalResourceSet() override;
        bool Update(const RHI::ResourceSetWrite* writes, uint32_t writeCount);
        void Bind(void* encoder, MetalPipeline* activePipeline);

    private:
        struct Impl;
        Impl* m_impl = nullptr;
    };
}
