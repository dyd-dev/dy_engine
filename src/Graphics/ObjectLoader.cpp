#include "Graphics/ObjectLoader.h"

#include "Core/ImageLoader.h"
#include "Graphics/FBXLoader.h"
#include "Graphics/Mesh.h"
#include "Graphics/OBJLoader.h"
#include "Graphics/Scene.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

namespace dy::Graphics
{
	namespace
	{
		enum class ObjectFormat
		{
			OBJ,
			FBX
		};

		[[nodiscard]] std::string ToLower(std::string value)
		{
			std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
			{
				return static_cast<char>(std::tolower(c));
			});
			return value;
		}

		[[nodiscard]] std::string BuildRelativeCandidate(const std::string& filepath, uint32_t parentDepth)
		{
			std::filesystem::path candidate;
			for (uint32_t depth = 0; depth < parentDepth; ++depth)
			{
				candidate /= "..";
			}
			candidate /= filepath;
			return candidate.lexically_normal().string();
		}

		[[nodiscard]] bool LoadMesh(ObjectFormat format, const std::string& filepath, MeshData& meshData, MaterialImportData& materialData)
		{
			switch (format)
			{
			case ObjectFormat::OBJ:
				return OBJLoader::Load(filepath, meshData, &materialData);
			case ObjectFormat::FBX:
				return FBXLoader::Load(filepath, meshData, &materialData);
			}
			return false;
		}

		void LoadTexture(Scene& scene, const std::string& texturePath, TextureID& outTexture)
		{
			if (texturePath.empty()) return;

			const Core::Image image = Core::LoadImageFromFile(texturePath);
			if (image.IsValid())
			{
				outTexture = scene.CreateTexture(image);
			}
		}

		[[nodiscard]] ObjectLoadResult LoadAsObject(
			Scene& scene,
			const std::string& filepath,
			ObjectFormat format,
			const Material& material,
			const Math::float4x4& worldMatrix,
			const RenderFlags& renderFlags)
		{
			MeshData meshData;
			MaterialImportData materialData;
			std::string resolvedPath;

			for (uint32_t parentDepth = 0; parentDepth <= 2; ++parentDepth)
			{
				const std::string candidate = BuildRelativeCandidate(filepath, parentDepth);
				if (LoadMesh(format, candidate, meshData, materialData))
				{
					resolvedPath = candidate;
					break;
				}
			}

			if (meshData.vertices.empty() || meshData.indices.empty())
			{
				return {};
			}

			Material resolvedMaterial = material;
			materialData.ApplyTo(resolvedMaterial);
			LoadTexture(scene, materialData.texturePaths.baseColorTexture, resolvedMaterial.baseColorTexture);
			LoadTexture(scene, materialData.texturePaths.metallicRoughnessTexture, resolvedMaterial.metallicRoughnessTexture);
			LoadTexture(scene, materialData.texturePaths.normalTexture, resolvedMaterial.normalTexture);
			LoadTexture(scene, materialData.texturePaths.occlusionTexture, resolvedMaterial.occlusionTexture);
			LoadTexture(scene, materialData.texturePaths.emissiveTexture, resolvedMaterial.emissiveTexture);

			const MeshID meshId = scene.CreateMesh(ToRenderMesh(meshData));
			const MaterialID materialId = scene.CreateMaterial(resolvedMaterial);
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
		const Material& material,
		const Math::float4x4& worldMatrix,
		const RenderFlags& renderFlags)
	{
		const std::string extension = ToLower(std::filesystem::path(filepath).extension().string());
		if (extension == ".obj")
		{
			return LoadOBJ(scene, filepath, material, worldMatrix, renderFlags);
		}
		if (extension == ".fbx")
		{
			return LoadFBX(scene, filepath, material, worldMatrix, renderFlags);
		}
		return {};
	}

	ObjectLoadResult ObjectLoader::LoadOBJ(
		Scene& scene,
		const std::string& filepath,
		const Material& material,
		const Math::float4x4& worldMatrix,
		const RenderFlags& renderFlags)
	{
		return LoadAsObject(scene, filepath, ObjectFormat::OBJ, material, worldMatrix, renderFlags);
	}

	ObjectLoadResult ObjectLoader::LoadFBX(
		Scene& scene,
		const std::string& filepath,
		const Material& material,
		const Math::float4x4& worldMatrix,
		const RenderFlags& renderFlags)
	{
		return LoadAsObject(scene, filepath, ObjectFormat::FBX, material, worldMatrix, renderFlags);
	}
}
