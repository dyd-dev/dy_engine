#include "Graphics/ModelLoaderInternal.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <sstream>
#include <utility>
#include <vector>

#include "Graphics/ImageFile.h"

#include <fastgltf/core.hpp>
#include <fastgltf/math.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

template<class... Ts> struct dy_gltf_visitor : Ts... { using Ts::operator()...; };
template<class... Ts> dy_gltf_visitor(Ts...) -> dy_gltf_visitor<Ts...>;

namespace dy::Graphics::ModelLoaderInternal
{
	namespace
	{
		class ModelLoadBudget
		{
		public:
			ModelLoadBudget(const std::string& path, const ModelLoadOptions& options)
				: m_path(path), m_options(options)
			{
			}

			[[nodiscard]] bool CheckNodes(uint64_t count) const
			{
				if(count <= m_options.maxNodes) return true;
				std::ostringstream message;
				message << "model node count " << count << " exceeds limit " << m_options.maxNodes;
				return ReportModelError(ModelDiagnosticCode::ResourceLimitExceeded, m_path, message.str());
			}

			[[nodiscard]] bool CheckJoints(uint64_t count) const
			{
				if(count <= m_options.maxJointsPerSkin) return true;
				std::ostringstream message;
				message << "skin joint count " << count << " exceeds limit " << m_options.maxJointsPerSkin;
				return ReportModelError(ModelDiagnosticCode::ResourceLimitExceeded, m_path, message.str());
			}

			[[nodiscard]] bool AddAnimationKeys(uint64_t count)
			{
				if(count > std::numeric_limits<uint64_t>::max() - m_animationKeys
					|| m_animationKeys + count > m_options.maxAnimationKeys)
				{
					std::ostringstream message;
					message << "animation key count exceeds limit " << m_options.maxAnimationKeys;
					return ReportModelError(ModelDiagnosticCode::ResourceLimitExceeded, m_path, message.str());
				}
				m_animationKeys += count;
				return true;
			}

			[[nodiscard]] bool AddBytes(uint64_t count, uint64_t stride, const char* category)
			{
				if(count != 0u && stride > std::numeric_limits<uint64_t>::max() / count)
				{
					return ReportModelError(
						ModelDiagnosticCode::ResourceLimitExceeded,
						m_path,
						std::string(category) + " byte-size calculation overflow");
				}
				const uint64_t bytes = count * stride;
				if(bytes > std::numeric_limits<uint64_t>::max() - m_decodedBytes
					|| m_decodedBytes + bytes > m_options.maxDecodedBytes)
				{
					std::ostringstream message;
					message << "estimated decoded bytes exceed limit " << m_options.maxDecodedBytes
						<< " while accounting for " << category;
					return ReportModelError(ModelDiagnosticCode::ResourceLimitExceeded, m_path, message.str());
				}
				m_decodedBytes += bytes;
				return true;
			}

		private:
			const std::string& m_path;
			const ModelLoadOptions& m_options;
			uint64_t m_decodedBytes = 0u;
			uint64_t m_animationKeys = 0u;
		};
		[[nodiscard]] uint32_t EnsureDefaultMaterial(ModelData& model)
		{
			const uint32_t defaultMaterialIndex = static_cast<uint32_t>(model.materials.size());
			model.materials.push_back({});
			return defaultMaterialIndex;
		}
		void SetTexturePath(ModelMaterialInfo& material, MaterialTextureKind kind, std::string path)
		{
			const uint32_t index = static_cast<uint32_t>(kind);
			material.texturePaths[index] = std::move(path);
			material.hasTexture[index] = !material.texturePaths[index].empty();
		}
		void ApplyGltfMaterialTexture(
			const std::filesystem::path& basePath,
			const fastgltf::Asset& gltf,
			const fastgltf::TextureInfo& textureInfo,
			ModelMaterialInfo& material,
			MaterialTextureKind kind,
			const std::string& filepath,
			std::string texCoordElement)
		{
			const size_t texCoordIndex = textureInfo.transform != nullptr
				&& textureInfo.transform->texCoordIndex.has_value()
				? textureInfo.transform->texCoordIndex.value()
				: textureInfo.texCoordIndex;
			if(texCoordIndex != 0u)
			{
				ReportModelWarning(
					ModelDiagnosticCode::UnsupportedFeature,
					filepath,
					"texture references an unsupported texture coordinate set; texture disabled",
					std::move(texCoordElement));
				return;
			}
			if(textureInfo.textureIndex >= gltf.textures.size()) return;
			const fastgltf::Texture& texture = gltf.textures[textureInfo.textureIndex];
			if(!texture.imageIndex.has_value() || texture.imageIndex.value() >= gltf.images.size()) return;

			const size_t imageIndex = texture.imageIndex.value();
			const uint32_t slot = static_cast<uint32_t>(kind);
			material.textureIndices[slot] = static_cast<uint32_t>(imageIndex);
			material.hasTexture[slot] = true;
			const fastgltf::Image& image = gltf.images[imageIndex];
			std::visit(dy_gltf_visitor{
				[&](const fastgltf::sources::URI& filePath) {
					SetTexturePath(material, kind, (basePath / filePath.uri.fspath()).string());
				},
				[&](const auto&) {}
			}, image.data);
		}

		[[nodiscard]] bool LoadGltfTextureAssets(
			const std::string& filepath,
			const std::filesystem::path& basePath,
			const fastgltf::Asset& gltf,
			uint64_t maxDecodedBytes,
			ModelData& outModel,
			uint64_t& outDecodedBytes)
		{
			outDecodedBytes = 0u;
			outModel.textures.clear();
			outModel.textures.reserve(gltf.images.size());
			for(size_t imageIndex = 0; imageIndex < gltf.images.size(); ++imageIndex)
			{
				const fastgltf::Image& image = gltf.images[imageIndex];
				TextureAsset textureAsset;
				fastgltf::span<const std::byte> encodedBytes;
				bool hasEmbeddedBytes = false;
				bool validSource = true;
				std::visit(dy_gltf_visitor{
					[&](const fastgltf::sources::URI& source) {
						if(!source.uri.isLocalPath())
						{
							validSource = false;
							return;
						}
						textureAsset.sourcePath = (basePath / source.uri.fspath()).string();
					},
					[&](const fastgltf::sources::BufferView& source) {
						if(source.bufferViewIndex >= gltf.bufferViews.size())
						{
							validSource = false;
							return;
						}
						encodedBytes = fastgltf::DefaultBufferDataAdapter{}(gltf, source.bufferViewIndex);
						hasEmbeddedBytes = true;
					},
					[&](const fastgltf::sources::Array& source) {
						encodedBytes = fastgltf::span<const std::byte>(
							source.bytes.data(), source.bytes.size_bytes());
						hasEmbeddedBytes = true;
					},
					[&](const fastgltf::sources::Vector& source) {
						encodedBytes = fastgltf::span<const std::byte>(source.bytes.data(), source.bytes.size());
						hasEmbeddedBytes = true;
					},
					[&](const fastgltf::sources::ByteView& source) {
						encodedBytes = source.bytes;
						hasEmbeddedBytes = true;
					},
					[&](const auto&) { validSource = false; }
				}, image.data);
				const std::string element = "images[" + std::to_string(imageIndex) + "]";
				if(!validSource)
				{
					return ReportModelError(
						ModelDiagnosticCode::UnsupportedFeature,
						filepath,
						"glTF image source is not supported",
						element);
				}
				if(hasEmbeddedBytes)
				{
					const uint64_t remainingBytes = outDecodedBytes <= maxDecodedBytes
						? maxDecodedBytes - outDecodedBytes
						: 0u;
					bool limitExceeded = false;
					ImageFile decoded = LoadImageMemory(
						reinterpret_cast<const uint8_t*>(encodedBytes.data()),
						encodedBytes.size(),
						remainingBytes,
						&limitExceeded);
					if(!decoded.IsValid())
					{
						return ReportModelError(
							limitExceeded
								? ModelDiagnosticCode::ResourceLimitExceeded
								: ModelDiagnosticCode::ParseFailed,
							filepath,
							limitExceeded
								? "embedded image decoded bytes exceed maxDecodedBytes"
								: "failed to decode embedded glTF image",
							element);
					}
					textureAsset.width = decoded.GetWidth();
					textureAsset.height = decoded.GetHeight();
					textureAsset.rgba8 = decoded.GetPixels();
					outDecodedBytes += static_cast<uint64_t>(textureAsset.rgba8.size());
				}
				outModel.textures.push_back(std::move(textureAsset));
			}
			return true;
		}
		[[nodiscard]] Math::float4x4 ToFloat4x4(const fastgltf::math::fmat4x4& source)
		{
			Math::float4x4 result = {};
			for(size_t column = 0; column < 4; ++column)
				for(size_t row = 0; row < 4; ++row)
					result.m[column * 4 + row] = source.col(column)[row];
			return result;
		}

