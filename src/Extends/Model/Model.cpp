#include <dyf/Extends/Model/Model.h>
#include "ModelLoader.h"
#include "ModelSkinning.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <map>

#include <dyf/Extends/Model/ModelScene.h>

namespace dyf
{
	namespace
	{
		struct ModelBounds
		{
			Math::float3 min;
			Math::float3 max;
			bool valid = false;
		};

		[[nodiscard]] const ModelMesh* ResolveGeometry(const ModelData& model, const ModelMesh& mesh)
		{
			if(mesh.geometryIndex == UINT32_MAX) return &mesh;
			if(mesh.geometryIndex >= model.meshes.size()) return nullptr;
			const auto& source = model.meshes[mesh.geometryIndex];
			return source.geometryIndex == UINT32_MAX ? &source : nullptr;
		}

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
				const auto* geometry = ResolveGeometry(model, mesh);
				if(!geometry) return {};
				Math::float4x4 vertexTransform = model.assetTransform;
				if(hasBindGlobals && mesh.nodeIndex < bindGlobals.size())
					vertexTransform = vertexTransform * bindGlobals[mesh.nodeIndex];
				for(const Vertex& vertex : geometry->mesh.vertices)
				{
					IncludePoint(bounds, Math::TransformPoint(vertexTransform, vertex.position));
				}
			}
			return bounds;
		}

		[[nodiscard]] bool BuildModelSceneTransform(const ModelAsset& asset, const ModelSceneDesc& desc,
			Math::float4x4& outTransform)
		{
			Math::float4x4 transform = Math::Translation(desc.position) * desc.transform;
			if(desc.normalize && asset.hasBounds)
			{
				if(!std::isfinite(asset.boundsLargestAxis) || asset.boundsLargestAxis < 0.0f
					|| !std::isfinite(desc.normalizedSize)) return false;
				// A valid extent has no source-unit cutoff. Normalize tiny assets
				// as well, rejecting only scales that float matrices cannot represent.
				const double requestedScale = asset.boundsLargestAxis > 0.0f
					? double(desc.normalizedSize) / asset.boundsLargestAxis : 1.0;
				if(!std::isfinite(requestedScale)
					|| std::abs(requestedScale) > std::numeric_limits<float>::max()) return false;
				const float scale = static_cast<float>(requestedScale);
				if(requestedScale != 0.0 && scale == 0.0f) return false;
				transform = transform * Math::Scaling(scale) * Math::Translation(Math::float3(
					-asset.boundsCenter.x,
					-asset.boundsCenter.y,
					-asset.boundsCenter.z));
			}
			outTransform = transform * asset.assetTransform;
			for(float value : outTransform.m) if(!std::isfinite(value)) return false;
			return true;
		}

        bool LoadExternalImage(const std::string& path, Image& image,
            const ModelLoadOptions& options, ModelLoadBudget& budget,
            uint64_t temporaryDecodeLimit=UINT64_MAX)
        {
            std::error_code error;
            const auto size=std::filesystem::file_size(path,error);
            if(error || !size || size>options.maxSourceBytes || size>options.maxParserInputBytes ||
                size>static_cast<uint64_t>(std::numeric_limits<int>::max()))
                return ReportModelError(path,"external image source size is invalid or exceeds its byte limit");
            std::ifstream file(path,std::ios::binary);
            std::vector<uint8_t> encoded(static_cast<size_t>(size));
            if(!file.read(reinterpret_cast<char*>(encoded.data()),static_cast<std::streamsize>(encoded.size())))
                return ReportModelError(path,"failed to read external image");
            bool exceeded=false;
            const uint64_t decodeLimit=temporaryDecodeLimit==UINT64_MAX
                ? options.maxDecodedBytes-budget.DecodedBytes() : temporaryDecodeLimit;
            if(!LoadImage(encoded.data(),encoded.size(),image,
                decodeLimit,options.maxTextureDimension,&exceeded))
                return ReportModelError(path,exceeded ? "external image pixels exceed maxDecodedBytes" : "failed to decode external image");
            if(!budget.AddBytes(image.GetPixels().size(),1,"external image pixels")) return false;
            image.SetSourcePath(path);
            return true;
        }
		[[nodiscard]] Image CreateTextureIfPresent(
			const ModelMaterialInfo& material,
			MaterialTextureKind kind,
			const std::vector<Image>& importedTextures, const ModelLoadOptions& options, ModelLoadBudget& budget)
		{
			const uint32_t slot = static_cast<uint32_t>(kind);
			const uint32_t textureIndex = material.textureIndices[slot];
			Image texture;
			if(textureIndex < importedTextures.size())
			{
				if(!importedTextures[textureIndex].IsValid()) return {};
				texture = importedTextures[textureIndex];
			}
			else if(material.hasTexture[slot] && !material.texturePaths[slot].empty())
			{
				if(!LoadExternalImage(material.texturePaths[slot], texture, options, budget)) return {};
			}
			else return {};
			// 재질별 색 공간만 바꾸며 이미 디코딩한 픽셀은 같은 저장소를 공유한다.
			texture.SetColorSpace((kind == MaterialTextureKind::BaseColor || kind == MaterialTextureKind::Emissive)
				? ColorSpace::Srgb : ColorSpace::Linear);
			return texture;
		}

		[[nodiscard]] bool BuildSceneMaterial(
			const ModelMaterialInfo& source,
			const std::vector<Image>& importedTextures, MaterialDesc& material,
            const ModelLoadOptions& options, ModelLoadBudget& budget)
		{
			material = source.material;
			material.baseColorTexture = CreateTextureIfPresent(source, MaterialTextureKind::BaseColor, importedTextures, options, budget);
			material.metallicRoughnessTexture = CreateTextureIfPresent(source, MaterialTextureKind::MetallicRoughness, importedTextures, options, budget);
			material.normalTexture = CreateTextureIfPresent(source, MaterialTextureKind::Normal, importedTextures, options, budget);
			material.occlusionTexture = CreateTextureIfPresent(source, MaterialTextureKind::Occlusion, importedTextures, options, budget);
			material.emissiveTexture = CreateTextureIfPresent(source, MaterialTextureKind::Emissive, importedTextures, options, budget);
			const Image* textures[]={&material.baseColorTexture,&material.metallicRoughnessTexture,&material.normalTexture,&material.occlusionTexture,&material.emissiveTexture};
            for(uint32_t slot=0;slot<5;++slot)
                if((source.textureIndices[slot]<importedTextures.size() || (source.hasTexture[slot] && !source.texturePaths[slot].empty())) && !textures[slot]->IsValid())return false;
            if(!source.roughnessTexturePath.empty())
            {
                Image roughness;
                if(!LoadExternalImage(source.roughnessTexturePath,roughness,options,budget))return false;
                const auto& orm=material.metallicRoughnessTexture;
                const bool hasOrm=orm.IsValid();
                const auto& roughnessPixels=roughness.GetPixels();
                const auto& ormPixels=orm.GetPixels();
                const uint32_t width=std::max(roughness.GetWidth(),hasOrm?orm.GetWidth():1u);
                const uint32_t height=std::max(roughness.GetHeight(),hasOrm?orm.GetHeight():1u);
                const uint64_t pixelCount=uint64_t(width)*height;
                if(pixelCount>std::numeric_limits<size_t>::max()/4u)
                    return ReportModelError(source.roughnessTexturePath,"combined roughness texture exceeds addressable memory");
                if(!budget.AddBytes(pixelCount,4,"combined roughness texture pixels"))return false;
                std::vector<uint8_t> pixels(static_cast<size_t>(pixelCount*4u),255u);
                // 크기가 다르면 정규화된 같은 UV의 원본 텍셀을 사용하며 색 공간 변환은 하지 않는다.
                const auto offset=[width,height](const Image& image,uint32_t x,uint32_t y) {
                    const auto sx=static_cast<uint32_t>(uint64_t(x)*image.GetWidth()/width);
                    const auto sy=static_cast<uint32_t>(uint64_t(y)*image.GetHeight()/height);
                    return (size_t(sy)*image.GetWidth()+sx)*4u;
                };
                for(uint32_t y=0;y<height;++y)
                    for(uint32_t x=0;x<width;++x)
                    {
                        const size_t target=(size_t(y)*width+x)*4u;
                        if(hasOrm)
                        {
                            const auto original=offset(orm,x,y);
                            pixels[target]=ormPixels[original];
                            pixels[target+2u]=ormPixels[original+2u];
                            pixels[target+3u]=ormPixels[original+3u];
                        }
                        pixels[target+1u]=roughnessPixels[offset(roughness,x,y)];
                    }
                material.metallicRoughnessTexture=Image(width,height,std::move(pixels),ColorSpace::Linear);
            }
            return true;
		}

		[[nodiscard]] std::string BuildModelAssetCacheKey(
			const std::string& path,
			const ModelLoadOptions& options)
		{
			std::error_code error;
			std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(path, error);
			if(error)
			{
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
			key += "|maxTextureDimension=" + std::to_string(options.maxTextureDimension);
			key += "|maxTextures=" + std::to_string(options.maxTextures);
			key += "|maxMorphTargetsPerMesh=" + std::to_string(options.maxMorphTargetsPerMesh);
			key += "|maxNodes=" + std::to_string(options.maxNodes);
			key += "|maxNodeDepth=" + std::to_string(options.maxNodeDepth);
			key += "|maxJointsPerSkin=" + std::to_string(options.maxJointsPerSkin);
			key += "|maxAnimationKeys=" + std::to_string(options.maxAnimationKeys);
			// Preserve every float bit (including signed zero) without locale or decimal rounding.
			const auto floatIdentity = [](float value) {
				uint32_t bits;
				static_assert(sizeof(bits) == sizeof(value));
				std::memcpy(&bits, &value, sizeof(bits));
				return std::to_string(bits);
			};
			key += "|fbxBakeRate=" + floatIdentity(options.fbxBakeRate);
			key += "|fbxConstantTrackTolerance=" + floatIdentity(options.fbxConstantTrackTolerance);
			key += "|fbxSpecularTextureIsOrm=" + std::to_string(options.fbxSpecularTextureIsOrm);
			key += "|fbxSpecularTextureHasOcclusion=" + std::to_string(options.fbxSpecularTextureHasOcclusion);
			key += "|fbxBumpTextureIsNormalMap=" + std::to_string(options.fbxBumpTextureIsNormalMap);
			key += "|allowMaterialFallbacks=" + std::to_string(options.allowMaterialFallbacks);
			key += "|fbxBaseColorTextureHasOpacity=" + std::to_string(options.fbxBaseColorTextureHasOpacity);
			key += "|fbxBaseColorAlphaMode=" + std::to_string(options.fbxBaseColorAlphaMode ? int(*options.fbxBaseColorAlphaMode) : -1);
			key += "|normalMapYSign=" + floatIdentity(options.normalMapYSign);
			key += "|importLights=" + std::to_string(options.importLights);
			key += "|materialOverrides=" + std::to_string(options.materialOverrides.size());
			for(const ModelMaterialOverride& value : options.materialOverrides)
			{
				key += "|name=" + std::to_string(value.name.size()) + ":" + value.name;
				key += "|transmission=" + floatIdentity(value.transmissionFactor);
				key += "|ior=" + floatIdentity(value.indexOfRefraction);
				key += "|thickness=" + floatIdentity(value.thickness);
				key += "|attenuation=" + floatIdentity(value.attenuationColor.x) + ","
					+ floatIdentity(value.attenuationColor.y) + "," + floatIdentity(value.attenuationColor.z);
				key += "|distance=" + floatIdentity(value.attenuationDistance);
				key += "|roughness=" + floatIdentity(value.roughnessFactor);
				key += "|metallic=" + floatIdentity(value.metallicFactor);
				key += "|alphaMode=" + std::to_string(value.alphaMode ? int(*value.alphaMode) : -1);
				key += "|doubleSided=" + std::to_string(value.doubleSided ? int(*value.doubleSided) : -1);
				key += "|fbxFallbackTextureUvSet=" + (value.fbxFallbackTextureUvSet
					? std::to_string(value.fbxFallbackTextureUvSet->size()) + ":" + *value.fbxFallbackTextureUvSet : "unset");
				key += "|baseColor=" + floatIdentity(value.baseColor.x) + "," + floatIdentity(value.baseColor.y)
					+ "," + floatIdentity(value.baseColor.z) + "," + floatIdentity(value.baseColor.w);
				for(size_t slot = 0; slot < value.texturePaths.size(); ++slot)
					key += "|texture" + std::to_string(slot) + "=" + std::to_string(value.texturePaths[slot].size())
						+ ":" + value.texturePaths[slot];
				key += "|roughnessTexture=" + std::to_string(value.roughnessTexturePath.size()) + ":" + value.roughnessTexturePath;
			}
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

	bool Detail::BuildSkinTransform(
		const SkinInfluence& influence,
		const std::vector<SkinJointMatrices>& palette,
		SkinTransform& outTransform, Math::float4x4* fallbackNormalMatrix)
	{
		if(!std::isfinite(influence.dqBlendWeight)
			|| influence.dqBlendWeight < 0.0f
			|| influence.dqBlendWeight > 1.0f) return false;
		double totalWeight = 0.0;
		for(size_t component = 0u; component < 4u; ++component)
		{
			const float weight = influence.weights[component];
			if(!std::isfinite(weight) || weight < 0.0f) return false;
			if(weight > 0.0f && influence.jointIndices[component] >= palette.size()) return false;
			totalWeight += weight;
		}
		if(totalWeight <= 1.0e-6f)
		{
			outTransform.copySource = true;
			return true;
		}

		Math::float4x4 linearMatrix = {};
		if(fallbackNormalMatrix) *fallbackNormalMatrix = {};
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
			const float weight = static_cast<float>(rawWeight / totalWeight);
			const SkinJointMatrices& joint = palette[influence.jointIndices[component]];
			for(size_t matrixIndex = 0u; matrixIndex < 16u; ++matrixIndex)
			{
				linearMatrix.m[matrixIndex] += joint.positionMatrix.m[matrixIndex] * weight;
				if(fallbackNormalMatrix) fallbackNormalMatrix->m[matrixIndex] += joint.normalMatrix.m[matrixIndex] * weight;
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
		outTransform.matrix = finalMatrix;
		outTransform.copySource = false;
		return true;
	}

	bool Detail::BuildSkinTransformGroups(const std::vector<SkinInfluence>& influences,
		std::vector<uint32_t>& representatives, std::vector<uint32_t>& vertexGroups)
	{
		representatives.clear();
		vertexGroups.clear();
		if(influences.size() > UINT32_MAX) return false;
		std::map<std::array<uint8_t, sizeof(SkinInfluence)>, uint32_t> groups;
		vertexGroups.reserve(influences.size());
		for(size_t vertex = 0; vertex < influences.size(); ++vertex)
		{
			std::array<uint8_t, sizeof(SkinInfluence)> key;
			std::memcpy(key.data(), &influences[vertex], key.size());
			const auto [entry, inserted] = groups.emplace(key, static_cast<uint32_t>(representatives.size()));
			if(inserted) representatives.push_back(static_cast<uint32_t>(vertex));
			vertexGroups.push_back(entry->second);
		}
		return true;
	}

	static bool SkinVertexImpl(
		const Vertex& source,
		const SkinInfluence& influence,
		const std::vector<SkinJointMatrices>& palette,
		Vertex& outVertex, bool positionOnly)
	{
		Detail::SkinTransform transform;
		Math::float4x4 fallbackNormalMatrix = {};
		if(!Detail::BuildSkinTransform(influence, palette, transform,
			positionOnly ? nullptr : &fallbackNormalMatrix)) return false;
		outVertex = source;
		if(transform.copySource) return true;
		const auto& finalMatrix = transform.matrix;
		outVertex.position = Detail::ApplySkinTransform(transform, source.position);
		if(positionOnly)
			return std::isfinite(outVertex.position.x) && std::isfinite(outVertex.position.y)
				&& std::isfinite(outVertex.position.z);
		Math::float4x4 finalNormalMatrix = {};
		if(!Math::InverseTranspose(finalMatrix, finalNormalMatrix))
			finalNormalMatrix = fallbackNormalMatrix;
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

	bool SkinVertex(const Vertex& source, const SkinInfluence& influence,
		const std::vector<SkinJointMatrices>& palette, Vertex& outVertex)
	{
		return SkinVertexImpl(source, influence, palette, outVertex, false);
	}

	bool Detail::SkinPosition(const Vertex& source, const SkinInfluence& influence,
		const std::vector<SkinJointMatrices>& palette, Math::float3& outPosition)
	{
		Vertex output;
		if(!SkinVertexImpl(source, influence, palette, output, true)) return false;
		outPosition = output.position;
		return true;
	}

	bool LoadModelAsset(
		ModelScene& scene,
		const std::string& path,
		const ModelLoadOptions& options,
		ModelAssetID* outAsset)
	{
		if(outAsset != nullptr) *outAsset = ModelAssetID::Invalid;
		if(options.textureDecodeThreadCount == 0)
			return ReportModelError(path, "textureDecodeThreadCount must be at least one");
		const std::string cacheKey = BuildModelAssetCacheKey(path, options);
		const ModelAssetID cachedAsset = scene.FindModelAsset(cacheKey);
		if(IsValid(cachedAsset))
		{
			if(outAsset != nullptr) *outAsset = cachedAsset;
			return true;
		}

		ModelData model = {};
        uint64_t decodedBytes=0;
		if(!LoadModel(path, model, options, decodedBytes)) return false;
        ModelLoadBudget budget(path,options);
        if(!budget.AddBytes(decodedBytes,1,"decoded model data")) return false;
		for(size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex)
		{
			const auto& modelMesh = model.meshes[meshIndex];
			const auto* geometry = ResolveGeometry(model, modelMesh);
			if(!geometry || (modelMesh.geometryIndex != UINT32_MAX &&
				(modelMesh.geometryIndex >= meshIndex || !modelMesh.mesh.vertices.empty() ||
				 !modelMesh.mesh.indices.empty() || !modelMesh.skinInfluences.empty() ||
				 !modelMesh.morphTargets.empty() || modelMesh.skinIndex != UINT32_MAX ||
				 !geometry->skinInfluences.empty() || !geometry->morphTargets.empty() ||
				 geometry->skinIndex != UINT32_MAX)))
				return ReportModelError(path, "invalid shared geometry reference");
			if(geometry->mesh.vertices.empty() || geometry->mesh.indices.empty()) continue;
			if(modelMesh.nodeIndex != UINT32_MAX && modelMesh.nodeIndex >= model.nodes.size()) return false;
			if(modelMesh.skinIndex != UINT32_MAX && (modelMesh.skinIndex >= model.skins.size() ||
				modelMesh.nodeIndex == UINT32_MAX)) return false;
		}

		const ModelBounds bounds = ComputeModelBounds(model);
		std::string extension = std::filesystem::path(path).extension().string();
		std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) {
			return static_cast<char>(std::tolower(value));
		});
		const bool ufbxTexturesDecoded = extension == ".fbx" || extension == ".obj";
		struct ImageJob
		{
			size_t index;
			std::string path;
			ColorSpace colorSpace;
			uint64_t decodedBytes;
			uint64_t temporaryDecodeLimit;
		};
		std::vector<ImageJob> imageJobs;
		imageJobs.reserve(model.textures.size());
		for(size_t index = 0; index < model.textures.size(); ++index)
		{
			const Image& texture = model.textures[index];
			if(!texture.IsValid())
			{
				// The ufbx loader already attempted bounded decoding. Do not retry without its budget.
				if(ufbxTexturesDecoded) return false;
				const std::string& imagePath = texture.GetSourcePath();
				uint32_t width = 0, height = 0;
				if(imagePath.empty() || !GetImageDimensions(imagePath, width, height,options.maxTextureDimension))
					return ReportModelError(path, "failed to read external image dimensions: " + imagePath);
				// Reserve retained pixels, not discarded full-resolution pixels. With
				// resizing, a separate ceiling still bounds the decoder's source RGBA.
				const uint64_t retainedBytes=uint64_t(width)*height*4;
				const uint64_t temporaryLimit=options.maxTextureDimension
					? options.maxDecodedBytes-budget.DecodedBytes() : retainedBytes;
				if(!budget.AddBytes(width, uint64_t(height) * 4, "external image pixels")) return false;
				imageJobs.push_back({index,imagePath,texture.GetColorSpace(),retainedBytes,temporaryLimit});
			}
		}
		if(!imageJobs.empty())
		{
			std::atomic<size_t> nextImage{0};
			std::atomic<bool> failed{false};
			const auto decode = [&]() -> bool {
				try
				{
					while(!failed.load(std::memory_order_relaxed))
					{
						const size_t jobIndex = nextImage.fetch_add(1, std::memory_order_relaxed);
						if(jobIndex >= imageJobs.size()) break;
						const auto& job = imageJobs[jobIndex];
						ModelLoadOptions localOptions = options;
						localOptions.maxDecodedBytes = job.decodedBytes;
						ModelLoadBudget localBudget(job.path, localOptions);
						Image decoded;
						// LoadExternalImage reads immutable encoded bytes, checks their
						// dimensions again before decoding, and enforces this reservation.
						if(!LoadExternalImage(job.path,decoded,localOptions,localBudget,job.temporaryDecodeLimit))
						{
							failed.store(true, std::memory_order_relaxed);
							return false;
						}
						decoded.SetColorSpace(job.colorSpace);
						// The vector's shape is fixed and each slot has exactly one job.
						model.textures[job.index] = std::move(decoded);
					}
					return !failed.load(std::memory_order_relaxed);
				}
				catch(...)
				{
					failed.store(true, std::memory_order_relaxed);
					throw;
				}
			};
			// Resizing keeps only small outputs, so do not overlap several large
			// PNG/JPEG decode temporaries outside their retained-pixel reservations.
			const size_t decodeThreads=options.maxTextureDimension ? 1u : options.textureDecodeThreadCount;
			const size_t workerCount = std::min<size_t>(decodeThreads,imageJobs.size());
			std::vector<std::future<bool>> workers;
			workers.reserve(workerCount);
			std::exception_ptr exception;
			try
			{
				for(size_t worker = 0; worker < workerCount; ++worker)
					workers.emplace_back(std::async(std::launch::async, decode));
			}
			catch(...)
			{
				failed.store(true, std::memory_order_relaxed);
				exception = std::current_exception();
			}
			bool imagesLoaded = !exception;
			for(auto& worker : workers)
			{
				try { if(!worker.get()) imagesLoaded = false; }
				catch(...)
				{
					imagesLoaded = false;
					if(!exception) exception = std::current_exception();
				}
			}
			if(exception)
			{
				try { std::rethrow_exception(exception); }
				catch(const std::exception& error)
				{
					return ReportModelError(path, std::string("external image decoding failed: ") + error.what());
				}
				catch(...) { return ReportModelError(path, "external image decoding failed"); }
			}
			if(!imagesLoaded) return false;
		}
		std::vector<MaterialID> materials;
		materials.reserve(model.materials.size());
		for(const ModelMaterialInfo& material : model.materials)
        {
            MaterialDesc prepared;
            if(!BuildSceneMaterial(material,model.textures,prepared,options,budget))return false;
            materials.push_back(scene.CreateMaterial(prepared));
        }
		if(materials.empty()) materials.push_back(scene.CreateMaterial(MaterialDesc{}));

		ModelAsset asset;
		asset.cacheKey = cacheKey;
		asset.nodes = std::move(model.nodes);
		asset.skins = std::move(model.skins);
		asset.animations = std::move(model.animations);
		asset.directionalLights = std::move(model.directionalLights);
		asset.pointLights = std::move(model.pointLights);
		asset.spotLights = std::move(model.spotLights);
		asset.assetTransform = model.assetTransform;
		asset.hasBounds = bounds.valid;
		if(bounds.valid)
		{
			asset.boundsCenter = (bounds.min + bounds.max) * 0.5f;
			asset.boundsLargestAxis = std::max(
				std::max(bounds.max.x - bounds.min.x, bounds.max.y - bounds.min.y),
				bounds.max.z - bounds.min.z);
		}
		asset.meshes.reserve(model.meshes.size());
		std::vector<MeshID> geometryIds(model.meshes.size(), MeshID::Invalid);
		for(size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex)
		{
			auto& modelMesh = model.meshes[meshIndex];
			const size_t geometryIndex = modelMesh.geometryIndex == UINT32_MAX ? meshIndex : modelMesh.geometryIndex;
			auto& geometry = model.meshes[geometryIndex];
			if(geometry.mesh.vertices.empty() || geometry.mesh.indices.empty()) continue;
			const MaterialID material = modelMesh.materialIndex < materials.size()
				? materials[modelMesh.materialIndex]
				: materials.front();
			ModelAssetMesh assetMesh;
			if(!IsValid(geometryIds[geometryIndex]))
			{
				geometryIds[geometryIndex] = scene.CreateMesh(geometry.mesh);
				scene.m_meshSkinInfluences.resize(scene.Meshes().size());
				scene.m_meshSkinInfluences[ToIndex(geometryIds[geometryIndex])] = std::move(geometry.skinInfluences);
			}
			assetMesh.mesh = geometryIds[geometryIndex];
			assetMesh.material = material;
			assetMesh.nodeIndex = modelMesh.nodeIndex;
			assetMesh.skinIndex = modelMesh.skinIndex;
			assetMesh.morphTargets = std::move(modelMesh.morphTargets);
			asset.meshes.push_back(std::move(assetMesh));
		}
		if(asset.meshes.empty()) return false;

		const ModelAssetID assetId = scene.CreateModelAsset(std::move(asset));
		if(outAsset != nullptr) *outAsset = assetId;
		return true;
	}

	bool InstantiateModel(
		ModelScene& scene,
		ModelAssetID assetId,
		const ModelSceneDesc& desc,
		ModelInstanceID* outInstance)
	{
		if(outInstance != nullptr) *outInstance = ModelInstanceID::Invalid;
		const ModelAsset* asset = scene.TryGetModelAsset(assetId);
		if(asset == nullptr) return false;
		const bool animatedModel = !asset->skins.empty() || !asset->animations.empty()
			|| std::any_of(asset->meshes.begin(), asset->meshes.end(), [](const ModelAssetMesh& mesh) {
				return mesh.nodeIndex != UINT32_MAX || !mesh.morphTargets.empty();
			});
		Math::float4x4 transform;
		if(!BuildModelSceneTransform(*asset, desc, transform))
			return ReportModelError(desc.path, "model transform or normalization exceeds the finite float range");

		ModelInstanceID instanceId = ModelInstanceID::Invalid;
		if(animatedModel)
		{
			ModelInstance instance;
			instance.assetId = assetId;
			instance.rootTransform = transform;
			instanceId = scene.CreateModelInstance(std::move(instance));
			if(!IsValid(instanceId)) return false;
		}

		bool createdAny = false;
		for(size_t assetMeshIndex = 0; assetMeshIndex < asset->meshes.size(); ++assetMeshIndex)
		{
			const ModelAssetMesh& assetMesh = asset->meshes[assetMeshIndex];
			if(!IsValid(assetMesh.mesh) || !IsValid(assetMesh.material)) continue;
			Math::float4x4 entityTransform = transform;
			if(assetMesh.nodeIndex != UINT32_MAX)
			{
				const ModelInstance& instance = scene.GetModelInstance(instanceId);
				if(assetMesh.nodeIndex >= instance.globalPose.size()) return false;
				entityTransform = transform * instance.globalPose[assetMesh.nodeIndex];
			}
			const EntityID entity = scene.CreateEntity(
				assetMesh.mesh,
				assetMesh.material,
				entityTransform,
				desc.lighting);
			if(assetMesh.nodeIndex != UINT32_MAX && !scene.BindEntityToModel(
				instanceId,
				entity,
				assetMesh.nodeIndex,
				assetMesh.skinIndex,
				static_cast<uint32_t>(assetMeshIndex))) return false;
			createdAny = true;
		}
		if(animatedModel)
		{
			if(!scene.UpdateAnimations(0.0f,instanceId)) return false;
			if(outInstance != nullptr) *outInstance = instanceId;
		}
		if(createdAny && (!asset->directionalLights.empty() || !asset->pointLights.empty() || !asset->spotLights.empty()))
		{
			// Uniform model normalization scales distances by s and inverse-square
			// intensities by s^2, preserving incident lighting on the scaled model.
			const float xScale = Math::Length(Math::TransformVector(transform, Math::float3(1, 0, 0)));
			const float yScale = Math::Length(Math::TransformVector(transform, Math::float3(0, 1, 0)));
			const float zScale = Math::Length(Math::TransformVector(transform, Math::float3(0, 0, 1)));
			const float scale = std::max({xScale, yScale, zScale});
			if(!std::isfinite(scale) || scale <= 0.0f)
				return ReportModelError(desc.path, "model light transform must have a finite nonzero scale");
			if(scale - std::min({xScale, yScale, zScale}) > scale * 1.0e-4f)
				ReportModelWarning(desc.path, "nonuniform model scaling approximates source light ranges and intensities using the largest axis scale");
			for(const DirectionalLight& source : asset->directionalLights)
			{
				DirectionalLight light = source;
				light.direction = Math::NormalizeOr(Math::TransformVector(transform, source.direction), Math::float3(0, 0, 1));
				if(!scene.Add(light)) return false;
			}
			for(const PointLight& source : asset->pointLights)
			{
				PointLight light = source;
				light.position = Math::TransformPoint(transform, source.position);
				light.range *= scale;
				light.sourceRadius *= scale;
				light.intensity = static_cast<float>(double(source.intensity) * scale * scale);
				if(!std::isfinite(light.position.x) || !std::isfinite(light.position.y) || !std::isfinite(light.position.z)
					|| !std::isfinite(light.range) || !std::isfinite(light.sourceRadius) || !std::isfinite(light.intensity))
					return ReportModelError(desc.path, "transformed point light exceeds finite float range");
				if(!scene.Add(light)) return false;
			}
			for(const SpotLight& source : asset->spotLights)
			{
				SpotLight light = source;
				light.position = Math::TransformPoint(transform, source.position);
				light.direction = Math::NormalizeOr(Math::TransformVector(transform, source.direction), Math::float3(0, 0, -1));
				light.range *= scale;
				light.sourceRadius *= scale;
				light.intensity = static_cast<float>(double(source.intensity) * scale * scale);
				if(!std::isfinite(light.position.x) || !std::isfinite(light.position.y) || !std::isfinite(light.position.z)
					|| !std::isfinite(light.range) || !std::isfinite(light.sourceRadius) || !std::isfinite(light.intensity))
					return ReportModelError(desc.path, "transformed spot light exceeds finite float range");
				if(!scene.Add(light)) return false;
			}
		}
		return createdAny;
	}

	bool AddModelToScene(ModelScene& scene, const ModelSceneDesc& desc, ModelInstanceID* outInstance)
	{
		ModelAssetID asset = ModelAssetID::Invalid;
		return LoadModelAsset(scene, desc.path, desc.loadOptions, &asset)
			&& InstantiateModel(scene, asset, desc, outInstance);
	}

	bool AddModelToScene(ModelScene& scene, const ModelSceneDesc& desc)
	{
		return AddModelToScene(scene, desc, nullptr);
	}

	bool AddModelToScene(ModelScene& scene, const std::string& path, const Math::float3& position)
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
			const auto* geometry = ResolveGeometry(model, sourceMesh);
			if(!geometry) return {};
			MeshData defaultMorphedMesh;
			const MeshData* sourceGeometry = &geometry->mesh;
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
			if(sourceMesh.nodeIndex != UINT32_MAX)
			{
				if(sourceMesh.nodeIndex >= bindGlobals.size()) return {};
				meshNodeGlobal = bindGlobals[sourceMesh.nodeIndex];
			}
			const Math::float4x4 outerTransform = model.assetTransform * meshNodeGlobal;
			const bool reflected = Math::Determinant3x3(outerTransform) < 0.0f;
			Math::float4x4 outerNormalTransform = {};
			if(!Math::InverseTranspose(outerTransform, outerNormalTransform)) return {};

			std::vector<SkinJointMatrices> bindPalette;
			const bool hasSkin = sourceMesh.skinIndex != UINT32_MAX;
			if(hasSkin)
			{
				if(sourceMesh.skinIndex >= model.skins.size()
					|| sourceMesh.skinInfluences.size() != sourceGeometry->vertices.size()
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
				const Vertex& sourceVertex = sourceGeometry->vertices[vertexIndex];
				Vertex vertex = sourceVertex;
				if(hasSkin)
				{
					const SkinInfluence& influence = sourceMesh.skinInfluences[vertexIndex];
					if(!SkinVertex(sourceVertex, influence, bindPalette, vertex)) return {};
				}

				vertex.position = Math::TransformPoint(outerTransform, vertex.position);
				vertex.normal = NormalizeOr(
					Math::TransformVector(outerNormalTransform, vertex.normal),
					Math::float3(0.0f, 0.0f, 1.0f));
				const Math::float3 rawTransformedTangent = Math::TransformVector(
					outerTransform, Math::float3(vertex.tangent.x, vertex.tangent.y, vertex.tangent.z));
				const Math::float3 transformedTangent = NormalizeOr(
					rawTransformedTangent - vertex.normal * Dot(vertex.normal, rawTransformedTangent),
					BuildFallbackTangent(vertex.normal));
				if(reflected) vertex.tangent.w = -vertex.tangent.w;
				vertex.tangent = Math::float4(
					transformedTangent.x,
					transformedTangent.y,
					transformedTangent.z,
					vertex.tangent.w);
				merged.vertices.push_back(vertex);
			}
			const size_t indexOffset = merged.indices.size();
			for(const uint32_t index : sourceGeometry->indices)
			{
				if(index >= sourceGeometry->vertices.size()) return {};
				merged.indices.push_back(vertexOffset + index);
			}
			if(reflected)
				for(size_t index = indexOffset; index + 2 < merged.indices.size(); index += 3)
					std::swap(merged.indices[index + 1], merged.indices[index + 2]);
		}
		return merged;
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
