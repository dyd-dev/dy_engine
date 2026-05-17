#pragma once
#include <cstdint>
#include <vector>

#include "Core/Image.h"
#include "Core/Types.h"

namespace dy::Graphics
{
	struct DirectionalLight
	{
		Math::float3 direction = Math::float3(0.35f, 0.65f, 0.68f);
		Math::float3 color = Math::float3(1.0f, 0.94f, 0.82f);
		float intensity = 4.0f;
		bool castShadow = true;
		float shadowStrength = 0.45f;
	};

	struct PointLight
	{
		Math::float3 position = Math::float3(0.0f, 0.0f, 2.0f);
		float range = 6.0f;
		Math::float3 color = Math::float3(1.0f, 0.94f, 0.82f);
		float intensity = 6.0f;
		Math::float3 direction = Math::float3(0.0f, 0.0f, -1.0f);
		bool castShadow = true;
		float shadowStrength = 0.5f;
	};

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
		[[nodiscard]] MaterialID CreateMaterial(const MaterialDesc& material)
		{
			m_materials.emplace_back(material);
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
		[[nodiscard]] uint32_t CreateDirectionalLight(const DirectionalLight& light)
		{
			m_directionalLights.push_back(light);
			return static_cast<uint32_t>(m_directionalLights.size() - 1u);
		}
		[[nodiscard]] uint32_t CreateDirectionalLight(
			const Math::float3& direction,
			const Math::float3& color,
			float intensity,
			bool castShadow = true,
			float shadowStrength = 0.45f)
		{
			DirectionalLight light = {};
			light.direction = direction;
			light.color = color;
			light.intensity = intensity;
			light.castShadow = castShadow;
			light.shadowStrength = shadowStrength;
			return CreateDirectionalLight(light);
		}
		[[nodiscard]] uint32_t CreatePointLight(const PointLight& light)
		{
			m_pointLights.push_back(light);
			return static_cast<uint32_t>(m_pointLights.size() - 1u);
		}
		[[nodiscard]] uint32_t CreatePointLight(
			const Math::float3& position,
			float range,
			const Math::float3& color,
			float intensity,
			const Math::float3& direction = Math::float3(0.0f, 0.0f, -1.0f),
			bool castShadow = true,
			float shadowStrength = 0.5f)
		{
			PointLight light = {};
			light.position = position;
			light.range = range;
			light.color = color;
			light.intensity = intensity;
			light.direction = direction;
			light.castShadow = castShadow;
			light.shadowStrength = shadowStrength;
			return CreatePointLight(light);
		}

		[[nodiscard]] uint32_t GetTextureCount() const { return static_cast<uint32_t>(m_textureImages.size()); }
		[[nodiscard]] uint32_t GetMaterialCount() const { return static_cast<uint32_t>(m_materials.size()); }
		[[nodiscard]] uint32_t GetMeshCount() const { return static_cast<uint32_t>(m_meshes.size()); }
		[[nodiscard]] uint32_t GetEntityCount() const { return static_cast<uint32_t>(m_entityMeshes.size()); }
		[[nodiscard]] uint32_t GetDirectionalLightCount() const { return static_cast<uint32_t>(m_directionalLights.size()); }
		[[nodiscard]] uint32_t GetPointLightCount() const { return static_cast<uint32_t>(m_pointLights.size()); }

		[[nodiscard]] const Core::Image& GetTexture(TextureID textureId) const
		{
			return m_textureImages[ToIndex(textureId)];
		}
		[[nodiscard]] const Material& GetMaterial(MaterialID materialId) const
		{
			return m_materials[ToIndex(materialId)];
		}
		void SetMaterial(MaterialID materialId, const Material& material)
		{
			m_materials[ToIndex(materialId)] = material;
		}
		void SetMaterial(MaterialID materialId, const MaterialDesc& material)
		{
			m_materials[ToIndex(materialId)] = Material(material);
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

	private:
		std::vector<Core::Image> m_textureImages;
		std::vector<Material> m_materials;
		std::vector<dy::Mesh> m_meshes;
		std::vector<MeshID> m_entityMeshes;
		std::vector<MaterialID> m_entityMaterials;
		std::vector<Transform> m_entityTransforms;
		std::vector<RenderFlags> m_entityRenderFlags;
		std::vector<DirectionalLight> m_directionalLights;
		std::vector<PointLight> m_pointLights;
	};
}
