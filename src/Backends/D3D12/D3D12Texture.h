#pragma once

#include "RHI/ITexture.h"

#include <cstddef>
#include <cstdint>

struct ID3D12Device;
struct ID3D12Resource;

namespace dy::Backends
{
    struct D3D12TextureInternal;

    class D3D12Texture : public RHI::ITexture
    {
    public:
        D3D12Texture(ID3D12Device* device, const RHI::TextureDesc& desc);
        D3D12Texture(ID3D12Resource* resource, const RHI::TextureDesc& desc);
        ~D3D12Texture() override;

        void* GetNativeResource() const;
        bool HasRenderTargetView() const;
        bool HasDepthStencilView() const;
        size_t GetRenderTargetViewHandlePtr() const;
        size_t GetDepthStencilViewHandlePtr() const;
        uint32_t GetResourceState() const;
        void SetResourceState(uint32_t state);

        static uint32_t ToDxgiFormat(RHI::Format format);
        static uint32_t ToDxgiShaderResourceFormat(RHI::Format format);
        static uint32_t ToDxgiDepthStencilFormat(RHI::Format format);

    private:
        D3D12TextureInternal* m_internal = nullptr;
    };
}
