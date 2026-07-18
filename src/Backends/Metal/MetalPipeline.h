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
