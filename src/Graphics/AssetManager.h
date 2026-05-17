#pragma once

#include "Core/Types.h"
#include "Graphics/MaterialTexturePaths.h"
#include "Graphics/Mesh.h"
#include "Graphics/ObjectLoader.h"

#include <string>
#include <unordered_map>

namespace dy::Graphics
{
	class Scene;

	struct CachedObjectData
	{
		MeshData meshData;
		MaterialImportData materialData;
		std::string resolvedPath;
		MeshID mesh = MeshID::Invalid;
	};

	class AssetManager
	{
	public:
		explicit AssetManager(Scene& scene);

		[[nodiscard]] Scene& GetScene() const;
		[[nodiscard]] TextureID LoadTexture(const std::string& filepath);
		[[nodiscard]] MeshID LoadMesh(
			const std::string& filepath,
			MaterialImportData* outMaterialData = nullptr,
			std::string* outResolvedPath = nullptr);
		[[nodiscard]] MaterialID CreateMaterial(const MaterialDesc& material);
		[[nodiscard]] MaterialID GetOrCreateMaterial(const MaterialDesc& material);
		[[nodiscard]] ObjectLoadResult LoadObject(
			const std::string& filepath,
			const MaterialDesc& material = MaterialDesc{},
			const Math::float4x4& worldMatrix = Math::float4x4::Identity(),
			const RenderFlags& renderFlags = {});
		[[nodiscard]] ObjectLoadResult LoadOBJ(
			const std::string& filepath,
			const MaterialDesc& material = MaterialDesc{},
			const Math::float4x4& worldMatrix = Math::float4x4::Identity(),
			const RenderFlags& renderFlags = {});
		[[nodiscard]] ObjectLoadResult LoadFBX(
			const std::string& filepath,
			const MaterialDesc& material = MaterialDesc{},
			const Math::float4x4& worldMatrix = Math::float4x4::Identity(),
			const RenderFlags& renderFlags = {});
		[[nodiscard]] bool LoadObjectData(
			const std::string& filepath,
			MeshData& outMeshData,
			MaterialImportData& outMaterialData,
			std::string* outResolvedPath = nullptr);

		void Clear();

	private:
		[[nodiscard]] bool FindOrLoadObjectData(const std::string& filepath, CachedObjectData*& outData);

		Scene* m_scene = nullptr;
		std::unordered_map<std::string, TextureID> m_textureCache;
		std::unordered_map<std::string, MaterialID> m_materialCache;
		std::unordered_map<std::string, CachedObjectData> m_objectCache;
	};
}
