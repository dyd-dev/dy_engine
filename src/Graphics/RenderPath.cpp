#include "Graphics/RenderPath.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <utility>
#include <vector>

#include "Graphics/Scene.h"
#include "RHI/IBuffer.h"
#include "RHI/ICommandList.h"
#include "RHI/IDevice.h"
#include "RHI/IPipelineState.h"
#include "RHI/ITexture.h"

using namespace dy;
using namespace dy::Graphics;

namespace Layout = dy::Graphics::RendererShaderLayout;

bool dy::Graphics::TryComputeRendererBufferBytes(
	uint64_t elementCount,
	uint32_t elementStride,
	uint32_t& outBytes)
{
	outBytes = 0u;
	if(elementStride == 0u) return false;
	if(elementCount > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) / elementStride)
		return false;
	outBytes = static_cast<uint32_t>(elementCount * elementStride);
	return true;
}

namespace
{
	bool SceneHasSkinnedEntity(const Scene& scene)
	{
		for(uint32_t entityIndex = 0; entityIndex < scene.GetEntityCount(); ++entityIndex)
			if(scene.IsEntitySkinned(static_cast<EntityID>(entityIndex))) return true;
		return false;
	}

	struct RendererVertex
	{
		float px = 0.0f, py = 0.0f, pz = 0.0f;
		float nx = 0.0f, ny = 0.0f, nz = 1.0f;
		float u = 0.0f, v = 0.0f;
		float tx = 1.0f, ty = 0.0f, tz = 0.0f, tw = 1.0f;
	};

	static_assert(sizeof(RendererVertex) == sizeof(float) * Layout::kRendererVertexFloatCount, "Renderer vertex layout must match shader layout.");

	struct MeshRange
	{
		uint32_t firstVertex = 0;
		uint32_t firstIndex = 0;
		uint32_t indexCount = 0;
	};

	struct InstanceTransform
	{
		Math::float4x4 modelMatrix = Math::float4x4::Identity();
	};

	struct PendingDrawBatch
	{
		uint32_t meshIndex = 0;
		uint32_t materialIndex = 0;
		uint32_t textureFlags = 0;
		std::vector<InstanceTransform> instances;
	};

	struct DrawBatch
	{
		uint32_t meshIndex = 0;
		uint32_t materialIndex = 0;
		uint32_t textureFlags = 0;
		uint32_t firstInstance = 0;
		uint32_t instanceCount = 0;
	};

	[[nodiscard]] uint32_t ComputeTextureFlags(const SceneMaterialState& materialState, const RenderFlags& renderFlags);

	[[nodiscard]] std::vector<RendererVertex> BuildRendererVertices(const MeshData& mesh)
	{
		std::vector<RendererVertex> vertices;
		vertices.reserve(mesh.vertices.size());
		for(const Vertex& vertex : mesh.vertices)
		{
			RendererVertex out = {};
			out.px = vertex.position.x; out.py = vertex.position.y; out.pz = vertex.position.z;
			out.nx = vertex.normal.x;   out.ny = vertex.normal.y;   out.nz = vertex.normal.z;
			out.u = vertex.uv.x;        out.v = vertex.uv.y;
			out.tx = vertex.tangent.x;  out.ty = vertex.tangent.y;  out.tz = vertex.tangent.z; out.tw = vertex.tangent.w;
			vertices.push_back(out);
		}
		return vertices;
	}

	[[nodiscard]] std::vector<uint32_t> BuildRendererIndices(const MeshData& mesh)
	{
		if(!mesh.indices.empty()) return mesh.indices;
		uint32_t vertexCount = 0u;
		if(!TryComputeRendererBufferBytes(mesh.vertices.size(), 1u, vertexCount)) return {};
		std::vector<uint32_t> indices;
		indices.reserve(mesh.vertices.size());
		for(uint32_t i = 0; i < vertexCount; ++i) indices.push_back(i);
		return indices;
	}

	void UploadBuffer(RHI::IBuffer* buffer, const void* source, uint32_t sizeBytes)
	{
		if(buffer == nullptr || source == nullptr || sizeBytes == 0u) return;
		void* data = buffer->Map(0);
		if(data != nullptr)
		{
			std::memcpy(data, source, sizeBytes);
			buffer->Unmap();
		}
	}

	bool EnsureBuffer(
		RHI::IDevice* device,
		RHI::IBuffer*& buffer,
		uint32_t& currentSizeBytes,
		uint32_t sizeBytes,
		uint32_t stride,
		RHI::BufferUsage usage,
		RHI::BufferMemoryUsage memoryUsage = RHI::BufferMemoryUsage::CpuToGpu)
	{
		if(device == nullptr || sizeBytes == 0u) return false;
		if(buffer != nullptr
			&& currentSizeBytes == sizeBytes
			&& buffer->GetStride() == stride
			&& buffer->GetUsage() == usage
			&& buffer->GetMemoryUsage() == memoryUsage) return true;
		if(buffer != nullptr)
		{
			device->DestroyBuffer(buffer);
			buffer = nullptr;
			currentSizeBytes = 0u;
		}
		buffer = device->CreateBuffer(RHI::BufferDesc{ sizeBytes, stride, usage, memoryUsage });
		if(buffer == nullptr) return false;
		currentSizeBytes = sizeBytes;
		return true;
	}

	void DestroyBuffer(RHI::IDevice* device, RHI::IBuffer*& buffer, uint32_t& sizeBytes)
	{
		if(device != nullptr && buffer != nullptr) device->DestroyBuffer(buffer);
		buffer = nullptr;
		sizeBytes = 0u;
	}

	void DestroyBufferRing(
		RHI::IDevice* device,
		std::vector<RHI::IBuffer*>& buffers,
		std::vector<uint32_t>& bufferBytes)
	{
		for(size_t index = 0u; index < buffers.size(); ++index)
		{
			uint32_t sizeBytes = index < bufferBytes.size() ? bufferBytes[index] : 0u;
			DestroyBuffer(device, buffers[index], sizeBytes);
		}
		buffers.clear();
		bufferBytes.clear();
	}

	bool BuildBatchedGeometry(
		const Scene& scene,
		RHI::IDevice* device,
		RHI::IBuffer*& vertexBuffer, uint32_t& vertexBytes,
		RHI::IBuffer*& indexBuffer, uint32_t& indexBytes,
		std::vector<MeshRange>& meshRanges)
	{
		const uint32_t meshCount = scene.GetMeshCount();
		std::vector<RendererVertex> vertices;
		std::vector<uint32_t> indices;
		meshRanges.assign(meshCount, {});
		for(uint32_t meshIndex = 0; meshIndex < meshCount; ++meshIndex)
		{
			const MeshData& mesh = scene.GetMesh(static_cast<MeshID>(meshIndex));
			if(mesh.vertices.empty()) continue;

			std::vector<RendererVertex> meshVertices = BuildRendererVertices(mesh);
			std::vector<uint32_t> meshIndices = BuildRendererIndices(mesh);
			if(meshVertices.empty() || meshIndices.empty()) continue;

			MeshRange& range = meshRanges[meshIndex];
			if(!TryComputeRendererBufferBytes(vertices.size(), 1u, range.firstVertex)
				|| !TryComputeRendererBufferBytes(indices.size(), 1u, range.firstIndex)
				|| !TryComputeRendererBufferBytes(meshIndices.size(), 1u, range.indexCount))
			{
				meshRanges.clear();
				return false;
			}
			vertices.insert(vertices.end(), meshVertices.begin(), meshVertices.end());
			indices.insert(indices.end(), meshIndices.begin(), meshIndices.end());
		}

		uint32_t newVertexBytes = 0u;
		uint32_t newIndexBytes = 0u;
		if(!TryComputeRendererBufferBytes(vertices.size(), static_cast<uint32_t>(sizeof(RendererVertex)), newVertexBytes)
			|| !TryComputeRendererBufferBytes(indices.size(), static_cast<uint32_t>(sizeof(uint32_t)), newIndexBytes))
		{
			meshRanges.clear();
			return false;
		}
		if(EnsureBuffer(device, vertexBuffer, vertexBytes, newVertexBytes, static_cast<uint32_t>(sizeof(RendererVertex)), dy::RHI::BufferUsage::Vertex | dy::RHI::BufferUsage::Storage, RHI::BufferMemoryUsage::GpuOnly))
		{
			UploadBuffer(vertexBuffer, vertices.data(), newVertexBytes);
		}
		if(EnsureBuffer(device, indexBuffer, indexBytes, newIndexBytes, static_cast<uint32_t>(sizeof(uint32_t)), dy::RHI::BufferUsage::Index | RHI::BufferUsage::Storage, RHI::BufferMemoryUsage::GpuOnly))
		{
			UploadBuffer(indexBuffer, indices.data(), newIndexBytes);
		}
		return vertexBuffer != nullptr && indexBuffer != nullptr;
	}

