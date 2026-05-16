#pragma once
#include <cstdint>
#include <vector>

#include "Core/Image.h"
#include "Core/Types.h"

namespace dy::Graphics
{
	class Scene
	{
	public:
		[[nodiscard]] TextureID CreateTexture(const Core::Image& image)
		{
			m_textureImages.push_back(image);
			return static_cast<TextureID>(m_textureImages.size() - 1u);
		}
		[[nodiscard]] MaterialID CreateMaterial(const Material& material)
		{
			m_materials.push_back(material);
			return static_cast<MaterialID>(m_materials.size() - 1u);
		}
		[[nodiscard]] MeshID CreateMesh(const dy::Mesh& mesh)
		{
			m_meshes.push_back(mesh);
			return static_cast<MeshID>(m_meshes.size() - 1u);
		}
		[[nodiscard]] EntityID CreateEntity(
			MeshID mesh,
			MaterialID material,
			const Math::float4x4& worldMatrix = Math::float4x4::Identity(),
			const RenderFlags& renderFlags = {})
		{
			const EntityID entity = static_cast<EntityID>(m_entityMeshes.size());
			m_entityMeshes.push_back(mesh);
			m_entityMaterials.push_back(material);
			m_entityTransforms.push_back(Transform{ worldMatrix });
			m_entityRenderFlags.push_back(renderFlags);
			return entity;
		}

		[[nodiscard]] uint32_t GetTextureCount() const { return static_cast<uint32_t>(m_textureImages.size()); }
		[[nodiscard]] uint32_t GetMeshCount() const { return static_cast<uint32_t>(m_meshes.size()); }
		[[nodiscard]] uint32_t GetEntityCount() const { return static_cast<uint32_t>(m_entityMeshes.size()); }

		[[nodiscard]] const Core::Image& GetTexture(TextureID textureId) const
		{
			return m_textureImages[ToIndex(textureId)];
		}
		[[nodiscard]] const Material& GetMaterial(MaterialID materialId) const
		{
			return m_materials[ToIndex(materialId)];
		}
		[[nodiscard]] const dy::Mesh& GetMesh(MeshID meshId) const
		{
			return m_meshes[ToIndex(meshId)];
		}
		[[nodiscard]] MeshID GetEntityMesh(EntityID entityId) const
		{
			return m_entityMeshes[ToIndex(entityId)];
		}
		[[nodiscard]] MaterialID GetEntityMaterial(EntityID entityId) const
		{
			return m_entityMaterials[ToIndex(entityId)];
		}
		[[nodiscard]] const Transform& GetTransform(EntityID entityId) const
		{
			return m_entityTransforms[ToIndex(entityId)];
		}
		[[nodiscard]] Transform& GetTransform(EntityID entityId)
		{
			return m_entityTransforms[ToIndex(entityId)];
		}
		[[nodiscard]] const RenderFlags& GetRenderFlags(EntityID entityId) const
		{
			return m_entityRenderFlags[ToIndex(entityId)];
		}
		void SetRenderFlags(EntityID entityId, const RenderFlags& renderFlags)
		{
			m_entityRenderFlags[ToIndex(entityId)] = renderFlags;
		}

	private:
		std::vector<Core::Image> m_textureImages;
		std::vector<Material> m_materials;
		std::vector<dy::Mesh> m_meshes;
		std::vector<MeshID> m_entityMeshes;
		std::vector<MaterialID> m_entityMaterials;
		std::vector<Transform> m_entityTransforms;
		std::vector<RenderFlags> m_entityRenderFlags;
	};
}
