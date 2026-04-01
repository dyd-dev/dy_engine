#include "Scene.h"

using namespace dy::Graphics;

void Scene::Initialize(uint32_t maxEntities)
{
	capacity = maxEntities;
	entityCount = 0;

	// Pre-allocate contiguous memory to prevent runtime reallocations
	activeFlags.resize(capacity, false);
	parentIndices.resize(capacity, -1);
	localTransforms.resize(capacity, Math::Matrix4x4::Identity());
	worldTransforms.resize(capacity, Math::Matrix4x4::Identity());
	meshIndices.resize(capacity, 0);
	materialIndices.resize(capacity, 0);
}

uint32_t Scene::AddEntity(uint32_t meshId, uint32_t materialId, int32_t parentIndex) 
{
	// In a real engine, you would find the first inactive flag or use a free-list.
	// Appending sequentially for demonstration.
	uint32_t id = entityCount++;
	
	activeFlags[id] = true;
	parentIndices[id] = parentIndex;
	localTransforms[id] = Math::Matrix4x4::Identity();
	worldTransforms[id] = Math::Matrix4x4::Identity();
	meshIndices[id] = meshId;
	materialIndices[id] = materialId;

	return id;
}

void Scene::UpdateWorldTransforms() 
{
	// Phase 1: Update root entities (no parents)
	for (uint32_t i = 0; i < entityCount; ++i)
		if (activeFlags[i] && parentIndices[i] == -1)
			worldTransforms[i] = localTransforms[i];

	// 여기 정렬되어 있어야 하는데 어쩔래? 위상 정렬 상태를 오버헤드 없이 어떻게?
	
	// Phase 2: Update child entities
	// NOTE: This assumes a topological sort where parent indices are strictly less than child indices.
	for (uint32_t i = 0; i < entityCount; ++i) 
	{
		if (activeFlags[i] && parentIndices[i] != -1) 
		{
			int32_t parentId = parentIndices[i];
			// World = ParentWorld * Local
			worldTransforms[i] = worldTransforms[parentId].Multiply(localTransforms[i]);
		}
	}
}