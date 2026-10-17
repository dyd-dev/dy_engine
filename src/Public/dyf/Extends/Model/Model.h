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
		std::array<std::string, ToIndex(MaterialTextureKind::Count)> texturePaths = {};
		std::array<bool, ToIndex(MaterialTextureKind::Count)> hasTexture = {};
		std::array<uint32_t, ToIndex(MaterialTextureKind::Count)> textureIndices = [] {
			std::array<uint32_t, ToIndex(MaterialTextureKind::Count)> indices = {};
			indices.fill(UINT32_MAX);
			return indices;
		}();
		// 독립 거칠기 원본의 R 채널을 재질 준비 시 공통 ORM의 G 채널에 결합한다.
		std::string roughnessTexturePath;
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
		// Immutable geometry may reference an earlier, owning ModelData::meshes
		// entry. The reference has empty mesh/skin/morph storage but keeps its own
		// material and node transform. UINT32_MAX owns the geometry in mesh.
		uint32_t geometryIndex = UINT32_MAX;
	};

	struct ModelData
	{
		std::vector<ModelMesh> meshes;
		std::vector<ModelMaterialInfo> materials;
		std::vector<Image> textures;
		std::vector<ModelNode> nodes;
		std::vector<ModelSkin> skins;
		std::vector<AnimationClip> animations;
		// Source lights in asset coordinates, independently instanced with the model.
		std::vector<DirectionalLight> directionalLights;
		std::vector<PointLight> pointLights;
		std::vector<SpotLight> spotLights;
		Math::float4x4 assetTransform = Math::float4x4::Identity();
	};

	// 모델과 함께 명시한 재질 설정이다. 재질 이름은 정확히 일치해야 한다.
	struct ModelMaterialOverride
	{
		std::string name;
		float transmissionFactor = 0.0f;
		float indexOfRefraction = 1.5f;
		float thickness = 0.0f;
		Math::float3 attenuationColor = Math::float3(1.0f, 1.0f, 1.0f);
		float attenuationDistance = 0.0f;
		// Negative values preserve the imported scalar factors.
		float roughnessFactor = -1.0f;
		float metallicFactor = -1.0f;
		// 네 성분 모두 -1이면 보존한다. 덮어쓸 때는 네 성분 모두 0..1이어야 한다.
		Math::float4 baseColor = Math::float4(-1.0f, -1.0f, -1.0f, -1.0f);
		// 빈 문자열은 보존한다. 경로는 모델 파일 기준 상대 경로나 절대 경로다.
		std::array<std::string, ToIndex(MaterialTextureKind::Count)> texturePaths = {};
		std::string roughnessTexturePath;
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
		// Source conventions are explicit: ordinary FBX specular RGB is not an ORM map.
		bool fbxSpecularTextureIsOrm = false;
		bool fbxBaseColorTextureHasOpacity = false;
		// +1 for OpenGL/glTF tangent-space normals; -1 for DirectX sources.
		float normalMapYSign = 1.0f;
		std::vector<ModelMaterialOverride> materialOverrides;
		bool importLights = false;
		// Maximum concurrent deferred external-image decodes in LoadModelAsset; >=1.
		// Workers end when loading completes. This does not change asset identity.
		uint32_t textureDecodeThreadCount = 1;
	};

	// Scene에 모델 파일을 추가할 때 사용하는 설정이다. 모델 객체나 생성된 인스턴스가 아니다.
	struct ModelSceneDesc
	{
		std::string path;
		Math::float3 position = Math::float3(0.0f, 0.0f, 0.0f);
		// normalize=true이면 모델 중심과 기준 크기를 먼저 맞춘 뒤 이 변환, position 순서로 적용한다.
		// normalize=false이면 원본 모델에 이 변환과 position만 적용한다.
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
		std::vector<DirectionalLight> directionalLights;
		std::vector<PointLight> pointLights;
		std::vector<SpotLight> spotLights;
		Math::float4x4 assetTransform = Math::float4x4::Identity();
		Math::float3 boundsCenter = Math::float3(0.0f, 0.0f, 0.0f);
		float boundsLargestAxis = 0.0f;
		bool hasBounds = false;
	};

	[[nodiscard]] bool LoadModel(const std::string& path, ModelData& outModel, const ModelLoadOptions& options = {});
	// FBX lights only, in source world coordinates. Geometry, textures and animations
	// are not decoded. sourceBounds defines the finite range for lights without a cutoff.
	[[nodiscard]] bool LoadModelLights(
		const std::string& path,
		ModelData& outModel,
		const Math::Bounds3& sourceBounds,
		const ModelLoadOptions& options = {});
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
