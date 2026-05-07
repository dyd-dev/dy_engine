#pragma once
#include "RHI/ITexture.h"

struct ID3D12Device;
struct ID3D12Resource;

namespace dy::Backends
{
    // DX12의 텍스처 리소스를 숨기기 위한 전방 선언
    struct D3D12TextureInternal;

    class D3D12Texture : public RHI::ITexture
    {
    public:
        D3D12Texture(ID3D12Device* device, const RHI::TextureDesc& desc);
        D3D12Texture(ID3D12Resource* resource, const RHI::TextureDesc& desc);
        ~D3D12Texture() override;

        // --- ITexture 오버라이딩 ---
        uint32_t GetWidth() const override;
        uint32_t GetHeight() const override;
        RHI::Format GetFormat() const override;

        // --- D3D12 전용 ---
        void* GetNativeResource() const;
        static uint32_t ToDxgiFormat(RHI::Format format);

    private:
        D3D12TextureInternal* m_internal;
        RHI::TextureDesc m_desc; // 너비, 높이 등을 기억해두기 위한 변수
    };
}