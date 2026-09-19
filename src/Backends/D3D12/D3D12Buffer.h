#pragma once

#include "dyf/RHI/Buffer.h"

namespace dyf::Backends
{
    struct D3D12BufferInternal;
    struct D3D12ObjectDeleter;

    class D3D12Buffer final : public RHI::Buffer
    {
    public:
        D3D12Buffer(void* nativeDevice, const RHI::BufferDesc& desc);

        void* GetNativeResource() const;
        [[nodiscard]] RHI::ResourceState GetState() const;
        void SetState(RHI::ResourceState state);

    private:
        friend struct D3D12ObjectDeleter;

        ~D3D12Buffer() override;

        D3D12BufferInternal* m_internal = nullptr;
        RHI::ResourceState m_state = RHI::ResourceState::Undefined;
    };
}
