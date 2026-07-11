#include "Graphics/ModelLoaderInternal.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <utility>

namespace dy::Graphics::ModelLoaderInternal
{
	ModelLoadResult*& ActiveModelLoadResult()
	{
		thread_local ModelLoadResult* result = nullptr;
		return result;
	}

	void ReportModelDiagnostic(
		ModelDiagnosticSeverity severity,
		ModelDiagnosticCode code,
		const std::string& path,
		std::string message,
		std::string element)
	{
		if(ActiveModelLoadResult() != nullptr)
		{
			ActiveModelLoadResult()->diagnostics.push_back(ModelLoadDiagnostic{
				severity,
				code,
				path,
				message,
				std::move(element) });
		}
		std::cerr << path << ": " << message << std::endl;
	}

	bool ReportModelError(
		ModelDiagnosticCode code,
		const std::string& path,
		std::string message,
		std::string element)
	{
		ReportModelDiagnostic(
			ModelDiagnosticSeverity::Error,
			code,
			path,
			std::move(message),
			std::move(element));
		return false;
	}

	void ReportModelWarning(
		ModelDiagnosticCode code,
		const std::string& path,
		std::string message,
		std::string element)
	{
		ReportModelDiagnostic(
			ModelDiagnosticSeverity::Warning,
			code,
			path,
			std::move(message),
			std::move(element));
	}
}

namespace dy::Graphics
{
	namespace
	{
		class ModelDiagnosticScope
		{
		public:
			explicit ModelDiagnosticScope(ModelLoadResult& result)
				: m_previous(ModelLoaderInternal::ActiveModelLoadResult())
			{
				ModelLoaderInternal::ActiveModelLoadResult() = &result;
			}

			~ModelDiagnosticScope()
			{
				ModelLoaderInternal::ActiveModelLoadResult() = m_previous;
			}

		private:
			ModelLoadResult* m_previous = nullptr;
		};
	}

	using ModelLoaderInternal::ReportModelDiagnostic;

	bool ModelLoader::Load(const std::string& path, ModelData& outModel, const ModelLoadOptions& options)
	{
		return LoadModel(path, outModel, options);
	}

	bool ModelLoader::LoadMesh(const std::string& path, MeshData& outMesh, ModelMaterialInfo* outMaterial, const ModelLoadOptions& options)
	{
		return Graphics::LoadMesh(path, outMesh, outMaterial, options);
	}

	bool ModelLoader::LoadMesh(const std::string& path, MeshData& outMesh, std::string* outBaseColorTexturePath, const ModelLoadOptions& options)
	{
		return Graphics::LoadMesh(path, outMesh, outBaseColorTexturePath, options);
	}

	bool LoadModel(const std::string& path, ModelData& outModel, const ModelLoadOptions& options)
	{
		return LoadModelDetailed(path, outModel, options).success;
	}

	ModelLoadResult LoadModelDetailed(
		const std::string& path,
		ModelData& outModel,
		const ModelLoadOptions& options)
	{
		ModelLoadResult result;
		ModelDiagnosticScope diagnosticScope(result);
		const std::string extension = std::filesystem::path(path).extension().string();
		std::string lowerExtension = extension;
		std::transform(lowerExtension.begin(), lowerExtension.end(), lowerExtension.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});

		ModelData loadedModel = {};
		bool loaded = false;
		const bool supportedExtension = lowerExtension == ".obj"
			|| lowerExtension == ".gltf"
			|| lowerExtension == ".glb"
			|| lowerExtension == ".fbx";
		if(supportedExtension)
		{
			std::error_code fileSizeError;
			const uintmax_t sourceBytes = std::filesystem::file_size(path, fileSizeError);
			if(fileSizeError)
			{
				ReportModelDiagnostic(
					ModelDiagnosticSeverity::Error,
					ModelDiagnosticCode::FileReadFailed,
					path,
					"failed to query model source file size");
			}
			else if(sourceBytes > options.maxSourceBytes)
			{
				std::ostringstream message;
				message << "model source size " << sourceBytes
					<< " exceeds limit " << options.maxSourceBytes;
				ReportModelDiagnostic(
					ModelDiagnosticSeverity::Error,
					ModelDiagnosticCode::ResourceLimitExceeded,
					path,
					message.str());
			}
			else if(lowerExtension == ".obj") loaded = ModelLoaderInternal::LoadUfbxModel(path, loadedModel, options);
			else if(lowerExtension == ".gltf" || lowerExtension == ".glb") loaded = ModelLoaderInternal::LoadGltfModel(path, loadedModel, options);
			else loaded = ModelLoaderInternal::LoadUfbxModel(path, loadedModel, options);
		}
		else
		{
			ReportModelDiagnostic(
				ModelDiagnosticSeverity::Error,
				ModelDiagnosticCode::UnsupportedFormat,
				path,
				"unsupported model file extension");
		}
		if(!loaded)
		{
			outModel = {};
			const bool hasError = std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const ModelLoadDiagnostic& diagnostic) {
				return diagnostic.severity == ModelDiagnosticSeverity::Error;
			});
			if(!hasError)
			{
				ReportModelDiagnostic(
					ModelDiagnosticSeverity::Error,
					ModelDiagnosticCode::InvalidData,
					path,
					"model load failed because the asset data is invalid");
			}
			return result;
		}
		outModel = std::move(loadedModel);
		result.success = true;
		return result;
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