	void AddPendingBatchInstance(
		std::vector<PendingDrawBatch>& batches,
		uint32_t meshIndex,
		uint32_t materialIndex,
		uint32_t textureFlags,
		const Transform& transform)
	{
		for(PendingDrawBatch& batch : batches)
		{
			if(batch.meshIndex == meshIndex &&
				batch.materialIndex == materialIndex &&
				batch.textureFlags == textureFlags)
			{
				batch.instances.push_back(InstanceTransform{ transform.worldMatrix });
				return;
			}
		}

		PendingDrawBatch batch = {};
		batch.meshIndex = meshIndex;
		batch.materialIndex = materialIndex;
		batch.textureFlags = textureFlags;
		batch.instances.push_back(InstanceTransform{ transform.worldMatrix });
		batches.push_back(std::move(batch));
	}

	void FlushPendingBatches(
		const std::vector<PendingDrawBatch>& pendingBatches,
		std::vector<DrawBatch>& drawBatches,
		std::vector<InstanceTransform>& instances)
	{
		drawBatches.clear();
		for(const PendingDrawBatch& pending : pendingBatches)
		{
			if(pending.instances.empty()) continue;

			DrawBatch batch = {};
			batch.meshIndex = pending.meshIndex;
			batch.materialIndex = pending.materialIndex;
			batch.textureFlags = pending.textureFlags;
			batch.firstInstance = static_cast<uint32_t>(instances.size());
			batch.instanceCount = static_cast<uint32_t>(pending.instances.size());
			drawBatches.push_back(batch);
			instances.insert(instances.end(), pending.instances.begin(), pending.instances.end());
		}
	}

	void BuildMainDrawBatches(
		const Scene& scene,
		const RenderPathContext& context,
		const std::vector<MeshRange>& meshRanges,
		std::vector<DrawBatch>& drawBatches,
		std::vector<InstanceTransform>& instances)
	{
		drawBatches.clear();
		instances.clear();
		if(context.materialStates == nullptr) return;

		const std::vector<SceneMaterialState>& materialStates = *context.materialStates;
		std::vector<PendingDrawBatch> pendingBatches;
		const uint32_t entityCount = scene.GetEntityCount();
		for(uint32_t entityIndex = 0; entityIndex < entityCount; ++entityIndex)
		{
			const EntityID entity = static_cast<EntityID>(entityIndex);
			const MeshID meshId = scene.GetEntityMesh(entity);
			const MaterialID materialId = scene.GetEntityMaterial(entity);
			if(!IsValid(meshId) || !IsValid(materialId)) continue;

			const uint32_t meshIndex = ToIndex(meshId);
			if(meshIndex >= meshRanges.size() || meshRanges[meshIndex].indexCount == 0u) continue;

			const uint32_t materialIndex = ToIndex(materialId);
			if(materialIndex >= materialStates.size()) continue;

			const RenderFlags& renderFlags = scene.GetRenderFlags(entity);
			const uint32_t textureFlags = ComputeTextureFlags(materialStates[materialIndex], renderFlags);
			AddPendingBatchInstance(pendingBatches, meshIndex, materialIndex, textureFlags, scene.GetTransform(entity));
		}

		FlushPendingBatches(pendingBatches, drawBatches, instances);
	}

	bool UploadInstanceTransforms(
		RHI::IDevice* device,
		std::vector<RHI::IBuffer*>& instanceBuffers,
		std::vector<uint32_t>& instanceBufferBytes,
		const std::vector<InstanceTransform>& instances,
		RHI::IBuffer*& outActiveBuffer,
		uint32_t& outActiveBufferBytes)
	{
		outActiveBuffer = nullptr;
		outActiveBufferBytes = 0u;
		if(device == nullptr) return false;
		const uint32_t frameCount = std::max(device->GetDesc().maxFramesInFlight, 1u);
		if(instanceBuffers.size() != frameCount)
		{
			DestroyBufferRing(device, instanceBuffers, instanceBufferBytes);
			instanceBuffers.assign(frameCount, nullptr);
			instanceBufferBytes.assign(frameCount, 0u);
		}
		const uint32_t frameIndex = device->GetCurrentFrameIndex() % frameCount;
		uint32_t bytes = 0u;
		if(!TryComputeRendererBufferBytes(instances.size(), static_cast<uint32_t>(sizeof(InstanceTransform)), bytes))
			return false;
		if(bytes == 0u) return false;
		if(!EnsureBuffer(device, instanceBuffers[frameIndex], instanceBufferBytes[frameIndex], bytes, static_cast<uint32_t>(sizeof(InstanceTransform)), RHI::BufferUsage::Storage))
		{
			return false;
		}
		UploadBuffer(instanceBuffers[frameIndex], instances.data(), bytes);
		outActiveBuffer = instanceBuffers[frameIndex];
		outActiveBufferBytes = instanceBufferBytes[frameIndex];
		return true;
	}

	[[nodiscard]] bool ShouldRecordShadow(const RenderPathContext& ctx)
	{
		return ctx.shadowPipeline != nullptr && ctx.shadowDepth != nullptr;
	}

	[[nodiscard]] uint32_t ComputeTextureFlags(const SceneMaterialState& materialState, const RenderFlags& renderFlags)
	{
		uint32_t textureFlags = materialState.textureFlags;
		if(renderFlags.receiveShadow) textureFlags |= Layout::kReceiveShadowFlag;
		if(renderFlags.castShadow) textureFlags |= Layout::kCastShadowFlag;
		return textureFlags;
	}

	[[nodiscard]] Layout::DrawConstants MakeDrawConstants(
		const RendererDesc& config,
		const MaterialDesc& material,
		const SceneMaterialState& materialState,
		const Transform& transform,
		uint32_t textureFlags,
		uint32_t firstIndex,
		int32_t vertexOffset)
	{
		// bindless 디스크립터 인덱스를 float 로 변환(INVALID 은 0; 해당 텍스처 flag 가 꺼져 샘플 안 됨).
		const auto texIndex = [&](uint32_t slot) -> float {
			const uint32_t index = materialState.textureDescriptorIndices[slot];
			return index == kInvalidDescriptorIndex ? 0.0f : static_cast<float>(index);
		};

		Layout::DrawConstants drawConstants = {};
		drawConstants.viewProjectionMatrix = config.viewProjectionMatrix;
		drawConstants.modelMatrix = transform.worldMatrix;
		drawConstants.drawMode = static_cast<float>(textureFlags);
		drawConstants.firstIndex = firstIndex;
		drawConstants.vertexOffset = vertexOffset;
		drawConstants.emissiveColor = Math::float4(
			material.emissiveColor.x,
			material.emissiveColor.y,
			material.emissiveColor.z,
			texIndex(kMaterialEmissiveTextureSlot));
		drawConstants.baseColor = material.baseColor;
		drawConstants.materialParams = Math::float4(material.metallicFactor, material.roughnessFactor, material.normalScale, material.occlusionStrength);
		drawConstants.textureIndices = Math::float4(
			texIndex(kMaterialBaseColorTextureSlot),
			texIndex(kMaterialMetallicRoughnessTextureSlot),
			texIndex(kMaterialNormalTextureSlot),
			texIndex(kMaterialOcclusionTextureSlot));
		return drawConstants;
	}

