#include "Graphics/Private/DrawData.h"

#include <array>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "Graphics/Private/RendererShaderLayout.h"
#include "Graphics/Scene.h"
#include "RHI/Buffer.h"
#include "RHI/ICommandList.h"
#include "RHI/IDevice.h"
#include "RHI/Pipeline.h"
#include "RHI/ResourceSet.h"
#include "RHI/Texture.h"

using namespace dy;
using namespace dy::Graphics;
using namespace dy::Graphics::Private;

namespace Layout = dy::Graphics::Private::RendererShaderLayout;

namespace
{
	using RendererVertex = Layout::RendererVertex;

	struct PendingMeshUpload
	{
		uint32_t meshIndex = 0;
		std::vector<RendererVertex> vertices;
		std::vector<uint32_t> indices;
	};

	[[nodiscard]] std::vector<RendererVertex> BuildRendererVertices(const MeshData& mesh)
	{
		std::vector<RendererVertex> vertices;
		vertices.reserve(mesh.vertices.size());
		for(const Vertex& vertex : mesh.vertices)
		{
			RendererVertex out = {};
			out.px = vertex.position.x;
			out.py = vertex.position.y;
			out.pz = vertex.position.z;
			out.nx = vertex.normal.x;
			out.ny = vertex.normal.y;
			out.nz = vertex.normal.z;
			out.u = vertex.uv.x;
			out.v = vertex.uv.y;
			out.tx = vertex.tangent.x;
			out.ty = vertex.tangent.y;
			out.tz = vertex.tangent.z;
			out.tw = vertex.tangent.w;
			vertices.push_back(out);
		}
		return vertices;
	}

	[[nodiscard]] std::vector<uint32_t> BuildRendererIndices(const MeshData& mesh)
	{
		if(!mesh.indices.empty()) return mesh.indices;

		std::vector<uint32_t> indices;
		indices.reserve(mesh.vertices.size());
		for(uint32_t index = 0; index < static_cast<uint32_t>(mesh.vertices.size()); ++index)
		{
			indices.push_back(index);
		}
		return indices;
	}

	void DestroyBuffer(RHI::IDevice* device, RHI::BufferHandle& buffer, bool& ready)
	{
		if(device != nullptr && buffer != nullptr) device->DestroyBuffer(buffer);
		buffer = nullptr;
		ready = false;
	}

	[[nodiscard]] bool EnsureBuffer(
		RHI::IDevice* device,
		RHI::BufferHandle& buffer,
		bool& ready,
		uint32_t sizeBytes,
		uint32_t stride,
		RHI::BufferUsage usage)
	{
		if(device == nullptr) return false;
		if(sizeBytes == 0)
		{
			DestroyBuffer(device, buffer, ready);
			return true;
		}
		if(buffer != nullptr && buffer->GetDesc().size == sizeBytes) return true;

		DestroyBuffer(device, buffer, ready);
		buffer = device->CreateBuffer(RHI::BufferDesc{
			sizeBytes,
			stride,
			usage,
			RHI::ResourceState::CopyDestination
		});
		if(buffer == nullptr) return false;
		return true;
	}

	[[nodiscard]] bool RecordBufferUpload(
		RHI::IDevice* device,
		RHI::ICommandList& commandList,
		RHI::BufferHandle buffer,
		const void* data,
		uint32_t sizeBytes,
		RHI::ResourceState useState)
	{
		if(device == nullptr || buffer == nullptr || data == nullptr || sizeBytes == 0) return false;
		if(!device->UpdateBuffer(commandList, buffer, 0, data, sizeBytes)) return false;

		const RHI::ResourceBarrierDesc afterCopy = {
			buffer,
			nullptr,
			RHI::ResourceState::CopyDestination,
			useState,
			{}
		};
		commandList.ResourceBarrier(&afterCopy, 1);
		return true;
	}

	void DestroyResourceSets(RHI::IDevice* device, std::vector<RHI::ResourceSetHandle>& resourceSets)
	{
		if(device != nullptr)
		{
			for(RHI::ResourceSetHandle resourceSet : resourceSets)
			{
				if(resourceSet != nullptr) device->DestroyResourceSet(resourceSet);
			}
		}
		resourceSets.clear();
	}

