//
//  MetalTexture.h
//  
//
//  Created by 정준혁 on 4/8/26.
//

#pragma once

#include <vector>

#include "RHI/Texture.h"
#include "RHI/ResourceState.h"

namespace dy::Backends
{
    class MetalDevice;
    struct MetalObjectDeleter;

    class MetalTexture : public RHI::Texture
    {
    public:
        MetalTexture(const RHI::TextureDesc& desc, void* device);

        void* GetNativeTexture() const;
        [[nodiscard]] bool IsSwapchainImage() const;
		[[nodiscard]] RHI::ResourceState GetState(uint32_t mipLevel, uint32_t arrayLayer) const;
		void SetState(uint32_t mipLevel, uint32_t arrayLayer, RHI::ResourceState state);

    private:
        friend class MetalDevice;
        friend struct MetalObjectDeleter;

        explicit MetalTexture(const RHI::TextureDesc& desc);
        ~MetalTexture() override;
        void SetBackBuffer(void* texture, const RHI::TextureDesc& desc);

        struct Impl;
        Impl* m_impl = nullptr;
		std::vector<RHI::ResourceState> m_states;
    };

}