	// 깊이 전용(그림자) 패스 시작: 컬러 RT 없이 그림자 깊이타겟만 바인딩하고 파이프라인/행렬 세팅.
	// 뷰포트는 백엔드 SetRenderTargets 가 깊이타겟 해상도로 맞춘다.
	void BeginShadowPass(RHI::ICommandList* commandList, const RenderPathContext& ctx)
	{
		commandList->BindGraphicsPipeline(ctx.shadowPipeline);
		commandList->SetRenderTargets(0, nullptr, ctx.shadowDepth);
		commandList->ClearDepth(ctx.shadowDepth, 1.0f);
		commandList->BindGlobalDescriptors();
		if(ctx.shadowMatrixBuffer != nullptr)
		{
			commandList->BindConstantBuffer(Layout::kShadowMatrixBinding, ctx.shadowMatrixBuffer, 0, static_cast<uint32_t>(sizeof(Layout::RendererShadowConstants)));
		}
	}

	// 메인 포워드 패스 시작: 이미 획득한 커맨드 리스트에 백버퍼/깊이/파이프라인/상수버퍼를 바인딩.
	// 그림자 깊이타겟이 있으면 SRV 로 바인딩한다(백엔드가 DEPTH_WRITE→PIXEL_SHADER_RESOURCE 전환).
	[[nodiscard]] bool BeginMainPassOn(RHI::ICommandList* commandList, RHI::ITexture* backBuffer, const RenderPathContext& ctx)
	{
		if(commandList == nullptr || backBuffer == nullptr || ctx.pipeline == nullptr) return false;

		const RendererDesc& config = *ctx.config;
		commandList->SetRenderTargets(1, &backBuffer, ctx.depthStencil);
		commandList->ClearColor(backBuffer, config.clearColor.x, config.clearColor.y, config.clearColor.z, config.clearColor.w);
		if(ctx.depthStencil != nullptr)
		{
			commandList->ClearDepth(ctx.depthStencil, 1.0f);
		}
		commandList->BindGraphicsPipeline(ctx.pipeline);
		commandList->BindGlobalDescriptors();
		if(ctx.lightingBuffer != nullptr)
		{
			commandList->BindConstantBuffer(Layout::kLightingConstantBinding, ctx.lightingBuffer, 0, static_cast<uint32_t>(sizeof(Layout::RendererLightingConstants)));
		}
		if(ctx.shadowMatrixBuffer != nullptr)
		{
			commandList->BindConstantBuffer(Layout::kShadowMatrixBinding, ctx.shadowMatrixBuffer, 0, static_cast<uint32_t>(sizeof(Layout::RendererShadowConstants)));
		}
		if(ctx.shadowDepth != nullptr)
		{
			commandList->BindTexture(Layout::kShadowSamplerBinding, ctx.shadowDepth);
		}
		return true;
	}

	void SubmitMainPass(RHI::IDevice* device, RHI::ICommandList* commandList)
	{
		commandList->Close();
		std::array<RHI::ICommandList*, 1> commandLists = { commandList };
		device->Submit(commandLists.data(), static_cast<uint32_t>(commandLists.size()));
	}

	void BindMaterialTextures(RHI::ICommandList* commandList, const SceneMaterialState& materialState)
	{
		commandList->BindTexture(Layout::kBaseColorTextureBinding, materialState.textures[kMaterialBaseColorTextureSlot]);
		commandList->BindTexture(Layout::kMetallicRoughnessSamplerBinding, materialState.textures[kMaterialMetallicRoughnessTextureSlot]);
		commandList->BindTexture(Layout::kNormalSamplerBinding, materialState.textures[kMaterialNormalTextureSlot]);
		commandList->BindTexture(Layout::kOcclusionSamplerBinding, materialState.textures[kMaterialOcclusionTextureSlot]);
		commandList->BindTexture(Layout::kEmissiveSamplerBinding, materialState.textures[kMaterialEmissiveTextureSlot]);
	}

	// ===== ??Per-draw bind ============================================================
	class PerDrawBindPath final : public IRenderPath
	{
	public:
		void PrepareResources(const Scene& scene, RHI::IDevice* device, const RenderPathContext& context) override;
		void RecordSkinningPass(const Scene& scene, RHI::IDevice* device, const RenderPathContext& context) override;
		void RecordMainPass(const Scene& scene, RHI::IDevice* device, const RenderPathContext& context) override;
		void Shutdown(RHI::IDevice* device) override;

	private:
		struct SceneMeshState
		{
			RHI::IBuffer* vertexBuffer = nullptr;
			RHI::IBuffer* indexBuffer = nullptr;
			RHI::IBuffer* influenceBuffer = nullptr;
			uint32_t vertexBytes = 0;
			uint32_t indexBytes = 0;
			uint32_t influenceBytes = 0;
			uint32_t indexCount = 0;
			bool hasSkinInfluences = false;
			uint64_t sourceRevision = 0u;
		};
		struct EntityMorphState
		{
			std::vector<RHI::IBuffer*> vertexBuffers;
			std::vector<uint32_t> vertexBufferBytes;
			RHI::IBuffer* activeVertexBuffer = nullptr;
		};
		struct EntitySkinningState
		{
			std::vector<RHI::IBuffer*> outputBuffers;
			std::vector<uint32_t> outputBufferBytes;
			RHI::IBuffer* sourceBuffer = nullptr;
			RHI::IBuffer* influenceBuffer = nullptr;
			uint32_t influenceBytes = 0u;
			RHI::IBuffer* activeOutputBuffer = nullptr;
			uint32_t activeOutputBytes = 0u;
			uint32_t vertexCount = 0u;
			uint32_t paletteOffset = 0u;
		};

		void DestroyMeshState(RHI::IDevice* device, SceneMeshState& meshState);
		void DestroyMorphState(RHI::IDevice* device, EntityMorphState& morphState);
		void DestroySkinningState(RHI::IDevice* device, EntitySkinningState& skinningState);
		void RecordComputeSkinning(RHI::ICommandList* commandList);
		[[nodiscard]] bool IsEntityPreSkinned(EntityID entity) const;
		[[nodiscard]] RHI::IBuffer* GetEntityVertexBuffer(
			EntityID entity,
			const SceneMeshState& meshState) const;
		void RecordShadowDraws(RHI::ICommandList* commandList, const Scene& scene, const RenderPathContext& context);

		std::vector<SceneMeshState> m_meshStates;
		std::vector<EntityMorphState> m_entityMorphStates;
		std::vector<EntitySkinningState> m_entitySkinningStates;
		std::vector<RHI::IBuffer*> m_instanceBuffers;
		std::vector<uint32_t> m_instanceBufferBytes;
		std::vector<InstanceTransform> m_instances;
		RHI::IBuffer* m_fallbackInfluenceBuffer = nullptr;
		uint32_t m_fallbackInfluenceBufferBytes = 0;
		std::vector<RHI::IBuffer*> m_paletteBuffers;
		std::vector<uint32_t> m_paletteBufferBytes;
		RHI::IBuffer* m_activePaletteBuffer = nullptr;
		uint32_t m_activePaletteBufferBytes = 0;
		bool m_skinningEnabled = false;
		bool m_computeSkinningEnabled = false;
	};

