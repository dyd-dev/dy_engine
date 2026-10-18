#include "ModelLoader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include <ufbx.h>

namespace dyf
{
	namespace
	{
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
		[[nodiscard]] bool UsesLocalFbxGeometry(const ufbx_scene& scene, const ufbx_node& node)
		{
			return scene.skin_deformers.count > 0 || scene.anim_stacks.count > 0 ||
				scene.blend_deformers.count > 0 || ufbx_matrix_determinant(&node.node_to_world) >= 0.0;
		}
		[[nodiscard]] bool CanShareFbxGeometry(const ufbx_scene& scene, const ufbx_node& node)
		{
			return node.mesh != nullptr && node.mesh->skin_deformers.count == 0 &&
				node.mesh->blend_deformers.count == 0 && UsesLocalFbxGeometry(scene, node);
		}
		[[nodiscard]] std::string FbxGeometryKey(const ufbx_node& node)
		{
			// Geometric pivots/offsets are baked into node-local vertices. Compare
			// their exact source bits, as well as per-instance material overrides.
			const auto& mesh = *node.mesh;
			std::string key(reinterpret_cast<const char*>(&mesh.typed_id), sizeof(mesh.typed_id));
			key.append(reinterpret_cast<const char*>(node.geometry_to_node.v), sizeof(node.geometry_to_node.v));
			const size_t slots = std::max(node.materials.count, mesh.materials.count);
			for(size_t slot = 0; slot < slots; ++slot)
			{
				const auto* material = slot < node.materials.count ? node.materials.data[slot] : mesh.materials.data[slot];
				const uint32_t identity = material != nullptr ? material->typed_id : UINT32_MAX;
				key.append(reinterpret_cast<const char*>(&identity), sizeof(identity));
			}
			return key;
		}
		[[nodiscard]] bool ValidateFbxLoadLimits(
			const std::string& filepath,
			const ufbx_scene& scene,
			const ModelLoadOptions& options,
			double bakeRate,
			std::vector<size_t>& sampleCounts,
			uint64_t& decodedBytes)
		{
			ModelLoadBudget budget(filepath, options);
			sampleCounts.assign(scene.anim_stacks.count, 0u);
			if(!budget.CheckNodes(scene.nodes.count)
				|| !budget.AddBytes(scene.nodes.count, sizeof(ModelNode), "nodes")
				|| !budget.AddBytes(scene.materials.count, sizeof(ModelMaterialInfo), "materials")) return false;
			if(options.importLights)
			{
				for(size_t i = 0; i < scene.nodes.count; ++i)
				{
					const ufbx_light* light = scene.nodes.data[i]->light;
					if(light == nullptr) continue;
					const size_t stride = light->type == UFBX_LIGHT_DIRECTIONAL ? sizeof(DirectionalLight)
						: light->type == UFBX_LIGHT_POINT ? sizeof(PointLight)
						: light->type == UFBX_LIGHT_SPOT ? sizeof(SpotLight) : 0u;
					if(stride != 0u && !budget.AddBytes(1u, stride, "FBX source lights")) return false;
				}
			}

			for(size_t skinIndex = 0; skinIndex < scene.skin_deformers.count; ++skinIndex)
			{
				const uint64_t jointCount = scene.skin_deformers.data[skinIndex]->clusters.count;
				if(!budget.CheckJoints(jointCount)
					|| !budget.AddBytes(jointCount, sizeof(uint32_t) + sizeof(Math::float4x4), "skin joints"))
					return false;
			}

			if(!budget.AddBytes(scene.anim_stacks.count, sizeof(AnimationClip), "FBX animation clips")) return false;
			for(size_t stackIndex = 0; stackIndex < scene.anim_stacks.count; ++stackIndex)
			{
				const ufbx_anim_stack* stack = scene.anim_stacks.data[stackIndex];
				if(stack->anim == nullptr) continue;
				const double span = stack->time_end - stack->time_begin;
				const double duration = std::max(0.0, span);
				const double intervals = std::ceil(duration * bakeRate);
				// The per-track cap is below both size_t and uint64_t limits on supported platforms.
				// Check while still floating point; conversion and +1 are then bounded.
				if(!std::isfinite(stack->time_begin) || !std::isfinite(stack->time_end)
					|| !std::isfinite(span) || !std::isfinite(intervals) || intervals > 99999.0)
					return ReportModelError(filepath, "FBX animation exceeds the per-track bake sample limit");
				const size_t sampleCount = static_cast<size_t>(intervals) + 1u;
				sampleCounts[stackIndex] = sampleCount;
				// Key storage is accounted after baking the source's animated channels
				// and removing constant tracks, rather than for every scene node.
			}

			std::set<std::string> countedGeometry;
			for(size_t nodeIndex = 0; nodeIndex < scene.nodes.count; ++nodeIndex)
			{
				const auto& node = *scene.nodes.data[nodeIndex];
				const ufbx_mesh* mesh = node.mesh;
				if(mesh == nullptr) continue;
				if(!budget.AddBytes(std::max<size_t>(1, mesh->material_parts.count),
					sizeof(ModelMesh) + sizeof(ModelAssetMesh), "FBX mesh instances")) return false;
				if(CanShareFbxGeometry(scene, node) && !countedGeometry.insert(FbxGeometryKey(node)).second) continue;
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
						filepath,
						"FBX morph target count exceeds maxMorphTargetsPerMesh");
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
			decodedBytes = budget.DecodedBytes();
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

		struct UfbxMorphSource
		{
			const ufbx_blend_shape* shape = nullptr;
			std::string name;
		};

		struct UfbxBlendChannelBinding
		{
			const ufbx_blend_channel* channel = nullptr;
			uint32_t nodeIndex = UINT32_MAX;
			uint32_t firstTargetIndex = UINT32_MAX;
			uint32_t targetCount = 0u;
		};

		[[nodiscard]] bool EvaluateUfbxBlendKeyframeWeights(
			const ufbx_blend_channel& channel,
			double channelWeight,
			std::vector<float>& weights)
		{
			weights.assign(channel.keyframes.count, 0.0f);
			if(!std::isfinite(channelWeight)) return false;
			// Match pinned ufbx's zero anchor, signed traversal and final-segment extrapolation.
			const size_t zero = channel.keyframes.count;
			size_t firstNonnegative = 0u;
			for(size_t index = 0; index < zero; ++index)
			{
				if(!std::isfinite(channel.keyframes.data[index].target_weight)) return false;
				if(channel.keyframes.data[index].target_weight < 0.0) firstNonnegative = index + 1u;
			}
			size_t previous = zero, next = zero;
			if(channelWeight > 0.0)
			{
				if(firstNonnegative > 0u) previous = firstNonnegative - 1u;
				for(size_t index = firstNonnegative; index < zero; ++index)
				{
					previous = next; next = index;
					if(channel.keyframes.data[next].target_weight > channelWeight) break;
				}
			}
			else
			{
				if(firstNonnegative < zero) previous = firstNonnegative;
				for(size_t index = firstNonnegative; index > 0u;)
				{
					previous = next; next = --index;
					if(channel.keyframes.data[next].target_weight < channelWeight) break;
				}
			}
			const auto target = [&](size_t index) -> double {
				return index == zero ? 0.0 : channel.keyframes.data[index].target_weight;
			};
			const double delta = target(next) - target(previous);
			if(!std::isfinite(delta)) return false;
			if(delta != 0.0)
			{
				const double alpha = (channelWeight - target(previous)) / delta;
				const double previousWeight = 1.0 - alpha;
				const auto fitsFloat = [](double value) {
					return std::isfinite(value) && std::fabs(value) <= std::numeric_limits<float>::max();
				};
				// Validate both stored endpoints before either narrowing conversion. The virtual
				// zero anchor has no output coefficient and remains outside the model data.
				if((previous != zero && !fitsFloat(previousWeight)) || (next != zero && !fitsFloat(alpha)))
					return false;
				if(previous != zero) weights[previous] = static_cast<float>(previousWeight);
				if(next != zero) weights[next] = static_cast<float>(alpha);
			}
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

		[[nodiscard]] bool ImportUfbxLights(
			const std::string& filepath,
			const ufbx_scene& scene,
			const Math::Bounds3& sourceBounds,
			ModelData& outModel,
			ModelLoadBudget* budget = nullptr)
		{
			bool warnedUnsupported = false, warnedDecay = false, warnedRange = false;
			const Math::float3 center = sourceBounds.valid ? sourceBounds.Center() : Math::float3(0, 0, 0);
			const float radius = sourceBounds.valid ? Math::Length(sourceBounds.max - center) : 0.0f;
			for(size_t i = 0; i < scene.nodes.count; ++i)
			{
				const ufbx_node* node = scene.nodes.data[i];
				const ufbx_light* source = node->light;
				if(source == nullptr) continue;
				const size_t stride = source->type == UFBX_LIGHT_DIRECTIONAL ? sizeof(DirectionalLight)
					: source->type == UFBX_LIGHT_POINT ? sizeof(PointLight)
					: source->type == UFBX_LIGHT_SPOT ? sizeof(SpotLight) : 0u;
				if(stride == 0u)
				{
					if(!warnedUnsupported) ReportModelWarning(filepath, "FBX area/volume lights are unsupported and were not imported");
					warnedUnsupported = true;
					continue;
				}
				if(budget != nullptr && !budget->AddBytes(1u, stride, "FBX source lights")) return false;
				const Math::float4x4 nodeToWorld = ToFloat4x4(node->node_to_world);
				const Math::float3 position = Math::TransformPoint(nodeToWorld, Math::float3(0, 0, 0));
				const Math::float3 localDirection(static_cast<float>(source->local_direction.x),
					static_cast<float>(source->local_direction.y), static_cast<float>(source->local_direction.z));
				const Math::float3 direction = Math::NormalizeOr(Math::TransformVector(nodeToWorld, localDirection), Math::float3(0, -1, 0));
				const Math::float3 color(static_cast<float>(source->color.x), static_cast<float>(source->color.y), static_cast<float>(source->color.z));
				const float intensity = static_cast<float>(source->intensity);
				if(!IsFinite(nodeToWorld) || !IsFinite(position) || !IsFinite(localDirection) || !IsFinite(direction)
					|| !IsFinite(color) || color.x < 0.0f || color.y < 0.0f || color.z < 0.0f
					|| !std::isfinite(intensity) || intensity < 0.0f)
					return ReportModelError(filepath, "FBX light contains invalid positions, color or intensity");
				const bool enabled = source->cast_light && node->visible;
				if(source->type == UFBX_LIGHT_DIRECTIONAL)
				{
					DirectionalLight light;
					light.enabled = enabled;
					// dyf stores the surface-to-light vector; FBX stores the emitted ray direction.
					light.direction = direction * -1.0f;
					light.color = color;
					light.intensity = intensity;
					light.castShadow = source->cast_shadows;
					light.shadowStrength = 1.0f;
					outModel.directionalLights.push_back(light);
					continue;
				}
				if(source->decay != UFBX_LIGHT_DECAY_QUADRATIC)
				{
					if(!warnedDecay) ReportModelWarning(filepath, "FBX non-quadratic light decay is approximated by the renderer's inverse-square attenuation");
					warnedDecay = true;
				}
				const double authoredRange = ufbx_find_bool(&source->props, "EnableFarAttenuation", false)
					? ufbx_find_real(&source->props, "FarAttenuationEnd", 0.0) : 0.0;
				if(!std::isfinite(authoredRange) || authoredRange < 0.0 || authoredRange > std::numeric_limits<float>::max())
					return ReportModelError(filepath, "FBX light contains an invalid far attenuation distance");
				// Without a source cutoff, cover the reference bounds and keep their geometry
				// inside the inner half of the renderer's finite attenuation window.
				const float range = authoredRange > 0.0 ? static_cast<float>(authoredRange)
					: 2.0f * std::max(Math::Length(position - center) + radius, 1.0f);
				if(!std::isfinite(range) || range <= 0.0f)
					return ReportModelError(filepath, "FBX light range cannot be represented");
				if(authoredRange == 0.0)
				{
					if(!warnedRange) ReportModelWarning(filepath, "FBX lights without a far cutoff use finite ranges derived from source bounds");
					warnedRange = true;
				}
				if(source->type == UFBX_LIGHT_POINT)
				{
					PointLight light;
					light.enabled = enabled; light.position = position; light.range = range;
					light.color = color; light.intensity = intensity;
					light.castShadow = source->cast_shadows; light.shadowStrength = 1.0f;
					outModel.pointLights.push_back(light);
				}
				else
				{
					constexpr double toRadians = 3.14159265358979323846 / 180.0;
					const float inner = static_cast<float>(source->inner_angle * toRadians);
					const float outer = static_cast<float>(source->outer_angle * toRadians);
					if(!std::isfinite(inner) || !std::isfinite(outer) || inner < 0.0f || outer < 0.0f)
						return ReportModelError(filepath, "FBX spot light contains invalid cone angles");
					SpotLight light;
					light.enabled = enabled; light.position = position; light.range = range;
					light.direction = direction; light.color = color; light.intensity = intensity;
					light.innerConeRadians = inner; light.outerConeRadians = outer;
					light.castShadow = source->cast_shadows; light.shadowStrength = 1.0f;
					outModel.spotLights.push_back(light);
				}
			}
			return true;
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
					filepath,
					"FBX texture count exceeds maxTextures");
			}
			outModel.textures.reserve(scene.textures.count);
			textureIndices.clear();
			decodedTextureBytes = 0u;
			std::map<std::filesystem::path, size_t> decodedExternalTextures;

			for(size_t textureIndex = 0; textureIndex < scene.textures.count; ++textureIndex)
			{
				const ufbx_texture* source = scene.textures.data[textureIndex];
				Image texture = {};
				if(source == nullptr)
				{
					return ReportModelError(
						filepath,
						"ufbx texture entry is null");
				}

				if(source->content.data != nullptr && source->content.size > 0u)
				{
					const uint64_t remainingDecodedBytes = decodedTextureBytes <= options.maxDecodedBytes
						? options.maxDecodedBytes - decodedTextureBytes
						: 0u;
					bool limitExceeded = false;
					if(!LoadImage(static_cast<const uint8_t*>(source->content.data), source->content.size, texture, remainingDecodedBytes, &limitExceeded))
					{
						return ReportModelError(
							filepath,
							limitExceeded
								? "embedded texture exceeds maxDecodedBytes"
								: "failed to decode embedded texture");
					}

					const uint64_t imageBytes = static_cast<uint64_t>(texture.GetPixels().size());
					if(imageBytes > options.maxDecodedBytes - decodedTextureBytes)
					{
						return ReportModelError(
							filepath,
							"embedded texture exceeds maxDecodedBytes");
					}
					decodedTextureBytes += imageBytes;
				}
				else
				{
					std::vector<std::filesystem::path> candidates;
					auto appendCandidates = [&](std::string filename) {
						// FBX commonly stores Windows paths even when loaded on another platform.
						std::replace(filename.begin(), filename.end(), '\\', '/');
						if(filename.empty()) return;
						const std::filesystem::path relativePath(filename);
						candidates.push_back(externalFileContext.modelDirectory / relativePath);
						const std::filesystem::path basename = relativePath.filename();
						if(basename.empty()) return;
						candidates.push_back(externalFileContext.modelDirectory / basename);
						candidates.push_back(externalFileContext.modelDirectory / "textures" / basename);
						candidates.push_back(externalFileContext.modelDirectory / "Textures" / basename);
					};
					appendCandidates(ToString(source->relative_filename));
					appendCandidates(ToString(source->filename));
					if(candidates.empty())
					{
						return ReportModelError(filepath, "ufbx external texture has no source path");
					}

					std::error_code pathError;
					for(const std::filesystem::path& candidate : candidates)
					{
						if(!IsPathInsideDirectory(externalFileContext.modelDirectory, candidate, pathError)) continue;
						if(!std::filesystem::is_regular_file(candidate, pathError) || pathError) continue;

						const std::filesystem::path canonicalPath =
							std::filesystem::weakly_canonical(candidate, pathError);
						if(pathError) continue;
						const uintmax_t fileBytes = std::filesystem::file_size(canonicalPath, pathError);
						if(pathError || fileBytes > std::numeric_limits<uint64_t>::max()) continue;
						const bool alreadyAccounted = externalFileContext.accountedPaths.find(canonicalPath)
							!= externalFileContext.accountedPaths.end();
						if(!alreadyAccounted)
						{
							const uint64_t sourceBytes = static_cast<uint64_t>(fileBytes);
							if(externalFileContext.sourceBytes > externalFileContext.sourceByteLimit
								|| sourceBytes > externalFileContext.sourceByteLimit - externalFileContext.sourceBytes)
							{
								return ReportModelError(
									filepath,
									"external texture exceeds maxSourceBytes");
							}
							externalFileContext.sourceBytes += sourceBytes;
							externalFileContext.accountedPaths.insert(canonicalPath);
						}
						const auto existing = decodedExternalTextures.find(canonicalPath);
						if(existing != decodedExternalTextures.end())
						{
							texture = outModel.textures[existing->second];
							break;
						}
						texture.SetSourcePath(canonicalPath.string());
						// Read only the accounted source size and use the allocation-bounded decoder.
						if(fileBytes > std::numeric_limits<int>::max()
							|| fileBytes > externalFileContext.sourceByteLimit)
							return ReportModelError(filepath, "external texture source size is invalid");
						std::ifstream stream(canonicalPath, std::ios::binary);
						std::vector<uint8_t> encoded(static_cast<size_t>(fileBytes));
						bool limitExceeded = false;
						if(stream.read(reinterpret_cast<char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()))
							&& stream.peek() == std::char_traits<char>::eof())
						{
							Image decoded;
							if(LoadImage(encoded.data(), encoded.size(), decoded,
								options.maxDecodedBytes - decodedTextureBytes, &limitExceeded))
							{
								decodedTextureBytes += decoded.GetPixels().size();
								decoded.SetSourcePath(canonicalPath.string());
								texture = std::move(decoded);
							}
							else if(limitExceeded)
								return ReportModelError(filepath, "external texture exceeds maxDecodedBytes");
						}
						// Missing/unsupported pixels remain invalid as before; asset creation will reject them.
						decodedExternalTextures.emplace(canonicalPath, outModel.textures.size());
						break;
					}
				}

				textureIndices[source] = static_cast<uint32_t>(outModel.textures.size());
				outModel.textures.push_back(std::move(texture));
			}
			return true;
		}

	}

		[[nodiscard]] bool LoadUfbxModel(const std::string& filepath, ModelData& outModel, const ModelLoadOptions& options,
			uint64_t* decodedBytes, const Math::Bounds3* lightOnlyBounds)
		{
			const bool lightsOnly = lightOnlyBounds != nullptr;
			if(decodedBytes) *decodedBytes=0;
			if(options.maxNodeDepth == 0u)
				return ReportModelError(
					filepath,
					"maxNodeDepth must be greater than zero");
			if(!std::isfinite(options.fbxBakeRate) || options.fbxBakeRate <= 0.0f)
				return ReportModelError(
					filepath,
					"FBX bake rate must be finite and greater than zero");
			if(!std::isfinite(options.fbxConstantTrackTolerance) || options.fbxConstantTrackTolerance < 0.0f)
				return ReportModelError(
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
			const auto sourcePath = std::filesystem::absolute(filepath, sourceSizeError);
			if(sourceSizeError) return ReportModelError(filepath, "failed to resolve ufbx source path");
			const uintmax_t mainSourceSize = std::filesystem::file_size(sourcePath, sourceSizeError);
			if(sourceSizeError || mainSourceSize > std::numeric_limits<uint64_t>::max())
			{
				return ReportModelError(
					filepath,
					"failed to query ufbx model source size");
			}
			if(mainSourceSize > options.maxSourceBytes)
				return ReportModelError(filepath, "model source size " + std::to_string(mainSourceSize)
					+ " exceeds limit " + std::to_string(options.maxSourceBytes));
			UfbxExternalFileContext externalFileContext{};
			externalFileContext.modelDirectory = sourcePath.parent_path();
			externalFileContext.sourceByteLimit = options.maxSourceBytes;
			externalFileContext.sourceBytes = static_cast<uint64_t>(mainSourceSize);
			ufbx_load_opts opts = {};
			opts.temp_allocator.allocator = sharedAllocator;
			opts.temp_allocator.memory_limit = parserByteLimit;
			opts.result_allocator.allocator = sharedAllocator;
			opts.result_allocator.memory_limit = parserByteLimit;
			opts.ignore_all_content = lightsOnly;
			opts.load_external_files = !lightsOnly;
			opts.open_main_file_with_default = true;
			opts.open_file_cb.fn = OpenRestrictedUfbxExternalFile;
			opts.open_file_cb.user = &externalFileContext;
			opts.generate_missing_normals = !lightsOnly;
			opts.node_depth_limit = options.maxNodeDepth;
			opts.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_HELPER_NODES;
			opts.scale_helper_name.data = "(scale helper)";
			opts.scale_helper_name.length = SIZE_MAX;

			ufbx_error error;
			ufbx_scene* scene = ufbx_load_file(sourcePath.string().c_str(), &opts, &error);
			if(externalFileContext.pathRejected || externalFileContext.sourceLimitExceeded)
			{
				if(scene != nullptr) ufbx_free_scene(scene);
				return ReportModelError(
					filepath,
					externalFileContext.pathRejected
						? "ufbx external file path escapes the model directory: " + externalFileContext.rejectedPath
						: "ufbx source bytes exceed maxSourceBytes while accounting for external files");
			}
			if(scene == nullptr)
			{
				if(allocatorBudget.limitExceeded
					|| error.type == UFBX_ERROR_MEMORY_LIMIT
					|| error.type == UFBX_ERROR_ALLOCATION_LIMIT)
				{
					return ReportModelError(
						filepath,
						"ufbx parser memory limit exceeded");
				}
				if(error.type == UFBX_ERROR_NODE_DEPTH_LIMIT)
				{
					return ReportModelError(
						filepath,
						"ufbx node hierarchy exceeds maxNodeDepth");
				}
				return ReportModelError(
					filepath,
					std::string("failed to load model through ufbx: ") + error.info);
			}
			std::unique_ptr<ufbx_scene, void(*)(ufbx_scene*)> sceneOwner(scene, ufbx_free_scene);
			if(lightsOnly)
			{
				ModelLoadBudget budget(filepath, options);
				if(!budget.CheckNodes(scene->nodes.count)) return false;
				outModel = {};
				if(!ImportUfbxLights(filepath, *scene, *lightOnlyBounds, outModel, &budget)) return false;
				if(decodedBytes != nullptr) *decodedBytes = budget.DecodedBytes();
				return true;
			}
			const double fbxBakeRate = static_cast<double>(options.fbxBakeRate);
			std::vector<size_t> sampleCounts;
			uint64_t geometryBytes = 0u;
			if(!ValidateFbxLoadLimits(filepath, *scene, options, fbxBakeRate, sampleCounts, geometryBytes)) return false;
			ModelLoadOptions textureOptions = options;
			textureOptions.maxDecodedBytes -= geometryBytes;
			outModel = {};
			std::map<const ufbx_texture*, uint32_t> textureIndices;
			uint64_t decodedTextureBytes = 0u;
			if(!LoadUfbxTextureAssets(
				filepath,
				*scene,
				textureOptions,
				externalFileContext,
				outModel,
				textureIndices,
				decodedTextureBytes)) return false;

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
						filepath,
						"ufbx node contains non-finite transform values");
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
						const double channelWeight = channel->weight;
						if(!std::isfinite(channelWeight))
						{
							return ReportModelError(
								filepath,
								"FBX blend channel weight is non-finite");
						}
						std::vector<float> defaultWeights;
						if(!EvaluateUfbxBlendKeyframeWeights(*channel, channelWeight, defaultWeights))
							return ReportModelError(filepath, "FBX blend coefficients are non-finite or exceed float range");
						for(size_t keyframeIndex = 0; keyframeIndex < channel->keyframes.count; ++keyframeIndex)
						{
							const ufbx_blend_keyframe& keyframe = channel->keyframes.data[keyframeIndex];
							if(keyframe.shape == nullptr || !std::isfinite(keyframe.target_weight))
							{
								return ReportModelError(
									filepath,
									"FBX blend keyframe is invalid");
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
							filepath,
							"FBX skin cluster has no valid bone node");
					}
					const Math::float4x4 inverseBind = ToFloat4x4(cluster->mesh_node_to_bone);
					if(!IsFinite(inverseBind))
					{
						return ReportModelError(
							filepath,
							"ufbx skin cluster contains a non-finite inverse-bind matrix");
					}
					skin.jointNodeIndices.push_back(joint->second);
					skin.inverseBindMatrices.push_back(inverseBind);
				}
				skinIndices[source] = static_cast<uint32_t>(outModel.skins.size());
				outModel.skins.push_back(std::move(skin));
			}

			ModelLoadBudget animationBudget(filepath, options);
			if(!animationBudget.AddBytes(geometryBytes, 1u, "FBX geometry and metadata")
				|| !animationBudget.AddBytes(decodedTextureBytes, 1u, "FBX decoded textures")) return false;
			outModel.animations.reserve(scene->anim_stacks.count);
			for(size_t stackIndex = 0; stackIndex < scene->anim_stacks.count; ++stackIndex)
			{
				const ufbx_anim_stack* stack = scene->anim_stacks.data[stackIndex];
				if(stack->anim == nullptr) continue;
				const double beginTime = stack->time_begin;
				const double duration = std::max(0.0, stack->time_end - beginTime);
				const size_t sampleCount = sampleCounts[stackIndex];
				ufbx_bake_opts bakeOptions = {};
				// Share the bounded parser allocator while the source scene is alive.
				bakeOptions.temp_allocator = opts.temp_allocator;
				bakeOptions.result_allocator = opts.result_allocator;
				bakeOptions.resample_rate = fbxBakeRate;
				bakeOptions.key_reduction_enabled = options.fbxConstantTrackTolerance > 0.0f;
				bakeOptions.key_reduction_rotation = true;
				bakeOptions.key_reduction_threshold = options.fbxConstantTrackTolerance;
				ufbx_error bakeError = {};
				std::unique_ptr<ufbx_baked_anim, void(*)(ufbx_baked_anim*)> baked(
					ufbx_bake_anim(scene, stack->anim, &bakeOptions, &bakeError), ufbx_free_baked_anim);
				if(!baked)
					return ReportModelError(filepath, allocatorBudget.limitExceeded
						? "ufbx animation bake exceeds maxParserBytes"
						: std::string("failed to bake FBX animation: ") + bakeError.info);

				AnimationClip clip;
				clip.name = stack->name.length > 0 ? stack->name.data : ("Animation_" + std::to_string(stackIndex));
				clip.duration = static_cast<float>(duration);
				if(!animationBudget.AddBytes(baked->nodes.count, sizeof(NodeAnimationTrack), "FBX node animation tracks")) return false;
				clip.tracks.reserve(baked->nodes.count);
				const float tolerance = options.fbxConstantTrackTolerance;
				auto importVec3Keys = [&](const ufbx_baked_vec3_list& source, const Math::float3& bind,
					std::vector<Vec3Key>& destination) -> bool {
					if(source.count == 0u) return true;
					const auto convert = [](const ufbx_vec3& value) {
						return Math::float3(static_cast<float>(value.x), static_cast<float>(value.y), static_cast<float>(value.z));
					};
					const Math::float3 first = convert(source.data[0].value);
					bool constant = true;
					for(size_t i = 0; i < source.count; ++i)
					{
						const Math::float3 value = convert(source.data[i].value);
						const float time = static_cast<float>(source.data[i].time - beginTime);
						if(!IsFinite(value) || !std::isfinite(time))
							return ReportModelError(filepath, "FBX baked vector key contains non-finite values");
						if(!NearlyEqual(first, value, tolerance)) constant = false;
					}
					if(constant && NearlyEqual(first, bind, tolerance)) return true;
					const size_t count = constant ? 1u : source.count;
					if(!animationBudget.AddAnimationKeys(count)
						|| !animationBudget.AddBytes(count, sizeof(Vec3Key), "FBX baked vector keys")) return false;
					destination.reserve(count);
					for(size_t i = 0; i < count; ++i)
					{
						Vec3Key key;
						key.time = static_cast<float>(source.data[i].time - beginTime);
						key.value = convert(source.data[i].value);
						destination.push_back(key);
					}
					return true;
				};
				auto importRotationKeys = [&](const ufbx_baked_quat_list& source, const Math::quat& bind,
					std::vector<QuatKey>& destination) -> bool {
					if(source.count == 0u) return true;
					const auto convert = [](const ufbx_quat& value) {
						return Math::quat(static_cast<float>(value.x), static_cast<float>(value.y),
							static_cast<float>(value.z), static_cast<float>(value.w));
					};
					const Math::quat first = convert(source.data[0].value);
					bool constant = true;
					for(size_t i = 0; i < source.count; ++i)
					{
						const Math::quat value = convert(source.data[i].value);
						const float time = static_cast<float>(source.data[i].time - beginTime);
						if(!IsFinite(value) || !std::isfinite(time))
							return ReportModelError(filepath, "FBX baked rotation key contains non-finite values");
						if(!NearlyEqualRotation(first, value, tolerance)) constant = false;
					}
					if(constant && NearlyEqualRotation(first, bind, tolerance)) return true;
					const size_t count = constant ? 1u : source.count;
					if(!animationBudget.AddAnimationKeys(count)
						|| !animationBudget.AddBytes(count, sizeof(QuatKey), "FBX baked rotation keys")) return false;
					destination.reserve(count);
					for(size_t i = 0; i < count; ++i)
					{
						QuatKey key;
						key.time = static_cast<float>(source.data[i].time - beginTime);
						key.value = convert(source.data[i].value);
						destination.push_back(key);
					}
					return true;
				};
				for(size_t nodeIndex = 0; nodeIndex < baked->nodes.count; ++nodeIndex)
				{
					const ufbx_baked_node& source = baked->nodes.data[nodeIndex];
					if(source.typed_id >= outModel.nodes.size())
						return ReportModelError(filepath, "FBX baked animation refers to an invalid node");
					NodeAnimationTrack track;
					track.nodeIndex = source.typed_id;
					const NodeTransform& bind = outModel.nodes[source.typed_id].bindTransform;
					if(!importVec3Keys(source.translation_keys, bind.translation, track.translations)
						|| !importRotationKeys(source.rotation_keys, bind.rotation, track.rotations)
						|| !importVec3Keys(source.scale_keys, bind.scale, track.scales)) return false;
					if(!track.translations.empty() || !track.rotations.empty() || !track.scales.empty())
						clip.tracks.push_back(std::move(track));
				}
				for(const UfbxBlendChannelBinding& binding : blendChannelBindings)
				{
					const ufbx_baked_element* bakedChannel = ufbx_find_baked_element_by_element_id(baked.get(), binding.channel->element_id);
					bool animatedWeight = false;
					if(bakedChannel != nullptr)
						for(size_t i = 0; i < bakedChannel->props.count; ++i)
							if(ToString(bakedChannel->props.data[i].name) == "DeformPercent"
								&& bakedChannel->props.data[i].keys.count != 0u) animatedWeight = true;
					if(!animatedWeight) continue;
					if(!animationBudget.AddBytes(binding.targetCount, sizeof(MorphWeightTrack), "FBX morph animation tracks")) return false;
					std::vector<MorphWeightTrack> tracks(binding.targetCount);
					std::vector<uint8_t> constant(binding.targetCount, 1u);
					std::vector<float> firstWeights;
					// Detect constants without allocating sampleCount*targetCount keys.
					for(size_t sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
					{
						const double relativeTime = std::min(duration, static_cast<double>(sampleIndex) / fbxBakeRate);
						const double channelWeight = ufbx_evaluate_blend_weight(
							stack->anim,
							binding.channel,
							beginTime + relativeTime);
						if(!std::isfinite(channelWeight))
						{
							return ReportModelError(
								filepath,
								"FBX blend animation sample is non-finite");
						}
						std::vector<float> effectiveWeights;
						if(!EvaluateUfbxBlendKeyframeWeights(*binding.channel, channelWeight, effectiveWeights))
							return ReportModelError(filepath, "FBX animated blend coefficients are non-finite or exceed float range");
						if(effectiveWeights.size() != binding.targetCount) return false;
						if(sampleIndex == 0u) firstWeights = std::move(effectiveWeights);
						else for(uint32_t targetOffset = 0u; targetOffset < binding.targetCount; ++targetOffset)
							if(std::fabs(effectiveWeights[targetOffset] - firstWeights[targetOffset]) > tolerance)
								constant[targetOffset] = 0u;
					}
					bool needsSamples = false;
					for(uint32_t targetOffset = 0u; targetOffset < binding.targetCount; ++targetOffset)
					{
						MorphWeightTrack& track = tracks[targetOffset];
						track.nodeIndex = binding.nodeIndex;
						track.targetIndex = binding.firstTargetIndex + targetOffset;
						const auto& bindWeights = outModel.nodes[binding.nodeIndex].morphWeights;
						if(track.targetIndex >= bindWeights.size()) return false;
						if(constant[targetOffset] && std::fabs(firstWeights[targetOffset] - bindWeights[track.targetIndex]) <= tolerance) continue;
						const size_t count = constant[targetOffset] ? 1u : sampleCount;
						if(!animationBudget.AddAnimationKeys(count)
							|| !animationBudget.AddBytes(count, sizeof(FloatKey), "FBX baked morph keys")) return false;
						track.weights.reserve(count);
						if(constant[targetOffset])
						{
							FloatKey key;
							key.value = firstWeights[targetOffset];
							track.weights.push_back(key);
						}
						else needsSamples = true;
					}
					if(needsSamples)
						for(size_t sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
						{
							const double relativeTime = std::min(duration, static_cast<double>(sampleIndex) / fbxBakeRate);
							const double channelWeight = ufbx_evaluate_blend_weight(stack->anim, binding.channel, beginTime + relativeTime);
							std::vector<float> effectiveWeights;
							if(!std::isfinite(channelWeight)
								|| !EvaluateUfbxBlendKeyframeWeights(*binding.channel, channelWeight, effectiveWeights)
								|| effectiveWeights.size() != binding.targetCount)
								return ReportModelError(filepath, "FBX animated blend coefficients are invalid");
							for(uint32_t targetOffset = 0u; targetOffset < binding.targetCount; ++targetOffset)
							{
								if(constant[targetOffset]) continue;
								FloatKey key;
								key.time = static_cast<float>(relativeTime);
								key.value = effectiveWeights[targetOffset];
								tracks[targetOffset].weights.push_back(key);
							}
						}
					for(MorphWeightTrack& track : tracks)
					{
						if(!track.weights.empty()) clip.morphTracks.push_back(std::move(track));
					}
				}
				outModel.animations.push_back(std::move(clip));
			}
			geometryBytes = animationBudget.DecodedBytes() - decodedTextureBytes;
			textureOptions.maxDecodedBytes = options.maxDecodedBytes - geometryBytes;

			outModel.materials.reserve(scene->materials.count);
			std::map<const ufbx_material*, uint32_t> materialIndices;
			auto textureIndex = [&](const ufbx_material_map& map) -> uint32_t {
				const ufbx_texture* source = map.texture;
				if(source == nullptr || !map.texture_enabled) return UINT32_MAX;
				const auto texture = textureIndices.find(source);
				return texture != textureIndices.end() ? texture->second : UINT32_MAX;
			};
			auto applyTextureIndex = [&](ModelMaterialInfo& material, MaterialTextureKind kind, uint32_t index) {
				if(index >= outModel.textures.size()) return;
				const Image& asset = outModel.textures[index];
				if(asset.GetSourcePath().empty() && asset.GetPixels().empty()) return;
				const uint32_t slot = static_cast<uint32_t>(kind);
				material.textureIndices[slot] = index;
				material.texturePaths[slot] = asset.GetSourcePath();
				material.hasTexture[slot] = true;
			};
			auto applyTexture = [&](ModelMaterialInfo& material, MaterialTextureKind kind, const ufbx_material_map& map) {
				applyTextureIndex(material, kind, textureIndex(map));
			};
			// Scalar FBX maps are independent images. Pack their red channels into the
			// renderer's G=roughness/B=metallic layout instead of discarding either map.
			std::map<std::array<uint32_t, 4>, uint32_t> packedScalarTextures;
			auto packScalarTextures = [&](uint32_t roughnessIndex, uint32_t metallicIndex,
				bool glossiness, float glossinessFactor, uint32_t& result) -> bool {
				uint32_t factorBits = 0;
				std::memcpy(&factorBits, &glossinessFactor, sizeof(factorBits));
				const std::array<uint32_t, 4> key = {roughnessIndex, metallicIndex, glossiness ? 1u : 0u, factorBits};
				const auto cached = packedScalarTextures.find(key);
				if(cached != packedScalarTextures.end()) { result = cached->second; return true; }
				const Image* roughness = roughnessIndex < outModel.textures.size() ? &outModel.textures[roughnessIndex] : nullptr;
				const Image* metallic = metallicIndex < outModel.textures.size() ? &outModel.textures[metallicIndex] : nullptr;
				if((roughness != nullptr && !roughness->IsValid()) || (metallic != nullptr && !metallic->IsValid()))
					return ReportModelError(filepath, "cannot pack an undecoded FBX scalar texture");
				const uint32_t width = std::max(roughness != nullptr ? roughness->GetWidth() : 1u,
					metallic != nullptr ? metallic->GetWidth() : 1u);
				const uint32_t height = std::max(roughness != nullptr ? roughness->GetHeight() : 1u,
					metallic != nullptr ? metallic->GetHeight() : 1u);
				const uint64_t pixelCount = uint64_t(width) * height;
				if(pixelCount > UINT64_MAX / 4u || pixelCount * 4u > SIZE_MAX
					|| decodedTextureBytes > textureOptions.maxDecodedBytes
					|| pixelCount * 4u > textureOptions.maxDecodedBytes - decodedTextureBytes)
					return ReportModelError(filepath, "packed FBX texture exceeds maxDecodedBytes");
				if(outModel.textures.size() >= options.maxTextures || outModel.textures.size() >= UINT32_MAX)
					return ReportModelError(filepath, "packed FBX texture exceeds maxTextures");
				const auto sampleScalar = [width, height](const Image* image, uint32_t x, uint32_t y) {
					if(image == nullptr) return uint8_t(255);
					const uint32_t sx = static_cast<uint32_t>(uint64_t(x) * image->GetWidth() / width);
					const uint32_t sy = static_cast<uint32_t>(uint64_t(y) * image->GetHeight() / height);
					return image->GetPixels()[(size_t(sy) * image->GetWidth() + sx) * 4u];
				};
				std::vector<uint8_t> pixels(static_cast<size_t>(pixelCount * 4u), 255u);
				for(uint32_t y = 0; y < height; ++y)
					for(uint32_t x = 0; x < width; ++x)
					{
						const size_t offset = (size_t(y) * width + x) * 4u;
						const uint8_t scalar = sampleScalar(roughness, x, y);
						pixels[offset + 1u] = glossiness
							? static_cast<uint8_t>(std::lround(255.0f * (1.0f - std::clamp(float(scalar) / 255.0f * glossinessFactor, 0.0f, 1.0f))))
							: scalar;
						pixels[offset + 2u] = sampleScalar(metallic, x, y);
					}
				decodedTextureBytes += pixelCount * 4u;
				result = static_cast<uint32_t>(outModel.textures.size());
				outModel.textures.emplace_back(width, height, std::move(pixels), ColorSpace::Linear);
				packedScalarTextures.emplace(key, result);
				return true;
			};
			for(size_t i = 0; i < scene->materials.count; ++i)
			{
				ufbx_material* mat = scene->materials.data[i];
				ModelMaterialInfo material = {};
				material.name = ToString(mat->name);
				const float baseFactor = static_cast<float>(mat->pbr.base_factor.value_real);
				const bool legacyTransparency = mat->shader_type == UFBX_SHADER_FBX_LAMBERT
					|| mat->shader_type == UFBX_SHADER_FBX_PHONG;
				float legacyTransmission = 0.0f;
				if(legacyTransparency)
				{
					const ufbx_vec3& color = mat->fbx.transparency_color.value_vec3;
					const double amount = mat->fbx.transparency_color.has_value
						? std::max({color.x, color.y, color.z}) : 1.0;
					legacyTransmission = static_cast<float>(mat->fbx.transparency_factor.value_real * amount);
				}
				float opacity = mat->pbr.opacity.has_value ? static_cast<float>(mat->pbr.opacity.value_real) : 1.0f;
				if(mat->pbr.base_color.value_components == 4)
					opacity *= static_cast<float>(mat->pbr.base_color.value_vec4.w);
				if(!mat->pbr.opacity.has_value && legacyTransparency)
					opacity *= 1.0f - legacyTransmission;
				material.material.baseColor = Math::float4(
					static_cast<float>(mat->pbr.base_color.value_vec3.x) * baseFactor,
					static_cast<float>(mat->pbr.base_color.value_vec3.y) * baseFactor,
					static_cast<float>(mat->pbr.base_color.value_vec3.z) * baseFactor,
					opacity);
				const bool glossiness = mat->features.roughness_as_glossiness.enabled;
				const ufbx_material_map& roughnessMap = glossiness ? mat->pbr.glossiness : mat->pbr.roughness;
				const uint32_t roughnessIndex = textureIndex(roughnessMap);
				const uint32_t metallicIndex = textureIndex(mat->pbr.metalness);
				material.material.metallicFactor = mat->pbr.metalness.has_value
					? static_cast<float>(mat->pbr.metalness.value_real) : metallicIndex != UINT32_MAX ? 1.0f : 0.0f;
				material.material.roughnessFactor = roughnessMap.has_value
					? static_cast<float>(mat->pbr.roughness.value_real) : roughnessIndex != UINT32_MAX ? 1.0f : 0.5f;
				const float emissiveFactor = static_cast<float>(mat->pbr.emission_factor.value_real);
				material.material.emissiveColor = Math::float3(
					static_cast<float>(mat->pbr.emission_color.value_vec3.x) * emissiveFactor,
					static_cast<float>(mat->pbr.emission_color.value_vec3.y) * emissiveFactor,
					static_cast<float>(mat->pbr.emission_color.value_vec3.z) * emissiveFactor);
				material.material.doubleSided = mat->features.double_sided.enabled;
				material.material.transmissionFactor = legacyTransparency ? legacyTransmission
					: static_cast<float>(mat->pbr.transmission_factor.value_real);
				if(mat->pbr.specular_ior.has_value)
					material.material.indexOfRefraction = static_cast<float>(mat->pbr.specular_ior.value_real);
				if(mat->pbr.transmission_depth.has_value && mat->pbr.transmission_depth.value_real > 0.0)
				{
					material.material.attenuationDistance = static_cast<float>(mat->pbr.transmission_depth.value_real);
					material.material.attenuationColor = Math::float3(
						static_cast<float>(mat->pbr.transmission_color.value_vec3.x),
						static_cast<float>(mat->pbr.transmission_color.value_vec3.y),
						static_cast<float>(mat->pbr.transmission_color.value_vec3.z));
				}
				if(!std::isfinite(material.material.baseColor.x)
					|| !std::isfinite(material.material.baseColor.y)
					|| !std::isfinite(material.material.baseColor.z)
					|| !std::isfinite(opacity)
					|| !std::isfinite(material.material.metallicFactor)
					|| !std::isfinite(material.material.roughnessFactor)
					|| !IsFinite(material.material.emissiveColor)
					|| !std::isfinite(material.material.transmissionFactor)
					|| !std::isfinite(material.material.indexOfRefraction)
					|| !std::isfinite(material.material.attenuationDistance)
					|| !IsFinite(material.material.attenuationColor))
				{
					return ReportModelError(
						filepath,
						"ufbx material contains non-finite values");
				}

				const ufbx_material_map& baseColorTexture = mat->pbr.base_color.texture != nullptr
					? mat->pbr.base_color
					: mat->fbx.diffuse_color;
				applyTexture(material, MaterialTextureKind::BaseColor, baseColorTexture);
				material.material.baseColor.w = std::clamp(opacity, 0.0f, 1.0f);
				material.material.metallicFactor = std::clamp(material.material.metallicFactor, 0.0f, 1.0f);
				material.material.roughnessFactor = std::clamp(material.material.roughnessFactor, 0.0f, 1.0f);
				material.material.transmissionFactor = std::clamp(material.material.transmissionFactor, 0.0f, 1.0f);
				if(material.material.indexOfRefraction <= 0.0f)
					return ReportModelError(filepath, "ufbx material index of refraction must be positive");
				const ufbx_material_map& specularTexture = textureIndex(mat->fbx.specular_color) != UINT32_MAX
					? mat->fbx.specular_color : mat->pbr.specular_color;
				if(options.fbxSpecularTextureIsOrm && textureIndex(specularTexture) != UINT32_MAX)
				{
					applyTexture(material, MaterialTextureKind::MetallicRoughness, specularTexture);
					applyTexture(material, MaterialTextureKind::Occlusion, specularTexture);
					material.material.metallicFactor = 1.0f;
					material.material.roughnessFactor = 1.0f;
				}
				else if(roughnessIndex != UINT32_MAX || metallicIndex != UINT32_MAX)
				{
					const float glossinessFactor = glossiness && roughnessMap.has_value
						? static_cast<float>(roughnessMap.value_real) : 1.0f;
					if(!std::isfinite(glossinessFactor))
						return ReportModelError(filepath, "ufbx glossiness factor is non-finite");
					uint32_t packedIndex = UINT32_MAX;
					if(!packScalarTextures(roughnessIndex, metallicIndex, glossiness, glossinessFactor, packedIndex)) return false;
					applyTextureIndex(material, MaterialTextureKind::MetallicRoughness, packedIndex);
					if(glossiness) material.material.roughnessFactor = 1.0f;
				}
				const ufbx_material_map& normalTexture = mat->pbr.normal_map.texture != nullptr
					? mat->pbr.normal_map
					: (mat->fbx.normal_map.texture != nullptr
						? mat->fbx.normal_map
						: mat->fbx.bump);
				applyTexture(material, MaterialTextureKind::Normal, normalTexture);
				applyTexture(material, MaterialTextureKind::Occlusion, mat->pbr.ambient_occlusion);
				const ufbx_material_map& emissiveTexture = mat->pbr.emission_color.texture != nullptr
					? mat->pbr.emission_color
					: mat->fbx.emission_color;
				applyTexture(material, MaterialTextureKind::Emissive, emissiveTexture);
				if(options.fbxBaseColorTextureHasOpacity)
				{
					const uint32_t baseIndex = material.textureIndices[ToIndex(MaterialTextureKind::BaseColor)];
					if(baseIndex < outModel.textures.size())
					{
						const std::vector<uint8_t>& pixels = outModel.textures[baseIndex].GetPixels();
						for(size_t pixel = 3; pixel < pixels.size(); pixel += 4)
						{
							if(pixels[pixel] == 255u) continue;
							material.material.alphaMode = pixels[pixel] == 0u
								? MaterialAlphaMode::Mask : MaterialAlphaMode::Blend;
							if(material.material.alphaMode == MaterialAlphaMode::Blend) break;
						}
					}
				}
				if(material.material.baseColor.w < 1.0f || material.material.transmissionFactor > 0.0f)
					material.material.alphaMode = MaterialAlphaMode::Blend;
				if(material.material.transmissionFactor > 0.0f) material.material.doubleSided = true;
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

			Math::float3 lightBoundsMin = Math::float3(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
			Math::float3 lightBoundsMax = Math::float3(-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());
			bool hasLightBounds = false;
			std::map<std::string, std::vector<std::pair<uint32_t, uint32_t>>> sharedGeometry;
			for(size_t ni = 0; ni < scene->nodes.count; ++ni)
			{
				ufbx_node* node = scene->nodes.data[ni];
				if(node->mesh == nullptr) continue;

				ufbx_mesh* sourceMesh = node->mesh;
				const auto modelNode = nodeIndices.find(node);
				if(modelNode == nodeIndices.end()) return false;
				const bool localGeometry = UsesLocalFbxGeometry(*scene, *node);
				const bool shareGeometry = CanShareFbxGeometry(*scene, *node);
				const ufbx_skin_deformer* skinDeformer = sourceMesh->skin_deformers.count > 0
					? sourceMesh->skin_deformers.data[0]
					: nullptr;
				uint32_t modelSkinIndex = UINT32_MAX;
				if(skinDeformer != nullptr)
				{
					for(size_t deformerIndex = 1u; deformerIndex < sourceMesh->skin_deformers.count; ++deformerIndex)
					{
						const ufbx_skin_deformer* additional = sourceMesh->skin_deformers.data[deformerIndex];
						if(additional == nullptr || !AreCompatibleSkinDeformers(*skinDeformer, *additional))
						{
							return ReportModelError(
								filepath,
								"FBX mesh uses incompatible multiple skin deformers");
						}
					}
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
						if(localGeometry) mesh.nodeIndex = modelNode->second;
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
				const ufbx_matrix& vertexMatrix = localGeometry
					? node->geometry_to_node : node->geometry_to_world;
				const ufbx_matrix normalMatrix = ufbx_matrix_for_normals(&vertexMatrix);
				const Math::float4x4 nodeToWorld = ToFloat4x4(node->node_to_world);
				const bool hasAuthoredTangents = sourceMesh->vertex_tangent.exists;
				const auto geometryKey = shareGeometry ? FbxGeometryKey(*node) : std::string{};
				const auto existingGeometry = shareGeometry ? sharedGeometry.find(geometryKey) : sharedGeometry.end();
				if(existingGeometry != sharedGeometry.end())
				{
					for(const auto& part : existingGeometry->second)
					{
						if(outModel.meshes.size() >= UINT32_MAX)
							return ReportModelError(filepath, "FBX mesh instance count exceeds the supported index range");
						if(options.importLights && scene->lights.count != 0u)
						{
							for(const auto& vertex : outModel.meshes[part.second].mesh.vertices)
							{
								const auto world = Math::TransformPoint(nodeToWorld, vertex.position);
								if(!IsFinite(world)) return ReportModelError(filepath, "FBX light bounds contain non-finite positions");
								lightBoundsMin.x = std::min(lightBoundsMin.x, world.x);
								lightBoundsMin.y = std::min(lightBoundsMin.y, world.y);
								lightBoundsMin.z = std::min(lightBoundsMin.z, world.z);
								lightBoundsMax.x = std::max(lightBoundsMax.x, world.x);
								lightBoundsMax.y = std::max(lightBoundsMax.y, world.y);
								lightBoundsMax.z = std::max(lightBoundsMax.z, world.z);
								hasLightBounds = true;
							}
						}
						auto& reference = getMesh(part.first);
						reference.geometryIndex = part.second;
						outModel.meshes.push_back(std::move(reference));
					}
					continue;
				}

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
								filepath,
								"ufbx vertex position contains non-finite values");
						}
						if(options.importLights && scene->lights.count != 0u)
						{
							const Math::float3 world = localGeometry
								? Math::TransformPoint(nodeToWorld, vertex.position) : vertex.position;
							if(!IsFinite(world)) return ReportModelError(filepath, "FBX light bounds contain non-finite positions");
							lightBoundsMin.x = std::min(lightBoundsMin.x, world.x);
							lightBoundsMin.y = std::min(lightBoundsMin.y, world.y);
							lightBoundsMin.z = std::min(lightBoundsMin.z, world.z);
							lightBoundsMax.x = std::max(lightBoundsMax.x, world.x);
							lightBoundsMax.y = std::max(lightBoundsMax.y, world.y);
							lightBoundsMax.z = std::max(lightBoundsMax.z, world.z);
							hasLightBounds = true;
						}

						double normalDeltaScale = 1.0;
						if(sourceMesh->vertex_normal.exists)
						{
							const ufbx_vec3 localNormal = ufbx_get_vertex_vec3(&sourceMesh->vertex_normal, index);
							const ufbx_vec3 worldNormal = ufbx_transform_direction(&normalMatrix, localNormal);
							const Math::float3 rawNormal(static_cast<float>(worldNormal.x), static_cast<float>(worldNormal.y), static_cast<float>(worldNormal.z));
							if(!IsFinite(rawNormal)) return ReportModelError(filepath, "ufbx vertex normal contains non-finite values");
							const double lengthSquared = double(rawNormal.x) * rawNormal.x
								+ double(rawNormal.y) * rawNormal.y + double(rawNormal.z) * rawNormal.z;
							if(lengthSquared > 1.0e-8f) normalDeltaScale = 1.0 / std::sqrt(lengthSquared);
							vertex.normal = NormalizeOr(
								rawNormal,
								Math::float3(0.0f, 1.0f, 0.0f));
							if(!IsFinite(vertex.normal))
							{
								return ReportModelError(
									filepath,
									"ufbx vertex normal contains non-finite values");
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
									filepath,
									"ufbx vertex tangent contains non-finite values");
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
										filepath,
										"ufbx vertex bitangent contains non-finite values");
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
									filepath,
									"ufbx vertex UV contains non-finite values");
							}
						}

						if(skinDeformer != nullptr)
						{
							std::map<uint32_t, float> combinedInfluences;
							for(size_t deformerIndex = 0u; deformerIndex < sourceMesh->skin_deformers.count; ++deformerIndex)
							{
								const ufbx_skin_deformer* deformer = sourceMesh->skin_deformers.data[deformerIndex];
								if(deformer == nullptr || logicalVertex >= deformer->vertices.count) return false;
								const ufbx_skin_vertex& skinVertex = deformer->vertices.data[logicalVertex];
								if(static_cast<size_t>(skinVertex.weight_begin) + skinVertex.num_weights > deformer->weights.count)
									return false;
								for(uint32_t weightIndex = 0; weightIndex < skinVertex.num_weights; ++weightIndex)
								{
									const ufbx_skin_weight& weight =
										deformer->weights.data[skinVertex.weight_begin + weightIndex];
									if(weight.cluster_index >= deformer->clusters.count) return false;
									if(!std::isfinite(weight.weight) || weight.weight < 0.0
										|| weight.weight > std::numeric_limits<float>::max())
									{
										return ReportModelError(
											filepath,
											"ufbx skin weight must be finite, non-negative and representable as float");
									}
									float& combined = combinedInfluences[weight.cluster_index];
									const double sum = double(combined) + weight.weight;
									if(sum > std::numeric_limits<float>::max())
										return ReportModelError(filepath, "ufbx combined skin weight exceeds float range");
									combined = static_cast<float>(sum);
								}
							}
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
								const float dqBlendWeight = static_cast<float>(skinDeformer->vertices.data[logicalVertex].dq_weight);
								if(!std::isfinite(dqBlendWeight) || dqBlendWeight < 0.0f || dqBlendWeight > 1.0f)
								{
									return ReportModelError(
										filepath,
										"FBX dual-quaternion blend weight must be finite and in [0, 1]");
								}
								influence.dqBlendWeight = dqBlendWeight;
							}
							mesh.skinInfluences.push_back(influence);
							if(truncated && !warnedInfluenceTruncation)
							{
								ReportModelWarning(
									filepath,
									"FBX vertex influences were truncated to the four strongest joints");
								warnedInfluenceTruncation = true;
							}
						}

						for(size_t targetIndex = 0; targetIndex < mesh.morphTargets.size(); ++targetIndex)
						{
							const ufbx_blend_shape* shape = morphSourcesByNode[ni][targetIndex].shape;
							const uint32_t offsetIndex = ufbx_get_blend_shape_offset_index(shape, logicalVertex);
							const ufbx_real offsetWeight = offsetIndex < shape->offset_weights.count
								? shape->offset_weights.data[offsetIndex] : 1.0;
							ufbx_vec3 sourcePositionDelta = {};
							if(offsetIndex != UFBX_NO_INDEX && offsetIndex < shape->position_offsets.count)
								sourcePositionDelta = shape->position_offsets.data[offsetIndex];
							sourcePositionDelta.x *= offsetWeight;
							sourcePositionDelta.y *= offsetWeight;
							sourcePositionDelta.z *= offsetWeight;
							const ufbx_vec3 transformedPositionDelta =
								ufbx_transform_direction(&vertexMatrix, sourcePositionDelta);
							const Math::float3 positionDelta(
								static_cast<float>(transformedPositionDelta.x),
								static_cast<float>(transformedPositionDelta.y),
								static_cast<float>(transformedPositionDelta.z));
							if(!IsFinite(positionDelta))
							{
								return ReportModelError(
									filepath,
									"FBX blend shape contains non-finite position deltas");
							}
							MorphTarget& target = mesh.morphTargets[targetIndex];
							target.positionDeltas.push_back(positionDelta);

							if(shape->normal_offsets.count > 0u)
							{
								ufbx_vec3 sourceNormalDelta = {};
								if(offsetIndex != UFBX_NO_INDEX && offsetIndex < shape->normal_offsets.count)
								{
									sourceNormalDelta = shape->normal_offsets.data[offsetIndex];
									sourceNormalDelta.x *= offsetWeight;
									sourceNormalDelta.y *= offsetWeight;
									sourceNormalDelta.z *= offsetWeight;
								}
								const ufbx_vec3 transformedNormalDelta =
									ufbx_transform_direction(&normalMatrix, sourceNormalDelta);
								const Math::float3 normalDelta(
									static_cast<float>(transformedNormalDelta.x * normalDeltaScale),
									static_cast<float>(transformedNormalDelta.y * normalDeltaScale),
									static_cast<float>(transformedNormalDelta.z * normalDeltaScale));
								if(!IsFinite(normalDelta))
								{
									return ReportModelError(
										filepath,
										"FBX blend shape contains non-finite normal deltas");
								}
								target.normalDeltas.push_back(normalDelta);
							}
						}

						mesh.mesh.indices.push_back(static_cast<uint32_t>(mesh.mesh.vertices.size()));
						mesh.mesh.vertices.push_back(vertex);
					}
				}

				std::vector<std::pair<uint32_t, uint32_t>> geometryParts;
				for(auto& [materialIndex, mesh] : meshesByMaterial)
				{
					if(mesh.mesh.indices.empty()) continue;
					if(ufbx_matrix_determinant(&vertexMatrix) < 0.0)
						for(size_t index = 0; index < mesh.mesh.indices.size(); index += 3)
							std::swap(mesh.mesh.indices[index + 1], mesh.mesh.indices[index + 2]);
					if(!hasAuthoredTangents && !CalculateTangents(mesh.mesh))
						return ReportModelError(filepath, "ufbx generated normal or tangent is non-finite");
					if(outModel.meshes.size() >= UINT32_MAX)
						return ReportModelError(filepath, "FBX mesh instance count exceeds the supported index range");
					if(shareGeometry) geometryParts.emplace_back(materialIndex, static_cast<uint32_t>(outModel.meshes.size()));
					outModel.meshes.push_back(std::move(mesh));
				}
				if(shareGeometry) sharedGeometry.emplace(geometryKey, std::move(geometryParts));
			}

			if(options.importLights)
			{
				Math::Bounds3 sourceBounds;
				sourceBounds.min = lightBoundsMin;
				sourceBounds.max = lightBoundsMax;
				sourceBounds.valid = hasLightBounds;
				if(!ImportUfbxLights(filepath, *scene, sourceBounds, outModel)) return false;
			}
			if(decodedBytes) *decodedBytes=geometryBytes+decodedTextureBytes;
			return !outModel.meshes.empty();
		}
}
