#pragma once
#include <vector>
#include "RHI/ICommandList.h"

namespace dy::Backends
{
    class MetalCommandList : public RHI::ICommandList
    {
    public:
        MetalCommandList(void* commandQueue);
        ~MetalCommandList() override;

        void BindGraphicsPipeline(RHI::IPipelineState* pipelineState) override;
        void BindGlobalDescriptorHeap() override;
        void BindIndexBuffer(RHI::IBuffer* buffer, RHI::Format format, uint32_t offset) override;
        void SetPushConstants(uint32_t size, const void* data) override;

        void SetRenderTargets(uint32_t numRenderTargets, RHI::ITexture** renderTargets, RHI::ITexture* depthStencil) override;
        void ClearColor(RHI::ITexture* renderTarget, float r, float g, float b, float a) override;
        void ClearDepth(RHI::ITexture* depthStencil, float depth) override;

        void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance) override;

        void ResourceBarrier(RHI::IBuffer* buffer, RHI::ResourceState before, RHI::ResourceState after) override;
        void ResourceBarrier(RHI::ITexture* texture, RHI::ResourceState before, RHI::ResourceState after) override;

        void Close() override;

        void Begin(void* drawable);
        void* GetNativeCommandBuffer() const;

        void SetNativePipelineState(void* pipelineState);
        void SetNativeVertexBuffer(RHI::IBuffer* buffer, uint32_t index);
        void SetNativeTexture(void* texture, uint32_t index);
        void DrawIndexed(RHI::IBuffer* indexBuffer, uint32_t indexCount);

    private:
        void EnsureRenderEncoder();

        struct Impl;
        Impl* m_impl = nullptr;
    };
}
