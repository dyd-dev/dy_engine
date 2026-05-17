#include "Graphics/ObjectLoader.h"

#include "Graphics/AssetManager.h"
#include "Graphics/Mesh.h"
#include "Graphics/Scene.h"

namespace dy::Graphics
{
	namespace
	{
		void LoadTexture(AssetManager& assets, const std::string& texturePath, TextureID& outTexture)
		{
			if(texturePath.empty()) return;
			const TextureID texture = assets.LoadTexture(texturePath);
			if(IsValid(texture))
			{
				outTexture = texture;
			}
		}

		[[nodiscard]] ObjectLoadResult LoadAsObject(
			AssetManager& assets,
			const std::string& filepath,
			const MaterialDesc& material,
			const Math::float4x4& worldMatrix,
			const RenderFlags& renderFlags)
		{
			MaterialImportData materialData;
			std::string resolvedPath;
			const MeshID meshId = assets.LoadMesh(filepath, &materialData, &resolvedPath);
			if(!IsValid(meshId))
			{
				return {};
			}

			Material resolvedMaterial(material);
			materialData.ApplyTo(resolvedMaterial);
			LoadTexture(assets, materialData.texturePaths.baseColorTexture, resolvedMaterial.baseColorTexture);
			LoadTexture(assets, materialData.texturePaths.metallicRoughnessTexture, resolvedMaterial.metallicRoughnessTexture);
			LoadTexture(assets, materialData.texturePaths.normalTexture, resolvedMaterial.normalTexture);
			LoadTexture(assets, materialData.texturePaths.occlusionTexture, resolvedMaterial.occlusionTexture);
			LoadTexture(assets, materialData.texturePaths.emissiveTexture, resolvedMaterial.emissiveTexture);

			Scene& scene = assets.GetScene();
			const MaterialID materialId = assets.GetOrCreateMaterial(resolvedMaterial);
			const EntityID entityId = scene.CreateEntity(meshId, materialId, worldMatrix, renderFlags);

			ObjectLoadResult result = {};
			result.succeeded = true;
			result.entity = entityId;
			result.mesh = meshId;
			result.material = materialId;
			result.resolvedPath = resolvedPath;
			result.texturePath = materialData.texturePaths.baseColorTexture;
			result.texturePaths = materialData.texturePaths;
			return result;
		}
	}

	ObjectLoadResult ObjectLoader::Load(
		Scene& scene,
		const std::string& filepath,
		const MaterialDesc& material,
		const Math::float4x4& worldMatrix,
		const RenderFlags& renderFlags)
	{
		AssetManager assets(scene);
		return Load(assets, filepath, material, worldMatrix, renderFlags);
	}

	ObjectLoadResult ObjectLoader::Load(
		AssetManager& assets,
		const std::string& filepath,
		const MaterialDesc& material,
		const Math::float4x4& worldMatrix,
		const RenderFlags& renderFlags)
	{
		return LoadAsObject(assets, filepath, material, worldMatrix, renderFlags);
	}

	ObjectLoadResult ObjectLoader::LoadOBJ(
		Scene& scene,
		const std::string& filepath,
		const MaterialDesc& material,
		const Math::float4x4& worldMatrix,
		const RenderFlags& renderFlags)
	{
		AssetManager assets(scene);
		return LoadOBJ(assets, filepath, material, worldMatrix, renderFlags);
	}

	ObjectLoadResult ObjectLoader::LoadOBJ(
		AssetManager& assets,
		const std::string& filepath,
		const MaterialDesc& material,
		const Math::float4x4& worldMatrix,
		const RenderFlags& renderFlags)
	{
		return LoadAsObject(assets, filepath, material, worldMatrix, renderFlags);
	}

	ObjectLoadResult ObjectLoader::LoadFBX(
		Scene& scene,
		const std::string& filepath,
		const MaterialDesc& material,
		const Math::float4x4& worldMatrix,
		const RenderFlags& renderFlags)
	{
		AssetManager assets(scene);
		return LoadFBX(assets, filepath, material, worldMatrix, renderFlags);
	}

	ObjectLoadResult ObjectLoader::LoadFBX(
		AssetManager& assets,
		const std::string& filepath,
		const MaterialDesc& material,
		const Math::float4x4& worldMatrix,
		const RenderFlags& renderFlags)
	{
		return LoadAsObject(assets, filepath, material, worldMatrix, renderFlags);
	}
}
