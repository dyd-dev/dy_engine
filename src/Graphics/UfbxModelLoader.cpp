#include "Graphics/ModelLoaderInternal.h"
#include "Graphics/ImageFile.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include <ufbx.h>

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
		struct UfbxAllocatorBudget
		{
			size_t limit = 0u;
			size_t used = 0u;
			bool limitExceeded = false;
		};

		void* UfbxBudgetAlloc(void* user, size_t size)
		{
			auto* budget = static_cast<UfbxAllocatorBudget*>(user);
			if(budget == nullptr || size > budget->limit - std::min(budget->used, budget->limit))
			{
				if(budget != nullptr) budget->limitExceeded = true;
				return nullptr;
			}
			void* allocation = std::malloc(size);
			if(allocation != nullptr) budget->used += size;
			return allocation;
		}

		void* UfbxBudgetRealloc(void* user, void* oldPointer, size_t oldSize, size_t newSize)
		{
			auto* budget = static_cast<UfbxAllocatorBudget*>(user);
			if(budget == nullptr) return nullptr;
			if(newSize > oldSize)
			{
				const size_t additionalBytes = newSize - oldSize;
				if(additionalBytes > budget->limit - std::min(budget->used, budget->limit))
				{
					budget->limitExceeded = true;
					return nullptr;
				}
			}
			if(newSize == 0u)
			{
				std::free(oldPointer);
				budget->used -= std::min(oldSize, budget->used);
				return nullptr;
			}
			void* allocation = std::realloc(oldPointer, newSize);
			if(allocation == nullptr) return nullptr;
			if(newSize >= oldSize) budget->used += newSize - oldSize;
			else budget->used -= std::min(oldSize - newSize, budget->used);
			return allocation;
		}

		void UfbxBudgetFree(void* user, void* pointer, size_t size)
		{
			auto* budget = static_cast<UfbxAllocatorBudget*>(user);
			std::free(pointer);
			if(budget != nullptr) budget->used -= std::min(size, budget->used);
		}
		[[nodiscard]] uint32_t EnsureDefaultMaterial(ModelData& model)
		{
			const uint32_t defaultMaterialIndex = static_cast<uint32_t>(model.materials.size());
			model.materials.push_back({});
			return defaultMaterialIndex;
		}
		[[nodiscard]] Math::float4x4 ToFloat4x4(const ufbx_matrix& source)
		{
			Math::float4x4 result = Math::float4x4::Identity();
			for(size_t column = 0; column < 4; ++column)
				for(size_t row = 0; row < 3; ++row)
					result.m[column * 4 + row] = static_cast<float>(source.cols[column].v[row]);
			return result;
		}

		[[nodiscard]] NodeTransform ToNodeTransform(const ufbx_transform& source)
		{
			NodeTransform result;
			result.translation = Math::float3(
				static_cast<float>(source.translation.x),
				static_cast<float>(source.translation.y),
				static_cast<float>(source.translation.z));
			result.rotation = Math::quat(
				static_cast<float>(source.rotation.x),
				static_cast<float>(source.rotation.y),
				static_cast<float>(source.rotation.z),
				static_cast<float>(source.rotation.w));
			result.scale = Math::float3(
				static_cast<float>(source.scale.x),
				static_cast<float>(source.scale.y),
				static_cast<float>(source.scale.z));
			return result;
		}
		[[nodiscard]] SkinningMethod ToSkinningMethod(ufbx_skinning_method method)
		{
			switch(method)
			{
			case UFBX_SKINNING_METHOD_RIGID: return SkinningMethod::Rigid;
			case UFBX_SKINNING_METHOD_DUAL_QUATERNION: return SkinningMethod::DualQuaternion;
			case UFBX_SKINNING_METHOD_BLENDED_DQ_LINEAR: return SkinningMethod::BlendedDualQuaternion;
			default: return SkinningMethod::Linear;
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
		[[nodiscard]] bool IsFinite(const Math::float2& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y);
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
		[[nodiscard]] bool MatricesNearlyEqual(
			const ufbx_matrix& lhs,
			const ufbx_matrix& rhs,
			float tolerance = 1.0e-5f)
		{
			const Math::float4x4 left = ToFloat4x4(lhs);
			const Math::float4x4 right = ToFloat4x4(rhs);
			for(size_t component = 0u; component < 16u; ++component)
				if(std::fabs(left.m[component] - right.m[component]) > tolerance) return false;
			return true;
		}

		[[nodiscard]] bool AreCompatibleSkinDeformers(
			const ufbx_skin_deformer& lhs,
			const ufbx_skin_deformer& rhs)
		{
			if(lhs.skinning_method != rhs.skinning_method
				|| lhs.clusters.count != rhs.clusters.count
				|| lhs.vertices.count != rhs.vertices.count) return false;
			for(size_t clusterIndex = 0u; clusterIndex < lhs.clusters.count; ++clusterIndex)
			{
				const ufbx_skin_cluster* leftCluster = lhs.clusters.data[clusterIndex];
				const ufbx_skin_cluster* rightCluster = rhs.clusters.data[clusterIndex];
				if(leftCluster == nullptr || rightCluster == nullptr
					|| leftCluster->bone_node != rightCluster->bone_node
					|| !MatricesNearlyEqual(leftCluster->geometry_to_bone, rightCluster->geometry_to_bone))
					return false;
			}
			for(size_t vertexIndex = 0u; vertexIndex < lhs.vertices.count; ++vertexIndex)
			{
				const double difference = std::fabs(
					lhs.vertices.data[vertexIndex].dq_weight
					- rhs.vertices.data[vertexIndex].dq_weight);
				if(!std::isfinite(difference) || difference > 1.0e-5) return false;
			}
			return true;
		}
		[[nodiscard]] bool ValidateFbxLoadLimits(
			const std::string& filepath,
			const ufbx_scene& scene,
			const ModelLoadOptions& options,
			double bakeRate)
		{
			ModelLoadBudget budget(filepath, options);
			if(!budget.CheckNodes(scene.nodes.count)
				|| !budget.AddBytes(scene.nodes.count, sizeof(ModelNode), "nodes")
				|| !budget.AddBytes(scene.materials.count, sizeof(ModelMaterialInfo), "materials")) return false;

			for(size_t skinIndex = 0; skinIndex < scene.skin_deformers.count; ++skinIndex)
			{
				const uint64_t jointCount = scene.skin_deformers.data[skinIndex]->clusters.count;
				if(!budget.CheckJoints(jointCount)
					|| !budget.AddBytes(jointCount, sizeof(uint32_t) + sizeof(Math::float4x4), "skin joints"))
					return false;
			}

			uint64_t morphTracksPerSample = 0u;
			for(size_t nodeIndex = 0; nodeIndex < scene.nodes.count; ++nodeIndex)
			{
				const ufbx_mesh* mesh = scene.nodes.data[nodeIndex]->mesh;
				if(mesh == nullptr) continue;
				for(size_t deformerIndex = 0; deformerIndex < mesh->blend_deformers.count; ++deformerIndex)
				{
					const ufbx_blend_deformer* deformer = mesh->blend_deformers.data[deformerIndex];
					for(size_t channelIndex = 0; channelIndex < deformer->channels.count; ++channelIndex)
					{
						const uint64_t keyframeCount = deformer->channels.data[channelIndex]->keyframes.count;
						if(keyframeCount > std::numeric_limits<uint64_t>::max() - morphTracksPerSample) return false;
						morphTracksPerSample += keyframeCount;
					}
				}
			}

			for(size_t stackIndex = 0; stackIndex < scene.anim_stacks.count; ++stackIndex)
			{
				const ufbx_anim_stack* stack = scene.anim_stacks.data[stackIndex];
				if(stack->anim == nullptr) continue;
				const double duration = std::max(0.0, stack->time_end - stack->time_begin);
				const uint64_t sampleCount = duration > 0.0
					? static_cast<uint64_t>(std::ceil(duration * bakeRate)) + 1u : 1u;
				if(scene.nodes.count != 0u
					&& sampleCount > std::numeric_limits<uint64_t>::max() / scene.nodes.count)
					return false;
				const uint64_t nodeSamples = sampleCount * scene.nodes.count;
				if(nodeSamples > std::numeric_limits<uint64_t>::max() / 3u) return false;
				const uint64_t keyCount = nodeSamples * 3u;
				if(!budget.AddAnimationKeys(keyCount)
					|| !budget.AddBytes(keyCount, sizeof(QuatKey), "FBX baked animation keys")) return false;
				if(morphTracksPerSample != 0u)
				{
					if(sampleCount > std::numeric_limits<uint64_t>::max() / morphTracksPerSample) return false;
					const uint64_t morphKeyCount = sampleCount * morphTracksPerSample;
					if(!budget.AddAnimationKeys(morphKeyCount)
						|| !budget.AddBytes(morphKeyCount, sizeof(FloatKey), "FBX baked morph keys")) return false;
				}
			}

			for(size_t nodeIndex = 0; nodeIndex < scene.nodes.count; ++nodeIndex)
			{
				const ufbx_mesh* mesh = scene.nodes.data[nodeIndex]->mesh;
				if(mesh == nullptr) continue;
				uint64_t triangleVertexCount = 0u;
				for(size_t faceIndex = 0; faceIndex < mesh->faces.count; ++faceIndex)
				{
					const uint64_t corners = mesh->faces.data[faceIndex].num_indices;
					if(corners < 3u) continue;
					const uint64_t faceVertices = (corners - 2u) * 3u;
					if(faceVertices > std::numeric_limits<uint64_t>::max() - triangleVertexCount) return false;
					triangleVertexCount += faceVertices;
				}
				uint64_t stride = sizeof(Vertex) + sizeof(uint32_t);
				if(mesh->skin_deformers.count > 0) stride += sizeof(SkinInfluence);
				if(!budget.AddBytes(triangleVertexCount, stride, "FBX triangulated geometry")) return false;
				uint64_t morphTargetCount = 0u;
				for(size_t deformerIndex = 0; deformerIndex < mesh->blend_deformers.count; ++deformerIndex)
				{
					const ufbx_blend_deformer* deformer = mesh->blend_deformers.data[deformerIndex];
					for(size_t channelIndex = 0; channelIndex < deformer->channels.count; ++channelIndex)
					{
						const uint64_t keyframeCount = deformer->channels.data[channelIndex]->keyframes.count;
						if(keyframeCount > std::numeric_limits<uint64_t>::max() - morphTargetCount) return false;
						morphTargetCount += keyframeCount;
					}
				}
				if(morphTargetCount > options.maxMorphTargetsPerMesh)
				{
					return ReportModelError(
						ModelDiagnosticCode::ResourceLimitExceeded,
						filepath,
						"FBX morph target count exceeds maxMorphTargetsPerMesh",
						"nodes[" + std::to_string(nodeIndex) + "].mesh.morphTargets");
				}
				if(!budget.AddBytes(morphTargetCount, sizeof(MorphTarget) + sizeof(float), "FBX morph targets")) return false;
				if(morphTargetCount != 0u)
				{
					if(triangleVertexCount > std::numeric_limits<uint64_t>::max() / morphTargetCount) return false;
					if(!budget.AddBytes(
						triangleVertexCount * morphTargetCount,
						sizeof(Math::float3) * 2u,
						"FBX morph deltas")) return false;
				}
			}
			return true;
		}
		[[nodiscard]] bool NearlyEqual(const Math::float3& lhs, const Math::float3& rhs, float tolerance)
		{
			return std::fabs(lhs.x - rhs.x) <= tolerance
				&& std::fabs(lhs.y - rhs.y) <= tolerance
				&& std::fabs(lhs.z - rhs.z) <= tolerance;
		}

		[[nodiscard]] bool NearlyEqualRotation(const Math::quat& lhs, const Math::quat& rhs, float tolerance)
		{
			return 1.0f - std::fabs(Math::Dot(Math::Normalize(lhs), Math::Normalize(rhs))) <= tolerance;
		}

		template<typename Key, typename Equal>
		void CollapseConstantTrack(std::vector<Key>& keys, Equal equal)
		{
			if(keys.size() <= 1u) return;
			const auto& reference = keys.front().value;
			if(std::all_of(keys.begin() + 1, keys.end(), [&](const Key& key) { return equal(reference, key.value); }))
				keys.resize(1u);
		}

		struct UfbxMorphSource
		{
			const ufbx_blend_shape* shape = nullptr;
			std::string name;
		};

		struct UfbxBlendChannelBinding
		{
			const ufbx_blend_channel* channel = nullptr;
			uint32_t nodeIndex = kInvalidAnimationIndex;
			uint32_t firstTargetIndex = kInvalidAnimationIndex;
			uint32_t targetCount = 0u;
		};

		[[nodiscard]] std::vector<float> EvaluateUfbxBlendKeyframeWeights(
			const ufbx_blend_channel& channel,
			float channelWeight)
		{
			std::vector<float> weights(channel.keyframes.count, 0.0f);
			if(channel.keyframes.count == 0u || !std::isfinite(channelWeight)) return weights;
			auto targetWeight = [&](size_t keyframeIndex) {
				return static_cast<float>(channel.keyframes.data[keyframeIndex].target_weight * 0.01);
			};

			const float firstTargetWeight = targetWeight(0u);
			if(channel.keyframes.count == 1u || channelWeight <= firstTargetWeight)
			{
				weights[0] = std::fabs(firstTargetWeight) > 1.0e-8f
					? channelWeight / firstTargetWeight
					: 1.0f;
				return weights;
			}

			for(size_t keyframeIndex = 1u; keyframeIndex < channel.keyframes.count; ++keyframeIndex)
			{
				const float leftTargetWeight = targetWeight(keyframeIndex - 1u);
				const float rightTargetWeight = targetWeight(keyframeIndex);
				if(channelWeight > rightTargetWeight) continue;
				const float width = rightTargetWeight - leftTargetWeight;
				const float alpha = std::fabs(width) > 1.0e-8f
					? (channelWeight - leftTargetWeight) / width
					: 1.0f;
				weights[keyframeIndex - 1u] = 1.0f - alpha;
				weights[keyframeIndex] = alpha;
				return weights;
			}

			const float lastTargetWeight = targetWeight(channel.keyframes.count - 1u);
			weights.back() = std::fabs(lastTargetWeight) > 1.0e-8f
				? channelWeight / lastTargetWeight
				: 1.0f;
			return weights;
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
		struct UfbxExternalFileContext
		{
			std::filesystem::path modelDirectory;
			uint64_t sourceByteLimit = 0u;
			uint64_t sourceBytes = 0u;
			bool pathRejected = false;
			bool sourceLimitExceeded = false;
			std::string rejectedPath;
			std::set<std::filesystem::path> accountedPaths;
		};

		bool OpenRestrictedUfbxExternalFile(
			void* user,
			ufbx_stream* stream,
			const char* path,
			size_t pathLength,
			const ufbx_open_file_info* info)
		{
			auto* context = static_cast<UfbxExternalFileContext*>(user);
			if(context == nullptr || stream == nullptr || path == nullptr || info == nullptr) return false;
			if(info->type == UFBX_OPEN_FILE_MAIN_MODEL)
				return ufbx_default_open_file(nullptr, stream, path, pathLength, info);

			const size_t actualPathLength = pathLength == SIZE_MAX ? std::strlen(path) : pathLength;
			const std::string pathString(path, actualPathLength);
			const std::filesystem::path externalPath(pathString);
			std::error_code pathError;
			if(!IsPathInsideDirectory(context->modelDirectory, externalPath, pathError))
			{
				context->pathRejected = true;
				context->rejectedPath = pathString;
				return false;
			}

			const std::filesystem::path canonicalPath =
				std::filesystem::weakly_canonical(externalPath, pathError);
			if(pathError)
			{
				context->pathRejected = true;
				context->rejectedPath = pathString;
				return false;
			}
			const bool alreadyAccounted = context->accountedPaths.find(canonicalPath)
				!= context->accountedPaths.end();
			uint64_t externalBytes = 0u;
			if(!alreadyAccounted)
			{
				const uintmax_t fileBytes = std::filesystem::file_size(canonicalPath, pathError);
				if(pathError || fileBytes > std::numeric_limits<uint64_t>::max()) return false;
				externalBytes = static_cast<uint64_t>(fileBytes);
				if(context->sourceBytes > context->sourceByteLimit
					|| externalBytes > context->sourceByteLimit - context->sourceBytes)
				{
					context->sourceLimitExceeded = true;
					return false;
				}
			}

			if(!ufbx_open_file_ctx(stream, info->context, path, pathLength, nullptr, nullptr))
				return false;
			if(!alreadyAccounted)
			{
				context->sourceBytes += externalBytes;
				context->accountedPaths.insert(canonicalPath);
			}
			return true;
		}

		[[nodiscard]] std::string ToString(ufbx_string value)
		{
			return value.data != nullptr ? std::string(value.data, value.length) : std::string{};
		}

		[[nodiscard]] bool LoadUfbxTextureAssets(
			const std::string& filepath,
			const ufbx_scene& scene,
			const ModelLoadOptions& options,
			UfbxExternalFileContext& externalFileContext,
			ModelData& outModel,
			std::map<const ufbx_texture*, uint32_t>& textureIndices,
			uint64_t& decodedTextureBytes)
		{
			outModel.textures.clear();
			if(scene.textures.count > options.maxTextures)
			{
				return ReportModelError(
					ModelDiagnosticCode::ResourceLimitExceeded,
					filepath,
					"FBX texture count exceeds maxTextures",
					"textures");
			}
			outModel.textures.reserve(scene.textures.count);
			textureIndices.clear();
			decodedTextureBytes = 0u;

			for(size_t textureIndex = 0; textureIndex < scene.textures.count; ++textureIndex)
			{
				const ufbx_texture* source = scene.textures.data[textureIndex];
				TextureAsset texture = {};
				const std::string element = "textures[" + std::to_string(textureIndex) + "]";
				if(source == nullptr)
				{
					return ReportModelError(
						ModelDiagnosticCode::InvalidData,
						filepath,
						"ufbx texture entry is null",
						element);
				}

				if(source->content.data != nullptr && source->content.size > 0u)
				{
					const uint64_t remainingDecodedBytes = decodedTextureBytes <= options.maxDecodedBytes
						? options.maxDecodedBytes - decodedTextureBytes
						: 0u;
					bool limitExceeded = false;
					const ImageFile image = LoadImageMemory(
						static_cast<const uint8_t*>(source->content.data),
						source->content.size,
						remainingDecodedBytes,
						&limitExceeded);
					if(!image.IsValid())
					{
						return ReportModelError(
							limitExceeded
								? ModelDiagnosticCode::ResourceLimitExceeded
								: ModelDiagnosticCode::ParseFailed,
							filepath,
							limitExceeded
								? "embedded texture exceeds maxDecodedBytes"
								: "failed to decode embedded texture",
							element + ".content");
					}

					texture.width = image.GetWidth();
					texture.height = image.GetHeight();
					texture.rgba8 = image.GetPixels();
					const uint64_t imageBytes = static_cast<uint64_t>(texture.rgba8.size());
					if(imageBytes > options.maxDecodedBytes - decodedTextureBytes)
					{
						return ReportModelError(
							ModelDiagnosticCode::ResourceLimitExceeded,
							filepath,
							"embedded texture exceeds maxDecodedBytes",
							element + ".content");
					}
					decodedTextureBytes += imageBytes;
				}
				else
				{
					std::vector<std::filesystem::path> candidates;
					const std::string relativeFilename = ToString(source->relative_filename);
					if(!relativeFilename.empty())
					{
						const std::filesystem::path relativePath(relativeFilename);
						candidates.push_back(externalFileContext.modelDirectory / relativePath);
						const std::filesystem::path basename = relativePath.filename();
						if(!basename.empty())
						{
							candidates.push_back(externalFileContext.modelDirectory / basename);
							candidates.push_back(externalFileContext.modelDirectory / "textures" / basename);
						}
					}

					const std::string filename = ToString(source->filename);
					if(!filename.empty()) candidates.emplace_back(filename);

					std::error_code pathError;
					for(const std::filesystem::path& candidate : candidates)
					{
						pathError.clear();
						if(!IsPathInsideDirectory(externalFileContext.modelDirectory, candidate, pathError)) continue;
						pathError.clear();
						if(!std::filesystem::is_regular_file(candidate, pathError) || pathError) continue;

						pathError.clear();
						const std::filesystem::path canonicalPath =
							std::filesystem::weakly_canonical(candidate, pathError);
						if(pathError) continue;
						const bool alreadyAccounted = externalFileContext.accountedPaths.find(canonicalPath)
							!= externalFileContext.accountedPaths.end();
						if(!alreadyAccounted)
						{
							const uintmax_t fileBytes = std::filesystem::file_size(canonicalPath, pathError);
							if(pathError || fileBytes > std::numeric_limits<uint64_t>::max()) continue;
							const uint64_t sourceBytes = static_cast<uint64_t>(fileBytes);
							if(externalFileContext.sourceBytes > externalFileContext.sourceByteLimit
								|| sourceBytes > externalFileContext.sourceByteLimit - externalFileContext.sourceBytes)
							{
								return ReportModelError(
									ModelDiagnosticCode::ResourceLimitExceeded,
									filepath,
									"external texture exceeds maxSourceBytes",
									element);
							}
							externalFileContext.sourceBytes += sourceBytes;
							externalFileContext.accountedPaths.insert(canonicalPath);
						}
						texture.sourcePath = canonicalPath.string();
						break;
					}
				}

				textureIndices[source] = static_cast<uint32_t>(outModel.textures.size());
				outModel.textures.push_back(std::move(texture));
			}
			return true;
		}

	}

		[[nodiscard]] bool LoadUfbxModel(const std::string& filepath, ModelData& outModel, const ModelLoadOptions& options)
		{
			if(options.maxNodeDepth == 0u)
				return ReportModelError(
					ModelDiagnosticCode::InvalidData,
					filepath,
					"maxNodeDepth must be greater than zero");
			if(!std::isfinite(options.fbxBakeRate) || options.fbxBakeRate <= 0.0f)
				return ReportModelError(
					ModelDiagnosticCode::InvalidData,
					filepath,
					"FBX bake rate must be finite and greater than zero");
			if(!std::isfinite(options.fbxConstantTrackTolerance) || options.fbxConstantTrackTolerance < 0.0f)
				return ReportModelError(
					ModelDiagnosticCode::InvalidData,
					filepath,
					"FBX constant track tolerance must be finite and non-negative");
			bool warnedInfluenceTruncation = false;
			const size_t parserByteLimit = options.maxParserBytes > static_cast<uint64_t>(SIZE_MAX)
				? SIZE_MAX
				: static_cast<size_t>(options.maxParserBytes);
			UfbxAllocatorBudget allocatorBudget{ parserByteLimit, 0u, false };
			ufbx_allocator sharedAllocator = {};
			sharedAllocator.alloc_fn = UfbxBudgetAlloc;
			sharedAllocator.realloc_fn = UfbxBudgetRealloc;
			sharedAllocator.free_fn = UfbxBudgetFree;
			sharedAllocator.user = &allocatorBudget;
			std::error_code sourceSizeError;
			const uintmax_t mainSourceSize = std::filesystem::file_size(filepath, sourceSizeError);
			if(sourceSizeError || mainSourceSize > std::numeric_limits<uint64_t>::max())
			{
				return ReportModelError(
					ModelDiagnosticCode::FileReadFailed,
					filepath,
					"failed to query ufbx model source size");
			}
			UfbxExternalFileContext externalFileContext{
				std::filesystem::path(filepath).parent_path(),
				options.maxSourceBytes,
				static_cast<uint64_t>(mainSourceSize) };
			ufbx_load_opts opts = {};
			opts.temp_allocator.allocator = sharedAllocator;
			opts.temp_allocator.memory_limit = parserByteLimit;
			opts.result_allocator.allocator = sharedAllocator;
			opts.result_allocator.memory_limit = parserByteLimit;
			opts.load_external_files = true;
			opts.open_main_file_with_default = true;
			opts.open_file_cb.fn = OpenRestrictedUfbxExternalFile;
			opts.open_file_cb.user = &externalFileContext;
			opts.generate_missing_normals = true;
			opts.node_depth_limit = options.maxNodeDepth;
			opts.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_HELPER_NODES;
			opts.scale_helper_name.data = "(scale helper)";
			opts.scale_helper_name.length = SIZE_MAX;

			ufbx_error error;
			ufbx_scene* scene = ufbx_load_file(filepath.c_str(), &opts, &error);
			if(externalFileContext.pathRejected)
			{
				if(scene != nullptr) ufbx_free_scene(scene);
				return ReportModelError(
					ModelDiagnosticCode::InvalidData,
					filepath,
					"ufbx external file path escapes the model directory: "
						+ externalFileContext.rejectedPath,
					"externalFile");
			}
			if(externalFileContext.sourceLimitExceeded)
			{
				if(scene != nullptr) ufbx_free_scene(scene);
				return ReportModelError(
					ModelDiagnosticCode::ResourceLimitExceeded,
					filepath,
					"ufbx source bytes exceed maxSourceBytes while accounting for external files",
					"externalFile");
			}
			if(scene == nullptr)
			{
				if(allocatorBudget.limitExceeded
					|| error.type == UFBX_ERROR_MEMORY_LIMIT
					|| error.type == UFBX_ERROR_ALLOCATION_LIMIT)
				{
					return ReportModelError(
						ModelDiagnosticCode::ResourceLimitExceeded,
						filepath,
						"ufbx parser memory limit exceeded");
				}
				if(error.type == UFBX_ERROR_NODE_DEPTH_LIMIT)
				{
					return ReportModelError(
						ModelDiagnosticCode::ResourceLimitExceeded,
						filepath,
						"ufbx node hierarchy exceeds maxNodeDepth");
				}
				return ReportModelError(
					ModelDiagnosticCode::ParseFailed,
					filepath,
					std::string("failed to load model through ufbx: ") + error.info);
			}
			std::unique_ptr<ufbx_scene, void(*)(ufbx_scene*)> sceneOwner(scene, ufbx_free_scene);
			outModel = {};
			std::map<const ufbx_texture*, uint32_t> textureIndices;
			uint64_t decodedTextureBytes = 0u;
			if(!LoadUfbxTextureAssets(
				filepath,
				*scene,
				options,
				externalFileContext,
				outModel,
				textureIndices,
				decodedTextureBytes)) return false;

			ModelLoadOptions geometryOptions = options;
			geometryOptions.maxDecodedBytes -= decodedTextureBytes;
			const double fbxBakeRate = static_cast<double>(options.fbxBakeRate);
			if(!ValidateFbxLoadLimits(filepath, *scene, geometryOptions, fbxBakeRate)) return false;
			const bool animatedModel = scene->skin_deformers.count > 0
				|| scene->anim_stacks.count > 0
				|| scene->blend_deformers.count > 0;
			std::map<const ufbx_node*, uint32_t> nodeIndices;
			outModel.nodes.resize(scene->nodes.count);
			for(size_t nodeIndex = 0; nodeIndex < scene->nodes.count; ++nodeIndex)
				nodeIndices[scene->nodes.data[nodeIndex]] = static_cast<uint32_t>(nodeIndex);
			for(size_t nodeIndex = 0; nodeIndex < scene->nodes.count; ++nodeIndex)
			{
				const ufbx_node* source = scene->nodes.data[nodeIndex];
				ModelNode& node = outModel.nodes[nodeIndex];
				node.name = source->name.data != nullptr ? source->name.data : "";
				node.bindTransform = ToNodeTransform(source->local_transform);
				if(!IsFinite(node.bindTransform))
				{
					return ReportModelError(
						ModelDiagnosticCode::InvalidData,
						filepath,
						"ufbx node contains non-finite transform values",
						"nodes[" + std::to_string(nodeIndex) + "].transform");
				}
				if(source->parent != nullptr)
				{
					const auto parent = nodeIndices.find(source->parent);
					if(parent == nodeIndices.end()) return false;
					node.parentIndex = static_cast<int32_t>(parent->second);
				}
			}

			std::vector<std::vector<UfbxMorphSource>> morphSourcesByNode(scene->nodes.count);
			std::vector<UfbxBlendChannelBinding> blendChannelBindings;
			for(size_t nodeIndex = 0; nodeIndex < scene->nodes.count; ++nodeIndex)
			{
				const ufbx_mesh* mesh = scene->nodes.data[nodeIndex]->mesh;
				if(mesh == nullptr) continue;
				ModelNode& modelNode = outModel.nodes[nodeIndex];
				std::vector<UfbxMorphSource>& morphSources = morphSourcesByNode[nodeIndex];
				for(size_t deformerIndex = 0; deformerIndex < mesh->blend_deformers.count; ++deformerIndex)
				{
					const ufbx_blend_deformer* deformer = mesh->blend_deformers.data[deformerIndex];
					for(size_t channelIndex = 0; channelIndex < deformer->channels.count; ++channelIndex)
					{
						const ufbx_blend_channel* channel = deformer->channels.data[channelIndex];
						if(channel == nullptr || channel->keyframes.count == 0u) continue;
						if(morphSources.size() > std::numeric_limits<uint32_t>::max()
							|| channel->keyframes.count > std::numeric_limits<uint32_t>::max() - morphSources.size())
							return false;
						const uint32_t firstTargetIndex = static_cast<uint32_t>(morphSources.size());
						const float channelWeight = static_cast<float>(channel->weight * 0.01);
						if(!std::isfinite(channelWeight))
						{
							return ReportModelError(
								ModelDiagnosticCode::InvalidData,
								filepath,
								"FBX blend channel weight is non-finite",
								"nodes[" + std::to_string(nodeIndex) + "].blendChannels["
									+ std::to_string(channelIndex) + "]");
						}
						const std::vector<float> defaultWeights =
							EvaluateUfbxBlendKeyframeWeights(*channel, channelWeight);
						for(size_t keyframeIndex = 0; keyframeIndex < channel->keyframes.count; ++keyframeIndex)
						{
							const ufbx_blend_keyframe& keyframe = channel->keyframes.data[keyframeIndex];
							if(keyframe.shape == nullptr || !std::isfinite(keyframe.target_weight))
							{
								return ReportModelError(
									ModelDiagnosticCode::InvalidData,
									filepath,
									"FBX blend keyframe is invalid",
									"nodes[" + std::to_string(nodeIndex) + "].blendChannels["
										+ std::to_string(channelIndex) + "].keyframes["
										+ std::to_string(keyframeIndex) + "]");
							}
							std::string targetName = ToString(keyframe.shape->name);
							if(targetName.empty())
								targetName = ToString(channel->name) + "_" + std::to_string(keyframeIndex);
							morphSources.push_back(UfbxMorphSource{ keyframe.shape, std::move(targetName) });
							modelNode.morphWeights.push_back(defaultWeights[keyframeIndex]);
						}
						blendChannelBindings.push_back(UfbxBlendChannelBinding{
							channel,
							static_cast<uint32_t>(nodeIndex),
							firstTargetIndex,
							static_cast<uint32_t>(channel->keyframes.count) });
					}
				}
			}

			std::map<const ufbx_skin_deformer*, uint32_t> skinIndices;
			outModel.skins.reserve(scene->skin_deformers.count);
			for(size_t skinIndex = 0; skinIndex < scene->skin_deformers.count; ++skinIndex)
			{
				const ufbx_skin_deformer* source = scene->skin_deformers.data[skinIndex];
				ModelSkin skin;
				skin.name = source->name.data != nullptr ? source->name.data : "";
				skin.method = ToSkinningMethod(source->skinning_method);
				skin.jointNodeIndices.reserve(source->clusters.count);
				skin.inverseBindMatrices.reserve(source->clusters.count);
				for(size_t clusterIndex = 0; clusterIndex < source->clusters.count; ++clusterIndex)
				{
					const ufbx_skin_cluster* cluster = source->clusters.data[clusterIndex];
					const auto joint = nodeIndices.find(cluster->bone_node);
					if(cluster->bone_node == nullptr || joint == nodeIndices.end())
					{
						return ReportModelError(
							ModelDiagnosticCode::InvalidData,
							filepath,
							"FBX skin cluster has no valid bone node",
							"skinDeformers[" + std::to_string(skinIndex)
								+ "].clusters[" + std::to_string(clusterIndex) + "]");
					}
					const Math::float4x4 inverseBind = ToFloat4x4(cluster->mesh_node_to_bone);
					if(!IsFinite(inverseBind))
					{
						return ReportModelError(
							ModelDiagnosticCode::InvalidData,
							filepath,
							"ufbx skin cluster contains a non-finite inverse-bind matrix",
							"skinDeformers[" + std::to_string(skinIndex)
								+ "].clusters[" + std::to_string(clusterIndex) + "].inverseBindMatrix");
					}
					skin.jointNodeIndices.push_back(joint->second);
					skin.inverseBindMatrices.push_back(inverseBind);
				}
				skinIndices[source] = static_cast<uint32_t>(outModel.skins.size());
				outModel.skins.push_back(std::move(skin));
			}

			outModel.animations.reserve(scene->anim_stacks.count);
			for(size_t stackIndex = 0; stackIndex < scene->anim_stacks.count; ++stackIndex)
			{
				const ufbx_anim_stack* stack = scene->anim_stacks.data[stackIndex];
				if(stack->anim == nullptr) continue;
				const double beginTime = stack->time_begin;
				const double duration = std::max(0.0, stack->time_end - beginTime);
				const size_t sampleCount = duration > 0.0
					? static_cast<size_t>(std::ceil(duration * fbxBakeRate)) + 1u
					: 1u;
				if(sampleCount > 100000u)
				{
					return ReportModelError(
						ModelDiagnosticCode::ResourceLimitExceeded,
						filepath,
						"FBX animation exceeds the per-track bake sample limit");
				}

				AnimationClip clip;
				clip.name = stack->name.length > 0 ? stack->name.data : ("Animation_" + std::to_string(stackIndex));
				clip.duration = static_cast<float>(duration);
				clip.tracks.reserve(scene->nodes.count);
				for(size_t nodeIndex = 0; nodeIndex < scene->nodes.count; ++nodeIndex)
				{
					NodeAnimationTrack track;
					track.nodeIndex = static_cast<uint32_t>(nodeIndex);
					track.translations.reserve(sampleCount);
					track.rotations.reserve(sampleCount);
					track.scales.reserve(sampleCount);
					Math::quat previousRotation = Math::quat::Identity();
					for(size_t sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
					{
						const double relativeTime = sampleCount > 1
							? std::min(duration, static_cast<double>(sampleIndex) / fbxBakeRate)
							: 0.0;
						const ufbx_transform transform = ufbx_evaluate_transform(
							stack->anim,
							scene->nodes.data[nodeIndex],
							beginTime + relativeTime);
						NodeTransform sampled = ToNodeTransform(transform);
						if(!IsFinite(sampled))
						{
							return ReportModelError(
								ModelDiagnosticCode::InvalidData,
								filepath,
								"ufbx animation sample contains non-finite transform values",
								"animationStacks[" + std::to_string(stackIndex)
									+ "].nodes[" + std::to_string(nodeIndex)
									+ "].samples[" + std::to_string(sampleIndex) + "]");
						}
						if(sampleIndex > 0 && Math::Dot(previousRotation, sampled.rotation) < 0.0f)
							sampled.rotation = Math::quat(-sampled.rotation.x, -sampled.rotation.y, -sampled.rotation.z, -sampled.rotation.w);
						previousRotation = sampled.rotation;

						const float keyTime = static_cast<float>(relativeTime);
						Vec3Key translationKey;
						translationKey.time = keyTime;
						translationKey.value = sampled.translation;
						track.translations.push_back(translationKey);
						QuatKey rotationKey;
						rotationKey.time = keyTime;
						rotationKey.value = sampled.rotation;
						track.rotations.push_back(rotationKey);
						Vec3Key scaleKey;
						scaleKey.time = keyTime;
						scaleKey.value = sampled.scale;
						track.scales.push_back(scaleKey);
					}
					const float tolerance = options.fbxConstantTrackTolerance;
					CollapseConstantTrack(track.translations, [tolerance](const Math::float3& lhs, const Math::float3& rhs) {
						return NearlyEqual(lhs, rhs, tolerance);
					});
					CollapseConstantTrack(track.rotations, [tolerance](const Math::quat& lhs, const Math::quat& rhs) {
						return NearlyEqualRotation(lhs, rhs, tolerance);
					});
					CollapseConstantTrack(track.scales, [tolerance](const Math::float3& lhs, const Math::float3& rhs) {
						return NearlyEqual(lhs, rhs, tolerance);
					});
					clip.tracks.push_back(std::move(track));
				}
				for(const UfbxBlendChannelBinding& binding : blendChannelBindings)
				{
					if(binding.channel == nullptr || binding.targetCount == 0u) continue;
					std::vector<MorphWeightTrack> tracks(binding.targetCount);
					for(uint32_t targetOffset = 0u; targetOffset < binding.targetCount; ++targetOffset)
					{
						MorphWeightTrack& track = tracks[targetOffset];
						track.nodeIndex = binding.nodeIndex;
						track.targetIndex = binding.firstTargetIndex + targetOffset;
						track.weights.reserve(sampleCount);
					}
					for(size_t sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
					{
						const double relativeTime = sampleCount > 1
							? std::min(duration, static_cast<double>(sampleIndex) / fbxBakeRate)
							: 0.0;
						const float channelWeight = static_cast<float>(ufbx_evaluate_blend_weight(
							stack->anim,
							binding.channel,
							beginTime + relativeTime));
						if(!std::isfinite(channelWeight))
						{
							return ReportModelError(
								ModelDiagnosticCode::InvalidData,
								filepath,
								"FBX blend animation sample is non-finite",
								"animationStacks[" + std::to_string(stackIndex)
									+ "].nodes[" + std::to_string(binding.nodeIndex)
									+ "].morphSamples[" + std::to_string(sampleIndex) + "]");
						}
						const std::vector<float> effectiveWeights =
							EvaluateUfbxBlendKeyframeWeights(*binding.channel, channelWeight);
						if(effectiveWeights.size() != binding.targetCount) return false;
						for(uint32_t targetOffset = 0u; targetOffset < binding.targetCount; ++targetOffset)
						{
							FloatKey key;
							key.time = static_cast<float>(relativeTime);
							key.value = effectiveWeights[targetOffset];
							tracks[targetOffset].weights.push_back(key);
						}
					}
					const float tolerance = options.fbxConstantTrackTolerance;
					for(MorphWeightTrack& track : tracks)
					{
						CollapseConstantTrack(track.weights, [tolerance](float lhs, float rhs) {
							return std::fabs(lhs - rhs) <= tolerance;
						});
						clip.morphTracks.push_back(std::move(track));
					}
				}
				outModel.animations.push_back(std::move(clip));
			}

			outModel.materials.reserve(scene->materials.count);
			std::map<const ufbx_material*, uint32_t> materialIndices;
			auto applyTexture = [&](ModelMaterialInfo& material, MaterialTextureKind kind, const ufbx_texture* source) {
				if(source == nullptr) return;
				const auto texture = textureIndices.find(source);
				if(texture == textureIndices.end() || texture->second >= outModel.textures.size()) return;
				const TextureAsset& asset = outModel.textures[texture->second];
				if(asset.sourcePath.empty() && asset.rgba8.empty()) return;

				const uint32_t slot = static_cast<uint32_t>(kind);
				material.textureIndices[slot] = texture->second;
				material.texturePaths[slot] = asset.sourcePath;
				material.hasTexture[slot] = true;
			};
			for(size_t i = 0; i < scene->materials.count; ++i)
			{
				ufbx_material* mat = scene->materials.data[i];
				ModelMaterialInfo material = {};
				material.name = mat->name.data != nullptr ? mat->name.data : "";
				material.material.baseColor = Math::float4(
					static_cast<float>(mat->pbr.base_color.value_vec3.x),
					static_cast<float>(mat->pbr.base_color.value_vec3.y),
					static_cast<float>(mat->pbr.base_color.value_vec3.z),
					1.0f);
				material.material.metallicFactor = static_cast<float>(mat->pbr.metalness.value_real);
				material.material.roughnessFactor = static_cast<float>(mat->pbr.roughness.value_real);
				if(!std::isfinite(material.material.baseColor.x)
					|| !std::isfinite(material.material.baseColor.y)
					|| !std::isfinite(material.material.baseColor.z)
					|| !std::isfinite(material.material.metallicFactor)
					|| !std::isfinite(material.material.roughnessFactor))
				{
					return ReportModelError(
						ModelDiagnosticCode::InvalidData,
						filepath,
						"ufbx material contains non-finite values",
						"materials[" + std::to_string(i) + "]");
				}
				material.hasBaseColor = true;

				const ufbx_texture* baseColorTexture = mat->pbr.base_color.texture != nullptr
					? mat->pbr.base_color.texture
					: mat->fbx.diffuse_color.texture;
				applyTexture(material, MaterialTextureKind::BaseColor, baseColorTexture);
				if(mat->pbr.metalness.texture != nullptr
					&& mat->pbr.metalness.texture == mat->pbr.roughness.texture)
				{
					applyTexture(material, MaterialTextureKind::MetallicRoughness, mat->pbr.metalness.texture);
				}
				const ufbx_texture* normalTexture = mat->pbr.normal_map.texture != nullptr
					? mat->pbr.normal_map.texture
					: (mat->fbx.normal_map.texture != nullptr
						? mat->fbx.normal_map.texture
						: mat->fbx.bump.texture);
				applyTexture(material, MaterialTextureKind::Normal, normalTexture);
				applyTexture(material, MaterialTextureKind::Occlusion, mat->pbr.ambient_occlusion.texture);
				const ufbx_texture* emissiveTexture = mat->pbr.emission_color.texture != nullptr
					? mat->pbr.emission_color.texture
					: mat->fbx.emission_color.texture;
				applyTexture(material, MaterialTextureKind::Emissive, emissiveTexture);
				materialIndices[mat] = static_cast<uint32_t>(outModel.materials.size());
				outModel.materials.push_back(std::move(material));
			}
			[[maybe_unused]] const uint32_t defaultMaterialIndex = EnsureDefaultMaterial(outModel);

			auto resolveMaterialIndex = [&](ufbx_node* node, ufbx_mesh* sourceMesh, uint32_t localMaterialIndex) -> uint32_t {
				ufbx_material* material = nullptr;
				if(localMaterialIndex < node->materials.count)
				{
					material = node->materials.data[localMaterialIndex];
				}
				else if(localMaterialIndex < sourceMesh->materials.count)
				{
					material = sourceMesh->materials.data[localMaterialIndex];
				}
				const auto it = materialIndices.find(material);
				return it != materialIndices.end() ? it->second : defaultMaterialIndex;
			};

			for(size_t ni = 0; ni < scene->nodes.count; ++ni)
			{
				ufbx_node* node = scene->nodes.data[ni];
				if(node->mesh == nullptr) continue;

				ufbx_mesh* sourceMesh = node->mesh;
				const auto modelNode = nodeIndices.find(node);
				if(modelNode == nodeIndices.end()) return false;
				const ufbx_skin_deformer* skinDeformer = sourceMesh->skin_deformers.count > 0
					? sourceMesh->skin_deformers.data[0]
					: nullptr;
				if(skinDeformer != nullptr)
				{
					for(size_t deformerIndex = 1u; deformerIndex < sourceMesh->skin_deformers.count; ++deformerIndex)
					{
						const ufbx_skin_deformer* additional = sourceMesh->skin_deformers.data[deformerIndex];
						if(additional == nullptr || !AreCompatibleSkinDeformers(*skinDeformer, *additional))
						{
							return ReportModelError(
								ModelDiagnosticCode::UnsupportedFeature,
								filepath,
								"FBX mesh uses incompatible multiple skin deformers",
								"nodes[" + std::to_string(ni) + "].mesh.skinDeformers["
									+ std::to_string(deformerIndex) + "]");
						}
					}
				}
				uint32_t modelSkinIndex = kInvalidAnimationIndex;
				if(skinDeformer != nullptr)
				{
					const auto skin = skinIndices.find(skinDeformer);
					if(skin == skinIndices.end()) return false;
					modelSkinIndex = skin->second;
				}
				std::map<uint32_t, ModelMesh> meshesByMaterial;
				const std::string nodeName = node->name.data != nullptr ? node->name.data : "";

				auto getMesh = [&](uint32_t materialIndex) -> ModelMesh& {
					ModelMesh& mesh = meshesByMaterial[materialIndex];
					if(mesh.name.empty())
					{
						const std::string materialName = materialIndex < outModel.materials.size()
							? outModel.materials[materialIndex].name
							: std::string{};
						mesh.name = materialName.empty()
							? nodeName + "_" + std::to_string(materialIndex)
							: nodeName + "_" + materialName;
						mesh.materialIndex = materialIndex;
						if(animatedModel) mesh.nodeIndex = modelNode->second;
						mesh.skinIndex = modelSkinIndex;
						mesh.defaultMorphWeights = outModel.nodes[ni].morphWeights;
						mesh.morphTargets.reserve(morphSourcesByNode[ni].size());
						for(const UfbxMorphSource& source : morphSourcesByNode[ni])
						{
							MorphTarget target;
							target.name = source.name;
							mesh.morphTargets.push_back(std::move(target));
						}
					}
					return mesh;
				};
				const ufbx_matrix& vertexMatrix = animatedModel
					? node->geometry_to_node : node->geometry_to_world;
				const ufbx_matrix normalMatrix = ufbx_matrix_for_normals(&vertexMatrix);
				const bool hasAuthoredTangents = sourceMesh->vertex_tangent.exists;

				for(size_t fi = 0; fi < sourceMesh->faces.count; ++fi)
				{
					ufbx_face face = sourceMesh->faces.data[fi];
					if(face.num_indices < 3) continue;

					const uint32_t localMaterialIndex = fi < sourceMesh->face_material.count
						? sourceMesh->face_material.data[fi]
						: 0u;
					ModelMesh& mesh = getMesh(resolveMaterialIndex(node, sourceMesh, localMaterialIndex));
					std::vector<uint32_t> triIndices((face.num_indices - 2u) * 3u);
					const size_t numTris = ufbx_triangulate_face(triIndices.data(), triIndices.size(), sourceMesh, face);

					for(size_t ti = 0; ti < numTris * 3u; ++ti)
					{
						const uint32_t index = triIndices[ti];
						if(index >= sourceMesh->vertex_indices.count) return false;
						const uint32_t logicalVertex = sourceMesh->vertex_indices.data[index];
						Vertex vertex = {};

						const ufbx_vec3 localPos = ufbx_get_vertex_vec3(&sourceMesh->vertex_position, index);
						const ufbx_vec3 transformedPosition = ufbx_transform_position(&vertexMatrix, localPos);
						vertex.position = Math::float3(
							static_cast<float>(transformedPosition.x),
							static_cast<float>(transformedPosition.y),
							static_cast<float>(transformedPosition.z));
						if(!IsFinite(vertex.position))
						{
							return ReportModelError(
								ModelDiagnosticCode::InvalidData,
								filepath,
								"ufbx vertex position contains non-finite values",
								"nodes[" + std::to_string(ni) + "].mesh.faces["
									+ std::to_string(fi) + "].position");
						}

						if(sourceMesh->vertex_normal.exists)
						{
							const ufbx_vec3 localNormal = ufbx_get_vertex_vec3(&sourceMesh->vertex_normal, index);
							const ufbx_vec3 worldNormal = ufbx_transform_direction(&normalMatrix, localNormal);
							vertex.normal = NormalizeOr(
								Math::float3(static_cast<float>(worldNormal.x), static_cast<float>(worldNormal.y), static_cast<float>(worldNormal.z)),
								Math::float3(0.0f, 1.0f, 0.0f));
							if(!IsFinite(vertex.normal))
							{
								return ReportModelError(
									ModelDiagnosticCode::InvalidData,
									filepath,
									"ufbx vertex normal contains non-finite values",
									"nodes[" + std::to_string(ni) + "].mesh.faces["
										+ std::to_string(fi) + "].normal");
							}
						}
						else
						{
							vertex.normal = Math::float3(0.0f, 1.0f, 0.0f);
						}

						if(hasAuthoredTangents)
						{
							const ufbx_vec3 localTangent =
								ufbx_get_vertex_vec3(&sourceMesh->vertex_tangent, index);
							const ufbx_vec3 transformedTangent =
								ufbx_transform_direction(&vertexMatrix, localTangent);
							const Math::float3 tangentDirection(
								static_cast<float>(transformedTangent.x),
								static_cast<float>(transformedTangent.y),
								static_cast<float>(transformedTangent.z));
							const Math::float3 orthogonalTangent = tangentDirection
								- vertex.normal * Dot(vertex.normal, tangentDirection);
							const Math::float3 tangent = NormalizeOr(
								orthogonalTangent,
								BuildFallbackTangent(vertex.normal));
							if(!IsFinite(tangent))
							{
								return ReportModelError(
									ModelDiagnosticCode::InvalidData,
									filepath,
									"ufbx vertex tangent contains non-finite values",
									"nodes[" + std::to_string(ni) + "].mesh.faces["
										+ std::to_string(fi) + "].tangent");
							}

							float handedness = -1.0f;
							if(sourceMesh->vertex_bitangent.exists)
							{
								const ufbx_vec3 localBitangent =
									ufbx_get_vertex_vec3(&sourceMesh->vertex_bitangent, index);
								const ufbx_vec3 transformedBitangent =
									ufbx_transform_direction(&vertexMatrix, localBitangent);
								const Math::float3 bitangent(
									static_cast<float>(transformedBitangent.x),
									static_cast<float>(transformedBitangent.y),
									static_cast<float>(transformedBitangent.z));
								if(!IsFinite(bitangent))
								{
									return ReportModelError(
										ModelDiagnosticCode::InvalidData,
										filepath,
										"ufbx vertex bitangent contains non-finite values",
										"nodes[" + std::to_string(ni) + "].mesh.faces["
											+ std::to_string(fi) + "].bitangent");
								}
								// Imported FBX bitangent uses the authored V axis, while the engine flips V.
								handedness = Dot(Cross(vertex.normal, tangent), bitangent) < 0.0f
									? 1.0f : -1.0f;
							}
							vertex.tangent = Math::float4(tangent.x, tangent.y, tangent.z, handedness);
						}

						if(sourceMesh->vertex_uv.exists)
						{
							const ufbx_vec2 uv = ufbx_get_vertex_vec2(&sourceMesh->vertex_uv, index);
							// FBX 포맷은 기본적으로 텍스처 좌표계 Y축(V)이 아래에서 위로 향하지만, 
							// 통합 엔진에서는 텍스처 이미지의 수직 반전을 수행하지 않으므로 UV의 Y축을 반전시켜야 렌더링 시 정상 출력됩니다.
							vertex.uv = Math::float2(static_cast<float>(uv.x), 1.0f - static_cast<float>(uv.y));
							if(!IsFinite(vertex.uv))
							{
								return ReportModelError(
									ModelDiagnosticCode::InvalidData,
									filepath,
									"ufbx vertex UV contains non-finite values",
									"nodes[" + std::to_string(ni) + "].mesh.faces["
										+ std::to_string(fi) + "].uv");
							}
						}

						if(skinDeformer != nullptr)
						{
							std::map<uint32_t, float> combinedInfluences;
							const ufbx_skin_vertex* primarySkinVertex = nullptr;
							for(size_t deformerIndex = 0u; deformerIndex < sourceMesh->skin_deformers.count; ++deformerIndex)
							{
								const ufbx_skin_deformer* deformer = sourceMesh->skin_deformers.data[deformerIndex];
								if(deformer == nullptr || logicalVertex >= deformer->vertices.count) return false;
								const ufbx_skin_vertex& skinVertex = deformer->vertices.data[logicalVertex];
								if(primarySkinVertex == nullptr) primarySkinVertex = &skinVertex;
								if(static_cast<size_t>(skinVertex.weight_begin) + skinVertex.num_weights > deformer->weights.count)
									return false;
								for(uint32_t weightIndex = 0; weightIndex < skinVertex.num_weights; ++weightIndex)
								{
									const ufbx_skin_weight& weight =
										deformer->weights.data[skinVertex.weight_begin + weightIndex];
									if(weight.cluster_index >= deformer->clusters.count) return false;
									if(!std::isfinite(weight.weight) || weight.weight < 0.0)
									{
										return ReportModelError(
											ModelDiagnosticCode::InvalidData,
											filepath,
											"ufbx skin weight must be finite and non-negative",
											"nodes[" + std::to_string(ni) + "].mesh.faces["
												+ std::to_string(fi) + "].skinDeformers["
												+ std::to_string(deformerIndex) + "].weights["
												+ std::to_string(weightIndex) + "]");
									}
									combinedInfluences[weight.cluster_index] += static_cast<float>(weight.weight);
								}
							}
							if(primarySkinVertex == nullptr) return false;
							std::vector<std::pair<uint32_t, float>> influences(
								combinedInfluences.begin(),
								combinedInfluences.end());
							bool truncated = false;
							SkinInfluence influence = MakeSkinInfluence(std::move(influences), truncated);
							if(skinDeformer->skinning_method == UFBX_SKINNING_METHOD_DUAL_QUATERNION)
							{
								influence.dqBlendWeight = 1.0f;
							}
							else if(skinDeformer->skinning_method == UFBX_SKINNING_METHOD_BLENDED_DQ_LINEAR)
							{
								const float dqBlendWeight = static_cast<float>(primarySkinVertex->dq_weight);
								if(!std::isfinite(dqBlendWeight) || dqBlendWeight < 0.0f || dqBlendWeight > 1.0f)
								{
									return ReportModelError(
										ModelDiagnosticCode::InvalidData,
										filepath,
										"FBX dual-quaternion blend weight must be finite and in [0, 1]",
										"nodes[" + std::to_string(ni) + "].mesh.faces["
											+ std::to_string(fi) + "].dqWeight");
								}
								influence.dqBlendWeight = dqBlendWeight;
							}
							mesh.mesh.skinInfluences.push_back(influence);
							if(truncated && !warnedInfluenceTruncation)
							{
								ReportModelWarning(
									ModelDiagnosticCode::DataLoss,
									filepath,
									"FBX vertex influences were truncated to the four strongest joints");
								warnedInfluenceTruncation = true;
							}
						}

						if(mesh.morphTargets.size() != morphSourcesByNode[ni].size()) return false;
						for(size_t targetIndex = 0; targetIndex < mesh.morphTargets.size(); ++targetIndex)
						{
							const ufbx_blend_shape* shape = morphSourcesByNode[ni][targetIndex].shape;
							if(shape == nullptr) return false;
							const ufbx_vec3 sourcePositionDelta =
								ufbx_get_blend_shape_vertex_offset(shape, logicalVertex);
							const ufbx_vec3 transformedPositionDelta =
								ufbx_transform_direction(&vertexMatrix, sourcePositionDelta);
							const Math::float3 positionDelta(
								static_cast<float>(transformedPositionDelta.x),
								static_cast<float>(transformedPositionDelta.y),
								static_cast<float>(transformedPositionDelta.z));
							if(!IsFinite(positionDelta))
							{
								return ReportModelError(
									ModelDiagnosticCode::InvalidData,
									filepath,
									"FBX blend shape contains non-finite position deltas",
									"nodes[" + std::to_string(ni) + "].morphTargets["
										+ std::to_string(targetIndex) + "]");
							}
							MorphTarget& target = mesh.morphTargets[targetIndex];
							target.positionDeltas.push_back(positionDelta);

							if(shape->normal_offsets.count > 0u)
							{
								ufbx_vec3 sourceNormalDelta = {};
								const uint32_t offsetIndex =
									ufbx_get_blend_shape_offset_index(shape, logicalVertex);
								if(offsetIndex != UFBX_NO_INDEX && offsetIndex < shape->normal_offsets.count)
								{
									sourceNormalDelta = shape->normal_offsets.data[offsetIndex];
									if(offsetIndex < shape->offset_weights.count)
									{
										const ufbx_real offsetWeight = shape->offset_weights.data[offsetIndex];
										sourceNormalDelta.x *= offsetWeight;
										sourceNormalDelta.y *= offsetWeight;
										sourceNormalDelta.z *= offsetWeight;
									}
								}
								const ufbx_vec3 transformedNormalDelta =
									ufbx_transform_direction(&normalMatrix, sourceNormalDelta);
								const Math::float3 normalDelta(
									static_cast<float>(transformedNormalDelta.x),
									static_cast<float>(transformedNormalDelta.y),
									static_cast<float>(transformedNormalDelta.z));
								if(!IsFinite(normalDelta))
								{
									return ReportModelError(
										ModelDiagnosticCode::InvalidData,
										filepath,
										"FBX blend shape contains non-finite normal deltas",
										"nodes[" + std::to_string(ni) + "].morphTargets["
											+ std::to_string(targetIndex) + "]");
								}
								target.normalDeltas.push_back(normalDelta);
							}
						}

						mesh.mesh.indices.push_back(static_cast<uint32_t>(mesh.mesh.vertices.size()));
						mesh.mesh.vertices.push_back(vertex);
					}
				}

				for(auto& [materialIndex, mesh] : meshesByMaterial)
				{
					(void)materialIndex;
					if(mesh.mesh.indices.empty()) continue;
					if(!hasAuthoredTangents) CalculateTangents(mesh.mesh);
					outModel.meshes.push_back(std::move(mesh));
				}
			}

			return !outModel.meshes.empty();
		}
}