	[[nodiscard]] bool CreateMaterialResourceSets(
		RHI::IDevice* device,
		const DrawContext& context,
		RHI::BufferHandle influences,
		RHI::BufferHandle palette,
		std::vector<RHI::ResourceSetHandle>& resourceSets)
	{
		if(device == nullptr || context.pipeline == nullptr || context.materialStates == nullptr ||
			context.lightingBuffer == nullptr) return false;
		const bool shadowsEnabled = context.shadowDepth != nullptr;
		if(shadowsEnabled && context.shadowMatrixBuffer == nullptr) return false;

		const std::vector<SceneMaterialState>& materials = *context.materialStates;
		resourceSets.resize(materials.size(), nullptr);

		for(uint32_t materialIndex = 0; materialIndex < static_cast<uint32_t>(materials.size()); ++materialIndex)
		{
			const SceneMaterialState& material = materials[materialIndex];
			const std::array<RHI::ResourceBinding, 10> bindings = {{
				{ RENDERER_BINDING_BASE_COLOR_TEXTURE, 0, nullptr, material.textures[static_cast<uint32_t>(MaterialTextureKind::BaseColor)], 0, 0, {} },
				{ RENDERER_BINDING_LIGHTING_CONSTANTS, 0, context.lightingBuffer, nullptr, 0, static_cast<uint32_t>(sizeof(Layout::RendererLightingConstants)), {} },
				{ RENDERER_BINDING_METALLIC_ROUGHNESS_TEXTURE, 0, nullptr, material.textures[static_cast<uint32_t>(MaterialTextureKind::MetallicRoughness)], 0, 0, {} },
				{ RENDERER_BINDING_NORMAL_TEXTURE, 0, nullptr, material.textures[static_cast<uint32_t>(MaterialTextureKind::Normal)], 0, 0, {} },
				{ RENDERER_BINDING_OCCLUSION_TEXTURE, 0, nullptr, material.textures[static_cast<uint32_t>(MaterialTextureKind::Occlusion)], 0, 0, {} },
				{ RENDERER_BINDING_EMISSIVE_TEXTURE, 0, nullptr, material.textures[static_cast<uint32_t>(MaterialTextureKind::Emissive)], 0, 0, {} },
				{ RENDERER_BINDING_SKIN_INFLUENCES, 0, influences, nullptr, 0, influences->GetDesc().size, {} },
				{ RENDERER_BINDING_SKIN_PALETTE, 0, palette, nullptr, 0, palette->GetDesc().size, {} },
				{ RENDERER_BINDING_SHADOW_MATRIX, 0, context.shadowMatrixBuffer, nullptr, 0, static_cast<uint32_t>(sizeof(Layout::RendererShadowConstants)), {} },
				{ RENDERER_BINDING_SHADOW_TEXTURE, 0, nullptr, context.shadowDepth, 0, 0, {} }
			}};
			resourceSets[materialIndex] = device->CreateResourceSet(RHI::ResourceSetDesc{
				context.pipeline,
				bindings.data(),
				shadowsEnabled
					? static_cast<uint32_t>(bindings.size())
					: static_cast<uint32_t>(bindings.size() - 2)
			});
			if(resourceSets[materialIndex] == nullptr)
			{
				DestroyResourceSets(device, resourceSets);
				return false;
			}
		}
		return true;
	}

	[[nodiscard]] uint32_t ComputeTextureFlags(const SceneMaterialState& material, const RenderFlags& renderFlags)
	{
		uint32_t flags = material.textureFlags;
		if(renderFlags.receiveShadow) flags |= RENDERER_TEXTURE_FLAG_RECEIVE_SHADOW;
		return flags;
	}

	[[nodiscard]] Layout::DrawConstants MakeDrawConstants(
		const DrawContext& context,
		const MaterialDesc& material,
		const Transform& transform,
		uint32_t textureFlags)
	{
		Layout::DrawConstants constants = {};
		constants.viewProjectionMatrix = context.viewProjection;
		constants.modelMatrix = transform.worldMatrix;
		constants.textureFlags = textureFlags;
		constants.emissiveColor = Math::float4(
			material.emissiveColor.x,
			material.emissiveColor.y,
			material.emissiveColor.z,
			0.0f);
		constants.baseColor = material.baseColor;
		constants.materialParams = Math::float4(
			material.metallicFactor,
			material.roughnessFactor,
			material.normalScale,
			material.occlusionStrength);
		return constants;
	}

