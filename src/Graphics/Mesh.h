#pragma once
#include "Core/Types.h"
#include "Graphics/Animation.h"
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

	struct alignas(16) SkinInfluence
	{
		std::array<uint32_t, 4> jointIndices = { 0u, 0u, 0u, 0u };
		Math::float4 weights = Math::float4(0.0f, 0.0f, 0.0f, 0.0f);
		float dqBlendWeight = 0.0f;
		std::array<float, 3> _padding = { 0.0f, 0.0f, 0.0f };
	};
	static_assert(sizeof(SkinInfluence) == 48u, "GLSL std430 SkinInfluence must be 48 bytes");

	struct MeshData
	{
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
		std::vector<SkinInfluence> skinInfluences;
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

	struct TextureAsset
	{
		std::string sourcePath;
		uint32_t width = 0;
		uint32_t height = 0;
		std::vector<uint8_t> rgba8 = {};
	};

	struct ModelMaterialInfo
	{
		MaterialDesc material;
		std::string name;
		std::array<std::string, kMaterialTextureCount> texturePaths = {};
		bool hasBaseColor = false;
		std::array<bool, kMaterialTextureCount> hasTexture = {};
		std::array<uint32_t, kMaterialTextureCount> textureIndices = [] {
			std::array<uint32_t, kMaterialTextureCount> indices = {};
			indices.fill(kInvalidAnimationIndex);
			return indices;
		}();
	};

	struct MorphTarget
	{
		std::string name;
		std::vector<Math::float3> positionDeltas;
		std::vector<Math::float3> normalDeltas;
		std::vector<Math::float3> tangentDeltas;
	};

	struct ModelMesh
	{
		MeshData mesh;
		std::vector<MorphTarget> morphTargets;
		std::vector<float> defaultMorphWeights;
		uint32_t materialIndex = 0;
		uint32_t nodeIndex = kInvalidAnimationIndex;
		uint32_t skinIndex = kInvalidAnimationIndex;
		std::string name;
	};

	struct ModelData
	{
		std::vector<ModelMesh> meshes;
		std::vector<ModelMaterialInfo> materials;
		std::vector<TextureAsset> textures;
		std::vector<ModelNode> nodes;
		std::vector<ModelSkin> skins;
		std::vector<AnimationClip> animations;
		Math::float4x4 assetTransform = Math::float4x4::Identity();
	};

	struct ModelLoadOptions
	{
		bool flipV = false;
		uint64_t maxSourceBytes = 512ull * 1024ull * 1024ull;
		// fastgltf does not expose a bounded heap allocator, so this limits parser-owned input payloads.
		uint64_t maxParserInputBytes = 512ull * 1024ull * 1024ull;
		// Backends with allocator hooks (currently ufbx) use this as an actual parser heap limit.
		uint64_t maxParserBytes = 512ull * 1024ull * 1024ull;
		uint64_t maxDecodedBytes = 512ull * 1024ull * 1024ull;
		uint32_t maxTextures = 4096u;
		uint32_t maxMorphTargetsPerMesh = 256u;
		uint32_t maxNodes = 65536u;
		uint32_t maxNodeDepth = 256u;
		uint32_t maxJointsPerSkin = 4096u;
		uint64_t maxAnimationKeys = 10000000ull;
		float fbxBakeRate = 30.0f;
		float fbxConstantTrackTolerance = 1.0e-5f;
	};

	enum class ModelDiagnosticSeverity : uint8_t
	{
		Warning,
		Error
	};

	enum class ModelDiagnosticCode : uint8_t
	{
		UnsupportedFormat,
		FileReadFailed,
		ParseFailed,
		ValidationFailed,
		ResourceLimitExceeded,
		InvalidData,
		UnsupportedFeature,
		DataLoss
	};

	struct ModelLoadDiagnostic
	{
		ModelDiagnosticSeverity severity = ModelDiagnosticSeverity::Error;
		ModelDiagnosticCode code = ModelDiagnosticCode::InvalidData;
		std::string path;
		std::string message;
		std::string element;
	};

	struct ModelLoadResult
	{
		bool success = false;
		std::vector<ModelLoadDiagnostic> diagnostics;
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

	struct ModelAssetMesh
	{
		MeshID mesh = MeshID::Invalid;
		MaterialID material = MaterialID::Invalid;
		uint32_t nodeIndex = kInvalidAnimationIndex;
		uint32_t skinIndex = kInvalidAnimationIndex;
		std::vector<MorphTarget> morphTargets;
		std::vector<float> defaultMorphWeights;
	};

	struct ModelAsset
	{
		std::string cacheKey;
		std::vector<ModelAssetMesh> meshes;
		std::vector<ModelNode> nodes;
		std::vector<ModelSkin> skins;
		std::vector<AnimationClip> animations;
		Math::float4x4 assetTransform = Math::float4x4::Identity();
		Math::float3 boundsCenter = Math::float3(0.0f, 0.0f, 0.0f);
		float boundsLargestAxis = 0.0f;
		bool hasBounds = false;
	};

	// 그림자/광원 시점 수학(ShadowDesc, ShadowMapDesc, Compute*LightViewProj, Fit*, BuildShadowMesh)은
	// Graphics/ShadowMath.h 로 분리됨(메시와 별개 관심사).

	class ModelLoader
	{
	public:
		static bool Load(const std::string& path, ModelData& outModel, const ModelLoadOptions& options = {});
		static bool LoadMesh(const std::string& path, MeshData& outMesh, ModelMaterialInfo* outMaterial = nullptr, const ModelLoadOptions& options = {});
		static bool LoadMesh(const std::string& path, MeshData& outMesh, std::string* outBaseColorTexturePath, const ModelLoadOptions& options = {});
	};

	[[nodiscard]] bool LoadModel(const std::string& path, ModelData& outModel, const ModelLoadOptions& options = {});
	[[nodiscard]] ModelLoadResult LoadModelDetailed(
		const std::string& path,
		ModelData& outModel,
		const ModelLoadOptions& options = {});
	[[nodiscard]] bool LoadMesh(const std::string& path, MeshData& outMesh, ModelMaterialInfo* outMaterial = nullptr, const ModelLoadOptions& options = {});
	[[nodiscard]] bool LoadMesh(const std::string& path, MeshData& outMesh, std::string* outBaseColorTexturePath, const ModelLoadOptions& options = {});
	[[nodiscard]] bool LoadModelAsset(
		Scene& scene,
		const std::string& path,
		const ModelLoadOptions& options,
		ModelAssetID* outAsset);
	[[nodiscard]] bool InstantiateModel(
		Scene& scene,
		ModelAssetID asset,
		const ModelSceneDesc& desc,
		ModelInstanceID* outInstance);
	[[nodiscard]] bool AddModelToScene(Scene& scene, const ModelSceneDesc& desc, ModelInstanceID* outInstance);
	[[nodiscard]] bool AddModelToScene(Scene& scene, const ModelSceneDesc& desc);
	[[nodiscard]] bool AddModelToScene(Scene& scene, const std::string& path, const Math::float3& position = Math::float3(0.0f, 0.0f, 0.0f));
	[[nodiscard]] MeshData MergeModelMeshes(const ModelData& model);
	[[nodiscard]] bool EvaluateMorphTargets(
		const MeshData& baseMesh,
		const std::vector<MorphTarget>& targets,
		const std::vector<float>& weights,
		MeshData& outMesh);
	[[nodiscard]] bool SkinVertex(
		const Vertex& source,
		const SkinInfluence& influence,
		const std::vector<SkinJointMatrices>& palette,
		Vertex& outVertex);
	[[nodiscard]] MeshData CreateCubeMesh(float size = 1.0f);
	[[nodiscard]] const char* ToString(MaterialTextureKind kind);
}
