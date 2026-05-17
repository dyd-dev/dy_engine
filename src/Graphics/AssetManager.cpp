#include "Graphics/AssetManager.h"

#include "Core/ImageLoader.h"
#include "Graphics/FBXLoader.h"
#include "Graphics/ObjectLoader.h"
#include "Graphics/OBJLoader.h"
#include "Graphics/Scene.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <utility>

namespace dy::Graphics
{
	namespace
	{
		enum class ObjectFormat
		{
			OBJ,
			FBX
		};

		[[nodiscard]] std::string NormalizeAssetKey(const std::string& filepath)
		{
			return std::filesystem::path(filepath).lexically_normal().generic_string();
		}

		[[nodiscard]] std::string BuildMaterialKey(const MaterialDesc& material)
		{
			std::ostringstream stream;
			stream.precision(9);
			stream
				<< material.baseColor.x << ','
				<< material.baseColor.y << ','
				<< material.baseColor.z << ','
				<< material.baseColor.w << ','
				<< ToIndex(material.baseColorTexture) << ','
				<< material.emissiveColor.x << ','
				<< material.emissiveColor.y << ','
				<< material.emissiveColor.z << ','
				<< material.metallicFactor << ','
				<< material.roughnessFactor << ','
				<< material.normalScale << ','
				<< material.occlusionStrength << ','
				<< ToIndex(material.metallicRoughnessTexture) << ','
				<< ToIndex(material.normalTexture) << ','
				<< ToIndex(material.occlusionTexture) << ','
				<< ToIndex(material.emissiveTexture);
			return stream.str();
		}

		[[nodiscard]] std::string ToLower(std::string value)
		{
			std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
			{
				return static_cast<char>(std::tolower(c));
			});
			return value;
		}

		[[nodiscard]] bool GetObjectFormat(const std::string& filepath, ObjectFormat& outFormat)
		{
			const std::string extension = ToLower(std::filesystem::path(filepath).extension().string());
			if(extension == ".obj")
			{
				outFormat = ObjectFormat::OBJ;
				return true;
			}
			if(extension == ".fbx")
			{
				outFormat = ObjectFormat::FBX;
				return true;
			}
			return false;
		}

		[[nodiscard]] std::string BuildRelativeCandidate(const std::string& filepath, uint32_t parentDepth)
		{
			std::filesystem::path candidate;
			for(uint32_t depth = 0; depth < parentDepth; ++depth)
			{
				candidate /= "..";
			}
			candidate /= filepath;
			return candidate.lexically_normal().generic_string();
		}

