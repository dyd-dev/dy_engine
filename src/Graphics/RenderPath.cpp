#include "Graphics/RenderPath.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "Graphics/Scene.h"
#include "RHI/IBuffer.h"
#include "RHI/ICommandList.h"
#include "RHI/IDevice.h"
#include "RHI/GraphicsPipeline.h"
#include "RHI/IResourceSet.h"
#include "RHI/ITexture.h"

using namespace dy;
using namespace dy::Graphics;

namespace Layout = dy::Graphics::RendererShaderLayout;

namespace
{
	using RendererVertex = Layout::RendererVertex;

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
		std::vector<uint32_t> indices;
		indices.reserve(mesh.vertices.size());
		for(uint32_t i = 0; i < static_cast<uint32_t>(mesh.vertices.size()); ++i) indices.push_back(i);
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
		RHI::BufferUsage usage)
	{
		if(device == nullptr || sizeBytes == 0u) return false;
		if(buffer != nullptr && currentSizeBytes == sizeBytes) return true;
		if(buffer != nullptr)
		{
			device->DestroyBuffer(buffer);
			buffer = nullptr;
			currentSizeBytes = 0u;
		}
		buffer = device->CreateBuffer(RHI::BufferDesc{ sizeBytes, stride, usage });
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

	void BuildBatchedGeometry(
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
			range.firstVertex = static_cast<uint32_t>(vertices.size());
			range.firstIndex = static_cast<uint32_t>(indices.size());
			range.indexCount = static_cast<uint32_t>(meshIndices.size());
			vertices.insert(vertices.end(), meshVertices.begin(), meshVertices.end());
			indices.insert(indices.end(), meshIndices.begin(), meshIndices.end());
		}

		const uint32_t newVertexBytes = static_cast<uint32_t>(vertices.size() * sizeof(RendererVertex));
		const uint32_t newIndexBytes = static_cast<uint32_t>(indices.size() * sizeof(uint32_t));
		if(EnsureBuffer(device, vertexBuffer, vertexBytes, newVertexBytes, static_cast<uint32_t>(sizeof(RendererVertex)), dy::RHI::BufferUsage::Vertex))
		{
			UploadBuffer(vertexBuffer, vertices.data(), newVertexBytes);
		}
		if(EnsureBuffer(device, indexBuffer, indexBytes, newIndexBytes, static_cast<uint32_t>(sizeof(uint32_t)), dy::RHI::BufferUsage::Index))
		{
			UploadBuffer(indexBuffer, indices.data(), newIndexBytes);
		}
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
		RHI::IBuffer*& instanceBuffer,
		uint32_t& instanceBufferBytes,
		const std::vector<InstanceTransform>& instances)
	{
		const uint32_t bytes = static_cast<uint32_t>(instances.size() * sizeof(InstanceTransform));
		if(bytes == 0u) return false;
		if(!EnsureBuffer(device, instanceBuffer, instanceBufferBytes, bytes, static_cast<uint32_t>(sizeof(InstanceTransform)), RHI::BufferUsage::Storage))
		{
			return false;
		}
		UploadBuffer(instanceBuffer, instances.data(), bytes);
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
		uint32_t textureFlags)
	{
		// bindless texture array index를 float 로 변환(INVALID 은 0; 해당 텍스처 flag 가 꺼져 샘플 안 됨).
		const auto texIndex = [&](uint32_t slot) -> float {
			const uint32_t index = materialState.textureIndices[slot];
			return index == kInvalidTextureIndex ? 0.0f : static_cast<float>(index);
		};

		Layout::DrawConstants drawConstants = {};
		drawConstants.viewProjectionMatrix = config.viewProjectionMatrix;
		drawConstants.modelMatrix = transform.worldMatrix;
		drawConstants.drawMode = static_cast<float>(textureFlags);
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
		commandList->SetRenderTargets(0, nullptr, ctx.shadowDepth);
		commandList->ClearDepth(ctx.shadowDepth, 1.0f);
		commandList->BindGraphicsPipeline(ctx.shadowPipeline);
		if(ctx.shadowResourceSet != nullptr) commandList->BindResourceSet(ctx.shadowResourceSet);
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
		return true;
	}

	void SubmitMainPass(RHI::IDevice* device, RHI::ICommandList* commandList)
	{
		commandList->Close();
		std::array<RHI::ICommandList*, 1> commandLists = { commandList };
		device->Submit(commandLists.data(), static_cast<uint32_t>(commandLists.size()));
	}

	void BindDrawResources(
		RHI::IDevice* device,
		RHI::ICommandList* commandList,
		const RenderPathContext& context,
		const SceneMaterialState& materialState,
		RHI::IBuffer* instanceBuffer,
		uint32_t instanceBufferBytes)
	{
		RHI::IResourceSet* resourceSet = context.config->enableBindlessTextures
			? context.mainResourceSet
			: materialState.resourceSet;
		if(resourceSet == nullptr) return;
		if(instanceBuffer != nullptr && instanceBufferBytes > 0)
		{
			const RHI::ResourceSetWrite write = {
				0, Layout::kBindlessTransformStorageBinding, 0, instanceBuffer, nullptr, nullptr, 0, instanceBufferBytes
			};
			device->UpdateResourceSet(resourceSet, &write, 1);
		}
		commandList->BindResourceSet(resourceSet);
	}

	// ===== ??Per-draw bind ============================================================
	class PerDrawBindPath final : public IRenderPath
	{
	public:
		void PrepareResources(const Scene& scene, RHI::IDevice* device, const RenderPathContext& context) override;
		void RecordMainPass(const Scene& scene, RHI::IDevice* device, const RenderPathContext& context) override;
		void Shutdown(RHI::IDevice* device) override;

	private:
		struct SceneMeshState
		{
			RHI::IBuffer* vertexBuffer = nullptr;
			RHI::IBuffer* indexBuffer = nullptr;
			uint32_t vertexBytes = 0;
			uint32_t indexBytes = 0;
			uint32_t indexCount = 0;
		};

		void DestroyMeshState(RHI::IDevice* device, SceneMeshState& meshState);
		void RecordShadowDraws(RHI::ICommandList* commandList, const Scene& scene, const RenderPathContext& context);

		std::vector<SceneMeshState> m_meshStates;
		RHI::IBuffer* m_instanceBuffer = nullptr;
		uint32_t m_instanceBufferBytes = 0;
		std::vector<InstanceTransform> m_instances;
	};

	void PerDrawBindPath::PrepareResources(const Scene& scene, RHI::IDevice* device, const RenderPathContext&)
	{
		const uint32_t meshCount = scene.GetMeshCount();
		if(m_meshStates.size() < meshCount) m_meshStates.resize(meshCount);

		for(uint32_t meshIndex = 0; meshIndex < meshCount; ++meshIndex)
		{
			const MeshData& mesh = scene.GetMesh(static_cast<MeshID>(meshIndex));
			if(mesh.vertices.empty()) continue;

			std::vector<RendererVertex> vertices = BuildRendererVertices(mesh);
			std::vector<uint32_t> indices = BuildRendererIndices(mesh);
			if(indices.empty()) continue;

			SceneMeshState& meshState = m_meshStates[meshIndex];
			const uint32_t vertexBytes = static_cast<uint32_t>(vertices.size() * sizeof(RendererVertex));
			const uint32_t indexBytes = static_cast<uint32_t>(indices.size() * sizeof(uint32_t));

			if(meshState.vertexBuffer == nullptr || meshState.indexBuffer == nullptr ||
				meshState.vertexBytes != vertexBytes || meshState.indexBytes != indexBytes)
			{
				DestroyMeshState(device, meshState);
				const RHI::BufferUsage vertexUsage = RHI::BufferUsage::Vertex;
				const RHI::BufferUsage indexUsage = RHI::BufferUsage::Index;
				meshState.vertexBuffer = device->CreateBuffer(RHI::BufferDesc{ vertexBytes, static_cast<uint32_t>(sizeof(RendererVertex)), vertexUsage });
				meshState.indexBuffer = device->CreateBuffer(RHI::BufferDesc{ indexBytes, static_cast<uint32_t>(sizeof(uint32_t)), indexUsage });
				meshState.vertexBytes = vertexBytes;
				meshState.indexBytes = indexBytes;
			}
			if(meshState.vertexBuffer == nullptr || meshState.indexBuffer == nullptr) continue;

			UploadBuffer(meshState.vertexBuffer, vertices.data(), vertexBytes);
			UploadBuffer(meshState.indexBuffer, indices.data(), indexBytes);
			meshState.indexCount = static_cast<uint32_t>(indices.size());
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
			const uint32_t textureFlags = ComputeTextureFlags(materialState, renderFlags);

			commandList->BindVertexBuffer(0, meshState.vertexBuffer, 0);
			commandList->BindIndexBuffer(meshState.indexBuffer, RHI::Format::R32_UINT, 0);

			Layout::DrawConstants drawConstants = MakeDrawConstants(config, material, materialState, transform, textureFlags);
			commandList->SetInlineConstants(0, sizeof(Layout::DrawConstants), &drawConstants);
			commandList->DrawIndexedInstanced(meshState.indexCount, 1, 0, 0, 0);
		}
	}

	void PerDrawBindPath::RecordMainPass(const Scene& scene, RHI::IDevice* device, const RenderPathContext& context)
	{
		RHI::ICommandList* commandList = device->AcquireCommandList();
		RHI::ITexture* backBuffer = device->GetBackBuffer();
		if(commandList == nullptr || backBuffer == nullptr || context.pipeline == nullptr) return;

		if(ShouldRecordShadow(context))
		{
			RecordShadowDraws(commandList, scene, context);
		}

		if(!BeginMainPassOn(commandList, backBuffer, context)) return;

		const RendererDesc& config = *context.config;
		const std::vector<SceneMaterialState>& materialStates = *context.materialStates;
		m_instances.assign(1, InstanceTransform{ Math::float4x4::Identity() });
		UploadInstanceTransforms(device, m_instanceBuffer, m_instanceBufferBytes, m_instances);

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
			const uint32_t textureFlags = ComputeTextureFlags(materialState, renderFlags);

			commandList->BindVertexBuffer(0, meshState.vertexBuffer, 0);
			commandList->BindIndexBuffer(meshState.indexBuffer, RHI::Format::R32_UINT, 0);
			BindDrawResources(device, commandList, context, materialState, m_instanceBuffer, m_instanceBufferBytes);

			Layout::DrawConstants drawConstants = MakeDrawConstants(config, material, materialState, transform, textureFlags);
			commandList->SetInlineConstants(0, sizeof(Layout::DrawConstants), &drawConstants);
			commandList->DrawIndexedInstanced(meshState.indexCount, 1, 0, 0, 0);
		}

		SubmitMainPass(device, commandList);
	}

	void PerDrawBindPath::DestroyMeshState(RHI::IDevice* device, SceneMeshState& meshState)
	{
		if(meshState.vertexBuffer != nullptr) { device->DestroyBuffer(meshState.vertexBuffer); meshState.vertexBuffer = nullptr; }
		if(meshState.indexBuffer != nullptr) { device->DestroyBuffer(meshState.indexBuffer); meshState.indexBuffer = nullptr; }
		meshState.vertexBytes = 0;
		meshState.indexBytes = 0;
		meshState.indexCount = 0;
	}

	void PerDrawBindPath::Shutdown(RHI::IDevice* device)
	{
		if(device == nullptr) return;
		for(SceneMeshState& meshState : m_meshStates) DestroyMeshState(device, meshState);
		m_meshStates.clear();
		DestroyBuffer(device, m_instanceBuffer, m_instanceBufferBytes);
		m_instances.clear();
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
		RHI::IBuffer* m_instanceBuffer = nullptr;
		uint32_t m_vertexBufferBytes = 0;
		uint32_t m_indexBufferBytes = 0;
		uint32_t m_instanceBufferBytes = 0;
		std::vector<MeshRange> m_meshRanges;
		std::vector<DrawBatch> m_drawBatches;
		std::vector<InstanceTransform> m_instances;
	};

	void BatchedBindPath::PrepareResources(const Scene& scene, RHI::IDevice* device, const RenderPathContext&)
	{
		BuildBatchedGeometry(scene, device, m_vertexBuffer, m_vertexBufferBytes, m_indexBuffer, m_indexBufferBytes, m_meshRanges);
	}

	void BatchedBindPath::RecordShadowDraws(RHI::ICommandList* commandList, const Scene& scene, const RenderPathContext& context)
	{
		BeginShadowPass(commandList, context);

		const RendererDesc& config = *context.config;
		const std::vector<SceneMaterialState>& materialStates = *context.materialStates;

		commandList->BindVertexBuffer(0, m_vertexBuffer, 0);
		commandList->BindIndexBuffer(m_indexBuffer, RHI::Format::R32_UINT, 0);

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
				config, material, materialState, transform, textureFlags);
			commandList->SetInlineConstants(0, sizeof(Layout::DrawConstants), &drawConstants);
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

		commandList->BindVertexBuffer(0, m_vertexBuffer, 0);
		commandList->BindIndexBuffer(m_indexBuffer, RHI::Format::R32_UINT, 0);

		BuildMainDrawBatches(scene, context, m_meshRanges, m_drawBatches, m_instances);
		if(UploadInstanceTransforms(device, m_instanceBuffer, m_instanceBufferBytes, m_instances))
		{
			const RendererDesc& config = *context.config;
			const std::vector<SceneMaterialState>& materialStates = *context.materialStates;
			for(const DrawBatch& batch : m_drawBatches)
			{
				if(batch.meshIndex >= m_meshRanges.size() || batch.materialIndex >= materialStates.size()) continue;
				const MeshRange& range = m_meshRanges[batch.meshIndex];
				if(range.indexCount == 0u || batch.instanceCount == 0u) continue;

				const MaterialDesc& material = scene.GetMaterial(static_cast<MaterialID>(batch.materialIndex));
				const SceneMaterialState& materialState = materialStates[batch.materialIndex];
				BindDrawResources(device, commandList, context, materialState, m_instanceBuffer, m_instanceBufferBytes);

				Layout::DrawConstants drawConstants = MakeDrawConstants(
					config, material, materialState, Transform{}, batch.textureFlags);
				drawConstants.instanceTransformOffset = batch.firstInstance + 1u;
				commandList->SetInlineConstants(0, sizeof(Layout::DrawConstants), &drawConstants);
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
		DestroyBuffer(device, m_instanceBuffer, m_instanceBufferBytes);
		m_meshRanges.clear();
		m_drawBatches.clear();
		m_instances.clear();
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
		RHI::IBuffer* m_instanceBuffer = nullptr;
		uint32_t m_vertexBufferBytes = 0;
		uint32_t m_indexBufferBytes = 0;
		uint32_t m_instanceBufferBytes = 0;
		std::vector<MeshRange> m_meshRanges;
		std::vector<DrawBatch> m_drawBatches;
		std::vector<InstanceTransform> m_instances;
	};

	void BindlessPath::PrepareResources(const Scene& scene, RHI::IDevice* device, const RenderPathContext& context)
	{
		(void)context;
		BuildBatchedGeometry(scene, device, m_vertexBuffer, m_vertexBufferBytes, m_indexBuffer, m_indexBufferBytes, m_meshRanges);
	}

	void BindlessPath::RecordShadowDraws(RHI::ICommandList* commandList, const Scene& scene, const RenderPathContext& context)
	{
		BeginShadowPass(commandList, context);

		const RendererDesc& config = *context.config;
		const std::vector<SceneMaterialState>& materialStates = *context.materialStates;

		commandList->BindVertexBuffer(0, m_vertexBuffer, 0);
		commandList->BindIndexBuffer(m_indexBuffer, RHI::Format::R32_UINT, 0);

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
				config, material, materialState, transform, textureFlags);
			commandList->SetInlineConstants(0, sizeof(Layout::DrawConstants), &drawConstants);
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

		commandList->BindVertexBuffer(0, m_vertexBuffer, 0);
		commandList->BindIndexBuffer(m_indexBuffer, RHI::Format::R32_UINT, 0);

		BuildMainDrawBatches(scene, context, m_meshRanges, m_drawBatches, m_instances);
		if(UploadInstanceTransforms(device, m_instanceBuffer, m_instanceBufferBytes, m_instances))
		{
			const RendererDesc& config = *context.config;
			const std::vector<SceneMaterialState>& materialStates = *context.materialStates;
			for(const DrawBatch& batch : m_drawBatches)
			{
				if(batch.meshIndex >= m_meshRanges.size() || batch.materialIndex >= materialStates.size()) continue;
				const MeshRange& range = m_meshRanges[batch.meshIndex];
				if(range.indexCount == 0u || batch.instanceCount == 0u) continue;

				const MaterialDesc& material = scene.GetMaterial(static_cast<MaterialID>(batch.materialIndex));
				const SceneMaterialState& materialState = materialStates[batch.materialIndex];
				BindDrawResources(device, commandList, context, materialState, m_instanceBuffer, m_instanceBufferBytes);
				Layout::DrawConstants drawConstants = MakeDrawConstants(
					config, material, materialState, Transform{}, batch.textureFlags);
				drawConstants.instanceTransformOffset = batch.firstInstance + 1u;
				commandList->SetInlineConstants(0, sizeof(Layout::DrawConstants), &drawConstants);
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
		DestroyBuffer(device, m_instanceBuffer, m_instanceBufferBytes);
		m_meshRanges.clear();
		m_drawBatches.clear();
		m_instances.clear();
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