	void PerDrawBindPath::PrepareResources(const Scene& scene, RHI::IDevice* device, const RenderPathContext& context)
	{
		if(device == nullptr) return;
		m_skinningEnabled = device->SupportsSkinningStorageBindings();
		if(m_skinningEnabled)
		{
			if(m_fallbackInfluenceBuffer == nullptr)
			{
				const SkinInfluence fallbackInfluence = {};
				const uint32_t fallbackInfluenceBytes = static_cast<uint32_t>(sizeof(SkinInfluence));
				if(EnsureBuffer(
					device,
					m_fallbackInfluenceBuffer,
					m_fallbackInfluenceBufferBytes,
					fallbackInfluenceBytes,
					fallbackInfluenceBytes,
					RHI::BufferUsage::Storage,
					RHI::BufferMemoryUsage::GpuOnly))
				{
					UploadBuffer(m_fallbackInfluenceBuffer, &fallbackInfluence, fallbackInfluenceBytes);
				}
			}

			const uint32_t frameCount = std::max(device->GetDesc().maxFramesInFlight, 1u);
			if(m_paletteBuffers.size() != frameCount)
			{
				for(size_t index = 0; index < m_paletteBuffers.size(); ++index)
					DestroyBuffer(device, m_paletteBuffers[index], m_paletteBufferBytes[index]);
				m_paletteBuffers.assign(frameCount, nullptr);
				m_paletteBufferBytes.assign(frameCount, 0u);
			}
			const uint32_t frameIndex = device->GetCurrentFrameIndex() % frameCount;
			const std::vector<SkinJointMatrices>& sourcePalette = scene.JointPaletteMatrices();
			const SkinJointMatrices identity = {};
			const uint64_t paletteCount = sourcePalette.empty() ? 1u : sourcePalette.size();
			uint32_t paletteBytes = 0u;
			if(!TryComputeRendererBufferBytes(
				paletteCount,
				static_cast<uint32_t>(sizeof(SkinJointMatrices)),
				paletteBytes))
			{
				m_activePaletteBuffer = nullptr;
				m_activePaletteBufferBytes = 0u;
				return;
			}
			if(EnsureBuffer(
				device,
				m_paletteBuffers[frameIndex],
				m_paletteBufferBytes[frameIndex],
				paletteBytes,
				static_cast<uint32_t>(sizeof(SkinJointMatrices)),
				RHI::BufferUsage::Storage))
			{
				const void* source = sourcePalette.empty()
					? static_cast<const void*>(&identity)
					: static_cast<const void*>(sourcePalette.data());
				UploadBuffer(m_paletteBuffers[frameIndex], source, paletteBytes);
			}
			m_activePaletteBuffer = m_paletteBuffers[frameIndex];
			m_activePaletteBufferBytes = m_paletteBufferBytes[frameIndex];
		}
		else
		{
			m_activePaletteBuffer = nullptr;
			m_activePaletteBufferBytes = 0u;
		}

		const uint32_t meshCount = scene.GetMeshCount();
		if(m_meshStates.size() < meshCount) m_meshStates.resize(meshCount);

		for(uint32_t meshIndex = 0; meshIndex < meshCount; ++meshIndex)
		{
			const MeshID meshId = static_cast<MeshID>(meshIndex);
			const MeshData& mesh = scene.GetMesh(meshId);
			if(mesh.vertices.empty()) continue;

			SceneMeshState& meshState = m_meshStates[meshIndex];
			const uint64_t sourceRevision = scene.GetMeshRevision(meshId);
			if(meshState.sourceRevision != sourceRevision)
			{
				std::vector<RendererVertex> vertices = BuildRendererVertices(mesh);
				std::vector<uint32_t> indices = BuildRendererIndices(mesh);
				if(vertices.empty() || indices.empty()) continue;
				uint32_t vertexBytes = 0u;
				uint32_t indexBytes = 0u;
				if(!TryComputeRendererBufferBytes(vertices.size(), static_cast<uint32_t>(sizeof(RendererVertex)), vertexBytes)
					|| !TryComputeRendererBufferBytes(indices.size(), static_cast<uint32_t>(sizeof(uint32_t)), indexBytes))
					continue;
				DestroyMeshState(device, meshState);
				const RHI::BufferUsage vertexUsage = RHI::BufferUsage::Vertex | RHI::BufferUsage::Storage;
				const RHI::BufferUsage indexUsage = RHI::BufferUsage::Index | RHI::BufferUsage::Storage;
				meshState.vertexBuffer = device->CreateBuffer(RHI::BufferDesc{ vertexBytes, static_cast<uint32_t>(sizeof(RendererVertex)), vertexUsage, RHI::BufferMemoryUsage::GpuOnly });
				meshState.indexBuffer = device->CreateBuffer(RHI::BufferDesc{ indexBytes, static_cast<uint32_t>(sizeof(uint32_t)), indexUsage, RHI::BufferMemoryUsage::GpuOnly });
				meshState.vertexBytes = vertexBytes;
				meshState.indexBytes = indexBytes;
				if(meshState.vertexBuffer == nullptr || meshState.indexBuffer == nullptr) continue;

				UploadBuffer(meshState.vertexBuffer, vertices.data(), vertexBytes);
				UploadBuffer(meshState.indexBuffer, indices.data(), indexBytes);
				meshState.hasSkinInfluences = m_skinningEnabled && mesh.skinInfluences.size() == mesh.vertices.size();
				if(meshState.hasSkinInfluences)
				{
					uint32_t influenceBytes = 0u;
					if(!TryComputeRendererBufferBytes(
						mesh.skinInfluences.size(),
						static_cast<uint32_t>(sizeof(SkinInfluence)),
						influenceBytes))
					{
						meshState.hasSkinInfluences = false;
						continue;
					}
					if(EnsureBuffer(
						device,
						meshState.influenceBuffer,
						meshState.influenceBytes,
						influenceBytes,
						static_cast<uint32_t>(sizeof(SkinInfluence)),
						RHI::BufferUsage::Storage,
						RHI::BufferMemoryUsage::GpuOnly))
					{
						UploadBuffer(meshState.influenceBuffer, mesh.skinInfluences.data(), influenceBytes);
					}
					else
					{
						meshState.hasSkinInfluences = false;
					}
				}
				else
				{
					DestroyBuffer(device, meshState.influenceBuffer, meshState.influenceBytes);
				}
				if(!TryComputeRendererBufferBytes(indices.size(), 1u, meshState.indexCount))
					meshState.indexCount = 0u;
				meshState.sourceRevision = sourceRevision;
			}
		}

		const uint32_t frameCount = std::max(device->GetDesc().maxFramesInFlight, 1u);
		const uint32_t frameIndex = device->GetCurrentFrameIndex() % frameCount;
		const uint32_t entityCount = scene.GetEntityCount();
		if(m_entityMorphStates.size() < entityCount) m_entityMorphStates.resize(entityCount);
		for(uint32_t entityIndex = 0u; entityIndex < entityCount; ++entityIndex)
		{
			EntityMorphState& morphState = m_entityMorphStates[entityIndex];
			morphState.activeVertexBuffer = nullptr;
			const MeshData* morphedMesh =
				scene.TryGetEntityMorphedMesh(static_cast<EntityID>(entityIndex));
			if(morphedMesh == nullptr || morphedMesh->vertices.empty()) continue;
			if(morphState.vertexBuffers.size() != frameCount)
			{
				DestroyBufferRing(device, morphState.vertexBuffers, morphState.vertexBufferBytes);
				morphState.vertexBuffers.assign(frameCount, nullptr);
				morphState.vertexBufferBytes.assign(frameCount, 0u);
			}
			const std::vector<RendererVertex> vertices = BuildRendererVertices(*morphedMesh);
			uint32_t vertexBytes = 0u;
			if(vertices.empty()
				|| !TryComputeRendererBufferBytes(
					vertices.size(),
					static_cast<uint32_t>(sizeof(RendererVertex)),
					vertexBytes)) continue;
			if(!EnsureBuffer(
				device,
				morphState.vertexBuffers[frameIndex],
				morphState.vertexBufferBytes[frameIndex],
				vertexBytes,
				static_cast<uint32_t>(sizeof(RendererVertex)),
				RHI::BufferUsage::Vertex | RHI::BufferUsage::Storage,
				RHI::BufferMemoryUsage::CpuToGpu)) continue;
			UploadBuffer(morphState.vertexBuffers[frameIndex], vertices.data(), vertexBytes);
			morphState.activeVertexBuffer = morphState.vertexBuffers[frameIndex];
		}

		m_computeSkinningEnabled =
			m_skinningEnabled
			&& context.skinningExecutionMode == SkinningExecutionMode::ComputePreSkin
			&& context.skinningPipeline != nullptr
			&& device->SupportsComputeSkinning();
		if(m_entitySkinningStates.size() < entityCount) m_entitySkinningStates.resize(entityCount);
		for(uint32_t entityIndex = 0u; entityIndex < entityCount; ++entityIndex)
		{
			EntitySkinningState& skinningState = m_entitySkinningStates[entityIndex];
			skinningState.sourceBuffer = nullptr;
			skinningState.influenceBuffer = nullptr;
			skinningState.influenceBytes = 0u;
			skinningState.activeOutputBuffer = nullptr;
			skinningState.activeOutputBytes = 0u;
			skinningState.vertexCount = 0u;
			skinningState.paletteOffset = 0u;
			if(!m_computeSkinningEnabled) continue;

			const EntityID entity = static_cast<EntityID>(entityIndex);
			if(!scene.IsEntitySkinned(entity)) continue;
			const MeshID meshId = scene.GetEntityMesh(entity);
			if(!IsValid(meshId) || ToIndex(meshId) >= m_meshStates.size()) continue;
			const SceneMeshState& meshState = m_meshStates[ToIndex(meshId)];
			if(!meshState.hasSkinInfluences || meshState.influenceBuffer == nullptr || meshState.vertexBuffer == nullptr) continue;

			RHI::IBuffer* sourceBuffer = meshState.vertexBuffer;
			if(entityIndex < m_entityMorphStates.size()
				&& m_entityMorphStates[entityIndex].activeVertexBuffer != nullptr)
			{
				sourceBuffer = m_entityMorphStates[entityIndex].activeVertexBuffer;
			}
			const uint32_t outputBytes = sourceBuffer->GetSize();
			if(outputBytes == 0u || outputBytes % sizeof(RendererVertex) != 0u) continue;
			if(skinningState.outputBuffers.size() != frameCount)
			{
				DestroyBufferRing(device, skinningState.outputBuffers, skinningState.outputBufferBytes);
				skinningState.outputBuffers.assign(frameCount, nullptr);
				skinningState.outputBufferBytes.assign(frameCount, 0u);
			}
			if(!EnsureBuffer(
				device,
				skinningState.outputBuffers[frameIndex],
				skinningState.outputBufferBytes[frameIndex],
				outputBytes,
				static_cast<uint32_t>(sizeof(RendererVertex)),
				RHI::BufferUsage::Vertex | RHI::BufferUsage::Storage,
				RHI::BufferMemoryUsage::GpuOnly)) continue;
			skinningState.sourceBuffer = sourceBuffer;
			skinningState.influenceBuffer = meshState.influenceBuffer;
			skinningState.influenceBytes = meshState.influenceBytes;
			skinningState.activeOutputBuffer = skinningState.outputBuffers[frameIndex];
			skinningState.activeOutputBytes = outputBytes;
			skinningState.vertexCount = outputBytes / static_cast<uint32_t>(sizeof(RendererVertex));
			skinningState.paletteOffset = scene.GetEntitySkinPaletteOffset(entity);
		}
	}

