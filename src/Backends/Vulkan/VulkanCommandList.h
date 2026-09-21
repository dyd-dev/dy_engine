#pragma once

#include "dyf/RHI/ICommandList.h"
#include "VulkanContext.h"

#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dyf::Backends
{
	class VulkanBuffer;
	class VulkanPipeline;
	class VulkanResourceSet;
	class VulkanTexture;
	struct VulkanObjectDeleter;

	struct VulkanSubmissionState
	{
		std::unordered_map<VulkanBuffer*, dyf::RHI::ResourceState> buffers;
		std::map<std::pair<VulkanTexture*, uint32_t>, dyf::RHI::ResourceState> textureSubresources;
	};

	class VulkanCommandList final : public dyf::RHI::ICommandList
	{
	public:
		explicit VulkanCommandList(const VulkanContext& context);
        void BeginDebugEventNative(const char*,const RHI::DebugLabelColor&) override;
        void EndDebugEventNative() override;
        void InsertDebugMarkerNative(const char*,const RHI::DebugLabelColor&) override;

		void ResourceBarrierNative(const dyf::RHI::ResourceBarrierDesc* barriers, uint32_t count) override;
        void GlobalBarrierNative() override;
		void BeginRenderingNative(const dyf::RHI::RenderingDesc& desc) override;
		void EndRenderingNative() override;

		void BindComputePipelineNative(RHI::PipelineHandle) override;
        void DispatchNative(uint32_t,uint32_t,uint32_t) override;
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

		[[nodiscard]] VkCommandBuffer GetCommandBuffer() const { return m_commandBuffer; }
		[[nodiscard]] bool IsClosed() const { return m_closed; }
		[[nodiscard]] bool IsValid() const { return m_closed && !m_failed; }
		[[nodiscard]] const std::vector<VulkanTexture*>& GetReferencedSwapchainImages() const
		{
			return m_referencedSwapchainImages;
		}
		[[nodiscard]] bool ValidateForSubmit(VulkanSubmissionState& state) const;
		void CommitResourceStates();
        uint32_t m_maxComputeGroups[3]={};

	private:
        void ResetTimestampsNative(RHI::TimestampQueryHandle,uint32_t,uint32_t) override;
        void WriteTimestampNative(RHI::TimestampQueryHandle,uint32_t) override;
		friend struct VulkanObjectDeleter;

		~VulkanCommandList() override;

		enum class OperationKind : uint8_t
		{
			BufferBarrier,
			TextureBarrier,
			BufferRequirement,
			TextureRequirement,
			BufferWrite,
			TextureWrite
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

		struct StagingAllocation
		{
			VkBuffer buffer = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
		};

		struct VertexBinding
		{
			VulkanBuffer* buffer = nullptr;
			uint32_t offset = 0;
		};

		[[nodiscard]] bool CreateStagingAllocation(const void* data, uint32_t size, StagingAllocation& allocation);
		[[nodiscard]] bool RequireBufferState(VulkanBuffer* buffer, dyf::RHI::ResourceState state);
		[[nodiscard]] bool RequireTextureState(
			VulkanTexture* texture,
			uint32_t mipLevel,
			uint32_t arrayLayer,
			dyf::RHI::ResourceState state);
		[[nodiscard]] bool RequireTextureSubresourcesInState(
			VulkanTexture* texture,
			const dyf::RHI::TextureSubresourceRange& range,
			dyf::RHI::ResourceState state);
		[[nodiscard]] bool ValidateDraw(
			bool indexed,
			uint32_t vertexCount,
			uint32_t instanceCount,
			uint32_t startVertex,
			uint32_t startInstance) const;
		void TrackSwapchainImage(VulkanTexture* texture);
		void Fail() { m_failed = true; }

		VulkanContext m_context;
		VkCommandPool m_commandPool = VK_NULL_HANDLE;
		VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
		VulkanPipeline* m_boundPipeline = nullptr;
		VulkanResourceSet* m_boundResourceSet = nullptr;
		VulkanBuffer* m_indexBuffer = nullptr;
		VulkanTexture* m_depthTexture = nullptr;
		std::vector<StagingAllocation> m_stagingAllocations;
		std::vector<Operation> m_operations;
		std::unordered_map<VulkanBuffer*, dyf::RHI::ResourceState> m_bufferStates;
		std::map<std::pair<VulkanTexture*, uint32_t>, dyf::RHI::ResourceState> m_textureStates;
		std::unordered_map<uint32_t, VertexBinding> m_vertexBindings;
		std::vector<VulkanTexture*> m_referencedSwapchainImages;
		std::vector<dyf::RHI::Format> m_colorFormats;
		std::vector<uint8_t> m_inlineConstantCoverage;
		dyf::RHI::Format m_indexFormat = dyf::RHI::Format::Unknown;
		dyf::RHI::Format m_depthFormat = dyf::RHI::Format::Unknown;
		dyf::RHI::ResourceState m_depthState = dyf::RHI::ResourceState::Undefined;
		uint32_t m_depthMipLevel = 0;
		uint32_t m_depthArrayLayer = 0;
		uint32_t m_indexOffset = 0;
		bool m_rendering = false;
		bool m_stencilConfigured = false;
		bool m_closed = false;
		bool m_failed = false;
		bool m_hasViewport = false;
		bool m_hasScissor = false;
		bool m_hasStencilReference = false;
	};
}