		[[nodiscard]] NodeTransform ToNodeTransform(const fastgltf::TRS& source)
		{
			NodeTransform result;
			result.translation = Math::float3(source.translation.x(), source.translation.y(), source.translation.z());
			result.rotation = Math::quat(source.rotation.x(), source.rotation.y(), source.rotation.z(), source.rotation.w());
			result.scale = Math::float3(source.scale.x(), source.scale.y(), source.scale.z());
			return result;
		}
		[[nodiscard]] AnimationInterpolation ToAnimationInterpolation(fastgltf::AnimationInterpolation source)
		{
			switch(source)
			{
			case fastgltf::AnimationInterpolation::Step: return AnimationInterpolation::Step;
			case fastgltf::AnimationInterpolation::CubicSpline: return AnimationInterpolation::CubicSpline;
			default: return AnimationInterpolation::Linear;
			}
		}
		[[nodiscard]] SkinInfluence MakeSkinInfluence(
			std::vector<std::pair<uint32_t, float>> values,
			bool& truncated)
		{
			values.erase(std::remove_if(values.begin(), values.end(), [](const auto& value) {
				return value.second <= 0.0f;
			}), values.end());
			std::stable_sort(values.begin(), values.end(), [](const auto& lhs, const auto& rhs) {
				return lhs.second > rhs.second;
			});
			truncated = values.size() > 4;
			if(values.size() > 4) values.resize(4);

			SkinInfluence result;
			float total = 0.0f;
			for(const auto& value : values) total += value.second;
			if(total <= 1.0e-6f) return result;
			for(size_t index = 0; index < values.size(); ++index)
			{
				result.jointIndices[index] = values[index].first;
				result.weights[index] = values[index].second / total;
			}
			return result;
		}
		[[nodiscard]] bool IsFinite(const Math::float3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		[[nodiscard]] bool IsFinite(const Math::quat& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y)
				&& std::isfinite(value.z) && std::isfinite(value.w);
		}

		[[nodiscard]] bool IsFinite(const Math::float4x4& value)
		{
			return std::all_of(std::begin(value.m), std::end(value.m), [](float component) {
				return std::isfinite(component);
			});
		}

		[[nodiscard]] bool IsFinite(const NodeTransform& value)
		{
			return IsFinite(value.translation) && IsFinite(value.rotation) && IsFinite(value.scale);
		}

		template<typename Key>
		[[nodiscard]] bool HasStrictFiniteKeyTimes(const std::vector<Key>& keys)
		{
			if(keys.empty() || !std::isfinite(keys.front().time)) return false;
			for(size_t index = 1; index < keys.size(); ++index)
				if(!std::isfinite(keys[index].time) || keys[index].time <= keys[index - 1].time) return false;
			return true;
		}