	void PerDrawBindPath::RecordShadowDraws(RHI::ICommandList* commandList, const Scene& scene, const RenderPathContext& context)
	{
		BeginShadowPass(commandList, context);

		const RendererDesc& config = *context.config;
		const std::vector<SceneMaterialState>& materialStates = *context.materialStates;

		const uint32_t entityCount = scene.GetEntityCount();
		for(uint32_t entityIndex = 0; entityIndex < entityCount; ++entityIndex)
		{
			const EntityID entity = static_cast<EntityID>(entityIndex);
			const RenderFlags& renderFlags = scene.GetRenderFlags(entity);
			if(!renderFlags.castShadow) continue;

			const MeshID meshId = scene.GetEntityMesh(entity);
			const MaterialID materialId = scene.GetEntityMaterial(entity);
			if(!IsValid(meshId) || !IsValid(materialId)) continue;

			const uint32_t meshIndex = ToIndex(meshId);
			if(meshIndex >= m_meshStates.size()) continue;
			const SceneMeshState& meshState = m_meshStates[meshIndex];
			if(meshState.vertexBuffer == nullptr || meshState.indexBuffer == nullptr || meshState.indexCount == 0u) continue;

			const uint32_t materialIndex = ToIndex(materialId);
			if(materialIndex >= materialStates.size()) continue;

			const MaterialDesc& material = scene.GetMaterial(materialId);
			const SceneMaterialState& materialState = materialStates[materialIndex];
			const Transform& transform = scene.GetTransform(entity);
			uint32_t textureFlags = ComputeTextureFlags(materialState, renderFlags);
			const bool skinned = m_skinningEnabled && scene.IsEntitySkinned(entity) && meshState.hasSkinInfluences;
			const bool shaderSkinned = skinned && !IsEntityPreSkinned(entity);
			if(shaderSkinned) textureFlags |= Layout::kSkinnedFlag;
			if(m_skinningEnabled)
			{
				RHI::IBuffer* influenceBuffer = meshState.hasSkinInfluences
					? meshState.influenceBuffer
					: m_fallbackInfluenceBuffer;
				const uint32_t influenceBytes = meshState.hasSkinInfluences
					? meshState.influenceBytes
					: m_fallbackInfluenceBufferBytes;
				if(influenceBuffer == nullptr || m_activePaletteBuffer == nullptr) continue;
				commandList->BindStorageBuffer(Layout::kSkinInfluenceStorageBinding, influenceBuffer, 0, influenceBytes);
				commandList->BindStorageBuffer(Layout::kSkinPaletteStorageBinding, m_activePaletteBuffer, 0, m_activePaletteBufferBytes);
			}

			RHI::GeometryBinding geometry = {};
			geometry.vertexBuffer = GetEntityVertexBuffer(entity, meshState);
			geometry.vertexStride = static_cast<uint32_t>(sizeof(RendererVertex));
			geometry.indexBuffer = meshState.indexBuffer;
			geometry.indexFormat = RHI::Format::R32_UINT;
			commandList->BindGeometry(geometry);

			Layout::DrawConstants drawConstants = MakeDrawConstants(config, material, materialState, transform, textureFlags, 0, 0);
			if(shaderSkinned) drawConstants.firstVertex = scene.GetEntitySkinPaletteOffset(entity);
			commandList->SetInlineConstants(sizeof(Layout::DrawConstants), &drawConstants);
			commandList->DrawIndexedInstanced(meshState.indexCount, 1, 0, 0, 0);
		}
	}