	[[nodiscard]] bool BeginShadowRendering(
		RHI::ICommandList& commandList,
		const DrawContext& context,
		RHI::ResourceSetHandle resourceSet)
	{
		if(context.shadowPipeline == nullptr || context.shadowDepth == nullptr || resourceSet == nullptr) return false;
		const uint32_t shadowMapResolution = context.shadowDepth->GetDesc().width;
		if(context.shadowDepthState != RHI::ResourceState::DepthWrite)
		{
			const RHI::ResourceBarrierDesc barrier = {
				nullptr,
				context.shadowDepth,
				context.shadowDepthState,
				RHI::ResourceState::DepthWrite,
				{}
			};
			commandList.ResourceBarrier(&barrier, 1);
		}

		RHI::DepthStencilAttachment depth = {};
		depth.texture = context.shadowDepth;
		depth.state = RHI::ResourceState::DepthWrite;
		depth.depthLoadOp = RHI::LoadOp::Clear;
		depth.depthStoreOp = RHI::StoreOp::Store;
		depth.clearDepth = 1.0f;
		if(context.shadowDepth->GetDesc().format == RHI::Format::D24_UNORM_S8_UINT)
		{
			depth.stencilLoadOp = RHI::LoadOp::Discard;
			depth.stencilStoreOp = RHI::StoreOp::Discard;
		}
		commandList.BeginRendering(RHI::RenderingDesc{ nullptr, 0, &depth });
		commandList.BindGraphicsPipeline(context.shadowPipeline);
		commandList.BindResourceSet(resourceSet);
		commandList.SetViewport(RHI::Viewport{
			0.0f,
			0.0f,
			static_cast<float>(shadowMapResolution),
			static_cast<float>(shadowMapResolution),
			0.0f,
			1.0f
		});
		commandList.SetScissor(RHI::Rect{ 0, 0, shadowMapResolution, shadowMapResolution });
		return true;
	}

	void EndShadowRendering(RHI::ICommandList& commandList, RHI::TextureHandle shadowDepth)
	{
		commandList.EndRendering();
		const RHI::ResourceBarrierDesc barrier = {
			nullptr,
			shadowDepth,
			RHI::ResourceState::DepthWrite,
			RHI::ResourceState::ShaderResource,
			{}
		};
		commandList.ResourceBarrier(&barrier, 1);
	}

	[[nodiscard]] bool BeginMainRendering(
		RHI::ICommandList& commandList,
		RHI::TextureHandle backBuffer,
		const DrawContext& context,
		bool shadowSubmitted)
	{
		if(backBuffer == nullptr || context.pipeline == nullptr) return false;

		std::array<RHI::ResourceBarrierDesc, 3> barriers = {};
		uint32_t barrierCount = 0;
		barriers[barrierCount++] = {
			nullptr,
			backBuffer,
			RHI::ResourceState::Present,
			RHI::ResourceState::RenderTarget,
			{}
		};
		if(context.depthStencil != nullptr && context.depthStencilState != RHI::ResourceState::DepthWrite)
		{
			barriers[barrierCount++] = {
				nullptr,
				context.depthStencil,
				context.depthStencilState,
				RHI::ResourceState::DepthWrite,
				{}
			};
		}
		if(context.shadowDepth != nullptr && !shadowSubmitted &&
			context.shadowDepthState != RHI::ResourceState::ShaderResource)
		{
			barriers[barrierCount++] = {
				nullptr,
				context.shadowDepth,
				context.shadowDepthState,
				RHI::ResourceState::ShaderResource,
				{}
			};
		}
		commandList.ResourceBarrier(barriers.data(), barrierCount);

		RHI::ColorAttachment color = {};
		color.texture = backBuffer;
		color.loadOp = RHI::LoadOp::Clear;
		color.storeOp = RHI::StoreOp::Store;
		color.clearColor[0] = context.clearColor.x;
		color.clearColor[1] = context.clearColor.y;
		color.clearColor[2] = context.clearColor.z;
		color.clearColor[3] = context.clearColor.w;

		RHI::DepthStencilAttachment depth = {};
		depth.texture = context.depthStencil;
		depth.state = RHI::ResourceState::DepthWrite;
		depth.depthLoadOp = RHI::LoadOp::Clear;
		depth.depthStoreOp = RHI::StoreOp::Discard;
		depth.clearDepth = 1.0f;
		if(context.depthStencil != nullptr &&
			context.depthStencil->GetDesc().format == RHI::Format::D24_UNORM_S8_UINT)
		{
			depth.stencilLoadOp = RHI::LoadOp::Discard;
			depth.stencilStoreOp = RHI::StoreOp::Discard;
		}

		const RHI::RenderingDesc rendering = {
			&color,
			1,
			context.depthStencil != nullptr ? &depth : nullptr
		};
		commandList.BeginRendering(rendering);
		commandList.BindGraphicsPipeline(context.pipeline);
		commandList.SetViewport(RHI::Viewport{
			0.0f,
			0.0f,
			static_cast<float>(backBuffer->GetDesc().width),
			static_cast<float>(backBuffer->GetDesc().height),
			0.0f,
			1.0f
		});
		commandList.SetScissor(RHI::Rect{ 0, 0, backBuffer->GetDesc().width, backBuffer->GetDesc().height });
		return true;
	}

