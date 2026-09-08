#pragma once

#include <cstdint>
#include <string>

#include "Graphics/Mesh.h"

namespace dy::Graphics::ModelLoaderInternal
{
	class ModelLoadBudget
	{
	public:
		ModelLoadBudget(const std::string& path, const ModelLoadOptions& options);
		[[nodiscard]] bool CheckNodes(uint64_t count) const;
		[[nodiscard]] bool CheckJoints(uint64_t count) const;
		[[nodiscard]] bool AddAnimationKeys(uint64_t count);
		[[nodiscard]] bool AddBytes(uint64_t count, uint64_t stride, const char* category);

	private:
		const std::string& m_path;
		const ModelLoadOptions& m_options;
		uint64_t m_decodedBytes = 0u;
		uint64_t m_animationKeys = 0u;
	};

	[[nodiscard]] ModelLoadResult*& ActiveModelLoadResult();
	void ReportModelDiagnostic(
		ModelDiagnosticSeverity severity,
		ModelDiagnosticCode code,
		const std::string& path,
		std::string message,
		std::string element = {});
	[[nodiscard]] bool ReportModelError(
		ModelDiagnosticCode code,
		const std::string& path,
		std::string message,
		std::string element = {});
	void ReportModelWarning(
		ModelDiagnosticCode code,
		const std::string& path,
		std::string message,
		std::string element = {});
	[[nodiscard]] Math::float3 BuildFallbackTangent(const Math::float3& normal);
	void CalculateTangents(MeshData& data, bool generateMissingNormals = false);
	[[nodiscard]] uint32_t EnsureDefaultMaterial(ModelData& model);
	[[nodiscard]] bool LoadGltfModel(
		const std::string& filepath,
		ModelData& outModel,
		const ModelLoadOptions& options);
	[[nodiscard]] bool LoadUfbxModel(
		const std::string& filepath,
		ModelData& outModel,
		const ModelLoadOptions& options);
}
