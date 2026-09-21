#include "dyf/Renderer.h"
#include "dyf/Camera.h"
#include "dyf/Scene.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

#include "ShaderLayout.h"
#include "ShaderLayout.h"
#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/ResourceSet.h"
#include "dyf/RHI/Texture.h"

namespace dyf
{

namespace
{
	void DestroyBuffer(RHI::IDevice* device, RHI::BufferHandle& buffer, bool& ready)
	{
		if(device != nullptr && buffer != nullptr) device->DestroyBuffer(buffer);
		buffer = nullptr;
		ready = false;
	}

	bool EnsureBuffer(RHI::IDevice* device, RHI::BufferHandle& buffer, bool& ready,
		uint32_t sizeBytes, uint32_t stride, RHI::BufferUsage usage)
	{
		if(device == nullptr) return false;
		if(sizeBytes == 0)
		{
			DestroyBuffer(device, buffer, ready);
			return true;
		}
		if(buffer != nullptr && buffer->GetDesc().size == sizeBytes) return true;
		DestroyBuffer(device, buffer, ready);
		buffer = device->CreateBuffer({sizeBytes, stride, usage, RHI::ResourceState::CopyDestination});
		return buffer != nullptr;
	}

	bool RecordBufferUpload(RHI::IDevice* device, RHI::ICommandList& commands,
		RHI::BufferHandle buffer, const void* data, uint32_t sizeBytes, RHI::ResourceState useState)
	{
		if(device == nullptr || buffer == nullptr || data == nullptr || sizeBytes == 0) return false;
		if(!device->UpdateBuffer(commands, buffer, 0, data, sizeBytes)) return false;
		const RHI::ResourceBarrierDesc ready = {
			buffer, nullptr, RHI::ResourceState::CopyDestination, useState, {}
		};
		commands.ResourceBarrier(&ready, 1);
		return true;
	}

	void DestroyResourceSets(RHI::IDevice* device, std::vector<RHI::ResourceSetHandle>& sets)
	{
		if(device != nullptr)
			for(auto* set : sets) if(set != nullptr) device->DestroyResourceSet(set);
		sets.clear();
	}

