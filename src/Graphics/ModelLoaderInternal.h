#pragma once

#include <string>

#include "Graphics/Mesh.h"

namespace dy::Graphics::ModelLoaderInternal
{
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
	[[nodiscard]] bool LoadGltfModel(
		const std::string& filepath,
		ModelData& outModel,
		const ModelLoadOptions& options);
	[[nodiscard]] bool LoadUfbxModel(
		const std::string& filepath,
		ModelData& outModel,
		const ModelLoadOptions& options);
}
