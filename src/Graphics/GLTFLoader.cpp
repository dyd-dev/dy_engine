#include "GLTFLoader.h"
#include <filesystem>
#include <iostream>

#include <fastgltf/core.hpp>
#include <fastgltf/types.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/math.hpp>

// std::visit를 위한 오버로딩 헬퍼
template<class... Ts> struct visitor : Ts... { using Ts::operator()...; };
template<class... Ts> visitor(Ts...) -> visitor<Ts...>;

namespace dy::Graphics
{
    bool GLTFLoader::Load(const std::string& filepath, std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices, std::string* outTexturePath)
    {
        auto data = fastgltf::GltfDataBuffer::FromPath(filepath);
        if (data.error() != fastgltf::Error::None) {
            std::cerr << "Failed to load glTF file: " << filepath << std::endl;
            return false;
        }

        fastgltf::Parser parser;
        // Parse either .glb or .gltf
        auto asset = parser.loadGltf(data.get(), std::filesystem::path(filepath).parent_path(), fastgltf::Options::LoadExternalBuffers);
        
        if (asset.error() != fastgltf::Error::None) {
            std::cerr << "Failed to parse glTF: " << fastgltf::getErrorMessage(asset.error()) << std::endl;
            return false;
        }

        auto& gltf = asset.get();

        // 람다를 이용한 재귀적 노드 탐색 함수
        std::function<void(size_t, fastgltf::math::mat<float, 4, 4>)> processNode = 
            [&](size_t nodeIndex, fastgltf::math::mat<float, 4, 4> parentMatrix) {
            auto& node = gltf.nodes[nodeIndex];
            
            // 현재 노드의 로컬 행렬 계산 (TRS -> Matrix)
            fastgltf::math::mat<float, 4, 4> localMatrix = fastgltf::getTransformMatrix(node);
            fastgltf::math::mat<float, 4, 4> globalMatrix = parentMatrix * localMatrix;

            if (node.meshIndex.has_value()) {
                auto& mesh = gltf.meshes[node.meshIndex.value()];
                for (auto& primitive : mesh.primitives) {
                    size_t initialVertexCount = outVertices.size();

                    fastgltf::Accessor* posAccessor = nullptr;
                    auto* posAttr = primitive.findAttribute("POSITION");
                    if (posAttr) {
                        posAccessor = &gltf.accessors[posAttr->accessorIndex];
                    }

                    if (!posAccessor) continue;

                    // 정점 버퍼 미리 할당
                    outVertices.resize(initialVertexCount + posAccessor->count);

                    // 1. 위치(Position) 로드 및 행렬 변환 적용
                    fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(gltf, *posAccessor, [&](fastgltf::math::fvec3 pos, size_t idx) {
                        // 행렬 곱셈 수행 (Bake transform)
                        // fastgltf math는 column-major
                        fastgltf::math::fvec4 p(pos.x(), pos.y(), pos.z(), 1.0f);
                        fastgltf::math::fvec4 transformedPos = globalMatrix * p;

                        outVertices[initialVertexCount + idx].position[0] = transformedPos.x();
                        outVertices[initialVertexCount + idx].position[1] = transformedPos.y();
                        outVertices[initialVertexCount + idx].position[2] = transformedPos.z();
                        
                        // 기본 색상 하얗게 초기화
                        outVertices[initialVertexCount + idx].color[0] = 1.0f;
                        outVertices[initialVertexCount + idx].color[1] = 1.0f;
                        outVertices[initialVertexCount + idx].color[2] = 1.0f;
                        outVertices[initialVertexCount + idx].texCoord[0] = 0.0f;
                        outVertices[initialVertexCount + idx].texCoord[1] = 0.0f;
                    });

                    // 2. UV 로드
                    auto* uvAttr = primitive.findAttribute("TEXCOORD_0");
                    if (uvAttr) {
                        auto& uvAcc = gltf.accessors[uvAttr->accessorIndex];
                        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(gltf, uvAcc, [&](fastgltf::math::fvec2 uv, size_t idx) {
                            outVertices[initialVertexCount + idx].texCoord[0] = uv.x();
                            outVertices[initialVertexCount + idx].texCoord[1] = 1.0f - uv.y(); // Flip Y to match stbi_load flip
                        });
                    }

                    // 3. 인덱스 로드
                    if (primitive.indicesAccessor.has_value()) {
                        auto& idxAccessor = gltf.accessors[primitive.indicesAccessor.value()];
                        fastgltf::iterateAccessor<std::uint32_t>(gltf, idxAccessor, [&](std::uint32_t idx) {
                            outIndices.push_back(static_cast<uint32_t>(initialVertexCount + idx));
                        });
                    } else {
                        for (size_t i = 0; i < posAccessor->count; ++i) {
                            outIndices.push_back(static_cast<uint32_t>(initialVertexCount + i));
                        }
                    }

                    // 4. 텍스처 경로 추출 (간단하게 첫 번째 것만)
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

            // 자식 노드들도 동일하게 처리
            for (auto& childIndex : node.children) {
                processNode(childIndex, globalMatrix);
            }
        };

        // 기본 씬(Default Scene)부터 시작
        auto* scene = gltf.defaultScene ? &gltf.scenes[*gltf.defaultScene] : &gltf.scenes[0];
        
        // Y축 180도 회전 행렬 (glTF의 -Z 앞방향을 관찰자 쪽으로 돌림)
        fastgltf::math::mat<float, 4, 4> rotationMatrix(1.0f);
        rotationMatrix.col(0).x() = -1.0f; // cos(180)
        rotationMatrix.col(2).z() = -1.0f; // cos(180)
        // R_y(180) = [[-1, 0, 0, 0], [0, 1, 0, 0], [0, 0, -1, 0], [0, 0, 0, 1]]

        for (auto& nodeIndex : scene->nodeIndices) {
            processNode(nodeIndex, rotationMatrix);
        }

        return true;
    }
}
