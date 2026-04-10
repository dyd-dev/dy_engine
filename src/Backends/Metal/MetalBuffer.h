#pragma once
#include "RHI/IBuffer.h"

namespace dy::Backends
{
    class MetalBuffer : public RHI::IBuffer
    {
    public:
        MetalBuffer(const RHI::BufferDesc& desc, void* device);
        ~MetalBuffer() override;

        void* Map(uint32_t offset, uint32_t size) override;
        void  Unmap() override;
        uint32_t GetSize() const override;

        // MetalDevice.mm에서 내부적으로 접근할 용도
        void* GetNativeBuffer() const;

    private:
        struct Impl;
        Impl* m_impl = nullptr;
    };
}