	void PerDrawBindPath::RecordMainPass(const Scene& scene, RHI::IDevice* device, const RenderPathContext& context)
	{
		RHI::ICommandList* commandList = device->AcquireCommandList();
		RHI::ITexture* backBuffer = device->GetBackBuffer();
		if(commandList == nullptr || backBuffer == nullptr || context.pipeline == nullptr) return;
		m_instances.assign(1u, InstanceTransform{ Math::float4x4::Identity() });
		RHI::IBuffer* activeInstanceBuffer = nullptr;
		uint32_t activeInstanceBufferBytes = 0u;
		if(UploadInstanceTransforms(
			device,
			m_instanceBuffers,
			m_instanceBufferBytes,
			m_instances,
			activeInstanceBuffer,
			activeInstanceBufferBytes))
		{
			commandList->BindStorageBuffer(
				Layout::kBindlessTransformStorageBinding,
				activeInstanceBuffer,
				0u,
				activeInstanceBufferBytes);
		}

		if(ShouldRecordShadow(context))
		{
			RecordShadowDraws(commandList, scene, context);
		}

		if(!BeginMainPassOn(commandList, backBuffer, context)) return;

		const RendererDesc& config = *context.config;
		const std::vector<SceneMaterialState>& materialStates = *context.materialStates;
		const uint32_t entityCount = scene.GetEntityCount();
		for(uint32_t entityIndex = 0; entityIndex < entityCount; ++entityIndex)
		{
			const EntityID entity = static_cast<EntityID>(entityIndex);
			const MeshID meshId = scene.GetEntityMesh(entity);
			const MaterialID materialId = scene.GetEntityMaterial(entity);
			if(!IsValid(meshId) || !IsValid(materialId)) continue;

			const uint32_t meshIndex = ToIndex(meshId);
			if(meshIndex >= m_meshStates.size()) continue;
			const SceneMeshState& meshState = m_meshStates[meshIndex];
			if(meshState.vertexBuffer == nullptr || meshState.indexBuffer == nullptr || meshState.indexCount == 0u) continue;

			const uint32_t materialIndex = ToIndex(materialId);
			if(materialIndex >= materialStates.size()) continue;

			const MaterialDesc& material = scene.GetMaterial(materialId);
			const SceneMaterialState& materialState = materialStates[materialIndex];
			const Transform& transform = scene.GetTransform(entity);
			const RenderFlags& renderFlags = scene.GetRenderFlags(entity);
			uint32_t textureFlags = ComputeTextureFlags(materialState, renderFlags);
			const bool skinned = m_skinningEnabled && scene.IsEntitySkinned(entity) && meshState.hasSkinInfluences;
			const bool shaderSkinned = skinned && !IsEntityPreSkinned(entity);
			if(shaderSkinned) textureFlags |= Layout::kSkinnedFlag;
			if(m_skinningEnabled)
			{
				RHI::IBuffer* influenceBuffer = meshState.hasSkinInfluences
					? meshState.influenceBuffer
					: m_fallbackInfluenceBuffer;
				const uint32_t influenceBytes = meshState.hasSkinInfluences
					? meshState.influenceBytes
					: m_fallbackInfluenceBufferBytes;
				if(influenceBuffer == nullptr || m_activePaletteBuffer == nullptr) continue;
				commandList->BindStorageBuffer(Layout::kSkinInfluenceStorageBinding, influenceBuffer, 0, influenceBytes);
				commandList->BindStorageBuffer(Layout::kSkinPaletteStorageBinding, m_activePaletteBuffer, 0, m_activePaletteBufferBytes);
			}

			RHI::GeometryBinding geometry = {};
			geometry.vertexBuffer = GetEntityVertexBuffer(entity, meshState);
			geometry.vertexStride = static_cast<uint32_t>(sizeof(RendererVertex));
			geometry.indexBuffer = meshState.indexBuffer;
			geometry.indexFormat = RHI::Format::R32_UINT;
			commandList->BindGeometry(geometry);
			if(!config.enableBindlessTextures)
			{
				BindMaterialTextures(commandList, materialState);
			}

			Layout::DrawConstants drawConstants = MakeDrawConstants(config, material, materialState, transform, textureFlags, 0, 0);
			if(shaderSkinned) drawConstants.firstVertex = scene.GetEntitySkinPaletteOffset(entity);
			commandList->SetInlineConstants(sizeof(Layout::DrawConstants), &drawConstants);
			commandList->DrawIndexedInstanced(meshState.indexCount, 1, 0, 0, 0);
		}

		SubmitMainPass(device, commandList);
	}

	void PerDrawBindPath::RecordSkinningPass(
		const Scene&,
		RHI::IDevice* device,
		const RenderPathContext& context)
	{
		if(!m_computeSkinningEnabled || device == nullptr || context.skinningPipeline == nullptr) return;
		RHI::ICommandList* commandList = device->AcquireCommandList();
		if(commandList == nullptr) return;
		commandList->BindComputePipeline(context.skinningPipeline);
		RecordComputeSkinning(commandList);
	}

	void PerDrawBindPath::DestroyMeshState(RHI::IDevice* device, SceneMeshState& meshState)
	{
		if(meshState.vertexBuffer != nullptr) { device->DestroyBuffer(meshState.vertexBuffer); meshState.vertexBuffer = nullptr; }
		if(meshState.indexBuffer != nullptr) { device->DestroyBuffer(meshState.indexBuffer); meshState.indexBuffer = nullptr; }
		if(meshState.influenceBuffer != nullptr) { device->DestroyBuffer(meshState.influenceBuffer); meshState.influenceBuffer = nullptr; }
		meshState.vertexBytes = 0;
		meshState.indexBytes = 0;
		meshState.influenceBytes = 0;
		meshState.indexCount = 0;
		meshState.hasSkinInfluences = false;
		meshState.sourceRevision = 0u;
	}

	void PerDrawBindPath::DestroyMorphState(RHI::IDevice* device, EntityMorphState& morphState)
	{
		DestroyBufferRing(device, morphState.vertexBuffers, morphState.vertexBufferBytes);
		morphState.activeVertexBuffer = nullptr;
	}

	void PerDrawBindPath::DestroySkinningState(RHI::IDevice* device, EntitySkinningState& skinningState)
	{
		DestroyBufferRing(device, skinningState.outputBuffers, skinningState.outputBufferBytes);
		skinningState.sourceBuffer = nullptr;
		skinningState.influenceBuffer = nullptr;
		skinningState.influenceBytes = 0u;
		skinningState.activeOutputBuffer = nullptr;
		skinningState.activeOutputBytes = 0u;
		skinningState.vertexCount = 0u;
		skinningState.paletteOffset = 0u;
	}

	void PerDrawBindPath::RecordComputeSkinning(RHI::ICommandList* commandList)
	{
		if(commandList == nullptr || m_activePaletteBuffer == nullptr) return;
		struct SkinningDispatchConstants
		{
			uint32_t vertexCount;
			uint32_t paletteOffset;
		};
		for(size_t entityIndex = 0u; entityIndex < m_entitySkinningStates.size(); ++entityIndex)
		{
			const EntitySkinningState& state = m_entitySkinningStates[entityIndex];
			if(state.sourceBuffer == nullptr
				|| state.influenceBuffer == nullptr
				|| state.activeOutputBuffer == nullptr
				|| state.vertexCount == 0u) continue;
			commandList->BindStorageBuffer(0u, state.sourceBuffer, 0u, state.sourceBuffer->GetSize());
			commandList->BindStorageBuffer(1u, state.influenceBuffer, 0u, state.influenceBytes);
			commandList->BindStorageBuffer(2u, m_activePaletteBuffer, 0u, m_activePaletteBufferBytes);
			commandList->BindStorageBuffer(3u, state.activeOutputBuffer, 0u, state.activeOutputBytes);
			const SkinningDispatchConstants constants{ state.vertexCount, state.paletteOffset };
			commandList->SetInlineConstants(static_cast<uint32_t>(sizeof(constants)), &constants);
			commandList->Dispatch((state.vertexCount + 63u) / 64u, 1u, 1u);
			commandList->BufferMemoryBarrier(
				state.activeOutputBuffer,
				RHI::BufferAccess::ComputeShaderWrite,
				RHI::BufferAccess::VertexShaderRead,
				0u,
				state.activeOutputBytes);
		}
	}

	bool PerDrawBindPath::IsEntityPreSkinned(EntityID entity) const
	{
		return IsValid(entity)
			&& ToIndex(entity) < m_entitySkinningStates.size()
			&& m_entitySkinningStates[ToIndex(entity)].activeOutputBuffer != nullptr;
	}

	RHI::IBuffer* PerDrawBindPath::GetEntityVertexBuffer(
		EntityID entity,
		const SceneMeshState& meshState) const
	{
		if(IsValid(entity) && ToIndex(entity) < m_entityMorphStates.size())
		{
			if(ToIndex(entity) < m_entitySkinningStates.size())
			{
				RHI::IBuffer* skinnedBuffer = m_entitySkinningStates[ToIndex(entity)].activeOutputBuffer;
				if(skinnedBuffer != nullptr) return skinnedBuffer;
			}
			RHI::IBuffer* morphedBuffer = m_entityMorphStates[ToIndex(entity)].activeVertexBuffer;
			if(morphedBuffer != nullptr) return morphedBuffer;
		}
		return meshState.vertexBuffer;
	}

