#include "Graphics/Mesh.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>

#include "Graphics/Scene.h"

namespace dy::Graphics
{
	namespace
	{
		[[nodiscard]] Math::float3 BuildFallbackTangent(const Math::float3& normal)
		{
			const Math::float3 up = std::fabs(normal.z) < 0.999f
				? Math::float3(0.0f, 0.0f, 1.0f)
				: Math::float3(0.0f, 1.0f, 0.0f);
			return NormalizeOr(Cross(up, normal), Math::float3(1.0f, 0.0f, 0.0f));
		}

		struct ModelBounds
		{
			Math::float3 min;
			Math::float3 max;
			bool valid = false;
		};

		void IncludePoint(ModelBounds& bounds, const Math::float3& point)
		{
			if(!bounds.valid)
			{
				bounds.min = point;
				bounds.max = point;
				bounds.valid = true;
				return;
			}
			bounds.min.x = std::min(bounds.min.x, point.x);
			bounds.min.y = std::min(bounds.min.y, point.y);
			bounds.min.z = std::min(bounds.min.z, point.z);
			bounds.max.x = std::max(bounds.max.x, point.x);
			bounds.max.y = std::max(bounds.max.y, point.y);
			bounds.max.z = std::max(bounds.max.z, point.z);
		}

		[[nodiscard]] ModelBounds ComputeModelBounds(const ModelData& model)
		{
			ModelBounds bounds = {};
			std::vector<NodeTransform> bindPose;
			std::vector<Math::float4x4> bindGlobals;
			bindPose.reserve(model.nodes.size());
			for(const ModelNode& node : model.nodes) bindPose.push_back(node.bindTransform);
			const bool hasBindGlobals = BuildGlobalNodeMatrices(model.nodes, bindPose, bindGlobals);
			for(const ModelMesh& mesh : model.meshes)
			{
				Math::float4x4 vertexTransform = model.assetTransform;
				if(hasBindGlobals && mesh.nodeIndex < bindGlobals.size())
					vertexTransform = vertexTransform * bindGlobals[mesh.nodeIndex];
				for(const Vertex& vertex : mesh.mesh.vertices)
				{
					IncludePoint(bounds, Math::TransformPoint(vertexTransform, vertex.position));
				}
			}
			return bounds;
		}

		[[nodiscard]] Math::float4x4 BuildModelSceneTransform(const ModelAsset& asset, const ModelSceneDesc& desc)
		{
			Math::float4x4 transform = Math::Translation(desc.position);
			if(desc.yUpToZUp)
			{
				// Y-up(+Z forward) → Z-up. RotationX(90°)만 하면 forward 가 world -Y 가 되어
				// (+Y 쪽 카메라가 모델 뒤통수를 봄) 180° yaw 를 더해 forward 를 +Y 로 돌린다.
				transform = transform * Math::RotationZ(3.14159265f) * Math::RotationX(1.5707963f);
			}
			if(desc.normalize && asset.hasBounds)
			{
				const float scale = asset.boundsLargestAxis > 0.0001f
					? desc.normalizedSize / asset.boundsLargestAxis
					: 1.0f;
				transform = transform * Math::Scaling(scale) * Math::Translation(Math::float3(
					-asset.boundsCenter.x,
					-asset.boundsCenter.y,
					-asset.boundsCenter.z));
			}
			return transform * desc.transform * asset.assetTransform;
		}

		[[nodiscard]] TextureID CreateTextureIfPresent(
			Scene& scene,
			const ModelMaterialInfo& material,
			MaterialTextureKind kind,
			const std::vector<TextureID>& importedTextures)
		{
			const uint32_t slot = static_cast<uint32_t>(kind);
			const uint32_t textureIndex = material.textureIndices[slot];
			if(textureIndex < importedTextures.size()) return importedTextures[textureIndex];
			return material.hasTexture[slot] && !material.texturePaths[slot].empty()
				? scene.CreateTexture(material.texturePaths[slot])
				: TextureID::Invalid;
		}

