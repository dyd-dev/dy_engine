#pragma once
#include <dyf/Extends/Model/Animation.h>
#include "dyf/Material.h"
#include "dyf/Mesh.h"
#include "dyf/Image.h"
#include "dyf/Light.h"
#include <string>
#include <array>
#include <vector>

namespace dyf
{
	class ModelScene;

	struct ModelMaterialInfo
	{
		MaterialDesc material;
		std::string name;
		std::array<std::string, kMaterialTextureCount> texturePaths = {};
		std::array<bool, kMaterialTextureCount> hasTexture = {};
		std::array<uint32_t, kMaterialTextureCount> textureIndices = [] {
			std::array<uint32_t, kMaterialTextureCount> indices = {};
			indices.fill(UINT32_MAX);
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
		std::vector<SkinInfluence> skinInfluences;
		std::vector<MorphTarget> morphTargets;
		std::vector<float> defaultMorphWeights;
		uint32_t materialIndex = 0;
		uint32_t nodeIndex = UINT32_MAX;
		uint32_t skinIndex = UINT32_MAX;
		std::string name;
	};

	struct ModelData
	{
		std::vector<ModelMesh> meshes;
		std::vector<ModelMaterialInfo> materials;
		std::vector<Image> textures;
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

	// Scene에 모델 파일을 추가할 때 사용하는 설정이다. 모델 객체나 생성된 인스턴스가 아니다.
	struct ModelSceneDesc
	{
		std::string path;
		Math::float3 position = Math::float3(0.0f, 0.0f, 0.0f);
		// 크기와 방향 보정은 일반 변환으로 지정한다. 생략하면 추가 회전을 적용하지 않는다.
		Math::float4x4 transform = Math::float4x4::Identity();
		float normalizedSize = 1.6f;
		bool normalize = true;
		EntityLightingDesc lighting = {};
		ModelLoadOptions loadOptions = {};
	};

	struct ModelAssetMesh
	{
		MeshID mesh = MeshID::Invalid;
		MaterialID material = MaterialID::Invalid;
		uint32_t nodeIndex = UINT32_MAX;
		uint32_t skinIndex = UINT32_MAX;
		std::vector<MorphTarget> morphTargets;
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

	[[nodiscard]] bool LoadModel(const std::string& path, ModelData& outModel, const ModelLoadOptions& options = {});
	[[nodiscard]] bool LoadMesh(const std::string& path, MeshData& outMesh, ModelMaterialInfo* outMaterial = nullptr, const ModelLoadOptions& options = {});
	[[nodiscard]] bool LoadMesh(const std::string& path, MeshData& outMesh, std::string* outBaseColorTexturePath, const ModelLoadOptions& options = {});
	[[nodiscard]] bool LoadModelAsset(
		ModelScene& scene,
		const std::string& path,
		const ModelLoadOptions& options,
		ModelAssetID* outAsset);
	// 생성 시에는 기본 자세를 유지한다. 애니메이션은 ModelScene::PlayAnimation으로 시작한다.
	[[nodiscard]] bool InstantiateModel(
		ModelScene& scene,
		ModelAssetID asset,
		const ModelSceneDesc& desc,
		ModelInstanceID* outInstance);
	[[nodiscard]] bool AddModelToScene(ModelScene& scene, const ModelSceneDesc& desc, ModelInstanceID* outInstance);
	[[nodiscard]] bool AddModelToScene(ModelScene& scene, const ModelSceneDesc& desc);
	[[nodiscard]] bool AddModelToScene(ModelScene& scene, const std::string& path, const Math::float3& position = Math::float3(0.0f, 0.0f, 0.0f));
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
	[[nodiscard]] const char* ToString(MaterialTextureKind kind);
}