	void PerDrawBindPath::Shutdown(RHI::IDevice* device)
	{
		if(device == nullptr) return;
		for(SceneMeshState& meshState : m_meshStates) DestroyMeshState(device, meshState);
		m_meshStates.clear();
		for(EntityMorphState& morphState : m_entityMorphStates) DestroyMorphState(device, morphState);
		m_entityMorphStates.clear();
		for(EntitySkinningState& skinningState : m_entitySkinningStates) DestroySkinningState(device, skinningState);
		m_entitySkinningStates.clear();
		DestroyBufferRing(device, m_instanceBuffers, m_instanceBufferBytes);
		m_instances.clear();
		DestroyBuffer(device, m_fallbackInfluenceBuffer, m_fallbackInfluenceBufferBytes);
		for(size_t index = 0; index < m_paletteBuffers.size(); ++index)
			DestroyBuffer(device, m_paletteBuffers[index], m_paletteBufferBytes[index]);
		m_paletteBuffers.clear();
		m_paletteBufferBytes.clear();
		m_activePaletteBuffer = nullptr;
		m_activePaletteBufferBytes = 0;
		m_skinningEnabled = false;
		m_computeSkinningEnabled = false;
	}

	// ===== ??Batched bind =============================================================
	class BatchedBindPath final : public IRenderPath
	{
	public:
		void PrepareResources(const Scene& scene, RHI::IDevice* device, const RenderPathContext& context) override;
		void RecordMainPass(const Scene& scene, RHI::IDevice* device, const RenderPathContext& context) override;
		void Shutdown(RHI::IDevice* device) override;

	private:
		void RecordShadowDraws(RHI::ICommandList* commandList, const Scene& scene, const RenderPathContext& context);

		RHI::IBuffer* m_vertexBuffer = nullptr;
		RHI::IBuffer* m_indexBuffer = nullptr;
		uint32_t m_vertexBufferBytes = 0;
		uint32_t m_indexBufferBytes = 0;
		std::vector<RHI::IBuffer*> m_instanceBuffers;
		std::vector<uint32_t> m_instanceBufferBytes;
		std::vector<MeshRange> m_meshRanges;
		std::vector<DrawBatch> m_drawBatches;
		std::vector<InstanceTransform> m_instances;
		bool m_warnedSkinnedModels = false;
		uint64_t m_geometryRevision = 0u;
	};

	void BatchedBindPath::PrepareResources(const Scene& scene, RHI::IDevice* device, const RenderPathContext&)
	{
		if(!m_warnedSkinnedModels && SceneHasSkinnedEntity(scene))
		{
			std::cerr << "BatchedBind render path does not support skinned models; rendering bind pose" << std::endl;
			m_warnedSkinnedModels = true;
		}
		if(device == nullptr) return;
		const uint64_t geometryRevision = scene.GetMeshCollectionRevision();
		if(m_geometryRevision != geometryRevision
			&& BuildBatchedGeometry(scene, device, m_vertexBuffer, m_vertexBufferBytes, m_indexBuffer, m_indexBufferBytes, m_meshRanges))
		{
			m_geometryRevision = geometryRevision;
		}
	}

	void BatchedBindPath::RecordShadowDraws(RHI::ICommandList* commandList, const Scene& scene, const RenderPathContext& context)
	{
		BeginShadowPass(commandList, context);

		const RendererDesc& config = *context.config;
		const std::vector<SceneMaterialState>& materialStates = *context.materialStates;

		RHI::GeometryBinding geometry = {};
		geometry.vertexBuffer = m_vertexBuffer;
		geometry.vertexStride = static_cast<uint32_t>(sizeof(RendererVertex));
		geometry.indexBuffer = m_indexBuffer;
		geometry.indexFormat = RHI::Format::R32_UINT;
		commandList->BindGeometry(geometry);

		const uint32_t entityCount = scene.GetEntityCount();
		for(uint32_t entityIndex = 0; entityIndex < entityCount; ++entityIndex)
		{
			const EntityID entity = static_cast<EntityID>(entityIndex);
			const RenderFlags& renderFlags = scene.GetRenderFlags(entity);
			if(!renderFlags.castShadow) continue;

			const MeshID meshId = scene.GetEntityMesh(entity);
			const MaterialID materialId = scene.GetEntityMaterial(entity);
			if(!IsValid(meshId) || !IsValid(materialId)) continue;

			const uint32_t meshIndex = ToIndex(meshId);
			if(meshIndex >= m_meshRanges.size()) continue;
			const MeshRange& range = m_meshRanges[meshIndex];
			if(range.indexCount == 0u) continue;

			const uint32_t materialIndex = ToIndex(materialId);
			if(materialIndex >= materialStates.size()) continue;

			const MaterialDesc& material = scene.GetMaterial(materialId);
			const SceneMaterialState& materialState = materialStates[materialIndex];
			const Transform& transform = scene.GetTransform(entity);
			const uint32_t textureFlags = ComputeTextureFlags(materialState, renderFlags);

			Layout::DrawConstants drawConstants = MakeDrawConstants(
				config, material, materialState, transform, textureFlags, range.firstIndex, static_cast<int32_t>(range.firstVertex));
			commandList->SetInlineConstants(sizeof(Layout::DrawConstants), &drawConstants);
			commandList->DrawIndexedInstanced(range.indexCount, 1, range.firstIndex, static_cast<int32_t>(range.firstVertex), 0);
		}
	}

	void BatchedBindPath::RecordMainPass(const Scene& scene, RHI::IDevice* device, const RenderPathContext& context)
	{
		if(m_vertexBuffer == nullptr || m_indexBuffer == nullptr || m_meshRanges.empty()) return;

		RHI::ICommandList* commandList = device->AcquireCommandList();
		RHI::ITexture* backBuffer = device->GetBackBuffer();
		if(commandList == nullptr || backBuffer == nullptr || context.pipeline == nullptr) return;

		if(ShouldRecordShadow(context))
		{
			RecordShadowDraws(commandList, scene, context);
		}

		if(!BeginMainPassOn(commandList, backBuffer, context)) return;

		RHI::GeometryBinding geometry = {};
		geometry.vertexBuffer = m_vertexBuffer;
		geometry.vertexStride = static_cast<uint32_t>(sizeof(RendererVertex));
		geometry.indexBuffer = m_indexBuffer;
		geometry.indexFormat = RHI::Format::R32_UINT;
		commandList->BindGeometry(geometry);

		BuildMainDrawBatches(scene, context, m_meshRanges, m_drawBatches, m_instances);
		RHI::IBuffer* activeInstanceBuffer = nullptr;
		uint32_t activeInstanceBufferBytes = 0u;
		if(UploadInstanceTransforms(device, m_instanceBuffers, m_instanceBufferBytes, m_instances, activeInstanceBuffer, activeInstanceBufferBytes))
		{
			commandList->BindStorageBuffer(Layout::kBindlessTransformStorageBinding, activeInstanceBuffer, 0, activeInstanceBufferBytes);
			const RendererDesc& config = *context.config;
			const std::vector<SceneMaterialState>& materialStates = *context.materialStates;
			for(const DrawBatch& batch : m_drawBatches)
			{
				if(batch.meshIndex >= m_meshRanges.size() || batch.materialIndex >= materialStates.size()) continue;
				const MeshRange& range = m_meshRanges[batch.meshIndex];
				if(range.indexCount == 0u || batch.instanceCount == 0u) continue;

				const MaterialDesc& material = scene.GetMaterial(static_cast<MaterialID>(batch.materialIndex));
				const SceneMaterialState& materialState = materialStates[batch.materialIndex];
				if(!config.enableBindlessTextures)
				{
					BindMaterialTextures(commandList, materialState);
				}

				Layout::DrawConstants drawConstants = MakeDrawConstants(
					config, material, materialState, Transform{}, batch.textureFlags, range.firstIndex, static_cast<int32_t>(range.firstVertex));
				drawConstants.firstVertex = batch.firstInstance + 1u;
				commandList->SetInlineConstants(sizeof(Layout::DrawConstants), &drawConstants);
				commandList->DrawIndexedInstanced(range.indexCount, batch.instanceCount, range.firstIndex, static_cast<int32_t>(range.firstVertex), 0);
			}
		}

		SubmitMainPass(device, commandList);
	}

	void BatchedBindPath::Shutdown(RHI::IDevice* device)
	{
		if(device == nullptr) return;
		DestroyBuffer(device, m_vertexBuffer, m_vertexBufferBytes);
		DestroyBuffer(device, m_indexBuffer, m_indexBufferBytes);
		DestroyBufferRing(device, m_instanceBuffers, m_instanceBufferBytes);
		m_meshRanges.clear();
		m_drawBatches.clear();
		m_instances.clear();
		m_geometryRevision = 0u;
	}

