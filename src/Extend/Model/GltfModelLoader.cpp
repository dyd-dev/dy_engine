#include "ModelLoader.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <utility>
#include <vector>


#include <fastgltf/core.hpp>
#include <fastgltf/math.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

template<class... Ts> struct dy_gltf_visitor : Ts... { using Ts::operator()...; };
template<class... Ts> dy_gltf_visitor(Ts...) -> dy_gltf_visitor<Ts...>;

namespace dyf
{
	namespace
	{
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
			const std::string& filepath)
		{
			const size_t texCoordIndex = textureInfo.transform != nullptr
				&& textureInfo.transform->texCoordIndex.has_value()
				? textureInfo.transform->texCoordIndex.value()
				: textureInfo.texCoordIndex;
			if(texCoordIndex != 0u)
			{
				ReportModelWarning(
					filepath,
					"texture references an unsupported texture coordinate set; texture disabled");
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
			if(const auto* filePath = std::get_if<fastgltf::sources::URI>(&image.data))
				SetTexturePath(material, kind, (basePath / filePath->uri.fspath()).string());
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
				Image textureAsset;
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
						textureAsset.SetSourcePath((basePath / source.uri.fspath()).string());
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
				if(!validSource)
				{
					return ReportModelError(
						filepath,
						"glTF image source is not supported");
				}
				if(hasEmbeddedBytes)
				{
					const uint64_t remainingBytes = outDecodedBytes <= maxDecodedBytes
						? maxDecodedBytes - outDecodedBytes
						: 0u;
					bool limitExceeded = false;
					if(!LoadImage(reinterpret_cast<const uint8_t*>(encodedBytes.data()), encodedBytes.size(), textureAsset, remainingBytes, &limitExceeded))
					{
						return ReportModelError(
							filepath,
							limitExceeded
								? "embedded image decoded bytes exceed maxDecodedBytes"
								: "failed to decode embedded glTF image");
					}
					outDecodedBytes += static_cast<uint64_t>(textureAsset.GetPixels().size());
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

		[[nodiscard]] bool HasStrictFiniteTimes(const std::vector<float>& times)
		{
			if(times.empty() || !std::isfinite(times.front())) return false;
			for(size_t index = 1; index < times.size(); ++index)
				if(!std::isfinite(times[index]) || times[index] <= times[index - 1]) return false;
			return true;
		}

		[[nodiscard]] bool ValidateGltfHierarchy(
			const std::string& filepath,
			const fastgltf::Asset& gltf,
			const ModelLoadOptions& options)
		{
			ModelLoadBudget budget(filepath, options);
			if(!budget.CheckNodes(gltf.nodes.size())) return false;
			if(options.maxNodeDepth == 0u)
				return ReportModelError(filepath, "maxNodeDepth must be greater than zero");
			if(gltf.nodes.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()))
				return ReportModelError(filepath, "glTF node count exceeds the supported index range");
			if(gltf.scenes.empty()
				|| (gltf.defaultScene.has_value() && gltf.defaultScene.value() >= gltf.scenes.size()))
				return ReportModelError(filepath, "glTF scene reference is invalid");

			const size_t noParent = std::numeric_limits<size_t>::max();
			std::vector<size_t> parents(gltf.nodes.size(), noParent);
			for(size_t nodeIndex = 0; nodeIndex < gltf.nodes.size(); ++nodeIndex)
			{
				for(const size_t childIndex : gltf.nodes[nodeIndex].children)
				{
					if(childIndex >= gltf.nodes.size() || parents[childIndex] != noParent)
						return ReportModelError(filepath, "glTF contains an invalid node hierarchy");
					parents[childIndex] = nodeIndex;
				}
			}

			// A single-parent forest can be checked from its roots without recursion. Any
			// unvisited nodes belong to a cycle, including cycles outside the selected scene.
			std::vector<std::pair<size_t, uint32_t>> pending;
			pending.reserve(gltf.nodes.size());
			for(size_t nodeIndex = 0; nodeIndex < parents.size(); ++nodeIndex)
				if(parents[nodeIndex] == noParent) pending.emplace_back(nodeIndex, 1u);
			for(size_t cursor = 0; cursor < pending.size(); ++cursor)
			{
				const auto [nodeIndex, depth] = pending[cursor];
				const auto& children = gltf.nodes[nodeIndex].children;
				if(!children.empty() && depth >= options.maxNodeDepth)
					return ReportModelError(filepath, "glTF node hierarchy exceeds maxNodeDepth");
				for(const size_t childIndex : children) pending.emplace_back(childIndex, depth + 1u);
			}
			if(pending.size() != gltf.nodes.size())
				return ReportModelError(filepath, "glTF node hierarchy contains a cycle");

			std::vector<size_t> rootScene(gltf.nodes.size(), noParent);
			for(size_t sceneIndex = 0; sceneIndex < gltf.scenes.size(); ++sceneIndex)
			{
				for(const size_t rootIndex : gltf.scenes[sceneIndex].nodeIndices)
				{
					if(rootIndex >= gltf.nodes.size() || parents[rootIndex] != noParent
						|| rootScene[rootIndex] == sceneIndex)
						return ReportModelError(filepath, "glTF scene contains an invalid or duplicate root");
					rootScene[rootIndex] = sceneIndex;
				}
			}
			return true;
		}

		[[nodiscard]] bool ValidateGltfReferences(const std::string& filepath, const fastgltf::Asset& gltf)
		{
			// fastgltf::validate() dereferences animation and sparse references before
			// checking their bounds. Keep these checks ahead of the SDK validator.
			for(const auto& accessor : gltf.accessors)
			{
				if((accessor.bufferViewIndex.has_value() && *accessor.bufferViewIndex >= gltf.bufferViews.size())
					|| (accessor.sparse && (accessor.sparse->indicesBufferView >= gltf.bufferViews.size()
						|| accessor.sparse->valuesBufferView >= gltf.bufferViews.size())))
					return ReportModelError(filepath, "glTF accessor buffer view reference is invalid");
			}
			for(const auto& animation : gltf.animations)
			{
				for(const auto& sampler : animation.samplers)
					if(sampler.inputAccessor >= gltf.accessors.size() || sampler.outputAccessor >= gltf.accessors.size())
						return ReportModelError(filepath, "glTF animation accessor reference is invalid");
				for(const auto& channel : animation.channels)
				{
					if(channel.samplerIndex >= animation.samplers.size()
						|| !channel.nodeIndex.has_value() || *channel.nodeIndex >= gltf.nodes.size())
						return ReportModelError(filepath, "glTF animation channel reference is invalid");
					const auto& sampler = animation.samplers[channel.samplerIndex];
					const auto& input = gltf.accessors[sampler.inputAccessor];
					const auto& output = gltf.accessors[sampler.outputAccessor];
					const auto outputType = channel.path == fastgltf::AnimationPath::Weights
						? fastgltf::AccessorType::Scalar : channel.path == fastgltf::AnimationPath::Rotation
							? fastgltf::AccessorType::Vec4 : fastgltf::AccessorType::Vec3;
					if(input.type != fastgltf::AccessorType::Scalar || output.type != outputType)
						return ReportModelError(filepath, "glTF animation accessor type is invalid");
					if(sampler.interpolation == fastgltf::AnimationInterpolation::CubicSpline
						&& input.count > std::numeric_limits<size_t>::max() / 3u)
						return ReportModelError(filepath, "glTF cubic animation accessor count overflows");
				}
			}
			for(const auto& mesh : gltf.meshes)
			{
				for(const auto& primitive : mesh.primitives)
				{
					for(const auto& attribute : primitive.attributes)
						if(attribute.accessorIndex >= gltf.accessors.size())
							return ReportModelError(filepath, "glTF attribute accessor reference is invalid");
					if(primitive.indicesAccessor.has_value())
					{
						if(*primitive.indicesAccessor >= gltf.accessors.size())
							return ReportModelError(filepath, "glTF index accessor reference is invalid");
						const auto& indices = gltf.accessors[*primitive.indicesAccessor];
						if(indices.type != fastgltf::AccessorType::Scalar
							|| (indices.componentType != fastgltf::ComponentType::UnsignedByte
								&& indices.componentType != fastgltf::ComponentType::UnsignedShort
								&& indices.componentType != fastgltf::ComponentType::UnsignedInt))
							return ReportModelError(filepath, "glTF index accessor type is invalid");
					}
					for(const auto& target : primitive.targets)
					{
						for(const auto& attribute : target)
						{
							if(attribute.accessorIndex >= gltf.accessors.size())
								return ReportModelError(filepath, "glTF morph accessor reference is invalid");
							if((attribute.name == "POSITION" || attribute.name == "NORMAL" || attribute.name == "TANGENT")
								&& gltf.accessors[attribute.accessorIndex].type != fastgltf::AccessorType::Vec3)
								return ReportModelError(filepath, "glTF morph accessor type is invalid");
						}
					}
				}
			}
			for(const auto& skin : gltf.skins)
			{
				for(const size_t joint : skin.joints)
					if(joint >= gltf.nodes.size())
						return ReportModelError(filepath, "glTF skin joint reference is invalid");
				if(skin.inverseBindMatrices.has_value())
				{
					if(*skin.inverseBindMatrices >= gltf.accessors.size())
						return ReportModelError(filepath, "glTF inverse-bind accessor reference is invalid");
					const auto& accessor = gltf.accessors[*skin.inverseBindMatrices];
					if(accessor.type != fastgltf::AccessorType::Mat4 || accessor.componentType != fastgltf::ComponentType::Float)
						return ReportModelError(filepath, "glTF inverse-bind accessor type is invalid");
				}
			}
			for(const auto& image : gltf.images)
				if(const auto* source = std::get_if<fastgltf::sources::BufferView>(&image.data))
					if(source->bufferViewIndex >= gltf.bufferViews.size())
						return ReportModelError(filepath, "glTF image buffer view reference is invalid");
			return true;
		}

		[[nodiscard]] bool IsGltfByteRangeValid(
			size_t byteLength, size_t byteOffset, size_t count, size_t stride, size_t elementBytes)
		{
			if(count == 0u || stride == 0u || byteOffset > byteLength) return false;
			const size_t remaining = byteLength - byteOffset;
			return elementBytes <= remaining && count - 1u <= (remaining - elementBytes) / stride;
		}

		[[nodiscard]] bool ValidateGltfBytes(const std::string& filepath, const fastgltf::Asset& gltf)
		{
			std::vector<fastgltf::span<const std::byte>> bufferBytes;
			bufferBytes.reserve(gltf.buffers.size());
			for(const auto& buffer : gltf.buffers)
			{
				fastgltf::span<const std::byte> bytes;
				bool supported = true;
				std::visit(dy_gltf_visitor{
					[&](const fastgltf::sources::Array& source) {
						bytes = fastgltf::span<const std::byte>(source.bytes.data(), source.bytes.size_bytes());
					},
					[&](const fastgltf::sources::Vector& source) {
						bytes = fastgltf::span<const std::byte>(source.bytes.data(), source.bytes.size());
					},
					[&](const fastgltf::sources::ByteView& source) { bytes = source.bytes; },
					[&](const auto&) { supported = false; }
				}, buffer.data);
				if(!supported) return ReportModelError(filepath, "glTF buffer source is not loaded or supported");
				if(buffer.byteLength > bytes.size())
					return ReportModelError(filepath, "glTF buffer byteLength exceeds the loaded bytes");
				bufferBytes.push_back(bytes);
			}
			for(const auto& view : gltf.bufferViews)
			{
				if(view.bufferIndex >= gltf.buffers.size())
					return ReportModelError(filepath, "glTF buffer view buffer reference is invalid");
				const size_t logicalBytes = gltf.buffers[view.bufferIndex].byteLength;
				const size_t loadedBytes = bufferBytes[view.bufferIndex].size();
				if(view.byteOffset > logicalBytes || view.byteLength > logicalBytes - view.byteOffset
					|| view.byteOffset > loadedBytes || view.byteLength > loadedBytes - view.byteOffset)
					return ReportModelError(filepath, "glTF buffer view byte range is out of bounds");
				if(view.byteStride.has_value()
					&& (*view.byteStride < 4u || *view.byteStride > 252u || *view.byteStride % 4u != 0u))
					return ReportModelError(filepath, "glTF buffer view byteStride is invalid");
			}
			for(const auto& accessor : gltf.accessors)
			{
				if(accessor.type == fastgltf::AccessorType::Invalid
					|| accessor.componentType == fastgltf::ComponentType::Invalid || accessor.count == 0u)
					return ReportModelError(filepath, "glTF accessor type or count is invalid");
				const size_t componentBytes = fastgltf::getComponentByteSize(accessor.componentType);
				size_t elementStride = fastgltf::getNumComponents(accessor.type) * componentBytes;
				size_t elementBytes = elementStride;
				if(fastgltf::isMatrix(accessor.type))
				{
					const size_t rows = fastgltf::getElementRowCount(accessor.type);
					const size_t columnBytes = rows * componentBytes;
					const size_t columnStride = (columnBytes + 3u) & ~size_t(3u);
					elementStride = rows * columnStride;
					// The final column's trailing padding may be omitted from the view.
					elementBytes = (rows - 1u) * columnStride + columnBytes;
				}
				auto validAlignment = [&](const fastgltf::BufferView& view, size_t offset, size_t componentSize) {
					return offset % componentSize == 0u && view.byteOffset % componentSize == 0u
						&& (!fastgltf::isMatrix(accessor.type)
							|| (view.byteOffset % 4u + offset % 4u) % 4u == 0u);
				};
				if(accessor.bufferViewIndex.has_value())
				{
					const auto& view = gltf.bufferViews[*accessor.bufferViewIndex];
					const size_t stride = view.byteStride.value_or(elementStride);
					if(stride < elementStride || stride % componentBytes != 0u
						|| !validAlignment(view, accessor.byteOffset, componentBytes)
						|| !IsGltfByteRangeValid(view.byteLength, accessor.byteOffset, accessor.count, stride, elementBytes))
						return ReportModelError(filepath, "glTF accessor byte range or stride is invalid");
				}
				else if(accessor.byteOffset != 0u)
					return ReportModelError(filepath, "glTF accessor without a buffer view has a byte offset");
				if(!accessor.sparse) continue;
				const auto& sparse = *accessor.sparse;
				if(sparse.count == 0u || sparse.count > accessor.count
					|| (sparse.indexComponentType != fastgltf::ComponentType::UnsignedByte
						&& sparse.indexComponentType != fastgltf::ComponentType::UnsignedShort
						&& sparse.indexComponentType != fastgltf::ComponentType::UnsignedInt))
					return ReportModelError(filepath, "glTF sparse accessor count or index type is invalid");
				const auto& indicesView = gltf.bufferViews[sparse.indicesBufferView];
				const auto& valuesView = gltf.bufferViews[sparse.valuesBufferView];
				const size_t indexBytes = fastgltf::getComponentByteSize(sparse.indexComponentType);
				if(indicesView.byteStride || indicesView.target || valuesView.byteStride || valuesView.target
					|| sparse.indicesByteOffset % indexBytes != 0u || indicesView.byteOffset % indexBytes != 0u
					|| !validAlignment(valuesView, sparse.valuesByteOffset, componentBytes)
					|| !IsGltfByteRangeValid(indicesView.byteLength, sparse.indicesByteOffset, sparse.count, indexBytes, indexBytes)
					|| !IsGltfByteRangeValid(valuesView.byteLength, sparse.valuesByteOffset, sparse.count, elementStride, elementBytes))
					return ReportModelError(filepath, "glTF sparse accessor byte range is invalid");

				// Read only after proving both additions and the complete index range fit.
				const auto indices = bufferBytes[indicesView.bufferIndex]
					.subspan(indicesView.byteOffset, indicesView.byteLength).subspan(sparse.indicesByteOffset);
				uint32_t previousIndex = 0u;
				for(size_t index = 0; index < sparse.count; ++index)
				{
					uint32_t value = 0u;
					for(size_t component = 0; component < indexBytes; ++component)
						value |= std::to_integer<uint32_t>(indices[index * indexBytes + component]) << (8u * component);
					if(value >= accessor.count || (index != 0u && value <= previousIndex))
						return ReportModelError(filepath, "glTF sparse indices must be in range and strictly increasing");
					previousIndex = value;
				}
			}
			return true;
		}

		[[nodiscard]] bool ValidateGltfLoadLimits(
			const std::string& filepath,
			const fastgltf::Asset& gltf,
			const ModelLoadOptions& options, uint64_t& decodedBytes)
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
					uint64_t keyCount = morphWeights
						? gltf.accessors[sampler.outputAccessor].count
						: gltf.accessors[sampler.inputAccessor].count;
					if(morphWeights)
					{
						if(!channel.nodeIndex.has_value() || channel.nodeIndex.value() >= gltf.nodes.size()) return false;
						const auto& node = gltf.nodes[channel.nodeIndex.value()];
						if(!node.meshIndex.has_value() || node.meshIndex.value() >= gltf.meshes.size()) return false;
						const auto& primitives = gltf.meshes[node.meshIndex.value()].primitives;
						if(primitives.empty()) return false;
						const uint64_t targets = primitives.front().targets.size();
						const uint64_t parts = sampler.interpolation == fastgltf::AnimationInterpolation::CubicSpline ? 3u : 1u;
						const uint64_t times = gltf.accessors[sampler.inputAccessor].count;
						if(targets == 0 || targets > std::numeric_limits<uint64_t>::max() / parts
							|| times > std::numeric_limits<uint64_t>::max() / (targets * parts)
							|| keyCount != times * targets * parts)
							return ReportModelError(filepath, "glTF morph animation input/output counts do not match");
						keyCount /= parts;
					}
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
			decodedBytes=budget.DecodedBytes();
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
					filepath,
					"failed to query glTF source file size");
			}
			if(mainSourceSize > options.maxSourceBytes)
			{
				return ReportModelError(
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
				if(!IsPathInsideDirectory(basePath, externalPath, sizeError))
				{
					return ReportModelError(
						filepath,
						sizeError
							? "failed to canonicalize external glTF buffer path"
							: "external glTF buffer path escapes the model directory");
				}
				const uintmax_t externalSize = std::filesystem::file_size(externalPath, sizeError);
				if(sizeError)
				{
					return ReportModelError(
						filepath,
						"failed to query external glTF buffer size");
				}
				if(externalSize > std::numeric_limits<uint64_t>::max()
					|| static_cast<uint64_t>(externalSize) > options.maxSourceBytes - totalSourceBytes)
				{
					return ReportModelError(
						filepath,
						"glTF source bytes exceed maxSourceBytes while accounting for external buffers");
				}
				totalSourceBytes += static_cast<uint64_t>(externalSize);
			}
			for(size_t imageIndex = 0; imageIndex < gltf.images.size(); ++imageIndex)
			{
				const auto* uriSource =
					std::get_if<fastgltf::sources::URI>(&gltf.images[imageIndex].data);
				if(uriSource == nullptr || !uriSource->uri.isLocalPath()) continue;

				const std::filesystem::path externalPath = basePath / uriSource->uri.fspath();
				if(!IsPathInsideDirectory(basePath, externalPath, sizeError))
				{
					return ReportModelError(
						filepath,
						sizeError
							? "failed to canonicalize external glTF image path"
							: "external glTF image path escapes the model directory");
				}
				const uintmax_t externalSize = std::filesystem::file_size(externalPath, sizeError);
				if(sizeError)
				{
					return ReportModelError(
						filepath,
						"failed to query external glTF image size");
				}
				if(externalSize > std::numeric_limits<uint64_t>::max()
					|| static_cast<uint64_t>(externalSize) > options.maxSourceBytes - totalSourceBytes)
				{
					return ReportModelError(
						filepath,
						"glTF source bytes exceed maxSourceBytes while accounting for external images");
				}
				totalSourceBytes += static_cast<uint64_t>(externalSize);
			}
			if(totalSourceBytes > options.maxParserInputBytes)
			{
				return ReportModelError(
					filepath,
					"glTF parser input bytes exceed maxParserInputBytes");
			}
			return true;
		}

	}

		[[nodiscard]] bool LoadGltfModel(const std::string& filepath, ModelData& outModel, const ModelLoadOptions& options, uint64_t* decodedBytes)
		{
			if(decodedBytes) *decodedBytes=0;
			bool warnedInfluenceTruncation = false;
			std::error_code sourceSizeError;
			// Resolve once so file reads, sibling assets and containment use the same directory.
			const auto sourcePath = std::filesystem::absolute(filepath, sourceSizeError);
			if(sourceSizeError) return ReportModelError(filepath, "failed to resolve glTF source path");
			const uintmax_t sourceSize = std::filesystem::file_size(sourcePath, sourceSizeError);
			if(sourceSizeError || sourceSize > std::numeric_limits<uint64_t>::max())
			{
				return ReportModelError(
					filepath,
					"failed to query glTF source size");
			}
			if(static_cast<uint64_t>(sourceSize) > options.maxSourceBytes
				|| static_cast<uint64_t>(sourceSize) > options.maxParserInputBytes)
			{
				return ReportModelError(
					filepath,
					static_cast<uint64_t>(sourceSize) > options.maxSourceBytes
						? "glTF source bytes exceed maxSourceBytes"
						: "glTF parser input bytes exceed maxParserInputBytes");
			}
			auto data = fastgltf::GltfDataBuffer::FromPath(sourcePath);
			if(data.error() != fastgltf::Error::None)
			{
				return ReportModelError(
					filepath,
					"failed to read glTF file");
			}

			const std::filesystem::path basePath = sourcePath.parent_path();
			{
				fastgltf::Parser metadataParser;
				auto metadataAsset = metadataParser.loadGltf(
					data.get(),
					basePath,
					fastgltf::Options::DecomposeNodeMatrices);
				if(metadataAsset.error() != fastgltf::Error::None)
				{
					return ReportModelError(
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
					filepath,
					std::string("failed to parse glTF: ")
						+ std::string(fastgltf::getErrorMessage(asset.error())));
			}
			if(!ValidateGltfHierarchy(filepath, asset.get(), options)
				|| !ValidateGltfReferences(filepath, asset.get())
				|| !ValidateGltfBytes(filepath, asset.get())) return false;
			const fastgltf::Error validationError = fastgltf::validate(asset.get());
			if(validationError != fastgltf::Error::None)
			{
				return ReportModelError(
					filepath,
					std::string("glTF validation failed: ")
						+ std::string(fastgltf::getErrorMessage(validationError)));
			}

			outModel = {};
			fastgltf::Asset& gltf = asset.get();
			if(gltf.images.size() > options.maxTextures)
			{
				return ReportModelError(
					filepath,
					"glTF texture count exceeds maxTextures");
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
			uint64_t geometryBytes=0;
			if(!ValidateGltfLoadLimits(filepath, gltf, geometryOptions, geometryBytes)) return false;
			outModel.materials.reserve(gltf.materials.size());
			for(size_t materialIndex = 0; materialIndex < gltf.materials.size(); ++materialIndex)
			{
				const fastgltf::Material& source = gltf.materials[materialIndex];
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
				if(source.pbrData.baseColorTexture.has_value()) ApplyGltfMaterialTexture(
					basePath, gltf, source.pbrData.baseColorTexture.value(), material,
					MaterialTextureKind::BaseColor, filepath);
				if(source.pbrData.metallicRoughnessTexture.has_value()) ApplyGltfMaterialTexture(
					basePath, gltf, source.pbrData.metallicRoughnessTexture.value(), material,
					MaterialTextureKind::MetallicRoughness, filepath);
				if(source.normalTexture.has_value())
				{
					ApplyGltfMaterialTexture(
						basePath, gltf, source.normalTexture.value(), material,
						MaterialTextureKind::Normal, filepath);
					material.material.normalScale = static_cast<float>(source.normalTexture->scale);
				}
				if(source.occlusionTexture.has_value())
				{
					ApplyGltfMaterialTexture(
						basePath, gltf, source.occlusionTexture.value(), material,
						MaterialTextureKind::Occlusion, filepath);
					material.material.occlusionStrength = static_cast<float>(source.occlusionTexture->strength);
				}
				if(source.emissiveTexture.has_value()) ApplyGltfMaterialTexture(
					basePath, gltf, source.emissiveTexture.value(), material,
					MaterialTextureKind::Emissive, filepath);
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
						filepath,
						"glTF node matrix could not be decomposed");
				}
				ModelNode& node = outModel.nodes[nodeIndex];
				node.name = source.name.c_str();
				node.bindTransform = ToNodeTransform(*trs);
				if(!IsFinite(node.bindTransform))
				{
					return ReportModelError(
						filepath,
						"glTF node contains non-finite transform values");
				}
				if(source.meshIndex.has_value())
				{
					const fastgltf::Mesh& mesh = gltf.meshes[source.meshIndex.value()];
					size_t targetCount = 0u;
					for(const fastgltf::Primitive& primitive : mesh.primitives)
					{
						if(targetCount == 0u) targetCount = primitive.targets.size();
						else if(!primitive.targets.empty() && primitive.targets.size() != targetCount)
						{
							return ReportModelError(
								filepath,
								"glTF mesh primitives use inconsistent morph target counts");
						}
					}
					const auto& sourceWeights = !source.weights.empty() ? source.weights : mesh.weights;
					if(!sourceWeights.empty() && sourceWeights.size() != targetCount)
					{
						return ReportModelError(
							filepath,
							"glTF morph default weight count does not match target count");
					}
					node.morphWeights.assign(targetCount, 0.0f);
					for(size_t weightIndex = 0; weightIndex < sourceWeights.size(); ++weightIndex)
					{
						const float weight = static_cast<float>(sourceWeights[weightIndex]);
						if(!std::isfinite(weight))
						{
							return ReportModelError(
								filepath,
								"glTF morph default weight is non-finite");
						}
						node.morphWeights[weightIndex] = weight;
					}
				}
				for(const size_t childIndex : source.children)
				{
					if(childIndex >= outModel.nodes.size() || outModel.nodes[childIndex].parentIndex != -1)
					{
						return ReportModelError(
							filepath,
							"glTF contains an invalid node hierarchy");
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
							filepath,
							"glTF skin references an invalid joint node");
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
							filepath,
							"glTF inverse-bind count does not match joint count");
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
							filepath,
							"glTF skin contains non-finite inverse-bind matrices");
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
					if(!channel.nodeIndex.has_value() || channel.nodeIndex.value() >= outModel.nodes.size()
						|| channel.samplerIndex >= source.samplers.size())
					{
						return ReportModelError(
							filepath,
							channel.path == fastgltf::AnimationPath::Weights
								? "glTF morph animation channel is invalid" : "glTF animation channel is invalid");
					}
					if(channel.path == fastgltf::AnimationPath::Weights)
					{
						const uint32_t nodeIndex = static_cast<uint32_t>(channel.nodeIndex.value());
						const size_t targetCount = outModel.nodes[nodeIndex].morphWeights.size();
						if(targetCount == 0u)
						{
							return ReportModelError(
								filepath,
								"glTF morph animation targets a node without morph targets");
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
								filepath,
								"glTF morph animation key times must be finite and strictly increasing");
						}
						const bool cubic = sampler.interpolation == fastgltf::AnimationInterpolation::CubicSpline;
						const uint64_t valuesPerKey = static_cast<uint64_t>(targetCount) * (cubic ? 3u : 1u);
						if(times.size() > std::numeric_limits<uint64_t>::max() / valuesPerKey
							|| outputAccessor.count != static_cast<uint64_t>(times.size()) * valuesPerKey)
						{
							return ReportModelError(
								filepath,
								"glTF morph animation input/output counts do not match");
						}
						std::vector<float> values;
						values.reserve(outputAccessor.count);
						fastgltf::iterateAccessor<float>(gltf, outputAccessor, [&](float value) { values.push_back(value); });
						if(std::any_of(values.begin(), values.end(), [](float value) { return !std::isfinite(value); }))
						{
							return ReportModelError(
								filepath,
								"glTF morph animation contains non-finite weights");
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
									key.outTangent = values[keyBase + targetCount * 2u + targetIndex];
								}
								key.value = values[keyBase + (cubic ? targetCount : 0u) + targetIndex];
								track.weights.push_back(key);
							}
							clip.morphTracks.push_back(std::move(track));
						}
						clip.duration = std::max(clip.duration, times.back());
						continue;
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
							filepath,
							"glTF animation key times must be finite and strictly increasing");
					}
					const bool cubic = sampler.interpolation == fastgltf::AnimationInterpolation::CubicSpline;
					const size_t expectedOutputCount = times.size() * (cubic ? 3u : 1u);
					if(outputAccessor.count != expectedOutputCount)
					{
						return ReportModelError(
							filepath,
							"glTF animation input/output counts do not match");
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
						if(std::any_of(track.rotations.begin(), track.rotations.end(), [](const QuatKey& key) {
								return !IsFinite(key.value) || !IsFinite(key.inTangent) || !IsFinite(key.outTangent);
							}))
						{
							return ReportModelError(
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
						if(std::any_of(destination->begin(), destination->end(), [](const Vec3Key& key) {
								return !IsFinite(key.value) || !IsFinite(key.inTangent) || !IsFinite(key.outTangent);
							}))
						{
							return ReportModelError(
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

			struct PendingNode
			{
				size_t index;
				fastgltf::math::fmat4x4 parentMatrix;
			};
			std::vector<PendingNode> pendingNodes;
			auto processNode =
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
								filepath,
								"glTF morph default weight count does not match primitive target count");
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
								filepath,
								"glTF POSITION attribute contains non-finite values");
						}

						const Math::float4x4 modelVertexMatrix = ToFloat4x4(vertexMatrix);
						const auto* normalAttr = primitive.findAttribute("NORMAL");
						const bool hasNormalAttribute = normalAttr != primitive.attributes.end();
						if(hasNormalAttribute)
						{
							if(normalAttr->accessorIndex >= gltf.accessors.size()) return false;
							const fastgltf::Accessor& normalAcc = gltf.accessors[normalAttr->accessorIndex];
							if(normalAcc.count != posAccessor.count) return false;
							Math::float4x4 normalMatrix = {};
							if(!Math::InverseTranspose(modelVertexMatrix, normalMatrix))
							{
								return ReportModelError(
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
									filepath,
									"glTF NORMAL attribute contains non-finite values");
							}
						}

						const auto* tangentAttr = primitive.findAttribute("TANGENT");
						if(tangentAttr != primitive.attributes.end())
						{
							if(tangentAttr->accessorIndex >= gltf.accessors.size()) return false;
							const fastgltf::Accessor& tangentAccessor = gltf.accessors[tangentAttr->accessorIndex];
							if(tangentAccessor.count != posAccessor.count) return false;
							const float tangentHandednessSign =
								(Math::Determinant3x3(modelVertexMatrix) < 0.0f ? -1.0f : 1.0f)
								* (options.flipV ? -1.0f : 1.0f);
							bool invalidTangent = false;
							fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(gltf, tangentAccessor, [&](const fastgltf::math::fvec4& value, size_t idx) {
								if(!std::isfinite(value.x()) || !std::isfinite(value.y())
									|| !std::isfinite(value.z()) || !std::isfinite(value.w()))
								{
									invalidTangent = true;
									return;
								}
								const Math::float3 transformed = Math::TransformVector(
									modelVertexMatrix,
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
									filepath,
									"glTF TANGENT attribute contains non-finite values");
							}
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
									filepath,
									"glTF TEXCOORD_0 attribute contains non-finite values");
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
									filepath,
									"glTF JOINTS/WEIGHTS attribute pair is incomplete");
							}
							if(!hasJoints) continue;
							hasInfluenceAttributes = true;
							if(mesh.skinIndex == UINT32_MAX)
							{
								return ReportModelError(
									filepath,
									"glTF mesh has influences but no skin");
							}

							if(jointAttribute->accessorIndex >= gltf.accessors.size()
								|| weightAttribute->accessorIndex >= gltf.accessors.size()) return false;
							const fastgltf::Accessor& jointAccessor = gltf.accessors[jointAttribute->accessorIndex];
							const fastgltf::Accessor& weightAccessor = gltf.accessors[weightAttribute->accessorIndex];
							if(jointAccessor.count != posAccessor.count || weightAccessor.count != posAccessor.count)
							{
								return ReportModelError(
									filepath,
									"glTF influence count does not match vertex count");
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
									filepath,
									"glTF vertex weights must be finite and non-negative");
							}
							if(invalidJoint)
							{
								return ReportModelError(
									filepath,
									"glTF vertex references a joint outside the skin");
							}
						}

						if(mesh.skinIndex != UINT32_MAX && !hasInfluenceAttributes)
						{
							return ReportModelError(
								filepath,
								"glTF skinned mesh has no JOINTS/WEIGHTS attributes");
						}
						if(hasInfluenceAttributes)
						{
							mesh.skinInfluences.reserve(posAccessor.count);
							bool anyTruncated = false;
							for(auto& values : vertexInfluences)
							{
								bool truncated = false;
								mesh.skinInfluences.push_back(MakeSkinInfluence(std::move(values), truncated));
								anyTruncated = anyTruncated || truncated;
							}
							if(anyTruncated && !warnedInfluenceTruncation)
							{
								ReportModelWarning(
									filepath,
									"glTF vertex influences were truncated to the four strongest joints");
								warnedInfluenceTruncation = true;
							}
						}

						mesh.morphTargets.reserve(primitive.targets.size());
						Math::float4x4 morphNormalMatrix = {};
						if(!primitive.targets.empty()
							&& !Math::InverseTranspose(modelVertexMatrix, morphNormalMatrix))
						{
							return ReportModelError(
								filepath,
								"glTF mesh transform is singular; morph deltas cannot be transformed");
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
									filepath,
									"glTF morph target contains non-finite deltas");
							};
							if(!loadDeltas("POSITION", modelVertexMatrix, target.positionDeltas)
								|| !loadDeltas("NORMAL", morphNormalMatrix, target.normalDeltas)
								|| !loadDeltas("TANGENT", modelVertexMatrix, target.tangentDeltas)) return false;
							if(target.positionDeltas.empty() && target.normalDeltas.empty() && target.tangentDeltas.empty())
							{
								return ReportModelError(
									filepath,
									"glTF morph target contains no supported deltas");
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
								filepath,
								"glTF TRIANGLES index count must be a non-zero multiple of three");
						}
						if(std::any_of(mesh.mesh.indices.begin(), mesh.mesh.indices.end(), [&](uint32_t index) {
							return index >= mesh.mesh.vertices.size();
						}))
						{
							return ReportModelError(
								filepath,
								"glTF index references a vertex outside the primitive");
						}

						if(Math::Determinant3x3(modelVertexMatrix) < 0.0f)
							for(size_t index = 0; index < mesh.mesh.indices.size(); index += 3)
								std::swap(mesh.mesh.indices[index + 1], mesh.mesh.indices[index + 2]);
						if(tangentAttr == primitive.attributes.end()
							&& !CalculateTangents(mesh.mesh, !hasNormalAttribute))
							return ReportModelError(filepath, "glTF generated normal or tangent is non-finite");
						outModel.meshes.push_back(std::move(mesh));
					}
				}

				for(auto child = node.children.rbegin(); child != node.children.rend(); ++child)
					pendingNodes.push_back({*child, globalMatrix});
				return true;
			};

			if(gltf.scenes.empty()) return false;
			if(gltf.defaultScene.has_value() && gltf.defaultScene.value() >= gltf.scenes.size()) return false;
			const fastgltf::Scene* scene = gltf.defaultScene.has_value() ? &gltf.scenes[*gltf.defaultScene] : &gltf.scenes[0];
			// 파일의 노드 변환을 보존한다. 화면에서의 배치 방향은 호출자가 정한다.
			const fastgltf::math::fmat4x4 initialMatrix(1.0f);
			for(auto root = scene->nodeIndices.rbegin(); root != scene->nodeIndices.rend(); ++root)
				pendingNodes.push_back({*root, initialMatrix});
			while(!pendingNodes.empty())
			{
				const PendingNode node = pendingNodes.back();
				pendingNodes.pop_back();
				if(!processNode(node.index, node.parentMatrix)) return false;
			}
			if(decodedBytes) *decodedBytes=geometryBytes+decodedImageBytes;
			return !outModel.meshes.empty();
		}

}
