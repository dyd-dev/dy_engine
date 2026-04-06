#pragma once
#include "RHI/IBuffer.h"

namespace dy::Backends
{
    // DX12 버퍼 객체(ID3D12Resource)를 숨기기 위한 전방 선언
    struct D3D12BufferInternal;

    class D3D12Buffer : public RHI::IBuffer
    {
    public:
        D3D12Buffer(void* nativeDevice, const RHI::BufferDesc& desc);
        ~D3D12Buffer() override;

        void* Map(uint32_t offset, uint32_t size) override;
        void Unmap() override;
        uint32_t GetSize() const override;

        // D3D12CommandList가 바인딩할 때 진짜 리소스(ID3D12Resource)를 꺼내갈 수 있도록 함
        void* GetNativeResource() const;

    private:
        D3D12BufferInternal* m_internal;
        uint32_t m_size;
    };
}