	void EndMainRendering(RHI::ICommandList& commandList, RHI::TextureHandle backBuffer)
	{
		commandList.EndRendering();
		const RHI::ResourceBarrierDesc barrier = {
			nullptr,
			backBuffer,
			RHI::ResourceState::RenderTarget,
			RHI::ResourceState::Present,
			{}
		};
		commandList.ResourceBarrier(&barrier, 1);
	}
}

void DrawData::DestroyMeshState(RHI::IDevice* device, SceneMeshState& mesh)
{
	DestroyBuffer(device, mesh.vertexBuffer, mesh.vertexReady);
	DestroyBuffer(device, mesh.indexBuffer, mesh.indexReady);
	mesh.indexCount = 0;
	mesh.prepared = false;
}

bool DrawData::Prepare(const Scene& scene, RHI::IDevice* device)
{
	if(device == nullptr) return false;

	while(m_meshes.size() > scene.GetMeshCount())
	{
		DestroyMeshState(device, m_meshes.back());
		m_meshes.pop_back();
	}
	m_meshes.resize(scene.GetMeshCount());

	std::vector<PendingMeshUpload> pending;
	for(uint32_t meshIndex = 0; meshIndex < scene.GetMeshCount(); ++meshIndex)
	{
		SceneMeshState& mesh = m_meshes[meshIndex];
		if(mesh.prepared) continue;

		const MeshData& source = scene.GetMesh(static_cast<MeshID>(meshIndex));
		if(source.vertices.size() > std::numeric_limits<uint32_t>::max() / sizeof(RendererVertex) ||
			(!source.indices.empty() &&
				source.indices.size() > std::numeric_limits<uint32_t>::max() / sizeof(uint32_t))) return false;

		PendingMeshUpload upload = {};
		upload.meshIndex = meshIndex;
		upload.vertices = BuildRendererVertices(source);
		upload.indices = BuildRendererIndices(source);
		if(upload.vertices.empty() || upload.indices.empty())
		{
			DestroyMeshState(device, mesh);
			mesh.prepared = true;
			continue;
		}

		const uint32_t vertexBytes = static_cast<uint32_t>(upload.vertices.size() * sizeof(RendererVertex));
		const uint32_t indexBytes = static_cast<uint32_t>(upload.indices.size() * sizeof(uint32_t));
		if(!EnsureBuffer(
				device, mesh.vertexBuffer, mesh.vertexReady,
				vertexBytes, static_cast<uint32_t>(sizeof(RendererVertex)), RHI::BufferUsage::Vertex) ||
			!EnsureBuffer(
				device, mesh.indexBuffer, mesh.indexReady,
				indexBytes, static_cast<uint32_t>(sizeof(uint32_t)), RHI::BufferUsage::Index)) return false;

		mesh.indexCount = static_cast<uint32_t>(upload.indices.size());
		if(mesh.vertexReady && mesh.indexReady)
		{
			mesh.prepared = true;
			continue;
		}
		pending.push_back(std::move(upload));
	}

	if(pending.empty()) return true;

	RHI::ICommandList* commandList = device->AcquireCommandList();
	if(commandList == nullptr) return false;
	bool uploadFailed = false;
	std::vector<bool*> uploaded;
	for(const PendingMeshUpload& upload : pending)
	{
		SceneMeshState& mesh = m_meshes[upload.meshIndex];
		if(!mesh.vertexReady)
		{
			if(!RecordBufferUpload(
					device, *commandList, mesh.vertexBuffer, upload.vertices.data(), mesh.vertexBuffer->GetDesc().size,
					RHI::ResourceState::VertexBuffer))
			{
				uploadFailed = true;
				break;
			}
			uploaded.push_back(&mesh.vertexReady);
		}
		if(!mesh.indexReady)
		{
			if(!RecordBufferUpload(
					device, *commandList, mesh.indexBuffer, upload.indices.data(), mesh.indexBuffer->GetDesc().size,
					RHI::ResourceState::IndexBuffer))
			{
				uploadFailed = true;
				break;
			}
			uploaded.push_back(&mesh.indexReady);
		}
	}

	commandList->Close();
	std::array<RHI::ICommandList*, 1> commands = { commandList };
	const bool submitted = device->Submit(commands.data(), 1);
	if(submitted)
	{
		for(bool* ready : uploaded) *ready = true;
		for(SceneMeshState& mesh : m_meshes)
		{
			if(mesh.vertexReady && mesh.indexReady) mesh.prepared = true;
		}
	}
	return submitted && !uploadFailed;
}

