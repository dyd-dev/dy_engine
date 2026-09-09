#pragma once

#include "RHI/ICommandList.h"
#include "VulkanContext.h"

#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dy::Backends
{
	class VulkanBuffer;
	class VulkanPipeline;
	class VulkanResourceSet;
	class VulkanTexture;
	struct VulkanObjectDeleter;

	struct VulkanSubmissionState
	{
		std::unordered_map<VulkanBuffer*, dy::RHI::ResourceState> buffers;
		std::map<std::pair<VulkanTexture*, uint32_t>, dy::RHI::ResourceState> textureSubresources;
	};

	class VulkanCommandList final : public dy::RHI::ICommandList
	{
	public:
		explicit VulkanCommandList(const VulkanContext& context);

		void ResourceBarrier(const dy::RHI::ResourceBarrierDesc* barriers, uint32_t count) override;
		void BeginRendering(const dy::RHI::RenderingDesc& desc) override;
		void EndRendering() override;

		void BindGraphicsPipeline(dy::RHI::PipelineHandle pipelineState) override;
		void BindResourceSet(dy::RHI::ResourceSetHandle resourceSet) override;
		void BindVertexBuffer(uint32_t binding, dy::RHI::BufferHandle buffer, uint32_t offset) override;
		void BindIndexBuffer(dy::RHI::BufferHandle buffer, dy::RHI::Format format, uint32_t offset) override;
		void SetInlineConstants(uint32_t offset, uint32_t size, const void* data) override;

		void SetViewport(const dy::RHI::Viewport& viewport) override;
		void SetScissor(const dy::RHI::Rect& rect) override;
		void SetStencilReference(uint32_t reference) override;

		void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance) override;
		void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) override;
		void Close() override;

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

	private:
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
			dy::RHI::ResourceState before = dy::RHI::ResourceState::Undefined;
			dy::RHI::ResourceState after = dy::RHI::ResourceState::Undefined;
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
		[[nodiscard]] bool RequireBufferState(VulkanBuffer* buffer, dy::RHI::ResourceState state);
		[[nodiscard]] bool RequireTextureState(
			VulkanTexture* texture,
			uint32_t mipLevel,
			uint32_t arrayLayer,
			dy::RHI::ResourceState state);
		[[nodiscard]] bool RequireTextureSubresourcesInState(
			VulkanTexture* texture,
			const dy::RHI::TextureSubresourceRange& range,
			dy::RHI::ResourceState state);
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
		std::unordered_map<VulkanBuffer*, dy::RHI::ResourceState> m_bufferStates;
		std::map<std::pair<VulkanTexture*, uint32_t>, dy::RHI::ResourceState> m_textureStates;
		std::unordered_map<uint32_t, VertexBinding> m_vertexBindings;
		std::vector<VulkanTexture*> m_referencedSwapchainImages;
		std::vector<dy::RHI::Format> m_colorFormats;
		std::vector<uint8_t> m_inlineConstantCoverage;
		dy::RHI::Format m_indexFormat = dy::RHI::Format::Unknown;
		dy::RHI::Format m_depthFormat = dy::RHI::Format::Unknown;
		dy::RHI::ResourceState m_depthState = dy::RHI::ResourceState::Undefined;
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
