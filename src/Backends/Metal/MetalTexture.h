//
//  MetalTexture.h
//  
//
//  Created by 정준혁 on 4/8/26.
//

#pragma once
#include "RHI/ITexture.h"

namespace dy::Backends
{
    class MetalTexture : public RHI::ITexture
    {
    public:
        MetalTexture(const RHI::TextureDesc& desc, void* device);
        ~MetalTexture() override;

        void* GetNativeTexture() const;
        void SetNativeTexture(void* texture);
    private:
        struct Impl;
        Impl* m_impl = nullptr;
    };

}
