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

        // 글로벌 디스크립터 힙 내 이 텍스처의 SRV 슬롯(UpdateDescriptorSlot 가 채움).
        // 커맨드 리스트가 SRV 디스크립터 테이블을 바인딩할 때 GPU 핸들 계산에 사용.
        uint32_t GetGlobalSrvIndex() const;
        void SetGlobalSrvIndex(uint32_t index);

        static uint32_t ToDxgiFormat(RHI::Format format);
        static uint32_t ToDxgiShaderResourceFormat(RHI::Format format);
        static uint32_t ToDxgiDepthStencilFormat(RHI::Format format);

    private:
        D3D12TextureInternal* m_internal = nullptr;
    };
}