bool DrawData::PrepareDeformations(const Scene& scene, RHI::IDevice* device)
{
	std::vector<SkinInfluence> influences;
	std::vector<uint32_t> offsets(scene.GetMeshCount(), UINT32_MAX);
	for(uint32_t index = 0; index < scene.GetMeshCount(); ++index)
	{
		const auto& mesh = scene.GetMesh(static_cast<MeshID>(index));
		if(mesh.skinInfluences.empty()) continue;
		if(mesh.skinInfluences.size() != mesh.vertices.size() ||
			influences.size() + mesh.skinInfluences.size() > UINT32_MAX / sizeof(SkinInfluence)) return false;
		offsets[index] = static_cast<uint32_t>(influences.size());
		influences.insert(influences.end(), mesh.skinInfluences.begin(), mesh.skinInfluences.end());
	}
	if(influences.empty()) influences.emplace_back();
	const auto& palette = scene.JointPaletteMatrices();
	if(palette.size() > UINT32_MAX / sizeof(SkinJointMatrices)) return false;
	for(uint32_t index = 0; index < scene.GetEntityCount(); ++index)
	{
		const auto entity = static_cast<EntityID>(index);
		if(!scene.IsEntitySkinned(entity)) continue;
		const auto meshId = scene.GetEntityMesh(entity);
		if(!IsValid(meshId) || ToIndex(meshId) >= offsets.size() || offsets[ToIndex(meshId)] == UINT32_MAX) return false;
		const auto& mesh = scene.GetMesh(meshId);
		const uint64_t paletteOffset = scene.GetEntitySkinPaletteOffset(entity);
		for(const auto& influence : mesh.skinInfluences)
		{
			if(!std::isfinite(influence.dqBlendWeight) || influence.dqBlendWeight < 0 || influence.dqBlendWeight > 1) return false;
			for(uint32_t component = 0; component < 4; ++component)
			{
				const auto weight = influence.weights[component];
				if(!std::isfinite(weight) || weight < 0 || (weight > 0 && paletteOffset + influence.jointIndices[component] >= palette.size())) return false;
			}
		}
	}
	RHI::ICommandList* commands = device->AcquireCommandList();
	if(!commands) return false;
	std::vector<RHI::BufferHandle> allocated;
	bool prepared = true;
	auto upload = [&](const void* data, size_t count, uint32_t stride, RHI::BufferUsage usage, RHI::ResourceState state)
	{
		if(!count || count > UINT32_MAX / stride) { prepared = false; return RHI::BufferHandle{}; }
		auto* buffer = device->CreateBuffer({static_cast<uint32_t>(count * stride), stride, usage, RHI::ResourceState::CopyDestination});
		if(!buffer) { prepared = false; return buffer; }
		allocated.push_back(buffer);
		if(!RecordBufferUpload(device, *commands, buffer, data, buffer->GetDesc().size, state)) prepared = false;
		return buffer;
	};
	auto* nextInfluences = upload(influences.data(), influences.size(), sizeof(SkinInfluence), RHI::BufferUsage::Storage, RHI::ResourceState::ShaderResource);
	const SkinJointMatrices identity;
	auto* nextPalette = upload(palette.empty() ? &identity : palette.data(), palette.empty() ? 1 : palette.size(),
		sizeof(SkinJointMatrices), RHI::BufferUsage::Storage, RHI::ResourceState::ShaderResource);
	std::vector<RHI::BufferHandle> morphVertices(scene.GetEntityCount(), nullptr);
	for(uint32_t index = 0; index < scene.GetEntityCount(); ++index)
	{
		const auto* morph = scene.TryGetEntityMorphedMesh(static_cast<EntityID>(index));
		if(!morph) continue;
		const auto vertices = BuildRendererVertices(*morph);
		morphVertices[index] = upload(vertices.data(), vertices.size(), sizeof(RendererVertex), RHI::BufferUsage::Vertex, RHI::ResourceState::VertexBuffer);
	}
	commands->Close();
	const bool submitted = device->Submit(&commands, 1);
	if(!prepared || !submitted)
	{
		for(auto* buffer : allocated) device->DestroyBuffer(buffer);
		return false;
	}
	if(m_influences) device->DestroyBuffer(m_influences);
	if(m_palette) device->DestroyBuffer(m_palette);
	for(auto* buffer : m_morphVertices) if(buffer) device->DestroyBuffer(buffer);
	m_influences = nextInfluences;
	m_palette = nextPalette;
	m_skinOffsets = std::move(offsets);
	m_morphVertices = std::move(morphVertices);
	return true;
}

