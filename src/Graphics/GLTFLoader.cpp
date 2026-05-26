#include "Loaders.h"
#include <cmath>
#include <filesystem>
#include <functional>
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
                    size_t initialVertexCount = outData.vertices.size();

                    fastgltf::Accessor* posAccessor = nullptr;
                    auto* posAttr = primitive.findAttribute("POSITION");
                    if (posAttr) {
                        posAccessor = &gltf.accessors[posAttr->accessorIndex];
                    }

                    if (!posAccessor) continue;

                    // 정점 버퍼 미리 할당
                    outData.vertices.resize(initialVertexCount + posAccessor->count);

                    // 1. 위치(Position) 로드 및 행렬 변환 적용
                    fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(gltf, *posAccessor, [&](fastgltf::math::fvec3 pos, size_t idx) {
                        // 행렬 곱셈 수행 (Bake transform)
                        // fastgltf math는 column-major
                        fastgltf::math::fvec4 p(pos.x(), pos.y(), pos.z(), 1.0f);
                        fastgltf::math::fvec4 transformedPos = globalMatrix * p;

                        outData.vertices[initialVertexCount + idx].position[0] = transformedPos.x();
                        outData.vertices[initialVertexCount + idx].position[1] = transformedPos.y();
                        outData.vertices[initialVertexCount + idx].position[2] = transformedPos.z();
                        
                        // 기본 색상 하얗게 초기화
                        outData.vertices[initialVertexCount + idx].uv[0] = 0.0f;
                        outData.vertices[initialVertexCount + idx].uv[1] = 0.0f;
                        
                        // 법선 기본값 초기화
                        outData.vertices[initialVertexCount + idx].normal[0] = 0.0f;
                        outData.vertices[initialVertexCount + idx].normal[1] = 1.0f;
                        outData.vertices[initialVertexCount + idx].normal[2] = 0.0f;
                    });

                    // 1.1. 법선(Normal) 로드 및 변환 적용
                    auto* normalAttr = primitive.findAttribute("NORMAL");
                    if (normalAttr) {
                        auto& normalAcc = gltf.accessors[normalAttr->accessorIndex];
                        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(gltf, normalAcc, [&](fastgltf::math::fvec3 normal, size_t idx) {
                            // 회전 및 스케일 변환 적용 (w = 0.0f)
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

                            outData.vertices[initialVertexCount + idx].normal[0] = nx;
                            outData.vertices[initialVertexCount + idx].normal[1] = ny;
                            outData.vertices[initialVertexCount + idx].normal[2] = nz;
                        });
                    }

                    // 2. UV 로드
                    auto* uvAttr = primitive.findAttribute("TEXCOORD_0");
                    if (uvAttr) {
                        auto& uvAcc = gltf.accessors[uvAttr->accessorIndex];
                        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(gltf, uvAcc, [&](fastgltf::math::fvec2 uv, size_t idx) {
                            outData.vertices[initialVertexCount + idx].uv[0] = uv.x();
                            outData.vertices[initialVertexCount + idx].uv[1] = 1.0f - uv.y(); // Flip Y to match stbi_load flip
                        });
                    }

                    // 3. 인덱스 로드
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
