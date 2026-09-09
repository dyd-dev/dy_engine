#pragma once
#include "RHI/Buffer.h"

namespace dy::Backends
{
    struct MetalObjectDeleter;

    class MetalBuffer : public RHI::Buffer
    {
    public:
        MetalBuffer(const RHI::BufferDesc& desc, void* device);

        [[nodiscard]] void* GetNativeBuffer() const;
        [[nodiscard]] RHI::ResourceState GetState() const;
        void SetState(RHI::ResourceState state);

    private:
        friend struct MetalObjectDeleter;

        ~MetalBuffer() override;

        struct Impl;
        Impl* m_impl = nullptr;
		RHI::ResourceState m_state = RHI::ResourceState::Undefined;
    };
}
