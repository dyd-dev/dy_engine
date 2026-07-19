#pragma once
#include <cstddef>
#include "RHI/ICommandList.h"

namespace dy::Backends
{
    struct D3D12CommandListInternal;

    class D3D12CommandList : public RHI::ICommandList
    {
    public:
        D3D12CommandList(void* nativeDevice, size_t rtvHandlePtr, void* globalDescriptorHeap = nullptr, uint32_t srvDescriptorSize = 0);
        ~D3D12CommandList() override;

        void Reset();

        void BindGraphicsPipeline(RHI::IPipelineState* pipelineState) override;

        void BindGlobalDescriptors() override;

        void BindGeometry(const RHI::GeometryBinding& geometry) override;
        void BindConstantBuffer(uint32_t binding, RHI::IBuffer* buffer, uint32_t offset, uint32_t size) override;
        void BindTexture(uint32_t binding, RHI::ITexture* texture) override;
        void BindStorageBuffer(uint32_t binding, RHI::IBuffer* buffer, uint32_t offset, uint32_t size) override;
        void SetInlineConstants(uint32_t size, const void* data) override;
        void SetRenderTargets(uint32_t numRenderTargets, RHI::ITexture** renderTargets, RHI::ITexture* depthStencil) override;
        void SetViewport(const RHI::Viewport& viewport) override;
        void SetScissor(const RHI::Rect& rect) override;

        // 배경색 지우기!
        void ClearColor(RHI::ITexture* renderTarget, float r, float g, float b, float a) override;

        void ClearDepth(RHI::ITexture* depthStencil, float depth) override;

        void BindVertexBuffer(RHI::IBuffer* buffer, uint32_t stride, uint32_t offset) override;
        void BindIndexBuffer(RHI::IBuffer* buffer, RHI::Format format, uint32_t offset) override;
        void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance) override;
        void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) override;

        void Close() override;
        // frame context와 swapchain image가 독립적이므로 매 frame 획득한 백버퍼를 연결한다.
        void SetBackBuffer(RHI::ITexture* texture, size_t rtvHandlePtr);

        void* GetNativeList(); // 디바이스가 가져가기 위한 함수

    private:
        D3D12CommandListInternal* m_internal;
    };
}