		[[nodiscard]] bool LoadMeshData(ObjectFormat format, const std::string& filepath, MeshData& meshData, MaterialImportData& materialData)
		{
			switch(format)
			{
			case ObjectFormat::OBJ:
				return OBJLoader::Load(filepath, meshData, &materialData);
			case ObjectFormat::FBX:
				return FBXLoader::Load(filepath, meshData, &materialData);
			}
			return false;
		}
	}

	AssetManager::AssetManager(Scene& scene)
		: m_scene(&scene)
	{
	}

	Scene& AssetManager::GetScene() const
	{
		return *m_scene;
	}

	TextureID AssetManager::LoadTexture(const std::string& filepath)
	{
		if(filepath.empty()) return TextureID::Invalid;

		const std::string key = NormalizeAssetKey(filepath);
		const auto cached = m_textureCache.find(key);
		if(cached != m_textureCache.end())
		{
			return cached->second;
		}

		const Core::Image image = Core::LoadImageFromFile(key);
		if(!image.IsValid()) return TextureID::Invalid;

		const TextureID texture = m_scene->CreateTexture(image);
		m_textureCache.emplace(key, texture);
		return texture;
	}

	MeshID AssetManager::LoadMesh(
		const std::string& filepath,
		MaterialImportData* outMaterialData,
		std::string* outResolvedPath)
	{
		CachedObjectData* objectData = nullptr;
		if(!FindOrLoadObjectData(filepath, objectData) || objectData == nullptr)
		{
			return MeshID::Invalid;
		}

		if(outMaterialData != nullptr) *outMaterialData = objectData->materialData;
		if(outResolvedPath != nullptr) *outResolvedPath = objectData->resolvedPath;

		if(!IsValid(objectData->mesh))
		{
			if(objectData->meshData.vertices.empty() || objectData->meshData.indices.empty())
			{
				return MeshID::Invalid;
			}
			objectData->mesh = m_scene->CreateMesh(ToRenderMesh(objectData->meshData));
		}

		return objectData->mesh;
	}

	MaterialID AssetManager::CreateMaterial(const MaterialDesc& material)
	{
		return m_scene->CreateMaterial(material);
	}

	MaterialID AssetManager::GetOrCreateMaterial(const MaterialDesc& material)
	{
		const std::string key = BuildMaterialKey(material);
		const auto cached = m_materialCache.find(key);
		if(cached != m_materialCache.end())
		{
			return cached->second;
		}

		const MaterialID materialId = CreateMaterial(material);
		m_materialCache.emplace(key, materialId);
		return materialId;
	}

	ObjectLoadResult AssetManager::LoadObject(
		const std::string& filepath,
		const MaterialDesc& material,
		const Math::float4x4& worldMatrix,
		const RenderFlags& renderFlags)
	{
		return ObjectLoader::Load(*this, filepath, material, worldMatrix, renderFlags);
	}

	ObjectLoadResult AssetManager::LoadOBJ(
		const std::string& filepath,
		const MaterialDesc& material,
		const Math::float4x4& worldMatrix,
		const RenderFlags& renderFlags)
	{
		return ObjectLoader::LoadOBJ(*this, filepath, material, worldMatrix, renderFlags);
	}

	ObjectLoadResult AssetManager::LoadFBX(
		const std::string& filepath,
		const MaterialDesc& material,
		const Math::float4x4& worldMatrix,
		const RenderFlags& renderFlags)
	{
		return ObjectLoader::LoadFBX(*this, filepath, material, worldMatrix, renderFlags);
	}

	bool AssetManager::LoadObjectData(
		const std::string& filepath,
		MeshData& outMeshData,
		MaterialImportData& outMaterialData,
		std::string* outResolvedPath)
	{
		CachedObjectData* objectData = nullptr;
		if(!FindOrLoadObjectData(filepath, objectData) || objectData == nullptr)
		{
			return false;
		}

		outMeshData = objectData->meshData;
		outMaterialData = objectData->materialData;
		if(outResolvedPath != nullptr) *outResolvedPath = objectData->resolvedPath;
		return true;
	}

	bool AssetManager::FindOrLoadObjectData(const std::string& filepath, CachedObjectData*& outData)
	{
		outData = nullptr;
		const std::string key = NormalizeAssetKey(filepath);
		const auto cached = m_objectCache.find(key);
		if(cached != m_objectCache.end())
		{
			outData = &cached->second;
			return true;
		}

		ObjectFormat format = ObjectFormat::OBJ;
		if(!GetObjectFormat(filepath, format)) return false;

		for(uint32_t parentDepth = 0; parentDepth <= 2; ++parentDepth)
		{
			const std::string candidate = BuildRelativeCandidate(filepath, parentDepth);
			MeshData meshData;
			MaterialImportData materialData;
			if(!LoadMeshData(format, candidate, meshData, materialData)) continue;

			CachedObjectData objectData = {};
			objectData.meshData = std::move(meshData);
			objectData.materialData = std::move(materialData);
			objectData.resolvedPath = candidate;

			auto inserted = m_objectCache.emplace(key, std::move(objectData));
			outData = &inserted.first->second;
			return inserted.second;
		}

		return false;
	}

	void AssetManager::Clear()
	{
		m_textureCache.clear();
		m_materialCache.clear();
		m_objectCache.clear();
	}
}
