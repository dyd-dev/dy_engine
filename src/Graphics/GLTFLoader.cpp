#include "GLTFLoader.h"

#include "Graphics/Loaders.h"

#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>

#include <fastgltf/core.hpp>
#include <fastgltf/math.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

template<class... Ts> struct visitor : Ts... { using Ts::operator()...; };
template<class... Ts> visitor(Ts...) -> visitor<Ts...>;

namespace dy::Graphics
{
	bool LoadFromGLTF(const char* filepath, MeshData& outData)
	{
		return LoadFromGLTF(filepath, outData, nullptr);
	}

	bool LoadFromGLTF(const char* filepath, MeshData& outData, std::string* outTexturePath)
	{
		auto data = fastgltf::GltfDataBuffer::FromPath(filepath);
		if (data.error() != fastgltf::Error::None) {
			std::cerr << "Failed to load glTF file: " << filepath << std::endl;
			return false;
		}

		fastgltf::Parser parser;
		auto asset = parser.loadGltf(data.get(), std::filesystem::path(filepath).parent_path(), fastgltf::Options::LoadExternalBuffers);

		if (asset.error() != fastgltf::Error::None) {
			std::cerr << "Failed to parse glTF: " << fastgltf::getErrorMessage(asset.error()) << std::endl;
			return false;
		}

		auto& gltf = asset.get();

		std::function<void(size_t, fastgltf::math::mat<float, 4, 4>)> processNode =
			[&](size_t nodeIndex, fastgltf::math::mat<float, 4, 4> parentMatrix) {
			auto& node = gltf.nodes[nodeIndex];
			fastgltf::math::mat<float, 4, 4> localMatrix = fastgltf::getTransformMatrix(node);
			fastgltf::math::mat<float, 4, 4> globalMatrix = parentMatrix * localMatrix;

			if (node.meshIndex.has_value()) {
				auto& mesh = gltf.meshes[node.meshIndex.value()];
				for (auto& primitive : mesh.primitives) {
					size_t initialVertexCount = outData.vertices.size();

					fastgltf::Accessor* posAccessor = nullptr;
					auto* posAttr = primitive.findAttribute("POSITION");
					if (posAttr) {
						posAccessor = &gltf.accessors[posAttr->accessorIndex];
					}
					if (!posAccessor) continue;

					outData.vertices.resize(initialVertexCount + posAccessor->count);

					fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(gltf, *posAccessor, [&](fastgltf::math::fvec3 pos, size_t idx) {
						fastgltf::math::fvec4 p(pos.x(), pos.y(), pos.z(), 1.0f);
						fastgltf::math::fvec4 transformedPos = globalMatrix * p;

						Vertex& vertex = outData.vertices[initialVertexCount + idx];
						vertex.position.x = transformedPos.x();
						vertex.position.y = transformedPos.y();
						vertex.position.z = transformedPos.z();
						vertex.uv = Math::float2(0.0f, 0.0f);
						vertex.normal = Math::float3(0.0f, 1.0f, 0.0f);
					});

					auto* normalAttr = primitive.findAttribute("NORMAL");
					if (normalAttr) {
						auto& normalAcc = gltf.accessors[normalAttr->accessorIndex];
						fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(gltf, normalAcc, [&](fastgltf::math::fvec3 normal, size_t idx) {
							fastgltf::math::fvec4 n(normal.x(), normal.y(), normal.z(), 0.0f);
							fastgltf::math::fvec4 transformedNormal = globalMatrix * n;

							float nx = transformedNormal.x();
							float ny = transformedNormal.y();
							float nz = transformedNormal.z();
							float len = std::sqrt(nx * nx + ny * ny + nz * nz);
							if (len > 0.0001f) {
								nx /= len;
								ny /= len;
								nz /= len;
							}

							outData.vertices[initialVertexCount + idx].normal = Math::float3(nx, ny, nz);
						});
					}

					auto* uvAttr = primitive.findAttribute("TEXCOORD_0");
					if (uvAttr) {
						auto& uvAcc = gltf.accessors[uvAttr->accessorIndex];
						fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(gltf, uvAcc, [&](fastgltf::math::fvec2 uv, size_t idx) {
							outData.vertices[initialVertexCount + idx].uv = Math::float2(uv.x(), 1.0f - uv.y());
						});
					}

					if (primitive.indicesAccessor.has_value()) {
						auto& idxAccessor = gltf.accessors[primitive.indicesAccessor.value()];
						fastgltf::iterateAccessor<std::uint32_t>(gltf, idxAccessor, [&](std::uint32_t idx) {
							outData.indices.push_back(static_cast<uint32_t>(initialVertexCount + idx));
						});
					} else {
						for (size_t i = 0; i < posAccessor->count; ++i) {
							outData.indices.push_back(static_cast<uint32_t>(initialVertexCount + i));
						}
					}

					if (primitive.materialIndex.has_value() && outTexturePath && outTexturePath->empty()) {
						auto& material = gltf.materials[primitive.materialIndex.value()];
						if (material.pbrData.baseColorTexture.has_value()) {
							auto texIndex = material.pbrData.baseColorTexture.value().textureIndex;
							auto& texture = gltf.textures[texIndex];
							if (texture.imageIndex.has_value()) {
								auto& image = gltf.images[texture.imageIndex.value()];
								std::visit(visitor{
									[&](const fastgltf::sources::URI& filePath) {
										*outTexturePath = (std::filesystem::path(filepath).parent_path() / filePath.uri.fspath()).string();
									},
									[&](const auto&) {}
								}, image.data);
							}
						}
					}
				}
			}

			for (auto& childIndex : node.children) {
				processNode(childIndex, globalMatrix);
			}
		};

		auto* scene = gltf.defaultScene ? &gltf.scenes[*gltf.defaultScene] : &gltf.scenes[0];

		fastgltf::math::mat<float, 4, 4> rotationMatrix(1.0f);
		rotationMatrix.col(0).x() = -1.0f;
		rotationMatrix.col(2).z() = -1.0f;

		for (auto& nodeIndex : scene->nodeIndices) {
			processNode(nodeIndex, rotationMatrix);
		}

		return true;
	}

	bool GLTFLoader::Load(const std::string& filepath, MeshData& outData, std::string* outTexturePath)
	{
		return LoadFromGLTF(filepath.c_str(), outData, outTexturePath);
	}
}