	// ===== Batched-geometry bindless ==================================================
	class BindlessPath final : public IRenderPath
	{
	public:
		void PrepareResources(const Scene& scene, RHI::IDevice* device, const RenderPathContext& context) override;
		void RecordMainPass(const Scene& scene, RHI::IDevice* device, const RenderPathContext& context) override;
		void Shutdown(RHI::IDevice* device) override;

	private:
		void RecordShadowDraws(RHI::ICommandList* commandList, const Scene& scene, const RenderPathContext& context);

		RHI::IBuffer* m_vertexBuffer = nullptr;
		RHI::IBuffer* m_indexBuffer = nullptr;
		uint32_t m_vertexBufferBytes = 0;
		uint32_t m_indexBufferBytes = 0;
		std::vector<RHI::IBuffer*> m_instanceBuffers;
		std::vector<uint32_t> m_instanceBufferBytes;
		std::vector<MeshRange> m_meshRanges;
		std::vector<DrawBatch> m_drawBatches;
		std::vector<InstanceTransform> m_instances;
		bool m_warnedSkinnedModels = false;
		uint64_t m_geometryRevision = 0u;
	};

	void BindlessPath::PrepareResources(const Scene& scene, RHI::IDevice* device, const RenderPathContext& context)
	{
		(void)context;
		if(!m_warnedSkinnedModels && SceneHasSkinnedEntity(scene))
		{
			std::cerr << "Bindless render path does not support skinned models; rendering bind pose" << std::endl;
			m_warnedSkinnedModels = true;
		}
		if(device == nullptr) return;
		const uint64_t geometryRevision = scene.GetMeshCollectionRevision();
		if(m_geometryRevision != geometryRevision
			&& BuildBatchedGeometry(scene, device, m_vertexBuffer, m_vertexBufferBytes, m_indexBuffer, m_indexBufferBytes, m_meshRanges))
		{
			m_geometryRevision = geometryRevision;
		}
	}

	void BindlessPath::RecordShadowDraws(RHI::ICommandList* commandList, const Scene& scene, const RenderPathContext& context)
	{
		BeginShadowPass(commandList, context);

		const RendererDesc& config = *context.config;
		const std::vector<SceneMaterialState>& materialStates = *context.materialStates;

		RHI::GeometryBinding geometry = {};
		geometry.vertexBuffer = m_vertexBuffer;
		geometry.vertexStride = static_cast<uint32_t>(sizeof(RendererVertex));
		geometry.indexBuffer = m_indexBuffer;
		geometry.indexFormat = RHI::Format::R32_UINT;
		commandList->BindGeometry(geometry);

		const uint32_t entityCount = scene.GetEntityCount();
		for(uint32_t entityIndex = 0; entityIndex < entityCount; ++entityIndex)
		{
			const EntityID entity = static_cast<EntityID>(entityIndex);
			const RenderFlags& renderFlags = scene.GetRenderFlags(entity);
			if(!renderFlags.castShadow) continue;

			const MeshID meshId = scene.GetEntityMesh(entity);
			const MaterialID materialId = scene.GetEntityMaterial(entity);
			if(!IsValid(meshId) || !IsValid(materialId)) continue;

			const uint32_t meshIndex = ToIndex(meshId);
			if(meshIndex >= m_meshRanges.size()) continue;
			const MeshRange& range = m_meshRanges[meshIndex];
			if(range.indexCount == 0u) continue;

			const uint32_t materialIndex = ToIndex(materialId);
			if(materialIndex >= materialStates.size()) continue;

			const MaterialDesc& material = scene.GetMaterial(materialId);
			const SceneMaterialState& materialState = materialStates[materialIndex];
			const Transform& transform = scene.GetTransform(entity);
			const uint32_t textureFlags = ComputeTextureFlags(materialState, renderFlags);

			Layout::DrawConstants drawConstants = MakeDrawConstants(
				config, material, materialState, transform, textureFlags, range.firstIndex, static_cast<int32_t>(range.firstVertex));
			commandList->SetInlineConstants(sizeof(Layout::DrawConstants), &drawConstants);
			commandList->DrawIndexedInstanced(range.indexCount, 1, range.firstIndex, static_cast<int32_t>(range.firstVertex), 0);
		}
	}

	void BindlessPath::RecordMainPass(const Scene& scene, RHI::IDevice* device, const RenderPathContext& context)
	{
		if(m_vertexBuffer == nullptr || m_indexBuffer == nullptr || m_meshRanges.empty()) return;

		RHI::ICommandList* commandList = device->AcquireCommandList();
		RHI::ITexture* backBuffer = device->GetBackBuffer();
		if(commandList == nullptr || backBuffer == nullptr || context.pipeline == nullptr) return;

		if(ShouldRecordShadow(context))
		{
			RecordShadowDraws(commandList, scene, context);
		}

		if(!BeginMainPassOn(commandList, backBuffer, context)) return;

		RHI::GeometryBinding geometry = {};
		geometry.vertexBuffer = m_vertexBuffer;
		geometry.vertexStride = static_cast<uint32_t>(sizeof(RendererVertex));
		geometry.indexBuffer = m_indexBuffer;
		geometry.indexFormat = RHI::Format::R32_UINT;
		commandList->BindGeometry(geometry);

		BuildMainDrawBatches(scene, context, m_meshRanges, m_drawBatches, m_instances);
		RHI::IBuffer* activeInstanceBuffer = nullptr;
		uint32_t activeInstanceBufferBytes = 0u;
		if(UploadInstanceTransforms(device, m_instanceBuffers, m_instanceBufferBytes, m_instances, activeInstanceBuffer, activeInstanceBufferBytes))
		{
			commandList->BindStorageBuffer(Layout::kBindlessTransformStorageBinding, activeInstanceBuffer, 0, activeInstanceBufferBytes);
			const RendererDesc& config = *context.config;
			const std::vector<SceneMaterialState>& materialStates = *context.materialStates;
			for(const DrawBatch& batch : m_drawBatches)
			{
				if(batch.meshIndex >= m_meshRanges.size() || batch.materialIndex >= materialStates.size()) continue;
				const MeshRange& range = m_meshRanges[batch.meshIndex];
				if(range.indexCount == 0u || batch.instanceCount == 0u) continue;

				const MaterialDesc& material = scene.GetMaterial(static_cast<MaterialID>(batch.materialIndex));
				const SceneMaterialState& materialState = materialStates[batch.materialIndex];
				Layout::DrawConstants drawConstants = MakeDrawConstants(
					config, material, materialState, Transform{}, batch.textureFlags, range.firstIndex, static_cast<int32_t>(range.firstVertex));
				drawConstants.firstVertex = batch.firstInstance + 1u;
				commandList->SetInlineConstants(sizeof(Layout::DrawConstants), &drawConstants);
				commandList->DrawIndexedInstanced(range.indexCount, batch.instanceCount, range.firstIndex, static_cast<int32_t>(range.firstVertex), 0);
			}
		}

		SubmitMainPass(device, commandList);
	}

	void BindlessPath::Shutdown(RHI::IDevice* device)
	{
		if(device == nullptr) return;
		DestroyBuffer(device, m_vertexBuffer, m_vertexBufferBytes);
		DestroyBuffer(device, m_indexBuffer, m_indexBufferBytes);
		DestroyBufferRing(device, m_instanceBuffers, m_instanceBufferBytes);
		m_meshRanges.clear();
		m_drawBatches.clear();
		m_instances.clear();
		m_geometryRevision = 0u;
	}
}

namespace dy::Graphics
{
	std::unique_ptr<IRenderPath> CreateRenderPath(RendererBindingMode bindingMode)
	{
		switch(bindingMode)
		{
		case RendererBindingMode::Bindless:
			return std::make_unique<BindlessPath>();
		case RendererBindingMode::BatchedBind:
			return std::make_unique<BatchedBindPath>();
		case RendererBindingMode::PerDrawBind:
		default:
			return std::make_unique<PerDrawBindPath>();
		}
	}
}