bool DrawData::SubmitShadow(
	RHI::ICommandList& commandList,
	const Scene& scene,
	const DrawContext& context,
	RHI::ResourceSetHandle resourceSet)
{
	if(context.materialStates == nullptr || !BeginShadowRendering(commandList, context, resourceSet)) return false;

	const std::vector<SceneMaterialState>& materials = *context.materialStates;
	for(uint32_t entityIndex = 0; entityIndex < scene.GetEntityCount(); ++entityIndex)
	{
		const EntityID entity = static_cast<EntityID>(entityIndex);
		const RenderFlags& renderFlags = scene.GetRenderFlags(entity);
		if(!renderFlags.castShadow) continue;

		const MeshID meshId = scene.GetEntityMesh(entity);
		const MaterialID materialId = scene.GetEntityMaterial(entity);
		if(!IsValid(meshId) || !IsValid(materialId)) continue;
		const uint32_t meshIndex = ToIndex(meshId);
		const uint32_t materialIndex = ToIndex(materialId);
		if(meshIndex >= m_meshes.size() || materialIndex >= materials.size()) continue;

		const SceneMeshState& mesh = m_meshes[meshIndex];
		if(!mesh.prepared || mesh.vertexBuffer == nullptr || mesh.indexBuffer == nullptr || mesh.indexCount == 0) continue;
		commandList.BindVertexBuffer(0, m_morphVertices[entityIndex] ? m_morphVertices[entityIndex] : mesh.vertexBuffer, 0);
		commandList.BindIndexBuffer(mesh.indexBuffer, RHI::Format::R32_UINT, 0);
		Layout::DrawConstants constants = MakeDrawConstants(
			context,
			scene.GetMaterial(materialId),
			scene.GetTransform(entity),
			ComputeTextureFlags(materials[materialIndex], renderFlags));
		constants.padding0 = m_skinOffsets[meshIndex];
		constants.padding1 = scene.GetEntitySkinPaletteOffset(entity);
		commandList.SetInlineConstants(0, static_cast<uint32_t>(sizeof(constants)), &constants);
		commandList.DrawIndexedInstanced(mesh.indexCount, 1, 0, 0, 0);
	}
	EndShadowRendering(commandList, context.shadowDepth);
	return true;
}

