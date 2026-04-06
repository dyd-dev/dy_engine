#pragma once
#include "RHI/ICommandList.h"

namespace dy::Backends
{
    struct D3D12CommandListInternal;

    class D3D12CommandList : public RHI::ICommandList
    {
    public:
        // 디바이스, 백버퍼 리소스, RTV 핸들 주소를 받습니다.
        D3D12CommandList(void* nativeDevice, void* nativeBackBuffer, size_t rtvHandlePtr);
        ~D3D12CommandList() override;

        void SetConstantBuffer(uint32_t index, RHI::IBuffer* buffer) override;
        void BindGraphicsPipeline(RHI::IPipelineState* pipelineState) override;
        void BindIndexBuffer(RHI::IBuffer* buffer, RHI::Format format, uint32_t offset) override;
        void SetPushConstants(uint32_t size, const void* data) override;
        void SetRenderTargets(uint32_t numRenderTargets, RHI::ITexture** renderTargets, RHI::ITexture* depthStencil) override;

        // 배경색 지우기!
        void ClearColor(RHI::ITexture* renderTarget, float r, float g, float b, float a) override;

        void ClearDepth(RHI::ITexture* depthStencil, float depth) override;
		void BindVertexBuffer(RHI::IBuffer* buffer) override; // 커맨드리스트에서 버퍼를 바인딩하는 함수
		void SetViewport(float width, float height) override;
        void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance) override;
        void ResourceBarrier(RHI::IBuffer* buffer, RHI::ResourceState before, RHI::ResourceState after) override;
        void ResourceBarrier(RHI::ITexture* texture, RHI::ResourceState before, RHI::ResourceState after) override;
        void Close() override;

        void* GetNativeList(); // 디바이스가 가져가기 위한 함수

    private:
        D3D12CommandListInternal* m_internal;
    };
}