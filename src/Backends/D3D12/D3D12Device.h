#pragma once

#include "dyf/RHI/IDevice.h"

namespace dyf::Backends
{
    struct D3D12InternalState;

    class D3D12Device final : public RHI::IDevice
    {
    public:
        D3D12Device();
        ~D3D12Device() override;

        void DestroySwapchainNative() override;
        bool WaitIdleNative() override;
        bool IsLostNative() const override;
        bool SupportsNative(RHI::Feature) const override;
        RHI::TimestampQueryHandle CreateTimestampQueryNative(const RHI::TimestampQueryDesc&) override;
        void DestroyTimestampQueryNative(RHI::TimestampQueryHandle) override;
        bool ReadTimestampsNative(RHI::TimestampQueryHandle, uint32_t, uint32_t, uint64_t*) override;
        double GetTimestampPeriodNative() const override;
        uint32_t GetTimestampValidBitsNative() const override;
        uint64_t GetLimitNative(RHI::Limit) const override;
        bool SupportsSamplerNative(const RHI::SamplerDesc&) const override;
        bool SupportsPipelineLayoutNative(const RHI::PipelineLayoutDesc&) const override;
        bool SupportsGraphicsPipelineNative(const RHI::GraphicsPipelineDesc& desc) const override;
        bool SupportsMeshPipelineNative(const RHI::MeshPipelineDesc& desc) const override;
        uint64_t GetCompletedSubmissionNative() override;
        uint64_t GetLastSubmissionNative() const override;

        bool CreateSwapchainNative(const RHI::SwapchainDesc& desc) override;
        bool BeginFrameNative() override;
        RHI::ICommandList* AcquireCommandListNative() override;
        void DiscardCommandListNative(RHI::ICommandList*) override;
        bool SubmitNative(RHI::ICommandList** commandLists, uint32_t count) override;
        bool PresentNative() override;
        RHI::TextureHandle GetBackBufferNative() override;
        bool ReadTextureNative(RHI::TextureHandle texture, RHI::TextureReadback& result) override;

        RHI::BufferHandle CreateBufferNative(const RHI::BufferDesc& desc) override;
        RHI::TextureHandle CreateTextureNative(const RHI::TextureDesc& desc) override;
        RHI::ShaderHandle CreateShaderNative(const RHI::ShaderDesc& desc) override;
        RHI::PipelineHandle CreateGraphicsPipelineNative(
            const RHI::GraphicsPipelineDesc& desc) override;
        RHI::PipelineHandle CreateMeshPipelineNative(
            const RHI::MeshPipelineDesc& desc) override;
        RHI::ResourceSetHandle CreateResourceSetNative(
            const RHI::ResourceSetDesc& desc) override;

        void DestroyBufferNative(RHI::BufferHandle buffer) override;
        void DestroyTextureNative(RHI::TextureHandle texture) override;
        void DestroyShaderNative(RHI::ShaderHandle shader) override;
        void DestroyPipelineNative(RHI::PipelineHandle pipeline) override;
        void DestroyResourceSetNative(RHI::ResourceSetHandle resourceSet) override;

        bool UpdateBufferNative(
            RHI::ICommandList& commandList,
            RHI::BufferHandle buffer,
            uint32_t offset,
            const void* data,
            uint32_t size) override;
        bool UpdateTextureNative(
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
