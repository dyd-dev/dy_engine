#pragma once

#include "RHI/ICommandList.h"

#include <cstdint>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dy::Backends
{
    struct D3D12ObjectDeleter;
    struct D3D12CommandListInternal;
    class D3D12Buffer;
    class D3D12Texture;

    struct D3D12SubmissionState
    {
        std::unordered_map<D3D12Buffer*, RHI::ResourceState> buffers;
        std::map<std::pair<D3D12Texture*, uint32_t>, RHI::ResourceState>
            textureSubresources;
    };

    class D3D12CommandList final : public RHI::ICommandList
    {
    public:
        explicit D3D12CommandList(void* nativeDevice);

        void ResourceBarrier(const RHI::ResourceBarrierDesc* barriers, uint32_t count) override;
        void BeginRendering(const RHI::RenderingDesc& desc) override;
        void EndRendering() override;

        void BindGraphicsPipeline(RHI::PipelineHandle pipelineState) override;
        void BindResourceSet(RHI::ResourceSetHandle resourceSet) override;
        void BindVertexBuffer(uint32_t binding, RHI::BufferHandle buffer, uint32_t offset) override;
        void BindIndexBuffer(RHI::BufferHandle buffer, RHI::Format format, uint32_t offset) override;
        void SetInlineConstants(uint32_t offset, uint32_t size, const void* data) override;
        void SetStencilReference(uint32_t reference) override;

        void SetViewport(const RHI::Viewport& viewport) override;
        void SetScissor(const RHI::Rect& rect) override;

        void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance) override;
        void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) override;

        void Close() override;

        void* GetNativeList();
        bool IsClosed() const;
        const std::vector<D3D12Texture*>& GetReferencedSwapchainImages() const;
        bool RecordBufferUpload(D3D12Buffer* buffer, uint32_t offset, const void* data, uint32_t size);
        bool RecordTextureUpload(
            D3D12Texture* texture,
            uint32_t mipLevel,
            uint32_t arrayLayer,
            const void* data,
            uint32_t dataSize,
            uint32_t rowPitch,
            uint32_t slicePitch);
        [[nodiscard]] bool ValidateForSubmit(
            D3D12SubmissionState& state) const;
        void CommitResourceStates();

    private:
        friend struct D3D12ObjectDeleter;

        ~D3D12CommandList() override;

        D3D12CommandListInternal* m_internal = nullptr;
    };
}
