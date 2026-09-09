#pragma once

#include "RHI/Texture.h"
#include "RHI/ResourceState.h"

#include <cstddef>
#include <cstdint>
#include <vector>

struct ID3D12Device;
struct ID3D12Resource;

namespace dy::Backends
{
    struct D3D12ObjectDeleter;
    struct D3D12TextureInternal;

    class D3D12Texture final : public RHI::Texture
    {
    public:
        D3D12Texture(ID3D12Device* device, const RHI::TextureDesc& desc);
        D3D12Texture(
            ID3D12Resource* resource,
            const RHI::TextureDesc& desc,
            std::size_t rtvHandle,
            bool swapchainImage);

        void* GetNativeResource() const;
        void* GetRenderTargetViewHeap() const;
        void* GetDepthStencilViewHeap() const;
        std::size_t GetRenderTargetViewHandle(uint32_t mipLevel, uint32_t arrayLayer) const;
        std::size_t GetDepthStencilViewHandle(
            uint32_t mipLevel,
            uint32_t arrayLayer,
            bool readOnly) const;
        [[nodiscard]] RHI::ResourceState GetState(
            uint32_t mipLevel,
            uint32_t arrayLayer) const;
        void SetState(
            uint32_t mipLevel,
            uint32_t arrayLayer,
            RHI::ResourceState state);
        bool IsSwapchainImage() const;

        static uint32_t ToDxgiFormat(RHI::Format format);
        static uint32_t ToDxgiShaderResourceFormat(RHI::Format format);
        static uint32_t ToDxgiDepthStencilFormat(RHI::Format format);

    private:
        friend struct D3D12ObjectDeleter;

        ~D3D12Texture() override;

        D3D12TextureInternal* m_internal = nullptr;
        std::vector<RHI::ResourceState> m_states;
    };
}