		[[nodiscard]] bool HasStrictFiniteTimes(const std::vector<float>& times)
		{
			if(times.empty() || !std::isfinite(times.front())) return false;
			for(size_t index = 1; index < times.size(); ++index)
				if(!std::isfinite(times[index]) || times[index] <= times[index - 1]) return false;
			return true;
		}
		[[nodiscard]] bool ValidateGltfLoadLimits(
			const std::string& filepath,
			const fastgltf::Asset& gltf,
			const ModelLoadOptions& options)
		{
			ModelLoadBudget budget(filepath, options);
			if(!budget.CheckNodes(gltf.nodes.size())
				|| !budget.AddBytes(gltf.nodes.size(), sizeof(ModelNode), "nodes")
				|| !budget.AddBytes(gltf.materials.size(), sizeof(ModelMaterialInfo), "materials")) return false;

			for(const fastgltf::Skin& skin : gltf.skins)
			{
				if(!budget.CheckJoints(skin.joints.size())
					|| !budget.AddBytes(skin.joints.size(), sizeof(uint32_t) + sizeof(Math::float4x4), "skin joints"))
					return false;
			}

			for(const fastgltf::Animation& animation : gltf.animations)
			{
				if(!budget.AddBytes(1u, sizeof(AnimationClip), "animation clips")) return false;
				for(const fastgltf::AnimationChannel& channel : animation.channels)
				{
					if(channel.samplerIndex >= animation.samplers.size()) return false;
					const fastgltf::AnimationSampler& sampler = animation.samplers[channel.samplerIndex];
					if(sampler.inputAccessor >= gltf.accessors.size()
						|| sampler.outputAccessor >= gltf.accessors.size()) return false;
					const bool morphWeights = channel.path == fastgltf::AnimationPath::Weights;
					const uint64_t keyCount = morphWeights
						? gltf.accessors[sampler.outputAccessor].count
						: gltf.accessors[sampler.inputAccessor].count;
					const uint64_t keyStride = morphWeights
						? sizeof(FloatKey)
						: (channel.path == fastgltf::AnimationPath::Rotation
							? sizeof(QuatKey) : sizeof(Vec3Key));
					if(!budget.AddAnimationKeys(keyCount)
						|| !budget.AddBytes(keyCount, keyStride, "animation keys")) return false;
				}
			}

			for(const fastgltf::Node& node : gltf.nodes)
			{
				if(!node.meshIndex.has_value()) continue;
				if(node.meshIndex.value() >= gltf.meshes.size()) return false;
				const fastgltf::Mesh& sourceMesh = gltf.meshes[node.meshIndex.value()];
				for(const fastgltf::Primitive& primitive : sourceMesh.primitives)
				{
					if(primitive.targets.size() > options.maxMorphTargetsPerMesh)
					{
						return ReportModelError(
							ModelDiagnosticCode::ResourceLimitExceeded,
							filepath,
							"glTF morph target count exceeds maxMorphTargetsPerMesh");
					}
					const auto* position = primitive.findAttribute("POSITION");
					if(position == primitive.attributes.end()) continue;
					if(position->accessorIndex >= gltf.accessors.size()) return false;
					const uint64_t vertexCount = gltf.accessors[position->accessorIndex].count;
					if(!budget.AddBytes(vertexCount, sizeof(Vertex), "vertices")) return false;
					if(!budget.AddBytes(primitive.targets.size(), sizeof(MorphTarget) + sizeof(float), "morph targets"))
						return false;
					for(const auto& target : primitive.targets)
					{
						for(const fastgltf::Attribute& attribute : target)
						{
							if(attribute.name == "POSITION" || attribute.name == "NORMAL" || attribute.name == "TANGENT")
								if(!budget.AddBytes(vertexCount, sizeof(Math::float3), "morph target deltas")) return false;
						}
					}
					if(node.skinIndex.has_value()
						&& !budget.AddBytes(vertexCount, sizeof(SkinInfluence), "skin influences")) return false;
					uint64_t indexCount = vertexCount;
					if(primitive.indicesAccessor.has_value())
					{
						if(primitive.indicesAccessor.value() >= gltf.accessors.size()) return false;
						indexCount = gltf.accessors[primitive.indicesAccessor.value()].count;
					}
					if(!budget.AddBytes(indexCount, sizeof(uint32_t), "indices")) return false;
				}
			}
			return true;
		}
		[[nodiscard]] bool IsPathInsideDirectory(
			const std::filesystem::path& directory,
			const std::filesystem::path& candidate,
			std::error_code& error)
		{
			error.clear();
			const std::filesystem::path canonicalDirectory =
				std::filesystem::weakly_canonical(directory, error);
			if(error) return false;
			const std::filesystem::path canonicalCandidate =
				std::filesystem::weakly_canonical(candidate, error);
			if(error) return false;
			const std::filesystem::path relative =
				canonicalCandidate.lexically_relative(canonicalDirectory);
			if(relative.empty() || relative.is_absolute()) return false;
			for(const std::filesystem::path& component : relative)
				if(component == "..") return false;
			return true;
		}
		[[nodiscard]] bool ValidateGltfSourceBudget(
			const std::string& filepath,
			const std::filesystem::path& basePath,
			const fastgltf::Asset& gltf,
			const ModelLoadOptions& options)
		{
			std::error_code sizeError;
			const uintmax_t mainSourceSize = std::filesystem::file_size(filepath, sizeError);
			if(sizeError)
			{
				return ReportModelError(
					ModelDiagnosticCode::FileReadFailed,
					filepath,
					"failed to query glTF source file size");
			}
			if(mainSourceSize > options.maxSourceBytes)
			{
				return ReportModelError(
					ModelDiagnosticCode::ResourceLimitExceeded,
					filepath,
					"glTF source bytes exceed maxSourceBytes");
			}
			uint64_t totalSourceBytes = static_cast<uint64_t>(mainSourceSize);
			for(size_t bufferIndex = 0; bufferIndex < gltf.buffers.size(); ++bufferIndex)
			{
				const auto* uriSource =
					std::get_if<fastgltf::sources::URI>(&gltf.buffers[bufferIndex].data);
				if(uriSource == nullptr || !uriSource->uri.isLocalPath()) continue;

				const std::filesystem::path externalPath = basePath / uriSource->uri.fspath();
				sizeError.clear();
				const std::string element = "buffers[" + std::to_string(bufferIndex) + "]";
				if(!IsPathInsideDirectory(basePath, externalPath, sizeError))
				{
					return ReportModelError(
						ModelDiagnosticCode::InvalidData,
						filepath,
						sizeError
							? "failed to canonicalize external glTF buffer path"
							: "external glTF buffer path escapes the model directory",
						element);
				}
				sizeError.clear();
				const uintmax_t externalSize = std::filesystem::file_size(externalPath, sizeError);
				if(sizeError)
				{
					return ReportModelError(
						ModelDiagnosticCode::FileReadFailed,
						filepath,
						"failed to query external glTF buffer size",
						element);
				}
				if(externalSize > std::numeric_limits<uint64_t>::max()
					|| static_cast<uint64_t>(externalSize) > options.maxSourceBytes - totalSourceBytes)
				{
					return ReportModelError(
						ModelDiagnosticCode::ResourceLimitExceeded,
						filepath,
						"glTF source bytes exceed maxSourceBytes while accounting for external buffers",
						element);
				}
				totalSourceBytes += static_cast<uint64_t>(externalSize);
			}
			for(size_t imageIndex = 0; imageIndex < gltf.images.size(); ++imageIndex)
			{
				const auto* uriSource =
					std::get_if<fastgltf::sources::URI>(&gltf.images[imageIndex].data);
				if(uriSource == nullptr || !uriSource->uri.isLocalPath()) continue;

				const std::filesystem::path externalPath = basePath / uriSource->uri.fspath();
				sizeError.clear();
				const std::string element = "images[" + std::to_string(imageIndex) + "]";
				if(!IsPathInsideDirectory(basePath, externalPath, sizeError))
				{
					return ReportModelError(
						ModelDiagnosticCode::InvalidData,
						filepath,
						sizeError
							? "failed to canonicalize external glTF image path"
							: "external glTF image path escapes the model directory",
							element);
				}
				sizeError.clear();
				const uintmax_t externalSize = std::filesystem::file_size(externalPath, sizeError);
				if(sizeError)
				{
					return ReportModelError(
						ModelDiagnosticCode::FileReadFailed,
						filepath,
						"failed to query external glTF image size",
						element);
				}
				if(externalSize > std::numeric_limits<uint64_t>::max()
					|| static_cast<uint64_t>(externalSize) > options.maxSourceBytes - totalSourceBytes)
				{
					return ReportModelError(
						ModelDiagnosticCode::ResourceLimitExceeded,
						filepath,
						"glTF source bytes exceed maxSourceBytes while accounting for external images",
						element);
				}
				totalSourceBytes += static_cast<uint64_t>(externalSize);
			}
			if(totalSourceBytes > options.maxParserInputBytes)
			{
				return ReportModelError(
					ModelDiagnosticCode::ResourceLimitExceeded,
					filepath,
					"glTF parser input bytes exceed maxParserInputBytes");
			}
			return true;
		}

	}

