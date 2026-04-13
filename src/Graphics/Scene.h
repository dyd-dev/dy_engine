#pragma once
#include <vector>
#include <cstdint>
#include "Types.h"

namespace dy
{
	// SOA (Structure of Arrays)
	class Scene
	{
	private:
		std::vector<TransformData> m_transforms;
		std::vector<MaterialData>  m_materials;
		std::vector<MeshID>        m_meshes;

		// Indirection tables for O(1) Swap-and-Pop deletion
		std::vector<uint32_t> m_entityToIndex; // Sparse array: EntityID -> Array Index
		std::vector<EntityID> m_indexToEntity; // Dense array: Array Index -> EntityID
		
		uint32_t m_activeCount = 0;
	
	public:
		~Scene() = default;

		[[nodiscard]] EntityID CreateEntity()
		{
			EntityID newId = static_cast<EntityID>(m_entityToIndex.size());
			uint32_t index = m_activeCount++;

			m_entityToIndex.push_back(index);

			if(index >= m_indexToEntity.size())
			{
				m_indexToEntity.push_back(newId);
				m_transforms.emplace_back();
				m_materials.emplace_back();
				m_meshes.emplace_back(MeshID::Invalid);
			}
			else m_indexToEntity[index] = newId;
			
			return newId;
		}
		void DestroyEntity(EntityID entity)
		{
			uint32_t deletedIndex = m_entityToIndex[static_cast<uint32_t>(entity)];
			uint32_t lastIndex = m_activeCount - 1;

			if(deletedIndex != lastIndex)
			{
				// Swap and Pop: Move the last element to the deleted slot
				m_transforms[deletedIndex] = m_transforms[lastIndex];
				m_materials[deletedIndex]  = m_materials[lastIndex];
				m_meshes[deletedIndex]     = m_meshes[lastIndex];

				EntityID lastEntity = m_indexToEntity[lastIndex];
				m_entityToIndex[static_cast<uint32_t>(lastEntity)] = deletedIndex;
				m_indexToEntity[deletedIndex] = lastEntity;
			}

			m_entityToIndex[static_cast<uint32_t>(entity)] = 0xFFFFFFFF;
			m_activeCount--;
		}

		inline uint32_t GetActiveCount() const { return m_activeCount; }

		[[nodiscard]] const TransformData* GetTransformArray() const { return m_transforms.data(); }
		[[nodiscard]] TransformData& GetTransform(EntityID entity)
		{
			uint32_t index = m_entityToIndex[static_cast<uint32_t>(entity)];
			return m_transforms[index];
		}
		[[nodiscard]] const MaterialData* GetMaterialArray() const { return m_materials.data(); }
		[[nodiscard]] MaterialData& GetMaterial(EntityID entity)
		{
			uint32_t index = m_entityToIndex[static_cast<uint32_t>(entity)];
			return m_materials[index];
		}
		[[nodiscard]] const MeshID* GetMeshArray() const { return m_meshes.data(); }
		[[nodiscard]] MeshID& GetMesh(EntityID entity) 
		{
			uint32_t index = m_entityToIndex[static_cast<uint32_t>(entity)];
			return m_meshes[index];
		}
	};
}