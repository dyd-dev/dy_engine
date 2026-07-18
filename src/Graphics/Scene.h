#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Core/Types.h"
#include "Graphics/Lighting.h"
#include "Graphics/Mesh.h"

namespace dy::Graphics
{
	class Scene
	{
	public:
		[[nodiscard]] TextureID CreateTexture(const TextureAsset& texture)
		{
			m_textures.push_back(texture);
			return static_cast<TextureID>(m_textures.size() - 1u);
		}
		[[nodiscard]] TextureID CreateTexture(
			std::string sourcePath,
			TextureColorSpace colorSpace = TextureColorSpace::SRGB)
		{
			TextureAsset texture;
			texture.sourcePath = std::move(sourcePath);
			texture.colorSpace = colorSpace;
			return CreateTexture(texture);
		}
		[[nodiscard]] MaterialID CreateMaterial(const MaterialDesc& material)
		{
			m_materials.push_back(material);
			return static_cast<MaterialID>(m_materials.size() - 1u);
		}
		[[nodiscard]] MeshID CreateMesh(const MeshData& mesh)
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
		// 라이트는 기본값이 충실한 Desc 구조체로 생성한다(확장-파라미터 오버로드는 제거).
		[[nodiscard]] DirectionalLightID CreateDirectionalLight(const DirectionalLight& light)
		{
			m_directionalLights.push_back(light);
			return static_cast<DirectionalLightID>(m_directionalLights.size() - 1u);
		}
		[[nodiscard]] PointLightID CreatePointLight(const PointLight& light)
		{
			m_pointLights.push_back(light);
			return static_cast<PointLightID>(m_pointLights.size() - 1u);
		}
		[[nodiscard]] SpotLightID CreateSpotLight(const SpotLight& light)
		{
			m_spotLights.push_back(light);
			return static_cast<SpotLightID>(m_spotLights.size() - 1u);
		}
		[[nodiscard]] RectAreaLightID CreateRectAreaLight(const RectAreaLight& light)
		{
			m_rectAreaLights.push_back(light);
			return static_cast<RectAreaLightID>(m_rectAreaLights.size() - 1u);
		}
		[[nodiscard]] DiscAreaLightID CreateDiscAreaLight(const DiscAreaLight& light)
		{
			m_discAreaLights.push_back(light);
			return static_cast<DiscAreaLightID>(m_discAreaLights.size() - 1u);
		}

		[[nodiscard]] uint32_t GetTextureCount() const { return static_cast<uint32_t>(m_textures.size()); }
		[[nodiscard]] uint32_t GetMaterialCount() const { return static_cast<uint32_t>(m_materials.size()); }
		[[nodiscard]] uint32_t GetMeshCount() const { return static_cast<uint32_t>(m_meshes.size()); }
		[[nodiscard]] uint32_t GetEntityCount() const { return static_cast<uint32_t>(m_entityMeshes.size()); }
		[[nodiscard]] uint32_t GetDirectionalLightCount() const { return static_cast<uint32_t>(m_directionalLights.size()); }
		[[nodiscard]] uint32_t GetPointLightCount() const { return static_cast<uint32_t>(m_pointLights.size()); }
		[[nodiscard]] uint32_t GetSpotLightCount() const { return static_cast<uint32_t>(m_spotLights.size()); }
		[[nodiscard]] uint32_t GetRectAreaLightCount() const { return static_cast<uint32_t>(m_rectAreaLights.size()); }
		[[nodiscard]] uint32_t GetDiscAreaLightCount() const { return static_cast<uint32_t>(m_discAreaLights.size()); }

