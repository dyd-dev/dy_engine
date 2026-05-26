#pragma once
#include <cstdint>
#include <vector>

#include "RenderContext.h"
#include "RendererResources.h"
#include "Scene.h"
#include "RHI/Enums.h"
#include "RHI/IPipelineState.h"

namespace dy::RHI
{
	class IBuffer;
	class ICommandList;
	class IDevice;
	class ITexture;
}

namespace dy::Graphics
{
	enum class GeometrySubmissionMode
	{
		// Portable default: bind vertex/index buffers through the graphics pipeline input stage.
		IndexedInputAssembler,
		// Reserved for ResourceTable/bindless-capable renderer paths; not implemented by this renderer yet.
		BindlessManualFetch
	};

	struct RendererConfig
	{
		Math::float4 clearColor = Math::float4(0.08f, 0.10f, 0.14f, 1.0f);
		GeometrySubmissionMode geometryMode = GeometrySubmissionMode::IndexedInputAssembler;
	};

	class Renderer
	{
	public:
		Renderer() = default;
		~Renderer() = default;

		bool Initialize(RHI::IDevice* device, const RHI::GraphicsPipelineDesc& mainPipeline, const RendererConfig& config = {});
		void Shutdown(RHI::IDevice* device);
		void Render(const Scene& scene, RHI::IDevice* device);

	private:
		struct RenderItemSoA
		{
			std::vector<uint32_t> meshIndices;
			std::vector<uint32_t> materialIndices;
			std::vector<uint32_t> transformIndices;
			std::vector<uint32_t> vertexCounts;
			std::vector<uint32_t> indexCounts;
			std::vector<uint8_t> indexedFlags;

			void Clear()
			{
				meshIndices.clear();
				materialIndices.clear();
				transformIndices.clear();
				vertexCounts.clear();
				indexCounts.clear();
				indexedFlags.clear();
			}

			void Reserve(uint32_t count)
			{
				meshIndices.reserve(count);
				materialIndices.reserve(count);
				transformIndices.reserve(count);
				vertexCounts.reserve(count);
				indexCounts.reserve(count);
				indexedFlags.reserve(count);
			}

			void Push(uint32_t meshIndex, uint32_t materialIndex, uint32_t transformIndex, uint32_t vertexCount, uint32_t indexCount)
			{
				meshIndices.push_back(meshIndex);
				materialIndices.push_back(materialIndex);
				transformIndices.push_back(transformIndex);
				vertexCounts.push_back(vertexCount);
				indexCounts.push_back(indexCount);
				indexedFlags.push_back(indexCount > 0u ? 1u : 0u);
			}

			[[nodiscard]] uint32_t Size() const { return static_cast<uint32_t>(meshIndices.size()); }
		};

		struct DrawBatch
		{
			uint32_t meshIndex = 0;
			uint32_t materialIndex = 0;
			uint32_t firstItem = 0;
			uint32_t itemCount = 0;
			bool indexed = false;
		};

		bool BuildPipelineStates(RHI::IDevice* device, const RHI::GraphicsPipelineDesc& mainPipeline);
		static bool UploadBuffer(RHI::IBuffer* buffer, const void* data, uint32_t size);
		void ReleaseGpuMeshes(RHI::IDevice* device);
		void EnsureGpuMeshes(const Scene& scene, RHI::IDevice* device);
		void ReleaseGpuTextures(RHI::IDevice* device);
		void EnsureGpuTextures(const Scene& scene, RHI::IDevice* device);
		void EnsureGpuMaterials(const Scene& scene);
		void BuildRenderItems(const Scene& scene);
		void BuildDrawBatches();
		MaterialDrawConstants BuildMaterialConstants(const Scene& scene, const DrawBatch& batch) const;

		RHI::ICommandList* BeginRenderPass(RHI::IDevice* device);
		void RecordIAPass(const Scene& scene, RHI::IDevice* device);
		void RecordBindlessPass(RHI::IDevice* device);

		RendererConfig m_config = {};
		RHI::IPipelineState* m_mainPipeline = nullptr;
		RendererResources m_resources;
		RenderItemSoA m_renderItems;
		std::vector<uint32_t> m_renderOrder;
		std::vector<DrawBatch> m_drawBatches;
	};
}
