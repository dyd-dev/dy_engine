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
        void BindResourceSet(RHI::IResourceSet* resourceSet) override;
        void BindVertexBuffer(uint32_t slot, RHI::IBuffer* buffer, uint32_t offset) override;
        void BindIndexBuffer(RHI::IBuffer* buffer, RHI::Format format, uint32_t offset) override;
        void SetInlineConstants(uint32_t offset, uint32_t size, const void* data) override;
        void Barrier(const RHI::TextureBarrier* barriers, uint32_t barrierCount) override;

        void BeginRendering(const RHI::RenderingInfo& renderingInfo) override;
        void EndRendering() override;
        void SetViewport(const RHI::Viewport& viewport) override;
        void SetScissor(const RHI::Rect& rect) override;

        void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance) override;
        void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) override;

        void Close() override;

        void Begin();
        void* GetNativeCommandBuffer() const;

        void SetNativePipelineState(void* pipelineState);
        void SetNativeVertexBuffer(RHI::IBuffer* buffer, uint32_t index);
        void DrawIndexed(RHI::IBuffer* indexBuffer, uint32_t indexCount);

    private:
        void EnsureRenderEncoder();

        struct Impl;
        Impl* m_impl = nullptr;
    };
}