		[[nodiscard]] MaterialID CreateSceneMaterial(
			Scene& scene,
			const ModelMaterialInfo& source,
			const std::vector<TextureID>& importedTextures)
		{
			MaterialDesc material = source.material;
			material.baseColorTexture = CreateTextureIfPresent(scene, source, MaterialTextureKind::BaseColor, importedTextures);
			material.metallicRoughnessTexture = CreateTextureIfPresent(scene, source, MaterialTextureKind::MetallicRoughness, importedTextures);
			material.normalTexture = CreateTextureIfPresent(scene, source, MaterialTextureKind::Normal, importedTextures);
			material.occlusionTexture = CreateTextureIfPresent(scene, source, MaterialTextureKind::Occlusion, importedTextures);
			material.emissiveTexture = CreateTextureIfPresent(scene, source, MaterialTextureKind::Emissive, importedTextures);
			return scene.CreateMaterial(material);
		}

		[[nodiscard]] std::string BuildModelAssetCacheKey(
			const std::string& path,
			const ModelLoadOptions& options)
		{
			std::error_code error;
			std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(path, error);
			if(error)
			{
				error.clear();
				canonicalPath = std::filesystem::absolute(path, error).lexically_normal();
				if(error) canonicalPath = std::filesystem::path(path).lexically_normal();
			}
			std::string key = canonicalPath.generic_string();
#if defined(_WIN32)
			std::transform(key.begin(), key.end(), key.begin(), [](unsigned char value) {
				return static_cast<char>(std::tolower(value));
			});
#endif
			key += "|flipV=" + std::to_string(options.flipV);
			key += "|maxSourceBytes=" + std::to_string(options.maxSourceBytes);
			key += "|maxParserInputBytes=" + std::to_string(options.maxParserInputBytes);
			key += "|maxParserBytes=" + std::to_string(options.maxParserBytes);
			key += "|maxDecodedBytes=" + std::to_string(options.maxDecodedBytes);
			key += "|maxTextures=" + std::to_string(options.maxTextures);
			key += "|maxMorphTargetsPerMesh=" + std::to_string(options.maxMorphTargetsPerMesh);
			key += "|maxNodes=" + std::to_string(options.maxNodes);
			key += "|maxNodeDepth=" + std::to_string(options.maxNodeDepth);
			key += "|maxJointsPerSkin=" + std::to_string(options.maxJointsPerSkin);
			key += "|maxAnimationKeys=" + std::to_string(options.maxAnimationKeys);
			key += "|fbxBakeRate=" + std::to_string(options.fbxBakeRate);
			key += "|fbxConstantTrackTolerance=" + std::to_string(options.fbxConstantTrackTolerance);
			return key;
		}
	}

