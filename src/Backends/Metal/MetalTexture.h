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

        uint32_t   GetWidth()  const override;
        uint32_t   GetHeight() const override;
        RHI::Format GetFormat() const override;

        void* GetNativeTexture() const;

    private:
        struct Impl;
        Impl* m_impl = nullptr;
    };
}
