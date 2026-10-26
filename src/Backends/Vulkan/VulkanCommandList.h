#pragma once

#include "dyf/RHI/ICommandList.h"
#include "VulkanContext.h"
#include "VulkanUpload.h"

#include <atomic>
#include <vector>

namespace dyf::Backends
{
	class VulkanDevice;
	class VulkanBuffer;
	class VulkanPipeline;
	class VulkanResourceSet;
	class VulkanTexture;
	class VulkanTimestampQuery;
	struct VulkanObjectDeleter;
    struct VulkanCommandStorage;

    class VulkanCommandPoolCache final
    {
    public:
        explicit VulkanCommandPoolCache(const VulkanContext&);
        ~VulkanCommandPoolCache();
        std::unique_ptr<VulkanCommandStorage> Acquire();
        void Return(std::unique_ptr<VulkanCommandStorage>) noexcept;
        void DisableReuse() noexcept {m_reuseEnabled=false;}
        void MarkDeviceLost() noexcept {m_deviceLost=true;DisableReuse();}
        bool IsDeviceLost() const noexcept {return m_deviceLost;}
    private:
        struct CacheBlock;
        static_assert(std::atomic<CacheBlock*>::is_always_lock_free &&
            std::atomic<VulkanCommandStorage*>::is_always_lock_free,
            "Vulkan command storage requires lock-free ownership atomics.");
        void RequireSuccess(VkResult,const char*);
        VkDevice m_device=VK_NULL_HANDLE;
        uint32_t m_queueFamily=UINT32_MAX;
        std::atomic<bool> m_reuseEnabled{true},m_deviceLost{false};
        std::atomic<CacheBlock*> m_available{nullptr};
    };

	class VulkanCommandList final : public dyf::RHI::ICommandList
	{
	public:
		explicit VulkanCommandList(const VulkanContext& context, std::shared_ptr<VulkanUploadPool> uploadPool = {},
            std::shared_ptr<VulkanCommandPoolCache> commandPools = {},RHI::Detail::UploadPolicy policy = {});
	protected:
        void BeginDebugEventNative(const char*,const RHI::DebugLabelColor&) override;
        void EndDebugEventNative() override;
        void InsertDebugMarkerNative(const char*,const RHI::DebugLabelColor&) override;

		void ResourceBarrierNative(const dyf::RHI::ResourceBarrierDesc* barriers, uint32_t count) override;
        void GlobalBarrierNative() override;
		void BeginRenderingNative(const dyf::RHI::RenderingDesc& desc) override;
		void EndRenderingNative() override;

		void BindComputePipelineNative(RHI::PipelineHandle) override;
        void DispatchNative(uint32_t,uint32_t,uint32_t) override;
        void DispatchIndirectNative(RHI::BufferHandle arguments, uint32_t offset) override;
        void BindGraphicsPipelineNative(dyf::RHI::PipelineHandle pipelineState) override;
		void BindResourceSetNative(dyf::RHI::ResourceSetHandle resourceSet) override;
		void BindVertexBufferNative(uint32_t binding, dyf::RHI::BufferHandle buffer, uint32_t offset) override;
		void BindIndexBufferNative(dyf::RHI::BufferHandle buffer, dyf::RHI::Format format, uint32_t offset) override;
		void SetInlineConstantsNative(uint32_t offset, uint32_t size, const void* data) override;

		void SetViewportNative(const dyf::RHI::Viewport& viewport) override;
		void SetScissorNative(const dyf::RHI::Rect& rect) override;
		void SetStencilReferenceNative(uint32_t reference) override;

		void DrawInstancedNative(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance) override;
		void DrawIndexedInstancedNative(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) override;
		void DrawIndirectNative(RHI::BufferHandle arguments, uint32_t offset) override;
		void DrawIndexedIndirectNative(RHI::BufferHandle arguments, uint32_t offset) override;
		void DispatchMeshNative(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ) override;
		bool CloseNative() override;

		[[nodiscard]] bool RecordBufferUpdate(VulkanBuffer& buffer, uint32_t offset, const void* data, uint32_t size);
		[[nodiscard]] bool RecordTextureUpdate(
			VulkanTexture& texture,
			uint32_t mipLevel,
			uint32_t arrayLayer,
			const void* data,
			uint32_t dataSize,
			uint32_t rowPitch,
			uint32_t slicePitch);

	public:
		[[nodiscard]] VkCommandBuffer GetCommandBuffer() const { return m_commandBuffer; }
		[[nodiscard]] bool IsClosed() const { return m_closed; }
		[[nodiscard]] bool IsValid() const { return m_closed && !m_failed; }
		[[nodiscard]] const std::vector<VulkanTexture*>& GetReferencedSwapchainImages() const
		{
			return m_referencedSwapchainImages;
		}
        [[nodiscard]] VkPipelineStageFlags GetSwapchainAcquireWaitStage(
            VulkanTexture* image,bool firstReference) const;
		void CommitResourceStates();
        void CompleteTimestampResets();
        void AllowPoolReuse() noexcept {m_poolReusable=true;}
        uint32_t m_maxComputeGroups[3]={};

	private:
        void ResetTimestampsNative(RHI::TimestampQueryHandle,uint32_t,uint32_t) override;
        void WriteTimestampNative(RHI::TimestampQueryHandle,uint32_t) override;
		friend class VulkanDevice;
		friend struct VulkanObjectDeleter;

		~VulkanCommandList() override;

		enum class OperationKind : uint8_t
		{
			BufferBarrier,
			TextureBarrier,
			TextureRequirement
		};

		struct Operation
		{
			OperationKind kind = OperationKind::BufferBarrier;
			VulkanBuffer* buffer = nullptr;
			VulkanTexture* texture = nullptr;
			dyf::RHI::ResourceState before = dyf::RHI::ResourceState::Undefined;
			dyf::RHI::ResourceState after = dyf::RHI::ResourceState::Undefined;
			uint32_t mipLevel = 0;
			uint32_t arrayLayer = 0;
		};

		void RecordTextureUsage(VulkanTexture* texture, dyf::RHI::ResourceState state);
		void TrackSwapchainImage(VulkanTexture* texture);
		void Fail() { m_failed = true; }

		VulkanContext m_context;
        std::shared_ptr<VulkanCommandPoolCache> m_commandPools;
        std::unique_ptr<VulkanCommandStorage> m_commandStorage;
        bool m_poolReusable=false;
        uint32_t m_maxColorAttachments=0;
        uint32_t m_maxViewportDimensions[2]={};
        float m_viewportBounds[2]={};
        struct QueryReset { VulkanTimestampQuery* query; uint32_t first, count; };
        std::vector<QueryReset> m_queryResets;
		bool m_supportsCompute = false;
		VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
		VulkanPipeline* m_boundPipeline = nullptr;
		VulkanUploadArena m_uploadArena;
		std::vector<Operation> m_operations;
		std::vector<VulkanTexture*> m_referencedSwapchainImages;
		std::vector<dyf::RHI::Format> m_colorFormats;
		dyf::RHI::Format m_depthFormat = dyf::RHI::Format::Unknown;
		bool m_rendering = false;
		bool m_closed = false;
		bool m_failed = false;
	};
}
