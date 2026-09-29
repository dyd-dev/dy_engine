#include "ModelLoader.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include "dyf/Platform/Log.h"
#include <utility>

namespace dyf
{
	bool ReportModelError(const std::string& path, const std::string& message)
	{
		Platform::Log::Write(Platform::LogLevel::Error, "Model", path + ": " + message, __FILE__, __LINE__);
		return false;
	}

	void ReportModelWarning(const std::string& path, const std::string& message)
	{
		Platform::Log::Write(Platform::LogLevel::Warning, "Model", path + ": " + message, __FILE__, __LINE__);
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
			ReportModelError(path, "unsupported model file extension");
			outModel = {};
			return false;
		}

		std::error_code fileSizeError;
		const uintmax_t sourceBytes = std::filesystem::file_size(path, fileSizeError);
		if(fileSizeError)
		{
			ReportModelError(path, "failed to query model source file size");
			outModel = {};
			return false;
		}
		if(sourceBytes > options.maxSourceBytes)
		{
			ReportModelError(path, "model source size " + std::to_string(sourceBytes)
                + " exceeds limit " + std::to_string(options.maxSourceBytes));
			outModel = {};
			return false;
		}

		ModelData loadedModel = {};
		const bool loaded = lowerExtension == ".gltf" || lowerExtension == ".glb"
			? LoadGltfModel(path, loadedModel, options)
			: LoadUfbxModel(path, loadedModel, options);
		if(!loaded)
		{
			ReportModelError(path, "model load failed because the asset data is invalid");
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
