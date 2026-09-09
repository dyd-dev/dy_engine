#pragma once

#include "RHI/IDevice.h"

namespace dy::Backends
{
    struct D3D12InternalState;

    class D3D12Device final : public RHI::IDevice
    {
    public:
        D3D12Device();
        ~D3D12Device() override;

        bool CreateSwapchain(const RHI::SwapchainDesc& desc) override;
        bool BeginFrame() override;
        RHI::ICommandList* AcquireCommandList() override;
        bool Submit(RHI::ICommandList** commandLists, uint32_t count) override;
        void Present() override;
        RHI::TextureHandle GetBackBuffer() override;
        bool ReadTexture(RHI::TextureHandle texture, RHI::TextureReadback& result) override;

        RHI::BufferHandle CreateBuffer(const RHI::BufferDesc& desc) override;
        RHI::TextureHandle CreateTexture(const RHI::TextureDesc& desc) override;
        RHI::ShaderHandle CreateShader(const RHI::ShaderDesc& desc) override;
        RHI::PipelineHandle CreateGraphicsPipeline(
            const RHI::GraphicsPipelineDesc& desc) override;
        RHI::ResourceSetHandle CreateResourceSet(
            const RHI::ResourceSetDesc& desc) override;

        void DestroyBuffer(RHI::BufferHandle buffer) override;
        void DestroyTexture(RHI::TextureHandle texture) override;
        void DestroyShader(RHI::ShaderHandle shader) override;
        void DestroyPipeline(RHI::PipelineHandle pipeline) override;
        void DestroyResourceSet(RHI::ResourceSetHandle resourceSet) override;

        bool UpdateBuffer(
            RHI::ICommandList& commandList,
            RHI::BufferHandle buffer,
            uint32_t offset,
            const void* data,
            uint32_t size) override;
        bool UpdateTexture(
            RHI::ICommandList& commandList,
            RHI::TextureHandle texture,
            uint32_t mipLevel,
            uint32_t arrayLayer,
            const void* data,
            uint32_t dataSize,
            uint32_t rowPitch,
            uint32_t slicePitch) override;

    protected:
        int Initialize(const void* windowHandle, const RHI::DeviceDesc& desc) override;

    private:
        D3D12InternalState* m_internal = nullptr;
    };
}