		[[nodiscard]] bool LoadGltfModel(const std::string& filepath, ModelData& outModel, const ModelLoadOptions& options)
		{
			bool warnedInfluenceTruncation = false;
			std::error_code sourceSizeError;
			const uintmax_t sourceSize = std::filesystem::file_size(filepath, sourceSizeError);
			if(sourceSizeError || sourceSize > std::numeric_limits<uint64_t>::max())
			{
				return ReportModelError(
					ModelDiagnosticCode::FileReadFailed,
					filepath,
					"failed to query glTF source size");
			}
			if(static_cast<uint64_t>(sourceSize) > options.maxSourceBytes
				|| static_cast<uint64_t>(sourceSize) > options.maxParserInputBytes)
			{
				return ReportModelError(
					ModelDiagnosticCode::ResourceLimitExceeded,
					filepath,
					static_cast<uint64_t>(sourceSize) > options.maxSourceBytes
						? "glTF source bytes exceed maxSourceBytes"
						: "glTF parser input bytes exceed maxParserInputBytes");
			}
			auto data = fastgltf::GltfDataBuffer::FromPath(filepath);
			if(data.error() != fastgltf::Error::None)
			{
				return ReportModelError(
					ModelDiagnosticCode::FileReadFailed,
					filepath,
					"failed to read glTF file");
			}

			const std::filesystem::path basePath = std::filesystem::path(filepath).parent_path();
			{
				fastgltf::Parser metadataParser;
				auto metadataAsset = metadataParser.loadGltf(
					data.get(),
					basePath,
					fastgltf::Options::DecomposeNodeMatrices);
				if(metadataAsset.error() != fastgltf::Error::None)
				{
					return ReportModelError(
						ModelDiagnosticCode::ParseFailed,
						filepath,
						std::string("failed to parse glTF metadata: ")
							+ std::string(fastgltf::getErrorMessage(metadataAsset.error())));
				}
				if(!ValidateGltfSourceBudget(filepath, basePath, metadataAsset.get(), options))
					return false;
			}
			data.get().reset();

			fastgltf::Parser parser;
			auto asset = parser.loadGltf(
				data.get(),
				basePath,
				fastgltf::Options::LoadExternalBuffers | fastgltf::Options::DecomposeNodeMatrices);
			if(asset.error() != fastgltf::Error::None)
			{
				return ReportModelError(
					ModelDiagnosticCode::ParseFailed,
					filepath,
					std::string("failed to parse glTF: ")
						+ std::string(fastgltf::getErrorMessage(asset.error())));
			}
			const fastgltf::Error validationError = fastgltf::validate(asset.get());
			if(validationError != fastgltf::Error::None)
			{
				return ReportModelError(
					ModelDiagnosticCode::ValidationFailed,
					filepath,
					std::string("glTF validation failed: ")
						+ std::string(fastgltf::getErrorMessage(validationError)));
			}

			outModel = {};
			fastgltf::Asset& gltf = asset.get();
			if(gltf.images.size() > options.maxTextures)
			{
				return ReportModelError(
					ModelDiagnosticCode::ResourceLimitExceeded,
					filepath,
					"glTF texture count exceeds maxTextures",
					"images");
			}
			uint64_t decodedImageBytes = 0u;
			if(!LoadGltfTextureAssets(
				filepath,
				basePath,
				gltf,
				options.maxDecodedBytes,
				outModel,
				decodedImageBytes)) return false;
			ModelLoadOptions geometryOptions = options;
			geometryOptions.maxDecodedBytes = decodedImageBytes <= options.maxDecodedBytes
				? options.maxDecodedBytes - decodedImageBytes
				: 0u;
			if(!ValidateGltfLoadLimits(filepath, gltf, geometryOptions)) return false;
			outModel.materials.reserve(gltf.materials.size());
			for(size_t materialIndex = 0; materialIndex < gltf.materials.size(); ++materialIndex)
			{
				const fastgltf::Material& source = gltf.materials[materialIndex];
				const std::string materialElement = "materials[" + std::to_string(materialIndex) + "].";
				ModelMaterialInfo material = {};
				material.name = source.name.c_str();
				material.material.baseColor = Math::float4(
					source.pbrData.baseColorFactor[0],
					source.pbrData.baseColorFactor[1],
					source.pbrData.baseColorFactor[2],
					source.pbrData.baseColorFactor[3]);
				material.material.metallicFactor = source.pbrData.metallicFactor;
				material.material.roughnessFactor = source.pbrData.roughnessFactor;
				material.material.emissiveColor = Math::float3(
					source.emissiveFactor[0],
					source.emissiveFactor[1],
					source.emissiveFactor[2]);
				material.hasBaseColor = true;
				if(source.pbrData.baseColorTexture.has_value()) ApplyGltfMaterialTexture(
					basePath, gltf, source.pbrData.baseColorTexture.value(), material,
					MaterialTextureKind::BaseColor, filepath,
					materialElement + "pbrMetallicRoughness.baseColorTexture.texCoord");
				if(source.pbrData.metallicRoughnessTexture.has_value()) ApplyGltfMaterialTexture(
					basePath, gltf, source.pbrData.metallicRoughnessTexture.value(), material,
					MaterialTextureKind::MetallicRoughness, filepath,
					materialElement + "pbrMetallicRoughness.metallicRoughnessTexture.texCoord");
				if(source.normalTexture.has_value())
				{
					ApplyGltfMaterialTexture(
						basePath, gltf, source.normalTexture.value(), material,
						MaterialTextureKind::Normal, filepath,
						materialElement + "normalTexture.texCoord");
					material.material.normalScale = static_cast<float>(source.normalTexture->scale);
				}
				if(source.occlusionTexture.has_value())
				{
					ApplyGltfMaterialTexture(
						basePath, gltf, source.occlusionTexture.value(), material,
						MaterialTextureKind::Occlusion, filepath,
						materialElement + "occlusionTexture.texCoord");
					material.material.occlusionStrength = static_cast<float>(source.occlusionTexture->strength);
				}
				if(source.emissiveTexture.has_value()) ApplyGltfMaterialTexture(
					basePath, gltf, source.emissiveTexture.value(), material,
					MaterialTextureKind::Emissive, filepath,
					materialElement + "emissiveTexture.texCoord");
				outModel.materials.push_back(std::move(material));
			}
			const uint32_t defaultMaterialIndex = EnsureDefaultMaterial(outModel);

			outModel.nodes.resize(gltf.nodes.size());
			for(size_t nodeIndex = 0; nodeIndex < gltf.nodes.size(); ++nodeIndex)
			{
				const fastgltf::Node& source = gltf.nodes[nodeIndex];
				const auto* trs = std::get_if<fastgltf::TRS>(&source.transform);
				if(trs == nullptr)
				{
					return ReportModelError(
						ModelDiagnosticCode::InvalidData,
						filepath,
						"glTF node matrix could not be decomposed",
						"nodes[" + std::to_string(nodeIndex) + "].transform");
				}
				ModelNode& node = outModel.nodes[nodeIndex];
				node.name = source.name.c_str();
				node.bindTransform = ToNodeTransform(*trs);
				if(!IsFinite(node.bindTransform))
				{
					return ReportModelError(
						ModelDiagnosticCode::InvalidData,
						filepath,
						"glTF node contains non-finite transform values",
						"nodes[" + std::to_string(nodeIndex) + "].transform");
				}
				if(source.meshIndex.has_value())
				{
					if(source.meshIndex.value() >= gltf.meshes.size()) return false;
					const fastgltf::Mesh& mesh = gltf.meshes[source.meshIndex.value()];
					size_t targetCount = 0u;
					for(const fastgltf::Primitive& primitive : mesh.primitives)
					{
						if(targetCount == 0u) targetCount = primitive.targets.size();
						else if(!primitive.targets.empty() && primitive.targets.size() != targetCount)
						{
							return ReportModelError(
								ModelDiagnosticCode::InvalidData,
								filepath,
								"glTF mesh primitives use inconsistent morph target counts",
								"meshes[" + std::to_string(source.meshIndex.value()) + "].primitives");
						}
					}
					const auto& sourceWeights = !source.weights.empty() ? source.weights : mesh.weights;
					if(!sourceWeights.empty() && sourceWeights.size() != targetCount)
					{
						return ReportModelError(
							ModelDiagnosticCode::InvalidData,
							filepath,
							"glTF morph default weight count does not match target count",
							"nodes[" + std::to_string(nodeIndex) + "].weights");
					}
					node.morphWeights.assign(targetCount, 0.0f);
					for(size_t weightIndex = 0; weightIndex < sourceWeights.size(); ++weightIndex)
					{
						const float weight = static_cast<float>(sourceWeights[weightIndex]);
						if(!std::isfinite(weight))
						{
							return ReportModelError(
								ModelDiagnosticCode::InvalidData,
								filepath,
								"glTF morph default weight is non-finite",
								"nodes[" + std::to_string(nodeIndex) + "].weights["
									+ std::to_string(weightIndex) + "]");
						}
						node.morphWeights[weightIndex] = weight;
					}
				}
				for(const size_t childIndex : source.children)
				{
					if(childIndex >= outModel.nodes.size() || outModel.nodes[childIndex].parentIndex != -1)
					{
						return ReportModelError(
							ModelDiagnosticCode::InvalidData,
							filepath,
							"glTF contains an invalid node hierarchy",
							"nodes[" + std::to_string(nodeIndex) + "].children");
					}
					outModel.nodes[childIndex].parentIndex = static_cast<int32_t>(nodeIndex);
				}
			}

			outModel.skins.reserve(gltf.skins.size());
			for(size_t skinIndex = 0; skinIndex < gltf.skins.size(); ++skinIndex)
			{
				const fastgltf::Skin& source = gltf.skins[skinIndex];
				ModelSkin skin;
				skin.name = source.name.c_str();
				skin.jointNodeIndices.reserve(source.joints.size());
				for(const size_t jointNodeIndex : source.joints)
				{
					if(jointNodeIndex >= outModel.nodes.size())
					{
						return ReportModelError(
							ModelDiagnosticCode::InvalidData,
							filepath,
							"glTF skin references an invalid joint node",
							"skins[" + std::to_string(skinIndex) + "].joints");
					}
					skin.jointNodeIndices.push_back(static_cast<uint32_t>(jointNodeIndex));
				}

				if(source.inverseBindMatrices.has_value())
				{
					const size_t accessorIndex = source.inverseBindMatrices.value();
					if(accessorIndex >= gltf.accessors.size()) return false;
					const fastgltf::Accessor& accessor = gltf.accessors[accessorIndex];
					if(accessor.count != source.joints.size())
					{
						return ReportModelError(
							ModelDiagnosticCode::InvalidData,
							filepath,
							"glTF inverse-bind count does not match joint count",
							"skins[" + std::to_string(skinIndex) + "].inverseBindMatrices");
					}
					skin.inverseBindMatrices.reserve(accessor.count);
					fastgltf::iterateAccessor<fastgltf::math::fmat4x4>(gltf, accessor, [&](const fastgltf::math::fmat4x4& matrix) {
						skin.inverseBindMatrices.push_back(ToFloat4x4(matrix));
					});
					if(std::any_of(skin.inverseBindMatrices.begin(), skin.inverseBindMatrices.end(), [](const Math::float4x4& matrix) {
						return !IsFinite(matrix);
					}))
					{
						return ReportModelError(
							ModelDiagnosticCode::InvalidData,
							filepath,
							"glTF skin contains non-finite inverse-bind matrices",
							"skins[" + std::to_string(skinIndex) + "].inverseBindMatrices");
					}
				}
				else
				{
					skin.inverseBindMatrices.assign(source.joints.size(), Math::float4x4::Identity());
				}
				outModel.skins.push_back(std::move(skin));
			}

			outModel.animations.reserve(gltf.animations.size());
			for(size_t animationIndex = 0; animationIndex < gltf.animations.size(); ++animationIndex)
			{
				const fastgltf::Animation& source = gltf.animations[animationIndex];
				AnimationClip clip;
				clip.name = source.name.empty() ? "Animation_" + std::to_string(animationIndex) : source.name.c_str();
				std::map<uint32_t, size_t> trackByNode;

				for(size_t channelIndex = 0; channelIndex < source.channels.size(); ++channelIndex)
				{
					const fastgltf::AnimationChannel& channel = source.channels[channelIndex];
					if(channel.path == fastgltf::AnimationPath::Weights)
					{
						if(!channel.nodeIndex.has_value()
							|| channel.nodeIndex.value() >= outModel.nodes.size()
							|| channel.samplerIndex >= source.samplers.size())
						{
							return ReportModelError(
								ModelDiagnosticCode::InvalidData,
								filepath,
								"glTF morph animation channel is invalid",
								"animations[" + std::to_string(animationIndex)
									+ "].channels[" + std::to_string(channelIndex) + "]");
						}
						const uint32_t nodeIndex = static_cast<uint32_t>(channel.nodeIndex.value());
						const size_t targetCount = outModel.nodes[nodeIndex].morphWeights.size();
						if(targetCount == 0u)
						{
							return ReportModelError(
								ModelDiagnosticCode::InvalidData,
								filepath,
								"glTF morph animation targets a node without morph targets",
								"animations[" + std::to_string(animationIndex)
									+ "].channels[" + std::to_string(channelIndex) + "]");
						}
						const fastgltf::AnimationSampler& sampler = source.samplers[channel.samplerIndex];
						if(sampler.inputAccessor >= gltf.accessors.size()
							|| sampler.outputAccessor >= gltf.accessors.size()) return false;
						const fastgltf::Accessor& timeAccessor = gltf.accessors[sampler.inputAccessor];
						const fastgltf::Accessor& outputAccessor = gltf.accessors[sampler.outputAccessor];
						std::vector<float> times;
						times.reserve(timeAccessor.count);
						fastgltf::iterateAccessor<float>(gltf, timeAccessor, [&](float time) { times.push_back(time); });
						if(!HasStrictFiniteTimes(times))
						{
							return ReportModelError(
								ModelDiagnosticCode::InvalidData,
								filepath,
								"glTF morph animation key times must be finite and strictly increasing",
								"animations[" + std::to_string(animationIndex)
									+ "].samplers[" + std::to_string(channel.samplerIndex) + "]");
						}
						const bool cubic = sampler.interpolation == fastgltf::AnimationInterpolation::CubicSpline;
						const uint64_t valuesPerKey = static_cast<uint64_t>(targetCount) * (cubic ? 3u : 1u);
						if(times.size() > std::numeric_limits<uint64_t>::max() / valuesPerKey
							|| outputAccessor.count != static_cast<uint64_t>(times.size()) * valuesPerKey)
						{
							return ReportModelError(
								ModelDiagnosticCode::InvalidData,
								filepath,
								"glTF morph animation input/output counts do not match",
								"animations[" + std::to_string(animationIndex)
									+ "].samplers[" + std::to_string(channel.samplerIndex) + "]");
						}
						std::vector<float> values;
						values.reserve(outputAccessor.count);
						fastgltf::iterateAccessor<float>(gltf, outputAccessor, [&](float value) { values.push_back(value); });
						if(std::any_of(values.begin(), values.end(), [](float value) { return !std::isfinite(value); }))
						{
							return ReportModelError(
								ModelDiagnosticCode::InvalidData,
								filepath,
								"glTF morph animation contains non-finite weights",
								"animations[" + std::to_string(animationIndex)
									+ "].samplers[" + std::to_string(channel.samplerIndex) + "].output");
						}

						const AnimationInterpolation interpolation = ToAnimationInterpolation(sampler.interpolation);
						for(size_t targetIndex = 0; targetIndex < targetCount; ++targetIndex)
						{
							if(std::any_of(clip.morphTracks.begin(), clip.morphTracks.end(), [&](const MorphWeightTrack& track) {
								return track.nodeIndex == nodeIndex && track.targetIndex == targetIndex;
							})) return false;
							MorphWeightTrack track;
							track.nodeIndex = nodeIndex;
							track.targetIndex = static_cast<uint32_t>(targetIndex);
							track.interpolation = interpolation;
							track.weights.reserve(times.size());
							for(size_t keyIndex = 0; keyIndex < times.size(); ++keyIndex)
							{
								FloatKey key;
								key.time = times[keyIndex];
								const size_t keyBase = keyIndex * targetCount * (cubic ? 3u : 1u);
								if(cubic)
								{
									key.inTangent = values[keyBase + targetIndex];
									key.value = values[keyBase + targetCount + targetIndex];
									key.outTangent = values[keyBase + targetCount * 2u + targetIndex];
								}
								else
								{
									key.value = values[keyBase + targetIndex];
								}
								track.weights.push_back(key);
							}
							clip.morphTracks.push_back(std::move(track));
						}
						clip.duration = std::max(clip.duration, times.back());
						continue;
					}
					if(!channel.nodeIndex.has_value() || channel.nodeIndex.value() >= outModel.nodes.size()
						|| channel.samplerIndex >= source.samplers.size())
					{
						return ReportModelError(
							ModelDiagnosticCode::InvalidData,
							filepath,
							"glTF animation channel is invalid",
							"animations[" + std::to_string(animationIndex)
								+ "].channels[" + std::to_string(channelIndex) + "]");
					}

					const fastgltf::AnimationSampler& sampler = source.samplers[channel.samplerIndex];
					if(sampler.inputAccessor >= gltf.accessors.size() || sampler.outputAccessor >= gltf.accessors.size()) return false;
					const fastgltf::Accessor& timeAccessor = gltf.accessors[sampler.inputAccessor];
					const fastgltf::Accessor& outputAccessor = gltf.accessors[sampler.outputAccessor];
					std::vector<float> times;
					times.reserve(timeAccessor.count);
					fastgltf::iterateAccessor<float>(gltf, timeAccessor, [&](float time) { times.push_back(time); });
					if(!HasStrictFiniteTimes(times))
					{
						return ReportModelError(
							ModelDiagnosticCode::InvalidData,
							filepath,
							"glTF animation key times must be finite and strictly increasing");
					}
					const bool cubic = sampler.interpolation == fastgltf::AnimationInterpolation::CubicSpline;
					const size_t expectedOutputCount = times.size() * (cubic ? 3u : 1u);
					if(outputAccessor.count != expectedOutputCount)
					{
						return ReportModelError(
							ModelDiagnosticCode::InvalidData,
							filepath,
							"glTF animation input/output counts do not match",
							"animations[" + std::to_string(animationIndex)
								+ "].samplers[" + std::to_string(channel.samplerIndex) + "]");
					}

					const uint32_t nodeIndex = static_cast<uint32_t>(channel.nodeIndex.value());
					auto [trackIterator, inserted] = trackByNode.emplace(nodeIndex, clip.tracks.size());
					if(inserted)
					{
						NodeAnimationTrack track;
						track.nodeIndex = nodeIndex;
						clip.tracks.push_back(std::move(track));
					}
					NodeAnimationTrack& track = clip.tracks[trackIterator->second];
					const AnimationInterpolation interpolation = ToAnimationInterpolation(sampler.interpolation);

					if(channel.path == fastgltf::AnimationPath::Rotation)
					{
						if(!track.rotations.empty()) return false;
						std::vector<fastgltf::math::fvec4> values;
						values.reserve(outputAccessor.count);
						fastgltf::iterateAccessor<fastgltf::math::fvec4>(gltf, outputAccessor, [&](const fastgltf::math::fvec4& value) {
							values.push_back(value);
						});
						track.rotationInterpolation = interpolation;
						track.rotations.reserve(times.size());
						for(size_t keyIndex = 0; keyIndex < times.size(); ++keyIndex)
						{
							const size_t valueIndex = cubic ? keyIndex * 3u + 1u : keyIndex;
							QuatKey key;
							key.time = times[keyIndex];
							key.value = Math::quat(values[valueIndex].x(), values[valueIndex].y(), values[valueIndex].z(), values[valueIndex].w());
							if(cubic)
							{
								const auto& in = values[valueIndex - 1u];
								const auto& out = values[valueIndex + 1u];
								key.inTangent = Math::quat(in.x(), in.y(), in.z(), in.w());
								key.outTangent = Math::quat(out.x(), out.y(), out.z(), out.w());
							}
							track.rotations.push_back(key);
						}
						if(!HasStrictFiniteKeyTimes(track.rotations)
							|| std::any_of(track.rotations.begin(), track.rotations.end(), [](const QuatKey& key) {
								return !IsFinite(key.value) || !IsFinite(key.inTangent) || !IsFinite(key.outTangent);
							}))
						{
							return ReportModelError(
								ModelDiagnosticCode::InvalidData,
								filepath,
								"glTF rotation track contains non-finite values");
						}
					}
					else
					{
						std::vector<fastgltf::math::fvec3> values;
						values.reserve(outputAccessor.count);
						fastgltf::iterateAccessor<fastgltf::math::fvec3>(gltf, outputAccessor, [&](const fastgltf::math::fvec3& value) {
							values.push_back(value);
						});
						std::vector<Vec3Key>* destination = channel.path == fastgltf::AnimationPath::Translation
							? &track.translations : &track.scales;
						AnimationInterpolation* destinationInterpolation = channel.path == fastgltf::AnimationPath::Translation
							? &track.translationInterpolation : &track.scaleInterpolation;
						if(!destination->empty()) return false;
						*destinationInterpolation = interpolation;
						destination->reserve(times.size());
						for(size_t keyIndex = 0; keyIndex < times.size(); ++keyIndex)
						{
							const size_t valueIndex = cubic ? keyIndex * 3u + 1u : keyIndex;
							Vec3Key key;
							key.time = times[keyIndex];
							key.value = Math::float3(values[valueIndex].x(), values[valueIndex].y(), values[valueIndex].z());
							if(cubic)
							{
								const auto& in = values[valueIndex - 1u];
								const auto& out = values[valueIndex + 1u];
								key.inTangent = Math::float3(in.x(), in.y(), in.z());
								key.outTangent = Math::float3(out.x(), out.y(), out.z());
							}
							destination->push_back(key);
						}
						if(!HasStrictFiniteKeyTimes(*destination)
							|| std::any_of(destination->begin(), destination->end(), [](const Vec3Key& key) {
								return !IsFinite(key.value) || !IsFinite(key.inTangent) || !IsFinite(key.outTangent);
							}))
						{
							return ReportModelError(
								ModelDiagnosticCode::InvalidData,
								filepath,
								"glTF vector track contains non-finite values");
						}
					}
					clip.duration = std::max(clip.duration, times.back());
				}

				if(!clip.tracks.empty() || !clip.morphTracks.empty()) outModel.animations.push_back(std::move(clip));
			}
			const bool hasMorphTargets = std::any_of(gltf.meshes.begin(), gltf.meshes.end(), [](const fastgltf::Mesh& mesh) {
				return std::any_of(mesh.primitives.begin(), mesh.primitives.end(), [](const fastgltf::Primitive& primitive) {
					return !primitive.targets.empty();
				});
			});
			const bool animatedModel = !outModel.skins.empty() || !outModel.animations.empty() || hasMorphTargets;

			std::function<bool(size_t, fastgltf::math::mat<float, 4, 4>)> processNode =
				[&](size_t nodeIndex, fastgltf::math::mat<float, 4, 4> parentMatrix)
			{
				if(nodeIndex >= gltf.nodes.size()) return false;
				const fastgltf::Node& node = gltf.nodes[nodeIndex];
				const fastgltf::math::mat<float, 4, 4> globalMatrix = parentMatrix * fastgltf::getTransformMatrix(node);

				if(node.meshIndex.has_value())
				{
					if(node.meshIndex.value() >= gltf.meshes.size()) return false;
					const size_t sourceMeshIndex = node.meshIndex.value();
					const fastgltf::Mesh& sourceMesh = gltf.meshes[sourceMeshIndex];
					for(size_t primitiveIndex = 0; primitiveIndex < sourceMesh.primitives.size(); ++primitiveIndex)
					{
						const fastgltf::Primitive& primitive = sourceMesh.primitives[primitiveIndex];
						if(primitive.type != fastgltf::PrimitiveType::Triangles)
						{
							return ReportModelError(
								ModelDiagnosticCode::InvalidData,
								filepath,
								"glTF primitive topology is not TRIANGLES");
						}
						auto* posAttr = primitive.findAttribute("POSITION");
						if(posAttr == primitive.attributes.end()) continue;

						if(posAttr->accessorIndex >= gltf.accessors.size()) return false;
						const fastgltf::Accessor& posAccessor = gltf.accessors[posAttr->accessorIndex];
						ModelMesh mesh = {};
						mesh.name = sourceMesh.name.c_str();
						if(primitive.materialIndex.has_value()
							&& primitive.materialIndex.value() >= gltf.materials.size()) return false;
						mesh.materialIndex = primitive.materialIndex.has_value()
							? static_cast<uint32_t>(primitive.materialIndex.value())
							: defaultMaterialIndex;
						if(mesh.materialIndex >= outModel.materials.size()) return false;
						if(animatedModel)
						{
							mesh.nodeIndex = static_cast<uint32_t>(nodeIndex);
							if(node.skinIndex.has_value())
							{
								if(node.skinIndex.value() >= outModel.skins.size()) return false;
								mesh.skinIndex = static_cast<uint32_t>(node.skinIndex.value());
							}
						}
						mesh.defaultMorphWeights = outModel.nodes[nodeIndex].morphWeights;
						if(mesh.defaultMorphWeights.size() != primitive.targets.size())
						{
							return ReportModelError(
								ModelDiagnosticCode::InvalidData,
								filepath,
								"glTF morph default weight count does not match primitive target count",
								"meshes[" + std::to_string(sourceMeshIndex)
									+ "].primitives[" + std::to_string(primitiveIndex) + "].targets");
						}
						mesh.mesh.vertices.resize(posAccessor.count);
						const fastgltf::math::fmat4x4 vertexMatrix = animatedModel
							? fastgltf::math::fmat4x4(1.0f)
							: globalMatrix;

						bool invalidPosition = false;
						fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(gltf, posAccessor, [&](fastgltf::math::fvec3 pos, size_t idx) {
							if(!std::isfinite(pos.x()) || !std::isfinite(pos.y()) || !std::isfinite(pos.z()))
							{
								invalidPosition = true;
								return;
							}
							const fastgltf::math::fvec4 p(pos.x(), pos.y(), pos.z(), 1.0f);
							const fastgltf::math::fvec4 transformedPos = vertexMatrix * p;
							if(!std::isfinite(transformedPos.x())
								|| !std::isfinite(transformedPos.y())
								|| !std::isfinite(transformedPos.z()))
							{
								invalidPosition = true;
								return;
							}
							Vertex& vertex = mesh.mesh.vertices[idx];
							vertex.position = Math::float3(transformedPos.x(), transformedPos.y(), transformedPos.z());
							vertex.uv = Math::float2(0.0f, 0.0f);
							vertex.normal = Math::float3(0.0f, 1.0f, 0.0f);
						});
						if(invalidPosition)
						{
							return ReportModelError(
								ModelDiagnosticCode::InvalidData,
								filepath,
								"glTF POSITION attribute contains non-finite values",
								"meshes[" + std::to_string(sourceMeshIndex)
									+ "].primitives[" + std::to_string(primitiveIndex)
									+ "].attributes.POSITION");
						}

						const auto* normalAttr = primitive.findAttribute("NORMAL");
						const bool hasNormalAttribute = normalAttr != primitive.attributes.end();
						if(hasNormalAttribute)
						{
							if(normalAttr->accessorIndex >= gltf.accessors.size()) return false;
							const fastgltf::Accessor& normalAcc = gltf.accessors[normalAttr->accessorIndex];
							if(normalAcc.count != posAccessor.count) return false;
							Math::float4x4 normalMatrix = {};
							if(!Math::InverseTranspose(ToFloat4x4(vertexMatrix), normalMatrix))
							{
								return ReportModelError(
									ModelDiagnosticCode::InvalidData,
									filepath,
									"glTF mesh transform is singular; normals cannot be transformed");
							}
							bool invalidNormal = false;
							fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(gltf, normalAcc, [&](fastgltf::math::fvec3 normal, size_t idx) {
								if(!std::isfinite(normal.x())
									|| !std::isfinite(normal.y())
									|| !std::isfinite(normal.z()))
								{
									invalidNormal = true;
									return;
								}
								const Math::float3 transformedNormal = Math::TransformVector(
									normalMatrix,
									Math::float3(normal.x(), normal.y(), normal.z()));
								if(!IsFinite(transformedNormal))
								{
									invalidNormal = true;
									return;
								}
								mesh.mesh.vertices[idx].normal = NormalizeOr(
									transformedNormal,
									Math::float3(0.0f, 1.0f, 0.0f));
							});
							if(invalidNormal)
							{
								return ReportModelError(
									ModelDiagnosticCode::InvalidData,
									filepath,
									"glTF NORMAL attribute contains non-finite values",
									"meshes[" + std::to_string(sourceMeshIndex)
										+ "].primitives[" + std::to_string(primitiveIndex)
										+ "].attributes.NORMAL");
							}
						}

						bool hasAuthoredTangents = false;
						if(auto* tangentAttr = primitive.findAttribute("TANGENT"); tangentAttr != primitive.attributes.end())
						{
							if(tangentAttr->accessorIndex >= gltf.accessors.size()) return false;
							const fastgltf::Accessor& tangentAccessor = gltf.accessors[tangentAttr->accessorIndex];
							if(tangentAccessor.count != posAccessor.count) return false;
							const Math::float4x4 tangentMatrix = ToFloat4x4(vertexMatrix);
							const float tangentHandednessSign =
								Math::Determinant3x3(tangentMatrix) < 0.0f ? -1.0f : 1.0f;
							bool invalidTangent = false;
							fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(gltf, tangentAccessor, [&](const fastgltf::math::fvec4& value, size_t idx) {
								if(!std::isfinite(value.x()) || !std::isfinite(value.y())
									|| !std::isfinite(value.z()) || !std::isfinite(value.w()))
								{
									invalidTangent = true;
									return;
								}
								const Math::float3 transformed = Math::TransformVector(
									tangentMatrix,
									Math::float3(value.x(), value.y(), value.z()));
								const Math::float3& normal = mesh.mesh.vertices[idx].normal;
								const Math::float3 orthogonal = transformed - normal * Dot(normal, transformed);
								const Math::float3 tangent = NormalizeOr(orthogonal, BuildFallbackTangent(normal));
								if(!IsFinite(tangent))
								{
									invalidTangent = true;
									return;
								}
								mesh.mesh.vertices[idx].tangent = Math::float4(
									tangent.x, tangent.y, tangent.z, value.w() * tangentHandednessSign);
							});
							if(invalidTangent)
							{
								return ReportModelError(
									ModelDiagnosticCode::InvalidData,
									filepath,
									"glTF TANGENT attribute contains non-finite values",
									"meshes[" + std::to_string(sourceMeshIndex)
										+ "].primitives[" + std::to_string(primitiveIndex)
										+ "].attributes.TANGENT");
							}
							hasAuthoredTangents = true;
						}

						if(auto* uvAttr = primitive.findAttribute("TEXCOORD_0"); uvAttr != primitive.attributes.end())
						{
							if(uvAttr->accessorIndex >= gltf.accessors.size()) return false;
							const fastgltf::Accessor& uvAcc = gltf.accessors[uvAttr->accessorIndex];
							if(uvAcc.count != posAccessor.count) return false;
							bool invalidUv = false;
							fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(gltf, uvAcc, [&](fastgltf::math::fvec2 uv, size_t idx) {
								if(!std::isfinite(uv.x()) || !std::isfinite(uv.y()))
								{
									invalidUv = true;
									return;
								}
								mesh.mesh.vertices[idx].uv = Math::float2(uv.x(), options.flipV ? 1.0f - uv.y() : uv.y());
							});
							if(invalidUv)
							{
								return ReportModelError(
									ModelDiagnosticCode::InvalidData,
									filepath,
									"glTF TEXCOORD_0 attribute contains non-finite values",
									"meshes[" + std::to_string(sourceMeshIndex)
										+ "].primitives[" + std::to_string(primitiveIndex)
										+ "].attributes.TEXCOORD_0");
							}
						}

						std::vector<std::vector<std::pair<uint32_t, float>>> vertexInfluences(posAccessor.count);
						bool hasInfluenceAttributes = false;
						for(uint32_t setIndex = 0; setIndex < 8u; ++setIndex)
						{
							const std::string jointName = "JOINTS_" + std::to_string(setIndex);
							const std::string weightName = "WEIGHTS_" + std::to_string(setIndex);
							const auto* jointAttribute = primitive.findAttribute(jointName);
							const auto* weightAttribute = primitive.findAttribute(weightName);
							const bool hasJoints = jointAttribute != primitive.attributes.end();
							const bool hasWeights = weightAttribute != primitive.attributes.end();
							if(hasJoints != hasWeights)
							{
								return ReportModelError(
									ModelDiagnosticCode::InvalidData,
									filepath,
									"glTF JOINTS/WEIGHTS attribute pair is incomplete",
									"meshes[" + std::to_string(sourceMeshIndex)
										+ "].primitives[" + std::to_string(primitiveIndex)
										+ "].attributes");
							}
							if(!hasJoints) continue;
							hasInfluenceAttributes = true;
							if(mesh.skinIndex == kInvalidAnimationIndex)
							{
								return ReportModelError(
									ModelDiagnosticCode::InvalidData,
									filepath,
									"glTF mesh has influences but no skin",
									"nodes[" + std::to_string(nodeIndex) + "].skin");
							}

							if(jointAttribute->accessorIndex >= gltf.accessors.size()
								|| weightAttribute->accessorIndex >= gltf.accessors.size()) return false;
							const fastgltf::Accessor& jointAccessor = gltf.accessors[jointAttribute->accessorIndex];
							const fastgltf::Accessor& weightAccessor = gltf.accessors[weightAttribute->accessorIndex];
							if(jointAccessor.count != posAccessor.count || weightAccessor.count != posAccessor.count)
							{
								return ReportModelError(
									ModelDiagnosticCode::InvalidData,
									filepath,
									"glTF influence count does not match vertex count",
									"meshes[" + std::to_string(sourceMeshIndex)
										+ "].primitives[" + std::to_string(primitiveIndex)
										+ "].attributes." + weightName);
							}

							std::vector<fastgltf::math::u32vec4> joints(posAccessor.count);
							fastgltf::iterateAccessorWithIndex<fastgltf::math::u32vec4>(gltf, jointAccessor, [&](const fastgltf::math::u32vec4& value, size_t index) {
								joints[index] = value;
							});
							const ModelSkin& skin = outModel.skins[mesh.skinIndex];
							bool invalidJoint = false;
							bool invalidWeight = false;
							fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(gltf, weightAccessor, [&](const fastgltf::math::fvec4& value, size_t index) {
								for(size_t component = 0; component < 4; ++component)
								{
									if(!std::isfinite(value[component]) || value[component] < 0.0f)
									{
										invalidWeight = true;
										continue;
									}
									if(value[component] == 0.0f) continue;
									if(joints[index][component] >= skin.jointNodeIndices.size()) invalidJoint = true;
									else vertexInfluences[index].emplace_back(joints[index][component], value[component]);
								}
							});
							if(invalidWeight)
							{
								return ReportModelError(
									ModelDiagnosticCode::InvalidData,
									filepath,
									"glTF vertex weights must be finite and non-negative",
									"meshes[" + std::to_string(sourceMeshIndex)
										+ "].primitives[" + std::to_string(primitiveIndex)
										+ "].attributes." + weightName);
							}
							if(invalidJoint)
							{
								return ReportModelError(
									ModelDiagnosticCode::InvalidData,
									filepath,
									"glTF vertex references a joint outside the skin",
									"meshes[" + std::to_string(sourceMeshIndex)
										+ "].primitives[" + std::to_string(primitiveIndex)
										+ "].attributes." + jointName);
							}
						}

						if(mesh.skinIndex != kInvalidAnimationIndex && !hasInfluenceAttributes)
						{
							return ReportModelError(
								ModelDiagnosticCode::InvalidData,
								filepath,
								"glTF skinned mesh has no JOINTS/WEIGHTS attributes",
								"meshes[" + std::to_string(sourceMeshIndex)
									+ "].primitives[" + std::to_string(primitiveIndex)
									+ "].attributes");
						}
						if(hasInfluenceAttributes)
						{
							mesh.mesh.skinInfluences.reserve(posAccessor.count);
							bool anyTruncated = false;
							for(auto& values : vertexInfluences)
							{
								bool truncated = false;
								mesh.mesh.skinInfluences.push_back(MakeSkinInfluence(std::move(values), truncated));
								anyTruncated = anyTruncated || truncated;
							}
							if(anyTruncated && !warnedInfluenceTruncation)
							{
								ReportModelWarning(
									ModelDiagnosticCode::DataLoss,
									filepath,
									"glTF vertex influences were truncated to the four strongest joints");
								warnedInfluenceTruncation = true;
							}
						}

						mesh.morphTargets.reserve(primitive.targets.size());
						const Math::float4x4 morphPositionMatrix = ToFloat4x4(vertexMatrix);
						Math::float4x4 morphNormalMatrix = {};
						if(!primitive.targets.empty()
							&& !Math::InverseTranspose(morphPositionMatrix, morphNormalMatrix))
						{
							return ReportModelError(
								ModelDiagnosticCode::InvalidData,
								filepath,
								"glTF mesh transform is singular; morph deltas cannot be transformed",
								"meshes[" + std::to_string(sourceMeshIndex)
									+ "].primitives[" + std::to_string(primitiveIndex) + "].targets");
						}
						for(size_t targetIndex = 0; targetIndex < primitive.targets.size(); ++targetIndex)
						{
							MorphTarget target;
							target.name = "Target_" + std::to_string(targetIndex);
							auto loadDeltas = [&](const char* semantic,
								const Math::float4x4& transform,
								std::vector<Math::float3>& destination) -> bool {
								const auto* attribute = primitive.findTargetAttribute(targetIndex, semantic);
								if(attribute == primitive.targets[targetIndex].end()) return true;
								if(attribute->accessorIndex >= gltf.accessors.size()) return false;
								const fastgltf::Accessor& accessor = gltf.accessors[attribute->accessorIndex];
								if(accessor.count != posAccessor.count) return false;
								destination.resize(posAccessor.count);
								bool invalidDelta = false;
								fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
									gltf,
									accessor,
									[&](const fastgltf::math::fvec3& value, size_t deltaIndex) {
										const Math::float3 sourceDelta(value.x(), value.y(), value.z());
										const Math::float3 transformedDelta =
											Math::TransformVector(transform, sourceDelta);
										if(!IsFinite(sourceDelta) || !IsFinite(transformedDelta))
										{
											invalidDelta = true;
											return;
										}
										destination[deltaIndex] = transformedDelta;
									});
								if(!invalidDelta) return true;
								return ReportModelError(
									ModelDiagnosticCode::InvalidData,
									filepath,
									"glTF morph target contains non-finite deltas",
									"meshes[" + std::to_string(sourceMeshIndex)
										+ "].primitives[" + std::to_string(primitiveIndex)
										+ "].targets[" + std::to_string(targetIndex) + "]." + semantic);
							};
							if(!loadDeltas("POSITION", morphPositionMatrix, target.positionDeltas)
								|| !loadDeltas("NORMAL", morphNormalMatrix, target.normalDeltas)
								|| !loadDeltas("TANGENT", morphPositionMatrix, target.tangentDeltas)) return false;
							if(target.positionDeltas.empty() && target.normalDeltas.empty() && target.tangentDeltas.empty())
							{
								return ReportModelError(
									ModelDiagnosticCode::InvalidData,
									filepath,
									"glTF morph target contains no supported deltas",
									"meshes[" + std::to_string(sourceMeshIndex)
										+ "].primitives[" + std::to_string(primitiveIndex)
										+ "].targets[" + std::to_string(targetIndex) + "]");
							}
							mesh.morphTargets.push_back(std::move(target));
						}

						if(primitive.indicesAccessor.has_value())
						{
							if(primitive.indicesAccessor.value() >= gltf.accessors.size()) return false;
							const fastgltf::Accessor& idxAccessor = gltf.accessors[primitive.indicesAccessor.value()];
							fastgltf::iterateAccessor<std::uint32_t>(gltf, idxAccessor, [&](std::uint32_t idx) {
								mesh.mesh.indices.push_back(idx);
							});
						}
						else
						{
							for(size_t i = 0; i < posAccessor.count; ++i) mesh.mesh.indices.push_back(static_cast<uint32_t>(i));
						}
						if(mesh.mesh.indices.empty() || (mesh.mesh.indices.size() % 3u) != 0u)
						{
							return ReportModelError(
								ModelDiagnosticCode::InvalidData,
								filepath,
								"glTF TRIANGLES index count must be a non-zero multiple of three");
						}
						if(std::any_of(mesh.mesh.indices.begin(), mesh.mesh.indices.end(), [&](uint32_t index) {
							return index >= mesh.mesh.vertices.size();
						}))
						{
							return ReportModelError(
								ModelDiagnosticCode::InvalidData,
								filepath,
								"glTF index references a vertex outside the primitive",
								"meshes[" + std::to_string(sourceMeshIndex)
									+ "].primitives[" + std::to_string(primitiveIndex) + "].indices");
						}

						if(!hasAuthoredTangents)
							CalculateTangents(mesh.mesh, !hasNormalAttribute);
						outModel.meshes.push_back(std::move(mesh));
					}
				}

				for(const size_t childIndex : node.children)
					if(!processNode(childIndex, globalMatrix)) return false;
				return true;
			};

			if(gltf.scenes.empty()) return false;
			if(gltf.defaultScene.has_value() && gltf.defaultScene.value() >= gltf.scenes.size()) return false;
			const fastgltf::Scene* scene = gltf.defaultScene.has_value() ? &gltf.scenes[*gltf.defaultScene] : &gltf.scenes[0];
			fastgltf::math::mat<float, 4, 4> rotationMatrix(1.0f);
			rotationMatrix.col(0).x() = -1.0f;
			rotationMatrix.col(2).z() = -1.0f;
			if(animatedModel) outModel.assetTransform = ToFloat4x4(rotationMatrix);
			const fastgltf::math::fmat4x4 initialMatrix = animatedModel
				? fastgltf::math::fmat4x4(1.0f)
				: rotationMatrix;
			for(const size_t nodeIndex : scene->nodeIndices)
				if(!processNode(nodeIndex, initialMatrix)) return false;
			return !outModel.meshes.empty();
		}

}
