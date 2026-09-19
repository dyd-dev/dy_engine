#pragma once

#include <cstdint>
#include <string>

#include <dyf/Extends/Model/Model.h>

namespace dyf
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

	[[nodiscard]] bool ReportModelError(
		const std::string& path,
		const std::string& message);
	void ReportModelWarning(
		const std::string& path,
		const std::string& message);
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
