#include "Renderer.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "RHI/IBuffer.h"
#include "RHI/ICommandList.h"
#include "RHI/IDevice.h"
#include "RHI/ITexture.h"

using namespace dy;
using namespace dy::Graphics;

bool Renderer::Initialize(RHI::IDevice* device, const RHI::GraphicsPipelineDesc& mainPipeline, const RendererConfig& config)
{
	if(device == nullptr) return false;

	m_config = config;
	return BuildPipelineStates(device, mainPipeline);
}

void Renderer::Shutdown(RHI::IDevice* device)
{
	if(device == nullptr) return;

	m_drawBatches.clear();
	m_renderOrder.clear();
	m_renderItems.Clear();
	ReleaseGpuMeshes(device);

	if(m_mainPipeline != nullptr)
	{
		device->DestroyPipelineState(m_mainPipeline);
		m_mainPipeline = nullptr;
	}
}

void Renderer::Render(const Scene& scene, RHI::IDevice* device)
{
	if(device == nullptr) return;

	EnsureGpuMeshes(scene, device);
	BuildRenderItems(scene);
	BuildDrawBatches();
	RecordScenePass(device);
}

bool Renderer::BuildPipelineStates(RHI::IDevice* device, const RHI::GraphicsPipelineDesc& mainPipeline)
{
	if(device == nullptr) return false;
	if(mainPipeline.vertexShader == nullptr || mainPipeline.vertexShaderSize == 0u) return false;
	if(mainPipeline.pixelShader == nullptr || mainPipeline.pixelShaderSize == 0u) return false;

	m_mainPipeline = device->CreateGraphicsPipeline(mainPipeline);
	return m_mainPipeline != nullptr;
}

bool Renderer::UploadBuffer(RHI::IBuffer* buffer, const void* data, uint32_t size)
{
	if(buffer == nullptr || data == nullptr || size == 0u) return false;

	void* mapped = buffer->Map(0, size);
	if(mapped == nullptr) return false;

	std::memcpy(mapped, data, size);
	buffer->Unmap();
	return true;
}

void Renderer::ReleaseGpuMeshes(RHI::IDevice* device)
{
	if(device == nullptr) return;

	for(GpuMesh& mesh : m_gpuMeshes)
	{
		if(mesh.vertexBuffer != nullptr)
		{
			device->DestroyBuffer(mesh.vertexBuffer);
			mesh.vertexBuffer = nullptr;
		}

		if(mesh.indexBuffer != nullptr)
		{
			device->DestroyBuffer(mesh.indexBuffer);
			mesh.indexBuffer = nullptr;
		}

		mesh.vertexCount = 0;
		mesh.indexCount = 0;
		mesh.vertexByteSize = 0;
		mesh.indexByteSize = 0;
		mesh.vertexStride = sizeof(Vertex);
		mesh.vertexOffset = 0;
		mesh.indexOffset = 0;
	}

	m_gpuMeshes.clear();
}