	bool EvaluateMorphTargets(
		const MeshData& baseMesh,
		const std::vector<MorphTarget>& targets,
		const std::vector<float>& weights,
		MeshData& outMesh)
	{
		if(targets.size() != weights.size()) return false;
		const size_t vertexCount = baseMesh.vertices.size();
		auto validDeltaCount = [vertexCount](const std::vector<Math::float3>& deltas) {
			return deltas.empty() || deltas.size() == vertexCount;
		};
		for(size_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex)
		{
			const MorphTarget& target = targets[targetIndex];
			if(!std::isfinite(weights[targetIndex])
				|| !validDeltaCount(target.positionDeltas)
				|| !validDeltaCount(target.normalDeltas)
				|| !validDeltaCount(target.tangentDeltas)) return false;
		}

		outMesh = baseMesh;
		for(size_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex)
		{
			const float weight = weights[targetIndex];
			if(weight == 0.0f) continue;
			const MorphTarget& target = targets[targetIndex];
			for(size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
			{
				Vertex& vertex = outMesh.vertices[vertexIndex];
				if(!target.positionDeltas.empty())
					vertex.position = vertex.position + target.positionDeltas[vertexIndex] * weight;
				if(!target.normalDeltas.empty())
					vertex.normal = vertex.normal + target.normalDeltas[vertexIndex] * weight;
				if(!target.tangentDeltas.empty())
				{
					const Math::float3 tangent(vertex.tangent.x, vertex.tangent.y, vertex.tangent.z);
					const Math::float3 morphedTangent = tangent + target.tangentDeltas[vertexIndex] * weight;
					vertex.tangent.x = morphedTangent.x;
					vertex.tangent.y = morphedTangent.y;
					vertex.tangent.z = morphedTangent.z;
				}
			}
		}

		for(Vertex& vertex : outMesh.vertices)
		{
			auto finite3 = [](const Math::float3& value) {
				return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
			};
			if(!finite3(vertex.position) || !finite3(vertex.normal)
				|| !std::isfinite(vertex.tangent.x) || !std::isfinite(vertex.tangent.y)
				|| !std::isfinite(vertex.tangent.z) || !std::isfinite(vertex.tangent.w)) return false;
			vertex.normal = NormalizeOr(vertex.normal, Math::float3(0.0f, 0.0f, 1.0f));
			const Math::float3 rawTangent(vertex.tangent.x, vertex.tangent.y, vertex.tangent.z);
			const Math::float3 tangent = NormalizeOr(
				rawTangent - vertex.normal * Dot(vertex.normal, rawTangent),
				BuildFallbackTangent(vertex.normal));
			vertex.tangent.x = tangent.x;
			vertex.tangent.y = tangent.y;
			vertex.tangent.z = tangent.z;
		}
		return true;
	}

	bool SkinVertex(
		const Vertex& source,
		const SkinInfluence& influence,
		const std::vector<SkinJointMatrices>& palette,
		Vertex& outVertex)
	{
		if(!std::isfinite(influence.dqBlendWeight)
			|| influence.dqBlendWeight < 0.0f
			|| influence.dqBlendWeight > 1.0f) return false;
		float totalWeight = 0.0f;
		for(size_t component = 0u; component < 4u; ++component)
		{
			const float weight = influence.weights[component];
			if(!std::isfinite(weight) || weight < 0.0f) return false;
			if(weight > 0.0f && influence.jointIndices[component] >= palette.size()) return false;
			totalWeight += weight;
		}
		if(totalWeight <= 1.0e-6f)
		{
			outVertex = source;
			return true;
		}

		Math::float4x4 linearMatrix = {};
		Math::float4x4 fallbackNormalMatrix = {};
		const bool useDualQuaternion = influence.dqBlendWeight > 0.0f;
		Math::quat referenceReal = Math::quat::Identity();
		bool hasReference = false;
		Math::quat realSum(0.0f, 0.0f, 0.0f, 0.0f);
		Math::quat dualSum(0.0f, 0.0f, 0.0f, 0.0f);
		Math::float3 scaleSum(0.0f, 0.0f, 0.0f);
		for(size_t component = 0u; component < 4u; ++component)
		{
			const float rawWeight = influence.weights[component];
			if(rawWeight <= 0.0f) continue;
			const float weight = rawWeight / totalWeight;
			const SkinJointMatrices& joint = palette[influence.jointIndices[component]];
			for(size_t matrixIndex = 0u; matrixIndex < 16u; ++matrixIndex)
			{
				linearMatrix.m[matrixIndex] += joint.positionMatrix.m[matrixIndex] * weight;
				fallbackNormalMatrix.m[matrixIndex] += joint.normalMatrix.m[matrixIndex] * weight;
			}

			if(useDualQuaternion)
			{
				const Math::quat& real = joint.dualQuaternion.real;
				const Math::quat& dual = joint.dualQuaternion.dual;
				if(!hasReference)
				{
					referenceReal = real;
					hasReference = true;
				}
				const float sign = Math::Dot(referenceReal, real) < 0.0f ? -1.0f : 1.0f;
				realSum.x += real.x * weight * sign;
				realSum.y += real.y * weight * sign;
				realSum.z += real.z * weight * sign;
				realSum.w += real.w * weight * sign;
				dualSum.x += dual.x * weight * sign;
				dualSum.y += dual.y * weight * sign;
				dualSum.z += dual.z * weight * sign;
				dualSum.w += dual.w * weight * sign;
				scaleSum = scaleSum + joint.dualQuaternion.scale * weight;
			}
		}

		Math::float4x4 finalMatrix = linearMatrix;
		if(useDualQuaternion)
		{
			const float realLengthSquared = Math::Dot(realSum, realSum);
			if(!hasReference || realLengthSquared <= 1.0e-12f) return false;
			const float inverseRealLength = 1.0f / std::sqrt(realLengthSquared);
			Math::DualQuaternionTRS blendedDualQuaternion;
			blendedDualQuaternion.real = Math::quat(
				realSum.x * inverseRealLength,
				realSum.y * inverseRealLength,
				realSum.z * inverseRealLength,
				realSum.w * inverseRealLength);
			blendedDualQuaternion.dual = Math::quat(
				dualSum.x * inverseRealLength,
				dualSum.y * inverseRealLength,
				dualSum.z * inverseRealLength,
				dualSum.w * inverseRealLength);
			blendedDualQuaternion.scale = scaleSum;

			const Math::float4x4 dqMatrix = Math::DualQuaternionToMatrix(blendedDualQuaternion);
			for(size_t matrixIndex = 0u; matrixIndex < 16u; ++matrixIndex)
			{
				finalMatrix.m[matrixIndex] = linearMatrix.m[matrixIndex]
					+ (dqMatrix.m[matrixIndex] - linearMatrix.m[matrixIndex]) * influence.dqBlendWeight;
			}
		}
		Math::float4x4 finalNormalMatrix = {};
		if(!Math::InverseTranspose(finalMatrix, finalNormalMatrix))
			finalNormalMatrix = fallbackNormalMatrix;

		outVertex = source;
		outVertex.position = Math::TransformPoint(finalMatrix, source.position);
		outVertex.normal = NormalizeOr(
			Math::TransformVector(finalNormalMatrix, source.normal),
			Math::float3(0.0f, 0.0f, 1.0f));
		const Math::float3 sourceTangent(source.tangent.x, source.tangent.y, source.tangent.z);
		const Math::float3 transformedTangent = Math::TransformVector(finalMatrix, sourceTangent);
		const Math::float3 tangent = NormalizeOr(
			transformedTangent - outVertex.normal * Dot(outVertex.normal, transformedTangent),
			BuildFallbackTangent(outVertex.normal));
		const float determinant = Math::Determinant3x3(finalMatrix);
		outVertex.tangent = Math::float4(
			tangent.x,
			tangent.y,
			tangent.z,
			determinant < 0.0f ? -source.tangent.w : source.tangent.w);
		return std::isfinite(outVertex.position.x) && std::isfinite(outVertex.position.y)
			&& std::isfinite(outVertex.position.z) && std::isfinite(outVertex.normal.x)
			&& std::isfinite(outVertex.normal.y) && std::isfinite(outVertex.normal.z);
	}

	bool LoadModelAsset(
		Scene& scene,
		const std::string& path,
		const ModelLoadOptions& options,
		ModelAssetID* outAsset)
	{
		if(outAsset != nullptr) *outAsset = ModelAssetID::Invalid;
		const std::string cacheKey = BuildModelAssetCacheKey(path, options);
		const ModelAssetID cachedAsset = scene.FindModelAsset(cacheKey);
		if(IsValid(cachedAsset))
		{
			if(outAsset != nullptr) *outAsset = cachedAsset;
			return true;
		}

		ModelData model = {};
		if(!LoadModel(path, model, options)) return false;
		const bool animatedModel = !model.skins.empty() || !model.animations.empty()
			|| std::any_of(model.meshes.begin(), model.meshes.end(), [](const ModelMesh& mesh) {
				return !mesh.morphTargets.empty();
			});
		for(const ModelMesh& modelMesh : model.meshes)
		{
			if(modelMesh.mesh.vertices.empty() || modelMesh.mesh.indices.empty()) continue;
			if(animatedModel && modelMesh.nodeIndex >= model.nodes.size()) return false;
			if(animatedModel
				&& modelMesh.skinIndex != kInvalidAnimationIndex
				&& modelMesh.skinIndex >= model.skins.size()) return false;
		}

		const ModelBounds bounds = ComputeModelBounds(model);
		std::vector<TextureID> importedTextures;
		importedTextures.reserve(model.textures.size());
		for(const TextureAsset& texture : model.textures)
			importedTextures.push_back(scene.CreateTexture(texture));
		std::vector<MaterialID> materials;
		materials.reserve(model.materials.size());
		for(const ModelMaterialInfo& material : model.materials)
			materials.push_back(CreateSceneMaterial(scene, material, importedTextures));
		if(materials.empty()) materials.push_back(scene.CreateMaterial(MaterialDesc{}));

		ModelAsset asset;
		asset.cacheKey = cacheKey;
		asset.nodes = std::move(model.nodes);
		asset.skins = std::move(model.skins);
		asset.animations = std::move(model.animations);
		asset.assetTransform = model.assetTransform;
		asset.hasBounds = bounds.valid;
		if(bounds.valid)
		{
			asset.boundsCenter = Math::float3(
				(bounds.min.x + bounds.max.x) * 0.5f,
				(bounds.min.y + bounds.max.y) * 0.5f,
				(bounds.min.z + bounds.max.z) * 0.5f);
			asset.boundsLargestAxis = std::max(
				std::max(bounds.max.x - bounds.min.x, bounds.max.y - bounds.min.y),
				bounds.max.z - bounds.min.z);
		}
		asset.meshes.reserve(model.meshes.size());
		for(ModelMesh& modelMesh : model.meshes)
		{
			if(modelMesh.mesh.vertices.empty() || modelMesh.mesh.indices.empty()) continue;
			const MaterialID material = modelMesh.materialIndex < materials.size()
				? materials[modelMesh.materialIndex]
				: materials.front();
			ModelAssetMesh assetMesh;
			assetMesh.mesh = scene.CreateMesh(modelMesh.mesh);
			assetMesh.material = material;
			assetMesh.nodeIndex = modelMesh.nodeIndex;
			assetMesh.skinIndex = modelMesh.skinIndex;
			assetMesh.morphTargets = std::move(modelMesh.morphTargets);
			assetMesh.defaultMorphWeights = std::move(modelMesh.defaultMorphWeights);
			asset.meshes.push_back(std::move(assetMesh));
		}
		if(asset.meshes.empty()) return false;

		const ModelAssetID assetId = scene.CreateModelAsset(std::move(asset));
		if(outAsset != nullptr) *outAsset = assetId;
		return true;
	}

	bool InstantiateModel(
		Scene& scene,
		ModelAssetID assetId,
		const ModelSceneDesc& desc,
		ModelInstanceID* outInstance)
	{
		if(outInstance != nullptr) *outInstance = ModelInstanceID::Invalid;
		const ModelAsset* asset = scene.TryGetModelAsset(assetId);
		if(asset == nullptr) return false;
		const bool animatedModel = !asset->skins.empty() || !asset->animations.empty()
			|| std::any_of(asset->meshes.begin(), asset->meshes.end(), [](const ModelAssetMesh& mesh) {
				return !mesh.morphTargets.empty();
			});
		const Math::float4x4 transform = BuildModelSceneTransform(*asset, desc);

		ModelInstanceID instanceId = ModelInstanceID::Invalid;
		if(animatedModel)
		{
			ModelInstance instance;
			instance.assetId = assetId;
			instance.rootTransform = transform;
			instanceId = scene.CreateModelInstance(std::move(instance));
			if(!IsValid(instanceId)) return false;
			if(!asset->animations.empty() && !scene.PlayAnimation(instanceId, 0u, true)) return false;
		}

		bool createdAny = false;
		for(size_t assetMeshIndex = 0; assetMeshIndex < asset->meshes.size(); ++assetMeshIndex)
		{
			const ModelAssetMesh& assetMesh = asset->meshes[assetMeshIndex];
			if(!IsValid(assetMesh.mesh) || !IsValid(assetMesh.material)) continue;
			Math::float4x4 entityTransform = transform;
			if(animatedModel)
			{
				const ModelInstance& instance = scene.GetModelInstance(instanceId);
				if(assetMesh.nodeIndex >= instance.globalPose.size()) return false;
				entityTransform = transform * instance.globalPose[assetMesh.nodeIndex];
			}
			const EntityID entity = scene.CreateEntity(
				assetMesh.mesh,
				assetMesh.material,
				entityTransform,
				desc.renderFlags);
			if(animatedModel && !scene.BindEntityToModel(
				instanceId,
				entity,
				assetMesh.nodeIndex,
				assetMesh.skinIndex,
				static_cast<uint32_t>(assetMeshIndex))) return false;
			createdAny = true;
		}
		if(animatedModel)
		{
			scene.UpdateAnimations(0.0f);
			if(outInstance != nullptr) *outInstance = instanceId;
		}
		return createdAny;
	}

	bool AddModelToScene(Scene& scene, const ModelSceneDesc& desc, ModelInstanceID* outInstance)
	{
		ModelAssetID asset = ModelAssetID::Invalid;
		return LoadModelAsset(scene, desc.path, desc.loadOptions, &asset)
			&& InstantiateModel(scene, asset, desc, outInstance);
	}

	bool AddModelToScene(Scene& scene, const ModelSceneDesc& desc)
	{
		return AddModelToScene(scene, desc, nullptr);
	}

	bool AddModelToScene(Scene& scene, const std::string& path, const Math::float3& position)
	{
		ModelSceneDesc desc = {};
		desc.path = path;
		desc.position = position;
		return AddModelToScene(scene, desc);
	}

	MeshData MergeModelMeshes(const ModelData& model)
	{
		MeshData merged = {};
		std::vector<NodeTransform> bindPose;
		bindPose.reserve(model.nodes.size());
		for(const ModelNode& node : model.nodes) bindPose.push_back(node.bindTransform);
		std::vector<Math::float4x4> bindGlobals;
		if(!model.nodes.empty()
			&& !BuildGlobalNodeMatrices(model.nodes, bindPose, bindGlobals)) return {};

		for(const ModelMesh& sourceMesh : model.meshes)
		{
			MeshData defaultMorphedMesh;
			const MeshData* sourceGeometry = &sourceMesh.mesh;
			if(!sourceMesh.morphTargets.empty())
			{
				if(!EvaluateMorphTargets(
					sourceMesh.mesh,
					sourceMesh.morphTargets,
					sourceMesh.defaultMorphWeights,
					defaultMorphedMesh)) return {};
				sourceGeometry = &defaultMorphedMesh;
			}
			Math::float4x4 meshNodeGlobal = Math::float4x4::Identity();
			if(sourceMesh.nodeIndex != kInvalidAnimationIndex)
			{
				if(sourceMesh.nodeIndex >= bindGlobals.size()) return {};
				meshNodeGlobal = bindGlobals[sourceMesh.nodeIndex];
			}
			const Math::float4x4 outerTransform = model.assetTransform * meshNodeGlobal;
			Math::float4x4 outerNormalTransform = {};
			if(!Math::InverseTranspose(outerTransform, outerNormalTransform)) return {};

			std::vector<SkinJointMatrices> bindPalette;
			const bool hasSkin = sourceMesh.skinIndex != kInvalidAnimationIndex;
			if(hasSkin)
			{
				if(sourceMesh.skinIndex >= model.skins.size()
					|| sourceGeometry->skinInfluences.size() != sourceGeometry->vertices.size()
					|| !BuildSkinPalette(
						meshNodeGlobal,
						bindGlobals,
						model.skins[sourceMesh.skinIndex],
						bindPalette)) return {};
			}

			if(merged.vertices.size() > std::numeric_limits<uint32_t>::max()
				- sourceGeometry->vertices.size()) return {};
			const uint32_t vertexOffset = static_cast<uint32_t>(merged.vertices.size());
			for(size_t vertexIndex = 0; vertexIndex < sourceGeometry->vertices.size(); ++vertexIndex)
			{
				Vertex vertex = sourceGeometry->vertices[vertexIndex];
				Math::float3 localPosition = vertex.position;
				Math::float3 localNormal = vertex.normal;
				Math::float3 localTangent(vertex.tangent.x, vertex.tangent.y, vertex.tangent.z);
				if(hasSkin)
				{
					const SkinInfluence& influence = sourceGeometry->skinInfluences[vertexIndex];
					Vertex skinnedVertex;
					if(!SkinVertex(vertex, influence, bindPalette, skinnedVertex)) return {};
					localPosition = skinnedVertex.position;
					localNormal = skinnedVertex.normal;
					localTangent = Math::float3(
						skinnedVertex.tangent.x,
						skinnedVertex.tangent.y,
						skinnedVertex.tangent.z);
					vertex.tangent.w = skinnedVertex.tangent.w;
				}

				vertex.position = Math::TransformPoint(outerTransform, localPosition);
				vertex.normal = NormalizeOr(
					Math::TransformVector(outerNormalTransform, localNormal),
					Math::float3(0.0f, 0.0f, 1.0f));
				const Math::float3 rawTransformedTangent = Math::TransformVector(outerTransform, localTangent);
				const Math::float3 transformedTangent = NormalizeOr(
					rawTransformedTangent - vertex.normal * Dot(vertex.normal, rawTransformedTangent),
					BuildFallbackTangent(vertex.normal));
				if(Math::Determinant3x3(outerTransform) < 0.0f) vertex.tangent.w = -vertex.tangent.w;
				vertex.tangent = Math::float4(
					transformedTangent.x,
					transformedTangent.y,
					transformedTangent.z,
					vertex.tangent.w);
				merged.vertices.push_back(vertex);
			}
			for(const uint32_t index : sourceGeometry->indices)
			{
				if(index >= sourceGeometry->vertices.size()) return {};
				merged.indices.push_back(vertexOffset + index);
			}
		}
		return merged;
	}

	MeshData CreateCubeMesh(float size)
	{
		const float h = std::max(size, 0.0f) * 0.5f;
		MeshData mesh = {};
		mesh.vertices.reserve(24u);
		mesh.indices.reserve(36u);

		auto addFace = [&](const Math::float3& a, const Math::float3& b, const Math::float3& c, const Math::float3& d, const Math::float3& normal, const Math::float4& tangent)
		{
			const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
			Vertex v0 = {};
			Vertex v1 = {};
			Vertex v2 = {};
			Vertex v3 = {};
			v0.position = a;
			v1.position = b;
			v2.position = c;
			v3.position = d;
			v0.normal = normal;
			v1.normal = normal;
			v2.normal = normal;
			v3.normal = normal;
			v0.tangent = tangent;
			v1.tangent = tangent;
			v2.tangent = tangent;
			v3.tangent = tangent;
			v0.uv = Math::float2(0.0f, 0.0f);
			v1.uv = Math::float2(1.0f, 0.0f);
			v2.uv = Math::float2(1.0f, 1.0f);
			v3.uv = Math::float2(0.0f, 1.0f);
			mesh.vertices.push_back(v0);
			mesh.vertices.push_back(v1);
			mesh.vertices.push_back(v2);
			mesh.vertices.push_back(v3);
			mesh.indices.push_back(base + 0u);
			mesh.indices.push_back(base + 1u);
			mesh.indices.push_back(base + 2u);
			mesh.indices.push_back(base + 0u);
			mesh.indices.push_back(base + 2u);
			mesh.indices.push_back(base + 3u);
		};

		addFace(Math::float3(-h, -h, h), Math::float3(h, -h, h), Math::float3(h, h, h), Math::float3(-h, h, h), Math::float3(0.0f, 0.0f, 1.0f), Math::float4(1.0f, 0.0f, 0.0f, 1.0f));
		addFace(Math::float3(h, -h, -h), Math::float3(-h, -h, -h), Math::float3(-h, h, -h), Math::float3(h, h, -h), Math::float3(0.0f, 0.0f, -1.0f), Math::float4(-1.0f, 0.0f, 0.0f, 1.0f));
		addFace(Math::float3(h, -h, h), Math::float3(h, -h, -h), Math::float3(h, h, -h), Math::float3(h, h, h), Math::float3(1.0f, 0.0f, 0.0f), Math::float4(0.0f, 0.0f, -1.0f, 1.0f));
		addFace(Math::float3(-h, -h, -h), Math::float3(-h, -h, h), Math::float3(-h, h, h), Math::float3(-h, h, -h), Math::float3(-1.0f, 0.0f, 0.0f), Math::float4(0.0f, 0.0f, 1.0f, 1.0f));
		addFace(Math::float3(-h, h, h), Math::float3(h, h, h), Math::float3(h, h, -h), Math::float3(-h, h, -h), Math::float3(0.0f, 1.0f, 0.0f), Math::float4(1.0f, 0.0f, 0.0f, 1.0f));
		addFace(Math::float3(-h, -h, -h), Math::float3(h, -h, -h), Math::float3(h, -h, h), Math::float3(-h, -h, h), Math::float3(0.0f, -1.0f, 0.0f), Math::float4(1.0f, 0.0f, 0.0f, 1.0f));
		return mesh;
	}

	const char* ToString(MaterialTextureKind kind)
	{
		switch(kind)
		{
		case MaterialTextureKind::BaseColor: return "BaseColor";
		case MaterialTextureKind::MetallicRoughness: return "MetallicRoughness";
		case MaterialTextureKind::Normal: return "Normal";
		case MaterialTextureKind::Occlusion: return "Occlusion";
		case MaterialTextureKind::Emissive: return "Emissive";
		default: return "Unknown";
		}
	}

}
