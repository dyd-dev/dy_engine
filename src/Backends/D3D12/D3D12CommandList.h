#pragma once

#include "dyf/RHI/ICommandList.h"
#include "D3D12Upload.h"

#include <cstdint>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dyf::Backends
{
    struct D3D12ObjectDeleter;
    struct D3D12CommandListInternal;
    class D3D12Buffer;
    class D3D12Texture;
    struct D3D12CommandStorage;

    class D3D12CommandPoolCache
    {
    public:
        explicit D3D12CommandPoolCache(void* nativeDevice);
        ~D3D12CommandPoolCache();
        std::unique_ptr<D3D12CommandStorage> Acquire();
        void Return(std::unique_ptr<D3D12CommandStorage>) noexcept;
    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

    class D3D12CommandList final : public RHI::ICommandList
    {
    public:
        explicit D3D12CommandList(void* nativeDevice,
            std::shared_ptr<D3D12UploadPool> uploadPool = {},
            void* drawSignature = nullptr,
            void* drawIndexedSignature = nullptr,
            void* dispatchSignature = nullptr,
            RHI::Detail::UploadPolicy policy = {},
            std::shared_ptr<D3D12CommandPoolCache> commandPool = {});

    protected:
        void AllowPoolReuse();
        void ResetTimestampsNative(RHI::TimestampQueryHandle, uint32_t, uint32_t) override;
        void WriteTimestampNative(RHI::TimestampQueryHandle, uint32_t) override;
        void MarkTimestampsSubmitted(uint64_t completion);
        void BeginDebugEventNative(const char*, const RHI::DebugLabelColor&) override;
        void EndDebugEventNative() override;
        void InsertDebugMarkerNative(const char*, const RHI::DebugLabelColor&) override;

        void ResourceBarrierNative(const RHI::ResourceBarrierDesc* barriers, uint32_t count) override;
        void GlobalBarrierNative() override;
        void CopyBufferNative(RHI::BufferHandle source, uint32_t sourceOffset,
            RHI::BufferHandle destination, uint32_t destinationOffset, uint32_t size) override;
        void BeginRenderingNative(const RHI::RenderingDesc& desc) override;
        void EndRenderingNative() override;

        void BindGraphicsPipelineNative(RHI::PipelineHandle pipelineState) override;
        void BindComputePipelineNative(RHI::PipelineHandle pipelineState) override;
        void DispatchNative(uint32_t x, uint32_t y, uint32_t z) override;
        void DispatchIndirectNative(RHI::BufferHandle arguments, uint32_t offset) override;
        void BindResourceSetNative(RHI::ResourceSetHandle resourceSet) override;
        void BindVertexBufferNative(uint32_t binding, RHI::BufferHandle buffer, uint32_t offset) override;
        void BindIndexBufferNative(RHI::BufferHandle buffer, RHI::Format format, uint32_t offset) override;
        void SetInlineConstantsNative(uint32_t offset, uint32_t size, const void* data) override;
        void SetStencilReferenceNative(uint32_t reference) override;

        void SetViewportNative(const RHI::Viewport& viewport) override;
        void SetScissorNative(const RHI::Rect& rect) override;

        void DrawInstancedNative(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance) override;
        void DrawIndexedInstancedNative(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) override;
        void DrawIndirectNative(RHI::BufferHandle arguments, uint32_t offset) override;
        void DrawIndexedIndirectNative(RHI::BufferHandle arguments, uint32_t offset, uint32_t drawCount, uint32_t stride) override;
        void DispatchMeshNative(uint32_t x, uint32_t y, uint32_t z) override;

        bool CloseNative() override;

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
        void CommitResourceStates();

    private:
        friend class D3D12Device;
        friend struct D3D12InternalState;
        friend struct D3D12ObjectDeleter;

        ~D3D12CommandList() override;

        D3D12CommandListInternal* m_internal = nullptr;
    };
}
