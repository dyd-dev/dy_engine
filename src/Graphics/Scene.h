#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Core/Types.h"
#include "Graphics/Mesh.h"

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
		[[nodiscard]] TextureID CreateTexture(const TextureAsset& texture)
		{
			for(size_t textureIndex = 0; textureIndex < m_textures.size(); ++textureIndex)
			{
				const TextureAsset& existing = m_textures[textureIndex];
				const bool samePath = !texture.sourcePath.empty()
					&& texture.sourcePath == existing.sourcePath;
				const bool samePixels = texture.sourcePath.empty()
					&& existing.sourcePath.empty()
					&& !texture.rgba8.empty()
					&& texture.width == existing.width
					&& texture.height == existing.height
					&& texture.rgba8 == existing.rgba8;
				if(samePath || samePixels) return static_cast<TextureID>(textureIndex);
			}
			m_textures.push_back(texture);
			return static_cast<TextureID>(m_textures.size() - 1u);
		}
		[[nodiscard]] TextureID CreateTexture(std::string sourcePath)
		{
			return CreateTexture(TextureAsset{ std::move(sourcePath) });
		}
		[[nodiscard]] MaterialID CreateMaterial(const MaterialDesc& material)
		{
			m_materials.push_back(material);
			return static_cast<MaterialID>(m_materials.size() - 1u);
		}
		[[nodiscard]] MeshID CreateMesh(const MeshData& mesh)
		{
			m_meshes.push_back(mesh);
			m_meshRevisions.push_back(++m_nextMeshRevision);
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
			m_entitySkinPaletteOffsets.push_back(kInvalidAnimationIndex);
			m_entityMorphedMeshes.emplace_back();
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

		[[nodiscard]] ModelInstanceID CreateModelInstance(ModelInstance instance);
		[[nodiscard]] ModelAssetID CreateModelAsset(ModelAsset asset)
		{
			m_modelAssets.push_back(std::move(asset));
			return static_cast<ModelAssetID>(m_modelAssets.size() - 1u);
		}
		[[nodiscard]] ModelAssetID FindModelAsset(const std::string& cacheKey) const
		{
			for(size_t assetIndex = 0; assetIndex < m_modelAssets.size(); ++assetIndex)
				if(m_modelAssets[assetIndex].cacheKey == cacheKey) return static_cast<ModelAssetID>(assetIndex);
			return ModelAssetID::Invalid;
		}
		[[nodiscard]] bool IsValidModelAsset(ModelAssetID asset) const
		{
			return IsValid(asset) && ToIndex(asset) < m_modelAssets.size();
		}
		[[nodiscard]] const ModelAsset* TryGetModelAsset(ModelAssetID asset) const
		{
			return IsValidModelAsset(asset) ? &m_modelAssets[ToIndex(asset)] : nullptr;
		}
		[[nodiscard]] const ModelAsset& GetModelAsset(ModelAssetID asset) const
		{
			return m_modelAssets[ToIndex(asset)];
		}
		[[nodiscard]] bool BindEntityToModel(
			ModelInstanceID instance,
			EntityID entity,
			uint32_t nodeIndex,
			uint32_t skinIndex = kInvalidAnimationIndex,
			uint32_t assetMeshIndex = kInvalidAnimationIndex);
		AnimationUpdateReport UpdateAnimations(float deltaSeconds);
		[[nodiscard]] bool PlayAnimation(ModelInstanceID instance, uint32_t clipIndex, bool loop = true);
		[[nodiscard]] bool SetAnimationPaused(ModelInstanceID instance, bool paused);
		[[nodiscard]] bool SetAnimationSpeed(ModelInstanceID instance, float speed);
		[[nodiscard]] bool SetAnimationLoop(ModelInstanceID instance, bool loop);
		[[nodiscard]] bool GetAnimationPlayback(ModelInstanceID instance, AnimationPlayback& outPlayback) const;
		[[nodiscard]] bool SetMorphWeight(
			ModelInstanceID instance,
			uint32_t nodeIndex,
			uint32_t targetIndex,
			float weight);
		[[nodiscard]] bool IsValidModelInstance(ModelInstanceID instance) const;
		[[nodiscard]] const ModelInstance* TryGetModelInstance(ModelInstanceID instance) const;
		[[nodiscard]] ModelInstance* TryGetModelInstance(ModelInstanceID instance);
		[[nodiscard]] const ModelInstance& GetModelInstance(ModelInstanceID instance) const;
		[[nodiscard]] ModelInstance& GetModelInstance(ModelInstanceID instance);
		[[nodiscard]] bool IsEntitySkinned(EntityID entity) const;
		[[nodiscard]] uint32_t GetEntitySkinPaletteOffset(EntityID entity) const;
		[[nodiscard]] const MeshData* TryGetEntityMorphedMesh(EntityID entity) const;
		[[nodiscard]] const std::vector<SkinJointMatrices>& JointPaletteMatrices() const { return m_jointPaletteMatrices; }

		[[nodiscard]] uint32_t GetTextureCount() const { return static_cast<uint32_t>(m_textures.size()); }
		[[nodiscard]] uint32_t GetMaterialCount() const { return static_cast<uint32_t>(m_materials.size()); }
		[[nodiscard]] uint32_t GetMeshCount() const { return static_cast<uint32_t>(m_meshes.size()); }
		[[nodiscard]] uint32_t GetModelAssetCount() const { return static_cast<uint32_t>(m_modelAssets.size()); }
		[[nodiscard]] uint64_t GetMeshCollectionRevision() const { return m_nextMeshRevision; }
		[[nodiscard]] uint32_t GetEntityCount() const { return static_cast<uint32_t>(m_entityMeshes.size()); }
		[[nodiscard]] uint32_t GetDirectionalLightCount() const { return static_cast<uint32_t>(m_directionalLights.size()); }
		[[nodiscard]] uint32_t GetPointLightCount() const { return static_cast<uint32_t>(m_pointLights.size()); }

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
		[[nodiscard]] uint64_t GetMeshRevision(MeshID meshId) const
		{
			return m_meshRevisions[ToIndex(meshId)];
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

	private:
		std::vector<TextureAsset> m_textures;
		std::vector<MaterialDesc> m_materials;
		std::vector<MeshData> m_meshes;
		std::vector<uint64_t> m_meshRevisions;
		uint64_t m_nextMeshRevision = 0u;
		std::vector<MeshID> m_entityMeshes;
		std::vector<MaterialID> m_entityMaterials;
		std::vector<Transform> m_entityTransforms;
		std::vector<RenderFlags> m_entityRenderFlags;
		std::vector<uint32_t> m_entitySkinPaletteOffsets;
		std::vector<MeshData> m_entityMorphedMeshes;
		std::vector<DirectionalLight> m_directionalLights;
		std::vector<PointLight> m_pointLights;
		std::vector<ModelAsset> m_modelAssets;
		std::vector<ModelInstance> m_modelInstances;
		std::vector<SkinJointMatrices> m_jointPaletteMatrices;
	};
}
