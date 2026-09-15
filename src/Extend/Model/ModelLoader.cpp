#include "ModelLoader.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <utility>

namespace dyf
{
	bool ReportModelError(const std::string& path, const std::string& message)
	{
		std::cerr << path << ": " << message << std::endl;
		return false;
	}

	void ReportModelWarning(const std::string& path, const std::string& message)
	{
		std::cerr << path << ": " << message << std::endl;
	}
}

namespace dyf
{
	bool LoadModel(const std::string& path, ModelData& outModel, const ModelLoadOptions& options)
	{
		const std::string extension = std::filesystem::path(path).extension().string();
		std::string lowerExtension = extension;
		std::transform(lowerExtension.begin(), lowerExtension.end(), lowerExtension.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});

		const bool supportedExtension = lowerExtension == ".obj"
			|| lowerExtension == ".gltf"
			|| lowerExtension == ".glb"
			|| lowerExtension == ".fbx";
		if(!supportedExtension)
		{
			std::cerr << path << ": unsupported model file extension" << std::endl;
			outModel = {};
			return false;
		}

		std::error_code fileSizeError;
		const uintmax_t sourceBytes = std::filesystem::file_size(path, fileSizeError);
		if(fileSizeError)
		{
			std::cerr << path << ": failed to query model source file size" << std::endl;
			outModel = {};
			return false;
		}
		if(sourceBytes > options.maxSourceBytes)
		{
			std::cerr << path << ": model source size " << sourceBytes
				<< " exceeds limit " << options.maxSourceBytes << std::endl;
			outModel = {};
			return false;
		}

		ModelData loadedModel = {};
		const bool loaded = lowerExtension == ".gltf" || lowerExtension == ".glb"
			? LoadGltfModel(path, loadedModel, options)
			: LoadUfbxModel(path, loadedModel, options);
		if(!loaded)
		{
			std::cerr << path << ": model load failed because the asset data is invalid" << std::endl;
			outModel = {};
			return false;
		}
		outModel = std::move(loadedModel);
		return true;
	}

	bool LoadMesh(const std::string& path, MeshData& outMesh, ModelMaterialInfo* outMaterial, const ModelLoadOptions& options)
	{
		outMesh = {};
		if(outMaterial != nullptr) *outMaterial = {};
		ModelData model = {};
		if(!LoadModel(path, model, options)) return false;
		outMesh = MergeModelMeshes(model);
		if(outMaterial != nullptr)
		{
			*outMaterial = !model.materials.empty() ? model.materials.front() : ModelMaterialInfo{};
		}
		return true;
	}

	bool LoadMesh(const std::string& path, MeshData& outMesh, std::string* outBaseColorTexturePath, const ModelLoadOptions& options)
	{
		if(outBaseColorTexturePath != nullptr) outBaseColorTexturePath->clear();
		ModelMaterialInfo material = {};
		if(!LoadMesh(path, outMesh, &material, options)) return false;
		if(outBaseColorTexturePath != nullptr)
		{
			const uint32_t slot = static_cast<uint32_t>(MaterialTextureKind::BaseColor);
			*outBaseColorTexturePath = material.hasTexture[slot] ? material.texturePaths[slot] : std::string{};
		}
		return true;
	}
}
