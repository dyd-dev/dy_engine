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
        void BindGlobalDescriptors() override;
        void BindVertexBuffer(uint32_t slot, RHI::IBuffer* buffer, uint32_t offset) override;
        void BindIndexBuffer(RHI::IBuffer* buffer, RHI::Format format, uint32_t offset) override;
        void BindConstantBuffer(uint32_t binding, RHI::IBuffer* buffer, uint32_t offset, uint32_t size) override;
        void BindStorageBuffer(uint32_t binding, RHI::IBuffer* buffer, uint32_t offset, uint32_t size) override;
        void BindTexture(uint32_t binding, RHI::ITexture* texture) override;
        void SetInlineConstants(uint32_t size, const void* data) override;

        void SetRenderTargets(uint32_t numRenderTargets, RHI::ITexture** renderTargets, RHI::ITexture* depthStencil) override;
        void SetViewport(const RHI::Viewport& viewport) override;
        void SetScissor(const RHI::Rect& rect) override;
        void ClearColor(RHI::ITexture* renderTarget, float r, float g, float b, float a) override;
        void ClearDepth(RHI::ITexture* depthStencil, float depth) override;

        void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance) override;
        void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) override;

        void Close() override;

        void Begin();
        void* GetNativeCommandBuffer() const;

        void SetNativePipelineState(void* pipelineState);
        void SetNativeVertexBuffer(RHI::IBuffer* buffer, uint32_t index);
        void SetNativeTexture(void* texture, uint32_t index);
        void DrawIndexed(RHI::IBuffer* indexBuffer, uint32_t indexCount);

    private:
        void EnsureRenderEncoder();
        void EndActivePass();

        struct Impl;
        Impl* m_impl = nullptr;
    };
}
