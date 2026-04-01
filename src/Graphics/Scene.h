#pragma once
#include "Math/Math.h"
#include <cstdint>
#include <vector>
/*

Cache Line (L1 Cache Line)
*/

namespace dy::Graphics
{
	// DOD Structure of Arrays (SoA) layout
	// Extremely friendly to CPU caches and multi-threading.
	class Scene
	{
	public:
		uint32_t capacity = 0; // Memory capacity
		uint32_t entityCount = 0;

		std::vector<bool> activeFlags;
		std::vector<int32_t> parentIndices; // -1 means no parent (root level)

		std::vector<Math::Matrix4x4> localTransforms;
		std::vector<Math::Matrix4x4> worldTransforms;
		
		std::vector<uint32_t> meshIndices;
		std::vector<uint32_t> materialIndices;
		
		void Initialize(uint32_t maxEntities);
		uint32_t AddEntity(uint32_t meshId, uint32_t materialId, int32_t parentIndex = -1);

		void UpdateWorldTransforms();
	};
}