void Renderer::EnsureGpuMeshes(const Scene& scene, RHI::IDevice* device)
{
	if(device == nullptr) return;

	if(m_gpuMeshes.size() < scene.m_meshes.size())
	{
		m_gpuMeshes.resize(scene.m_meshes.size());
	}

	for(uint32_t meshIndex = 0; meshIndex < static_cast<uint32_t>(scene.m_meshes.size()); ++meshIndex)
	{
		const MeshData& cpuMesh = scene.m_meshes[meshIndex];
		GpuMesh& gpuMesh = m_gpuMeshes[meshIndex];

		const uint32_t vertexCount = static_cast<uint32_t>(cpuMesh.vertices.size());
		const uint32_t indexCount = static_cast<uint32_t>(cpuMesh.indices.size());
		const uint32_t vertexByteSize = vertexCount * static_cast<uint32_t>(sizeof(Vertex));
		const uint32_t indexByteSize = indexCount * static_cast<uint32_t>(sizeof(uint32_t));
		if(vertexCount == 0u) continue;

		if(gpuMesh.vertexBuffer != nullptr && gpuMesh.vertexByteSize != vertexByteSize)
		{
			device->DestroyBuffer(gpuMesh.vertexBuffer);
			gpuMesh.vertexBuffer = nullptr;
			gpuMesh.vertexCount = 0;
			gpuMesh.vertexByteSize = 0;
		}

		if(gpuMesh.vertexBuffer == nullptr)
		{
			RHI::BufferDesc vertexDesc = {};
			vertexDesc.size = vertexByteSize;
			vertexDesc.stride = static_cast<uint32_t>(sizeof(Vertex));
			vertexDesc.usage = RHI::BufferUsage::Vertex;
			gpuMesh.vertexBuffer = device->CreateBuffer(vertexDesc);
			if(UploadBuffer(gpuMesh.vertexBuffer, cpuMesh.vertices.data(), vertexDesc.size))
			{
				gpuMesh.vertexCount = vertexCount;
				gpuMesh.vertexByteSize = vertexDesc.size;
				gpuMesh.vertexStride = vertexDesc.stride;
				gpuMesh.vertexOffset = 0;
			}
		}

		if(gpuMesh.indexBuffer != nullptr && gpuMesh.indexByteSize != indexByteSize)
		{
			device->DestroyBuffer(gpuMesh.indexBuffer);
			gpuMesh.indexBuffer = nullptr;
			gpuMesh.indexCount = 0;
			gpuMesh.indexByteSize = 0;
		}

		if(indexCount > 0u && gpuMesh.indexBuffer == nullptr)
		{
			RHI::BufferDesc indexDesc = {};
			indexDesc.size = indexCount * static_cast<uint32_t>(sizeof(uint32_t));
			indexDesc.stride = static_cast<uint32_t>(sizeof(uint32_t));
			indexDesc.usage = RHI::BufferUsage::Index;
			gpuMesh.indexBuffer = device->CreateBuffer(indexDesc);
			if(UploadBuffer(gpuMesh.indexBuffer, cpuMesh.indices.data(), indexDesc.size))
			{
				gpuMesh.indexCount = indexCount;
				gpuMesh.indexByteSize = indexDesc.size;
				gpuMesh.indexOffset = 0;
			}
		}
	}
}

void Renderer::BuildRenderItems(const Scene& scene)
{
	m_renderItems.Clear();
	m_renderItems.Reserve(static_cast<uint32_t>(scene.m_entityMeshes.size()));
	m_renderOrder.clear();

	const uint32_t entityCount = static_cast<uint32_t>(scene.m_entityMeshes.size());
	for(uint32_t entityIndex = 0; entityIndex < entityCount; ++entityIndex)
	{
		if(entityIndex >= scene.m_entityMaterials.size() || entityIndex >= scene.m_entityTransforms.size()) continue;

		const MeshID meshId = scene.m_entityMeshes[entityIndex];
		const MaterialID materialId = scene.m_entityMaterials[entityIndex];
		if(!IsValid(meshId) || !IsValid(materialId)) continue;

		const uint32_t meshIndex = ToIndex(meshId);
		const uint32_t materialIndex = ToIndex(materialId);
		if(meshIndex >= scene.m_meshes.size() || materialIndex >= scene.m_materials.size()) continue;

		const MeshData& mesh = scene.m_meshes[meshIndex];
		const uint32_t vertexCount = static_cast<uint32_t>(mesh.vertices.size());
		const uint32_t indexCount = static_cast<uint32_t>(mesh.indices.size());
		if(vertexCount == 0u) continue;

		m_renderItems.Push(meshIndex, materialIndex, entityIndex, vertexCount, indexCount);
	}

	const uint32_t itemCount = m_renderItems.Size();
	m_renderOrder.resize(itemCount);
	for(uint32_t itemIndex = 0; itemIndex < itemCount; ++itemIndex)
	{
		m_renderOrder[itemIndex] = itemIndex;
	}

	std::sort(m_renderOrder.begin(), m_renderOrder.end(), [this](uint32_t lhs, uint32_t rhs) {
		const uint32_t lhsMesh = m_renderItems.meshIndices[lhs];
		const uint32_t rhsMesh = m_renderItems.meshIndices[rhs];
		if(lhsMesh != rhsMesh) return lhsMesh < rhsMesh;

		const uint32_t lhsMaterial = m_renderItems.materialIndices[lhs];
		const uint32_t rhsMaterial = m_renderItems.materialIndices[rhs];
		if(lhsMaterial != rhsMaterial) return lhsMaterial < rhsMaterial;

		const uint8_t lhsIndexed = m_renderItems.indexedFlags[lhs];
		const uint8_t rhsIndexed = m_renderItems.indexedFlags[rhs];
		if(lhsIndexed != rhsIndexed) return lhsIndexed > rhsIndexed;

		return m_renderItems.transformIndices[lhs] < m_renderItems.transformIndices[rhs];
	});
}

