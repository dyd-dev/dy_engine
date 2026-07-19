//
//  MetalPipeline.h
//  
//
//  Created by 정준혁 on 4/8/26.
//

#pragma once
#include "RHI/IPipelineState.h"

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
        float GetDepthBias() const;
        float GetDepthBiasSlope() const;
        float GetDepthBiasClamp() const;
        bool IsValid() const;

    private:
        struct Impl;
        Impl* m_impl = nullptr;
    };
}