bool DrawData::Submit(const Scene& scene, RHI::IDevice* device, const DrawContext& context)
{
	if(device == nullptr || context.materialStates == nullptr ||
		context.pipeline == nullptr) return false;

	std::vector<RHI::ResourceSetHandle> materialResourceSets;
	if(!m_influences || !m_palette || !CreateMaterialResourceSets(device, context, m_influences, m_palette, materialResourceSets)) return false;

	const bool submitShadow = context.shadowPipeline != nullptr && context.shadowDepth != nullptr &&
		context.shadowMatrixBuffer != nullptr && context.shadowDepth->GetDesc().width != 0;
	RHI::ResourceSetHandle shadowResourceSet = nullptr;
	if(submitShadow)
	{
		const std::array<RHI::ResourceBinding, 3> bindings = {{
			{ RENDERER_BINDING_SHADOW_MATRIX, 0, context.shadowMatrixBuffer, nullptr, 0, static_cast<uint32_t>(sizeof(Layout::RendererShadowConstants)), {} },
			{ RENDERER_BINDING_SKIN_INFLUENCES, 0, m_influences, nullptr, 0, m_influences->GetDesc().size, {} },
			{ RENDERER_BINDING_SKIN_PALETTE, 0, m_palette, nullptr, 0, m_palette->GetDesc().size, {} }
		}};
		shadowResourceSet = device->CreateResourceSet(RHI::ResourceSetDesc{
			context.shadowPipeline,
			bindings.data(),
			static_cast<uint32_t>(bindings.size())
		});
	}
	if(submitShadow && shadowResourceSet == nullptr)
	{
		DestroyResourceSets(device, materialResourceSets);
		return false;
	}

	RHI::TextureHandle backBuffer = device->GetBackBuffer();
	if(backBuffer == nullptr)
	{
		DestroyResourceSets(device, materialResourceSets);
		if(shadowResourceSet != nullptr) device->DestroyResourceSet(shadowResourceSet);
		return false;
	}
	RHI::ICommandList* commandList = device->AcquireCommandList();
	if(commandList == nullptr)
	{
		DestroyResourceSets(device, materialResourceSets);
		if(shadowResourceSet != nullptr) device->DestroyResourceSet(shadowResourceSet);
		return false;
	}
	const bool shadowSubmitted = !submitShadow ||
		SubmitShadow(*commandList, scene, context, shadowResourceSet);
	const bool mainBegan = shadowSubmitted && BeginMainRendering(*commandList, backBuffer, context, submitShadow);

	if(mainBegan)
	{
		const std::vector<SceneMaterialState>& materials = *context.materialStates;
		for(uint32_t entityIndex = 0; entityIndex < scene.GetEntityCount(); ++entityIndex)
		{
			const EntityID entity = static_cast<EntityID>(entityIndex);
			const MeshID meshId = scene.GetEntityMesh(entity);
			const MaterialID materialId = scene.GetEntityMaterial(entity);
			if(!IsValid(meshId) || !IsValid(materialId)) continue;

			const uint32_t meshIndex = ToIndex(meshId);
			const uint32_t materialIndex = ToIndex(materialId);
			if(meshIndex >= m_meshes.size() || materialIndex >= materials.size() ||
				materialIndex >= materialResourceSets.size()) continue;
			const SceneMeshState& mesh = m_meshes[meshIndex];
			if(!mesh.prepared || mesh.vertexBuffer == nullptr || mesh.indexBuffer == nullptr || mesh.indexCount == 0) continue;

			commandList->BindResourceSet(materialResourceSets[materialIndex]);
			commandList->BindVertexBuffer(0, m_morphVertices[entityIndex] ? m_morphVertices[entityIndex] : mesh.vertexBuffer, 0);
			commandList->BindIndexBuffer(mesh.indexBuffer, RHI::Format::R32_UINT, 0);
			Layout::DrawConstants constants = MakeDrawConstants(
				context,
				scene.GetMaterial(materialId),
				scene.GetTransform(entity),
				ComputeTextureFlags(materials[materialIndex], scene.GetRenderFlags(entity)));
			constants.padding0 = m_skinOffsets[meshIndex];
			constants.padding1 = scene.GetEntitySkinPaletteOffset(entity);
			commandList->SetInlineConstants(0, static_cast<uint32_t>(sizeof(constants)), &constants);
			commandList->DrawIndexedInstanced(mesh.indexCount, 1, 0, 0, 0);
		}

		EndMainRendering(*commandList, backBuffer);
	}
	commandList->Close();
	std::array<RHI::ICommandList*, 1> commands = { commandList };
	const bool submitted = device->Submit(commands.data(), 1) && mainBegan;
	DestroyResourceSets(device, materialResourceSets);
	if(shadowResourceSet != nullptr) device->DestroyResourceSet(shadowResourceSet);
	return submitted;
}

void DrawData::Shutdown(RHI::IDevice* device)
{
	if(device == nullptr) return;
	for(SceneMeshState& mesh : m_meshes) DestroyMeshState(device, mesh);
	m_meshes.clear();
	if(m_influences) device->DestroyBuffer(m_influences);
	if(m_palette) device->DestroyBuffer(m_palette);
	for(auto* buffer : m_morphVertices) if(buffer) device->DestroyBuffer(buffer);
	m_influences = m_palette = nullptr;
	m_skinOffsets.clear();
	m_morphVertices.clear();
}