void Renderer::BuildDrawBatches()
{
	m_drawBatches.clear();
	if(m_renderOrder.empty()) return;

	uint32_t firstItem = 0;
	while(firstItem < static_cast<uint32_t>(m_renderOrder.size()))
	{
		const uint32_t firstIndex = m_renderOrder[firstItem];
		const uint32_t firstMesh = m_renderItems.meshIndices[firstIndex];
		const uint32_t firstMaterial = m_renderItems.materialIndices[firstIndex];
		const uint8_t firstIndexed = m_renderItems.indexedFlags[firstIndex];
		uint32_t itemCount = 1;

		while(firstItem + itemCount < static_cast<uint32_t>(m_renderOrder.size()))
		{
			const uint32_t currentIndex = m_renderOrder[firstItem + itemCount];
			if(m_renderItems.meshIndices[currentIndex] != firstMesh
				|| m_renderItems.materialIndices[currentIndex] != firstMaterial
				|| m_renderItems.indexedFlags[currentIndex] != firstIndexed)
			{
				break;
			}

			++itemCount;
		}

		DrawBatch batch = {};
		batch.meshIndex = firstMesh;
		batch.materialIndex = firstMaterial;
		batch.firstItem = firstItem;
		batch.itemCount = itemCount;
		batch.indexed = firstIndexed != 0u;
		m_drawBatches.push_back(batch);

		firstItem += itemCount;
	}
}

void Renderer::RecordScenePass(RHI::IDevice* device)
{
	RHI::ICommandList* commandList = device->AcquireCommandList();
	RHI::ITexture* backBuffer = device->GetBackBuffer();
	if(commandList == nullptr || backBuffer == nullptr || m_mainPipeline == nullptr) return;

	commandList->SetRenderTargets(1, &backBuffer, nullptr);
	commandList->ClearColor(backBuffer, m_config.clearColor.x, m_config.clearColor.y, m_config.clearColor.z, m_config.clearColor.w);
	commandList->BindGraphicsPipeline(m_mainPipeline);
	commandList->BindGlobalDescriptorHeap();

	for(const DrawBatch& batch : m_drawBatches)
	{
		if(batch.meshIndex >= m_gpuMeshes.size()) continue;

		const GpuMesh& mesh = m_gpuMeshes[batch.meshIndex];
		if(mesh.vertexBuffer == nullptr) continue;

		commandList->BindVertexBuffer(mesh.vertexBuffer);
		if(batch.indexed && mesh.indexBuffer != nullptr)
		{
			commandList->BindIndexBuffer(mesh.indexBuffer, RHI::Format::R32_UINT, mesh.indexOffset);
		}

		if(batch.indexed && mesh.indexBuffer != nullptr)
		{
			commandList->DrawIndexedInstanced(mesh.indexCount, batch.itemCount, 0, 0, 0);
		}
		else
		{
			commandList->DrawInstanced(mesh.vertexCount, batch.itemCount, 0, 0);
		}
	}

	commandList->Close();

	std::array<RHI::ICommandList*, 1> commandLists = { commandList };
	device->Submit(commandLists.data(), static_cast<uint32_t>(commandLists.size()));
}
