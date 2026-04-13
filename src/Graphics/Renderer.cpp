#include "Renderer.h"
#include "RHI/IDevice.h"
#include "RHI/ICommandList.h"
#include "RHI/IPipelineState.h"
#include "RHI/IBuffer.h"
#include "Graphics/Scene.h"
#include "Graphics/JobSystem.h"

namespace dy::Graphics
{
	struct PushConstants
	{
		uint32_t entityIndex;
	};
}

using namespace dy::Graphics;

void Renderer::Initialize(RHI::IDevice* device)
{
	RHI::BufferDesc transformDesc = {};
	transformDesc.size = MAX_SUPPORTED_ENTITIES * sizeof(TransformData) * 3;
	transformDesc.usage = RHI::BufferUsage::None; //dynamic

	RHI::BufferDesc materialDesc = {};
	materialDesc.size = MAX_SUPPORTED_ENTITIES * sizeof(MaterialData) * 3;
	materialDesc.usage = RHI::BufferUsage::None; // dynamic

	m_globalTransformBuffer = device->CreateBuffer(transformDesc);
	m_globalMaterialBuffer = device->CreateBuffer(materialDesc);

	BuildPipelineStates(device);
}

void Renderer::Shutdown(RHI::IDevice* device)
{
	device->DestroyBuffer(m_globalTransformBuffer);
	device->DestroyBuffer(m_globalMaterialBuffer);
	// pso...
}

void Renderer::SubmitFrame(const Scene& scene, RHI::IDevice* device, JobSystem* jobSystem)
{
	uint32_t activeCount = scene.GetActiveCount();
	if(activeCount == 0) return;

	uint32_t frameIdx = device->GetCurrentFrameIndex(); // 0, 1, or 2

	// 1. Triple-Buffering Memory Mapping (Zero-Copy Upload)
	// Calculate offsets to avoid overwriting memory currently read by the GPU
	uint32_t transformOffset = frameIdx * MAX_SUPPORTED_ENTITIES * sizeof(TransformData);
	uint32_t materialOffset  = frameIdx * MAX_SUPPORTED_ENTITIES * sizeof(MaterialData);

	void* mappedTransforms = m_globalTransformBuffer->Map(transformOffset, activeCount * sizeof(TransformData));
	std::memcpy(mappedTransforms, scene.GetTransformArray(), activeCount * sizeof(TransformData));
	m_globalTransformBuffer->Unmap();

	void* mappedMaterials = m_globalMaterialBuffer->Map(materialOffset, activeCount * sizeof(MaterialData));
	std::memcpy(mappedMaterials, scene.GetMaterialArray(), activeCount * sizeof(MaterialData));
	m_globalMaterialBuffer->Unmap();

	// 2. Prepare Multithreaded Command Recording
	const uint32_t numThreads = jobSystem->GetWorkerCount();
	const uint32_t chunkSize = (activeCount + numThreads - 1) / numThreads;

	std::vector<RHI::ICommandList*> recordedCmdLists(numThreads, nullptr);
	const MeshID* meshes = scene.GetMeshArray();

	// 3. Dispatch Lock-Free Recording Tasks
	jobSystem->ParallelDispatch(numThreads, [=, &recordedCmdLists](uint32_t threadIdx) 
	{
		uint32_t startIdx = threadIdx * chunkSize;
		uint32_t endIdx = std::min(startIdx + chunkSize, activeCount);

		if (startIdx >= endIdx) return;

		// Fetch thread-local command list (No Mutex!)
		RHI::ICommandList* cmd = device->AcquireCommandList();

		cmd->BindGraphicsPipeline(m_opaquePSO);
		cmd->BindGlobalDescriptorHeap(); 

		// High-performance DOD iteration
		MeshID currentBoundMesh = MeshID::Invalid;

		for (uint32_t i = startIdx; i < endIdx; ++i) 
		{
			MeshID meshId = meshes[i];
			if (meshId == MeshID::Invalid) continue;

			// Bind Index Buffer ONLY if it changes
			if (meshId != currentBoundMesh) 
			{
				// RenderMesh* meshData = AssetManager::GetRenderMesh(meshId);
				// cmd->BindIndexBuffer(meshData->indexBuffer, FORMAT_R32_UINT, 0);
				currentBoundMesh = meshId;
			}

			// Inject 32-bit DOD Index directly into registers
			PushConstants pc = { i };
			cmd->SetPushConstants(sizeof(PushConstants), &pc);

			// Draw
			// cmd->DrawInstanced(meshData->indexCount, 1, 0, 0);
		}

		cmd->Close();
		recordedCmdLists[threadIdx] = cmd;
	});

	// 4. Synchronization and Submission
	jobSystem->WaitForAll();

	std::vector<RHI::ICommandList*> finalCmdLists;
	finalCmdLists.reserve(numThreads);
	for (auto* cmd : recordedCmdLists)
	{
		if (cmd) finalCmdLists.push_back(cmd);
	}

	device->Submit(finalCmdLists.data(), static_cast<uint32_t>(finalCmdLists.size()));
}

void Renderer::BuildPipelineStates(RHI::IDevice* device)
{
}