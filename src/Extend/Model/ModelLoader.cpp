#include "ModelLoader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
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
        uint64_t decodedBytes=0;
        return LoadModel(path,outModel,options,decodedBytes);
    }

    bool LoadModel(const std::string& path, ModelData& outModel, const ModelLoadOptions& options, uint64_t& decodedBytes)
    {
        decodedBytes=0;
		if(options.normalMapYSign != 1.0f && options.normalMapYSign != -1.0f)
		{
			outModel = {};
			return ReportModelError(path, "normalMapYSign must be +1 or -1");
		}
		for(const ModelMaterialOverride& value : options.materialOverrides)
		{
			const bool preserveBaseColor = value.baseColor.x == -1.0f && value.baseColor.y == -1.0f
				&& value.baseColor.z == -1.0f && value.baseColor.w == -1.0f;
			const bool validBaseColor = std::isfinite(value.baseColor.x) && value.baseColor.x >= 0.0f && value.baseColor.x <= 1.0f
				&& std::isfinite(value.baseColor.y) && value.baseColor.y >= 0.0f && value.baseColor.y <= 1.0f
				&& std::isfinite(value.baseColor.z) && value.baseColor.z >= 0.0f && value.baseColor.z <= 1.0f
				&& std::isfinite(value.baseColor.w) && value.baseColor.w >= 0.0f && value.baseColor.w <= 1.0f;
			if(!preserveBaseColor && !validBaseColor)
			{
				outModel = {};
				return ReportModelError(path, "material override baseColor must be all -1 or finite RGBA in 0..1: " + value.name);
			}
			if(!std::isfinite(value.transmissionFactor) || value.transmissionFactor < 0.0f || value.transmissionFactor > 1.0f
				|| !std::isfinite(value.indexOfRefraction) || value.indexOfRefraction <= 0.0f
				|| !std::isfinite(value.thickness) || value.thickness < 0.0f
				|| !std::isfinite(value.attenuationColor.x) || value.attenuationColor.x < 0.0f || value.attenuationColor.x > 1.0f
				|| !std::isfinite(value.attenuationColor.y) || value.attenuationColor.y < 0.0f || value.attenuationColor.y > 1.0f
				|| !std::isfinite(value.attenuationColor.z) || value.attenuationColor.z < 0.0f || value.attenuationColor.z > 1.0f
				|| !std::isfinite(value.attenuationDistance) || value.attenuationDistance < 0.0f
				|| !std::isfinite(value.roughnessFactor) || value.roughnessFactor > 1.0f
				|| !std::isfinite(value.metallicFactor) || value.metallicFactor > 1.0f)
			{
				outModel = {};
				return ReportModelError(path, "material override contains invalid optical parameters: " + value.name);
			}
		}
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
			? LoadGltfModel(path, loadedModel, options, &decodedBytes)
			: LoadUfbxModel(path, loadedModel, options, &decodedBytes);
		if(!loaded)
		{
            decodedBytes=0;
			ReportModelError(path, "model load failed because the asset data is invalid");
			outModel = {};
			return false;
		}
		const auto overridePath = [&path](const std::string& value) {
			const auto source = std::filesystem::path(value);
			return (source.is_absolute() ? source : std::filesystem::path(path).parent_path() / source).lexically_normal().string();
		};
		for(ModelMaterialInfo& material : loadedModel.materials)
		{
			material.material.normalYSign = options.normalMapYSign;
			for(const ModelMaterialOverride& value : options.materialOverrides)
			{
				if(value.name != material.name) continue;
				material.material.transmissionFactor = value.transmissionFactor;
				material.material.indexOfRefraction = value.indexOfRefraction;
				material.material.thickness = value.thickness;
				material.material.attenuationColor = value.attenuationColor;
				material.material.attenuationDistance = value.attenuationDistance;
				if(value.roughnessFactor >= 0.0f) material.material.roughnessFactor = value.roughnessFactor;
				if(value.metallicFactor >= 0.0f) material.material.metallicFactor = value.metallicFactor;
				if(value.baseColor.x >= 0.0f) material.material.baseColor = value.baseColor;
				for(size_t slot = 0; slot < value.texturePaths.size(); ++slot)
				{
					if(value.texturePaths[slot].empty()) continue;
					material.texturePaths[slot] = overridePath(value.texturePaths[slot]);
					material.textureIndices[slot] = UINT32_MAX;
					material.hasTexture[slot] = true;
				}
				if(!value.roughnessTexturePath.empty())
					material.roughnessTexturePath = overridePath(value.roughnessTexturePath);
				if(value.transmissionFactor > 0.0f)
				{
					material.material.alphaMode = MaterialAlphaMode::Blend;
					material.material.doubleSided = true;
				}
			}
		}
		outModel = std::move(loadedModel);
		return true;
	}

	bool LoadModelLights(const std::string& path, ModelData& outModel,
		const Math::Bounds3& sourceBounds, const ModelLoadOptions& options)
	{
		outModel = {};
		std::string extension = std::filesystem::path(path).extension().string();
		std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		if(extension != ".fbx")
			return ReportModelError(path, "light-only model loading currently supports FBX files");
		if(!sourceBounds.valid
			|| !std::isfinite(sourceBounds.min.x) || !std::isfinite(sourceBounds.min.y) || !std::isfinite(sourceBounds.min.z)
			|| !std::isfinite(sourceBounds.max.x) || !std::isfinite(sourceBounds.max.y) || !std::isfinite(sourceBounds.max.z)
			|| sourceBounds.min.x > sourceBounds.max.x || sourceBounds.min.y > sourceBounds.max.y || sourceBounds.min.z > sourceBounds.max.z)
			return ReportModelError(path, "light-only loading requires finite source bounds");
		ModelLoadOptions lightOptions = options;
		lightOptions.importLights = true;
		ModelData loadedModel;
		if(!LoadUfbxModel(path, loadedModel, lightOptions, nullptr, &sourceBounds)) return false;
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
		if(outMesh.vertices.empty() || outMesh.indices.empty())
			return ReportModelError(path, "mesh merge failed or produced no geometry");
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
