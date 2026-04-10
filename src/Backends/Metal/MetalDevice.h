#pragma once
#include "RHI/IDevice.h"
#include "RHI/ICommandList.h"

namespace dy::Backends
{
    class MetalDevice : public RHI::IDevice
    {
    public:
        MetalDevice();
        ~MetalDevice() override;

        void BeginFrame() override;
        uint32_t GetCurrentFrameIndex() const override;

        RHI::ICommandList* AcquireCommandList() override;
        void Submit(RHI::ICommandList** cmdLists, uint32_t count) override;
        void Present() override;

        RHI::IBuffer*        CreateBuffer(const RHI::BufferDesc& desc) override;
        RHI::ITexture*       CreateTexture(const RHI::TextureDesc& desc) override;
        RHI::IPipelineState* CreateGraphicsPipeline(const RHI::GraphicsPipelineDesc& desc) override;

        void DestroyBuffer(RHI::IBuffer* buffer) override;
        void DestroyTexture(RHI::ITexture* texture) override;
        void DestroyPipelineState(RHI::IPipelineState* pipeline) override;

        RHI::ITexture* GetBackBuffer() override;

    protected:
        int Initialize(const void* windowHandle) override;

    private:
        struct Impl;
        Impl* m_impl = nullptr;
    };
}
