#pragma once
#include <string>
#include <utility>
#include <vector>

#include "Material.h"
#include "Mesh.h"
#include "Texture.h"

namespace dy::Graphics
{
	struct RenderFlags
	{
		bool castShadow = true;
		bool receiveShadow = true;
	};

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

	struct alignas(16) Transform
	{
		Math::float4x4 worldMatrix;
	};

	struct alignas(16) Camera
	{
		Math::float4x4 viewMatrix;
		Math::float4x4 projectionMatrix;
		Math::float3 worldPosition;
	};

	struct ModelData
	{
		std::vector<MeshData> meshes;
		std::vector<MaterialData> materials;
	};

	struct Scene
	{
		std::vector<Transform> m_entityTransforms;
		std::vector<MeshID> m_entityMeshes;
		std::vector<MaterialID> m_entityMaterials;
		std::vector<RenderFlags> m_entityRenderFlags;

		std::vector<MeshData> m_meshes;
		std::vector<MaterialData> m_materials;
		std::vector<TextureData> m_textures;
		std::vector<DirectionalLight> m_directionalLights;
		std::vector<PointLight> m_pointLights;

		[[nodiscard]] MeshID AddMesh(const MeshData& mesh)
		{
			const MeshID id = static_cast<MeshID>(m_meshes.size());
			m_meshes.push_back(mesh);
			return id;
		}

		[[nodiscard]] MeshID AddMesh(MeshData&& mesh)
		{
			const MeshID id = static_cast<MeshID>(m_meshes.size());
			m_meshes.push_back(std::move(mesh));
			return id;
		}

		[[nodiscard]] MaterialID AddMaterial(const MaterialData& material)
		{
			const MaterialID id = static_cast<MaterialID>(m_materials.size());
			m_materials.push_back(material);
			return id;
		}

		[[nodiscard]] MaterialID AddMaterial(const Math::float4& baseColor, TextureID baseColorTex = TextureID::Invalid, float metallic = 0.0f, float roughness = 1.0f)
		{
			MaterialData material = {};
			material.baseColor = baseColor;
			material.baseColorTex = baseColorTex;
			material.metallic = metallic;
			material.roughness = roughness;
			return AddMaterial(material);
		}

		[[nodiscard]] TextureID AddTexture(const TextureData& texture)
		{
			const TextureID id = static_cast<TextureID>(m_textures.size());
			m_textures.push_back(texture);
			return id;
		}

		[[nodiscard]] TextureID AddTexture(std::string sourcePath)
		{
			TextureData texture = {};
			texture.sourcePath = std::move(sourcePath);
			return AddTexture(texture);
		}

		EntityID CreateEntity(MeshID mesh, MaterialID material, const Math::float4x4& worldMatrix = Math::float4x4::Identity(), const RenderFlags& renderFlags = {})
		{
			if(!IsValid(mesh) || !IsValid(material)) return EntityID::Invalid;
			if(ToIndex(mesh) >= m_meshes.size() || ToIndex(material) >= m_materials.size()) return EntityID::Invalid;

			const EntityID id = static_cast<EntityID>(m_entityMeshes.size());
			m_entityMeshes.push_back(mesh);
			m_entityMaterials.push_back(material);
			m_entityTransforms.push_back(Transform{ worldMatrix });
			m_entityRenderFlags.push_back(renderFlags);
			return id;
		}

		[[nodiscard]] uint32_t AddDirectionalLight(const DirectionalLight& light)
		{
			m_directionalLights.push_back(light);
			return static_cast<uint32_t>(m_directionalLights.size() - 1u);
		}

		[[nodiscard]] uint32_t AddDirectionalLight(
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
			return AddDirectionalLight(light);
		}

		[[nodiscard]] uint32_t AddPointLight(const PointLight& light)
		{
			m_pointLights.push_back(light);
			return static_cast<uint32_t>(m_pointLights.size() - 1u);
		}
	};
}