		[[nodiscard]] const TextureAsset& GetTexture(TextureID textureId) const
		{
			return m_textures[ToIndex(textureId)];
		}
		[[nodiscard]] const MaterialDesc& GetMaterial(MaterialID materialId) const
		{
			return m_materials[ToIndex(materialId)];
		}
		void SetMaterial(MaterialID materialId, const MaterialDesc& material)
		{
			m_materials[ToIndex(materialId)] = material;
		}
		[[nodiscard]] const MeshData& GetMesh(MeshID meshId) const
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
		[[nodiscard]] const DirectionalLight& GetDirectionalLight(uint32_t lightIndex) const
		{
			return m_directionalLights[lightIndex];
		}
		void SetDirectionalLight(uint32_t lightIndex, const DirectionalLight& light)
		{
			m_directionalLights[lightIndex] = light;
		}
		[[nodiscard]] const PointLight& GetPointLight(uint32_t lightIndex) const
		{
			return m_pointLights[lightIndex];
		}
		void SetPointLight(uint32_t lightIndex, const PointLight& light)
		{
			m_pointLights[lightIndex] = light;
		}
		[[nodiscard]] const SpotLight& GetSpotLight(uint32_t lightIndex) const
		{
			return m_spotLights[lightIndex];
		}
		void SetSpotLight(uint32_t lightIndex, const SpotLight& light)
		{
			m_spotLights[lightIndex] = light;
		}
		[[nodiscard]] const RectAreaLight& GetRectAreaLight(uint32_t lightIndex) const
		{
			return m_rectAreaLights[lightIndex];
		}
		void SetRectAreaLight(uint32_t lightIndex, const RectAreaLight& light)
		{
			m_rectAreaLights[lightIndex] = light;
		}
		[[nodiscard]] const DiscAreaLight& GetDiscAreaLight(uint32_t lightIndex) const
		{
			return m_discAreaLights[lightIndex];
		}
		void SetDiscAreaLight(uint32_t lightIndex, const DiscAreaLight& light)
		{
			m_discAreaLights[lightIndex] = light;
		}

		// ---- 연속 메모리 뷰 (DOD/SIMD 일괄 처리용) ----------------------------------
		[[nodiscard]] const std::vector<MeshID>& EntityMeshes() const { return m_entityMeshes; }
		[[nodiscard]] const std::vector<MaterialID>& EntityMaterials() const { return m_entityMaterials; }
		[[nodiscard]] const std::vector<Transform>& Transforms() const { return m_entityTransforms; }
		[[nodiscard]] std::vector<Transform>& TransformsMutable() { return m_entityTransforms; }
		[[nodiscard]] const std::vector<RenderFlags>& EntityRenderFlags() const { return m_entityRenderFlags; }
		[[nodiscard]] const std::vector<MeshData>& Meshes() const { return m_meshes; }
		[[nodiscard]] const std::vector<MaterialDesc>& Materials() const { return m_materials; }
		[[nodiscard]] const std::vector<TextureAsset>& Textures() const { return m_textures; }
		[[nodiscard]] const std::vector<DirectionalLight>& DirectionalLights() const { return m_directionalLights; }
		[[nodiscard]] const std::vector<PointLight>& PointLights() const { return m_pointLights; }
		[[nodiscard]] const std::vector<SpotLight>& SpotLights() const { return m_spotLights; }
		[[nodiscard]] const std::vector<RectAreaLight>& RectAreaLights() const { return m_rectAreaLights; }
		[[nodiscard]] const std::vector<DiscAreaLight>& DiscAreaLights() const { return m_discAreaLights; }

	private:
		std::vector<TextureAsset> m_textures;
		std::vector<MaterialDesc> m_materials;
		std::vector<MeshData> m_meshes;
		std::vector<MeshID> m_entityMeshes;
		std::vector<MaterialID> m_entityMaterials;
		std::vector<Transform> m_entityTransforms;
		std::vector<RenderFlags> m_entityRenderFlags;
		std::vector<DirectionalLight> m_directionalLights;
		std::vector<PointLight> m_pointLights;
		std::vector<SpotLight> m_spotLights;
		std::vector<RectAreaLight> m_rectAreaLights;
		std::vector<DiscAreaLight> m_discAreaLights;
	};
}