	DrawConstants MakeDrawConstants(const Math::float4x4& viewProjection,
		const MaterialDesc& material, const Transform& transform, uint32_t textureFlags)
	{
		DrawConstants constants = {};
		constants.viewProjectionMatrix = viewProjection;
		constants.modelMatrix = transform.worldMatrix;
		constants.textureFlags = textureFlags;
		constants.emissiveColor = {material.emissiveColor.x, material.emissiveColor.y, material.emissiveColor.z, 0};
		constants.baseColor = material.baseColor;
		constants.materialParams = {material.metallicFactor, material.roughnessFactor,
			material.normalScale, material.occlusionStrength};
		return constants;
	}
}

bool Renderer::CreateMaterialResourceSets(const Scene& scene, RHI::ICommandList& commands,
	const std::vector<RendererDrawDesc>* draws, std::vector<RHI::ResourceSetHandle>& sets)
{
	if(device == nullptr || pipeline == nullptr || lightingBuffer == nullptr) return false;
	const bool shadowsEnabled = shadowDepthTarget != nullptr;
	if(shadowsEnabled && shadowMatrixBuffer == nullptr) return false;

	if(UsesBindlessMaterials())
	{
		if(materialStates.empty()) return true;
		// 기본 셰이더의 28개 텍스처 단위로 묶어 재질 수와 관계없이 바인딩한다.
		struct Page
		{
			std::vector<RHI::TextureHandle> textures;
			std::vector<uint32_t> materials;
		};
		std::vector<Page> pages(1);
		std::vector<uint32_t> indices(materialStates.size() * 8, 0);
		for(uint32_t i = 0; i < materialStates.size(); ++i)
		{
			auto textures = pages.back().textures;
			for(auto* texture : materialStates[i].textures)
				if(std::find(textures.begin(), textures.end(), texture) == textures.end()) textures.push_back(texture);
			if(textures.size() > 28) pages.emplace_back();
			auto& page = pages.back();
			page.materials.push_back(i);
			for(uint32_t slot = 0; slot < kMaterialTextureCount; ++slot)
			{
				const auto texture = materialStates[i].textures[slot];
				const auto found = std::find(page.textures.begin(), page.textures.end(), texture);
				if(found == page.textures.end())
				{
					indices[i * 8 + slot] = static_cast<uint32_t>(page.textures.size());
					page.textures.push_back(texture);
				}
				else indices[i * 8 + slot] = static_cast<uint32_t>(found - page.textures.begin());
			}
		}
		if(indices.size() > UINT32_MAX / sizeof(uint32_t)) return false;
		auto* materialBuffer = device->CreateBuffer({static_cast<uint32_t>(indices.size() * sizeof(uint32_t)),
			16, RHI::BufferUsage::Storage, RHI::ResourceState::CopyDestination});
		if(!materialBuffer) return false;
		// 재질 인덱스 업로드를 장면 명령의 앞부분에 기록한다.
		if(!device->UpdateBuffer(commands, materialBuffer, 0, indices.data(), materialBuffer->GetDesc().size))
		{
			device->DestroyBuffer(materialBuffer);
			return false;
		}
		const RHI::ResourceBarrierDesc ready = {
			materialBuffer, nullptr, RHI::ResourceState::CopyDestination, RHI::ResourceState::ShaderResource, {}
		};
		commands.ResourceBarrier(&ready, 1);
		sets.assign(draws ? scene.GetEntityCount() : materialStates.size(), nullptr);
		for(const auto& page : pages)
		{
			std::vector<RHI::ResourceBinding> bindings;
			for(uint32_t i = 0; i < 28; ++i)
				bindings.push_back({0, i, nullptr, page.textures[i < page.textures.size() ? i : 0], 0, 0, {}});
			bindings.push_back({1u, 0, lightingBuffer, nullptr,
				0, static_cast<uint32_t>(sizeof(RendererLightingConstants)), {}});
			bindings.push_back({29, 0, materialBuffer, nullptr, 0, materialBuffer->GetDesc().size, {}});
			if(shadowsEnabled)
			{
				bindings.push_back({28, 0, nullptr, shadowDepthTarget, 0, 0, {}});
				bindings.push_back({3u, 0, shadowMatrixBuffer, nullptr,
					0, static_cast<uint32_t>(sizeof(RendererShadowConstants)), {}});
			}
			if(draws)
			{
				// 확장이 제공한 객체별 자원을 기본 재질 바인딩에 추가한다.
				for(uint32_t entity = 0; entity < scene.GetEntityCount(); ++entity)
				{
					const auto material = ToIndex(scene.GetEntityMaterial(static_cast<EntityID>(entity)));
					if(std::find(page.materials.begin(), page.materials.end(), material) == page.materials.end()) continue;
					auto combined = bindings;
					const auto& extra = (*draws)[entity].vertexResources;
					combined.insert(combined.end(), extra.begin(), extra.end());
					sets[entity] = device->CreateResourceSet({pipeline, combined.data(), static_cast<uint32_t>(combined.size())});
					if(!sets[entity])
					{
						DestroyResourceSets(device, sets);
						device->DestroyBuffer(materialBuffer);
						return false;
					}
				}
			}
			else
			{
				auto* table = device->CreateResourceSet({pipeline, bindings.data(), static_cast<uint32_t>(bindings.size())});
				if(!table)
				{
					DestroyResourceSets(device, sets);
					device->DestroyBuffer(materialBuffer);
					return false;
				}
				for(auto index : page.materials) sets[index] = table;
			}
		}
		device->DestroyBuffer(materialBuffer);
		return true;
	}

	sets.resize(draws ? scene.GetEntityCount() : materialStates.size(), nullptr);
	for(uint32_t setIndex = 0; setIndex < sets.size(); ++setIndex)
	{
		const auto materialIndex = draws
			? ToIndex(scene.GetEntityMaterial(static_cast<EntityID>(setIndex))) : setIndex;
		if(materialIndex >= materialStates.size()) continue;
		const auto& material = materialStates[materialIndex];
		std::vector<RHI::ResourceBinding> bindings = {
			{0u, 0, nullptr, material.textures[ToIndex(MaterialTextureKind::BaseColor)], 0, 0, {}},
			{1u, 0, lightingBuffer, nullptr, 0, static_cast<uint32_t>(sizeof(RendererLightingConstants)), {}},
			{4u, 0, nullptr, material.textures[ToIndex(MaterialTextureKind::MetallicRoughness)], 0, 0, {}},
			{5u, 0, nullptr, material.textures[ToIndex(MaterialTextureKind::Normal)], 0, 0, {}},
			{6u, 0, nullptr, material.textures[ToIndex(MaterialTextureKind::Occlusion)], 0, 0, {}},
			{7u, 0, nullptr, material.textures[ToIndex(MaterialTextureKind::Emissive)], 0, 0, {}},
			{3u, 0, shadowMatrixBuffer, nullptr, 0, static_cast<uint32_t>(sizeof(RendererShadowConstants)), {}},
			{2u, 0, nullptr, shadowDepthTarget, 0, 0, {}}
		};
		if(!shadowsEnabled) bindings.resize(bindings.size() - 2);
		if(draws)
		{
			const auto& extra = (*draws)[setIndex].vertexResources;
			bindings.insert(bindings.end(), extra.begin(), extra.end());
		}
		sets[setIndex] = device->CreateResourceSet({pipeline, bindings.data(), static_cast<uint32_t>(bindings.size())});
		if(!sets[setIndex])
		{
			DestroyResourceSets(device, sets);
			return false;
		}
	}
	return true;
}

void Renderer::DestroyMeshState(RHI::IDevice* device, SceneMeshState& mesh)
{
	DestroyBuffer(device, mesh.vertexBuffer, mesh.vertexReady);
	DestroyBuffer(device, mesh.indexBuffer, mesh.indexReady);
	mesh.indexCount = 0;
	mesh.prepared = false;
}

// 공개 RHI가 업로드 바이트를 복사하므로 메시별 변환 배열은 기록 직후 해제할 수 있다.
bool Renderer::PrepareGeometry(const Scene& scene, RHI::IDevice* device)
{
	if(device == nullptr) return false;
	while(m_meshes.size() > scene.Meshes().size())
	{
		DestroyMeshState(device, m_meshes.back());
		m_meshes.pop_back();
	}
	m_meshes.resize(scene.Meshes().size());

	RHI::ICommandList* commands = nullptr;
	bool uploadFailed = false;
	std::vector<bool*> uploaded;
	for(uint32_t meshIndex = 0; meshIndex < scene.Meshes().size(); ++meshIndex)
	{
		SceneMeshState& mesh = m_meshes[meshIndex];
		const auto& sourceMesh = scene.Meshes()[meshIndex];
		if(mesh.source.lock() != sourceMesh)
		{
			DestroyMeshState(device, mesh);
			mesh.source = sourceMesh;
		}
		if(mesh.prepared) continue;
		const MeshData& source = *sourceMesh;
		if(source.vertices.size() > UINT32_MAX / sizeof(RendererVertex) ||
			source.indices.size() > UINT32_MAX / sizeof(uint32_t))
		{
			if(commands) device->DestroyCommandList(commands);
			return false;
		}
		std::vector<RendererVertex> vertices;
		vertices.reserve(source.vertices.size());
		for(const auto& vertex : source.vertices)
		{
			vertices.push_back({vertex.position.x, vertex.position.y, vertex.position.z,
				vertex.normal.x, vertex.normal.y, vertex.normal.z, vertex.uv.x, vertex.uv.y,
				vertex.tangent.x, vertex.tangent.y, vertex.tangent.z, vertex.tangent.w});
		}
		std::vector<uint32_t> sequentialIndices;
		if(source.indices.empty())
		{
			sequentialIndices.reserve(source.vertices.size());
			for(uint32_t index = 0; index < source.vertices.size(); ++index) sequentialIndices.push_back(index);
		}
		const auto& indices = source.indices.empty() ? sequentialIndices : source.indices;
		if(vertices.empty() || indices.empty())
		{
			DestroyMeshState(device, mesh);
			mesh.prepared = true;
			continue;
		}
		const uint32_t vertexBytes = static_cast<uint32_t>(vertices.size() * sizeof(RendererVertex));
		const uint32_t indexBytes = static_cast<uint32_t>(indices.size() * sizeof(uint32_t));
		if(!EnsureBuffer(device, mesh.vertexBuffer, mesh.vertexReady, vertexBytes,
				sizeof(RendererVertex), RHI::BufferUsage::Vertex | RHI::BufferUsage::Storage) ||
			!EnsureBuffer(device, mesh.indexBuffer, mesh.indexReady, indexBytes,
				sizeof(uint32_t), RHI::BufferUsage::Index))
		{
			if(commands) device->DestroyCommandList(commands);
			return false;
		}
		mesh.indexCount = static_cast<uint32_t>(indices.size());
		if(mesh.vertexReady && mesh.indexReady)
		{
			mesh.prepared = true;
			continue;
		}
		if(!commands) commands = device->AcquireCommandList();
		if(!commands) return false;
		if(!mesh.vertexReady)
		{
			if(!RecordBufferUpload(device, *commands, mesh.vertexBuffer, vertices.data(), vertexBytes,
				RHI::ResourceState::VertexBuffer))
			{
				uploadFailed = true;
				break;
			}
			uploaded.push_back(&mesh.vertexReady);
		}
		if(!mesh.indexReady)
		{
			if(!RecordBufferUpload(device, *commands, mesh.indexBuffer, indices.data(), indexBytes,
				RHI::ResourceState::IndexBuffer))
			{
				uploadFailed = true;
				break;
			}
			uploaded.push_back(&mesh.indexReady);
		}
	}

	if(!commands) return true;
	const bool closed = commands->Close();
	const bool submitted = closed && device->Submit(&commands, 1);
	device->DestroyCommandList(commands);
	if(submitted)
	{
		// 실패한 제출은 준비 완료로 표시하지 않아 다음 프레임에서 다시 업로드한다.
		for(bool* ready : uploaded) *ready = true;
		for(auto& mesh : m_meshes)
			if(mesh.vertexReady && mesh.indexReady) mesh.prepared = true;
	}
	return submitted && !uploadFailed;
}

bool Renderer::RecordShadowPass(const Scene& scene, const Camera& camera, const ShadowData& shadows,
	RHI::ICommandList& commands, const std::vector<RendererDrawDesc>* draws,
	RHI::TimestampQueryHandle shadowQuery)
{
	std::vector<RHI::ResourceSetHandle> shadowSets;
	{
		commands.BeginDebugEvent("Shadow");
		shadowSets.resize(draws ? scene.GetEntityCount() : 1, nullptr);
		for(uint32_t i = 0; i < shadowSets.size(); ++i)
		{
			std::vector<RHI::ResourceBinding> bindings = {{3u,
				0, shadowMatrixBuffer, nullptr, 0, static_cast<uint32_t>(sizeof(RendererShadowConstants)), {}}};
			if(draws)
			{
				const auto& extra = (*draws)[i].vertexResources;
				bindings.insert(bindings.end(), extra.begin(), extra.end());
			}
			shadowSets[i] = device->CreateResourceSet({shadowPipeline, bindings.data(), static_cast<uint32_t>(bindings.size())});
			if(!shadowSets[i])
			{
				DestroyResourceSets(device, shadowSets);
				return false;
			}
		}
	}
	const auto viewProjection = camera.projection * camera.view;
	if(shadowQuery) { commands.ResetTimestamps(shadowQuery, 0, 2); commands.WriteTimestamp(shadowQuery, 0); }
	{
		RHI::DepthStencilAttachment depth;
		depth.texture = shadowDepthTarget;
		depth.state = RHI::ResourceState::DepthWrite;
		depth.depthLoadOp = RHI::LoadOp::Clear;
		depth.depthStoreOp = RHI::StoreOp::Store;
		depth.clearDepth = 1;
		if(shadowDepthTarget->GetDesc().format == RHI::Format::D24_UNORM_S8_UINT)
		{
			depth.stencilLoadOp = RHI::LoadOp::Discard;
			depth.stencilStoreOp = RHI::StoreOp::Discard;
		}
		commands.BeginRendering({nullptr, 0, &depth});
		commands.BindGraphicsPipeline(shadowPipeline);
		for(uint32_t view = 0; view < shadows.viewCount; ++view)
		{
			const uint32_t x = (view % shadows.columns) * shadows.resolution;
			const uint32_t y = (view / shadows.columns) * shadows.resolution;
			commands.SetViewport({static_cast<float>(x), static_cast<float>(y),
				static_cast<float>(shadows.resolution), static_cast<float>(shadows.resolution), 0, 1});
			commands.SetScissor({static_cast<int32_t>(x), static_cast<int32_t>(y), shadows.resolution, shadows.resolution});
			for(uint32_t entityIndex = 0; entityIndex < scene.GetEntityCount(); ++entityIndex)
			{
				const auto entity = static_cast<EntityID>(entityIndex);
				const auto& lighting = scene.GetEntityLighting(entity);
				if(!lighting.castShadow) continue;
				const auto meshId = scene.GetEntityMesh(entity);
				const auto materialId = scene.GetEntityMaterial(entity);
				if(!IsValid(meshId) || !IsValid(materialId)) continue;
				const uint32_t meshIndex = ToIndex(meshId), materialIndex = ToIndex(materialId);
				if(meshIndex >= m_meshes.size() || materialIndex >= materialStates.size()) continue;
				const auto& mesh = m_meshes[meshIndex];
				if(!mesh.prepared || !mesh.vertexBuffer || !mesh.indexBuffer || !mesh.indexCount) continue;
				const auto* input = draws ? &(*draws)[entityIndex] : nullptr;
				commands.BindResourceSet(shadowSets[input ? entityIndex : 0]);
				commands.BindVertexBuffer(0, input && input->vertexBuffer ? input->vertexBuffer : mesh.vertexBuffer, 0);
				commands.BindIndexBuffer(mesh.indexBuffer, RHI::Format::R32_UINT, 0);
				const uint32_t flags = materialStates[materialIndex].textureFlags |
					(lighting.receiveShadow ? 32u : 0u);
				auto constants = MakeDrawConstants(viewProjection, scene.Materials()[materialIndex], scene.GetTransform(entity), flags);
				constants.padding2 = view;
				commands.SetInlineConstants(0, sizeof(constants), &constants);
				if(input && !input->inlineConstants.empty())
					commands.SetInlineConstants(sizeof(constants), static_cast<uint32_t>(input->inlineConstants.size()), input->inlineConstants.data());
				commands.DrawIndexedInstanced(mesh.indexCount, 1, 0, 0, 0);
			}
		}
		commands.EndRendering();

	}
	if(shadowQuery) commands.WriteTimestamp(shadowQuery, 1);
	commands.EndDebugEvent();
	DestroyResourceSets(device, shadowSets);
	return true;
}

bool Renderer::RecordMainPass(const Scene& scene, const Camera& camera,
	RHI::ICommandList& commands, const std::vector<RendererDrawDesc>* draws,
	RHI::TimestampQueryHandle mainQuery)
{
	if(device == nullptr || pipeline == nullptr) return false;
	std::vector<RHI::ResourceSetHandle> materialSets;
	if(!CreateMaterialResourceSets(scene, commands, draws, materialSets)) return false;
	auto* target = config.enableHdrRendering ? hdrTarget : device->GetBackBuffer();
	if(!target) { DestroyResourceSets(device, materialSets); return false; }
	const auto viewProjection = camera.projection * camera.view;
	if(mainQuery) { commands.ResetTimestamps(mainQuery, 0, 2); commands.WriteTimestamp(mainQuery, 0); }
	commands.BeginDebugEvent("MainForward");

	RHI::ColorAttachment color;
	color.texture = target;
	color.loadOp = RHI::LoadOp::Clear;
	color.storeOp = RHI::StoreOp::Store;
	color.clearColor[0] = config.clearColor.x;
	color.clearColor[1] = config.clearColor.y;
	color.clearColor[2] = config.clearColor.z;
	color.clearColor[3] = config.clearColor.w;
	RHI::DepthStencilAttachment depth;
	depth.texture = depthStencilTarget;
	depth.state = RHI::ResourceState::DepthWrite;
	depth.depthLoadOp = RHI::LoadOp::Clear;
	depth.depthStoreOp = RHI::StoreOp::Discard;
	depth.clearDepth = 1;
	if(depthStencilTarget->GetDesc().format == RHI::Format::D24_UNORM_S8_UINT)
	{
		depth.stencilLoadOp = RHI::LoadOp::Discard;
		depth.stencilStoreOp = RHI::StoreOp::Discard;
	}
	commands.BeginRendering({&color, 1, &depth});
	commands.BindGraphicsPipeline(pipeline);
	commands.SetViewport({0, 0, static_cast<float>(target->GetDesc().width), static_cast<float>(target->GetDesc().height), 0, 1});
	commands.SetScissor({0, 0, target->GetDesc().width, target->GetDesc().height});
	const bool batchBindings = shaderSources.bytes[MeshFragment].empty();
	RHI::ResourceSetHandle boundMaterial = nullptr;
	for(uint32_t entityIndex = 0; entityIndex < scene.GetEntityCount(); ++entityIndex)
	{
		const auto entity = static_cast<EntityID>(entityIndex);
		const auto meshId = scene.GetEntityMesh(entity);
		const auto materialId = scene.GetEntityMaterial(entity);
		if(!IsValid(meshId) || !IsValid(materialId)) continue;
		const uint32_t meshIndex = ToIndex(meshId), materialIndex = ToIndex(materialId);
		const uint32_t setIndex = draws ? entityIndex : materialIndex;
		if(meshIndex >= m_meshes.size() || materialIndex >= materialStates.size() || setIndex >= materialSets.size()) continue;
		const auto& mesh = m_meshes[meshIndex];
		if(!mesh.prepared || !mesh.vertexBuffer || !mesh.indexBuffer || !mesh.indexCount) continue;
		if(!batchBindings || boundMaterial != materialSets[setIndex])
		{
			boundMaterial = materialSets[setIndex];
			commands.BindResourceSet(boundMaterial);
		}
		const auto* input = draws ? &(*draws)[entityIndex] : nullptr;
		commands.BindVertexBuffer(0, input && input->vertexBuffer ? input->vertexBuffer : mesh.vertexBuffer, 0);
		commands.BindIndexBuffer(mesh.indexBuffer, RHI::Format::R32_UINT, 0);
		const uint32_t flags = materialStates[materialIndex].textureFlags |
			(scene.GetEntityLighting(entity).receiveShadow ? 32u : 0u);
		auto constants = MakeDrawConstants(viewProjection, scene.Materials()[materialIndex], scene.GetTransform(entity), flags);
		constants.padding2 = materialIndex;
		commands.SetInlineConstants(0, sizeof(constants), &constants);
		if(input && !input->inlineConstants.empty())
			commands.SetInlineConstants(sizeof(constants), static_cast<uint32_t>(input->inlineConstants.size()), input->inlineConstants.data());
		commands.DrawIndexedInstanced(mesh.indexCount, 1, 0, 0, 0);
	}
	commands.EndRendering();
	commands.EndDebugEvent();
	DestroyResourceSets(device, materialSets);
	return true;
}

void Renderer::ReleaseGeometry(RHI::IDevice* device)
{
	if(device == nullptr) return;
	for(auto& mesh : m_meshes) DestroyMeshState(device, mesh);
	m_meshes.clear();
}
}
