#pragma once
#include "Core/Types.h"
#include "Math/Math.h"
#include <cstdint>
#include <array>
#include <vector>
#include <string>

namespace dy::Graphics
{
	class Scene;

	enum class MaterialTextureKind : uint32_t
	{
		BaseColor = 0,
		MetallicRoughness,
		Normal,
		Occlusion,
		Emissive,
		Count
	};

	inline constexpr uint32_t kMaterialTextureCount = static_cast<uint32_t>(MaterialTextureKind::Count);

	struct Vertex
	{
		dy::Math::float3 position;
		dy::Math::float3 normal;
		dy::Math::float2 uv;
		dy::Math::float4 color = dy::Math::float4(1.0f, 1.0f, 1.0f, 1.0f);
		dy::Math::float4 tangent = dy::Math::float4(1.0f, 0.0f, 0.0f, 1.0f);
	};

	struct MeshData
	{
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
	};

	struct RenderFlags
	{
		bool castShadow = true;
		bool receiveShadow = true;
	};

	struct MaterialDesc
	{
		Math::float4 baseColor = Math::float4(1.0f, 1.0f, 1.0f, 1.0f);
		TextureID baseColorTexture = TextureID::Invalid;
		Math::float3 emissiveColor = Math::float3(0.0f, 0.0f, 0.0f);
		float metallicFactor = 0.0f;
		float roughnessFactor = 0.5f;
		float normalScale = 1.0f;
		float occlusionStrength = 1.0f;
		TextureID metallicRoughnessTexture = TextureID::Invalid;
		TextureID normalTexture = TextureID::Invalid;
		TextureID occlusionTexture = TextureID::Invalid;
		TextureID emissiveTexture = TextureID::Invalid;
	};

	struct ModelMaterialInfo
	{
		MaterialDesc material;
		std::string name;
		std::array<std::string, kMaterialTextureCount> texturePaths = {};
		bool hasBaseColor = false;
		std::array<bool, kMaterialTextureCount> hasTexture = {};
	};

	struct ModelMesh
	{
		MeshData mesh;
		uint32_t materialIndex = 0;
		std::string name;
	};

	struct ModelData
	{
		std::vector<ModelMesh> meshes;
		std::vector<ModelMaterialInfo> materials;
	};

	struct ModelLoadOptions
	{
		bool flipV = false;
	};

	struct ModelSceneDesc
	{
		std::string path;
		Math::float3 position = Math::float3(0.0f, 0.0f, 0.0f);
		Math::float4x4 transform = Math::float4x4::Identity();
		float normalizedSize = 1.6f;
		bool normalize = true;
		bool yUpToZUp = true;
		RenderFlags renderFlags = {};
		ModelLoadOptions loadOptions = {};
	};

	// 그림자/광원 시점 수학(ShadowDesc, ShadowMapDesc, Compute*LightViewProj, Fit*, BuildShadowMesh)은
	// Graphics/ShadowMath.h 로 분리됨(메시와 별개 관심사).

	enum class TextureColorSpace : uint8_t
	{
		Linear,
		SRGB
	};

	struct TextureAsset
	{
		std::string sourcePath;
		uint32_t width = 0;
		uint32_t height = 0;
		std::vector<uint8_t> rgba8 = {};
		TextureColorSpace colorSpace = TextureColorSpace::SRGB;
	};

	class ModelLoader
	{
	public:
		static bool Load(const std::string& path, ModelData& outModel, const ModelLoadOptions& options = {});
		static bool LoadMesh(const std::string& path, MeshData& outMesh, ModelMaterialInfo* outMaterial = nullptr, const ModelLoadOptions& options = {});
		static bool LoadMesh(const std::string& path, MeshData& outMesh, std::string* outBaseColorTexturePath, const ModelLoadOptions& options = {});
	};

	[[nodiscard]] bool LoadModel(const std::string& path, ModelData& outModel, const ModelLoadOptions& options = {});
	[[nodiscard]] bool LoadMesh(const std::string& path, MeshData& outMesh, ModelMaterialInfo* outMaterial = nullptr, const ModelLoadOptions& options = {});
	[[nodiscard]] bool LoadMesh(const std::string& path, MeshData& outMesh, std::string* outBaseColorTexturePath, const ModelLoadOptions& options = {});
	[[nodiscard]] bool AddModelToScene(Scene& scene, const ModelSceneDesc& desc);
	[[nodiscard]] bool AddModelToScene(Scene& scene, const std::string& path, const Math::float3& position = Math::float3(0.0f, 0.0f, 0.0f));
	[[nodiscard]] MeshData MergeModelMeshes(const ModelData& model);
	[[nodiscard]] MeshData CreateCubeMesh(float size = 1.0f);
	[[nodiscard]] const char* ToString(MaterialTextureKind kind);
}
